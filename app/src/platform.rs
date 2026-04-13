use crate::services::CaptureInfo;
use crate::window::{focus_window, hide_window, place_overlay_window};
#[cfg(target_os = "linux")]
use evdev_rs::{enums::EventCode, enums::EV_KEY, Device, ReadFlag};
#[cfg(not(target_os = "linux"))]
use rdev::{grab, Event, EventType, Key};
use std::sync::atomic::AtomicBool as StdAtomicBool;
use std::sync::{
    atomic::{AtomicBool, Ordering},
    Arc,
};
use tauri::{AppHandle, Emitter, Manager, WebviewWindow};

pub(crate) fn prepare_overlay_for_capture(app: &AppHandle, capture: &CaptureInfo) {
    hide_window(app, "popup");

    if let Some(overlay) = app.get_webview_window("overlay") {
        show_overlay(app, &overlay, capture);
        focus_window(app, "overlay");
    }
}

pub(crate) fn install_capture_shortcut(
    app: AppHandle,
    busy: Arc<AtomicBool>,
    on_trigger: impl Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static,
) {
    #[cfg(target_os = "linux")]
    {
        install_linux_capture_shortcut(app, busy, on_trigger);
        return;
    }

    #[cfg(not(target_os = "linux"))]
    install_rdev_capture_shortcut(app, busy, on_trigger);
}

#[cfg(not(target_os = "linux"))]
fn install_rdev_capture_shortcut(
    app: AppHandle,
    busy: Arc<AtomicBool>,
    on_trigger: impl Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static,
) {
    let handle = app;
    let callback = Arc::new(on_trigger);
    let debug = shortcut_debug_enabled();
    let warned = Arc::new(StdAtomicBool::new(false));

    std::thread::spawn(move || {
        if debug {
            eprintln!("[단축키] 전역 키 훅 스레드 시작");
        }
        if let Err(e) = grab(move |event: Event| {
            log_shortcut_event(&event, debug);
            if is_capture_shortcut_pressed(&event) {
                if debug {
                    eprintln!("[단축키] PrtSc 입력 감지");
                }
                if should_trigger_capture(&handle, &busy) {
                    if debug {
                        eprintln!("[단축키] 캡처 처리 시작");
                    }
                    callback(handle.clone(), busy.clone());
                } else if debug && !warned.swap(true, Ordering::SeqCst) {
                    eprintln!(
                        "[단축키] PrtSc는 감지됐지만 busy=true 이거나 overlay가 이미 보여서 무시됨"
                    );
                }
                return None;
            }
            Some(event)
        }) {
            eprintln!("[단축키] 전역 키 훅 설치 실패: {e:?}");
        }
    });
}

#[cfg(target_os = "linux")]
fn install_linux_capture_shortcut(
    app: AppHandle,
    busy: Arc<AtomicBool>,
    on_trigger: impl Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static,
) {
    let callback = Arc::new(on_trigger);
    let debug = shortcut_debug_enabled();
    let warned = Arc::new(StdAtomicBool::new(false));

    std::thread::spawn(move || {
        if debug {
            eprintln!("[단축키] Linux evdev 단축키 스레드 시작");
        }

        let entries = match std::fs::read_dir("/dev/input") {
            Ok(entries) => entries,
            Err(err) => {
                eprintln!("[단축키] /dev/input 조회 실패: {err}");
                return;
            }
        };

        for entry in entries.flatten() {
            let path = entry.path();
            let Some(name) = path.file_name().and_then(|name| name.to_str()) else {
                continue;
            };
            if !name.starts_with("event") {
                continue;
            }

            let handle = app.clone();
            let busy = busy.clone();
            let callback = callback.clone();
            let warned = warned.clone();

            std::thread::spawn(move || {
                if let Err(err) =
                    watch_linux_input_device(&path, handle, busy, callback, warned, debug)
                {
                    eprintln!("[단축키] 입력 장치 감시 실패 ({}): {err}", path.display());
                }
            });
        }
    });
}

fn shortcut_debug_enabled() -> bool {
    matches!(
        std::env::var("SHORTCUT_DEBUG").ok().as_deref(),
        Some("1" | "true" | "TRUE" | "yes" | "YES")
    )
}

#[cfg(not(target_os = "linux"))]
fn log_shortcut_event(event: &Event, debug: bool) {
    if !debug {
        return;
    }

    match event.event_type {
        EventType::KeyPress(key) | EventType::KeyRelease(key) => {
            eprintln!(
                "[단축키][debug] key={key:?} type={:?} name={:?}",
                event.event_type, event.name
            );
        }
        _ => {}
    }
}

#[cfg(target_os = "linux")]
fn watch_linux_input_device(
    path: &std::path::Path,
    app: AppHandle,
    busy: Arc<AtomicBool>,
    callback: Arc<impl Fn(AppHandle, Arc<AtomicBool>) + Send + Sync + 'static>,
    warned: Arc<StdAtomicBool>,
    debug: bool,
) -> std::io::Result<()> {
    let file = std::fs::File::open(path)?;
    let mut device = Device::new().ok_or_else(|| std::io::Error::other("evdev init failed"))?;
    device.set_fd(file)?;

    if !device.has(&evdev_rs::enums::EventType::EV_KEY) {
        return Ok(());
    }

    if debug {
        let device_name = device.name().unwrap_or("<unknown>");
        eprintln!(
            "[단축키][debug] 감시 시작: {} ({device_name})",
            path.display()
        );
    }

    loop {
        let (_, event) = match device.next_event(ReadFlag::NORMAL | ReadFlag::BLOCKING) {
            Ok(event) => event,
            Err(err) => {
                return Err(std::io::Error::other(err));
            }
        };

        if debug {
            eprintln!(
                "[단축키][debug] evdev path={} code={:?} value={}",
                path.display(),
                event.event_code,
                event.value
            );
        }

        if is_linux_capture_shortcut_event(&event.event_code, event.value) {
            if debug {
                eprintln!("[단축키] PrtSc/SysRq 입력 감지");
            }
            if should_trigger_capture(&app, &busy) {
                if debug {
                    eprintln!("[단축키] 캡처 처리 시작");
                }
                callback(app.clone(), busy.clone());
            } else if debug && !warned.swap(true, Ordering::SeqCst) {
                eprintln!(
                    "[단축키] PrtSc/SysRq는 감지됐지만 busy=true 이거나 overlay가 이미 보여서 무시됨"
                );
            }
        }
    }
}

#[cfg(target_os = "linux")]
fn is_linux_capture_shortcut_event(code: &EventCode, value: i32) -> bool {
    value != 0
        && matches!(
            code,
            EventCode::EV_KEY(EV_KEY::KEY_PRINT) | EventCode::EV_KEY(EV_KEY::KEY_SYSRQ)
        )
}

fn show_overlay(_app: &AppHandle, overlay: &WebviewWindow, capture: &CaptureInfo) {
    let _ = overlay.emit("overlay_show", ());
    let _ = overlay.set_ignore_cursor_events(false);
    #[cfg(target_os = "linux")]
    {
        let monitor = app
            .available_monitors()
            .ok()
            .and_then(|mut monitors| monitors.drain(..).next())
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

#[cfg(not(target_os = "linux"))]
fn is_capture_shortcut_pressed(event: &Event) -> bool {
    match event.event_type {
        EventType::KeyPress(Key::PrintScreen) => true,
        #[cfg(target_os = "linux")]
        EventType::KeyPress(Key::Unknown(99)) => true,
        _ => false,
    }
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
    #[cfg(not(target_os = "linux"))]
    use super::is_capture_shortcut_pressed;
    #[cfg(target_os = "linux")]
    use evdev_rs::enums::{EventCode, EV_KEY};
    #[cfg(not(target_os = "linux"))]
    use rdev::{Event, EventType, Key};

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn 캡처_단축키는_print_screen_누름만_감지한다() {
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

    #[cfg(not(target_os = "linux"))]
    #[test]
    fn 캡처_단축키는_다른_키를_무시한다() {
        let event = Event {
            time: std::time::SystemTime::now(),
            name: None,
            event_type: EventType::KeyPress(Key::KeyA),
        };

        assert!(!is_capture_shortcut_pressed(&event));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn 캡처_단축키는_linux_sysrq_키코드도_감지한다() {
        assert!(super::is_linux_capture_shortcut_event(
            &EventCode::EV_KEY(EV_KEY::KEY_SYSRQ),
            1
        ));
    }
}
