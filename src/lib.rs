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
use std::{
    env,
    path::{Path, PathBuf},
};
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager};

use crate::config::Config;
use crate::ocr::OcrEngine;
use crate::platform::{install_capture_shortcut, prepare_overlay_for_capture};
use crate::popup::calc_popup_pos;
use crate::services::{call_ai, capture_screen, run_ocr};
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

    // 1. 스크린샷 캡처
    let info = match capture_screen(&app).await {
        Ok(v) => v,
        Err(e) => {
            eprintln!("캡처 오류: {e}");
            busy.store(false, Ordering::SeqCst);
            return;
        }
    };

    let (orig_width, orig_height) = (info.orig_width, info.orig_height);

    // 2. 오버레이 즉시 표시 (로딩 상태)
    prepare_overlay_for_capture(&app, &info);

    // 3. OCR 실행 (블로킹 — spawn_blocking 내에서 호출됨)
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

    // 1. 실행파일 옆 cuda/ 폴더 (Tauri 번들 / 개발 시 target/debug/cuda/)
    let exe_cuda = env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|d| d.join("cuda")))
        .filter(|p| p.exists());

    // 2. CUDA_PATH 환경변수 (Windows CUDA 툴킷 기본 설치 경로)
    let env_cuda = env::var_os("CUDA_PATH")
        .or_else(|| env::var_os("CUDA_HOME"))
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

fn has_required_models(models_dir: &Path) -> bool {
    ["det.onnx", "cls.onnx", "rec.onnx", "rec_dict.txt"]
        .iter()
        .all(|name| models_dir.join(name).exists())
}

fn resolve_models_dir(
    resource_dir: Option<PathBuf>,
    current_exe: Option<PathBuf>,
) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();

    if let Some(dir) = resource_dir {
        candidates.push(dir.join("models"));
    }
    if let Some(exe_dir) = current_exe.and_then(|path| path.parent().map(Path::to_path_buf)) {
        let exe_models = exe_dir.join("models");
        if !candidates.iter().any(|candidate| candidate == &exe_models) {
            candidates.push(exe_models);
        }
    }

    for candidate in candidates {
        if has_required_models(&candidate) {
            return Ok(candidate);
        }
    }

    Err("OCR 모델 디렉토리를 찾을 수 없음".to_string())
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
        .manage(reqwest::Client::new())
        .setup(move |app| {
            #[cfg(target_os = "linux")]
            let _ = register_linux_portal_host_app();

            // OCR 엔진 초기화
            let models_dir =
                resolve_models_dir(app.path().resource_dir().ok(), env::current_exe().ok())
                    .expect("OCR 모델 디렉토리를 찾을 수 없음");
            let (det_thresh, box_thresh) = {
                let cfg = app.state::<Config>();
                (cfg.det_thresh, cfg.box_thresh)
            };
            let engine =
                OcrEngine::new(&models_dir, det_thresh, box_thresh).expect("OCR 엔진 초기화 실패");
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
                tauri::async_runtime::block_on(handle_prtsc(app, busy));
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

#[cfg(test)]
mod tests {
    use super::resolve_models_dir;
    use std::fs;
    use std::path::{Path, PathBuf};
    use std::time::{SystemTime, UNIX_EPOCH};

    fn temp_path(prefix: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("시계가 UNIX_EPOCH 이전입니다")
            .as_nanos();
        std::env::temp_dir().join(format!("{prefix}-{nanos}"))
    }

    fn create_required_models(dir: &Path) {
        fs::create_dir_all(dir).expect("모델 디렉토리 생성 실패");
        for name in ["det.onnx", "cls.onnx", "rec.onnx", "rec_dict.txt"] {
            fs::write(dir.join(name), b"x").expect("모델 파일 생성 실패");
        }
    }

    #[test]
    fn 번들_리소스_모델을_우선_사용한다() {
        let resource_dir = temp_path("buzhidao-resource");
        let exe_dir = temp_path("buzhidao-exe");
        create_required_models(&resource_dir.join("models"));
        create_required_models(&exe_dir.join("models"));
        fs::create_dir_all(&exe_dir).expect("실행 파일 디렉토리 생성 실패");
        let exe_path = exe_dir.join("buzhidao.exe");
        fs::write(&exe_path, b"exe").expect("실행 파일 생성 실패");

        let resolved = resolve_models_dir(Some(resource_dir.clone()), Some(exe_path))
            .expect("리소스 디렉토리의 모델을 찾아야 한다");

        assert_eq!(resolved, resource_dir.join("models"));

        let _ = fs::remove_dir_all(resource_dir);
        let _ = fs::remove_dir_all(exe_dir);
    }

    #[test]
    fn 실행파일_옆_models를_fallback으로_사용한다() {
        let resource_dir = temp_path("buzhidao-empty-resource");
        let exe_dir = temp_path("buzhidao-exe-fallback");
        fs::create_dir_all(&resource_dir).expect("빈 리소스 디렉토리 생성 실패");
        create_required_models(&exe_dir.join("models"));
        fs::create_dir_all(&exe_dir).expect("실행 파일 디렉토리 생성 실패");
        let exe_path = exe_dir.join("buzhidao.exe");
        fs::write(&exe_path, b"exe").expect("실행 파일 생성 실패");

        let resolved = resolve_models_dir(Some(resource_dir.clone()), Some(exe_path))
            .expect("실행 파일 옆 models를 찾아야 한다");

        assert_eq!(resolved, exe_dir.join("models"));

        let _ = fs::remove_dir_all(resource_dir);
        let _ = fs::remove_dir_all(exe_dir);
    }
}
