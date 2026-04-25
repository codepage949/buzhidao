#![allow(dead_code)]

use crate::services::{OcrBoundsPayload, OcrDebugDetection, OcrDetection};
use serde::Deserialize;
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_float, c_int};
use std::path::Path;
use std::path::PathBuf;
use std::ptr::NonNull;
use std::sync::LazyLock;
use std::sync::Mutex;

#[repr(C)]
struct BuzhiOcrEngine {
    _private: [u8; 0],
}

unsafe extern "C" {
    fn buzhi_ocr_create(
        model_dir: *const c_char,
        use_gpu: c_int,
        source: *const c_char,
        err: *mut *mut c_char,
    ) -> *mut BuzhiOcrEngine;
    fn buzhi_ocr_destroy(engine: *mut BuzhiOcrEngine);
    fn buzhi_ocr_warmup_predictors(engine: *mut BuzhiOcrEngine, err: *mut *mut c_char) -> c_int;
    fn buzhi_ocr_run_image_file(
        engine: *mut BuzhiOcrEngine,
        image_path: *const c_char,
        det_resize_long: c_int,
        score_thresh: c_float,
        debug_trace: c_int,
        err: *mut *mut c_char,
    ) -> *mut c_char;
    fn buzhi_ocr_run_image_file_result(
        engine: *mut BuzhiOcrEngine,
        image_path: *const c_char,
        det_resize_long: c_int,
        score_thresh: c_float,
        debug_trace: c_int,
        err: *mut *mut c_char,
    ) -> *mut BuzhiOcrResult;
    fn buzhi_ocr_run_image_rgba_result(
        engine: *mut BuzhiOcrEngine,
        rgba: *const u8,
        width: c_int,
        height: c_int,
        stride: c_int,
        det_resize_long: c_int,
        score_thresh: c_float,
        debug_trace: c_int,
        err: *mut *mut c_char,
    ) -> *mut BuzhiOcrResult;
    fn buzhi_ocr_free_string(s: *mut c_char);
    fn buzhi_ocr_free_result(result: *mut BuzhiOcrResult);
}

#[repr(C)]
struct BuzhiOcrPoint {
    x: f32,
    y: f32,
}

#[repr(C)]
struct BuzhiOcrDetection {
    polygon: [BuzhiOcrPoint; 4],
    text: *mut c_char,
}

#[repr(C)]
struct BuzhiOcrDebugDetection {
    polygon: [BuzhiOcrPoint; 4],
    text: *mut c_char,
    score: f32,
    accepted: c_int,
}

#[repr(C)]
struct BuzhiOcrResult {
    detections: *mut BuzhiOcrDetection,
    detection_count: c_int,
    debug_detections: *mut BuzhiOcrDebugDetection,
    debug_detection_count: c_int,
}

#[derive(Deserialize)]
struct LangEntry {
    code: String,
}

static SUPPORTED_LANG_CODES: LazyLock<Vec<String>> = LazyLock::new(|| {
    let entries: Vec<LangEntry> = serde_json::from_str(include_str!("../../shared/langs.json"))
        .expect("shared/langs.json 파싱 실패");
    entries.into_iter().map(|entry| entry.code).collect()
});

pub(crate) struct PaddleFfiEngine {
    model_dir: PathBuf,
    use_gpu: bool,
    state: Mutex<EngineState>,
}

struct EngineState {
    raw: Option<NonNull<BuzhiOcrEngine>>,
    source: String,
    warmed: bool,
}

const WARMUP_IMAGE_WIDTH: u32 = 64;
const WARMUP_IMAGE_HEIGHT: u32 = 64;
const WARMUP_PIXEL_STRIDE: u32 = 4;

unsafe impl Send for PaddleFfiEngine {}
unsafe impl Sync for PaddleFfiEngine {}

impl PaddleFfiEngine {
    pub(crate) fn new(model_dir: &Path, use_gpu: bool, source: &str) -> Result<Self, String> {
        Ok(Self {
            model_dir: model_dir.to_path_buf(),
            use_gpu,
            state: Mutex::new(EngineState {
                raw: None,
                source: normalize_source(source),
                warmed: false,
            }),
        })
    }

    pub(crate) fn warmup(&self) -> Result<(), String> {
        let mut state = self
            .state
            .lock()
            .map_err(|_| "FFI 엔진 상태 잠금 실패".to_string())?;
        if state.raw.is_none() {
            let raw = self.create_engine_locked(&mut state)?;
            state.raw = Some(raw);
        }
        if !state.warmed {
            self.warmup_predictors_locked(&mut state)?;
            state.warmed = true;
        }
        Ok(())
    }

    pub(crate) fn set_lang(&self, source: &str) -> Result<(), String> {
        let requested = normalize_source(source);
        let mut state = self
            .state
            .lock()
            .map_err(|_| "FFI 엔진 상태 잠금 실패".to_string())?;
        if state.source == requested {
            return Ok(());
        }
        state.source = requested;
        shutdown_state(&mut state);
        Ok(())
    }

    pub(crate) fn run_image_file(
        &self,
        image_path: &Path,
        det_resize_long: u32,
        score_thresh: f32,
        debug_trace: bool,
        source: &str,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let requested = normalize_source(source);
        let image_path = CString::new(image_path.to_string_lossy().as_bytes())
            .map_err(|_| "이미지 경로에 NUL 바이트가 포함되어 있습니다".to_string())?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let mut state = self
            .state
            .lock()
            .map_err(|_| "FFI 엔진 상태 잠금 실패".to_string())?;
        if state.source != requested || state.raw.is_none() {
            state.source = requested;
            shutdown_state(&mut state);
            state.raw = Some(self.create_engine_locked(&mut state)?);
        }
        let raw_engine = state
            .raw
            .ok_or_else(|| "FFI 엔진이 초기화되지 않았습니다".to_string())?;

        let raw_result = unsafe {
            buzhi_ocr_run_image_file_result(
                raw_engine.as_ptr(),
                image_path.as_ptr(),
                det_resize_long as c_int,
                score_thresh,
                debug_trace as c_int,
                &mut err,
            )
        };
        let Some(raw_result) = NonNull::new(raw_result) else {
            return Err(
                take_ffi_string(err).unwrap_or_else(|| "Paddle FFI OCR 실행 실패".to_string())
            );
        };
        let result = unsafe { convert_ffi_result(raw_result.as_ptr()) };
        unsafe { buzhi_ocr_free_result(raw_result.as_ptr()) };
        state.warmed = true;
        Ok(result)
    }

    pub(crate) fn run_image_rgba(
        &self,
        rgba: &[u8],
        width: u32,
        height: u32,
        det_resize_long: u32,
        score_thresh: f32,
        debug_trace: bool,
        source: &str,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let requested = normalize_source(source);
        let mut err: *mut c_char = std::ptr::null_mut();
        let mut state = self
            .state
            .lock()
            .map_err(|_| "FFI 엔진 상태 잠금 실패".to_string())?;
        if state.source != requested || state.raw.is_none() {
            state.source = requested;
            shutdown_state(&mut state);
            state.raw = Some(self.create_engine_locked(&mut state)?);
        }
        let raw_engine = state
            .raw
            .ok_or_else(|| "FFI 엔진이 초기화되지 않았습니다".to_string())?;
        let stride = width
            .checked_mul(4)
            .ok_or_else(|| "이미지 stride 계산 실패".to_string())?;
        let expected_len = stride
            .checked_mul(height)
            .ok_or_else(|| "이미지 길이 계산 실패".to_string())?;
        if rgba.len() != expected_len as usize {
            return Err(format!(
                "RGBA 버퍼 길이가 잘못되었습니다: expected={}, actual={}",
                expected_len,
                rgba.len()
            ));
        }

        let raw_result = unsafe {
            buzhi_ocr_run_image_rgba_result(
                raw_engine.as_ptr(),
                rgba.as_ptr(),
                width as c_int,
                height as c_int,
                stride as c_int,
                det_resize_long as c_int,
                score_thresh,
                debug_trace as c_int,
                &mut err,
            )
        };
        let Some(raw_result) = NonNull::new(raw_result) else {
            return Err(
                take_ffi_string(err).unwrap_or_else(|| "Paddle FFI OCR 실행 실패".to_string())
            );
        };
        let result = unsafe { convert_ffi_result(raw_result.as_ptr()) };
        unsafe { buzhi_ocr_free_result(raw_result.as_ptr()) };
        state.warmed = true;
        Ok(result)
    }

    fn warmup_predictors_locked(&self, state: &mut EngineState) -> Result<(), String> {
        let raw_engine = state
            .raw
            .ok_or_else(|| "FFI 엔진이 초기화되지 않았습니다".to_string())?;
        let mut err: *mut c_char = std::ptr::null_mut();
        let ok = unsafe { buzhi_ocr_warmup_predictors(raw_engine.as_ptr(), &mut err) };
        if ok == 0 {
            return Err(
                take_ffi_string(err)
                    .unwrap_or_else(|| "Paddle FFI predictor warmup 실행 실패".to_string())
            );
        }
        Ok(())
    }

    fn create_engine_locked(&self, state: &EngineState) -> Result<NonNull<BuzhiOcrEngine>, String> {
        let mut err: *mut c_char = std::ptr::null_mut();
        let model_dir = self.model_dir.to_string_lossy().to_string();
        let source = state.source.clone();
        let model_dir = CString::new(model_dir.as_bytes())
            .map_err(|_| "모델 경로에 NUL 바이트가 포함되어 있습니다".to_string())?;
        let source = CString::new(source.as_bytes())
            .map_err(|_| "OCR source에 NUL 바이트가 포함되어 있습니다".to_string())?;
        let raw = unsafe {
            buzhi_ocr_create(
                model_dir.as_ptr(),
                self.use_gpu as c_int,
                source.as_ptr(),
                &mut err,
            )
        };
        if let Some(raw) = NonNull::new(raw) {
            return Ok(raw);
        }
        Err(take_ffi_string(err).unwrap_or_else(|| "Paddle FFI 엔진 생성 실패".to_string()))
    }
}

impl Drop for PaddleFfiEngine {
    fn drop(&mut self) {
        if let Ok(mut state) = self.state.lock() {
            shutdown_state(&mut state);
        }
    }
}

fn normalize_source(source: &str) -> String {
    let source = source.trim().to_ascii_lowercase();
    if source.is_empty() {
        return "en".to_string();
    }
    if SUPPORTED_LANG_CODES.iter().any(|code| code == &source) {
        return source;
    }
    if source == "en" || source == "eng" || source == "english" {
        return "en".to_string();
    }
    if source == "cn" || source == "zh" || source == "chi" || source == "chinese" {
        return "ch".to_string();
    }
    if source.starts_with("ch_") || source.starts_with("zh-") || source.starts_with("zh_") {
        return "ch".to_string();
    }
    "en".to_string()
}

fn shutdown_state(state: &mut EngineState) {
    if let Some(raw) = state.raw.take() {
        unsafe {
            buzhi_ocr_destroy(raw.as_ptr());
        }
    }
    state.warmed = false;
}

fn warmup_image_rgba() -> (Vec<u8>, u32, u32, u32) {
    let stride = WARMUP_IMAGE_WIDTH * WARMUP_PIXEL_STRIDE;
    let rgba = vec![255u8; (stride * WARMUP_IMAGE_HEIGHT) as usize];
    (rgba, WARMUP_IMAGE_WIDTH, WARMUP_IMAGE_HEIGHT, stride)
}

fn take_ffi_string(ptr: *mut c_char) -> Option<String> {
    let ptr = NonNull::new(ptr)?;
    let message = unsafe { CStr::from_ptr(ptr.as_ptr()) }
        .to_string_lossy()
        .into_owned();
    unsafe { buzhi_ocr_free_string(ptr.as_ptr()) };
    Some(message)
}

unsafe fn convert_ffi_result(
    raw: *const BuzhiOcrResult,
) -> (Vec<OcrDetection>, Vec<OcrDebugDetection>) {
    let raw = &*raw;
    let detections = if raw.detections.is_null() || raw.detection_count <= 0 {
        Vec::new()
    } else {
        let raw_detections =
            std::slice::from_raw_parts(raw.detections, raw.detection_count as usize);
        let mut detections = Vec::with_capacity(raw_detections.len());
        for item in raw_detections {
            let text = if item.text.is_null() {
                String::new()
            } else {
                CStr::from_ptr(item.text).to_string_lossy().into_owned()
            };
            detections.push((bounds_from_polygon(&item.polygon), text));
        }
        detections
    };
    let debug_detections = if raw.debug_detections.is_null() || raw.debug_detection_count <= 0 {
        Vec::new()
    } else {
        let raw_debug_detections =
            std::slice::from_raw_parts(raw.debug_detections, raw.debug_detection_count as usize);
        let mut debug_detections = Vec::with_capacity(raw_debug_detections.len());
        for item in raw_debug_detections {
            let text = if item.text.is_null() {
                String::new()
            } else {
                CStr::from_ptr(item.text).to_string_lossy().into_owned()
            };
            debug_detections.push((
                bounds_from_polygon(&item.polygon),
                text,
                item.score,
                item.accepted != 0,
            ));
        }
        debug_detections
    };
    (detections, debug_detections)
}

fn bounds_from_polygon(polygon: &[BuzhiOcrPoint; 4]) -> OcrBoundsPayload {
    let mut min_x = f64::INFINITY;
    let mut min_y = f64::INFINITY;
    let mut max_x = f64::NEG_INFINITY;
    let mut max_y = f64::NEG_INFINITY;
    for point in polygon {
        let x = point.x as f64;
        let y = point.y as f64;
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }
    OcrBoundsPayload {
        x: min_x,
        y: min_y,
        width: (max_x - min_x).max(0.0),
        height: (max_y - min_y).max(0.0),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use image::{DynamicImage, ImageBuffer, ImageFormat, Rgba};
    use std::ffi::CString;
    use std::io::Read;
    use std::path::PathBuf;

    fn bmp_header(path: &Path) -> (u16, u32) {
        let mut file = std::fs::File::open(path).expect("BMP 파일 열기 실패");
        let mut header = [0u8; 34];
        file.read_exact(&mut header).expect("BMP 헤더 읽기 실패");
        let bits_per_pixel = u16::from_le_bytes([header[28], header[29]]);
        let compression = u32::from_le_bytes([header[30], header[31], header[32], header[33]]);
        (bits_per_pixel, compression)
    }

    fn paddle_ocr_cache_roots() -> Vec<PathBuf> {
        let mut roots = Vec::new();
        if let Some(home) = dirs::home_dir() {
            roots.push(home.join(".paddlex").join("official_models"));
            roots.push(home.join(".paddleocr"));
        }
        roots
    }

    #[test]
    fn 네이티브_ffi_결과를_공용_detection_형식으로_변환한다() {
        let detection_text = CString::new("hello").expect("detection text 생성 실패");
        let debug_text = CString::new("world").expect("debug text 생성 실패");
        let mut detections = [BuzhiOcrDetection {
            polygon: [
                BuzhiOcrPoint { x: 1.0, y: 2.0 },
                BuzhiOcrPoint { x: 3.0, y: 4.0 },
                BuzhiOcrPoint { x: 5.0, y: 6.0 },
                BuzhiOcrPoint { x: 7.0, y: 8.0 },
            ],
            text: detection_text.as_ptr() as *mut c_char,
        }];
        let mut debug_detections = [BuzhiOcrDebugDetection {
            polygon: [
                BuzhiOcrPoint { x: 10.0, y: 20.0 },
                BuzhiOcrPoint { x: 30.0, y: 40.0 },
                BuzhiOcrPoint { x: 50.0, y: 60.0 },
                BuzhiOcrPoint { x: 70.0, y: 80.0 },
            ],
            text: debug_text.as_ptr() as *mut c_char,
            score: 0.9,
            accepted: 1,
        }];
        let raw = BuzhiOcrResult {
            detections: detections.as_mut_ptr(),
            detection_count: detections.len() as c_int,
            debug_detections: debug_detections.as_mut_ptr(),
            debug_detection_count: debug_detections.len() as c_int,
        };

        let (detections, debug_detections) = unsafe { convert_ffi_result(&raw) };

        assert_eq!(
            detections,
            vec![(
                OcrBoundsPayload {
                    x: 1.0,
                    y: 2.0,
                    width: 6.0,
                    height: 6.0,
                },
                "hello".to_string()
            )]
        );
        assert_eq!(
            debug_detections,
            vec![(
                OcrBoundsPayload {
                    x: 10.0,
                    y: 20.0,
                    width: 60.0,
                    height: 60.0,
                },
                "world".to_string(),
                0.9,
                true,
            )]
        );
    }

    #[test]
    fn source_정규화는_지원하지_않는_언어를_기본값_en으로_돌린다() {
        assert_eq!(normalize_source("ch"), "ch");
        assert_eq!(normalize_source("cn"), "ch");
        assert_eq!(normalize_source("zh"), "ch");
        assert_eq!(normalize_source("zh-cn"), "ch");
        assert_eq!(normalize_source("ch_tra"), "ch_tra");
        assert_eq!(normalize_source("japan"), "japan");
        assert_eq!(normalize_source("unknown"), "en");
    }

    #[test]
    fn paddle_ffi_엔진_생성은_warmup_없이도_성공한다() {
        let model_dir = PathBuf::from("non-existent-model-root");
        let result = PaddleFfiEngine::new(&model_dir, false, "en");
        assert!(result.is_ok());
    }

    #[test]
    fn paddle_ffi_엔진_생성후_raw_상태는_아직_비어있다() {
        let model_dir = PathBuf::from("non-existent-model-root");
        let engine = PaddleFfiEngine::new(&model_dir, false, "en").expect("엔진 생성 실패");
        let state = engine.state.lock().expect("엔진 상태 잠금 실패");
        assert!(state.raw.is_none());
        assert!(!state.warmed);
    }

    #[test]
    fn warmup_더미_이미지는_64x64_rgba_흰색으로_구성된다() {
        let (rgba, width, height, stride) = warmup_image_rgba();
        assert_eq!(width, 64);
        assert_eq!(height, 64);
        assert_eq!(stride, 256);
        assert_eq!(rgba.len(), (width * height * 4) as usize);
        assert!(rgba.iter().all(|pixel| *pixel == 255));
    }

    #[test]
    fn shutdown_state는_warmed_상태도_false로_되돌린다() {
        let mut state = EngineState {
            raw: None,
            source: "en".to_string(),
            warmed: true,
        };

        shutdown_state(&mut state);

        assert!(state.raw.is_none());
        assert!(!state.warmed);
    }

    #[test]
    fn bmp_입력은_원본_경로를_그대로_사용한다() {
        let source_path = std::env::temp_dir().join("buzhidao-prepared-image-test.bmp");
        DynamicImage::ImageRgba8(ImageBuffer::<Rgba<u8>, Vec<u8>>::new(1, 1))
            .save_with_format(&source_path, ImageFormat::Bmp)
            .expect("테스트 BMP 저장 실패");
        assert!(source_path.exists());
        let (bits_per_pixel, compression) = bmp_header(&source_path);
        assert!(bits_per_pixel == 24 || bits_per_pixel == 32);
        assert!(compression == 0 || compression == 3);

        std::fs::remove_file(source_path).expect("테스트 BMP 삭제 실패");
    }

    #[test]
    fn png_입력도_원본_경로를_그대로_사용한다() {
        let source_path = std::env::temp_dir().join("buzhidao-ffi-original-image-test.png");
        DynamicImage::ImageRgba8(ImageBuffer::<Rgba<u8>, Vec<u8>>::new(1, 1))
            .save_with_format(&source_path, ImageFormat::Png)
            .expect("테스트 PNG 저장 실패");
        assert!(source_path.exists());
        std::fs::remove_file(source_path).expect("테스트 PNG 삭제 실패");
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn _1_png를_ffi로_실행해서_결과를_출력한다() {
        if std::env::var("BUZHIDAO_RUN_FFI_SAMPLE_TEST").unwrap_or_default() != "1" {
            eprintln!("샘플 FFI OCR 테스트는 BUZHIDAO_RUN_FFI_SAMPLE_TEST=1일 때만 실행합니다.");
            return;
        }
        let model_candidates = paddle_ocr_cache_roots();
        let model_dir = model_candidates.iter().find(|path| path.exists()).cloned();

        let Some(model_dir) = model_dir else {
            eprintln!("기본 PaddleOCR 캐시 경로에 모델이 없어 샘플 테스트를 스킵합니다.");
            return;
        };
        let engine =
            PaddleFfiEngine::new(&model_dir, false, "zh").expect("Paddle FFI 엔진 생성 실패");

        let source_image = std::env::var("BUZHIDAO_FFI_TEST_IMAGE").ok().map_or_else(
            || {
                PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("MANIFEST_DIR 없음"))
                    .join("1.png")
            },
            PathBuf::from,
        );
        if !source_image.exists() {
            eprintln!("테스트 이미지가 없어 스킵합니다: {:?}", source_image);
            return;
        }

        let (detections, debug_detections) = engine
            .run_image_file(&source_image, 0, 0.1, true, "zh")
            .expect("FFI OCR 실행 실패");

        println!("[FFI] detections={:?}", detections);
        println!("[FFI] debug_len={}", debug_detections.len());
        if debug_detections.is_empty() {
            println!("[FFI] debug trace도 비어있음");
        } else {
            debug_detections.iter().for_each(|item| {
                println!(
                    "[FFI] debug: text={}, score={}, accepted={}",
                    item.1, item.2, item.3
                );
            });
        }
        if detections.is_empty() {
            println!("[FFI] 텍스트가 검출되지 않았습니다");
        }
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn 공식_캐시의_다국어_rec_모델로_엔진을_생성할_수_있다() {
        let model_candidates = paddle_ocr_cache_roots();
        let model_dir = model_candidates.iter().find(|path| path.exists()).cloned();
        let Some(model_dir) = model_dir else {
            eprintln!("기본 PaddleOCR 캐시 경로에 모델이 없어 다국어 엔진 테스트를 스킵합니다.");
            return;
        };

        PaddleFfiEngine::new(&model_dir, false, "fr").expect("fr FFI 엔진 생성 실패");
        PaddleFfiEngine::new(&model_dir, false, "ch_tra").expect("ch_tra FFI 엔진 생성 실패");
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn 지정한_이미지들로_ffi_ocr_지연시간을_측정한다() {
        if std::env::var("BUZHIDAO_RUN_FFI_BENCH").unwrap_or_default() != "1" {
            eprintln!("FFI OCR 벤치는 BUZHIDAO_RUN_FFI_BENCH=1일 때만 실행합니다.");
            return;
        }

        let model_candidates = paddle_ocr_cache_roots();
        let model_dir = model_candidates.iter().find(|path| path.exists()).cloned();
        let Some(model_dir) = model_dir else {
            eprintln!("기본 PaddleOCR 캐시 경로에 모델이 없어 FFI 벤치를 스킵합니다.");
            return;
        };

        let images_json = std::env::var("BUZHIDAO_FFI_BENCH_IMAGES_JSON")
            .expect("BUZHIDAO_FFI_BENCH_IMAGES_JSON 없음");
        let image_paths: Vec<String> =
            serde_json::from_str(&images_json).expect("이미지 목록 JSON 파싱 실패");
        let source =
            std::env::var("BUZHIDAO_FFI_BENCH_SOURCE").unwrap_or_else(|_| "ch".to_string());
        let score_thresh = std::env::var("BUZHIDAO_FFI_BENCH_SCORE_THRESH")
            .ok()
            .and_then(|raw| raw.parse::<f32>().ok())
            .unwrap_or(0.1);
        let warmups = std::env::var("BUZHIDAO_FFI_BENCH_WARMUPS")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .unwrap_or(3);
        let iterations = std::env::var("BUZHIDAO_FFI_BENCH_ITERATIONS")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .unwrap_or(10);

        let engine =
            PaddleFfiEngine::new(&model_dir, false, &source).expect("Paddle FFI 엔진 생성 실패");
        engine.warmup().expect("Paddle FFI warmup 실패");

        for image_path in image_paths {
            let path = PathBuf::from(&image_path);
            if !path.exists() {
                panic!("벤치 이미지가 없습니다: {}", image_path);
            }

            for _ in 0..warmups {
                engine
                    .run_image_file(&path, 0, score_thresh, false, &source)
                    .expect("FFI warmup 실행 실패");
            }

            let mut elapsed_ms = Vec::with_capacity(iterations);
            let mut detection_count = 0usize;
            for _ in 0..iterations {
                let started = std::time::Instant::now();
                let (detections, _) = engine
                    .run_image_file(&path, 0, score_thresh, false, &source)
                    .expect("FFI 벤치 실행 실패");
                detection_count = detections.len();
                elapsed_ms.push(started.elapsed().as_secs_f64() * 1000.0);
            }

            println!(
                "[FFI_BENCH] {}",
                serde_json::json!({
                    "image": image_path,
                    "source": source,
                    "score_thresh": score_thresh,
                    "warmups": warmups,
                    "iterations": iterations,
                    "detection_count": detection_count,
                    "elapsed_ms": elapsed_ms,
                })
            );
        }
    }
}
