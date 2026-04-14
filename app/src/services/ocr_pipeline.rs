use crate::config::Config;
use crate::ocr::OcrBackend;
use crate::services::capture::CaptureInfo;
use serde::Serialize;

pub(crate) type OcrDetection = (Vec<[f64; 2]>, String);
pub(crate) type OcrDebugDetection = (Vec<[f64; 2]>, String, f32, bool);

#[derive(Serialize, Clone)]
pub(crate) struct OcrResultPayload {
    pub(crate) detections: Vec<OcrDetection>,
    pub(crate) debug_detections: Vec<OcrDebugDetection>,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
    pub(crate) source: String,
    pub(crate) word_gap: i32,
    pub(crate) line_gap: i32,
    pub(crate) debug_trace: bool,
}

pub(crate) fn crop_capture_to_region(
    capture: CaptureInfo,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(image::DynamicImage, f64, f64, u32, u32), String> {
    let (crop_x, crop_y, crop_w, crop_h) = selection_rect_to_image_rect(
        capture.orig_width,
        capture.orig_height,
        rect_x,
        rect_y,
        rect_w,
        rect_h,
        viewport_w,
        viewport_h,
    )?;

    let cropped = capture.image.crop_imm(crop_x, crop_y, crop_w, crop_h);
    Ok((
        cropped,
        crop_x as f64,
        crop_y as f64,
        capture.orig_width,
        capture.orig_height,
    ))
}

fn for_each_point(payload: &mut OcrResultPayload, mut f: impl FnMut(&mut [f64; 2])) {
    for (polygon, _) in &mut payload.detections {
        for point in polygon {
            f(point);
        }
    }
    for (polygon, _, _, _) in &mut payload.debug_detections {
        for point in polygon {
            f(point);
        }
    }
}

pub(crate) fn offset_ocr_result(payload: &mut OcrResultPayload, offset_x: f64, offset_y: f64) {
    for_each_point(payload, |p| {
        p[0] += offset_x;
        p[1] += offset_y;
    });
}

pub(crate) fn scale_ocr_result(payload: &mut OcrResultPayload, scale_x: f64, scale_y: f64) {
    for_each_point(payload, |p| {
        p[0] *= scale_x;
        p[1] *= scale_y;
    });
}

fn resize_image_to_max_width(
    dyn_img: image::DynamicImage,
    max_width: u32,
) -> (image::DynamicImage, f64, f64) {
    if dyn_img.width() <= max_width || max_width == 0 {
        return (dyn_img, 1.0, 1.0);
    }

    let ratio = max_width as f64 / dyn_img.width() as f64;
    let target_height = ((dyn_img.height() as f64) * ratio).round().max(1.0) as u32;
    let resized = dyn_img.resize_exact(
        max_width,
        target_height,
        image::imageops::FilterType::Lanczos3,
    );
    let scale_x = dyn_img.width() as f64 / resized.width() as f64;
    let scale_y = dyn_img.height() as f64 / resized.height() as f64;
    (resized, scale_x, scale_y)
}

fn selection_rect_to_image_rect(
    image_width: u32,
    image_height: u32,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(u32, u32, u32, u32), String> {
    if image_width == 0 || image_height == 0 {
        return Err("캡처 이미지 크기가 0입니다".to_string());
    }
    if viewport_w <= 0.0 || viewport_h <= 0.0 {
        return Err("뷰포트 크기가 유효하지 않습니다".to_string());
    }

    let x1 = rect_x.min(rect_x + rect_w).max(0.0).min(viewport_w);
    let y1 = rect_y.min(rect_y + rect_h).max(0.0).min(viewport_h);
    let x2 = rect_x.max(rect_x + rect_w).max(0.0).min(viewport_w);
    let y2 = rect_y.max(rect_y + rect_h).max(0.0).min(viewport_h);

    let scale_x = image_width as f64 / viewport_w;
    let scale_y = image_height as f64 / viewport_h;
    let crop_x = (x1 * scale_x).floor() as u32;
    let crop_y = (y1 * scale_y).floor() as u32;
    let mut crop_w = ((x2 - x1) * scale_x).ceil() as u32;
    let mut crop_h = ((y2 - y1) * scale_y).ceil() as u32;

    crop_w = crop_w.max(1).min(image_width.saturating_sub(crop_x));
    crop_h = crop_h.max(1).min(image_height.saturating_sub(crop_y));

    Ok((crop_x, crop_y, crop_w, crop_h))
}

pub(crate) fn run_ocr(
    cfg: &Config,
    engine: &OcrBackend,
    dyn_img: image::DynamicImage,
    orig_width: u32,
    orig_height: u32,
) -> Result<OcrResultPayload, String> {
    let (prepared_img, scale_x, scale_y) =
        resize_image_to_max_width(dyn_img, engine.resize_width_before_ocr());
    let t0 = std::time::Instant::now();
    let (detections, debug_detections) = engine.run_image(
        &prepared_img,
        &cfg.source,
        cfg.score_thresh,
        cfg.ocr_debug_trace,
    )?;
    eprintln!(
        "[OCR] backend 전체 처리: {:.0}ms ({}×{}, resize_width {})",
        t0.elapsed().as_millis(),
        prepared_img.width(),
        prepared_img.height(),
        engine.resize_width_before_ocr()
    );

    let mut payload = OcrResultPayload {
        detections,
        debug_detections,
        orig_width,
        orig_height,
        source: cfg.source.clone(),
        word_gap: cfg.word_gap,
        line_gap: cfg.line_gap,
        debug_trace: cfg.ocr_debug_trace,
    };

    if (scale_x - 1.0).abs() > f64::EPSILON || (scale_y - 1.0).abs() > f64::EPSILON {
        scale_ocr_result(&mut payload, scale_x, scale_y);
    }

    Ok(payload)
}

#[cfg(test)]
mod tests {
    use super::{
        offset_ocr_result, resize_image_to_max_width, scale_ocr_result,
        selection_rect_to_image_rect, OcrResultPayload,
    };
    use image::{DynamicImage, Rgba, RgbaImage};

    #[test]
    fn 선택_영역을_원본_이미지_좌표로_변환한다() {
        let rect =
            selection_rect_to_image_rect(2560, 1440, 100.0, 50.0, 300.0, 200.0, 1280.0, 720.0)
                .expect("좌표 변환 실패");

        assert_eq!(rect, (200, 100, 600, 400));
    }

    #[test]
    fn ocr_결과에_crop_오프셋을_더한다() {
        let mut payload = OcrResultPayload {
            detections: vec![(vec![[10.0, 20.0], [30.0, 40.0]], "text".to_string())],
            debug_detections: vec![(vec![[1.0, 2.0], [3.0, 4.0]], "dbg".to_string(), 0.9, true)],
            orig_width: 100,
            orig_height: 100,
            source: "en".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        offset_ocr_result(&mut payload, 5.0, 7.0);

        assert_eq!(payload.detections[0].0[0], [15.0, 27.0]);
        assert_eq!(payload.debug_detections[0].0[1], [8.0, 11.0]);
    }

    #[test]
    fn 넓은_이미지는_1024w로_축소하고_배율을_반환한다() {
        let img = DynamicImage::ImageRgba8(RgbaImage::from_pixel(2048, 1024, Rgba([1, 2, 3, 4])));

        let (resized, scale_x, scale_y) = resize_image_to_max_width(img, 1024);

        assert_eq!(resized.width(), 1024);
        assert_eq!(resized.height(), 512);
        assert!((scale_x - 2.0).abs() < f64::EPSILON);
        assert!((scale_y - 2.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 선택_영역이_이미지_크기_0이면_실패한다() {
        let err = selection_rect_to_image_rect(0, 100, 0.0, 0.0, 10.0, 10.0, 10.0, 10.0)
            .expect_err("이미지 크기 0이면 실패해야 한다");
        assert!(err.contains("캡처 이미지 크기"));
    }

    #[test]
    fn 선택_영역이_뷰포트_크기_0_이하이면_실패한다() {
        let err = selection_rect_to_image_rect(100, 100, 0.0, 0.0, 10.0, 10.0, 0.0, 10.0)
            .expect_err("뷰포트 크기 0이면 실패해야 한다");
        assert!(err.contains("뷰포트"));
    }

    #[test]
    fn 음수_크기의_선택_영역은_좌상단으로_정규화된다() {
        let rect = selection_rect_to_image_rect(100, 100, 80.0, 60.0, -40.0, -30.0, 100.0, 100.0)
            .expect("음수 선택 영역 처리 실패");
        assert_eq!(rect, (40, 30, 40, 30));
    }

    #[test]
    fn 뷰포트_밖_선택은_이미지_범위_안으로_클램프된다() {
        let rect = selection_rect_to_image_rect(200, 200, -50.0, -50.0, 400.0, 400.0, 100.0, 100.0)
            .expect("클램프 실패");
        assert_eq!(rect, (0, 0, 200, 200));
    }

    #[test]
    fn 이미지_폭이_최대_이하이면_원본을_유지한다() {
        let img = DynamicImage::ImageRgba8(RgbaImage::from_pixel(512, 256, Rgba([9, 9, 9, 9])));
        let (out, sx, sy) = resize_image_to_max_width(img, 1024);
        assert_eq!(out.width(), 512);
        assert_eq!(out.height(), 256);
        assert!((sx - 1.0).abs() < f64::EPSILON);
        assert!((sy - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 최대_폭이_0이면_이미지를_그대로_반환한다() {
        let img = DynamicImage::ImageRgba8(RgbaImage::from_pixel(2000, 1000, Rgba([0, 0, 0, 0])));
        let (out, sx, sy) = resize_image_to_max_width(img, 0);
        assert_eq!(out.width(), 2000);
        assert!((sx - 1.0).abs() < f64::EPSILON);
        assert!((sy - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 축소된_ocr_좌표를_원본_배율로_복원한다() {
        let mut payload = OcrResultPayload {
            detections: vec![(vec![[10.0, 20.0], [30.0, 40.0]], "text".to_string())],
            debug_detections: vec![(vec![[1.0, 2.0], [3.0, 4.0]], "dbg".to_string(), 0.9, true)],
            orig_width: 100,
            orig_height: 100,
            source: "en".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        scale_ocr_result(&mut payload, 2.0, 3.0);

        assert_eq!(payload.detections[0].0[0], [20.0, 60.0]);
        assert_eq!(payload.debug_detections[0].0[1], [6.0, 12.0]);
    }
}
