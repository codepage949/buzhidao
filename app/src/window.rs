use tauri::{AppHandle, Manager, PhysicalPosition, PhysicalSize, WebviewWindow};

pub(crate) fn hide_window(app: &AppHandle, label: &str) {
    if let Some(window) = app.get_webview_window(label) {
        let _ = window.hide();
    }
}

pub(crate) fn focus_window(app: &AppHandle, label: &str) {
    if let Some(window) = app.get_webview_window(label) {
        let _ = window.set_focus();
    }
}

pub(crate) fn focus_active_window(app: &AppHandle) {
    let overlay_visible = window_visible(app, "overlay");
    let popup_visible = window_visible(app, "popup");

    if let Some(label) = preferred_window_to_focus(overlay_visible, popup_visible) {
        if let Some(window) = app.get_webview_window(label) {
            let _ = window.show();
            let _ = window.unminimize();
            let _ = window.set_focus();
        }
    }
}

pub(crate) fn place_overlay_window(
    window: &WebviewWindow,
    x: i32,
    y: i32,
    width: u32,
    height: u32,
) {
    let (position, size) = overlay_target_bounds(x, y, width, height);
    let _ = window.set_fullscreen(false);
    let _ = window.set_position(tauri::Position::Physical(position));
    let _ = window.set_size(tauri::Size::Physical(size));
}

fn overlay_target_bounds(
    x: i32,
    y: i32,
    width: u32,
    height: u32,
) -> (PhysicalPosition<i32>, PhysicalSize<u32>) {
    (PhysicalPosition::new(x, y), PhysicalSize::new(width, height))
}

fn window_visible(app: &AppHandle, label: &str) -> bool {
    app.get_webview_window(label)
        .and_then(|window| window.is_visible().ok())
        .unwrap_or(false)
}

fn preferred_window_to_focus(overlay_visible: bool, popup_visible: bool) -> Option<&'static str> {
    if popup_visible {
        Some("popup")
    } else if overlay_visible {
        Some("overlay")
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::{overlay_target_bounds, preferred_window_to_focus};
    use tauri::{PhysicalPosition, PhysicalSize};

    #[test]
    fn 팝업이_보이면_팝업을_우선한다() {
        assert_eq!(preferred_window_to_focus(true, true), Some("popup"));
    }

    #[test]
    fn 팝업이_없고_오버레이가_보이면_오버레이를_선택한다() {
        assert_eq!(preferred_window_to_focus(true, false), Some("overlay"));
    }

    #[test]
    fn 둘_다_숨김이면_포커스_대상을_선택하지_않는다() {
        assert_eq!(preferred_window_to_focus(false, false), None);
    }

    #[test]
    fn 오버레이를_캡처한_모니터_영역에_맞춘다() {
        assert_eq!(
            overlay_target_bounds(-1920, 0, 1920, 1080),
            (
                PhysicalPosition::new(-1920, 0),
                PhysicalSize::new(1920, 1080),
            )
        );
    }
}
