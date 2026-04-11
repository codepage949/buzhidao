use crate::config::{Config, OCR_DET_RESIZE_LONG};
use crate::ocr::OcrEngine;
use image::DynamicImage;
use serde::{Deserialize, Serialize};

pub(crate) type OcrDetection = (Vec<[f64; 2]>, String);

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
    let detections = predict_with_tiles(engine, &dyn_img, cfg.score_thresh, OCR_DET_RESIZE_LONG)?;

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
    det_resize_long: u32,
) -> Result<Vec<OcrDetection>, String> {
    let t_det_full = std::time::Instant::now();
    let full_boxes = engine.detect(img, det_resize_long)?;
    eprintln!("[OCR] det 단일 모드: full det only");
    eprintln!(
        "[OCR] det 전체: {:.0}ms ({} 박스, {}×{}, resize_long {})",
        t_det_full.elapsed().as_millis(),
        full_boxes.len(),
        img.width(),
        img.height(),
        det_resize_long
    );

    engine.recognize_boxes(img, &full_boxes, score_thresh)
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
    use super::OCR_DET_RESIZE_LONG;

    #[test]
    fn 단일_ocr_모드는_det_resize_long_1024를_사용한다() {
        assert_eq!(OCR_DET_RESIZE_LONG, 1024);
    }
}
