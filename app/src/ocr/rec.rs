use image::DynamicImage;
use ndarray::{s, Array3, Array4, ArrayView2};
use ort::session::Session;

const REC_H: u32 = 48;
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
            .max_by(|a, b| a.1.partial_cmp(b.1).unwrap())
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

fn preprocess(img: &DynamicImage) -> Array4<f32> {
    let (w, h) = (img.width(), img.height());
    let ratio = w as f32 / h as f32;
    let target_w = ((REC_H as f32 * ratio).ceil() as u32).min(MAX_W);

    let resized = img.resize_exact(target_w, REC_H, image::imageops::FilterType::Lanczos3);
    let rgb = resized.to_rgb8();

    let mut tensor = Array3::<f32>::zeros((3, REC_H as usize, target_w as usize));
    for y in 0..REC_H as usize {
        for x in 0..target_w as usize {
            let pixel = rgb.get_pixel(x as u32, y as u32);
            for c in 0..3 {
                tensor[[c, y, x]] = pixel[c] as f32 / 255.0 / 0.5 - 1.0;
            }
        }
    }

    tensor.insert_axis(ndarray::Axis(0))
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

    let flat: Vec<f32> = data.to_vec();
    let logits_2d = ArrayView2::from_shape((time_steps, num_classes), &flat)
        .map_err(|e| format!("rec 출력 reshape 실패: {e}"))?;

    Ok(ctc_decode(&logits_2d, dict))
}

#[cfg(test)]
mod tests {
    use super::*;
    use ndarray::Array2;

    fn sample_dict() -> Vec<String> {
        vec!["a", "b", "c", "d", "e"]
            .into_iter()
            .map(String::from)
            .collect()
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
