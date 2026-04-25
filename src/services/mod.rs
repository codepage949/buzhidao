pub(crate) mod ai;
pub(crate) mod capture;
pub(crate) mod ocr_pipeline;

pub(crate) use ai::call_ai;
pub(crate) use capture::{capture_screen, CaptureInfo};
pub(crate) use ocr_pipeline::{
    crop_capture_to_region, offset_ocr_result, run_ocr, OcrBoundsPayload, OcrDebugDetection,
    OcrDetection, OcrResultPayload,
};
