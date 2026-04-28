const DEFAULT_SYSTEM_PROMPT: &str = "다음을 한국어로 번역하세요.";
const DEFAULT_SOURCE: &str = "en";
const DEFAULT_SCORE_THRESH: f32 = 0.0;
const DEFAULT_OCR_DEBUG_TRACE: bool = false;
const DEFAULT_WORD_GAP: i32 = 20;
const DEFAULT_LINE_GAP: i32 = 15;

/// 플랫폼별 캡처 단축키 기본값 (Tauri Accelerator 문자열).
/// 수식키 없는 PrtSc는 OS API가 전역 등록을 거부하므로 조합키를 기본으로 한다.
pub(crate) fn default_capture_shortcut() -> &'static str {
    if cfg!(target_os = "macos") {
        "Cmd+Shift+A"
    } else {
        "Ctrl+Alt+A"
    }
}

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
    pub(crate) capture_shortcut: String,
}

impl Config {
    pub(crate) fn from_env_file(
        path: &std::path::Path,
        prompt_path: &std::path::Path,
    ) -> Result<Self, String> {
        dotenvy::from_path_override(path)
            .map_err(|e| format!("환경 파일 로드 실패 ({}): {e}", path.display()))?;
        Self::from_loaded_env(prompt_path)
    }

    fn from_loaded_env(prompt_path: &std::path::Path) -> Result<Self, String> {
        Ok(Self {
            source: env_or("SOURCE", DEFAULT_SOURCE),
            score_thresh: env_or("SCORE_THRESH", "0.0")
                .parse()
                .unwrap_or(DEFAULT_SCORE_THRESH),
            ocr_debug_trace: env_or("OCR_DEBUG_TRACE", "false")
                .parse()
                .unwrap_or(DEFAULT_OCR_DEBUG_TRACE),
            ocr_server_device: parse_ocr_server_device(optional_env(
                crate::env_keys::OCR_SERVER_DEVICE,
            ))?,
            ai_gateway_api_key: optional_env("AI_GATEWAY_API_KEY").unwrap_or_default(),
            ai_gateway_model: optional_env("AI_GATEWAY_MODEL").unwrap_or_default(),
            system_prompt: load_system_prompt(prompt_path)?,
            word_gap: env_or("WORD_GAP", "20").parse().unwrap_or(DEFAULT_WORD_GAP),
            line_gap: env_or("LINE_GAP", "15").parse().unwrap_or(DEFAULT_LINE_GAP),
            capture_shortcut: optional_env(crate::env_keys::CAPTURE_SHORTCUT)
                .unwrap_or_else(|| default_capture_shortcut().to_string()),
        })
    }
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

pub(crate) fn materialize_prompt_file(path: &std::path::Path) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("프롬프트 디렉토리 생성 실패: {e}"))?;
    }
    if path.exists() {
        return Ok(());
    }
    std::fs::write(path, DEFAULT_SYSTEM_PROMPT)
        .map_err(|e| format!("프롬프트 파일 생성 실패 ({}): {e}", path.display()))
}

fn load_system_prompt(path: &std::path::Path) -> Result<String, String> {
    if !path.exists() {
        materialize_prompt_file(path)?;
    }
    std::fs::read_to_string(path)
        .map(|text| text.trim().to_string())
        .map_err(|e| format!("시스템 프롬프트 로드 실패 ({}): {e}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::{
        default_capture_shortcut, load_system_prompt, materialize_prompt_file, optional_env,
        parse_ocr_server_device, Config, DEFAULT_SCORE_THRESH, DEFAULT_SOURCE,
        DEFAULT_SYSTEM_PROMPT,
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
        let path = temp_prompt_path();
        let prompt = load_system_prompt(&path).expect("기본 프롬프트를 불러와야 한다");

        assert_eq!(prompt, DEFAULT_SYSTEM_PROMPT);
        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn 환경변수가_있으면_파일에서_시스템_프롬프트를_읽는다() {
        let _guard = env_lock().lock().unwrap();
        let path = temp_prompt_path();
        std::fs::write(&path, "커스텀 프롬프트").expect("테스트 프롬프트 파일 작성 실패");
        let prompt = load_system_prompt(&path).expect("프롬프트 파일을 읽어야 한다");

        assert_eq!(prompt, "커스텀 프롬프트");

        let _ = std::fs::remove_file(path);
    }

    #[test]
    fn prompt_파일이_없으면_기본값으로_생성한다() {
        let _guard = env_lock().lock().unwrap();
        let path = temp_prompt_path();
        materialize_prompt_file(&path).expect(".prompt 생성 실패");
        let prompt = std::fs::read_to_string(&path).expect(".prompt 읽기 실패");
        assert_eq!(prompt, DEFAULT_SYSTEM_PROMPT);
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
        assert!(err.contains(crate::env_keys::OCR_SERVER_DEVICE));
    }

    #[test]
    fn from_env_file은_누락값에_기본값을_적용한다() {
        let _guard = env_lock().lock().unwrap();
        std::env::remove_var("SYSTEM_PROMPT");
        std::env::remove_var("SYSTEM_PROMPT_PATH");
        let dir = std::env::temp_dir().join(format!(
            "buzhidao-config-env-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("시계가 UNIX_EPOCH 이전입니다")
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).expect("임시 디렉토리 생성 실패");
        let env_path = dir.join(".env");
        let prompt_path = dir.join(".prompt");
        std::fs::write(&env_path, "").expect(".env 작성 실패");

        let cfg = Config::from_env_file(&env_path, &prompt_path).expect("환경 파일 로드 실패");

        assert_eq!(cfg.source, DEFAULT_SOURCE);
        assert!((cfg.score_thresh - DEFAULT_SCORE_THRESH).abs() < f32::EPSILON);
        assert_eq!(cfg.ocr_server_device, "cpu");
        assert_eq!(cfg.ai_gateway_api_key, "");
        assert_eq!(cfg.ai_gateway_model, "");
        assert_eq!(cfg.system_prompt, DEFAULT_SYSTEM_PROMPT);
        assert_eq!(cfg.capture_shortcut, default_capture_shortcut());

        let _ = std::fs::remove_file(&env_path);
        let _ = std::fs::remove_file(&prompt_path);
        let _ = std::fs::remove_dir(&dir);
    }

    #[test]
    fn capture_shortcut_기본값은_플랫폼별로_다르다() {
        let shortcut = default_capture_shortcut();
        if cfg!(target_os = "macos") {
            assert_eq!(shortcut, "Cmd+Shift+A");
        } else {
            assert_eq!(shortcut, "Ctrl+Alt+A");
        }
    }

    #[test]
    fn from_env_file은_prompt_파일을_읽는다() {
        let _guard = env_lock().lock().unwrap();
        let dir = std::env::temp_dir().join(format!(
            "buzhidao-config-env-quoted-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("시계가 UNIX_EPOCH 이전입니다")
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).expect("임시 디렉토리 생성 실패");
        let env_path = dir.join(".env");
        let prompt_path = dir.join(".prompt");
        std::fs::write(&env_path, "").expect(".env 작성 실패");
        std::fs::write(&prompt_path, "다음을 한국어로 번역하세요.").expect(".prompt 작성 실패");

        let cfg = Config::from_env_file(&env_path, &prompt_path).expect("환경 파일 로드 실패");

        assert_eq!(cfg.system_prompt, "다음을 한국어로 번역하세요.");

        let _ = std::fs::remove_file(&env_path);
        let _ = std::fs::remove_file(&prompt_path);
        let _ = std::fs::remove_dir(&dir);
    }
}
