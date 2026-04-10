use crate::config::Config;
use crate::ocr::det::DetBox;
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
    let t_det_full = std::time::Instant::now();
    let full_boxes = engine.detect(img)?;
    eprintln!(
        "[OCR] det 전체: {:.0}ms ({} 박스, {}×{})",
        t_det_full.elapsed().as_millis(),
        full_boxes.len(),
        img.width(),
        img.height()
    );

    if img.width().max(img.height()) < TILE_TRIGGER_SIZE {
        return engine.recognize_boxes(img, &full_boxes, score_thresh);
    }

    // 1단계: 타일에서 det 수행
    let mut tile_boxes = Vec::new();
    let tile_grid = tile_grid_for_size(img.width(), img.height());
    let tile_overlap = tile_overlap_for_grid(tile_grid);
    let tile_w = img.width().div_ceil(tile_grid);
    let tile_h = img.height().div_ceil(tile_grid);

    let t_det_tiles = std::time::Instant::now();
    for row in 0..tile_grid {
        for col in 0..tile_grid {
            let x0 = col.saturating_mul(tile_w).saturating_sub(tile_overlap);
            let y0 = row.saturating_mul(tile_h).saturating_sub(tile_overlap);
            let x1 = ((col + 1) * tile_w + tile_overlap).min(img.width());
            let y1 = ((row + 1) * tile_h + tile_overlap).min(img.height());

            let tile = img.crop_imm(x0, y0, x1.saturating_sub(x0), y1.saturating_sub(y0));
            for mut box_pts in engine.detect(&tile)? {
                for pt in &mut box_pts {
                    pt[0] += x0 as f64;
                    pt[1] += y0 as f64;
                }
                tile_boxes.push(box_pts);
            }
        }
    }
    eprintln!(
        "[OCR] det 타일 {}×{}: {:.0}ms ({} 박스)",
        tile_grid,
        tile_grid,
        t_det_tiles.elapsed().as_millis(),
        tile_boxes.len()
    );

    // 2단계: 타일 박스 우선 병합
    let unique_boxes = merge_tile_priority(full_boxes, tile_boxes);

    // 3단계: 고유 박스에 대해서만 cls+rec 실행 (원본 이미지에서 crop)
    engine.recognize_boxes(img, &unique_boxes, score_thresh)
}

fn tile_grid_for_size(width: u32, height: u32) -> u32 {
    let longest = width.max(height);
    if longest >= DENSE_TILE_TRIGGER_SIZE {
        4
    } else {
        3
    }
}

fn tile_overlap_for_grid(tile_grid: u32) -> u32 {
    if tile_grid >= 4 {
        BASE_TILE_OVERLAP + 64
    } else {
        BASE_TILE_OVERLAP
    }
}

/// 타일 박스 우선 병합.
/// 1. 타일 박스끼리 NMS (인접 타일 오버랩 중복 제거)
/// 2. 전체 이미지 박스 중 타일이 이미 커버하는 영역은 제거
/// 3. 타일이 못 잡은 영역만 전체 이미지 박스로 보충
fn merge_tile_priority(full_boxes: Vec<DetBox>, tile_boxes: Vec<DetBox>) -> Vec<DetBox> {
    // 타일끼리 NMS
    let unique_tiles = nms_boxes(tile_boxes);

    let tile_bounds: Vec<(f64, f64, f64, f64)> =
        unique_tiles.iter().map(|b| detbox_bounds(b)).collect();

    // 전체 이미지 박스 중 타일 박스가 커버하지 않는 것만 추가
    let mut result = unique_tiles;
    for full_box in full_boxes {
        let fb = detbox_bounds(&full_box);
        let covered = tile_bounds.iter().any(|&tb| {
            let inter = bbox_intersection_area(fb, tb);
            let full_area = bbox_area(fb);
            // 전체 박스의 50% 이상이 타일 박스와 겹치면 커버된 것으로 간주
            full_area > 0.0 && inter / full_area > 0.5
        });
        if !covered {
            result.push(full_box);
        }
    }

    result
}

/// det 박스들의 순수 기하학적 NMS.
fn nms_boxes(boxes: Vec<DetBox>) -> Vec<DetBox> {
    if boxes.is_empty() {
        return boxes;
    }

    let mut indexed: Vec<(usize, (f64, f64, f64, f64), f64)> = boxes
        .iter()
        .enumerate()
        .map(|(i, b)| {
            let bounds = detbox_bounds(b);
            let area = (bounds.2 - bounds.0) * (bounds.3 - bounds.1);
            (i, bounds, area)
        })
        .collect();
    indexed.sort_by(|a, b| b.2.partial_cmp(&a.2).unwrap());

    let mut keep = vec![true; boxes.len()];

    for i in 0..indexed.len() {
        if !keep[indexed[i].0] {
            continue;
        }
        for j in (i + 1)..indexed.len() {
            if !keep[indexed[j].0] {
                continue;
            }
            if nms_should_suppress(indexed[i].1, indexed[j].1) {
                keep[indexed[j].0] = false;
            }
        }
    }

    boxes
        .into_iter()
        .enumerate()
        .filter(|(i, _)| keep[*i])
        .map(|(_, b)| b)
        .collect()
}

fn detbox_bounds(b: &DetBox) -> (f64, f64, f64, f64) {
    let mut min_x = f64::MAX;
    let mut min_y = f64::MAX;
    let mut max_x = f64::MIN;
    let mut max_y = f64::MIN;
    for &[x, y] in b {
        min_x = min_x.min(x);
        min_y = min_y.min(y);
        max_x = max_x.max(x);
        max_y = max_y.max(y);
    }
    (min_x, min_y, max_x, max_y)
}

/// 작은 박스를 억제할지 판정한다.
/// a는 큰 박스, b는 작은 박스 (면적 내림차순으로 호출).
fn nms_should_suppress(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> bool {
    let inter = bbox_intersection_area(a, b);
    if inter <= 0.0 {
        return false;
    }

    let area_a = bbox_area(a);
    let area_b = bbox_area(b);

    // IoU 기준
    let union = area_a + area_b - inter;
    if union > 0.0 && inter / union > 0.5 {
        return true;
    }

    // 작은 박스 포함 비율: 작은 박스의 70% 이상이 큰 박스에 포함
    let min_area = area_a.min(area_b);
    if min_area > 0.0 && inter / min_area > 0.7 {
        return true;
    }

    false
}

fn bbox_area(b: (f64, f64, f64, f64)) -> f64 {
    (b.2 - b.0).max(0.0) * (b.3 - b.1).max(0.0)
}

fn bbox_intersection_area(a: (f64, f64, f64, f64), b: (f64, f64, f64, f64)) -> f64 {
    let inter_w = (a.2.min(b.2) - a.0.max(b.0)).max(0.0);
    let inter_h = (a.3.min(b.3) - a.1.max(b.1)).max(0.0);
    inter_w * inter_h
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
    fn NMS_억제_판정_IoU가_높으면_true() {
        assert!(nms_should_suppress(
            (0.0, 0.0, 100.0, 20.0),
            (2.0, 1.0, 98.0, 19.0),
        ));
    }

    #[test]
    fn NMS_억제_판정_포함_비율이_높으면_true() {
        assert!(nms_should_suppress(
            (10.0, 10.0, 200.0, 30.0),
            (50.0, 12.0, 150.0, 28.0),
        ));
    }

    #[test]
    fn NMS_억제_판정_겹치지_않으면_false() {
        assert!(!nms_should_suppress(
            (0.0, 0.0, 100.0, 20.0),
            (0.0, 50.0, 100.0, 70.0),
        ));
    }

    #[test]
    fn NMS는_IoU가_높은_박스를_억제한다() {
        let boxes = vec![
            [[10.0, 10.0], [100.0, 10.0], [100.0, 30.0], [10.0, 30.0]],
            [[12.0, 11.0], [98.0, 11.0], [98.0, 29.0], [12.0, 29.0]],
        ];
        let result = nms_boxes(boxes);
        assert_eq!(result.len(), 1);
    }

    #[test]
    fn NMS는_포함된_작은_박스를_억제한다() {
        let boxes = vec![
            [[10.0, 10.0], [200.0, 10.0], [200.0, 30.0], [10.0, 30.0]],
            [[50.0, 12.0], [150.0, 12.0], [150.0, 28.0], [50.0, 28.0]],
        ];
        let result = nms_boxes(boxes);
        assert_eq!(result.len(), 1);
        // 큰 박스가 남아야 함
        assert_eq!(result[0][0], [10.0, 10.0]);
    }

    #[test]
    fn NMS는_떨어진_박스를_유지한다() {
        let boxes = vec![
            [[10.0, 10.0], [100.0, 10.0], [100.0, 30.0], [10.0, 30.0]],
            [[10.0, 50.0], [100.0, 50.0], [100.0, 70.0], [10.0, 70.0]],
        ];
        let result = nms_boxes(boxes);
        assert_eq!(result.len(), 2);
    }

    #[test]
    fn NMS는_같은_줄_다른_단어를_유지한다() {
        // 같은 줄에서 겹치지 않는 두 단어
        let boxes = vec![
            [[10.0, 10.0], [80.0, 10.0], [80.0, 30.0], [10.0, 30.0]],
            [[100.0, 10.0], [200.0, 10.0], [200.0, 30.0], [100.0, 30.0]],
        ];
        let result = nms_boxes(boxes);
        assert_eq!(result.len(), 2);
    }

    #[test]
    fn 타일_우선_병합은_타일이_커버하는_전체_박스를_제거한다() {
        let full_boxes = vec![
            [[10.0, 10.0], [100.0, 10.0], [100.0, 30.0], [10.0, 30.0]],
        ];
        let tile_boxes = vec![
            [[12.0, 11.0], [98.0, 11.0], [98.0, 29.0], [12.0, 29.0]],
        ];
        let result = merge_tile_priority(full_boxes, tile_boxes);
        assert_eq!(result.len(), 1);
        // 타일 박스가 남아야 함
        assert_eq!(result[0][0], [12.0, 11.0]);
    }

    #[test]
    fn 타일_우선_병합은_타일이_못_잡은_영역을_전체_박스로_보충한다() {
        let full_boxes = vec![
            [[10.0, 10.0], [100.0, 10.0], [100.0, 30.0], [10.0, 30.0]],
            [[10.0, 50.0], [100.0, 50.0], [100.0, 70.0], [10.0, 70.0]],
        ];
        let tile_boxes = vec![
            [[12.0, 11.0], [98.0, 11.0], [98.0, 29.0], [12.0, 29.0]],
        ];
        let result = merge_tile_priority(full_boxes, tile_boxes);
        // 타일 1개 + 커버 안 된 전체 박스 1개
        assert_eq!(result.len(), 2);
    }

    #[test]
    fn 타일_그리드는_화면_크기에_따라_결정된다() {
        assert_eq!(tile_grid_for_size(2560, 1440), 4);
        assert_eq!(tile_grid_for_size(1920, 1080), 3);
        assert_eq!(tile_grid_for_size(1200, 800), 3);
    }
}
