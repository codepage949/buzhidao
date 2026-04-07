mod config;
mod popup;
mod services;
mod window;

use rdev::{grab, Event, EventType, Key};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::popup::calc_popup_pos;
use crate::services::{call_ai, capture_screen, run_ocr};
use crate::window::{focus_active_window, focus_window, hide_window, place_overlay_window};

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
    let _ = popup.set_position(tauri::Position::Logical(tauri::LogicalPosition::new(
        px, py,
    )));
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
    hide_window(&app, "overlay");
    hide_window(&app, "popup");
    Ok(())
}

/// 팝업만 닫기: 팝업을 숨기고 오버레이 포커스를 복구한다.
#[tauri::command]
async fn close_popup(app: AppHandle) -> Result<(), String> {
    hide_window(&app, "popup");
    focus_window(&app, "overlay");
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
    hide_window(&app, "popup");

    // 3. 오버레이 즉시 표시 (로딩 상태)
    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.emit("overlay_show", ());
        let _ = overlay.set_ignore_cursor_events(false);
        place_overlay_window(&overlay, info.x, info.y, info.orig_width, info.orig_height);
        let _ = overlay.set_fullscreen(true);
        let _ = overlay.show();
        focus_window(&app, "overlay");
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
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            focus_active_window(app);
        }))
        .manage(config)
        .setup(move |app| {
            // 시스템 트레이: 종료 메뉴
            let quit_item = MenuItemBuilder::new("종료").id("quit").build(app)?;
            let menu = MenuBuilder::new(app).items(&[&quit_item]).build()?;
            let tray_rgba = image::load_from_memory(include_bytes!("../icons/tray-icon.png"))
                .expect("트레이 아이콘 로드 실패")
                .into_rgba8();
            let (tw, th) = tray_rgba.dimensions();
            let tray_icon = tauri::image::Image::new_owned(tray_rgba.into_raw(), tw, th);
            let _tray = TrayIconBuilder::new()
                .icon(tray_icon)
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
        .invoke_handler(tauri::generate_handler![
            select_text,
            close_overlay,
            close_popup
        ])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}
