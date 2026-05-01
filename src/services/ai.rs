use crate::config::Config;
use serde::{Deserialize, Serialize};
use std::time::{Duration, Instant};

const AI_GATEWAY_URL: &str = "https://ai-gateway.vercel.sh/v1/chat/completions";
const AI_CONNECT_TIMEOUT: Duration = Duration::from_secs(10);
const AI_REQUEST_TIMEOUT: Duration = Duration::from_secs(45);
const AI_POOL_IDLE_TIMEOUT: Duration = Duration::from_secs(10);
const AI_ERROR_BODY_LIMIT: usize = 500;

#[derive(Serialize)]
struct ChatRequest<'a> {
    model: &'a str,
    messages: Vec<ChatMessage<'a>>,
    temperature: f32,
}

#[derive(Serialize)]
struct ChatMessage<'a> {
    role: &'a str,
    content: &'a str,
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Vec<ChatChoice>,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: ChatMessageContent,
}

#[derive(Deserialize)]
struct ChatMessageContent {
    content: Option<String>,
}

pub(crate) fn create_ai_client() -> reqwest::Client {
    reqwest::Client::builder()
        .connect_timeout(AI_CONNECT_TIMEOUT)
        .timeout(AI_REQUEST_TIMEOUT)
        .pool_idle_timeout(AI_POOL_IDLE_TIMEOUT)
        .build()
        .expect("AI HTTP client 생성 실패")
}

pub(crate) async fn call_ai(
    client: &reqwest::Client,
    cfg: &Config,
    text: &str,
) -> Result<String, String> {
    let body = ChatRequest {
        model: &cfg.ai_gateway_model,
        messages: vec![
            ChatMessage {
                role: "system",
                content: &cfg.system_prompt,
            },
            ChatMessage {
                role: "user",
                content: text,
            },
        ],
        temperature: 0.7,
    };

    let started = Instant::now();
    let resp = client
        .post(AI_GATEWAY_URL)
        .bearer_auth(&cfg.ai_gateway_api_key)
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("AI API 요청 실패: {e}"))?;

    let status = resp.status();
    if !status.is_success() {
        let body = resp.text().await.unwrap_or_default();
        return Err(format_ai_http_error(status.as_u16(), &body));
    }

    let chat: ChatResponse = resp
        .json()
        .await
        .map_err(|e| format!("AI 응답 파싱 실패: {e}"))?;

    let result = extract_chat_content(chat);
    eprintln!(
        "[AI] translation request_ms={}",
        started.elapsed().as_millis()
    );
    result
}

fn extract_chat_content(chat: ChatResponse) -> Result<String, String> {
    chat.choices
        .into_iter()
        .next()
        .and_then(|c| c.message.content)
        .filter(|content| !content.trim().is_empty())
        .ok_or_else(|| "AI 응답이 비어 있음".to_string())
}

fn format_ai_http_error(status: u16, body: &str) -> String {
    let compact = body.split_whitespace().collect::<Vec<_>>().join(" ");
    let excerpt: String = compact.chars().take(AI_ERROR_BODY_LIMIT).collect();
    if excerpt.is_empty() {
        format!("AI API 오류: HTTP {status}")
    } else {
        format!("AI API 오류: HTTP {status}: {excerpt}")
    }
}

#[cfg(test)]
mod tests {
    use super::{
        extract_chat_content, format_ai_http_error, ChatChoice, ChatMessageContent, ChatResponse,
        AI_ERROR_BODY_LIMIT,
    };

    #[test]
    fn ai_http_오류는_상태코드와_본문을_함께_보여준다() {
        let err = format_ai_http_error(429, "rate\nlimit exceeded");

        assert_eq!(err, "AI API 오류: HTTP 429: rate limit exceeded");
    }

    #[test]
    fn ai_http_오류_본문은_길이를_제한한다() {
        let body = "x".repeat(AI_ERROR_BODY_LIMIT + 20);
        let err = format_ai_http_error(500, &body);

        assert!(err.starts_with("AI API 오류: HTTP 500: "));
        assert_eq!(
            err.trim_start_matches("AI API 오류: HTTP 500: ")
                .chars()
                .count(),
            AI_ERROR_BODY_LIMIT
        );
    }

    #[test]
    fn ai_응답_본문이_비어_있으면_오류로_처리한다() {
        let chat = ChatResponse {
            choices: vec![ChatChoice {
                message: ChatMessageContent {
                    content: Some("   ".to_string()),
                },
            }],
        };

        let err = extract_chat_content(chat).expect_err("빈 응답은 실패해야 한다");

        assert_eq!(err, "AI 응답이 비어 있음");
    }
}
