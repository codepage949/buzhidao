const DEFAULT_SYSTEM_PROMPT: &str = "다음을 한국어로 번역하세요.";
const DEFAULT_SOURCE: &str = "en";
const DEFAULT_SCORE_THRESH: f32 = 0.8;
const DEFAULT_OCR_DEBUG_TRACE: bool = false;
const DEFAULT_OCR_SERVER_DEVICE: &str = "cpu";
const DEFAULT_AI_GATEWAY_MODEL: &str = "alibaba/qwen-3-32b";
const DEFAULT_WORD_GAP: i32 = 20;
const DEFAULT_LINE_GAP: i32 = 15;
const DEFAULT_OCR_SERVER_STARTUP_TIMEOUT_SECS: u64 = 30;
const DEFAULT_OCR_SERVER_REQUEST_TIMEOUT_SECS: u64 = 20;
pub(crate) const OCR_SERVER_RESIZE_WIDTH: u32 = 1024;

#[derive(Clone)]
pub(crate) struct Config {
    pub(crate) source: String,
    pub(crate) score_thresh: f32,
    pub(crate) ocr_debug_trace: bool,
    pub(crate) ocr_server_device: String,
    pub(crate) ai_gateway_api_key: String,
    pub(crate) ai_gateway_model: String,
    pub(crate) system_prompt: String,
    pub(crate) word_gap: i32,
    pub(crate) line_gap: i32,
    pub(crate) ocr_server_executable: String,
    pub(crate) ocr_server_startup_timeout_secs: u64,
    pub(crate) ocr_server_request_timeout_secs: u64,
}

impl Config {
    pub(crate) fn from_env() -> Result<Self, String> {
        let _ = dotenvy::dotenv();
        Self::from_loaded_env()
    }

    pub(crate) fn from_env_file(path: &std::path::Path) -> Result<Self, String> {
        dotenvy::from_path_override(path)
            .map_err(|e| format!("환경 파일 로드 실패 ({}): {e}", path.display()))?;
        Self::from_loaded_env()
    }

    fn from_loaded_env() -> Result<Self, String> {
        Ok(Self {
            source: env_or("SOURCE", DEFAULT_SOURCE),
            score_thresh: env_or("SCORE_THRESH", "0.8")
                .parse()
                .unwrap_or(DEFAULT_SCORE_THRESH),
            ocr_debug_trace: env_or("OCR_DEBUG_TRACE", "false")
                .parse()
                .unwrap_or(DEFAULT_OCR_DEBUG_TRACE),
            ocr_server_device: parse_ocr_server_device(optional_env("OCR_SERVER_DEVICE"))?,
            ai_gateway_api_key: require_env("AI_GATEWAY_API_KEY")?,
            ai_gateway_model: require_env("AI_GATEWAY_MODEL")?,
            system_prompt: load_system_prompt()?,
            word_gap: env_or("WORD_GAP", "20").parse().unwrap_or(DEFAULT_WORD_GAP),
            line_gap: env_or("LINE_GAP", "15").parse().unwrap_or(DEFAULT_LINE_GAP),
            ocr_server_executable: optional_env("OCR_SERVER_EXECUTABLE")
                .unwrap_or_else(default_ocr_server_executable),
            ocr_server_startup_timeout_secs: env_or("OCR_SERVER_STARTUP_TIMEOUT_SECS", "30")
                .parse()
                .unwrap_or(DEFAULT_OCR_SERVER_STARTUP_TIMEOUT_SECS),
            ocr_server_request_timeout_secs: env_or("OCR_SERVER_REQUEST_TIMEOUT_SECS", "20")
                .parse()
                .unwrap_or(DEFAULT_OCR_SERVER_REQUEST_TIMEOUT_SECS),
        })
    }

    pub(crate) fn default_runtime() -> Self {
        Self {
            source: DEFAULT_SOURCE.to_string(),
            score_thresh: DEFAULT_SCORE_THRESH,
            ocr_debug_trace: DEFAULT_OCR_DEBUG_TRACE,
            ocr_server_device: DEFAULT_OCR_SERVER_DEVICE.to_string(),
            ai_gateway_api_key: String::new(),
            ai_gateway_model: DEFAULT_AI_GATEWAY_MODEL.to_string(),
            system_prompt: DEFAULT_SYSTEM_PROMPT.to_string(),
            word_gap: DEFAULT_WORD_GAP,
            line_gap: DEFAULT_LINE_GAP,
            ocr_server_executable: default_ocr_server_executable(),
            ocr_server_startup_timeout_secs: DEFAULT_OCR_SERVER_STARTUP_TIMEOUT_SECS,
            ocr_server_request_timeout_secs: DEFAULT_OCR_SERVER_REQUEST_TIMEOUT_SECS,
        }
    }
}

fn default_ocr_server_executable() -> String {
    let file_name = if cfg!(target_os = "windows") {
        "ocr_server.exe"
    } else {
        "ocr_server"
    };
    let path = std::path::PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .join("..")
        .join("ocr_server")
        .join("dist")
        .join("ocr_server")
        .join(file_name);
    path.to_string_lossy().into_owned()
}

fn require_env(name: &str) -> Result<String, String> {
    std::env::var(name).map_err(|_| format!("환경변수 누락: {name}"))
}

fn env_or(name: &str, default: &str) -> String {
    optional_env(name).unwrap_or_else(|| default.to_string())
}

fn optional_env(name: &str) -> Option<String> {
    std::env::var(name)
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

fn parse_ocr_server_device(value: Option<String>) -> Result<String, String> {
    let normalized = value
        .unwrap_or_else(|| "cpu".to_string())
        .trim()
        .to_ascii_lowercase();
    match normalized.as_str() {
        "cpu" | "gpu" => Ok(normalized),
        _ => Err(format!(
            "지원하지 않는 OCR_SERVER_DEVICE 값: {normalized} (허용값: cpu, gpu)"
        )),
    }
}

fn load_system_prompt() -> Result<String, String> {
    if let Some(prompt) = optional_env("SYSTEM_PROMPT") {
        return Ok(prompt.replace("\\n", "\n"));
    }
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
        load_system_prompt, optional_env, parse_ocr_server_device, Config, DEFAULT_AI_GATEWAY_MODEL,
        DEFAULT_SCORE_THRESH, DEFAULT_SOURCE, DEFAULT_SYSTEM_PROMPT,
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
    fn system_prompt_환경변수가_있으면_파일보다_우선한다() {
        let _guard = env_lock().lock().unwrap();
        let path = temp_prompt_path();
        std::fs::write(&path, "파일 프롬프트").expect("테스트 프롬프트 파일 작성 실패");
        std::env::set_var("SYSTEM_PROMPT_PATH", &path);
        std::env::set_var("SYSTEM_PROMPT", "첫 줄\\n둘째 줄");

        let prompt = load_system_prompt().expect("인라인 프롬프트를 읽어야 한다");

        assert_eq!(prompt, "첫 줄\n둘째 줄");

        std::env::remove_var("SYSTEM_PROMPT");
        std::env::remove_var("SYSTEM_PROMPT_PATH");
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn optional_env는_앞뒤_공백을_제거한다() {
        let _guard = env_lock().lock().unwrap();
        std::env::set_var("TEST_OPTIONAL_ENV", "  value  ");
        assert_eq!(optional_env("TEST_OPTIONAL_ENV").as_deref(), Some("value"));
        std::env::remove_var("TEST_OPTIONAL_ENV");
    }

    #[test]
    fn ocr_server_device_기본값은_cpu다() {
        let device = parse_ocr_server_device(None).expect("기본 장치 파싱 실패");
        assert_eq!(device, "cpu");
    }

    #[test]
    fn ocr_server_device는_공백과_대소문자를_정규화한다() {
        let device = parse_ocr_server_device(Some("  GpU ".to_string())).expect("장치 정규화 실패");
        assert_eq!(device, "gpu");
    }

    #[test]
    fn ocr_server_device가_잘못되면_실패한다() {
        let err = parse_ocr_server_device(Some("cuda".to_string()))
            .expect_err("잘못된 장치는 실패해야 한다");
        assert!(err.contains("OCR_SERVER_DEVICE"));
    }

    #[test]
    fn runtime_기본값은_하드코딩된_설정을_사용한다() {
        let cfg = Config::default_runtime();

        assert_eq!(cfg.source, DEFAULT_SOURCE);
        assert!((cfg.score_thresh - DEFAULT_SCORE_THRESH).abs() < f32::EPSILON);
        assert_eq!(cfg.ai_gateway_api_key, "");
        assert_eq!(cfg.ai_gateway_model, DEFAULT_AI_GATEWAY_MODEL);
        assert_eq!(cfg.system_prompt, DEFAULT_SYSTEM_PROMPT);
    }
}
