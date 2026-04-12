use crate::config::{Config, OCR_DET_RESIZE_LONG};
use crate::ocr::OcrEngine;
use serde::{Deserialize, Serialize};

pub(crate) type OcrDetection = (Vec<[f64; 2]>, String);
pub(crate) type OcrDebugDetection = (Vec<[f64; 2]>, String, f32, bool);

#[derive(Serialize, Clone)]
pub(crate) struct OcrResultPayload {
    pub(crate) detections: Vec<OcrDetection>,
    pub(crate) debug_detections: Vec<OcrDebugDetection>,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
    pub(crate) source: String,
    pub(crate) word_gap: i32,
    pub(crate) line_gap: i32,
    pub(crate) debug_trace: bool,
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
    let monitors = xcap::Monitor::all().map_err(|e| e.to_string())?;
    let monitor = monitors.first().ok_or("디스플레이를 찾을 수 없음")?;
    let rgba_image = monitor.capture_image().map_err(|e| e.to_string())?;

    let orig_width = rgba_image.width();
    let orig_height = rgba_image.height();

    Ok(CaptureInfo {
        image: image::DynamicImage::ImageRgba8(rgba_image),
        x: monitor.x().map_err(|e| e.to_string())?,
        y: monitor.y().map_err(|e| e.to_string())?,
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
    let t0 = std::time::Instant::now();
    let boxes = engine.detect(&dyn_img, OCR_DET_RESIZE_LONG)?;
    eprintln!(
        "[OCR] det: {:.0}ms ({} 박스, {}×{}, resize_long {})",
        t0.elapsed().as_millis(),
        boxes.len(),
        dyn_img.width(),
        dyn_img.height(),
        OCR_DET_RESIZE_LONG
    );

    let (detections, debug_detections) =
        engine.recognize_boxes(&dyn_img, &boxes, cfg.score_thresh, cfg.ocr_debug_trace)?;

    Ok(OcrResultPayload {
        detections,
        debug_detections,
        orig_width,
        orig_height,
        source: cfg.source.clone(),
        word_gap: cfg.word_gap,
        line_gap: cfg.line_gap,
        debug_trace: cfg.ocr_debug_trace,
    })
}

pub(crate) async fn call_ai(
    client: &reqwest::Client,
    cfg: &Config,
    text: &str,
) -> Result<String, String> {
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
