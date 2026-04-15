mod config;
mod ocr;
mod platform;
mod popup;
mod services;
mod settings;
mod window;

use std::env;
use std::path::PathBuf;
use std::sync::{
    atomic::{AtomicBool, AtomicU64, Ordering},
    Arc, Mutex, RwLock,
};
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager, WebviewWindow, WebviewWindowBuilder};

use crate::config::Config;
use crate::ocr::OcrBackend;
use crate::platform::{
    install_capture_shortcut, prepare_overlay_for_capture, replace_capture_shortcut,
    CaptureShortcutHandler,
};
use crate::popup::calc_popup_pos;
use crate::services::{
    call_ai, capture_screen, crop_capture_to_region, offset_ocr_result, run_ocr, CaptureInfo,
    OcrResultPayload,
};
use crate::window::{focus_active_window, focus_window, hide_window};

type SharedConfig = Arc<RwLock<Config>>;

struct SettingsState {
    store: SettingsStore,
    prompt_path: PathBuf,
}

struct PendingSettingsNotice(Mutex<Option<SettingsNoticePayload>>);

struct CaptureShortcutState {
    busy: Arc<AtomicBool>,
    handler: CaptureShortcutHandler,
}

enum SettingsStore {
    Env(PathBuf),
}

#[derive(Clone, serde::Serialize)]
struct SettingsNoticePayload {
    message: String,
    missing_fields: Vec<String>,
}

#[derive(serde::Serialize)]
struct SaveUserSettingsResult {
    restart_required: bool,
}

#[derive(serde::Serialize)]
struct GetUserSettingsResult {
    settings: settings::UserSettings,
    show_ocr_server_device: bool,
    notice: Option<SettingsNoticePayload>,
}

fn config_snapshot(app: &AppHandle) -> Result<Config, String> {
    let shared = app.state::<SharedConfig>();
    shared
        .read()
        .map_err(|_| "설정 상태 읽기 잠금 실패".to_string())
        .map(|guard| guard.clone())
}

fn show_ocr_device_setting(cfg: &Config) -> bool {
    let _ = cfg;
    cfg!(feature = "gpu")
}

fn missing_prtsc_required_settings(cfg: &Config) -> Vec<&'static str> {
    settings::missing_required_fields(&cfg.ai_gateway_api_key, &cfg.ai_gateway_model)
}

fn missing_prtsc_required_setting_keys(cfg: &Config) -> Vec<&'static str> {
    settings::missing_required_field_keys(&cfg.ai_gateway_api_key, &cfg.ai_gateway_model)
}

fn build_settings_notice_payload(message: String, missing_fields: &[&str]) -> SettingsNoticePayload {
    SettingsNoticePayload {
        message,
        missing_fields: missing_fields.iter().map(|field| (*field).to_string()).collect(),
    }
}

fn open_settings_with_notice(app: &AppHandle, payload: &SettingsNoticePayload) {
    if let Some(window) = app.get_webview_window("settings") {
        let _ = window.show();
        let _ = window.set_focus();
        let _ = window.emit("settings_notice", payload);
        return;
    }

    store_pending_settings_notice(app, payload.clone());
    if let Err(err) = open_settings_window(app) {
        eprintln!("설정 창을 열 수 없음: {err}");
    }
}

fn open_settings_window(app: &AppHandle) -> Result<WebviewWindow, String> {
    let window = ensure_settings_window(app)?;
    window
        .show()
        .map_err(|e| format!("설정 창 표시 실패: {e}"))?;
    window
        .set_focus()
        .map_err(|e| format!("설정 창 포커스 실패: {e}"))?;
    Ok(window)
}

fn ensure_settings_window(app: &AppHandle) -> Result<WebviewWindow, String> {
    if let Some(window) = app.get_webview_window("settings") {
        return Ok(window);
    }

    let config = app
        .config()
        .app
        .windows
        .iter()
        .find(|window| window.label == "settings")
        .cloned()
        .ok_or("settings 윈도우 설정을 찾을 수 없음".to_string())?;

    WebviewWindowBuilder::from_config(app, &config)
        .map_err(|e| format!("설정 창 빌더 생성 실패: {e}"))?
        .build()
        .map_err(|e| format!("설정 창 생성 실패: {e}"))
}

fn store_pending_settings_notice(app: &AppHandle, payload: SettingsNoticePayload) {
    if let Some(state) = app.try_state::<PendingSettingsNotice>() {
        if let Ok(mut guard) = state.0.lock() {
            *guard = Some(payload);
        }
    }
}

fn take_pending_settings_notice(app: &AppHandle) -> Option<SettingsNoticePayload> {
    app.try_state::<PendingSettingsNotice>()
        .and_then(|state| take_pending_settings_notice_slot(&state.0))
}

fn take_pending_settings_notice_slot(
    slot: &Mutex<Option<SettingsNoticePayload>>,
) -> Option<SettingsNoticePayload> {
    slot.lock().ok().and_then(|mut guard| guard.take())
}

fn is_development_build() -> bool {
    cfg!(debug_assertions)
}

fn emit_ocr_outcome(app: &AppHandle, result: Result<OcrResultPayload, String>) {
    let Some(overlay) = app.get_webview_window("overlay") else {
        if let Err(e) = &result {
            eprintln!("OCR 오류 (오버레이 없음): {e}");
        }
        return;
    };
    match result {
        Ok(ocr) => {
            let _ = overlay.emit("ocr_result", &ocr);
        }
        Err(e) => {
            eprintln!("OCR 오류: {e}");
            let _ = overlay.emit("ocr_error", &e);
        }
    }
}

/// OCR 세대 토큰. 진행 중 OCR이 끝난 시점에 세대가 바뀌었으면 결과를 버린다.
struct OcrJobGen(AtomicU64);

/// OCR 시작 세대(`my_gen`)와 현재 세대(`current_gen`)가 같으면 emit해도 된다.
fn should_emit_ocr(my_gen: u64, current_gen: u64) -> bool {
    my_gen == current_gen
}

fn emit_ocr_outcome_if_current(
    app: &AppHandle,
    my_gen: u64,
    result: Result<OcrResultPayload, String>,
) {
    let current = app.state::<OcrJobGen>().0.load(Ordering::SeqCst);
    if !should_emit_ocr(my_gen, current) {
        eprintln!("[OCR] 취소된 작업의 결과를 버립니다 (my_gen={my_gen}, current={current})");
        return;
    }
    emit_ocr_outcome(app, result);
}

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

    let cfg = config_snapshot(&app)?;
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
    // 진행 중 OCR 작업의 결과를 무효화한다.
    app.state::<OcrJobGen>().0.fetch_add(1, Ordering::SeqCst);
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
fn get_user_settings(app: AppHandle) -> Result<GetUserSettingsResult, String> {
    let cfg = config_snapshot(&app)?;
    Ok(GetUserSettingsResult {
        settings: settings::UserSettings::from_config(&cfg),
        show_ocr_server_device: show_ocr_device_setting(&cfg),
        notice: take_pending_settings_notice(&app),
    })
}

#[tauri::command]
fn save_user_settings(
    app: AppHandle,
    settings: settings::UserSettings,
) -> Result<SaveUserSettingsResult, String> {
    let settings = settings.validate();
    let missing = settings::missing_required_fields(
        &settings.ai_gateway_api_key,
        &settings.ai_gateway_model,
    );
    if !missing.is_empty() {
        return Err(format!("필수 항목을 입력하세요: {}", missing.join(", ")));
    }
    settings::validate_capture_shortcut(&settings.capture_shortcut)?;
    let current_config = config_snapshot(&app)?;
    let shortcut_state = app.state::<CaptureShortcutState>();
    replace_capture_shortcut(
        &app,
        shortcut_state.busy.clone(),
        &current_config.capture_shortcut,
        &settings.capture_shortcut,
        shortcut_state.handler.clone(),
    )?;

    let rollback_shortcut = |cause: String| -> Result<SaveUserSettingsResult, String> {
        let rollback_result = replace_capture_shortcut(
            &app,
            shortcut_state.busy.clone(),
            &settings.capture_shortcut,
            &current_config.capture_shortcut,
            shortcut_state.handler.clone(),
        );
        match rollback_result {
            Ok(()) => Err(format!(
                "{cause}. 캡처 단축키는 기존 값으로 복구했습니다."
            )),
            Err(rollback_err) => Err(format!(
                "{cause}. 캡처 단축키 복구도 실패했습니다: {rollback_err}"
            )),
        }
    };

    let state = app.state::<SettingsState>();
    match &state.store {
        SettingsStore::Env(path) => {
            if let Err(err) = settings::save_to_env_file(path, &settings) {
                return rollback_shortcut(err);
            }
        }
    }
    if let Err(e) = std::fs::write(&state.prompt_path, &settings.system_prompt) {
        return rollback_shortcut(format!(
            "프롬프트 파일 저장 실패 ({}): {e}",
            state.prompt_path.display()
        ));
    }

    let shared = app.state::<SharedConfig>();
    let restart_required = {
        let mut guard = shared
            .write()
            .map_err(|_| "설정 상태 쓰기 잠금 실패".to_string())?;
        let restart_required = guard.ocr_server_device != settings.ocr_server_device;
        settings.apply_to(&mut guard);
        restart_required
    };

    Ok(SaveUserSettingsResult { restart_required })
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
    // 영역 선택 OCR은 같은 세션의 연속이므로 세대를 bump하지 않고 스냅샷만 기록.
    let my_gen = app.state::<OcrJobGen>().0.load(Ordering::SeqCst);
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

    let cfg = config_snapshot(&app)?;
    let engine = app.state::<Arc<OcrBackend>>().inner().clone();
    let result = tauri::async_runtime::spawn_blocking(move || {
        let mut ocr = run_ocr(&cfg, &engine, cropped, orig_width, orig_height)?;
        offset_ocr_result(&mut ocr, offset_x, offset_y);
        Ok::<_, String>(ocr)
    })
    .await
    .map_err(|e| format!("OCR 스레드 오류: {e}"))?;

    emit_ocr_outcome_if_current(&app, my_gen, result);
    Ok(())
}

// ── PrtSc 처리 ────────────────────────────────────────────────────────────────

async fn handle_prtsc(app: AppHandle, busy: Arc<AtomicBool>) {
    if busy.swap(true, Ordering::SeqCst) {
        return;
    }

    let cfg = match config_snapshot(&app) {
        Ok(cfg) => cfg,
        Err(e) => {
            eprintln!("설정 스냅샷 오류: {e}");
            busy.store(false, Ordering::SeqCst);
            return;
        }
    };
    let missing = missing_prtsc_required_settings(&cfg);
    if !missing.is_empty() {
        let payload = build_settings_notice_payload(
            format!("설정에서 다음 항목을 먼저 입력하세요: {}", missing.join(", ")),
            &missing_prtsc_required_setting_keys(&cfg),
        );
        open_settings_with_notice(&app, &payload);
        busy.store(false, Ordering::SeqCst);
        return;
    }

    // 새 캡처 세션 시작: 세대 번호를 bump해 진행 중인 이전 작업을 무효화한다.
    let my_gen = app.state::<OcrJobGen>().0.fetch_add(1, Ordering::SeqCst) + 1;

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
    emit_ocr_outcome_if_current(&app, my_gen, ocr_result);

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
    let current_exe_dir = env::current_exe()
        .ok()
        .and_then(|current_exe| current_exe.parent().map(|parent| parent.to_path_buf()));
    resolve_ocr_server_executable_with_current_exe_dir(resource_dir, current_exe_dir, configured)
}

fn resolve_ocr_server_executable_with_current_exe_dir(
    resource_dir: Option<PathBuf>,
    current_exe_dir: Option<PathBuf>,
    configured: &str,
) -> String {
    let configured_path = PathBuf::from(configured);
    if configured_path.exists() {
        return configured.to_string();
    }

    let Some(file_name) = configured_path.file_name() else {
        return configured.to_string();
    };
    let mut candidates = Vec::new();
    if let Some(resource_dir) = resource_dir {
        candidates.push(resource_dir.join(file_name));
        if let Some(parent_name) = configured_path
            .parent()
            .and_then(|parent| parent.file_name())
        {
            candidates.push(resource_dir.join(parent_name).join(file_name));
        }
    }
    if let Some(app_dir) = current_exe_dir {
        candidates.push(app_dir.join(file_name));
        candidates.push(app_dir.join("ocr_server").join(file_name));
    }

    for resource_path in candidates {
        if resource_path.exists() {
            return resource_path.to_string_lossy().into_owned();
        }
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
    const ENV_EXAMPLE: &str = include_str!("../.env.example");
    let development_build = is_development_build();
    // 시작 시 warmup이 끝날 때까지 핫키를 차단하기 위해 busy=true로 초기화.
    let busy = Arc::new(AtomicBool::new(true));
    let capture_shortcut_handler: CaptureShortcutHandler = Arc::new(|app, busy| {
        tauri::async_runtime::spawn(async move {
            handle_prtsc(app, busy).await;
        });
    });

    tauri::Builder::default()
        .plugin(tauri_plugin_single_instance::init(|app, _args, _cwd| {
            focus_active_window(app);
        }))
        .plugin(tauri_plugin_global_shortcut::Builder::new().build())
        .manage(reqwest::Client::new())
        .manage(PendingCapture(Mutex::new(None)))
        .manage(PendingSettingsNotice(Mutex::new(None)))
        .manage(OcrJobGen(AtomicU64::new(0)))
        .manage(CaptureShortcutState {
            busy: busy.clone(),
            handler: capture_shortcut_handler.clone(),
        })
        .setup(move |app| {
            #[cfg(target_os = "linux")]
            let _ = register_linux_portal_host_app();

            let (env_path, prompt_path) = if development_build {
                let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
                (root.join(".env"), root.join(".prompt"))
            } else {
                let dir = app
                    .path()
                    .app_data_dir()
                    .map_err(|e| format!("앱 데이터 경로 확인 실패: {e}"))?;
                (dir.join(".env"), dir.join(".prompt"))
            };
            settings::materialize_env_file(&env_path, ENV_EXAMPLE)?;
            config::materialize_prompt_file(&prompt_path)?;
            app.manage(SettingsState {
                store: SettingsStore::Env(env_path.clone()),
                prompt_path: prompt_path.clone(),
            });

            let config = Config::from_env_file(&env_path, &prompt_path).map_err(|e| {
                if development_build {
                    format!("개발 설정(.env/.prompt) 로드 실패: {e}")
                } else {
                    e
                }
            })?;
            app.manage(Arc::new(RwLock::new(config.clone())));

            // OCR 엔진 초기화
            let mut config = config;
            config.ocr_server_executable = resolve_ocr_server_executable(
                app.path().resource_dir().ok(),
                &config.ocr_server_executable,
            );
            let backend = OcrBackend::new(&config).expect("OCR 엔진 초기화 실패");
            app.manage(Arc::new(backend));
            // 시스템 트레이: 종료 메뉴
            let settings_item = MenuItemBuilder::new("설정…").id("settings").build(app)?;
            let quit_item = MenuItemBuilder::new("종료").id("quit").build(app)?;
            let menu = MenuBuilder::new(app)
                .items(&[&settings_item, &quit_item])
                .build()?;
            let tray_rgba = image::load_from_memory(include_bytes!("../icons/tray-icon.png"))
                .expect("트레이 아이콘 로드 실패")
                .into_rgba8();
            let (tw, th) = tray_rgba.dimensions();
            let tray_icon = tauri::image::Image::new_owned(tray_rgba.into_raw(), tw, th);
            let _tray = TrayIconBuilder::new()
                .icon(tray_icon)
                .menu(&menu)
                .on_menu_event(|app, event| {
                    match event.id().as_ref() {
                        "settings" => {
                            let _ = open_settings_window(app);
                        }
                        "quit" => app.exit(0),
                        _ => {}
                    }
                })
                .build(app)?;

            let capture_shortcut = config.capture_shortcut.clone();
            install_capture_shortcut(
                app.handle().clone(),
                busy.clone(),
                &capture_shortcut,
                capture_shortcut_handler.clone(),
            );

            // 백그라운드에서 OCR 사이드카를 선행 시작(Python 쪽 warmup_models 포함)한 뒤
            // 로딩 창을 숨기고 핫키 busy 플래그를 해제한다.
            let warmup_handle = app.handle().clone();
            let warmup_busy = busy.clone();
            tauri::async_runtime::spawn(async move {
                let engine = warmup_handle.state::<Arc<OcrBackend>>().inner().clone();
                let warmup_result = tauri::async_runtime::spawn_blocking(move || engine.warmup())
                    .await
                    .map_err(|e| format!("OCR warmup 스레드 오류: {e}"))
                    .and_then(|r| r);
                if let Err(e) = warmup_result {
                    eprintln!("OCR warmup 실패: {e}");
                }
                if let Some(loading) = warmup_handle.get_webview_window("loading") {
                    let _ = loading.close();
                }
                warmup_busy.store(false, Ordering::SeqCst);
            });

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            select_text,
            close_overlay,
            close_popup,
            run_region_ocr,
            get_user_settings,
            save_user_settings
        ])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}

#[cfg(test)]
mod tests {
    use super::{
        build_settings_notice_payload, clone_pending_capture, missing_prtsc_required_setting_keys,
        missing_prtsc_required_settings, resolve_ocr_server_executable,
        resolve_ocr_server_executable_with_current_exe_dir, should_emit_ocr, show_ocr_device_setting,
        take_pending_settings_notice_slot, SettingsNoticePayload,
    };
    use crate::config::Config;
    use crate::services::CaptureInfo;
    use image::{DynamicImage, Rgba, RgbaImage};
    use std::fs;
    use std::path::PathBuf;
    use std::sync::Mutex;
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
    fn 세대가_같으면_ocr_결과를_emit한다() {
        assert!(should_emit_ocr(7, 7));
    }

    #[test]
    fn 세대가_다르면_ocr_결과를_버린다() {
        assert!(!should_emit_ocr(7, 8));
        assert!(!should_emit_ocr(0, 1));
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
            resolve_ocr_server_executable_with_current_exe_dir(
                Some(resource_dir.clone()),
                None,
                "missing/ocr_server.exe",
            );

        assert_eq!(PathBuf::from(resolved), exe_path);

        let _ = fs::remove_dir_all(resource_dir);
    }

    #[test]
    fn configured_경로가_실존하면_그대로_반환한다() {
        let dir = temp_path("buzhidao-ocr-server-configured");
        fs::create_dir_all(&dir).expect("configured 디렉토리 생성 실패");
        let exe_path = dir.join("ocr_server.exe");
        fs::write(&exe_path, b"exe").expect("configured exe 생성 실패");

        let resolved =
            resolve_ocr_server_executable_with_current_exe_dir(
                Some(dir.clone()),
                None,
                &exe_path.to_string_lossy(),
            );

        assert_eq!(PathBuf::from(resolved), exe_path);

        let _ = fs::remove_dir_all(dir);
    }

    #[test]
    fn resource_dir가_없으면_configured_경로를_그대로_반환한다() {
        let resolved =
            resolve_ocr_server_executable_with_current_exe_dir(None, None, "missing/ocr_server.exe");
        assert_eq!(resolved, "missing/ocr_server.exe");
    }

    #[test]
    fn 번들_리소스에_onedir_폴더가_있으면_그_안의_ocr_server를_사용한다() {
        let resource_dir = temp_path("buzhidao-ocr-server-onedir-resource");
        let onedir_dir = resource_dir.join("ocr_server");
        fs::create_dir_all(&onedir_dir).expect("onedir 리소스 디렉토리 생성 실패");
        let exe_path = onedir_dir.join("ocr_server.exe");
        fs::write(&exe_path, b"exe").expect("ocr server 생성 실패");

        let resolved = resolve_ocr_server_executable(
            Some(resource_dir.clone()),
            "missing-nonexistent/ocr_server/ocr_server.exe",
        );

        assert_eq!(PathBuf::from(resolved), exe_path);

        let _ = fs::remove_dir_all(resource_dir);
    }

    #[test]
    fn 릴리즈_배치에서는_앱_옆_ocr_server_폴더를_사용한다() {
        let app_dir = temp_path("buzhidao-release-app-dir");
        let sidecar_dir = app_dir.join("ocr_server");
        let sidecar_path = sidecar_dir.join("ocr_server.exe");
        fs::create_dir_all(&sidecar_dir).expect("ocr_server 디렉토리 생성 실패");
        fs::write(&sidecar_path, b"exe").expect("릴리즈 배치용 ocr_server 생성 실패");

        let resolved = resolve_ocr_server_executable_with_current_exe_dir(
            None,
            Some(app_dir.clone()),
            "missing/ocr_server.exe",
        );

        assert_eq!(PathBuf::from(resolved), sidecar_path);

        let _ = fs::remove_dir_all(app_dir);
    }

    #[test]
    fn gpu_앱_빌드에서만_장치_설정을_노출한다() {
        let cfg = Config {
            source: "en".to_string(),
            score_thresh: 0.5,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: "k".to_string(),
            ai_gateway_model: "m".to_string(),
            system_prompt: "p".to_string(),
            word_gap: 20,
            line_gap: 15,
            ocr_server_executable: "../ocr_server/dist/ocr_server/ocr_server.exe".to_string(),
            ocr_server_startup_timeout_secs: 30,
            ocr_server_request_timeout_secs: 20,
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        };
        assert_eq!(show_ocr_device_setting(&cfg), cfg!(feature = "gpu"));
    }

    #[test]
    fn prt_sc_필수_설정_누락을_판별한다() {
        let cfg = Config {
            source: "en".to_string(),
            score_thresh: 0.5,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: "".to_string(),
            ai_gateway_model: " ".to_string(),
            system_prompt: "p".to_string(),
            word_gap: 20,
            line_gap: 15,
            ocr_server_executable: "../ocr_server/dist/ocr_server/ocr_server.exe".to_string(),
            ocr_server_startup_timeout_secs: 30,
            ocr_server_request_timeout_secs: 20,
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        };

        assert_eq!(
            missing_prtsc_required_settings(&cfg),
            vec!["AI Gateway API Key", "AI Gateway Model"]
        );
        assert_eq!(
            missing_prtsc_required_setting_keys(&cfg),
            vec!["ai_gateway_api_key", "ai_gateway_model"]
        );
    }

    #[test]
    fn 설정_안내_payload를_구성한다() {
        let payload = build_settings_notice_payload(
            "필수 항목을 입력하세요".to_string(),
            &["ai_gateway_api_key", "ai_gateway_model"],
        );

        assert_eq!(payload.message, "필수 항목을 입력하세요");
        assert_eq!(
            payload.missing_fields,
            vec!["ai_gateway_api_key", "ai_gateway_model"]
        );
    }

    #[test]
    fn pending_settings_notice는_한번만_소비된다() {
        let slot = Mutex::new(Some(SettingsNoticePayload {
            message: "필수 항목을 입력하세요".to_string(),
            missing_fields: vec!["ai_gateway_api_key".to_string()],
        }));

        let first = take_pending_settings_notice_slot(&slot);
        let second = take_pending_settings_notice_slot(&slot);

        assert_eq!(first.as_ref().map(|payload| payload.message.as_str()), Some("필수 항목을 입력하세요"));
        assert!(second.is_none());
    }
}
