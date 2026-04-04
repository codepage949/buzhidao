use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use image::ImageFormat;
use reqwest::multipart;
use serde::{Deserialize, Serialize};
use std::{
    io::Cursor,
    sync::{
        atomic::{AtomicBool, Ordering},
        Arc,
    },
};
use tauri::{AppHandle, Emitter, Manager};
use tauri_plugin_global_shortcut::{Code, GlobalShortcutExt, Shortcut, ShortcutState};

// ── 설정 ──────────────────────────────────────────────────────────────────────

#[derive(Clone)]
struct Config {
    source: String,
    api_base_url: String,
    ai_gateway_api_key: String,
    ai_gateway_model: String,
    system_prompt: String,
    x_delta: i32,
    y_delta: i32,
}

impl Config {
    fn from_env() -> Result<Self, String> {
        let _ = dotenvy::dotenv();
        Ok(Config {
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

// ── 이벤트 페이로드 타입 ──────────────────────────────────────────────────────

type OcrDetection = (Vec<[f64; 2]>, String);

#[derive(Serialize, Clone)]
struct OcrResultPayload {
    detections: Vec<OcrDetection>,
    scale: f64,
    orig_width: u32,
    orig_height: u32,
    source: String,
    x_delta: i32,
    y_delta: i32,
}

// ── 캡처 (동기, spawn_blocking용) ────────────────────────────────────────────

struct CaptureInfo {
    image: image::DynamicImage,
    orig_width: u32,
    orig_height: u32,
}

fn capture_screen() -> Result<CaptureInfo, String> {
    let screens = screenshots::Screen::all().map_err(|e| e.to_string())?;
    let screen = screens.first().ok_or("디스플레이를 찾을 수 없음")?;
    let capture = screen.capture().map_err(|e| e.to_string())?;

    let orig_width = capture.width();
    let orig_height = capture.height();

    let raw_bytes: Vec<u8> = capture.to_vec();
    let rgba_image = image::RgbaImage::from_raw(orig_width, orig_height, raw_bytes)
        .ok_or("이미지 버퍼 변환 실패")?;

    Ok(CaptureInfo {
        image: image::DynamicImage::ImageRgba8(rgba_image),
        orig_width,
        orig_height,
    })
}

// ── OCR (비동기) ──────────────────────────────────────────────────────────────

async fn run_ocr(
    cfg: &Config,
    dyn_img: image::DynamicImage,
    orig_width: u32,
    orig_height: u32,
) -> Result<OcrResultPayload, String> {
    let (ocr_png, scale) = if orig_width > 1024 {
        let ratio = 1024.0 / orig_width as f64;
        let new_h = (orig_height as f64 * ratio) as u32;
        let resized = dyn_img.resize_exact(1024, new_h, image::imageops::FilterType::Lanczos3);
        let mut buf: Vec<u8> = Vec::new();
        resized
            .write_to(&mut Cursor::new(&mut buf), ImageFormat::Png)
            .map_err(|e| e.to_string())?;
        (buf, orig_width as f64 / 1024.0)
    } else {
        let mut buf: Vec<u8> = Vec::new();
        dyn_img
            .write_to(&mut Cursor::new(&mut buf), ImageFormat::Png)
            .map_err(|e| e.to_string())?;
        (buf, 1.0)
    };

    let client = reqwest::Client::new();
    let part = multipart::Part::bytes(ocr_png)
        .file_name("capture.png")
        .mime_str("image/png")
        .map_err(|e| e.to_string())?;
    let form = multipart::Form::new().part("file", part);

    let url = format!("{}/infer/{}", cfg.api_base_url, cfg.source);
    let resp = client
        .post(&url)
        .multipart(form)
        .send()
        .await
        .map_err(|e| format!("OCR 서버 요청 실패: {e}"))?;

    let detections: Vec<OcrDetection> = resp
        .json()
        .await
        .map_err(|e| format!("OCR 응답 파싱 실패: {e}"))?;

    Ok(OcrResultPayload {
        detections,
        scale,
        orig_width,
        orig_height,
        source: cfg.source.clone(),
        x_delta: cfg.x_delta,
        y_delta: cfg.y_delta,
    })
}

// ── AI 번역 ──────────────────────────────────────────────────────────────────

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

async fn call_ai(cfg: &Config, text: &str) -> Result<String, String> {
    let client = reqwest::Client::new();
    let body = ChatRequest {
        model: &cfg.ai_gateway_model,
        messages: vec![
            ChatMessage { role: "system", content: &cfg.system_prompt },
            ChatMessage { role: "user", content: text },
        ],
        temperature: 0.7,
    };

    let resp = client
        .post("https://ai-gateway.vercel.sh/v1/chat/completions")
        .bearer_auth(&cfg.ai_gateway_api_key)
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("AI API 요청 실패: {e}"))?;

    let chat: ChatResponse = resp
        .json()
        .await
        .map_err(|e| format!("AI 응답 파싱 실패: {e}"))?;

    chat.choices
        .into_iter()
        .next()
        .and_then(|c| c.message.content)
        .ok_or_else(|| "AI 응답이 비어 있음".to_string())
}

// ── Tauri 커맨드 ─────────────────────────────────────────────────────────────

/// 오버레이에서 텍스트 영역 선택 시 호출:
/// Rust 측에서 오버레이를 먼저 숨기고 번역을 실행한다.
/// JS에서 hide() 후 invoke()를 하면 WebView2가 서스펜드되어 invoke가 실행되지 않음.
#[tauri::command]
async fn select_text(app: AppHandle, text: String) -> Result<(), String> {
    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.hide();
    }

    let cfg = app.state::<Config>().inner().clone();
    let main_win = app
        .get_webview_window("main")
        .ok_or("메인 창을 찾을 수 없음")?;
    main_win.emit("translating", ()).map_err(|e| e.to_string())?;

    match call_ai(&cfg, &text).await {
        Ok(result) => {
            main_win
                .emit("translation_result", &result)
                .map_err(|e| e.to_string())?;
        }
        Err(e) => {
            main_win
                .emit("translation_error", &e)
                .map_err(|e2| e2.to_string())?;
        }
    }

    Ok(())
}

// ── PrtSc 처리 ────────────────────────────────────────────────────────────────


async fn handle_prtsc(app: AppHandle, busy: Arc<AtomicBool>) {
    if busy.swap(true, Ordering::SeqCst) {
        return;
    }

    let cfg = app.state::<Config>().inner().clone();

    // 1. 스크린샷 캡처 (블로킹 작업 → spawn_blocking)
    let capture_result = tauri::async_runtime::spawn_blocking(capture_screen).await;
    let info = match capture_result {
        Ok(Ok(v)) => v,
        Ok(Err(e)) => {
            eprintln!("캡처 오류: {e}");
            busy.store(false, Ordering::SeqCst);
            return;
        }
        Err(e) => {
            eprintln!("캡처 스레드 오류: {e}");
            busy.store(false, Ordering::SeqCst);
            return;
        }
    };

    let (orig_width, orig_height) = (info.orig_width, info.orig_height);

    // 2. 오버레이 즉시 표시 (로딩 상태)
    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.emit("overlay_show", ());
        let _ = overlay.set_ignore_cursor_events(false);
        let _ = overlay.set_fullscreen(true);
        let _ = overlay.show();
        let _ = overlay.set_focus();
    }

    // 3. OCR 실행
    match run_ocr(&cfg, info.image, orig_width, orig_height).await {
        Ok(ocr) => {
            if let Some(overlay) = app.get_webview_window("overlay") {
                let _ = overlay.emit("ocr_result", &ocr);
            }
        }
        Err(e) => {
            eprintln!("OCR 오류: {e}");
            if let Some(overlay) = app.get_webview_window("overlay") {
                let _ = overlay.emit("ocr_error", &e);
            }
        }
    }

    busy.store(false, Ordering::SeqCst);
}

// ── 앱 진입점 ─────────────────────────────────────────────────────────────────

pub fn run() {
    let config = Config::from_env().expect("설정 로드 실패");
    let busy = Arc::new(AtomicBool::new(false));

    tauri::Builder::default()
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .manage(config)
        .setup(move |app| {
            let handle = app.handle().clone();
            let busy_clone = busy.clone();

            let shortcut = Shortcut::new(None, Code::PrintScreen);
            app.handle()
                .global_shortcut()
                .on_shortcut(shortcut, move |_app, _shortcut, event| {
                    if event.state() == ShortcutState::Pressed {
                        let h = handle.clone();
                        let b = busy_clone.clone();
                        tauri::async_runtime::spawn(async move {
                            handle_prtsc(h, b).await;
                        });
                    }
                })?;

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![select_text])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}
