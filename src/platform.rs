use crate::services::{capture_screen, CaptureInfo};
use crate::window::{focus_window, hide_window, place_overlay_window};
use rdev::{grab, Event, EventType, Key};
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

pub(crate) fn capture_active_screen() -> Result<CaptureInfo, String> {
    capture_screen()
}

pub(crate) fn prepare_overlay_for_capture(app: &AppHandle, capture: &CaptureInfo) {
    hide_window(app, "popup");

    if let Some(overlay) = app.get_webview_window("overlay") {
        show_overlay(&overlay, capture);
        focus_window(app, "overlay");
    }
}

pub(crate) fn install_capture_shortcut(
    app: AppHandle,
    busy: Arc<AtomicBool>,
    on_trigger: impl Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static,
) {
    let handle = app;
    let callback = Arc::new(on_trigger);

    std::thread::spawn(move || {
        let callback = callback.clone();
        let _ = grab(move |event: Event| {
            if is_capture_shortcut_pressed(&event) {
                if should_trigger_capture(&handle, &busy) {
                    callback(handle.clone(), busy.clone());
                }
                return None;
            }
            Some(event)
        });
    });
}

fn show_overlay(overlay: &WebviewWindow, capture: &CaptureInfo) {
    let _ = overlay.emit("overlay_show", ());
    let _ = overlay.set_ignore_cursor_events(false);
    place_overlay_window(
        overlay,
        capture.x,
        capture.y,
        capture.orig_width,
        capture.orig_height,
    );
    let _ = overlay.set_fullscreen(true);
    let _ = overlay.show();
}

fn is_capture_shortcut_pressed(event: &Event) -> bool {
    matches!(event.event_type, EventType::KeyPress(Key::PrintScreen))
}

fn should_trigger_capture(app: &AppHandle, busy: &Arc<AtomicBool>) -> bool {
    !overlay_visible(app) && !busy.load(Ordering::SeqCst)
}

fn overlay_visible(app: &AppHandle) -> bool {
    app.get_webview_window("overlay")
        .and_then(|w| w.is_visible().ok())
        .unwrap_or(false)
}

#[cfg(test)]
mod tests {
    use super::is_capture_shortcut_pressed;
    use rdev::{Event, EventType, Key};

    #[test]
    fn 캡처_단축키는_PrintScreen_누름만_감지한다() {
        let key_down = Event {
            time: std::time::SystemTime::now(),
            name: None,
            event_type: EventType::KeyPress(Key::PrintScreen),
        };
        let key_up = Event {
            time: std::time::SystemTime::now(),
            name: None,
            event_type: EventType::KeyRelease(Key::PrintScreen),
        };

        assert!(is_capture_shortcut_pressed(&key_down));
        assert!(!is_capture_shortcut_pressed(&key_up));
    }

    #[test]
    fn 캡처_단축키는_다른_키를_무시한다() {
        let event = Event {
            time: std::time::SystemTime::now(),
            name: None,
            event_type: EventType::KeyPress(Key::KeyA),
        };

        assert!(!is_capture_shortcut_pressed(&event));
    }
}
