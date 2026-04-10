mod config;
mod ocr;
mod platform;
mod popup;
mod services;
mod window;

use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::ocr::OcrEngine;
use crate::platform::{capture_active_screen, install_capture_shortcut, prepare_overlay_for_capture};
use crate::popup::calc_popup_pos;
use crate::services::{call_ai, run_ocr};
use crate::window::{focus_active_window, focus_window, hide_window};

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
    let capture_result = tauri::async_runtime::spawn_blocking(capture_active_screen).await;
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
    prepare_overlay_for_capture(&app, &info);

    // 4. OCR 실행 (블로킹 — spawn_blocking 내에서 호출됨)
    let engine = app.state::<Arc<OcrEngine>>().inner().clone();
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

// ── CUDA DLL 선탐색 ───────────────────────────────────────────────────────────

/// CUDA 및 cuDNN DLL을 ORT가 세션을 열기 전에 미리 로드한다.
///
/// `ort::ep::cuda::preload_dylibs`를 사용하면 DLL 탐색 순서를 제어할 수 있다.
/// 배포 패키지에 번들된 DLL이 있으면 시스템 PATH보다 우선 적용된다.
///
/// CUDA와 cuDNN DLL은 동일한 디렉토리에 둔다 (`cuda/`).
/// cuDNN이 없으면 Conv2D 등 대부분의 연산이 CPU로 폴백되어 GPU 가속 효과가 없다.
///
/// 탐색 순서:
/// 1. `<실행파일 디렉토리>/cuda/`  (Tauri 번들 배포 시 리소스 위치)
/// 2. `CUDA_PATH` 환경변수 → `{CUDA_PATH}/bin`  (개발 시 CUDA 툴킷)
/// 3. 아무것도 없으면 호출 생략 — ORT가 시스템 PATH에서 자동 탐색
#[cfg(feature = "gpu")]
fn preload_cuda_dylibs_early() {
    use ort::ep::cuda;
    use std::path::PathBuf;

    // 1. 실행파일 옆 cuda/ 폴더 (Tauri 번들 / 개발 시 target/debug/cuda/)
    let exe_cuda = std::env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("cuda")))
        .filter(|p| p.exists());

    // 2. CUDA_PATH 환경변수 (Windows CUDA 툴킷 기본 설치 경로)
    let env_cuda = std::env::var_os("CUDA_PATH")
        .or_else(|| std::env::var_os("CUDA_HOME"))
        .map(|v| PathBuf::from(v).join("bin"))
        .filter(|p| p.exists());

    let cuda_dir: Option<PathBuf> = exe_cuda.or(env_cuda);

    if let Some(ref dir) = cuda_dir {
        // CUDA와 cuDNN DLL이 같은 디렉토리에 있으므로 동일한 경로를 전달한다
        match cuda::preload_dylibs(Some(dir), Some(dir)) {
            Ok(()) => eprintln!("[CUDA] DLL 로드 성공: {}", dir.display()),
            Err(e) => eprintln!("[CUDA] DLL 로드 실패 ({}): {e}", dir.display()),
        }
    }
    // cuda_dir == None이면 아무것도 하지 않는다 — ORT가 PATH에서 자동 탐색한다
}

// ── 앱 진입점 ─────────────────────────────────────────────────────────────────

pub fn run() {
    // GPU 빌드: CUDA 런타임 DLL을 조기 로드한다.
    // 1순위 — Tauri 리소스 디렉토리의 cuda/ 및 cudnn/ 폴더 (배포 번들)
    // 2순위 — CUDA_PATH 환경변수 (개발 시 CUDA 툴킷 설치 경로)
    // 3순위 — 시스템 PATH (CUDA 툴킷이 전역 설치된 경우 자동 탐색)
    #[cfg(feature = "gpu")]
    preload_cuda_dylibs_early();

    let config = Config::from_env().expect("설정 로드 실패");
    let busy = Arc::new(AtomicBool::new(false));

    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            focus_active_window(app);
        }))
        .manage(config)
        .setup(move |app| {
            // OCR 엔진 초기화
            let models_dir = app
                .path()
                .resource_dir()
                .expect("리소스 디렉토리를 찾을 수 없음")
                .join("models");
            let engine =
                OcrEngine::new(&models_dir).expect("OCR 엔진 초기화 실패");
            app.manage(Arc::new(engine));
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
            close_popup
        ])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}
