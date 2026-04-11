use image::DynamicImage;
use ndarray::{s, Array3, Array4, ArrayView2};
use ort::session::Session;
use rayon::prelude::*;

const REC_H: u32 = 48;
/// PaddleOCR 참조 구현과 맞추기 위해 폭 상한을 충분히 크게 둔다.
/// 긴 문장을 768로 강하게 압축하면 수평 정보가 뭉개져 오인식이 늘어난다.
const MAX_W: u32 = 3200;

/// CTC 디코딩: 모델 출력에서 텍스트를 추출한다.
fn ctc_decode(logits: &ArrayView2<f32>, dict: &[String]) -> (String, f32) {
    let (time_steps, _num_classes) = logits.dim();
    let mut text = String::new();
    let mut score_sum = 0f64;
    let mut score_count = 0u32;
    let mut prev_idx: Option<usize> = None;

    for t in 0..time_steps {
        let row = logits.slice(s![t, ..]);
        let (max_idx, &max_val) = row
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap_or(std::cmp::Ordering::Equal))
            .unwrap();

        if max_idx != 0 && Some(max_idx) != prev_idx {
            if max_idx <= dict.len() {
                text.push_str(&dict[max_idx - 1]);
            }
            score_sum += max_val as f64;
            score_count += 1;
        }
        prev_idx = Some(max_idx);
    }

    let avg_score = if score_count > 0 {
        (score_sum / score_count as f64) as f32
    } else {
        0.0
    };

    (text, avg_score)
}

fn target_width(img: &DynamicImage) -> u32 {
    let ratio = img.width() as f32 / img.height() as f32;
    ((REC_H as f32 * ratio + 0.5) as u32).min(MAX_W)
}

// rec 정규화: pixel / 255.0 / 0.5 - 1.0 = pixel * (2.0/255.0) - 1.0
const REC_SCALE: f32 = 2.0 / 255.0;

/// 이미지를 rec 입력 텐서로 전처리한다.
/// 반환값: shape=(3, REC_H, target_w) Array3
fn preprocess_to_array(img: &DynamicImage) -> Array3<f32> {
    let tw = target_width(img);
    let resized = img.resize_exact(tw, REC_H, image::imageops::FilterType::Triangle);
    let rgb = resized.to_rgb8();

    // as_raw()로 연속 메모리 직접 접근: bounds check 없이 RGBRGB... 순서로 순회
    let raw = rgb.as_raw();
    let tw_usize = tw as usize;
    let mut arr = Array3::<f32>::zeros((3, REC_H as usize, tw_usize));
    for (i, chunk) in raw.chunks_exact(3).enumerate() {
        let y = i / tw_usize;
        let x = i % tw_usize;
        // BGR 순서 (PaddleOCR rec 모델은 BGR 입력으로 학습됨)
        arr[[0, y, x]] = chunk[2] as f32 * REC_SCALE - 1.0; // B
        arr[[1, y, x]] = chunk[1] as f32 * REC_SCALE - 1.0; // G
        arr[[2, y, x]] = chunk[0] as f32 * REC_SCALE - 1.0; // R
    }
    arr
}

fn preprocess(img: &DynamicImage) -> Array4<f32> {
    preprocess_to_array(img).insert_axis(ndarray::Axis(0))
}

// 너비 정렬 후 청크 단위 배치 처리 크기.
// 청크 내 max_w로만 패딩하므로 time step을 대폭 줄인다.
const REC_BATCH_SIZE: usize = 24;
const REC_BATCH_SIZE_MEDIUM_LARGE: usize = 16;
const REC_BATCH_SIZE_LARGE: usize = 12;
const REC_BATCH_SIZE_XL: usize = 8;
const REC_BATCH_SIZE_XXL: usize = 6;
const REC_BATCH_SIZE_XXXL: usize = 4;
const REC_SINGLE_RETRY_SCORE: f32 = 0.7;
const REC_SINGLE_RETRY_SUSPICIOUS_WIDTH: usize = 120;
const REC_SINGLE_RETRY_SHORT_TEXT_LEN: usize = 4;

/// 워밍업에 사용할 너비 목록 (MAX_W=960 기준 자주 등장하는 크기들).
/// 각 너비에 대해 실행해 cuDNN 알고리즘 캐시를 미리 채운다.
pub(crate) const WARMUP_WIDTHS: &[u32] = &[96, 128, 192, 256, 320, 480, 640, 768];

fn rec_batch_size_for_start_width(width: usize) -> usize {
    if width >= 768 {
        REC_BATCH_SIZE_XXXL
    } else if width >= 640 {
        REC_BATCH_SIZE_XXL
    } else if width >= 500 {
        REC_BATCH_SIZE_XL
    } else if width >= 400 {
        REC_BATCH_SIZE_LARGE
    } else if width >= 236 {
        REC_BATCH_SIZE_MEDIUM_LARGE
    } else {
        REC_BATCH_SIZE
    }
}

fn non_whitespace_char_count(text: &str) -> usize {
    text.chars().filter(|c| !c.is_whitespace()).count()
}

fn should_retry_single_result(text: &str, score: f32, width: usize) -> bool {
    score < REC_SINGLE_RETRY_SCORE
        || (width >= REC_SINGLE_RETRY_SUSPICIOUS_WIDTH
            && non_whitespace_char_count(text) <= REC_SINGLE_RETRY_SHORT_TEXT_LEN)
}

fn should_prefer_retry_result(current: &(String, f32), retried: &(String, f32)) -> bool {
    let current_len = non_whitespace_char_count(&current.0);
    let retried_len = non_whitespace_char_count(&retried.0);

    retried.1 > current.1
        || (retried_len >= current_len + 2 && retried.1 + 0.1 >= current.1)
        || (retried_len >= current_len + 4 && retried.1 + 0.25 >= current.1)
        || (current_len == 0 && retried_len > 0)
}

/// 미리 조립된 배치 텐서로 추론한다. 성공하면 결과를 반환하고, 실패하면 None.
fn try_batch_run(
    session: &mut Session,
    batch: Array4<f32>,
    dict: &[String],
    n: usize,
) -> Option<Vec<(String, f32)>> {
    let input_values = ort::value::Value::from_array(batch).ok()?;
    let outputs = session.run(ort::inputs![input_values]).ok()?;
    let (shape, data) = outputs[0].try_extract_tensor::<f32>().ok()?;

    let dims: Vec<usize> = shape.iter().map(|&d| d as usize).collect();
    // shape: [n, time_steps, num_classes]
    if dims.len() != 3 || dims[0] != n {
        return None;
    }
    let (time_steps, num_classes) = (dims[1], dims[2]);

    Some(
        (0..n)
            .map(|i| {
                let start = i * time_steps * num_classes;
                let slice = &data[start..start + time_steps * num_classes];
                let view = ndarray::ArrayView2::from_shape((time_steps, num_classes), slice)
                    .unwrap_or_else(|_| ndarray::ArrayView2::from_shape((0, 1), &[]).unwrap());
                ctc_decode(&view, dict)
            })
            .collect(),
    )
}

/// 미리 전처리된 단일 텐서를 추론한다 (배치 실패 시 폴백용).
fn recognize_from_array(session: &mut Session, arr: &Array3<f32>, dict: &[String]) -> (String, f32) {
    (|| -> Option<(String, f32)> {
        let arr4 = arr.clone().insert_axis(ndarray::Axis(0));
        let iv = ort::value::Value::from_array(arr4).ok()?;
        let out = session.run(ort::inputs![iv]).ok()?;
        let (shape, data) = out[0].try_extract_tensor::<f32>().ok()?;
        let dims: Vec<usize> = shape.iter().map(|&d| d as usize).collect();
        let (ts, nc) = if dims.len() == 3 {
            (dims[1], dims[2])
        } else {
            (dims[0], dims[1])
        };
        let view = ndarray::ArrayView2::from_shape((ts, nc), data).ok()?;
        Some(ctc_decode(&view, dict))
    })()
    .unwrap_or(("".to_string(), 0.0))
}

/// 여러 이미지를 너비 순 정렬 후 REC_BATCH_SIZE 단위 청크로 배치 인식한다.
///
/// 전처리는 rayon으로 병렬 수행해 CPU 대기를 줄인다.
/// 청크 내 max_w로만 패딩하여 time step 폭발을 방지한다.
pub(crate) fn recognize_batch(
    session: &mut Session,
    imgs: &[DynamicImage],
    dict: &[String],
) -> Result<Vec<(String, f32)>, String> {
    if imgs.is_empty() {
        return Ok(vec![]);
    }
    if imgs.len() == 1 {
        return Ok(vec![recognize(session, &imgs[0], dict)?]);
    }

    // 병렬 CPU 전처리: 모든 이미지를 동시에 리사이즈/정규화
    let tensors: Vec<Array3<f32>> = imgs
        .par_iter()
        .map(|img| preprocess_to_array(img))
        .collect();

    // 너비 오름차순으로 인덱스를 정렬 — 청크 내 너비 편차를 최소화한다
    let mut order: Vec<usize> = (0..tensors.len()).collect();
    order.sort_by_key(|&i| tensors[i].dim().2); // dim().2 == target_width

    let mut results = vec![("".to_string(), 0.0f32); imgs.len()];
    let widths: Vec<usize> = order.iter().map(|&i| tensors[i].dim().2).collect();
    if let (Some(min_w), Some(max_w)) = (widths.first(), widths.last()) {
        let p50 = widths[widths.len() / 2];
        let p90 = widths[widths.len().saturating_mul(9) / 10];
        eprintln!(
            "[OCR] rec 너비 분포: n={}, min={}, p50={}, p90={}, max={}",
            widths.len(),
            min_w,
            p50,
            p90,
            max_w
        );
    }

    let mut fallback_chunks = 0usize;

    let mut chunk_no = 0usize;
    let mut cursor = 0usize;
    while cursor < order.len() {
        let start_w = tensors[order[cursor]].dim().2;
        let chunk_size = rec_batch_size_for_start_width(start_w);
        let end = (cursor + chunk_size).min(order.len());
        let chunk_idx = &order[cursor..end];
        let max_w = chunk_idx
            .iter()
            .map(|&i| tensors[i].dim().2)
            .max()
            .unwrap_or(1);
        let chunk_n = chunk_idx.len();

        // 패딩 값 -1.0 = 검은색 픽셀(RGB=0)에 해당하는 정규화 값
        let mut batch = Array4::<f32>::from_elem((chunk_n, 3, REC_H as usize, max_w), -1.0f32);
        for (bi, &orig_i) in chunk_idx.iter().enumerate() {
            let arr = &tensors[orig_i];
            let tw = arr.dim().2;
            batch.slice_mut(s![bi, .., .., ..tw]).assign(arr);
        }

        let t_chunk = std::time::Instant::now();
        let (chunk_results, mode) = if let Some(r) = try_batch_run(session, batch, dict, chunk_n) {
            (r, "batch")
        } else {
            fallback_chunks += 1;
            // 배치 실패 시 청크 내 순차 처리
            (
                chunk_idx
                    .iter()
                    .map(|&orig_i| recognize_from_array(session, &tensors[orig_i], dict))
                    .collect(),
                "fallback",
            )
        };
        eprintln!(
            "[OCR] rec 청크 {}: {}개, start_w={}, max_w={}, mode={}, {:.0}ms",
            chunk_no + 1,
            chunk_n,
            start_w,
            max_w,
            mode,
            t_chunk.elapsed().as_millis()
        );

        for (&orig_i, result) in chunk_idx.iter().zip(chunk_results) {
            results[orig_i] = result;
        }

        cursor = end;
        chunk_no += 1;
    }

    if fallback_chunks > 0 {
        eprintln!("[OCR] rec 배치 폴백 청크 수: {}", fallback_chunks);
    }

    let mut retried = 0usize;
    for &orig_i in &order {
        let current = &results[orig_i];
        let width = tensors[orig_i].dim().2;
        if !should_retry_single_result(&current.0, current.1, width) {
            continue;
        }
        let retried_result = recognize_from_array(session, &tensors[orig_i], dict);
        if should_prefer_retry_result(current, &retried_result) {
            results[orig_i] = retried_result;
        }
        retried += 1;
    }
    if retried > 0 {
        eprintln!("[OCR] rec low-score single retry 수: {}", retried);
    }

    Ok(results)
}

/// 텍스트 인식: 이미지에서 텍스트를 추출한다.
pub(crate) fn recognize(
    session: &mut Session,
    img: &DynamicImage,
    dict: &[String],
) -> Result<(String, f32), String> {
    let input = preprocess(img);
    let input_values = ort::value::Value::from_array(input).map_err(|e| e.to_string())?;
    let outputs = session
        .run(ort::inputs![input_values])
        .map_err(|e| e.to_string())?;

    let (shape, data) = outputs[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| format!("rec 출력 추출 실패: {e}"))?;

    let dims: Vec<usize> = shape.iter().map(|&d| d as usize).collect();
    let (time_steps, num_classes) = if dims.len() == 3 {
        (dims[1], dims[2])
    } else {
        (dims[0], dims[1])
    };

    let logits_2d = ArrayView2::from_shape((time_steps, num_classes), data)
        .map_err(|e| format!("rec 출력 reshape 실패: {e}"))?;

    Ok(ctc_decode(&logits_2d, dict))
}

#[cfg(test)]
pub(crate) fn recognize_batch_vs_single(
    session: &mut Session,
    img: &DynamicImage,
    dict: &[String],
) -> Result<((String, f32), (String, f32)), String> {
    let batch = recognize_batch(session, std::slice::from_ref(img), dict)?
        .into_iter()
        .next()
        .ok_or_else(|| "batch 결과가 비어 있음".to_string())?;
    let single = recognize(session, img, dict)?;
    Ok((batch, single))
}

#[cfg(test)]
pub(crate) fn recognize_multi_batch_vs_single(
    session: &mut Session,
    imgs: &[DynamicImage],
    target_index: usize,
    dict: &[String],
) -> Result<((String, f32), (String, f32)), String> {
    let batch = recognize_batch(session, imgs, dict)?
        .into_iter()
        .nth(target_index)
        .ok_or_else(|| "batch target 결과가 비어 있음".to_string())?;
    let single = recognize(session, &imgs[target_index], dict)?;
    Ok((batch, single))
}

#[cfg(test)]
mod tests {
    use super::*;
    use ndarray::Array2;

    #[test]
    fn BGR_정규화가_채널_위치_순서로_적용된다() {
        // R=200, G=100, B=50인 1x1 이미지
        let img = image::DynamicImage::ImageRgb8(image::RgbImage::from_pixel(
            1,
            1,
            image::Rgb([200, 100, 50]),
        ));
        let tensor = preprocess(&img);
        let b = tensor[[0, 0, 0, 0]]; // 채널 0 = B
        let g = tensor[[0, 1, 0, 0]]; // 채널 1 = G
        let r = tensor[[0, 2, 0, 0]]; // 채널 2 = R

        let expected_b = 50.0 / 255.0 / 0.5 - 1.0;
        let expected_g = 100.0 / 255.0 / 0.5 - 1.0;
        let expected_r = 200.0 / 255.0 / 0.5 - 1.0;

        assert!((b - expected_b).abs() < 1e-4, "B: {b} != {expected_b}");
        assert!((g - expected_g).abs() < 1e-4, "G: {g} != {expected_g}");
        assert!((r - expected_r).abs() < 1e-4, "R: {r} != {expected_r}");
    }

    fn sample_dict() -> Vec<String> {
        vec!["a", "b", "c", "d", "e"]
            .into_iter()
            .map(String::from)
            .collect()
    }

    #[test]
    fn target_width는_MAX_W로_상한된다() {
        let img = image::DynamicImage::ImageRgb8(image::RgbImage::new(4000, 48));
        assert_eq!(target_width(&img), MAX_W);
    }

    #[test]
    fn target_width는_참조구현처럼_round_half_up을_쓴다() {
        let img = image::DynamicImage::ImageRgb8(image::RgbImage::new(101, 48));
        assert_eq!(target_width(&img), 101);
    }

    #[test]
    fn rec_배치_크기는_tail_구간에서만_줄어든다() {
        assert_eq!(rec_batch_size_for_start_width(235), REC_BATCH_SIZE);
        assert_eq!(
            rec_batch_size_for_start_width(236),
            REC_BATCH_SIZE_MEDIUM_LARGE
        );
        assert_eq!(rec_batch_size_for_start_width(400), REC_BATCH_SIZE_LARGE);
        assert_eq!(rec_batch_size_for_start_width(500), REC_BATCH_SIZE_XL);
        assert_eq!(rec_batch_size_for_start_width(640), REC_BATCH_SIZE_XXL);
        assert_eq!(rec_batch_size_for_start_width(768), REC_BATCH_SIZE_XXXL);
    }

    #[test]
    fn low_score_single_retry_기준은_0_7이다() {
        assert!((REC_SINGLE_RETRY_SCORE - 0.7).abs() < f32::EPSILON);
    }

    #[test]
    fn 긴_crop의_두글자_결과는_single_retry_대상이다() {
        assert!(should_retry_single_result("你好", 0.95, 200));
        assert!(should_retry_single_result("ab", 0.88, 200));
        assert!(!should_retry_single_result("你好世界", 0.95, 80));
    }

    #[test]
    fn 긴_crop의_세글자_절단도_single_retry_대상이다() {
        assert!(should_retry_single_result("就好。", 0.775, 140));
        assert!(!should_retry_single_result("就好像下雪了一样。", 0.775, 140));
    }

    #[test]
    fn retry가_점수는_조금_낮아도_의미있게_길면_채택된다() {
        let current = ("你好".to_string(), 0.95);
        let retried = ("你好世界".to_string(), 0.87);
        assert!(should_prefer_retry_result(&current, &retried));
    }

    #[test]
    fn retry가_훨씬_긴_문장을_주면_점수차가_커도_채택된다() {
        let current = ("就好。".to_string(), 0.775);
        let retried = ("就好像下雪了一样。".to_string(), 0.997);
        assert!(should_prefer_retry_result(&current, &retried));
    }

    #[test]
    fn CTC_디코딩_기본_동작() {
        let dict = sample_dict();
        // 6 classes: 0=blank, 1=a, 2=b, 3=c, 4=d, 5=e
        // 시퀀스: blank, a, a, b, blank, c → "abc"
        let mut logits = Array2::<f32>::zeros((6, 6));
        logits[[0, 0]] = 1.0; // blank
        logits[[1, 1]] = 0.9; // a
        logits[[2, 1]] = 0.8; // a (중복 → 무시)
        logits[[3, 2]] = 0.7; // b
        logits[[4, 0]] = 1.0; // blank
        logits[[5, 3]] = 0.6; // c

        let view = logits.view();
        let (text, score) = ctc_decode(&view, &dict);
        assert_eq!(text, "abc");
        let expected_score = (0.9 + 0.7 + 0.6) / 3.0;
        assert!((score - expected_score).abs() < 1e-5);
    }

    #[test]
    fn CTC_디코딩_빈_입력() {
        let dict = sample_dict();
        // 모든 스텝이 blank
        let mut logits = Array2::<f32>::zeros((3, 6));
        logits[[0, 0]] = 1.0;
        logits[[1, 0]] = 1.0;
        logits[[2, 0]] = 1.0;

        let view = logits.view();
        let (text, score) = ctc_decode(&view, &dict);
        assert_eq!(text, "");
        assert_eq!(score, 0.0);
    }

    #[test]
    fn CTC_디코딩_중복_제거_후_blank_사이_같은_문자() {
        let dict = sample_dict();
        // a, blank, a → "aa" (blank으로 구분된 같은 문자는 두 번)
        let mut logits = Array2::<f32>::zeros((3, 6));
        logits[[0, 1]] = 0.9; // a
        logits[[1, 0]] = 1.0; // blank
        logits[[2, 1]] = 0.8; // a

        let view = logits.view();
        let (text, _score) = ctc_decode(&view, &dict);
        assert_eq!(text, "aa");
    }
}
