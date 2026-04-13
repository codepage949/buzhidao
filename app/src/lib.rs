mod config;
mod ocr;
mod platform;
mod popup;
mod services;
mod window;

use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc, Mutex,
};
use std::env;
use std::path::PathBuf;
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::ocr::OcrBackend;
use crate::platform::{install_capture_shortcut, prepare_overlay_for_capture};
use crate::popup::calc_popup_pos;
use crate::services::{
    call_ai, capture_screen, crop_capture_to_region, offset_ocr_result, run_ocr, CaptureInfo,
};
use crate::window::{focus_active_window, focus_window, hide_window};

struct PendingCapture(Mutex<Option<CaptureInfo>>);

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
) -> Result<(), String> {
    let popup = app
        .get_webview_window("popup")
        .ok_or("팝업 창을 찾을 수 없음")?;

    let (px, py) = calc_popup_pos(&app, box_x, box_y, box_w);
    let _ = popup.set_position(tauri::Position::Logical(tauri::LogicalPosition::new(
        px, py,
    )));
    popup.emit("translating", ()).map_err(|e| e.to_string())?;
    let _ = popup.show();
    let _ = popup.set_focus();

    let cfg = app.state::<Config>().inner().clone();
    let client = app.state::<reqwest::Client>().inner().clone();
    match call_ai(&client, &cfg, &text).await {
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
    clear_pending_capture(&app);
    Ok(())
}

/// 팝업만 닫기: 팝업을 숨기고 오버레이 포커스를 복구한다.
#[tauri::command]
async fn close_popup(app: AppHandle) -> Result<(), String> {
    hide_window(&app, "popup");
    focus_window(&app, "overlay");
    Ok(())
}

#[tauri::command]
async fn run_region_ocr(
    app: AppHandle,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(), String> {
    let (cropped, offset_x, offset_y, orig_width, orig_height) = {
        let pending = app.state::<PendingCapture>();
        let guard = pending
            .0
            .lock()
            .map_err(|_| "캡처 상태 잠금 실패".to_string())?;
        let capture = clone_pending_capture(&guard)?;
        crop_capture_to_region(
            capture, rect_x, rect_y, rect_w, rect_h, viewport_w, viewport_h,
        )?
    };

    let cfg = app.state::<Config>().inner().clone();
    let engine = app.state::<Arc<OcrBackend>>().inner().clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let mut ocr = run_ocr(&cfg, &engine, cropped, orig_width, orig_height)?;
        offset_ocr_result(&mut ocr, offset_x, offset_y);
        Ok::<_, String>(ocr)
    })
    .await
    .map_err(|e| format!("OCR 스레드 오류: {e}"))?;

    let overlay = app
        .get_webview_window("overlay")
        .ok_or("오버레이 창을 찾을 수 없음".to_string())?;
    match result {
        Ok(ocr) => overlay
            .emit("ocr_result", &ocr)
            .map_err(|e| e.to_string())?,
        Err(err) => overlay.emit("ocr_error", &err).map_err(|e| e.to_string())?,
    }

    Ok(())
}

// ── PrtSc 처리 ────────────────────────────────────────────────────────────────

async fn handle_prtsc(app: AppHandle, busy: Arc<AtomicBool>) {
    if busy.swap(true, Ordering::SeqCst) {
        return;
    }

    // 1. 스크린샷 캡처
    let info = match capture_screen(&app).await {
        Ok(v) => v,
        Err(e) => {
            eprintln!("캡처 오류: {e}");
            busy.store(false, Ordering::SeqCst);
            return;
        }
    };

    // 2. 오버레이 즉시 표시 (로딩 상태)
    prepare_overlay_for_capture(&app, &info);
    store_pending_capture(&app, info.clone());

    let engine = app.state::<Arc<OcrBackend>>().inner().clone();

    // 3. OCR 실행 (블로킹 — spawn_blocking 내에서 호출됨)
    let cfg = app.state::<Config>().inner().clone();
    let (orig_width, orig_height) = (info.orig_width, info.orig_height);
    let ocr_result = {
        let img = info.image;
        tauri::async_runtime::spawn_blocking(move || {
            run_ocr(&cfg, &engine, img, orig_width, orig_height)
        })
        .await
        .map_err(|e| format!("OCR 스레드 오류: {e}"))
        .and_then(|r| r)
    };
    match ocr_result {
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

#[allow(dead_code)]
fn store_pending_capture(app: &AppHandle, capture: CaptureInfo) {
    if let Some(state) = app.try_state::<PendingCapture>() {
        if let Ok(mut guard) = state.0.lock() {
            *guard = Some(capture);
        }
    }
}

fn clone_pending_capture(capture: &Option<CaptureInfo>) -> Result<CaptureInfo, String> {
    capture
        .clone()
        .ok_or("선택할 캡처 이미지가 없음".to_string())
}

fn clear_pending_capture(app: &AppHandle) {
    if let Some(state) = app.try_state::<PendingCapture>() {
        if let Ok(mut guard) = state.0.lock() {
            *guard = None;
        }
    }
}

fn resolve_ocr_server_executable(resource_dir: Option<PathBuf>, configured: &str) -> String {
    let configured_path = PathBuf::from(configured);
    if configured_path.exists() {
        return configured.to_string();
    }

    let Some(file_name) = configured_path.file_name() else {
        return configured.to_string();
    };
    let Some(resource_dir) = resource_dir else {
        return configured.to_string();
    };

    let resource_path = resource_dir.join(file_name);
    if resource_path.exists() {
        return resource_path.to_string_lossy().into_owned();
    }

    configured.to_string()
}

#[cfg(target_os = "linux")]
const PORTAL_APP_ID: &str = "com.buzhidao.desktop";

#[cfg(target_os = "linux")]
fn ensure_linux_desktop_entry() -> Result<PathBuf, String> {
    use std::fs;

    let apps_dir = dirs::home_dir()
        .ok_or("HOME 디렉토리를 찾을 수 없음".to_string())?
        .join(".local/share/applications");
    fs::create_dir_all(&apps_dir).map_err(|e| format!("desktop 디렉토리 생성 실패: {e}"))?;

    let desktop_path = apps_dir.join(format!("{PORTAL_APP_ID}.desktop"));
    if desktop_path.exists() {
        return Ok(desktop_path);
    }

    let exe = env::current_exe().map_err(|e| format!("실행 파일 경로 확인 실패: {e}"))?;
    let content = format!(
        "[Desktop Entry]\nType=Application\nName=buzhidao\nExec={}\nTerminal=false\nCategories=Utility;\nStartupNotify=false\n",
        exe.display()
    );
    fs::write(&desktop_path, content)
        .map_err(|e| format!("desktop 파일 생성 실패 ({}): {e}", desktop_path.display()))?;

    Ok(desktop_path)
}

#[cfg(target_os = "linux")]
fn register_linux_portal_host_app() -> Result<(), String> {
    use ashpd::zbus::blocking::{Connection, Proxy};
    use ashpd::zvariant::Value;
    use std::collections::HashMap;

    let _desktop_path = ensure_linux_desktop_entry()?;
    let connection = Connection::session().map_err(|e| format!("D-Bus 세션 연결 실패: {e}"))?;
    let proxy = Proxy::new(
        &connection,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.host.portal.Registry",
    )
    .map_err(|e| format!("포털 registry 프록시 생성 실패: {e}"))?;
    let options = HashMap::<&str, Value<'_>>::new();

    proxy
        .call_method("Register", &(PORTAL_APP_ID, options))
        .map_err(|e| format!("포털 host app 등록 실패: {e}"))?;

    Ok(())
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
        .manage(reqwest::Client::new())
        .manage(PendingCapture(Mutex::new(None)))
        .setup(move |app| {
            #[cfg(target_os = "linux")]
            let _ = register_linux_portal_host_app();

            // OCR 엔진 초기화
            let mut config = app.state::<Config>().inner().clone();
            config.ocr_server_executable = resolve_ocr_server_executable(
                app.path().resource_dir().ok(),
                &config.ocr_server_executable,
            );
            let backend = OcrBackend::new(&config).expect("OCR 엔진 초기화 실패");
            app.manage(Arc::new(backend));
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

            install_capture_shortcut(app.handle().clone(), busy.clone(), |app, busy| {
                tauri::async_runtime::spawn(async move {
                    handle_prtsc(app, busy).await;
                });
            });

            Ok(())
        })
        .device_event_filter(tauri::DeviceEventFilter::Always)
        .invoke_handler(tauri::generate_handler![
            select_text,
            close_overlay,
            close_popup,
            run_region_ocr
        ])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}

#[cfg(test)]
mod tests {
    use super::{clone_pending_capture, resolve_ocr_server_executable};
    use crate::services::CaptureInfo;
    use image::{DynamicImage, Rgba, RgbaImage};
    use std::fs;
    use std::path::PathBuf;
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_path(prefix: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("시계가 UNIX_EPOCH 이전입니다")
            .as_nanos();
        std::env::temp_dir().join(format!("{prefix}-{nanos}"))
    }

    #[test]
    fn pending_capture는_영역_ocr_후에도_재사용할_수_있다() {
        let capture = CaptureInfo {
            image: DynamicImage::ImageRgba8(RgbaImage::from_pixel(4, 4, Rgba([1, 2, 3, 4]))),
            x: 10,
            y: 20,
            orig_width: 4,
            orig_height: 4,
        };
        let pending = Some(capture);

        let first = clone_pending_capture(&pending).expect("첫 번째 clone 실패");
        let second = clone_pending_capture(&pending).expect("두 번째 clone 실패");

        assert_eq!(first.orig_width, 4);
        assert_eq!(second.orig_height, 4);
        assert_eq!(pending.as_ref().map(|v| v.x), Some(10));
        assert_eq!(pending.as_ref().map(|v| v.y), Some(20));
    }

    #[test]
    fn pending_capture가_없으면_영역_ocr를_실행할_수_없다() {
        let pending: Option<CaptureInfo> = None;

        let err = match clone_pending_capture(&pending) {
            Ok(_) => panic!("빈 캡처는 실패해야 한다"),
            Err(err) => err,
        };

        assert!(err.contains("캡처 이미지가 없음"));
    }

    #[test]
    fn 번들_리소스에_ocr_server가_있으면_그_경로를_사용한다() {
        let resource_dir = temp_path("buzhidao-ocr-server-resource");
        fs::create_dir_all(&resource_dir).expect("리소스 디렉토리 생성 실패");
        let exe_path = resource_dir.join("ocr_server.exe");
        fs::write(&exe_path, b"exe").expect("ocr server 생성 실패");

        let resolved =
            resolve_ocr_server_executable(Some(resource_dir.clone()), "missing/ocr_server.exe");

        assert_eq!(PathBuf::from(resolved), exe_path);

        let _ = fs::remove_dir_all(resource_dir);
    }
}
