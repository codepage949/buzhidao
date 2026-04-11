use image::DynamicImage;
use ndarray::{Array3, Array4};
use ort::session::Session;
use rayon::prelude::*;

/// 분류 결과: 0 = 0°, 1 = 180° 회전
pub(crate) type ClsLabel = usize;

const CLS_W: u32 = 160;
const CLS_H: u32 = 80;
const MEAN: [f32; 3] = [0.485, 0.456, 0.406];
const STD: [f32; 3] = [0.229, 0.224, 0.225];
const SCALE: f32 = 1.0 / 255.0;

// cls는 고정 크기(160×80)라 배치를 크게 잡아도 max_w 팽창 문제가 없다.
const CLS_BATCH_SIZE: usize = 32;

fn preprocess_inner(img: &DynamicImage) -> Array3<f32> {
    let resized = img.resize_exact(CLS_W, CLS_H, image::imageops::FilterType::Triangle);
    let rgb = resized.to_rgb8();

    // as_raw()로 연속 메모리 직접 접근: bounds check 없이 RGBRGB... 순서로 순회
    let raw = rgb.as_raw();
    let mut tensor = Array3::<f32>::zeros((3, CLS_H as usize, CLS_W as usize));
    for (i, chunk) in raw.chunks_exact(3).enumerate() {
        let y = i / CLS_W as usize;
        let x = i % CLS_W as usize;
        // BGR 순서 + mean/std 채널 위치 순서 (PaddleOCR 방식)
        tensor[[0, y, x]] = (chunk[2] as f32 * SCALE - MEAN[0]) / STD[0]; // B
        tensor[[1, y, x]] = (chunk[1] as f32 * SCALE - MEAN[1]) / STD[1]; // G
        tensor[[2, y, x]] = (chunk[0] as f32 * SCALE - MEAN[2]) / STD[2]; // R
    }
    tensor
}

fn preprocess(img: &DynamicImage) -> Array4<f32> {
    preprocess_inner(img).insert_axis(ndarray::Axis(0))
}

/// 텍스트 방향 분류. 0=정방향, 1=180° 회전
pub(crate) fn classify(session: &mut Session, img: &DynamicImage) -> Result<ClsLabel, String> {
    let input = preprocess(img);
    let input_values = ort::value::Value::from_array(input).map_err(|e| e.to_string())?;
    let outputs = session
        .run(ort::inputs![input_values])
        .map_err(|e| e.to_string())?;

    let (_shape, data) = outputs[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| format!("cls 출력 추출 실패: {e}"))?;

    let label = data
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.partial_cmp(b.1).unwrap_or(std::cmp::Ordering::Equal))
        .map(|(i, _)| i)
        .unwrap_or(0);

    Ok(label)
}

/// 미리 전처리된 텐서 슬라이스를 배치로 추론한다.
/// 실패(동적 배치 미지원 등)하면 None을 반환한다.
fn try_batch_classify_tensors(
    session: &mut Session,
    tensors: &[Array3<f32>],
) -> Option<Vec<ClsLabel>> {
    let n = tensors.len();
    let mut batch = Array4::<f32>::zeros((n, 3, CLS_H as usize, CLS_W as usize));
    for (i, t) in tensors.iter().enumerate() {
        batch.slice_mut(ndarray::s![i, .., .., ..]).assign(t);
    }
    let input_values = ort::value::Value::from_array(batch).ok()?;
    let outputs = session.run(ort::inputs![input_values]).ok()?;
    let (_shape, data) = outputs[0].try_extract_tensor::<f32>().ok()?;
    if data.len() < n * 2 {
        return None;
    }
    Some(
        (0..n)
            .map(|i| if data[i * 2 + 1] > data[i * 2] { 1 } else { 0 })
            .collect(),
    )
}

/// 미리 전처리된 단일 텐서를 추론한다 (배치 실패 시 폴백용).
fn classify_from_tensor(session: &mut Session, tensor: &Array3<f32>) -> ClsLabel {
    let arr4 = tensor.clone().insert_axis(ndarray::Axis(0));
    (|| -> Option<ClsLabel> {
        let iv = ort::value::Value::from_array(arr4).ok()?;
        let out = session.run(ort::inputs![iv]).ok()?;
        let (_s, d) = out[0].try_extract_tensor::<f32>().ok()?;
        Some(if d.len() >= 2 && d[1] > d[0] { 1 } else { 0 })
    })()
    .unwrap_or(0)
}

/// 여러 이미지를 CLS_BATCH_SIZE 단위 청크로 방향 분류한다.
///
/// 전처리는 rayon으로 병렬 수행해 CPU 대기를 줄인다.
/// 배치 추론이 실패하면 순차 처리로 폴백한다.
pub(crate) fn classify_batch(
    session: &mut Session,
    imgs: &[DynamicImage],
) -> Result<Vec<ClsLabel>, String> {
    match imgs.len() {
        0 => return Ok(vec![]),
        1 => return Ok(vec![classify(session, &imgs[0])?]),
        _ => {}
    }

    // 병렬 CPU 전처리
    let tensors: Vec<Array3<f32>> = imgs.par_iter().map(|img| preprocess_inner(img)).collect();

    let n = tensors.len();
    let mut results = vec![0usize; n];

    for (ci, chunk) in tensors.chunks(CLS_BATCH_SIZE).enumerate() {
        let base = ci * CLS_BATCH_SIZE;
        let labels = if let Some(lbls) = try_batch_classify_tensors(session, chunk) {
            lbls
        } else {
            // 배치 실패 시 청크 내 순차 처리
            chunk
                .iter()
                .map(|t| classify_from_tensor(session, t))
                .collect()
        };
        for (i, label) in labels.into_iter().enumerate() {
            results[base + i] = label;
        }
    }

    Ok(results)
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, Rgb, RgbImage};

    #[test]
    fn BGR_정규화가_채널_위치_순서로_적용된다() {
        let mut img = RgbImage::new(1, 1);
        img.put_pixel(0, 0, Rgb([10, 20, 30]));

        let tensor = preprocess(&DynamicImage::ImageRgb8(img));

        let expected_b = (30.0 * SCALE - MEAN[0]) / STD[0];
        let expected_g = (20.0 * SCALE - MEAN[1]) / STD[1];
        let expected_r = (10.0 * SCALE - MEAN[2]) / STD[2];

        assert!((tensor[[0, 0, 0, 0]] - expected_b).abs() < 1e-6);
        assert!((tensor[[0, 1, 0, 0]] - expected_g).abs() < 1e-6);
        assert!((tensor[[0, 2, 0, 0]] - expected_r).abs() < 1e-6);
    }
}
