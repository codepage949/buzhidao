mod cls;
mod det;
mod rec;

use image::DynamicImage;
use ort::session::Session;
use std::path::Path;
use std::sync::Mutex;

use crate::services::OcrDetection;

/// ONNX Runtime 기반 OCR 엔진.
/// det(검출) → cls(방향분류) → rec(인식) 파이프라인.
pub(crate) struct OcrEngine {
    det_session: Mutex<Session>,
    cls_session: Mutex<Session>,
    rec_session: Mutex<Session>,
    dict: Vec<String>,
}

impl OcrEngine {
    pub(crate) fn new(models_dir: &Path) -> Result<Self, String> {
        let det_session = Session::builder()
            .map_err(|e| format!("det 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("det.onnx"))
            .map_err(|e| format!("det 모델 로드 실패: {e}"))?;

        let cls_session = Session::builder()
            .map_err(|e| format!("cls 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("cls.onnx"))
            .map_err(|e| format!("cls 모델 로드 실패: {e}"))?;

        let rec_session = Session::builder()
            .map_err(|e| format!("rec 세션 빌더 실패: {e}"))?
            .commit_from_file(models_dir.join("rec.onnx"))
            .map_err(|e| format!("rec 모델 로드 실패: {e}"))?;

        let dict_path = models_dir.join("rec_dict.txt");
        let dict_content =
            std::fs::read_to_string(&dict_path).map_err(|e| format!("사전 파일 로드 실패: {e}"))?;
        let dict: Vec<String> = dict_content.lines().map(|s| s.to_string()).collect();

        Ok(Self {
            det_session: Mutex::new(det_session),
            cls_session: Mutex::new(cls_session),
            rec_session: Mutex::new(rec_session),
            dict,
        })
    }

    pub(crate) fn predict(
        &self,
        img: &DynamicImage,
        score_thresh: f32,
    ) -> Result<Vec<OcrDetection>, String> {
        let boxes = {
            let mut session = self.det_session.lock().unwrap();
            det::detect(&mut session, img)?
        };

        let mut detections = Vec::new();

        for box_pts in &boxes {
            let cropped = crop_box(img, box_pts);

            let label = {
                let mut session = self.cls_session.lock().unwrap();
                cls::classify(&mut session, &cropped)?
            };
            let oriented = if label == 1 {
                cropped.rotate180()
            } else {
                cropped
            };

            let (text, score) = {
                let mut session = self.rec_session.lock().unwrap();
                rec::recognize(&mut session, &oriented, &self.dict)?
            };

            if score >= score_thresh && !text.is_empty() {
                let polygon: Vec<[f64; 2]> = box_pts.iter().map(|&pt| pt).collect();
                detections.push((polygon, text));
            }
        }

        Ok(detections)
    }
}

fn crop_box(img: &DynamicImage, box_pts: &[[f64; 2]; 4]) -> DynamicImage {
    let mut min_x = f64::MAX;
    let mut min_y = f64::MAX;
    let mut max_x = f64::MIN;
    let mut max_y = f64::MIN;

    for &[x, y] in box_pts {
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }

    let x = min_x.max(0.0) as u32;
    let y = min_y.max(0.0) as u32;
    let w = ((max_x - min_x).ceil() as u32).max(1).min(img.width().saturating_sub(x));
    let h = ((max_y - min_y).ceil() as u32).max(1).min(img.height().saturating_sub(y));

    img.crop_imm(x, y, w, h)
}
