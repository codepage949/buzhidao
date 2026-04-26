mod config;
mod ocr;
mod paddle_models;
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
use std::time::Instant;
use tauri::menu::{MenuBuilder, MenuItemBuilder};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager, WebviewWindow, WebviewWindowBuilder};

use crate::config::Config;
use crate::ocr::{ocr_stage_logging_enabled, OcrBackend};
use crate::platform::{
    install_capture_shortcut, overlay_visible, prepare_overlay_for_capture,
    replace_capture_shortcut, show_overlay_notice, CaptureShortcutHandler,
};
use crate::popup::calc_popup_pos;
use crate::services::{
    call_ai, capture_screen, crop_capture_to_region, offset_ocr_result, run_ocr, CaptureInfo,
    OcrResultPayload,
};
use crate::window::{focus_active_window, focus_window, hide_window};

type SharedConfig = Arc<RwLock<Config>>;
type SharedOcrBackend = Arc<RwLock<Result<Arc<OcrBackend>, String>>>;

pub const RELEASE_OCR_SMOKE_ARG: &str = "--release-ocr-smoke";

pub fn is_release_ocr_smoke_requested<I, S>(args: I) -> bool
where
    I: IntoIterator<Item = S>,
    S: AsRef<str>,
{
    args.into_iter()
        .any(|arg| arg.as_ref() == RELEASE_OCR_SMOKE_ARG)
}

pub fn run_release_ocr_smoke_from_env() -> Result<(), String> {
    services::ocr_pipeline::run_release_ocr_smoke_from_env()
}

struct SettingsState {
    store: SettingsStore,
    prompt_path: PathBuf,
}

struct PendingSettingsNotice(Mutex<Option<SettingsNoticePayload>>);
struct LoadingStatusState(Mutex<LoadingStatusPayload>);
struct PaddleModelRootState(RwLock<Option<PathBuf>>);

struct CaptureShortcutState {
    busy: Arc<AtomicBool>,
    handler: CaptureShortcutHandler,
}

struct CaptureRetryState {
    ocr_in_flight: AtomicBool,
    pending_retry: AtomicBool,
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

#[derive(Clone, serde::Serialize)]
struct LoadingStatusPayload {
    kind: String,
    message: Option<String>,
}

struct TranslationRequestSeq(AtomicU64);

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct TranslationStartPayload {
    request_id: u64,
}

#[derive(Clone, serde::Serialize)]
#[serde(rename_all = "camelCase")]
struct TranslationContentPayload {
    request_id: u64,
    content: String,
}

fn config_snapshot(app: &AppHandle) -> Result<Config, String> {
    let shared = app.state::<SharedConfig>();
    shared
        .read()
        .map_err(|_| "설정 상태 읽기 잠금 실패".to_string())
        .map(|guard| guard.clone())
}

fn ocr_backend_snapshot(app: &AppHandle) -> Result<Arc<OcrBackend>, String> {
    let shared = app.state::<SharedOcrBackend>();
    shared
        .read()
        .map_err(|_| "OCR 엔진 상태 읽기 잠금 실패".to_string())
        .and_then(|guard| guard.as_ref().map(Arc::clone).map_err(Clone::clone))
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

fn build_settings_notice_payload(
    message: String,
    missing_fields: &[&str],
) -> SettingsNoticePayload {
    SettingsNoticePayload {
        message,
        missing_fields: missing_fields
            .iter()
            .map(|field| (*field).to_string())
            .collect(),
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

fn set_loading_status(app: &AppHandle, kind: &str, message: Option<String>) {
    if let Some(state) = app.try_state::<LoadingStatusState>() {
        if let Ok(mut guard) = state.0.lock() {
            *guard = LoadingStatusPayload {
                kind: kind.to_string(),
                message,
            };
        }
    }
}

fn emit_ocr_busy_changed(app: &AppHandle, busy: bool) {
    if let Some(settings) = app.get_webview_window("settings") {
        let _ = settings.emit("ocr_busy_changed", busy);
    }
}

fn set_capture_busy(app: &AppHandle, value: bool) {
    let state = app.state::<CaptureShortcutState>();
    let previous = state.busy.swap(value, Ordering::SeqCst);
    if previous != value {
        emit_ocr_busy_changed(app, value);
    }
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

#[derive(Clone, Debug, PartialEq, Eq)]
struct OcrAppStageLog {
    phase: &'static str,
    capture_ms: u128,
    spawn_wait_ms: u128,
    emit_ms: u128,
}

fn format_ocr_app_stage_log(log: &OcrAppStageLog) -> String {
    format!(
        "[OCR_STAGE] app phase={} capture_ms={} spawn_wait_ms={} emit_ms={}",
        log.phase, log.capture_ms, log.spawn_wait_ms, log.emit_ms
    )
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum CaptureHotkeyAction {
    StartNow,
    QueueRetry,
    Ignore,
}

fn decide_capture_hotkey_action(
    overlay_is_visible: bool,
    busy: bool,
    ocr_in_flight: bool,
) -> CaptureHotkeyAction {
    if overlay_is_visible {
        return CaptureHotkeyAction::Ignore;
    }
    if !busy {
        return CaptureHotkeyAction::StartNow;
    }
    if ocr_in_flight {
        return CaptureHotkeyAction::QueueRetry;
    }
    CaptureHotkeyAction::Ignore
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
) -> Result<(), String> {
    let popup = app
        .get_webview_window("popup")
        .ok_or("팝업 창을 찾을 수 없음")?;
    let request_id = app
        .state::<TranslationRequestSeq>()
        .0
        .fetch_add(1, Ordering::SeqCst)
        + 1;

    let (px, py) = calc_popup_pos(&app, box_x, box_y, box_w);
    let _ = popup.set_position(tauri::Position::Logical(tauri::LogicalPosition::new(
        px, py,
    )));
    popup
        .emit("translating", TranslationStartPayload { request_id })
        .map_err(|e| e.to_string())?;
    let _ = popup.show();
    let _ = popup.set_focus();

    let cfg = config_snapshot(&app)?;
    let client = app.state::<reqwest::Client>().inner().clone();
    match call_ai(&client, &cfg, &text).await {
        Ok(result) => {
            popup
                .emit(
                    "translation_result",
                    TranslationContentPayload {
                        request_id,
                        content: result,
                    },
                )
                .map_err(|e| e.to_string())?;
        }
        Err(e) => {
            popup
                .emit(
                    "translation_error",
                    TranslationContentPayload {
                        request_id,
                        content: e,
                    },
                )
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
    set_pending_retry(&app, false);
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
fn exit_app(app: AppHandle) {
    app.exit(1);
}

#[tauri::command]
fn get_loading_status(app: AppHandle) -> Result<LoadingStatusPayload, String> {
    app.state::<LoadingStatusState>()
        .0
        .lock()
        .map(|guard| guard.clone())
        .map_err(|_| "로딩 상태 읽기 잠금 실패".to_string())
}

#[tauri::command]
fn get_ocr_busy(app: AppHandle) -> Result<bool, String> {
    Ok(app
        .state::<CaptureShortcutState>()
        .busy
        .load(Ordering::SeqCst))
}

fn show_loading_window(app: &AppHandle) {
    set_loading_status(app, "loading", None);
    if let Some(loading) = app.get_webview_window("loading") {
        let _ = loading.emit("warmup_loading", ());
        let _ = loading.show();
        let _ = loading.set_focus();
    }
}

fn show_warmup_failure(app: &AppHandle, message: &str) {
    set_loading_status(app, "failed", Some(message.to_string()));
    if let Some(loading) = app.get_webview_window("loading") {
        let _ = loading.emit("warmup_failed", message);
        let _ = loading.show();
        let _ = loading.set_focus();
    }
}

#[tauri::command]
fn save_user_settings(
    app: AppHandle,
    settings: settings::UserSettings,
) -> Result<SaveUserSettingsResult, String> {
    let settings = settings.validate();
    let missing =
        settings::missing_required_fields(&settings.ai_gateway_api_key, &settings.ai_gateway_model);
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
            Ok(()) => Err(format!("{cause}. 캡처 단축키는 기존 값으로 복구했습니다.")),
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
    let (restart_required, lang_changed) = {
        let mut guard = shared
            .write()
            .map_err(|_| "설정 상태 쓰기 잠금 실패".to_string())?;
        let restart_required = guard.ocr_server_device != settings.ocr_server_device;
        let lang_changed = guard.source != settings.source;
        settings.apply_to(&mut guard);
        (restart_required, lang_changed)
    };

    if lang_changed {
        set_capture_busy(&app, true);
        show_loading_window(&app);
        let engine = match ocr_backend_snapshot(&app) {
            Ok(engine) => engine,
            Err(err) => {
                show_warmup_failure(&app, &err);
                return Ok(SaveUserSettingsResult { restart_required });
            }
        };
        let new_lang = settings.source.clone();
        let app_handle = app.clone();
        let model_root = app
            .state::<PaddleModelRootState>()
            .0
            .read()
            .map_err(|_| "Paddle 모델 루트 읽기 잠금 실패".to_string())?
            .clone();
        tauri::async_runtime::spawn(async move {
            let Some(model_root) = model_root else {
                show_warmup_failure(
                    &app_handle,
                    "Paddle 모델 루트가 초기화되지 않았습니다. 앱을 다시 실행해 주세요.",
                );
                return;
            };
            if let Err(err) =
                paddle_models::ensure_paddle_models_for_lang_in_root(&new_lang, &model_root).await
            {
                eprintln!("[OCR] 언어 변경 모델 보장 실패: {err}");
                show_warmup_failure(&app_handle, &err);
                return;
            }
            let engine_task = engine.clone();
            let lang_for_task = new_lang.clone();
            let result = tauri::async_runtime::spawn_blocking(move || {
                engine_task.set_lang(&lang_for_task)?;
                engine_task.warmup()
            })
            .await
            .map_err(|e| format!("OCR 재웜업 스레드 오류: {e}"))
            .and_then(|r| r);
            match result {
                Ok(()) => {
                    if let Some(loading) = app_handle.get_webview_window("loading") {
                        let _ = loading.hide();
                    }
                    set_capture_busy(&app_handle, false);
                }
                Err(e) => {
                    eprintln!("[OCR] 언어 변경 웜업 실패: {e}");
                    show_warmup_failure(&app_handle, &e);
                }
            }
        });
    }

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
    let engine = ocr_backend_snapshot(&app)?;
    let result = tauri::async_runtime::spawn_blocking(move || {
        let mut ocr = run_ocr(&cfg, &engine, &cropped, orig_width, orig_height)?;
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
    let action = decide_capture_hotkey_action(
        overlay_visible(&app),
        busy.load(Ordering::SeqCst),
        ocr_in_flight(&app),
    );
    if action == CaptureHotkeyAction::QueueRetry {
        set_pending_retry(&app, true);
        show_overlay_notice(
            &app,
            "overlay_pending_retry",
            "이전 작업이 끝나면 새 캡처를 시작합니다",
        );
        return;
    }
    if action == CaptureHotkeyAction::Ignore {
        return;
    }
    if busy.swap(true, Ordering::SeqCst) {
        let action = decide_capture_hotkey_action(overlay_visible(&app), true, ocr_in_flight(&app));
        if action == CaptureHotkeyAction::QueueRetry {
            set_pending_retry(&app, true);
            show_overlay_notice(
                &app,
                "overlay_pending_retry",
                "이전 작업이 끝나면 새 캡처를 시작합니다",
            );
        }
        return;
    }
    emit_ocr_busy_changed(&app, true);

    loop {
        let flow_started = Instant::now();
        let cfg = match config_snapshot(&app) {
            Ok(cfg) => cfg,
            Err(e) => {
                eprintln!("설정 스냅샷 오류: {e}");
                break;
            }
        };
        let missing = missing_prtsc_required_settings(&cfg);
        if !missing.is_empty() {
            let payload = build_settings_notice_payload(
                format!(
                    "설정에서 다음 항목을 먼저 입력하세요: {}",
                    missing.join(", ")
                ),
                &missing_prtsc_required_setting_keys(&cfg),
            );
            open_settings_with_notice(&app, &payload);
            break;
        }

        // 새 캡처 세션 시작: 세대 번호를 bump해 진행 중인 이전 작업을 무효화한다.
        let my_gen = app.state::<OcrJobGen>().0.fetch_add(1, Ordering::SeqCst) + 1;

        // 1. 스크린샷 캡처
        let capture_started = Instant::now();
        let info = match capture_screen(&app).await {
            Ok(v) => v,
            Err(e) => {
                eprintln!("캡처 오류: {e}");
                break;
            }
        };
        let capture_ms = capture_started.elapsed().as_millis();

        // 2. 오버레이 즉시 표시 (로딩 상태)
        prepare_overlay_for_capture(&app, &info);
        let image = info.image.clone();
        let (orig_width, orig_height) = (info.orig_width, info.orig_height);
        store_pending_capture(&app, info);

        let engine = match ocr_backend_snapshot(&app) {
            Ok(engine) => engine,
            Err(e) => {
                set_ocr_in_flight(&app, false);
                emit_ocr_outcome_if_current(&app, my_gen, Err(e));
                break;
            }
        };
        set_ocr_in_flight(&app, true);

        // 3. OCR 실행 (블로킹 — spawn_blocking 내에서 호출됨)
        let spawn_started = Instant::now();
        let ocr_result = {
            tauri::async_runtime::spawn_blocking(move || {
                run_ocr(&cfg, &engine, image.as_ref(), orig_width, orig_height)
            })
            .await
            .map_err(|e| format!("OCR 스레드 오류: {e}"))
            .and_then(|r| r)
        };
        let spawn_wait_ms = spawn_started.elapsed().as_millis();
        set_ocr_in_flight(&app, false);
        let emit_started = Instant::now();
        emit_ocr_outcome_if_current(&app, my_gen, ocr_result);
        let emit_ms = emit_started.elapsed().as_millis();
        if ocr_stage_logging_enabled() {
            eprintln!(
                "{}",
                format_ocr_app_stage_log(&OcrAppStageLog {
                    phase: "capture_hotkey",
                    capture_ms,
                    spawn_wait_ms,
                    emit_ms,
                })
            );
            eprintln!(
                "[OCR_STAGE] app phase=capture_hotkey total_ms={}",
                flow_started.elapsed().as_millis()
            );
        }

        if !take_pending_retry(&app) {
            break;
        }
    }

    if !busy.swap(false, Ordering::SeqCst) {
        return;
    }
    emit_ocr_busy_changed(&app, false);
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

fn ocr_in_flight(app: &AppHandle) -> bool {
    app.state::<CaptureRetryState>()
        .ocr_in_flight
        .load(Ordering::SeqCst)
}

fn set_ocr_in_flight(app: &AppHandle, value: bool) {
    app.state::<CaptureRetryState>()
        .ocr_in_flight
        .store(value, Ordering::SeqCst);
}

fn set_pending_retry(app: &AppHandle, value: bool) {
    app.state::<CaptureRetryState>()
        .pending_retry
        .store(value, Ordering::SeqCst);
}

fn take_pending_retry(app: &AppHandle) -> bool {
    app.state::<CaptureRetryState>()
        .pending_retry
        .swap(false, Ordering::SeqCst)
}

#[cfg(test)]
fn resolve_paddle_model_dir_with_roots<I>(roots: I) -> Option<PathBuf>
where
    I: IntoIterator<Item = PathBuf>,
{
    roots
        .into_iter()
        .find(|root| has_required_paddle_model_files(root))
}

#[cfg(test)]
fn has_required_paddle_model_files(dir: &PathBuf) -> bool {
    has_stem_files(dir, "det") && has_stem_files(dir, "cls") && has_stem_files(dir, "rec")
}

#[cfg(test)]
fn find_named_submodel_dir(model_root: &PathBuf, stem: &str) -> Option<PathBuf> {
    use std::fs;
    use std::path::PathBuf;

    let stem_aliases: &[&str] = match stem {
        "det" => &[
            "det",
            "textdet",
            "text_det",
            "detection",
            "textdetv",
            "text_detection",
        ],
        "rec" => &[
            "rec",
            "textrec",
            "text_rec",
            "recognition",
            "textrecg",
            "text_recog",
        ],
        "cls" => &[
            "cls",
            "textcls",
            "textline",
            "orientation",
            "angle",
            "textorientation",
        ],
        _ => &[],
    };

    let entries = match fs::read_dir(model_root) {
        Ok(entries) => entries,
        Err(_) => return None,
    };
    let mut candidates = Vec::<PathBuf>::new();

    for entry in entries.flatten() {
        let candidate = entry.path();
        if !candidate.is_dir() {
            continue;
        }
        let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
        let is_match = stem_aliases.iter().any(|alias| name.contains(*alias));
        if !is_match {
            continue;
        }
        candidates.push(candidate);
    }

    candidates.sort_by(|a, b| {
        let an = a
            .file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        let bn = b
            .file_name()
            .map(|name| name.to_string_lossy().to_ascii_lowercase())
            .unwrap_or_default();
        an.cmp(&bn)
    });

    for candidate in candidates {
        if has_stem_files_in_dir(&candidate) {
            return Some(candidate);
        }
    }

    None
}

#[cfg(test)]
fn has_stem_files_in_dir(dir: &PathBuf) -> bool {
    let infer_json = dir.join("inference.json");
    let infer_pdmodel = dir.join("inference.pdmodel");
    let infer_params = dir.join("inference.pdiparams");
    (infer_json.is_file() && infer_params.is_file())
        || (infer_pdmodel.is_file() && infer_params.is_file())
}

#[cfg(test)]
fn has_stem_files(model_root: &PathBuf, stem: &str) -> bool {
    let direct_json = model_root.join(format!("{stem}.json"));
    let direct_params = model_root.join(format!("{stem}.pdiparams"));
    let direct_dir = model_root.join(stem);

    (direct_json.is_file() && direct_params.is_file())
        || has_stem_files_in_dir(&direct_dir)
        || find_named_submodel_dir(model_root, stem).is_some()
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

/// GPU 빌드에서는 `.env` 최초 생성 시 `OCR_SERVER_DEVICE` 기본값이 `gpu`가 되도록 치환한다.
fn default_env_example() -> std::borrow::Cow<'static, str> {
    const BASE: &str = include_str!("../.env.example");
    #[cfg(feature = "gpu")]
    {
        std::borrow::Cow::Owned(BASE.replace("OCR_SERVER_DEVICE=cpu", "OCR_SERVER_DEVICE=gpu"))
    }
    #[cfg(not(feature = "gpu"))]
    {
        std::borrow::Cow::Borrowed(BASE)
    }
}

#[cfg(any(target_os = "windows", target_os = "linux"))]
fn prepend_runtime_path_var(name: &str, dirs: impl IntoIterator<Item = PathBuf>) {
    let separator = if cfg!(target_os = "windows") {
        ";"
    } else {
        ":"
    };
    let mut entries: Vec<String> = dirs
        .into_iter()
        .filter(|dir| dir.is_dir())
        .map(|dir| dir.to_string_lossy().to_string())
        .collect();
    if entries.is_empty() {
        return;
    }

    if let Some(current) = env::var_os(name) {
        let current = current.to_string_lossy();
        if !current.is_empty() {
            entries.push(current.to_string());
        }
    }
    env::set_var(name, entries.join(separator));
}

#[cfg(target_os = "windows")]
fn prepare_gpu_runtime_search_path(app: &tauri::App, development_build: bool) {
    if !cfg!(feature = "gpu") {
        return;
    }

    let mut dirs = Vec::new();
    if development_build {
        dirs.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(".cuda"));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(parent) = exe.parent() {
            dirs.push(parent.join(".cuda"));
        }
    }
    if let Ok(resource_dir) = app.path().resource_dir() {
        dirs.push(resource_dir.join(".cuda"));
    }
    prepend_runtime_path_var("PATH", dirs);
}

#[cfg(target_os = "linux")]
fn prepare_gpu_runtime_search_path(app: &tauri::App, development_build: bool) {
    if !cfg!(feature = "gpu") {
        return;
    }

    let mut dirs = Vec::new();
    if development_build {
        dirs.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(".cuda"));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(parent) = exe.parent() {
            dirs.push(parent.join(".cuda"));
        }
    }
    if let Ok(resource_dir) = app.path().resource_dir() {
        dirs.push(resource_dir.join(".cuda"));
    }
    prepend_runtime_path_var("LD_LIBRARY_PATH", dirs);
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
fn prepare_gpu_runtime_search_path(_app: &tauri::App, _development_build: bool) {}

pub fn run() {
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
        .manage(LoadingStatusState(Mutex::new(LoadingStatusPayload {
            kind: "loading".to_string(),
            message: None,
        })))
        .manage(OcrJobGen(AtomicU64::new(0)))
        .manage(TranslationRequestSeq(AtomicU64::new(0)))
        .manage(CaptureRetryState {
            ocr_in_flight: AtomicBool::new(false),
            pending_retry: AtomicBool::new(false),
        })
        .manage(CaptureShortcutState {
            busy: busy.clone(),
            handler: capture_shortcut_handler.clone(),
        })
        .setup(move |app| {
            show_loading_window(app.handle());

            prepare_gpu_runtime_search_path(app, development_build);

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
            settings::materialize_env_file(&env_path, &default_env_example())?;
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
            let config = config;
            let paddle_model_dir = tauri::async_runtime::block_on(
                paddle_models::ensure_paddle_models_for_lang(&config.source),
            );
            let backend = match &paddle_model_dir {
                Ok(model_dir) => OcrBackend::new(&config, Some(model_dir.as_path())).map(Arc::new),
                Err(err) => Err(err.clone()),
            };
            app.manage(Arc::new(RwLock::new(backend)));
            app.manage(PaddleModelRootState(RwLock::new(paddle_model_dir.ok())));
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
                .on_menu_event(|app, event| match event.id().as_ref() {
                    "settings" => {
                        let _ = open_settings_window(app);
                    }
                    "quit" => app.exit(0),
                    _ => {}
                })
                .build(app)?;

            let capture_shortcut = config.capture_shortcut.clone();
            install_capture_shortcut(
                app.handle().clone(),
                busy.clone(),
                &capture_shortcut,
                capture_shortcut_handler.clone(),
            );

            // 백그라운드에서 OCR 백엔드를 선행 warmup한 뒤
            // 로딩 창을 숨기고 핫키 busy 플래그를 해제한다.
            let warmup_handle = app.handle().clone();
            tauri::async_runtime::spawn(async move {
                show_loading_window(&warmup_handle);
                let engine = match ocr_backend_snapshot(&warmup_handle) {
                    Ok(engine) => engine,
                    Err(e) => {
                        eprintln!("OCR 엔진 초기화 실패: {e}");
                        show_warmup_failure(&warmup_handle, &e);
                        return;
                    }
                };
                let warmup_result = tauri::async_runtime::spawn_blocking(move || engine.warmup())
                    .await
                    .map_err(|e| format!("OCR warmup 스레드 오류: {e}"))
                    .and_then(|r| r);
                match warmup_result {
                    Ok(()) => {
                        if let Some(loading) = warmup_handle.get_webview_window("loading") {
                            let _ = loading.hide();
                        }
                        set_capture_busy(&warmup_handle, false);
                    }
                    Err(e) => {
                        eprintln!("OCR warmup 실패: {e}");
                        show_warmup_failure(&warmup_handle, &e);
                    }
                }
            });

            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            select_text,
            close_overlay,
            close_popup,
            run_region_ocr,
            get_user_settings,
            save_user_settings,
            exit_app,
            get_loading_status,
            get_ocr_busy
        ])
        .run(tauri::generate_context!())
        .expect("Tauri 앱 실행 오류");
}

#[cfg(test)]
mod tests {
    #[cfg(any(target_os = "windows", target_os = "linux"))]
    use super::prepend_runtime_path_var;
    use super::{
        build_settings_notice_payload, clone_pending_capture, decide_capture_hotkey_action,
        default_env_example, format_ocr_app_stage_log, has_required_paddle_model_files,
        is_release_ocr_smoke_requested, missing_prtsc_required_setting_keys,
        missing_prtsc_required_settings, resolve_paddle_model_dir_with_roots, should_emit_ocr,
        show_ocr_device_setting, take_pending_settings_notice_slot, CaptureHotkeyAction,
        OcrAppStageLog, SettingsNoticePayload, RELEASE_OCR_SMOKE_ARG,
    };
    use crate::config::Config;
    use crate::services::CaptureInfo;
    use image::{Rgba, RgbaImage};
    use std::fs;
    use std::path::PathBuf;
    use std::sync::{Arc, Mutex};
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
            image: Arc::new(RgbaImage::from_pixel(4, 4, Rgba([1, 2, 3, 4]))),
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
    fn 오버레이가_보이는_동안_핫키를_다시_눌러도_무시한다() {
        let action = decide_capture_hotkey_action(true, false, false);

        assert_eq!(action, CaptureHotkeyAction::Ignore);
    }

    #[test]
    fn 유휴_상태에서_핫키를_누르면_즉시_캡처를_시작한다() {
        let action = decide_capture_hotkey_action(false, false, false);

        assert_eq!(action, CaptureHotkeyAction::StartNow);
    }

    #[test]
    fn 이전_ocr이_끝나는_중이면_재요청을_예약한다() {
        let action = decide_capture_hotkey_action(false, true, true);

        assert_eq!(action, CaptureHotkeyAction::QueueRetry);
    }

    #[test]
    fn warmup처럼_ocr이_아닌_busy_상태에서는_재요청을_예약하지_않는다() {
        let action = decide_capture_hotkey_action(false, true, false);

        assert_eq!(action, CaptureHotkeyAction::Ignore);
    }

    #[test]
    fn 로딩_창_크기_계약은_tauri_설정과_일치한다() {
        let config: serde_json::Value = serde_json::from_str(include_str!("../tauri.conf.json"))
            .expect("Tauri 설정 JSON 파싱 실패");
        let windows = config
            .pointer("/app/windows")
            .and_then(|value| value.as_array())
            .expect("Tauri window 설정 배열 확인 실패");
        let loading = windows
            .iter()
            .find(|window| window.get("label").and_then(|label| label.as_str()) == Some("loading"))
            .expect("loading 창 설정 확인 실패");
        assert_eq!(
            loading.get("width").and_then(|value| value.as_f64()),
            Some(300.0)
        );
        assert_eq!(
            loading.get("height").and_then(|value| value.as_f64()),
            Some(90.0)
        );
        assert_eq!(
            loading.get("resizable").and_then(|value| value.as_bool()),
            Some(false)
        );
        assert_eq!(
            loading.get("visible").and_then(|value| value.as_bool()),
            Some(false)
        );
        assert_eq!(
            loading.get("transparent").and_then(|value| value.as_bool()),
            Some(true)
        );
        assert!(loading.get("minWidth").is_none());
        assert!(loading.get("minHeight").is_none());
        assert!(loading.get("maxWidth").is_none());
        assert!(loading.get("maxHeight").is_none());
    }

    #[test]
    fn 로딩_html은_뷰포트_높이로_내용을_가운데_정렬한다() {
        let html = include_str!("../ui/src/loading.html");

        assert!(html.contains("height: 100%;"));
        assert!(html.contains("align-items: center;"));
        assert!(html.contains("justify-content: center;"));
        assert!(html.contains("background: transparent;"));
        assert!(html.contains(".panel"));
        assert!(html.contains("width: 300px;"));
        assert!(html.contains("height: 90px;"));
        assert!(!html.contains("position: fixed;"));
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
    fn 모델_루트가_없으면_필수_모델_파일_검사를_실패한다() {
        let root = temp_path("buzhidao-paddle-model-missing");
        fs::create_dir_all(&root).expect("paddle 모델 루트 생성 실패");
        assert!(!has_required_paddle_model_files(&root));

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn 구성한_모델_루트_경로_는_탐색_대상이_아님() {
        let configured_root = temp_path("buzhidao-paddle-model-configured");
        fs::create_dir_all(&configured_root).expect("configured 모델 루트 생성 실패");
        for stem in ["det", "cls", "rec"] {
            fs::write(configured_root.join(format!("{stem}.json")), b"{}")
                .expect("모델 파일 생성 실패");
            fs::write(configured_root.join(format!("{stem}.pdiparams")), b"param")
                .expect("파라미터 파일 생성 실패");
        }

        let resolved =
            resolve_paddle_model_dir_with_roots([PathBuf::from("definitely-not-a-valid-cache")]);

        assert_eq!(resolved, None);
        let _ = fs::remove_dir_all(configured_root);
    }

    #[test]
    fn paddle_모델_검색은_캐시_경로_에서_탐색한다() {
        let cache_dir = temp_path("buzhidao-paddle-cache-dir");
        fs::create_dir_all(&cache_dir).expect("cache 모델 디렉토리 생성 실패");
        for stem in ["det", "cls", "rec"] {
            let stem_dir = cache_dir.join(stem);
            fs::create_dir_all(&stem_dir).expect("stem 하위 디렉토리 생성 실패");
            fs::write(stem_dir.join("inference.pdiparams"), b"param")
                .expect("inference.pdiparams 생성 실패");
            fs::write(stem_dir.join("inference.json"), b"{}").expect("inference.json 생성 실패");
        }
        let resolved = resolve_paddle_model_dir_with_roots(vec![cache_dir.clone()]);

        assert_eq!(resolved, Some(cache_dir.clone()));

        let _ = fs::remove_dir_all(cache_dir);
    }

    #[test]
    fn paddle_모델_검색은_공식_모델_하위_폴더도_인식한다() {
        let cache_dir = temp_path("buzhidao-paddle-official-cache-dir");
        let det_dir = cache_dir.join("PP-OCRv5_server_det");
        let rec_dir = cache_dir.join("PP-OCRv5_server_rec");
        let cls_dir = cache_dir.join("PP-LCNet_x1_0_textline_ori");
        fs::create_dir_all(&det_dir).expect("det 폴더 생성 실패");
        fs::create_dir_all(&rec_dir).expect("rec 폴더 생성 실패");
        fs::create_dir_all(&cls_dir).expect("cls 폴더 생성 실패");

        for dir in [&det_dir, &rec_dir, &cls_dir] {
            fs::write(dir.join("inference.pdiparams"), b"param")
                .expect("inference.pdiparams 생성 실패");
            fs::write(dir.join("inference.json"), b"{}").expect("inference.json 생성 실패");
        }

        let resolved = resolve_paddle_model_dir_with_roots(vec![cache_dir.clone()]);

        assert_eq!(resolved, Some(cache_dir.clone()));

        let _ = fs::remove_dir_all(cache_dir);
    }

    #[test]
    fn gpu_빌드에서_env_example의_device_기본값은_gpu다() {
        let example = default_env_example();
        if cfg!(feature = "gpu") {
            assert!(
                example.contains("OCR_SERVER_DEVICE=gpu"),
                "GPU 빌드에서는 OCR_SERVER_DEVICE=gpu가 포함되어야 한다"
            );
            assert!(
                !example.contains("OCR_SERVER_DEVICE=cpu"),
                "GPU 빌드에서는 OCR_SERVER_DEVICE=cpu가 없어야 한다"
            );
        } else {
            assert!(
                example.contains("OCR_SERVER_DEVICE=cpu"),
                "CPU 빌드에서는 OCR_SERVER_DEVICE=cpu가 포함되어야 한다"
            );
        }
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
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        };
        assert_eq!(show_ocr_device_setting(&cfg), cfg!(feature = "gpu"));
    }

    #[cfg(any(target_os = "windows", target_os = "linux"))]
    #[test]
    fn 런타임_경로는_존재하는_디렉터리만_앞쪽에_추가한다() {
        let root = temp_path("buzhidao-runtime-path");
        let existing_dir = root.join("existing");
        let missing_dir = root.join("missing");
        fs::create_dir_all(&existing_dir).expect("runtime 테스트 디렉토리 생성 실패");
        let env_name = format!(
            "BUZHIDAO_TEST_RUNTIME_PATH_{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .expect("시계가 UNIX_EPOCH 이전입니다")
                .as_nanos()
        );
        std::env::set_var(&env_name, "previous");

        prepend_runtime_path_var(&env_name, [missing_dir, existing_dir.clone()]);

        let separator = if cfg!(target_os = "windows") { ";" } else { ":" };
        let expected = format!("{}{}previous", existing_dir.to_string_lossy(), separator);
        assert_eq!(std::env::var(&env_name).as_deref(), Ok(expected.as_str()));

        std::env::remove_var(&env_name);
        let _ = fs::remove_dir_all(root);
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

        assert_eq!(
            first.as_ref().map(|payload| payload.message.as_str()),
            Some("필수 항목을 입력하세요")
        );
        assert!(second.is_none());
    }

    #[test]
    fn 앱_ocr_stage_로그를_일관된_형식으로_만든다() {
        let line = format_ocr_app_stage_log(&OcrAppStageLog {
            phase: "capture_hotkey",
            capture_ms: 12,
            spawn_wait_ms: 345,
            emit_ms: 6,
        });

        assert_eq!(
            line,
            "[OCR_STAGE] app phase=capture_hotkey capture_ms=12 spawn_wait_ms=345 emit_ms=6"
        );
    }

    #[test]
    fn 릴리즈_ocr_smoke_cli_인자를_판별한다() {
        assert!(is_release_ocr_smoke_requested([
            "--ignored",
            RELEASE_OCR_SMOKE_ARG,
        ]));
        assert!(!is_release_ocr_smoke_requested(["--ignored"]));
    }
}
