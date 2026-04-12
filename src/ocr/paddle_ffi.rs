#![allow(dead_code)]

use crate::services::{OcrDebugDetection, OcrDetection};
use serde::Deserialize;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int};
use std::path::Path;
use std::ptr::NonNull;

#[repr(C)]
struct BuzhiOcrEngine {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn buzhi_ocr_create(
        model_dir: *const c_char,
        use_gpu: c_int,
        err: *mut *mut c_char,
    ) -> *mut BuzhiOcrEngine;
    fn buzhi_ocr_destroy(engine: *mut BuzhiOcrEngine);
    fn buzhi_ocr_run_image_file(
        engine: *mut BuzhiOcrEngine,
        image_path: *const c_char,
        det_resize_long: c_int,
        score_thresh: c_float,
        debug_trace: c_int,
        err: *mut *mut c_char,
    ) -> *mut c_char;
    fn buzhi_ocr_free_string(s: *mut c_char);
}

#[derive(Deserialize)]
struct RawOcrResult {
    detections: Vec<RawDetection>,
    #[serde(default)]
    debug_detections: Vec<RawDebugDetection>,
}

#[derive(Deserialize)]
struct RawDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
}

#[derive(Deserialize)]
struct RawDebugDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
    score: f32,
    accepted: bool,
}

pub(crate) struct PaddleFfiEngine {
    raw: NonNull<BuzhiOcrEngine>,
}

// SAFETY:
// - 이 타입은 외부 C ABI 엔진 핸들을 소유한다.
// - 현재 스켈레톤 브리지는 내부 가변 상태를 노출하지 않으며, 호출은 Rust 쪽에서 공유 참조로만 이뤄진다.
// - 실제 Paddle predictor를 연결할 때도, 여기서 스레드 안전성 보장을 다시 검토해야 한다.
unsafe impl Send for PaddleFfiEngine {}
unsafe impl Sync for PaddleFfiEngine {}

impl PaddleFfiEngine {
    pub(crate) fn new(models_dir: &Path, use_gpu: bool) -> Result<Self, String> {
        let model_dir = CString::new(models_dir.to_string_lossy().as_bytes())
            .map_err(|_| "모델 경로에 NUL 바이트가 포함되어 있습니다".to_string())?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let raw = unsafe { buzhi_ocr_create(model_dir.as_ptr(), use_gpu as c_int, &mut err) };
        if let Some(raw) = NonNull::new(raw) {
            return Ok(Self { raw });
        }
        Err(take_ffi_string(err).unwrap_or_else(|| "Paddle FFI 엔진 생성 실패".to_string()))
    }

    pub(crate) fn run_image_file(
        &self,
        image_path: &Path,
        det_resize_long: u32,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let image_path = CString::new(image_path.to_string_lossy().as_bytes())
            .map_err(|_| "이미지 경로에 NUL 바이트가 포함되어 있습니다".to_string())?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let raw_json = unsafe {
            buzhi_ocr_run_image_file(
                self.raw.as_ptr(),
                image_path.as_ptr(),
                det_resize_long as c_int,
                score_thresh,
                debug_trace as c_int,
                &mut err,
            )
        };
        let Some(raw_json) = NonNull::new(raw_json) else {
            return Err(
                take_ffi_string(err)
                    .unwrap_or_else(|| "Paddle FFI OCR 실행 실패".to_string())
            );
        };

        let json = unsafe { CStr::from_ptr(raw_json.as_ptr()) }
            .to_str()
            .map_err(|e| format!("Paddle FFI JSON UTF-8 파싱 실패: {e}"))?
            .to_string();
        unsafe { buzhi_ocr_free_string(raw_json.as_ptr()) };

        let parsed: RawOcrResult =
            serde_json::from_str(&json).map_err(|e| format!("Paddle FFI JSON 파싱 실패: {e}"))?;

        Ok((
            parsed
                .detections
                .into_iter()
                .map(|item| (item.polygon, item.text))
                .collect(),
            parsed
                .debug_detections
                .into_iter()
                .map(|item| (item.polygon, item.text, item.score, item.accepted))
                .collect(),
        ))
    }
}

impl Drop for PaddleFfiEngine {
    fn drop(&mut self) {
        unsafe { buzhi_ocr_destroy(self.raw.as_ptr()) };
    }
}

fn take_ffi_string(ptr: *mut c_char) -> Option<String> {
    let ptr = NonNull::new(ptr)?;
    let message = unsafe { CStr::from_ptr(ptr.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    unsafe { buzhi_ocr_free_string(ptr.as_ptr()) };
    Some(message)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn ffi_json을_공용_detection_형식으로_변환한다() {
        let raw = r#"{
          "detections": [
            { "polygon": [[1.0, 2.0], [3.0, 4.0]], "text": "hello" }
          ],
          "debug_detections": [
            {
              "polygon": [[5.0, 6.0], [7.0, 8.0]],
              "text": "world",
              "score": 0.9,
              "accepted": true
            }
          ]
        }"#;

        let parsed: RawOcrResult = serde_json::from_str(raw).expect("JSON 파싱 실패");
        let detections: Vec<OcrDetection> = parsed
            .detections
            .into_iter()
            .map(|item| (item.polygon, item.text))
            .collect();

        assert_eq!(detections.len(), 1);
        assert_eq!(detections[0].1, "hello");
    }

    #[test]
    fn paddle_ffi_엔진을_생성할_수_있다() {
        let Some(model_dir) = std::env::var_os("PADDLE_MODEL_DIR").map(PathBuf::from) else {
            eprintln!("PADDLE_MODEL_DIR 미설정 — 건너뜀");
            return;
        };

        let result = PaddleFfiEngine::new(&model_dir, false);
        assert!(result.is_ok(), "PaddleFfiEngine 생성 실패: {:?}", result.err());
    }
}
