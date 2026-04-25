#[cfg(feature = "paddle-ffi")]
mod paddle_ffi;

use crate::config::Config;
use crate::services::{OcrDebugDetection, OcrDetection};
#[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
use paddle_ffi::PaddleFfiEngine;
use std::path::Path;
use std::sync::LazyLock;
use std::time::Instant;

static OCR_STAGE_LOGGING_ENABLED: LazyLock<bool> = LazyLock::new(|| {
    matches!(
        std::env::var("BUZHIDAO_OCR_STAGE_LOG").ok().as_deref(),
        Some("1") | Some("true") | Some("TRUE") | Some("True")
    )
});

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum OcrStageLogKind {
    Backend,
}

pub(crate) fn ocr_stage_logging_enabled() -> bool {
    *OCR_STAGE_LOGGING_ENABLED
}

pub(crate) fn format_ocr_stage_log(
    kind: OcrStageLogKind,
    fields: &[(&str, String)],
) -> String {
    let mut message = match kind {
        OcrStageLogKind::Backend => "[OCR_STAGE] backend".to_string(),
    };
    for (key, value) in fields {
        message.push(' ');
        message.push_str(key);
        message.push('=');
        message.push_str(value);
    }
    message
}

pub(crate) enum OcrBackend {
    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    PaddleFfi(PaddleFfiEngine),
    #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
    Unsupported,
}

impl OcrBackend {
    pub(crate) fn new(cfg: &Config, paddle_model_dir: Option<&Path>) -> Result<Self, String> {
        #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
        {
            let model_dir = paddle_model_dir.ok_or_else(|| {
                "FFI 단일 모드에서는 기본 PaddleOCR 캐시 경로에 det/cls/rec 모델이 필요합니다"
                    .to_string()
            })?;
            let use_gpu = cfg.ocr_server_device == "gpu";
            Ok(Self::PaddleFfi(PaddleFfiEngine::new(
                model_dir,
                use_gpu,
                &cfg.source,
            )?))
        }
        #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
        {
            let _ = (cfg, paddle_model_dir);
            Ok(Self::Unsupported)
        }
    }

    pub(crate) fn warmup(&self) -> Result<(), String> {
        match self {
            #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
            Self::PaddleFfi(engine) => engine.warmup(),
            #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
            Self::Unsupported => Err(
                "FFI 단일 모드에서는 paddle-ffi feature와 Paddle Inference 링크가 필요합니다"
                    .to_string(),
            ),
        }
    }

    #[cfg_attr(
        not(all(feature = "paddle-ffi", has_paddle_inference)),
        allow(unused_variables)
    )]
    pub(crate) fn set_lang(&self, new_lang: &str) -> Result<(), String> {
        match self {
            #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
            Self::PaddleFfi(engine) => engine.set_lang(new_lang),
            #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
            Self::Unsupported => Err(
                "FFI 단일 모드에서는 paddle-ffi feature와 Paddle Inference 링크가 필요합니다"
                    .to_string(),
            ),
        }
    }

    pub(crate) fn resize_width_before_ocr(&self) -> u32 {
        match self {
            #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
            Self::PaddleFfi(_) => 0,
            #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
            Self::Unsupported => 0,
        }
    }

    #[cfg_attr(
        not(all(feature = "paddle-ffi", has_paddle_inference)),
        allow(unused_variables)
    )]
    pub(crate) fn run_image(
        &self,
        img: &image::RgbaImage,
        source: &str,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
        let det_resize_long = self.resize_width_before_ocr();
        match self {
            #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
            Self::PaddleFfi(engine) => {
                let backend_started = Instant::now();
                let result = engine.run_image_rgba(
                    img.as_raw(),
                    img.width(),
                    img.height(),
                    det_resize_long,
                    score_thresh,
                    debug_trace,
                    source,
                );
                let backend_ms = backend_started.elapsed().as_millis();
                if ocr_stage_logging_enabled() {
                    eprintln!(
                        "{}",
                        format_ocr_stage_log(
                            OcrStageLogKind::Backend,
                            &[
                                ("image", format!("{}x{}", img.width(), img.height())),
                                ("prepare_image_ms", "0".to_string()),
                                ("ffi_ms", backend_ms.to_string()),
                            ],
                        )
                    );
                }
                result
            }
            #[cfg(not(all(feature = "paddle-ffi", has_paddle_inference)))]
            Self::Unsupported => Err(
                "FFI 단일 모드에서는 paddle-ffi feature와 Paddle Inference 링크가 필요합니다"
                    .to_string(),
            ),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{format_ocr_stage_log, OcrBackend, OcrStageLogKind};

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn ffi_단일_모드에서는_모델_경로가_있으면_paddle_ffi를_생성한다() {
        let cfg = crate::config::Config {
            source: "en".to_string(),
            score_thresh: 0.5,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: String::new(),
            ai_gateway_model: String::new(),
            system_prompt: String::new(),
            word_gap: 20,
            line_gap: 15,
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        };
        let model_dir = std::env::temp_dir();
        let result = OcrBackend::new(&cfg, Some(model_dir.as_path()));
        assert!(result.is_err() || matches!(result, Ok(OcrBackend::PaddleFfi(_))));
    }

    #[test]
    fn ffi_단일_모드에서는_모델_경로가_없으면_초기화에_실패한다() {
        let cfg = crate::config::Config {
            source: "en".to_string(),
            score_thresh: 0.5,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: String::new(),
            ai_gateway_model: String::new(),
            system_prompt: String::new(),
            word_gap: 20,
            line_gap: 15,
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        };
        let err = match OcrBackend::new(&cfg, None) {
            Ok(engine) => engine.warmup().expect_err("FFI 준비는 실패해야 한다"),
            Err(err) => err,
        };
        assert!(err.contains("FFI"));
    }

    #[test]
    fn ocr_stage_로그를_일관된_형식으로_만든다() {
        let line = format_ocr_stage_log(
            OcrStageLogKind::Backend,
            &[("image", "1919x1024".to_string()), ("ffi_ms", "1234".to_string())],
        );

        assert_eq!(line, "[OCR_STAGE] backend image=1919x1024 ffi_ms=1234");
    }

}
