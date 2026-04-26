use crate::config::Config;
use crate::ocr::{ocr_stage_logging_enabled, OcrBackend};
use crate::services::capture::CaptureInfo;
use image::imageops::FilterType;
use serde::Serialize;

#[derive(Serialize, Clone, Debug, PartialEq)]
pub(crate) struct OcrBoundsPayload {
    pub(crate) x: f64,
    pub(crate) y: f64,
    pub(crate) width: f64,
    pub(crate) height: f64,
}

#[derive(Serialize, Clone, Debug, PartialEq)]
pub(crate) struct OcrGroupPayload {
    pub(crate) text: String,
    pub(crate) bounds: OcrBoundsPayload,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub(crate) members: Vec<String>,
}

pub(crate) type OcrDetection = (OcrBoundsPayload, String);
pub(crate) type OcrDebugDetection = (OcrBoundsPayload, String, f32, bool);
const SCREENSHOT_MAX_WIDTH_BEFORE_OCR: u32 = 1024;
const SCREENSHOT_MAX_LONG_SIDE_BEFORE_OCR: u32 = 1024;
const SCREENSHOT_RESIZE_FILTER_BEFORE_OCR: FilterType = FilterType::Triangle;

fn is_false(value: &bool) -> bool {
    !*value
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum ScreenshotResizeMode {
    Width,
    LongSide,
}

impl ScreenshotResizeMode {
    fn from_env() -> Self {
        match std::env::var("BUZHIDAO_APP_OCR_RESIZE_MODE")
            .ok()
            .as_deref()
        {
            Some("width") => Self::Width,
            _ => Self::LongSide,
        }
    }
}

#[derive(Serialize, Clone)]
pub(crate) struct OcrResultPayload {
    #[serde(skip_serializing)]
    pub(crate) detections: Vec<OcrDetection>,
    pub(crate) groups: Vec<OcrGroupPayload>,
    #[serde(skip_serializing_if = "Vec::is_empty")]
    pub(crate) debug_detections: Vec<OcrDebugDetection>,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
    #[serde(skip_serializing)]
    pub(crate) source: String,
    #[serde(skip_serializing)]
    pub(crate) word_gap: i32,
    #[serde(skip_serializing)]
    pub(crate) line_gap: i32,
    #[serde(skip_serializing_if = "is_false")]
    pub(crate) debug_trace: bool,
}

pub(crate) fn crop_capture_to_region(
    capture: CaptureInfo,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(image::RgbaImage, f64, f64, u32, u32), String> {
    let (crop_x, crop_y, crop_w, crop_h) = selection_rect_to_image_rect(
        capture.orig_width,
        capture.orig_height,
        rect_x,
        rect_y,
        rect_w,
        rect_h,
        viewport_w,
        viewport_h,
    )?;

    let cropped = image::imageops::crop_imm(capture.image.as_ref(), crop_x, crop_y, crop_w, crop_h)
        .to_image();
    Ok((
        cropped,
        crop_x as f64,
        crop_y as f64,
        capture.orig_width,
        capture.orig_height,
    ))
}

fn for_each_bounds(payload: &mut OcrResultPayload, mut f: impl FnMut(&mut OcrBoundsPayload)) {
    for (bounds, _) in &mut payload.detections {
        f(bounds);
    }
    for group in &mut payload.groups {
        f(&mut group.bounds);
    }
    for (bounds, _, _, _) in &mut payload.debug_detections {
        f(bounds);
    }
}

#[derive(Clone)]
struct GroupItem {
    text: String,
    bounds: OcrBoundsPayload,
}

#[derive(Clone)]
struct GroupState {
    text: String,
    bounds: OcrBoundsPayload,
    members: Vec<GroupItem>,
}

fn area(bounds: &OcrBoundsPayload) -> f64 {
    bounds.width * bounds.height
}

fn intersection_area(a: &OcrBoundsPayload, b: &OcrBoundsPayload) -> f64 {
    let x0 = a.x.max(b.x);
    let y0 = a.y.max(b.y);
    let x1 = (a.x + a.width).min(b.x + b.width);
    let y1 = (a.y + a.height).min(b.y + b.height);
    if x1 <= x0 || y1 <= y0 {
        return 0.0;
    }
    (x1 - x0) * (y1 - y0)
}

fn overlap_ratio_of_smaller(a: &OcrBoundsPayload, b: &OcrBoundsPayload) -> f64 {
    let inter = intersection_area(a, b);
    if inter <= 0.0 {
        return 0.0;
    }
    inter / area(a).min(area(b)).max(1.0)
}

fn horizontal_gap(a: &OcrBoundsPayload, b: &OcrBoundsPayload) -> f64 {
    let a_right = a.x + a.width;
    let b_right = b.x + b.width;
    if a_right < b.x {
        return b.x - a_right;
    }
    if b_right < a.x {
        return a.x - b_right;
    }
    0.0
}

fn vertical_center_distance(a: &OcrBoundsPayload, b: &OcrBoundsPayload) -> f64 {
    (a.y + a.height / 2.0 - (b.y + b.height / 2.0)).abs()
}

fn merge_bounds(a: &OcrBoundsPayload, b: &OcrBoundsPayload) -> OcrBoundsPayload {
    let x = a.x.min(b.x);
    let y = a.y.min(b.y);
    OcrBoundsPayload {
        x,
        y,
        width: (a.x + a.width).max(b.x + b.width) - x,
        height: (a.y + a.height).max(b.y + b.height) - y,
    }
}

fn merge_candidate_score(group: &OcrBoundsPayload, item: &OcrBoundsPayload) -> f64 {
    vertical_center_distance(group, item) * 10000.0 + horizontal_gap(group, item)
}

fn join_text(a: &str, b: &str, source: &str) -> String {
    if a == b {
        return a.to_string();
    }
    if source == "en" {
        format!("{a} {b}")
    } else {
        format!("{a}{b}")
    }
}

fn can_merge(group: &OcrBoundsPayload, item: &OcrBoundsPayload, x_gap: i32, y_gap: i32) -> bool {
    let group_bottom = group.y + group.height;
    let group_right = group.x + group.width;
    let item_right = item.x + item.width;
    let max_height = group.height.max(item.height);
    let adaptive_x_gap = (x_gap as f64).max(group.height.min(item.height) * 1.2);
    let x_near = item.x <= group_right + adaptive_x_gap && item_right >= group.x - adaptive_x_gap;
    let y_overlap = item.y < group_bottom && item.y + item.height > group.y;
    let same_line_by_center = vertical_center_distance(group, item) <= max_height * 0.6;
    let y_adjacent = item.y >= group_bottom && item.y <= group_bottom + y_gap as f64;
    let same_line_gap_merge = same_line_by_center && horizontal_gap(group, item) <= adaptive_x_gap;
    (x_near && (y_overlap || y_adjacent)) || same_line_gap_merge
}

fn is_nested_duplicate_item(group: &GroupState, item: &GroupItem) -> bool {
    group.members.iter().any(|member| {
        let overlap = overlap_ratio_of_smaller(&member.bounds, &item.bounds);
        overlap >= 0.9 && (member.text.contains(&item.text) || item.text.contains(&member.text))
    })
}

fn sort_items_by_reading_order(items: &mut [GroupItem]) {
    items.sort_by(|a, b| {
        a.bounds
            .y
            .partial_cmp(&b.bounds.y)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| {
                a.bounds
                    .x
                    .partial_cmp(&b.bounds.x)
                    .unwrap_or(std::cmp::Ordering::Equal)
            })
    });
}

fn build_group_text_from_sorted_members(members: &[GroupItem], source: &str) -> String {
    let mut iter = members.iter();
    let Some(first) = iter.next() else {
        return String::new();
    };
    let mut text = first.text.clone();
    for member in iter {
        text = join_text(&text, &member.text, source);
    }
    text
}

fn deduplicate_groups(groups: Vec<GroupState>) -> Vec<GroupState> {
    let mut sorted = groups;
    sorted.sort_by(|a, b| {
        area(&b.bounds)
            .partial_cmp(&area(&a.bounds))
            .unwrap_or(std::cmp::Ordering::Equal)
    });
    let mut result: Vec<GroupState> = Vec::new();
    'outer: for group in sorted {
        for kept in &result {
            let overlap = overlap_ratio_of_smaller(&group.bounds, &kept.bounds);
            if overlap >= 0.9 && (kept.text.contains(&group.text) || group.text.contains(&kept.text))
            {
                continue 'outer;
            }
        }
        result.push(group);
    }
    result.sort_by(|a, b| {
        a.bounds
            .y
            .partial_cmp(&b.bounds.y)
            .unwrap_or(std::cmp::Ordering::Equal)
            .then_with(|| {
                a.bounds
                    .x
                    .partial_cmp(&b.bounds.x)
                    .unwrap_or(std::cmp::Ordering::Equal)
            })
    });
    result
}

fn build_overlay_groups(
    detections: &[OcrDetection],
    source: &str,
    x_gap: i32,
    y_gap: i32,
    include_members: bool,
) -> Vec<OcrGroupPayload> {
    let mut items: Vec<GroupItem> = detections
        .iter()
        .filter_map(|(bounds, raw_text)| {
            let text = raw_text.trim();
            if text.is_empty() {
                return None;
            }
            Some(GroupItem {
                text: text.to_string(),
                bounds: bounds.clone(),
            })
        })
        .collect();
    sort_items_by_reading_order(&mut items);

    let mut groups: Vec<GroupState> = Vec::new();
    for item in items {
        let mut idx = None;
        let mut best_score = f64::INFINITY;
        let mut duplicate_hit = false;
        for (group_index, group) in groups.iter().enumerate() {
            if !can_merge(&group.bounds, &item.bounds, x_gap, y_gap) {
                continue;
            }
            if is_nested_duplicate_item(group, &item) {
                duplicate_hit = true;
                break;
            }
            let score = merge_candidate_score(&group.bounds, &item.bounds);
            if score < best_score {
                best_score = score;
                idx = Some(group_index);
            }
        }
        if duplicate_hit {
            continue;
        }
        if let Some(index) = idx {
            let group = &mut groups[index];
            group.text = join_text(&group.text, &item.text, source);
            group.bounds = merge_bounds(&group.bounds, &item.bounds);
            group.members.push(item);
        } else {
            groups.push(GroupState {
                text: item.text.clone(),
                bounds: item.bounds.clone(),
                members: vec![item],
            });
        }
    }

    deduplicate_groups(groups)
        .into_iter()
        .map(|mut group| {
            sort_items_by_reading_order(&mut group.members);
            OcrGroupPayload {
                text: build_group_text_from_sorted_members(&group.members, source),
                bounds: group.bounds,
                members: if include_members {
                    group.members.into_iter().map(|member| member.text).collect()
                } else {
                    Vec::new()
                },
            }
        })
        .collect()
}

pub(crate) fn offset_ocr_result(payload: &mut OcrResultPayload, offset_x: f64, offset_y: f64) {
    for_each_bounds(payload, |bounds| {
        bounds.x += offset_x;
        bounds.y += offset_y;
    });
}

pub(crate) fn scale_ocr_result(payload: &mut OcrResultPayload, scale_x: f64, scale_y: f64) {
    for_each_bounds(payload, |bounds| {
        bounds.x *= scale_x;
        bounds.y *= scale_y;
        bounds.width *= scale_x;
        bounds.height *= scale_y;
    });
}

fn resize_image_to_fit(
    dyn_img: &image::RgbaImage,
    max_width: u32,
    max_height: u32,
) -> (std::borrow::Cow<'_, image::RgbaImage>, f64, f64) {
    let width_ratio = if max_width > 0 && dyn_img.width() > max_width {
        max_width as f64 / dyn_img.width() as f64
    } else {
        1.0
    };
    let height_ratio = if max_height > 0 && dyn_img.height() > max_height {
        max_height as f64 / dyn_img.height() as f64
    } else {
        1.0
    };
    let ratio = width_ratio.min(height_ratio);
    if ratio >= 1.0 {
        return (std::borrow::Cow::Borrowed(dyn_img), 1.0, 1.0);
    }

    let target_width = ((dyn_img.width() as f64) * ratio).round().max(1.0) as u32;
    let target_height = ((dyn_img.height() as f64) * ratio).round().max(1.0) as u32;
    let resized = image::imageops::resize(
        dyn_img,
        target_width,
        target_height,
        SCREENSHOT_RESIZE_FILTER_BEFORE_OCR,
    );
    let scale_x = dyn_img.width() as f64 / resized.width() as f64;
    let scale_y = dyn_img.height() as f64 / resized.height() as f64;
    (std::borrow::Cow::Owned(resized), scale_x, scale_y)
}

fn selection_rect_to_image_rect(
    image_width: u32,
    image_height: u32,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(u32, u32, u32, u32), String> {
    if image_width == 0 || image_height == 0 {
        return Err("캡처 이미지 크기가 0입니다".to_string());
    }
    if viewport_w <= 0.0 || viewport_h <= 0.0 {
        return Err("뷰포트 크기가 유효하지 않습니다".to_string());
    }

    let x1 = rect_x.min(rect_x + rect_w).max(0.0).min(viewport_w);
    let y1 = rect_y.min(rect_y + rect_h).max(0.0).min(viewport_h);
    let x2 = rect_x.max(rect_x + rect_w).max(0.0).min(viewport_w);
    let y2 = rect_y.max(rect_y + rect_h).max(0.0).min(viewport_h);

    let scale_x = image_width as f64 / viewport_w;
    let scale_y = image_height as f64 / viewport_h;
    let crop_x = (x1 * scale_x).floor() as u32;
    let crop_y = (y1 * scale_y).floor() as u32;
    let mut crop_w = ((x2 - x1) * scale_x).ceil() as u32;
    let mut crop_h = ((y2 - y1) * scale_y).ceil() as u32;

    crop_w = crop_w.max(1).min(image_width.saturating_sub(crop_x));
    crop_h = crop_h.max(1).min(image_height.saturating_sub(crop_y));

    Ok((crop_x, crop_y, crop_w, crop_h))
}

fn compute_resize_limits(
    image_width: u32,
    image_height: u32,
    backend_max_width: u32,
    mode: ScreenshotResizeMode,
) -> (u32, u32) {
    match mode {
        ScreenshotResizeMode::Width => {
            let max_width = match (backend_max_width > 0, SCREENSHOT_MAX_WIDTH_BEFORE_OCR > 0) {
                (true, true) => backend_max_width.min(SCREENSHOT_MAX_WIDTH_BEFORE_OCR),
                (true, false) => backend_max_width,
                (false, true) => SCREENSHOT_MAX_WIDTH_BEFORE_OCR,
                (false, false) => 0,
            };
            (max_width, 0)
        }
        ScreenshotResizeMode::LongSide => {
            if image_width >= image_height {
                let max_width = match (backend_max_width > 0, SCREENSHOT_MAX_LONG_SIDE_BEFORE_OCR > 0)
                {
                    (true, true) => backend_max_width.min(SCREENSHOT_MAX_LONG_SIDE_BEFORE_OCR),
                    (true, false) => backend_max_width,
                    (false, true) => SCREENSHOT_MAX_LONG_SIDE_BEFORE_OCR,
                    (false, false) => 0,
                };
                (max_width, 0)
            } else {
                (0, SCREENSHOT_MAX_LONG_SIDE_BEFORE_OCR)
            }
        }
    }
}

pub(crate) fn run_ocr(
    cfg: &Config,
    engine: &OcrBackend,
    dyn_img: &image::RgbaImage,
    orig_width: u32,
    orig_height: u32,
) -> Result<OcrResultPayload, String> {
    let backend_max_width = engine.resize_width_before_ocr();
    let resize_mode = ScreenshotResizeMode::from_env();
    let (max_width, max_height) = compute_resize_limits(
        dyn_img.width(),
        dyn_img.height(),
        backend_max_width,
        resize_mode,
    );
    let (prepared_img, scale_x, scale_y) = resize_image_to_fit(dyn_img, max_width, max_height);
    let prepared_ref = prepared_img.as_ref();
    let t0 = std::time::Instant::now();
    let (detections, debug_detections) = engine.run_image(
        prepared_ref,
        &cfg.source,
        cfg.score_thresh,
        cfg.ocr_debug_trace,
    )?;
    if ocr_stage_logging_enabled() {
        eprintln!(
            "[OCR] backend 전체 처리: {:.0}ms ({}×{}, resize_width {})",
            t0.elapsed().as_millis(),
            prepared_ref.width(),
            prepared_ref.height(),
            if max_width > 0 { max_width } else { max_height }
        );
    }

    let mut payload = OcrResultPayload {
        detections,
        groups: Vec::new(),
        debug_detections,
        orig_width,
        orig_height,
        source: cfg.source.clone(),
        word_gap: cfg.word_gap,
        line_gap: cfg.line_gap,
        debug_trace: cfg.ocr_debug_trace,
    };

    if (scale_x - 1.0).abs() > f64::EPSILON || (scale_y - 1.0).abs() > f64::EPSILON {
        scale_ocr_result(&mut payload, scale_x, scale_y);
    }
    payload.groups = build_overlay_groups(
        &payload.detections,
        &payload.source,
        payload.word_gap,
        payload.line_gap,
        payload.debug_trace,
    );

    Ok(payload)
}

#[cfg(test)]
mod tests {
    use super::{
        build_overlay_groups, compute_resize_limits, offset_ocr_result, resize_image_to_fit,
        run_ocr, scale_ocr_result, selection_rect_to_image_rect, OcrBoundsPayload,
        OcrGroupPayload,
        OcrResultPayload, ScreenshotResizeMode,
    };
    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    use crate::{config::Config, ocr::OcrBackend};
    use image::{Rgba, RgbaImage};
    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    use std::path::PathBuf;

    #[test]
    fn 선택_영역을_원본_이미지_좌표로_변환한다() {
        let rect =
            selection_rect_to_image_rect(2560, 1440, 100.0, 50.0, 300.0, 200.0, 1280.0, 720.0)
                .expect("좌표 변환 실패");

        assert_eq!(rect, (200, 100, 600, 400));
    }

    #[test]
    fn ocr_결과에_crop_오프셋을_더한다() {
        let mut payload = OcrResultPayload {
            detections: vec![(
                OcrBoundsPayload {
                    x: 10.0,
                    y: 20.0,
                    width: 20.0,
                    height: 20.0,
                },
                "text".to_string(),
            )],
            groups: vec![],
            debug_detections: vec![(
                OcrBoundsPayload {
                    x: 1.0,
                    y: 2.0,
                    width: 2.0,
                    height: 2.0,
                },
                "dbg".to_string(),
                0.9,
                true,
            )],
            orig_width: 100,
            orig_height: 100,
            source: "en".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        offset_ocr_result(&mut payload, 5.0, 7.0);

        assert_eq!(payload.detections[0].0.x, 15.0);
        assert_eq!(payload.detections[0].0.y, 27.0);
        assert_eq!(payload.debug_detections[0].0.x, 6.0);
        assert_eq!(payload.debug_detections[0].0.y, 9.0);
    }

    #[test]
    fn 넓은_이미지는_1024w로_축소하고_배율을_반환한다() {
        let img = RgbaImage::from_pixel(2048, 1024, Rgba([1, 2, 3, 4]));

        let (resized, scale_x, scale_y) = resize_image_to_fit(&img, 1024, 0);

        assert_eq!(resized.width(), 1024);
        assert_eq!(resized.height(), 512);
        assert!((scale_x - 2.0).abs() < f64::EPSILON);
        assert!((scale_y - 2.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 선택_영역이_이미지_크기_0이면_실패한다() {
        let err = selection_rect_to_image_rect(0, 100, 0.0, 0.0, 10.0, 10.0, 10.0, 10.0)
            .expect_err("이미지 크기 0이면 실패해야 한다");
        assert!(err.contains("캡처 이미지 크기"));
    }

    #[test]
    fn 선택_영역이_뷰포트_크기_0_이하이면_실패한다() {
        let err = selection_rect_to_image_rect(100, 100, 0.0, 0.0, 10.0, 10.0, 0.0, 10.0)
            .expect_err("뷰포트 크기 0이면 실패해야 한다");
        assert!(err.contains("뷰포트"));
    }

    #[test]
    fn 음수_크기의_선택_영역은_좌상단으로_정규화된다() {
        let rect = selection_rect_to_image_rect(100, 100, 80.0, 60.0, -40.0, -30.0, 100.0, 100.0)
            .expect("음수 선택 영역 처리 실패");
        assert_eq!(rect, (40, 30, 40, 30));
    }

    #[test]
    fn 뷰포트_밖_선택은_이미지_범위_안으로_클램프된다() {
        let rect = selection_rect_to_image_rect(200, 200, -50.0, -50.0, 400.0, 400.0, 100.0, 100.0)
            .expect("클램프 실패");
        assert_eq!(rect, (0, 0, 200, 200));
    }

    #[test]
    fn 이미지_폭이_최대_이하이면_원본을_유지한다() {
        let img = RgbaImage::from_pixel(512, 256, Rgba([9, 9, 9, 9]));
        let (out, sx, sy) = resize_image_to_fit(&img, 1024, 0);
        assert_eq!(out.width(), 512);
        assert_eq!(out.height(), 256);
        assert!((sx - 1.0).abs() < f64::EPSILON);
        assert!((sy - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 최대_폭이_0이면_이미지를_그대로_반환한다() {
        let img = RgbaImage::from_pixel(2000, 1000, Rgba([0, 0, 0, 0]));
        let (out, sx, sy) = resize_image_to_fit(&img, 0, 0);
        assert_eq!(out.width(), 2000);
        assert!((sx - 1.0).abs() < f64::EPSILON);
        assert!((sy - 1.0).abs() < f64::EPSILON);
    }

    #[test]
    fn 높은_이미지도_폭_기준으로만_축소한다() {
        let img = RgbaImage::from_pixel(1600, 3200, Rgba([5, 6, 7, 8]));

        let (resized, scale_x, scale_y) = resize_image_to_fit(&img, 1024, 0);

        assert_eq!(resized.width(), 1024);
        assert_eq!(resized.height(), 2048);
        assert!((scale_x - 1.5625).abs() < f64::EPSILON);
        assert!((scale_y - 1.5625).abs() < f64::EPSILON);
    }

    #[test]
    fn long_side_모드에서_가로가_긴_이미지는_1024w를_쓴다() {
        let limits = compute_resize_limits(2559, 1304, 0, ScreenshotResizeMode::LongSide);
        assert_eq!(limits, (1024, 0));
    }

    #[test]
    fn long_side_모드에서_세로가_긴_이미지는_1024h를_쓴다() {
        let limits = compute_resize_limits(1304, 2559, 0, ScreenshotResizeMode::LongSide);
        assert_eq!(limits, (0, 1024));
    }

    #[test]
    fn 축소된_ocr_좌표를_원본_배율로_복원한다() {
        let mut payload = OcrResultPayload {
            detections: vec![(
                OcrBoundsPayload {
                    x: 10.0,
                    y: 20.0,
                    width: 20.0,
                    height: 20.0,
                },
                "text".to_string(),
            )],
            groups: vec![],
            debug_detections: vec![(
                OcrBoundsPayload {
                    x: 1.0,
                    y: 2.0,
                    width: 2.0,
                    height: 2.0,
                },
                "dbg".to_string(),
                0.9,
                true,
            )],
            orig_width: 100,
            orig_height: 100,
            source: "en".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        scale_ocr_result(&mut payload, 2.0, 3.0);

        assert_eq!(payload.detections[0].0.x, 20.0);
        assert_eq!(payload.detections[0].0.y, 60.0);
        assert_eq!(payload.detections[0].0.width, 40.0);
        assert_eq!(payload.detections[0].0.height, 60.0);
        assert_eq!(payload.debug_detections[0].0.x, 2.0);
        assert_eq!(payload.debug_detections[0].0.y, 6.0);
        assert_eq!(payload.debug_detections[0].0.width, 4.0);
        assert_eq!(payload.debug_detections[0].0.height, 6.0);
    }

    #[test]
    fn 오버레이_그룹을_rust에서_미리_계산한다() {
        let groups = build_overlay_groups(
            &[
                (
                    OcrBoundsPayload {
                        x: 10.0,
                        y: 10.0,
                        width: 20.0,
                        height: 10.0,
                    },
                    "Hello".to_string(),
                ),
                (
                    OcrBoundsPayload {
                        x: 34.0,
                        y: 10.0,
                        width: 24.0,
                        height: 10.0,
                    },
                    "World".to_string(),
                ),
            ],
            "en",
            20,
            15,
            false,
        );

        assert_eq!(groups.len(), 1);
        assert_eq!(groups[0].text, "Hello World");
        assert_eq!(groups[0].bounds.x, 10.0);
        assert_eq!(groups[0].bounds.width, 48.0);
        assert!(groups[0].members.is_empty());
    }

    #[test]
    fn 디버그일때만_그룹_멤버를_직렬화용으로_보관한다() {
        let groups = build_overlay_groups(
            &[(
                OcrBoundsPayload {
                    x: 0.0,
                    y: 0.0,
                    width: 10.0,
                    height: 10.0,
                },
                "abc".to_string(),
            )],
            "en",
            20,
            15,
            true,
        );

        assert_eq!(groups[0].members, vec!["abc".to_string()]);
    }

    #[test]
    fn 오버레이_emit용_json에는_내부_필드를_싣지_않는다() {
        let payload = OcrResultPayload {
            detections: vec![(
                OcrBoundsPayload {
                    x: 1.0,
                    y: 2.0,
                    width: 3.0,
                    height: 4.0,
                },
                "hidden".to_string(),
            )],
            groups: vec![OcrGroupPayload {
                text: "hello".to_string(),
                bounds: OcrBoundsPayload {
                    x: 10.0,
                    y: 20.0,
                    width: 30.0,
                    height: 40.0,
                },
                members: Vec::new(),
            }],
            debug_detections: Vec::new(),
            orig_width: 100,
            orig_height: 50,
            source: "ch".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        let value = serde_json::to_value(&payload).expect("payload 직렬화 실패");
        let object = value.as_object().expect("payload JSON object 아님");

        assert!(object.contains_key("groups"));
        assert!(object.contains_key("orig_width"));
        assert!(object.contains_key("orig_height"));
        assert!(!object.contains_key("detections"));
        assert!(!object.contains_key("source"));
        assert!(!object.contains_key("word_gap"));
        assert!(!object.contains_key("line_gap"));
        assert!(!object.contains_key("debug_detections"));
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    fn paddle_ocr_cache_roots() -> Vec<PathBuf> {
        let mut roots = Vec::new();
        if let Some(home) = dirs::home_dir() {
            roots.push(home.join(".paddlex").join("official_models"));
            roots.push(home.join(".paddleocr"));
        }
        roots
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    fn bench_config(source: &str, score_thresh: f32) -> Config {
        Config {
            source: source.to_string(),
            score_thresh,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: String::new(),
            ai_gateway_model: String::new(),
            system_prompt: String::new(),
            word_gap: 20,
            line_gap: 15,
            capture_shortcut: "Ctrl+Alt+A".to_string(),
        }
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn _1_png를_앱_ocr_경로로_실행해서_결과를_출력한다() {
        if std::env::var("BUZHIDAO_RUN_APP_OCR_SAMPLE_TEST").unwrap_or_default() != "1" {
            eprintln!(
                "앱 OCR 샘플 테스트는 BUZHIDAO_RUN_APP_OCR_SAMPLE_TEST=1일 때만 실행합니다."
            );
            return;
        }

        let model_dir = paddle_ocr_cache_roots().into_iter().find(|path| path.exists());
        let Some(model_dir) = model_dir else {
            eprintln!("기본 PaddleOCR 캐시 경로에 모델이 없어 앱 OCR 샘플 테스트를 스킵합니다.");
            return;
        };

        let source =
            std::env::var("BUZHIDAO_APP_OCR_TEST_SOURCE").unwrap_or_else(|_| "ch".to_string());
        let score_thresh = std::env::var("BUZHIDAO_APP_OCR_TEST_SCORE_THRESH")
            .ok()
            .and_then(|raw| raw.parse::<f32>().ok())
            .unwrap_or(0.1);
        let source_image = std::env::var("BUZHIDAO_APP_OCR_TEST_IMAGE").ok().map_or_else(
            || {
                PathBuf::from(std::env::var("CARGO_MANIFEST_DIR").expect("MANIFEST_DIR 없음"))
                    .join("1.png")
            },
            PathBuf::from,
        );
        if !source_image.exists() {
            eprintln!("테스트 이미지가 없어 스킵합니다: {:?}", source_image);
            return;
        }

        let cfg = bench_config(&source, score_thresh);
        let engine =
            OcrBackend::new(&cfg, Some(model_dir.as_path())).expect("앱 OCR 엔진 생성 실패");
        engine.warmup().expect("앱 OCR warmup 실패");

        let rgba_img = image::open(&source_image)
            .expect("테스트 이미지 열기 실패")
            .into_rgba8();
        let payload = run_ocr(&cfg, &engine, &rgba_img, rgba_img.width(), rgba_img.height())
            .expect("앱 OCR 실행 실패");

        println!(
            "[APP_OCR] {}",
            serde_json::json!({
                "image": source_image,
                "detections": payload.detections,
                "debug_detections": payload.debug_detections,
                "orig_width": payload.orig_width,
                "orig_height": payload.orig_height,
                "source": payload.source,
            })
        );
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn 릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다() {
        if std::env::var("BUZHIDAO_RUN_RELEASE_OCR_SMOKE").unwrap_or_default() != "1" {
            eprintln!("릴리즈 OCR smoke는 BUZHIDAO_RUN_RELEASE_OCR_SMOKE=1일 때만 실행합니다.");
            return;
        }
        let source = std::env::var("BUZHIDAO_RELEASE_OCR_SMOKE_SOURCE")
            .unwrap_or_else(|_| "en".to_string());
        let model_dir = tauri::async_runtime::block_on(
            crate::paddle_models::ensure_paddle_models_for_lang(&source),
        )
        .expect("릴리즈 OCR smoke 모델 보장 실패");
        crate::paddle_models::validate_paddle_model_root_for_lang(&source, &model_dir)
            .expect("릴리즈 OCR smoke 모델 루트 검증 실패");
        eprintln!("릴리즈 OCR smoke 모델 루트: {}", model_dir.display());

        let mut cfg = bench_config(&source, 0.1);
        cfg.ocr_server_device =
            std::env::var("OCR_SERVER_DEVICE").unwrap_or_else(|_| "cpu".to_string());
        let engine =
            OcrBackend::new(&cfg, Some(model_dir.as_path())).expect("릴리즈 OCR smoke 엔진 생성 실패");
        engine.warmup().expect("릴리즈 OCR smoke warmup 실패");

        let image = RgbaImage::from_pixel(128, 64, Rgba([255, 255, 255, 255]));
        let payload = run_ocr(&cfg, &engine, &image, image.width(), image.height())
            .expect("릴리즈 OCR smoke 실행 실패");

        assert_eq!(payload.orig_width, 128);
        assert_eq!(payload.orig_height, 64);
        assert_eq!(payload.source, source);
    }

    #[cfg(all(feature = "paddle-ffi", has_paddle_inference))]
    #[test]
    fn 지정한_이미지들로_앱_ocr_경로_지연시간을_측정한다() {
        if std::env::var("BUZHIDAO_RUN_APP_OCR_BENCH").unwrap_or_default() != "1" {
            eprintln!("앱 OCR 벤치는 BUZHIDAO_RUN_APP_OCR_BENCH=1일 때만 실행합니다.");
            return;
        }

        let model_dir = paddle_ocr_cache_roots().into_iter().find(|path| path.exists());
        let Some(model_dir) = model_dir else {
            eprintln!("기본 PaddleOCR 캐시 경로에 모델이 없어 앱 OCR 벤치를 스킵합니다.");
            return;
        };

        let images_json = std::env::var("BUZHIDAO_APP_OCR_BENCH_IMAGES_JSON")
            .expect("BUZHIDAO_APP_OCR_BENCH_IMAGES_JSON 없음");
        let image_paths: Vec<String> =
            serde_json::from_str(&images_json).expect("이미지 목록 JSON 파싱 실패");
        let source =
            std::env::var("BUZHIDAO_APP_OCR_BENCH_SOURCE").unwrap_or_else(|_| "ch".to_string());
        let score_thresh = std::env::var("BUZHIDAO_APP_OCR_BENCH_SCORE_THRESH")
            .ok()
            .and_then(|raw| raw.parse::<f32>().ok())
            .unwrap_or(0.1);
        let warmups = std::env::var("BUZHIDAO_APP_OCR_BENCH_WARMUPS")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .unwrap_or(3);
        let iterations = std::env::var("BUZHIDAO_APP_OCR_BENCH_ITERATIONS")
            .ok()
            .and_then(|raw| raw.parse::<usize>().ok())
            .unwrap_or(10);

        let cfg = bench_config(&source, score_thresh);
        let engine =
            OcrBackend::new(&cfg, Some(model_dir.as_path())).expect("앱 OCR 엔진 생성 실패");
        engine.warmup().expect("앱 OCR warmup 실패");

        for image_path in image_paths {
            let path = PathBuf::from(&image_path);
            if !path.exists() {
                panic!("벤치 이미지가 없습니다: {}", image_path);
            }

            let rgba_img = image::open(&path).expect("벤치 이미지 열기 실패").into_rgba8();
            let (orig_width, orig_height) = (rgba_img.width(), rgba_img.height());

            for _ in 0..warmups {
                let _ = run_ocr(&cfg, &engine, &rgba_img, orig_width, orig_height)
                    .expect("앱 OCR warmup 실행 실패");
            }

            let mut elapsed_ms = Vec::with_capacity(iterations);
            let mut detection_count = 0usize;
            for _ in 0..iterations {
                let started = std::time::Instant::now();
                let payload = run_ocr(&cfg, &engine, &rgba_img, orig_width, orig_height)
                    .expect("앱 OCR 벤치 실행 실패");
                let _json = serde_json::to_string(&payload).expect("앱 OCR payload 직렬화 실패");
                detection_count = payload.detections.len();
                elapsed_ms.push(started.elapsed().as_secs_f64() * 1000.0);
            }

            println!(
                "[APP_OCR_BENCH] {}",
                serde_json::json!({
                    "image": image_path,
                    "source": source,
                    "score_thresh": score_thresh,
                    "warmups": warmups,
                    "iterations": iterations,
                    "detection_count": detection_count,
                    "elapsed_ms": elapsed_ms,
                })
            );
        }
    }
}
