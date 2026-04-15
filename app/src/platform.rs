use crate::services::CaptureInfo;
use crate::window::{focus_window, hide_window, place_overlay_window};
use std::str::FromStr;
use std::sync::{
    atomic::AtomicBool,
    Arc,
};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};
use tauri_plugin_global_shortcut::{GlobalShortcutExt, Shortcut, ShortcutState};

pub(crate) type CaptureShortcutHandler =
    Arc<dyn Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static>;

pub(crate) fn prepare_overlay_for_capture(app: &AppHandle, capture: &CaptureInfo) {
    hide_window(app, "popup");

    if let Some(overlay) = app.get_webview_window("overlay") {
        show_overlay(app, &overlay, capture);
        focus_window(app, "overlay");
    }
}

pub(crate) fn show_overlay_notice(app: &AppHandle, event_name: &str, message: &str) {
    hide_window(app, "popup");

    if let Some(overlay) = app.get_webview_window("overlay") {
        let _ = overlay.emit(event_name, message);
        let _ = overlay.set_ignore_cursor_events(false);
        let _ = overlay.show();
        let _ = overlay.set_focus();
    }
}

/// 플러그인으로 전역 단축키를 등록한다. 등록 실패 시 에러 로그만 남기고 앱은 계속 구동된다.
pub(crate) fn install_capture_shortcut(
    app: AppHandle,
    busy: Arc<AtomicBool>,
    accelerator: &str,
    on_trigger: CaptureShortcutHandler,
) {
    if let Err(err) = register_capture_shortcut(&app, busy, accelerator, on_trigger) {
        eprintln!("{err}");
    }
}

pub(crate) fn replace_capture_shortcut(
    app: &AppHandle,
    busy: Arc<AtomicBool>,
    current_accelerator: &str,
    next_accelerator: &str,
    on_trigger: CaptureShortcutHandler,
) -> Result<(), String> {
    if current_accelerator == next_accelerator {
        return Ok(());
    }

    let current_shortcut = Shortcut::from_str(current_accelerator).map_err(|err| {
        format!(
            "현재 캡처 단축키를 해제할 수 없습니다: {current_accelerator} ({err:?})"
        )
    })?;

    app.global_shortcut()
        .unregister(current_shortcut)
        .map_err(|err| {
            format!(
                "기존 캡처 단축키 해제 실패: {current_accelerator} ({err:?})"
            )
        })?;

    match register_capture_shortcut(app, busy.clone(), next_accelerator, on_trigger.clone()) {
        Ok(()) => Ok(()),
        Err(register_err) => {
            let rollback_result =
                register_capture_shortcut(app, busy, current_accelerator, on_trigger);
            match rollback_result {
                Ok(()) => Err(format!(
                    "새 캡처 단축키 등록 실패: {register_err}. 기존 단축키로 복구했습니다."
                )),
                Err(rollback_err) => Err(format!(
                    "새 캡처 단축키 등록 실패: {register_err}. 기존 단축키 복구도 실패했습니다: {rollback_err}"
                )),
            }
        }
    }
}

fn shortcut_debug_enabled() -> bool {
    matches!(
        std::env::var("SHORTCUT_DEBUG").ok().as_deref(),
        Some("1" | "true" | "TRUE" | "yes" | "YES")
    )
}

fn register_capture_shortcut(
    app: &AppHandle,
    busy: Arc<AtomicBool>,
    accelerator: &str,
    on_trigger: CaptureShortcutHandler,
) -> Result<(), String> {
    let debug = shortcut_debug_enabled();
    let shortcut = Shortcut::from_str(accelerator).map_err(|err| {
        format!(
            "[단축키] 단축키 파싱 실패: accelerator={accelerator:?} err={err:?} — 캡처 단축키가 동작하지 않습니다"
        )
    })?;

    let callback = on_trigger;
    let handler_shortcut = shortcut;
    let handler_busy = busy.clone();
    let handler_app = app.clone();
    let accelerator_owned = accelerator.to_string();
    let accelerator_for_handler = accelerator_owned.clone();

    app.global_shortcut()
        .on_shortcut(shortcut, move |_app, received, event| {
            if received != &handler_shortcut {
                return;
            }
            if event.state() != ShortcutState::Pressed {
                return;
            }
            if debug {
                eprintln!("[단축키] {accelerator_for_handler} 감지");
            }
            if overlay_visible(&handler_app) {
                if debug {
                    eprintln!("[단축키] overlay가 이미 보여서 무시됨");
                }
                return;
            }
            let app = handler_app.clone();
            let busy = handler_busy.clone();
            let callback = callback.clone();
            tauri::async_runtime::spawn(async move {
                callback(app, busy);
            });
        })
        .map_err(|err| {
            format!(
                "[단축키] 단축키 등록 실패: accelerator={accelerator_owned:?} err={err:?}\n\
                 ├─ Linux Wayland에서는 전역 단축키가 차단될 수 있습니다.\n\
                 └─ 다른 앱이 이미 같은 단축키를 선점했을 수 있습니다."
            )
        })?;

    if debug {
        eprintln!("[단축키] {accelerator_owned} 등록 완료");
    }

    Ok(())
}

fn show_overlay(app: &AppHandle, overlay: &WebviewWindow, capture: &CaptureInfo) {
    let _ = overlay.emit("overlay_show", ());
    let _ = overlay.set_ignore_cursor_events(false);
    #[cfg(not(target_os = "linux"))]
    let _ = app;
    #[cfg(target_os = "linux")]
    {
        let monitor = app
            .available_monitors()
            .ok()
            .and_then(|monitors| monitors.into_iter().next())
            .or_else(|| app.primary_monitor().ok().flatten());

        if let Some(monitor) = monitor {
            let position = monitor.position();
            let size = monitor.size();
            let _ = overlay.set_fullscreen(false);
            let _ = overlay.set_position(tauri::Position::Physical(tauri::PhysicalPosition::new(
                position.x, position.y,
            )));
            let _ = overlay.set_size(tauri::Size::Physical(tauri::PhysicalSize::new(
                size.width,
                size.height,
            )));
        } else {
            place_overlay_window(
                overlay,
                capture.x,
                capture.y,
                capture.orig_width,
                capture.orig_height,
            );
        }
    }
    #[cfg(not(target_os = "linux"))]
    place_overlay_window(
        overlay,
        capture.x,
        capture.y,
        capture.orig_width,
        capture.orig_height,
    );
    let _ = overlay.show();
    #[cfg(not(target_os = "linux"))]
    let _ = overlay.set_fullscreen(true);
}

pub(crate) fn overlay_visible(app: &AppHandle) -> bool {
    app.get_webview_window("overlay")
        .and_then(|w| w.is_visible().ok())
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::str::FromStr;

    #[test]
    fn 기본_capture_shortcut_accelerator는_파싱된다() {
        assert!(Shortcut::from_str(crate::config::default_capture_shortcut()).is_ok());
    }

    #[test]
    fn 사용자_지정_accelerator_예시가_파싱된다() {
        for acc in ["Ctrl+Alt+A", "Ctrl+Shift+Space", "Cmd+Shift+A", "Alt+F4"] {
            assert!(Shortcut::from_str(acc).is_ok(), "파싱 실패: {acc}");
        }
    }
}
