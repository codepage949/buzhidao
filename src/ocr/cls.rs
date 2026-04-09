use image::DynamicImage;
use ndarray::{Array3, Array4};
use ort::session::Session;

/// 분류 결과: 0 = 0°, 1 = 180° 회전
pub(crate) type ClsLabel = usize;

const CLS_W: u32 = 160;
const CLS_H: u32 = 80;
const MEAN: [f32; 3] = [0.485, 0.456, 0.406];
const STD: [f32; 3] = [0.229, 0.224, 0.225];
const SCALE: f32 = 1.0 / 255.0;

fn preprocess(img: &DynamicImage) -> Array4<f32> {
    let resized = img.resize_exact(CLS_W, CLS_H, image::imageops::FilterType::Lanczos3);
    let rgb = resized.to_rgb8();

    let mut tensor = Array3::<f32>::zeros((3, CLS_H as usize, CLS_W as usize));
    for y in 0..CLS_H as usize {
        for x in 0..CLS_W as usize {
            let pixel = rgb.get_pixel(x as u32, y as u32);
            // BGR 순서 + mean/std 채널 위치 순서 (PaddleOCR 방식)
            tensor[[0, y, x]] = (pixel[2] as f32 * SCALE - MEAN[0]) / STD[0]; // B
            tensor[[1, y, x]] = (pixel[1] as f32 * SCALE - MEAN[1]) / STD[1]; // G
            tensor[[2, y, x]] = (pixel[0] as f32 * SCALE - MEAN[2]) / STD[2]; // R
        }
    }

    tensor.insert_axis(ndarray::Axis(0))
}

/// 텍스트 방향 분류. 0=정방향, 1=180° 회전
pub(crate) fn classify(session: &mut Session, img: &DynamicImage) -> Result<ClsLabel, String> {
    let input = preprocess(img);
    let input_values = ort::value::Value::from_array(input).map_err(|e| e.to_string())?;
    let outputs = session.run(ort::inputs![input_values]).map_err(|e| e.to_string())?;

    let (_shape, data) = outputs[0]
        .try_extract_tensor::<f32>()
        .map_err(|e| format!("cls 출력 추출 실패: {e}"))?;

    let label = data
        .iter()
        .enumerate()
        .max_by(|a, b| a.1.partial_cmp(b.1).unwrap())
        .map(|(i, _)| i)
        .unwrap_or(0);

    Ok(label)
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
