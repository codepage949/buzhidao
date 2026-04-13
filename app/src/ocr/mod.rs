mod python_sidecar;

use crate::config::{Config, OCR_SERVER_RESIZE_WIDTH};
use crate::services::{OcrDebugDetection, OcrDetection};
use image::DynamicImage;
use python_sidecar::PythonSidecarEngine;

pub(crate) struct OcrBackend(PythonSidecarEngine);

impl OcrBackend {
    pub(crate) fn new(cfg: &Config) -> Result<Self, String> {
        Ok(Self(PythonSidecarEngine::new(cfg)?))
    }

    pub(crate) fn resize_width_before_ocr(&self) -> u32 {
        OCR_SERVER_RESIZE_WIDTH
    }

    pub(crate) fn run_image(
        &self,
        img: &DynamicImage,
        source: &str,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        self.0.run_image(img, source, score_thresh, debug_trace)
    }
}
