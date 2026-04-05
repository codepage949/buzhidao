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
use rdev::{grab, Event, EventType, Key};
use tauri::{AppHandle, Emitter, Manager};
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;

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

// ── 팝업 위치 계산 ────────────────────────────────────────────────────────────

/// OCR 박스 논리 좌표 기반으로 팝업 창 위치를 계산한다.
/// 기본: 박스 우측 배치, 화면 벗어나면 좌측으로 전환.
fn calc_popup_pos(
    app: &AppHandle,
    box_x: f64,
    box_y: f64,
    box_w: f64,
    box_h: f64,
) -> (f64, f64) {
    const POPUP_W: f64 = 420.0;
    const POPUP_H: f64 = 500.0;
    const GAP: f64 = 12.0;

    let (screen_w, screen_h) = app
        .primary_monitor()
        .ok()
        .flatten()
        .map(|m| {
            let sf = m.scale_factor();
            let sz = m.size();
            (sz.width as f64 / sf, sz.height as f64 / sf)
        })
        .unwrap_or((1920.0, 1080.0));

    // X: 박스 우측 우선, 공간 부족 시 좌측
    let x = if box_x + box_w + GAP + POPUP_W <= screen_w {
        box_x + box_w + GAP
    } else {
        (box_x - POPUP_W - GAP).max(0.0)
    };

    // Y: 박스 상단 정렬, 화면 아래 벗어나면 위로 올림
    let y = if box_y + POPUP_H <= screen_h {
        box_y
    } else {
        (screen_h - POPUP_H).max(0.0)
    };

    let _ = box_h; // 미사용 경고 방지
    (x, y)
}

// ── Tauri 커맨드 ─────────────────────────────────────────────────────────────

/// OCR 영역 클릭 시 호출. 오버레이는 유지하고 팝업에 번역 결과를 표시한다.
/// box_x/y/w/h: 오버레이 논리 픽셀 좌표 (CSS pixels)
#[tauri::command]
async fn select_text(
    app: AppHandle,
    text: String,
    box_x: f64,
    box_y: f64,
    box_w: f64,
    box_h: f64,
) -> Result<(), String> {
    let popup = app
        .get_webview_window("popup")
        .ok_or("팝업 창을 찾을 수 없음")?;

    let (px, py) = calc_popup_pos(&app, box_x, box_y, box_w, box_h);
    let _ = popup.set_position(tauri::Position::Logical(tauri::LogicalPosition::new(px, py)));
    popup.emit("translating", ()).map_err(|e| e.to_string())?;
    let _ = popup.show();
    let _ = popup.set_focus();

    let cfg = app.state::<Config>().inner().clone();
    match call_ai(&cfg, &text).await {
        Ok(result) => {
            popup
                .emit("translation_result", &result)
                .map_err(|e| e.to_string())?;
        }
        Err(e) => {
            popup
                .emit("translation_error", &e)
                .map_err(|e2| e2.to_string())?;
        }
    }

    Ok(())
}

/// 오버레이 닫기: 오버레이와 팝업을 함께 숨긴다.
#[tauri::command]
async fn close_overlay(app: AppHandle) -> Result<(), String> {
    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.hide();
    }
    if let Some(popup) = app.get_webview_window("popup") {
        let _ = popup.hide();
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

    // 2. 팝업 숨김 (이전 번역 결과 초기화)
    if let Some(popup) = app.get_webview_window("popup") {
        let _ = popup.hide();
    }

    // 3. 오버레이 즉시 표시 (로딩 상태)
    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.emit("overlay_show", ());
        let _ = overlay.set_ignore_cursor_events(false);
        let _ = overlay.set_fullscreen(true);
        let _ = overlay.show();
        let _ = overlay.set_focus();
    }

    // 4. OCR 실행
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
        .manage(config)
        .setup(move |app| {
            // 시스템 트레이: 종료 메뉴
            let quit_item = MenuItemBuilder::new("종료")
                .id("quit")
                .build(app)?;
            let menu = MenuBuilder::new(app)
                .items(&[&quit_item])
                .build()?;
            let _tray = TrayIconBuilder::new()
                .icon(app.default_window_icon().unwrap().clone())
                .menu(&menu)
                .on_menu_event(|app, event| {
                    if event.id() == "quit" {
                        app.exit(0);
                    }
                })
                .build(app)?;

            let handle = app.handle().clone();
            let busy_clone = busy.clone();

            // WH_KEYBOARD_LL 기반 전역 키보드 훅 (RegisterHotKey는 수식키 없는 PrintScreen 등록 불가)
            std::thread::spawn(move || {
                let _ = grab(move |event: Event| {
                    if let EventType::KeyPress(Key::PrintScreen) = event.event_type {
                        // 오버레이 표시 중이거나 처리 중이면 키만 억제하고 무시
                        let overlay_visible = handle
                            .get_webview_window("overlay")
                            .and_then(|w| w.is_visible().ok())
                            .unwrap_or(false);

                        if !overlay_visible && !busy_clone.load(Ordering::SeqCst) {
                            let h = handle.clone();
                            let b = busy_clone.clone();
                            tauri::async_runtime::spawn(async move {
                                handle_prtsc(h, b).await;
                            });
                        }
                        return None; // OS 기본 동작(캡처 저장 등) 항상 차단
                    }
                    Some(event)
                });
            });

            Ok(())
        })
        .device_event_filter(tauri::DeviceEventFilter::Always)
        .invoke_handler(tauri::generate_handler![select_text, close_overlay])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}
