use crate::config::{Config, OCR_DET_RESIZE_LONG};
use crate::ocr::OcrBackend;
#[cfg(target_os = "linux")]
use ashpd::desktop::{
    PersistMode,
    screencast::{CursorMode, Screencast, SelectSourcesOptions, SourceType, Stream as CastStream},
};
#[cfg(target_os = "linux")]
use pipewire as pw;
use serde::{Deserialize, Serialize};
use tauri::AppHandle;

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

#[derive(Clone)]
pub(crate) struct CaptureInfo {
    pub(crate) image: image::DynamicImage,
    pub(crate) x: i32,
    pub(crate) y: i32,
    pub(crate) orig_width: u32,
    pub(crate) orig_height: u32,
}

pub(crate) fn crop_capture_to_region(
    capture: CaptureInfo,
    rect_x: f64,
    rect_y: f64,
    rect_w: f64,
    rect_h: f64,
    viewport_w: f64,
    viewport_h: f64,
) -> Result<(image::DynamicImage, f64, f64, u32, u32), String> {
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

    let cropped = capture.image.crop_imm(crop_x, crop_y, crop_w, crop_h);
    Ok((
        cropped,
        crop_x as f64,
        crop_y as f64,
        capture.orig_width,
        capture.orig_height,
    ))
}

pub(crate) fn offset_ocr_result(payload: &mut OcrResultPayload, offset_x: f64, offset_y: f64) {
    for (polygon, _) in &mut payload.detections {
        for point in polygon {
            point[0] += offset_x;
            point[1] += offset_y;
        }
    }
    for (polygon, _, _, _) in &mut payload.debug_detections {
        for point in polygon {
            point[0] += offset_x;
            point[1] += offset_y;
        }
    }
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

pub(crate) async fn capture_screen(_app: &AppHandle) -> Result<CaptureInfo, String> {
    #[cfg(target_os = "linux")]
    if should_use_wayland_portal() {
        return capture_screen_via_wayland_portal().await;
    }

    tauri::async_runtime::spawn_blocking(capture_screen_with_xcap)
        .await
        .map_err(|e| format!("캡처 스레드 오류: {e}"))?
}

fn capture_screen_with_xcap() -> Result<CaptureInfo, String> {
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

#[cfg(target_os = "linux")]
async fn capture_screen_via_wayland_portal() -> Result<CaptureInfo, String> {
    capture_screen_via_screencast().await
}

#[cfg(target_os = "linux")]
async fn capture_screen_via_screencast() -> Result<CaptureInfo, String> {
    let restore_token = load_wayland_restore_token();
    if let Some(token) = restore_token.as_deref() {
        match capture_screen_via_screencast_with_restore_token(Some(token)).await {
            Ok(info) => return Ok(info),
            Err(err) => {
                eprintln!("[포털] 저장된 restore_token 재사용 실패: {err}");
                clear_wayland_restore_token();
            }
        }
    }

    capture_screen_via_screencast_with_restore_token(None).await
}

#[cfg(target_os = "linux")]
async fn capture_screen_via_screencast_with_restore_token(
    restore_token: Option<&str>,
) -> Result<CaptureInfo, String> {
    let proxy = Screencast::new()
        .await
        .map_err(|e| format!("ScreenCast 프록시 생성 실패: {e}"))?;
    let session = proxy
        .create_session(Default::default())
        .await
        .map_err(|e| format!("ScreenCast 세션 생성 실패: {e}"))?;

    proxy
        .select_sources(
            &session,
            SelectSourcesOptions::default()
                .set_cursor_mode(CursorMode::Hidden)
                .set_sources(Some(SourceType::Monitor.into()))
                .set_multiple(false)
                .set_restore_token(restore_token)
                .set_persist_mode(PersistMode::ExplicitlyRevoked),
        )
        .await
        .map_err(|e| format!("ScreenCast 소스 선택 요청 실패: {e}"))?
        .response()
        .map_err(|e| format!("ScreenCast 소스 선택 응답 실패: {e}"))?;

    let response = proxy
        .start(&session, None, Default::default())
        .await
        .map_err(|e| format!("ScreenCast 시작 요청 실패: {e}"))?
        .response()
        .map_err(|e| format!("ScreenCast 시작 응답 실패: {e}"))?;

    if let Some(token) = response.restore_token() {
        save_wayland_restore_token(token);
    }

    let stream = response
        .streams()
        .first()
        .cloned()
        .ok_or("ScreenCast 스트림이 비어 있음")?;
    let fd = proxy
        .open_pipe_wire_remote(&session, Default::default())
        .await
        .map_err(|e| format!("PipeWire remote 열기 실패: {e}"))?;

    let capture = tauri::async_runtime::spawn_blocking(move || {
        capture_first_screencast_frame(stream, fd)
    })
    .await
    .map_err(|e| format!("ScreenCast 캡처 스레드 오류: {e}"))??;

    let _ = session.close().await;
    Ok(capture)
}

#[cfg(target_os = "linux")]
fn wayland_restore_token_path() -> Option<std::path::PathBuf> {
    dirs::config_dir().map(|dir| dir.join("buzhidao").join("wayland-screencast-token"))
}

#[cfg(target_os = "linux")]
fn load_wayland_restore_token() -> Option<String> {
    let path = wayland_restore_token_path()?;
    std::fs::read_to_string(path)
        .ok()
        .map(|value| value.trim().to_string())
        .filter(|value| !value.is_empty())
}

#[cfg(target_os = "linux")]
fn save_wayland_restore_token(token: &str) {
    let Some(path) = wayland_restore_token_path() else {
        return;
    };
    let Some(parent) = path.parent() else {
        return;
    };
    if std::fs::create_dir_all(parent).is_err() {
        return;
    }
    let _ = std::fs::write(&path, token);
}

#[cfg(target_os = "linux")]
fn clear_wayland_restore_token() {
    let Some(path) = wayland_restore_token_path() else {
        return;
    };
    let _ = std::fs::remove_file(path);
}

#[cfg(target_os = "linux")]
struct PipeWireUserData {
    format: pw::spa::param::video::VideoInfoRaw,
    first_frame: std::sync::mpsc::SyncSender<Result<ScreenFrame, String>>,
    mainloop: pw::main_loop::MainLoopRc,
    done: bool,
}

#[cfg(target_os = "linux")]
struct ScreenFrame {
    image: image::RgbaImage,
}

#[cfg(target_os = "linux")]
fn capture_first_screencast_frame(
    stream: CastStream,
    fd: std::os::fd::OwnedFd,
) -> Result<CaptureInfo, String> {
    use pw::{properties::properties, spa};

    pw::init();

    let mainloop = pw::main_loop::MainLoopRc::new(None)
        .map_err(|e| format!("PipeWire main loop 생성 실패: {e}"))?;
    let context = pw::context::ContextRc::new(&mainloop, None)
        .map_err(|e| format!("PipeWire context 생성 실패: {e}"))?;
    let core = context
        .connect_fd_rc(fd, None)
        .map_err(|e| format!("PipeWire remote 연결 실패: {e}"))?;

    let (sender, receiver) = std::sync::mpsc::sync_channel(1);
    let timeout_sender = sender.clone();
    let timeout_loop = mainloop.clone();
    let timer = mainloop.loop_().add_timer(move |_| {
        let _ = timeout_sender.send(Err("ScreenCast 첫 프레임 대기 시간 초과".to_string()));
        timeout_loop.quit();
    });
    timer
        .update_timer(Some(std::time::Duration::from_secs(10)), None)
        .into_result()
        .map_err(|e| format!("PipeWire 타이머 설정 실패: {e}"))?;

    let user_data = PipeWireUserData {
        format: Default::default(),
        first_frame: sender,
        mainloop: mainloop.clone(),
        done: false,
    };

    let pw_stream = pw::stream::StreamBox::new(
        &core,
        "buzhidao-screen-capture",
        properties! {
            *pw::keys::MEDIA_TYPE => "Video",
            *pw::keys::MEDIA_CATEGORY => "Capture",
            *pw::keys::MEDIA_ROLE => "Screen",
        },
    )
    .map_err(|e| format!("PipeWire stream 생성 실패: {e}"))?;

    let _listener = pw_stream
        .add_local_listener_with_user_data(user_data)
        .state_changed(|_, _, _, _| {})
        .param_changed(|_, user_data, id, param| {
            let Some(param) = param else {
                return;
            };
            if id != pw::spa::param::ParamType::Format.as_raw() {
                return;
            }

            let Ok((media_type, media_subtype)) = pw::spa::param::format_utils::parse_format(param)
            else {
                return;
            };

            if media_type != pw::spa::param::format::MediaType::Video
                || media_subtype != pw::spa::param::format::MediaSubtype::Raw
            {
                return;
            }

            if let Err(err) = user_data.format.parse(param) {
                let _ = user_data
                    .first_frame
                    .send(Err(format!("PipeWire 비디오 포맷 파싱 실패: {err}")));
                user_data.mainloop.quit();
            }
        })
        .process(|stream, user_data| {
            if user_data.done {
                return;
            }

            let Some(mut buffer) = stream.dequeue_buffer() else {
                return;
            };
            let datas = buffer.datas_mut();
            if datas.is_empty() {
                return;
            }

            let _ = user_data
                .first_frame
                .send(extract_screen_frame(&mut datas[0], &user_data.format));
            user_data.done = true;
            user_data.mainloop.quit();
        })
        .register()
        .map_err(|e| format!("PipeWire listener 등록 실패: {e}"))?;

    let format_obj = spa::pod::object!(
        spa::utils::SpaTypes::ObjectParamFormat,
        spa::param::ParamType::EnumFormat,
        spa::pod::property!(
            spa::param::format::FormatProperties::MediaType,
            Id,
            spa::param::format::MediaType::Video
        ),
        spa::pod::property!(
            spa::param::format::FormatProperties::MediaSubtype,
            Id,
            spa::param::format::MediaSubtype::Raw
        ),
        spa::pod::property!(
            spa::param::format::FormatProperties::VideoFormat,
            Choice,
            Enum,
            Id,
            spa::param::video::VideoFormat::RGBA,
            spa::param::video::VideoFormat::RGBA,
            spa::param::video::VideoFormat::RGBx,
            spa::param::video::VideoFormat::BGRx,
        ),
    );
    let values: Vec<u8> = spa::pod::serialize::PodSerializer::serialize(
        std::io::Cursor::new(Vec::new()),
        &spa::pod::Value::Object(format_obj),
    )
    .map_err(|e| format!("PipeWire 포맷 직렬화 실패: {e}"))?
    .0
    .into_inner();
    let mut params = [spa::pod::Pod::from_bytes(&values)
        .ok_or("PipeWire 포맷 pod 변환 실패")?];

    pw_stream
        .connect(
            spa::utils::Direction::Input,
            Some(stream.pipe_wire_node_id()),
            pw::stream::StreamFlags::AUTOCONNECT | pw::stream::StreamFlags::MAP_BUFFERS,
            &mut params,
        )
        .map_err(|e| format!("PipeWire stream 연결 실패: {e}"))?;

    mainloop.run();

    let frame = receiver
        .recv()
        .map_err(|e| format!("PipeWire 프레임 수신 실패: {e}"))??;

    let (orig_width, orig_height) = frame.image.dimensions();
    Ok(CaptureInfo {
        image: image::DynamicImage::ImageRgba8(frame.image),
        x: stream.position().map(|(x, _)| x).unwrap_or(0),
        y: stream.position().map(|(_, y)| y).unwrap_or(0),
        orig_width,
        orig_height,
    })
}

#[cfg(target_os = "linux")]
fn extract_screen_frame(
    data: &mut pw::spa::buffer::Data,
    info: &pw::spa::param::video::VideoInfoRaw,
) -> Result<ScreenFrame, String> {
    let width = info.size().width;
    let height = info.size().height;
    if width == 0 || height == 0 {
        return Err("PipeWire 비디오 크기가 0임".to_string());
    }

    let chunk = data.chunk();
    let offset = chunk.offset() as usize;
    let size = chunk.size() as usize;
    let stride = if chunk.stride() > 0 {
        chunk.stride() as usize
    } else {
        width as usize * 4
    };
    let slice = data.data().ok_or("PipeWire 프레임 버퍼가 비어 있음")?;

    if offset.checked_add(size).is_none_or(|end| end > slice.len()) {
        return Err("PipeWire 프레임 청크 범위가 잘못됨".to_string());
    }

    let source = &slice[offset..offset + size];
    let rgba = rgba_image_from_raw_frame(info.format(), width, height, stride, source)?;
    Ok(ScreenFrame { image: rgba })
}

#[cfg(target_os = "linux")]
fn rgba_image_from_raw_frame(
    format: pw::spa::param::video::VideoFormat,
    width: u32,
    height: u32,
    stride: usize,
    source: &[u8],
) -> Result<image::RgbaImage, String> {
    let width_usize = width as usize;
    let height_usize = height as usize;
    let row_bytes = width_usize
        .checked_mul(4)
        .ok_or("프레임 너비가 너무 큼")?;
    let required = stride
        .checked_mul(height_usize)
        .ok_or("프레임 stride가 너무 큼")?;
    if source.len() < required {
        return Err(format!(
            "프레임 버퍼 길이가 부족함: have={}, need={required}",
            source.len()
        ));
    }

    let mut out = vec![0u8; row_bytes * height_usize];
    for y in 0..height_usize {
        let src_row = &source[y * stride..y * stride + row_bytes];
        let dst_row = &mut out[y * row_bytes..(y + 1) * row_bytes];
        match format {
            pw::spa::param::video::VideoFormat::RGBA => {
                dst_row.copy_from_slice(src_row);
            }
            pw::spa::param::video::VideoFormat::RGBx => {
                for (src, dst) in src_row.chunks_exact(4).zip(dst_row.chunks_exact_mut(4)) {
                    dst[0] = src[0];
                    dst[1] = src[1];
                    dst[2] = src[2];
                    dst[3] = 255;
                }
            }
            pw::spa::param::video::VideoFormat::BGRx => {
                for (src, dst) in src_row.chunks_exact(4).zip(dst_row.chunks_exact_mut(4)) {
                    dst[0] = src[2];
                    dst[1] = src[1];
                    dst[2] = src[0];
                    dst[3] = 255;
                }
            }
            other => {
                return Err(format!("지원하지 않는 PipeWire 비디오 포맷: {other:?}"));
            }
        }
    }

    image::RgbaImage::from_raw(width, height, out)
        .ok_or("PipeWire RGBA 이미지 생성 실패".to_string())
}

#[cfg(target_os = "linux")]
fn should_use_wayland_portal() -> bool {
    should_use_wayland_portal_for(std::env::var("XDG_SESSION_TYPE").ok().as_deref())
}

#[cfg(target_os = "linux")]
fn should_use_wayland_portal_for(session_type: Option<&str>) -> bool {
    session_type.is_some_and(|value| value.eq_ignore_ascii_case("wayland"))
}

pub(crate) fn run_ocr(
    cfg: &Config,
    engine: &OcrBackend,
    dyn_img: image::DynamicImage,
    orig_width: u32,
    orig_height: u32,
) -> Result<OcrResultPayload, String> {
    let det_resize_long =
        effective_det_resize_long(engine.is_cpu_mode(), dyn_img.width(), dyn_img.height());
    let t0 = std::time::Instant::now();
    let (detections, debug_detections) =
        engine.run_image(
            &dyn_img,
            det_resize_long,
            &cfg.source,
            cfg.score_thresh,
            cfg.ocr_debug_trace,
        )?;
    eprintln!(
        "[OCR] backend 전체 처리: {:.0}ms ({}×{}, resize_long {})",
        t0.elapsed().as_millis(),
        dyn_img.width(),
        dyn_img.height(),
        det_resize_long
    );

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

fn effective_det_resize_long(cpu_mode: bool, width: u32, height: u32) -> u32 {
    if !cpu_mode {
        return OCR_DET_RESIZE_LONG;
    }

    OCR_DET_RESIZE_LONG.min(width.max(height).max(128))
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
    use super::{
        effective_det_resize_long, offset_ocr_result, selection_rect_to_image_rect,
        OcrResultPayload,
    };
    #[cfg(target_os = "linux")]
    use super::{rgba_image_from_raw_frame, should_use_wayland_portal_for, wayland_restore_token_path};
    #[cfg(target_os = "linux")]
    use pipewire as pw;

    #[test]
    fn 단일_ocr_모드는_det_resize_long_1024를_사용한다() {
        assert_eq!(OCR_DET_RESIZE_LONG, 1024);
    }

    #[test]
    fn cpu_모드에서는_작은_영역_det를_불필요하게_확대하지_않는다() {
        assert_eq!(effective_det_resize_long(true, 367, 366), 367);
        assert_eq!(
            effective_det_resize_long(false, 367, 366),
            OCR_DET_RESIZE_LONG
        );
    }

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
            detections: vec![(vec![[10.0, 20.0], [30.0, 40.0]], "text".to_string())],
            debug_detections: vec![(vec![[1.0, 2.0], [3.0, 4.0]], "dbg".to_string(), 0.9, true)],
            orig_width: 100,
            orig_height: 100,
            source: "en".to_string(),
            word_gap: 20,
            line_gap: 15,
            debug_trace: false,
        };

        offset_ocr_result(&mut payload, 5.0, 7.0);

        assert_eq!(payload.detections[0].0[0], [15.0, 27.0]);
        assert_eq!(payload.debug_detections[0].0[1], [8.0, 11.0]);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn wayland_세션이면_포털_캡처를_사용한다() {
        assert!(should_use_wayland_portal_for(Some("wayland")));
        assert!(should_use_wayland_portal_for(Some("Wayland")));
        assert!(!should_use_wayland_portal_for(Some("x11")));
        assert!(!should_use_wayland_portal_for(None));
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn bgrx_프레임을_rgba_이미지로_변환한다() {
        let image = rgba_image_from_raw_frame(
            pw::spa::param::video::VideoFormat::BGRx,
            2,
            1,
            8,
            &[10, 20, 30, 0, 40, 50, 60, 0],
        )
        .unwrap();

        assert_eq!(image.as_raw(), &[30, 20, 10, 255, 60, 50, 40, 255]);
    }

    #[cfg(target_os = "linux")]
    #[test]
    fn restore_token_저장_경로는_config_dir_아래다() {
        let path = wayland_restore_token_path().expect("config dir가 있어야 한다");
        assert!(path.ends_with("buzhidao/wayland-screencast-token"));
    }
}
