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
            for c in 0..3 {
                tensor[[c, y, x]] = (pixel[c] as f32 * SCALE - MEAN[c]) / STD[c];
            }
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
