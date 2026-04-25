use tauri::AppHandle;

const POPUP_W: f64 = 420.0;
const POPUP_H: f64 = 500.0;
const GAP: f64 = 12.0;

/// OCR 박스 논리 좌표 기반으로 팝업 창 위치를 계산한다.
/// 기본: 박스 우측 배치, 화면 벗어나면 좌측으로 전환.
pub(crate) fn calc_popup_pos_from_screen(
    screen_w: f64,
    screen_h: f64,
    box_x: f64,
    box_y: f64,
    box_w: f64,
) -> (f64, f64) {
    let x = if box_x + box_w + GAP + POPUP_W <= screen_w {
        box_x + box_w + GAP
    } else {
        (box_x - POPUP_W - GAP).max(0.0)
    };

    let y = if box_y + POPUP_H <= screen_h {
        box_y
    } else {
        (screen_h - POPUP_H).max(0.0)
    };

    (x, y)
}

pub(crate) fn calc_popup_pos(app: &AppHandle, box_x: f64, box_y: f64, box_w: f64) -> (f64, f64) {
    let (screen_w, screen_h) = app
        .primary_monitor()
        .ok()
        .flatten()
        .map(|m| {
            let sf = m.scale_factor();
            let sz = m.size();
            (sz.width as f64 / sf, sz.height as f64 / sf)
        })
        .unwrap_or((1920.0, 1080.0));

    calc_popup_pos_from_screen(screen_w, screen_h, box_x, box_y, box_w)
}

#[cfg(test)]
mod tests {
    use super::calc_popup_pos_from_screen;

    #[test]
    fn 오른쪽_공간이_충분하면_박스_오른쪽에_팝업을_배치한다() {
        let (x, y) = calc_popup_pos_from_screen(1920.0, 1080.0, 100.0, 200.0, 300.0);
        assert_eq!((x, y), (412.0, 200.0));
    }

    #[test]
    fn 오른쪽_공간이_부족하면_박스_왼쪽으로_이동한다() {
        let (x, y) = calc_popup_pos_from_screen(1280.0, 1080.0, 1000.0, 150.0, 200.0);
        assert_eq!((x, y), (568.0, 150.0));
    }

    #[test]
    fn 왼쪽도_부족하면_x_좌표를_0으로_고정한다() {
        let (x, y) = calc_popup_pos_from_screen(500.0, 1080.0, 200.0, 120.0, 350.0);
        assert_eq!((x, y), (0.0, 120.0));
    }

    #[test]
    fn 화면_아래를_벗어나면_y_좌표를_화면_안으로_보정한다() {
        let (x, y) = calc_popup_pos_from_screen(1920.0, 700.0, 100.0, 450.0, 200.0);
        assert_eq!((x, y), (312.0, 200.0));
    }
}
