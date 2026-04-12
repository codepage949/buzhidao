const DEFAULT_SYSTEM_PROMPT: &str = "다음을 한국어로 번역하세요.";
const DEFAULT_SCORE_THRESH: f32 = 0.5;
pub(crate) const OCR_DET_RESIZE_LONG: u32 = 1024;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum OcrBackendKind {
    Onnx,
    PythonSidecar,
    #[cfg(feature = "paddle-ffi")]
    PaddleFfi,
}

#[derive(Clone)]
pub(crate) struct Config {
    pub(crate) source: String,
    pub(crate) score_thresh: f32,
    pub(crate) ocr_debug_trace: bool,
    pub(crate) ai_gateway_api_key: String,
    pub(crate) ai_gateway_model: String,
    pub(crate) system_prompt: String,
    pub(crate) word_gap: i32,
    pub(crate) line_gap: i32,
    /// det 히트맵 이진화 임계값 (낮을수록 더 많은 텍스트 픽셀 포함)
    pub(crate) det_thresh: f32,
    /// det 박스 채택 임계값 (낮을수록 더 많은 박스 채택)
    pub(crate) box_thresh: f32,
    pub(crate) ocr_backend: OcrBackendKind,
}

impl Config {
    pub(crate) fn from_env() -> Result<Self, String> {
        let _ = dotenvy::dotenv();
        Ok(Self {
            source: env_or("SOURCE", "en"),
            score_thresh: env_or("SCORE_THRESH", "0.5")
                .parse()
                .unwrap_or(DEFAULT_SCORE_THRESH),
            ocr_debug_trace: env_or("OCR_DEBUG_TRACE", "false")
                .parse()
                .unwrap_or(false),
            ai_gateway_api_key: require_env("AI_GATEWAY_API_KEY")?,
            ai_gateway_model: require_env("AI_GATEWAY_MODEL")?,
            system_prompt: load_system_prompt()?,
            word_gap: env_or("WORD_GAP", "20").parse().unwrap_or(20),
            line_gap: env_or("LINE_GAP", "15").parse().unwrap_or(15),
            det_thresh: env_or("DET_THRESH", "0.2").parse().unwrap_or(0.2),
            box_thresh: env_or("BOX_THRESH", "0.4").parse().unwrap_or(0.4),
            ocr_backend: parse_ocr_backend(&env_or("OCR_BACKEND", "onnx"))?,
        })
    }
}

fn parse_ocr_backend(value: &str) -> Result<OcrBackendKind, String> {
    match value.trim().to_ascii_lowercase().as_str() {
        "onnx" => Ok(OcrBackendKind::Onnx),
        "python_sidecar" | "python-sidecar" | "python" => Ok(OcrBackendKind::PythonSidecar),
        #[cfg(feature = "paddle-ffi")]
        "paddle_ffi" | "paddle-ffi" => Ok(OcrBackendKind::PaddleFfi),
        #[cfg(not(feature = "paddle-ffi"))]
        "paddle_ffi" | "paddle-ffi" => {
            Err("OCR_BACKEND=paddle_ffi 는 paddle-ffi feature 빌드에서만 사용할 수 있습니다".to_string())
        }
        other => Err(format!("지원하지 않는 OCR_BACKEND 값: {other}")),
    }
}

fn require_env(name: &str) -> Result<String, String> {
    std::env::var(name).map_err(|_| format!("환경변수 누락: {name}"))
}

fn env_or(name: &str, default: &str) -> String {
    std::env::var(name).unwrap_or_else(|_| default.to_string())
}

fn load_system_prompt() -> Result<String, String> {
    match std::env::var("SYSTEM_PROMPT_PATH") {
        Ok(path) => std::fs::read_to_string(&path)
            .map_err(|e| format!("시스템 프롬프트 로드 실패 ({path}): {e}")),
        Err(std::env::VarError::NotPresent) => Ok(DEFAULT_SYSTEM_PROMPT.to_string()),
        Err(std::env::VarError::NotUnicode(_)) => {
            Err("환경변수 값이 유효한 UTF-8이 아닙니다: SYSTEM_PROMPT_PATH".to_string())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::{
        env_or, load_system_prompt, parse_ocr_backend, DEFAULT_SCORE_THRESH, DEFAULT_SYSTEM_PROMPT,
        OCR_DET_RESIZE_LONG, OcrBackendKind,
    };
    use std::path::PathBuf;
    use std::sync::{Mutex, OnceLock};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn env_lock() -> &'static Mutex<()> {
        static LOCK: OnceLock<Mutex<()>> = OnceLock::new();
        LOCK.get_or_init(|| Mutex::new(()))
    }

    fn temp_prompt_path() -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("시계가 UNIX_EPOCH 이전입니다")
            .as_nanos();
        std::env::temp_dir().join(format!("buzhidao-system-prompt-{nanos}.txt"))
    }

    #[test]
    fn 환경변수가_없으면_기본_프롬프트를_사용한다() {
        let _guard = env_lock().lock().unwrap();
        std::env::remove_var("SYSTEM_PROMPT_PATH");

        let prompt = load_system_prompt().expect("기본 프롬프트를 불러와야 한다");

        assert_eq!(prompt, DEFAULT_SYSTEM_PROMPT);
    }

    #[test]
    fn 환경변수가_있으면_파일에서_시스템_프롬프트를_읽는다() {
        let _guard = env_lock().lock().unwrap();
        let path = temp_prompt_path();
        std::fs::write(&path, "커스텀 프롬프트").expect("테스트 프롬프트 파일 작성 실패");
        std::env::set_var("SYSTEM_PROMPT_PATH", &path);

        let prompt = load_system_prompt().expect("프롬프트 파일을 읽어야 한다");

        assert_eq!(prompt, "커스텀 프롬프트");

        std::env::remove_var("SYSTEM_PROMPT_PATH");
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn score_thresh_기본값은_0_5다() {
        assert!((DEFAULT_SCORE_THRESH - 0.5).abs() < f32::EPSILON);
    }

    #[test]
    fn ocr_debug_trace_기본값은_false다() {
        let _guard = env_lock().lock().unwrap();
        std::env::remove_var("OCR_DEBUG_TRACE");

        let value = env_or("OCR_DEBUG_TRACE", "false")
            .parse::<bool>()
            .expect("bool 파싱이 되어야 한다");

        assert!(!value);
    }

    #[test]
    fn ocr_det_resize_long_기본값은_1024다() {
        assert_eq!(OCR_DET_RESIZE_LONG, 1024);
    }

    #[test]
    fn ocr_backend_기본값은_onnx다() {
        assert_eq!(parse_ocr_backend("onnx").unwrap(), OcrBackendKind::Onnx);
    }
}
