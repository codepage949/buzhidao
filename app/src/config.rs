const DEFAULT_SYSTEM_PROMPT: &str = "다음을 한국어로 번역하세요.";

#[derive(Clone)]
pub(crate) struct Config {
    pub(crate) source: String,
    pub(crate) score_thresh: f32,
    pub(crate) ai_gateway_api_key: String,
    pub(crate) ai_gateway_model: String,
    pub(crate) system_prompt: String,
    pub(crate) x_delta: i32,
    pub(crate) y_delta: i32,
}

impl Config {
    pub(crate) fn from_env() -> Result<Self, String> {
        let _ = dotenvy::dotenv();
        Ok(Self {
            source: env_or("SOURCE", "en"),
            score_thresh: env_or("SCORE_THRESH", "0.8")
                .parse()
                .unwrap_or(0.8),
            ai_gateway_api_key: require_env("AI_GATEWAY_API_KEY")?,
            ai_gateway_model: require_env("AI_GATEWAY_MODEL")?,
            system_prompt: load_system_prompt()?,
            x_delta: env_or("X_DELTA", "25").parse().unwrap_or(25),
            y_delta: env_or("Y_DELTA", "225").parse().unwrap_or(225),
        })
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
    use super::{load_system_prompt, DEFAULT_SYSTEM_PROMPT};
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
}
