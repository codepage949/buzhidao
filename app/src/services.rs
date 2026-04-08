use crate::config::Config;
use crate::ocr::OcrEngine;
use image::DynamicImage;
use serde::{Deserialize, Serialize};

pub(crate) type OcrDetection = (Vec<[f64; 2]>, String);
const BASE_TILE_OVERLAP: u32 = 128;
const TILE_TRIGGER_SIZE: u32 = 1400;
const DENSE_TILE_TRIGGER_SIZE: u32 = 2400;

#[derive(Serialize, Clone)]
pub(crate) struct OcrResultPayload {
    pub(crate) detections: Vec<OcrDetection>,
    pub(crate) scale: f64,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
    pub(crate) source: String,
    pub(crate) x_delta: i32,
    pub(crate) y_delta: i32,
}

pub(crate) struct CaptureInfo {
    pub(crate) image: image::DynamicImage,
    pub(crate) x: i32,
    pub(crate) y: i32,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
}

#[derive(Serialize)]
struct ChatRequest<'a> {
    model: &'a str,
    messages: Vec<ChatMessage<'a>>,
    temperature: f32,
}

#[derive(Serialize)]
struct ChatMessage<'a> {
    role: &'a str,
    content: &'a str,
}

#[derive(Deserialize)]
struct ChatResponse {
    choices: Vec<ChatChoice>,
}

#[derive(Deserialize)]
struct ChatChoice {
    message: ChatMessageContent,
}

#[derive(Deserialize)]
struct ChatMessageContent {
    content: Option<String>,
}

pub(crate) fn capture_screen() -> Result<CaptureInfo, String> {
    let screens = screenshots::Screen::all().map_err(|e| e.to_string())?;
    let screen = screens.first().ok_or("디스플레이를 찾을 수 없음")?;
    let capture = screen.capture().map_err(|e| e.to_string())?;

    let orig_width = capture.width();
    let orig_height = capture.height();

    let raw_bytes: Vec<u8> = capture.to_vec();
    let rgba_image = image::RgbaImage::from_raw(orig_width, orig_height, raw_bytes)
        .ok_or("이미지 버퍼 변환 실패")?;

    Ok(CaptureInfo {
        image: image::DynamicImage::ImageRgba8(rgba_image),
        x: screen.display_info.x,
        y: screen.display_info.y,
        orig_width,
        orig_height,
    })
}

pub(crate) fn run_ocr(
    cfg: &Config,
    engine: &OcrEngine,
    dyn_img: image::DynamicImage,
    orig_width: u32,
    orig_height: u32,
) -> Result<OcrResultPayload, String> {
    // det 단계가 자체적으로 resize_long=960 전처리를 수행하므로
    // 전체 화면을 여기서 한 번 더 축소하면 rec 단계 crop 해상도만 불필요하게 낮아진다.
    let detections = predict_with_tiles(engine, &dyn_img, cfg.score_thresh)?;

    Ok(OcrResultPayload {
        detections,
        scale: 1.0,
        orig_width,
        orig_height,
        source: cfg.source.clone(),
        x_delta: cfg.x_delta,
        y_delta: cfg.y_delta,
    })
}

fn predict_with_tiles(
    engine: &OcrEngine,
    img: &DynamicImage,
    score_thresh: f32,
) -> Result<Vec<OcrDetection>, String> {
    let mut detections = engine.predict(img, score_thresh)?;

    if img.width().max(img.height()) < TILE_TRIGGER_SIZE {
        return Ok(detections);
    }

    let tile_grid = tile_grid_for_size(img.width(), img.height());
    let tile_overlap = tile_overlap_for_grid(tile_grid);
    let tile_w = img.width().div_ceil(tile_grid);
    let tile_h = img.height().div_ceil(tile_grid);

    for row in 0..tile_grid {
        for col in 0..tile_grid {
            let x0 = col.saturating_mul(tile_w).saturating_sub(tile_overlap);
            let y0 = row.saturating_mul(tile_h).saturating_sub(tile_overlap);
            let x1 = ((col + 1) * tile_w + tile_overlap).min(img.width());
            let y1 = ((row + 1) * tile_h + tile_overlap).min(img.height());

            let tile = img.crop_imm(x0, y0, x1.saturating_sub(x0), y1.saturating_sub(y0));
            let tile_detections = engine.predict(&tile, score_thresh)?;

            for (polygon, text) in tile_detections {
                let shifted: Vec<[f64; 2]> = polygon
                    .into_iter()
                    .map(|[x, y]| [x + x0 as f64, y + y0 as f64])
                    .collect();
                detections.push((shifted, text));
            }
        }
    }

    Ok(deduplicate_detections(detections))
}

fn tile_grid_for_size(width: u32, height: u32) -> u32 {
    let longest = width.max(height);
    if longest >= DENSE_TILE_TRIGGER_SIZE {
        3
    } else {
        2
    }
}

fn tile_overlap_for_grid(tile_grid: u32) -> u32 {
    if tile_grid >= 3 {
        BASE_TILE_OVERLAP + 64
    } else {
        BASE_TILE_OVERLAP
    }
}

fn deduplicate_detections(detections: Vec<OcrDetection>) -> Vec<OcrDetection> {
    let mut deduped: Vec<OcrDetection> = Vec::new();

    'outer: for detection in detections {
        for existing in &mut deduped {
            if is_same_detection(existing, &detection) {
                if should_replace(existing, &detection) {
                    *existing = detection;
                }
                continue 'outer;
            }
        }
        deduped.push(detection);
    }

    deduped
}

fn is_same_detection(a: &OcrDetection, b: &OcrDetection) -> bool {
    let a_box = polygon_bounds(&a.0);
    let b_box = polygon_bounds(&b.0);
    let iou = bbox_iou(a_box, b_box);
    let overlap_min = bbox_overlap_min(a_box, b_box);
    let overlap_max = bbox_overlap_max(a_box, b_box);
    let center_dist = bbox_center_distance(a_box, b_box);
    let scale = bbox_diag(a_box).max(bbox_diag(b_box));
    let area_ratio = bbox_area_ratio(a_box, b_box);
    let x_overlap = bbox_axis_overlap_ratio(a_box.0, a_box.2, b_box.0, b_box.2);
    let y_overlap = bbox_axis_overlap_ratio(a_box.1, a_box.3, b_box.1, b_box.3);
    let same_line = is_same_text_line(a_box, b_box);

    if iou > 0.85 || overlap_min > 0.9 {
        return true;
    }

    if overlap_max > 0.75 && center_dist < scale * 0.2 {
        return true;
    }

    if texts_overlap_meaningfully(&a.1, &b.1)
        && same_line
        && x_overlap > 0.6
        && overlap_max > 0.28
    {
        return true;
    }

    if texts_overlap_meaningfully(&a.1, &b.1)
        && overlap_max > 0.4
        && x_overlap > 0.78
        && y_overlap > 0.82
        && center_dist < scale * 0.25
    {
        return true;
    }

    if a.1 == b.1
        && overlap_max > 0.45
        && area_ratio < 3.5
        && x_overlap > 0.8
        && y_overlap > 0.75
        && center_dist < scale * 0.3
    {
        return true;
    }

    if a.1 == b.1 && overlap_min > 0.55 && center_dist < scale * 0.35 {
        return true;
    }

    false
}

fn should_replace(current: &OcrDetection, candidate: &OcrDetection) -> bool {
    if candidate.1.len() != current.1.len() {
        return candidate.1.len() > current.1.len();
    }

    polygon_area_f64(&candidate.0) < polygon_area_f64(&current.0)
}

fn polygon_bounds(polygon: &[[f64; 2]]) -> (f64, f64, f64, f64) {
    let mut min_x = f64::MAX;
    let mut min_y = f64::MAX;
    let mut max_x = f64::MIN;
    let mut max_y = f64::MIN;

    for &[x, y] in polygon {
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }

    (min_x, min_y, max_x, max_y)
}

fn texts_overlap_meaningfully(a: &str, b: &str) -> bool {
    let a = normalize_detection_text(a);
    let b = normalize_detection_text(b);

    if a.len() < 2 || b.len() < 2 {
        return false;
    }

    a.contains(&b) || b.contains(&a)
}

fn normalize_detection_text(text: &str) -> String {
    text.chars()
        .filter(|c| !c.is_whitespace() && !matches!(c, '.' | ',' | '·' | '。' | '，' | ':' | '：'))
        .collect()
}

fn is_same_text_line(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> bool {
    let y_overlap = bbox_axis_overlap_ratio(a.1, a.3, b.1, b.3);
    let height_scale = bbox_height(a).max(bbox_height(b));
    let center_y_gap = bbox_center_gap_y(a, b);

    y_overlap > 0.72 && center_y_gap < height_scale * 0.45
}

fn bbox_iou(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let inter = bbox_intersection_area(a, b);
    if inter <= 0.0 {
        return 0.0;
    }

    let area_a = (a.2 - a.0).max(0.0) * (a.3 - a.1).max(0.0);
    let area_b = (b.2 - b.0).max(0.0) * (b.3 - b.1).max(0.0);
    let union = area_a + area_b - inter;
    if union <= 0.0 {
        0.0
    } else {
        inter / union
    }
}

fn bbox_overlap_min(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let inter = bbox_intersection_area(a, b);
    if inter <= 0.0 {
        return 0.0;
    }
    let area_a = (a.2 - a.0).max(0.0) * (a.3 - a.1).max(0.0);
    let area_b = (b.2 - b.0).max(0.0) * (b.3 - b.1).max(0.0);
    let min_area = area_a.min(area_b);
    if min_area <= 0.0 {
        0.0
    } else {
        inter / min_area
    }
}

fn bbox_overlap_max(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let inter = bbox_intersection_area(a, b);
    if inter <= 0.0 {
        return 0.0;
    }
    let max_area = bbox_area(a).max(bbox_area(b));
    if max_area <= 0.0 {
        0.0
    } else {
        inter / max_area
    }
}

fn bbox_center_distance(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let acx = (a.0 + a.2) * 0.5;
    let acy = (a.1 + a.3) * 0.5;
    let bcx = (b.0 + b.2) * 0.5;
    let bcy = (b.1 + b.3) * 0.5;
    let dx = acx - bcx;
    let dy = acy - bcy;
    (dx * dx + dy * dy).sqrt()
}

fn bbox_diag(b: (f64, f64, f64, f64)) -> f64 {
    let w = (b.2 - b.0).max(0.0);
    let h = (b.3 - b.1).max(0.0);
    (w * w + h * h).sqrt()
}

fn bbox_height(b: (f64, f64, f64, f64)) -> f64 {
    (b.3 - b.1).max(0.0)
}

fn bbox_center_gap_y(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let ay = (a.1 + a.3) * 0.5;
    let by = (b.1 + b.3) * 0.5;
    (ay - by).abs()
}

fn bbox_area(b: (f64, f64, f64, f64)) -> f64 {
    (b.2 - b.0).max(0.0) * (b.3 - b.1).max(0.0)
}

fn bbox_area_ratio(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let min_area = bbox_area(a).min(bbox_area(b));
    let max_area = bbox_area(a).max(bbox_area(b));
    if min_area <= 0.0 {
        f64::INFINITY
    } else {
        max_area / min_area
    }
}

fn bbox_axis_overlap_ratio(a0: f64, a1: f64, b0: f64, b1: f64) -> f64 {
    let inter = (a1.min(b1) - a0.max(b0)).max(0.0);
    if inter <= 0.0 {
        return 0.0;
    }

    let min_len = (a1 - a0).max(0.0).min((b1 - b0).max(0.0));
    if min_len <= 0.0 {
        0.0
    } else {
        inter / min_len
    }
}

fn bbox_intersection_area(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let inter_w = (a.2.min(b.2) - a.0.max(b.0)).max(0.0);
    let inter_h = (a.3.min(b.3) - a.1.max(b.1)).max(0.0);
    inter_w * inter_h
}

fn polygon_area_f64(polygon: &[[f64; 2]]) -> f64 {
    if polygon.len() < 3 {
        return 0.0;
    }
    let mut area = 0.0;
    for i in 0..polygon.len() {
        let j = (i + 1) % polygon.len();
        area += polygon[i][0] * polygon[j][1] - polygon[j][0] * polygon[i][1];
    }
    area.abs() / 2.0
}

pub(crate) async fn call_ai(cfg: &Config, text: &str) -> Result<String, String> {
    let client = reqwest::Client::new();
    let body = ChatRequest {
        model: &cfg.ai_gateway_model,
        messages: vec![
            ChatMessage {
                role: "system",
                content: &cfg.system_prompt,
            },
            ChatMessage {
                role: "user",
                content: text,
            },
        ],
        temperature: 0.7,
    };

    let resp = client
        .post("https://ai-gateway.vercel.sh/v1/chat/completions")
        .bearer_auth(&cfg.ai_gateway_api_key)
        .json(&body)
        .send()
        .await
        .map_err(|e| format!("AI API 요청 실패: {e}"))?;

    let chat: ChatResponse = resp
        .json()
        .await
        .map_err(|e| format!("AI 응답 파싱 실패: {e}"))?;

    chat.choices
        .into_iter()
        .next()
        .and_then(|c| c.message.content)
        .ok_or_else(|| "AI 응답이 비어 있음".to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn 중복_검출은_더_긴_텍스트를_남긴다() {
        let detections = vec![
            (
                vec![[0.0, 0.0], [10.0, 0.0], [10.0, 5.0], [0.0, 5.0]],
                "abc".to_string(),
            ),
            (
                vec![[1.0, 0.0], [10.5, 0.0], [10.5, 5.0], [1.0, 5.0]],
                "abcdef".to_string(),
            ),
        ];

        let deduped = deduplicate_detections(detections);
        assert_eq!(deduped.len(), 1);
        assert_eq!(deduped[0].1, "abcdef");
    }

    #[test]
    fn bbox_iou는_겹침을_계산한다() {
        let iou = bbox_iou((0.0, 0.0, 10.0, 10.0), (5.0, 5.0, 15.0, 15.0));
        assert!(iou > 0.1 && iou < 0.2);
    }

    #[test]
    fn 포함된_박스는_중복으로_간주한다() {
        let a = (
            vec![[0.0, 0.0], [20.0, 0.0], [20.0, 10.0], [0.0, 10.0]],
            "hello".to_string(),
        );
        let b = (
            vec![[1.0, 1.0], [19.0, 1.0], [19.0, 9.0], [1.0, 9.0]],
            "hello".to_string(),
        );

        assert!(is_same_detection(&a, &b));
    }

    #[test]
    fn 멀리_떨어진_같은_텍스트는_중복이_아니다() {
        let a = (
            vec![[0.0, 0.0], [20.0, 0.0], [20.0, 10.0], [0.0, 10.0]],
            "hello".to_string(),
        );
        let b = (
            vec![[0.0, 100.0], [20.0, 100.0], [20.0, 110.0], [0.0, 110.0]],
            "hello".to_string(),
        );

        assert!(!is_same_detection(&a, &b));
    }

    #[test]
    fn 크기만_다르고_중심이_거의_같은_박스는_중복이다() {
        let a = (
            vec![[10.0, 10.0], [70.0, 10.0], [70.0, 30.0], [10.0, 30.0]],
            "설정".to_string(),
        );
        let b = (
            vec![[16.0, 12.0], [60.0, 12.0], [60.0, 28.0], [16.0, 28.0]],
            "설정".to_string(),
        );

        assert!(is_same_detection(&a, &b));
    }

    #[test]
    fn 가까운_다른_줄은_중복이_아니다() {
        let a = (
            vec![[10.0, 10.0], [90.0, 10.0], [90.0, 28.0], [10.0, 28.0]],
            "파일".to_string(),
        );
        let b = (
            vec![[10.0, 30.0], [92.0, 30.0], [92.0, 48.0], [10.0, 48.0]],
            "파일".to_string(),
        );

        assert!(!is_same_detection(&a, &b));
    }

    #[test]
    fn 부분_문자열과_강한_중첩이_있으면_중복이다() {
        let a = (
            vec![[10.0, 10.0], [150.0, 10.0], [150.0, 32.0], [10.0, 32.0]],
            "명사 中文词典。".to_string(),
        );
        let b = (
            vec![[78.0, 11.0], [154.0, 11.0], [154.0, 31.0], [78.0, 31.0]],
            "中文词典.".to_string(),
        );

        assert!(is_same_detection(&a, &b));
    }

    #[test]
    fn 같은_줄의_우측_부분_문자열_박스는_중복이다() {
        let a = (
            vec![[10.0, 10.0], [180.0, 10.0], [180.0, 30.0], [10.0, 30.0]],
            "명사 中文词典。 中文词典.".to_string(),
        );
        let b = (
            vec![[122.0, 11.0], [181.0, 11.0], [181.0, 29.0], [122.0, 29.0]],
            "中文词典.".to_string(),
        );

        assert!(is_same_detection(&a, &b));
    }

    #[test]
    fn 큰_화면은_3x3_타일을_사용한다() {
        assert_eq!(tile_grid_for_size(2560, 1440), 3);
        assert_eq!(tile_grid_for_size(1920, 1080), 2);
    }
}
