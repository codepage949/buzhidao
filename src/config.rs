#[derive(Clone)]
pub(crate) struct Config {
    pub(crate) source: String,
    pub(crate) api_base_url: String,
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
            api_base_url: env_or("API_BASE_URL", "http://127.0.0.1:8000"),
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
    let path = env_or("SYSTEM_PROMPT_PATH", ".system_prompt.txt");
    std::fs::read_to_string(&path).map_err(|e| format!("시스템 프롬프트 로드 실패 ({path}): {e}"))
}
