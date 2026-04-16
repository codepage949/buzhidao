use crate::config::Config;
use crate::services::{OcrDebugDetection, OcrDetection};
use image::{DynamicImage, ImageFormat};
use serde::{Deserialize, Serialize};
use std::io::{BufRead, BufReader, Read, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError};
use std::sync::Mutex;
use std::time::{Duration, SystemTime, UNIX_EPOCH};
#[cfg(target_os = "windows")]
use std::os::windows::process::CommandExt;

#[cfg(target_os = "windows")]
const CREATE_NO_WINDOW: u32 = 0x08000000;
#[cfg(target_os = "windows")]
const DETACHED_PROCESS: u32 = 0x00000008;

pub(crate) struct PythonSidecarEngine {
    executable: PathBuf,
    device: String,
    lang: Mutex<String>,
    startup_timeout: Duration,
    request_timeout: Duration,
    state: Mutex<SidecarState>,
}

struct SidecarState {
    running: Option<RunningSidecar>,
    next_id: u64,
}

struct RunningSidecar {
    child: Child,
    stdin: ChildStdin,
    messages: Receiver<SidecarEvent>,
}

enum SidecarEvent {
    Message(SidecarMessage),
    Closed(String),
}

#[derive(Deserialize)]
#[serde(tag = "type", rename_all = "snake_case")]
enum SidecarMessage {
    Ready {
        langs: Vec<String>,
    },
    Result {
        id: u64,
        detections: Vec<SidecarDetection>,
        debug_detections: Vec<SidecarDebugDetection>,
    },
    Error {
        id: u64,
        message: String,
    },
}

#[derive(Serialize)]
struct SidecarRequest<'a> {
    id: u64,
    image_path: &'a str,
    source: &'a str,
    score_thresh: f32,
    debug_trace: bool,
}

#[derive(Deserialize)]
struct SidecarDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
}

#[derive(Deserialize)]
struct SidecarDebugDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
    score: f32,
    accepted: bool,
}

impl PythonSidecarEngine {
    pub(crate) fn new(cfg: &Config) -> Result<Self, String> {
        let executable = PathBuf::from(cfg.ocr_server_executable.trim());
        if !executable.exists() {
            return Err(format!(
                "OCR server 실행 파일을 찾을 수 없습니다: {}",
                executable.display()
            ));
        }

        Ok(Self {
            executable,
            device: cfg.ocr_server_device.clone(),
            lang: Mutex::new(cfg.source.clone()),
            startup_timeout: Duration::from_secs(cfg.ocr_server_startup_timeout_secs.max(1)),
            request_timeout: Duration::from_secs(cfg.ocr_server_request_timeout_secs.max(1)),
            state: Mutex::new(SidecarState {
                running: None,
                next_id: 1,
            }),
        })
    }

    pub(crate) fn warmup(&self) -> Result<(), String> {
        let lang = self.lang_snapshot()?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| "OCR server 상태 잠금 실패".to_string())?;
        ensure_running(
            &mut state.running,
            &self.executable,
            &self.device,
            &lang,
            self.startup_timeout,
            "warmup",
        )?;
        Ok(())
    }

    pub(crate) fn set_lang(&self, new_lang: &str) -> Result<(), String> {
        {
            let mut current = self
                .lang
                .lock()
                .map_err(|_| "OCR server 언어 잠금 실패".to_string())?;
            if current.as_str() == new_lang {
                return Ok(());
            }
            *current = new_lang.to_string();
        }
        let mut state = self
            .state
            .lock()
            .map_err(|_| "OCR server 상태 잠금 실패".to_string())?;
        shutdown_state(&mut state);
        eprintln!("[OCR] 언어 변경으로 OCR server 재시작 예약: {new_lang}");
        Ok(())
    }

    fn lang_snapshot(&self) -> Result<String, String> {
        self.lang
            .lock()
            .map(|g| g.clone())
            .map_err(|_| "OCR server 언어 잠금 실패".to_string())
    }

    pub(crate) fn run_image(
        &self,
        img: &DynamicImage,
        source: &str,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let temp_path = temp_image_path();
        img.save_with_format(&temp_path, ImageFormat::Png)
            .map_err(|e| format!("OCR server 임시 이미지 저장 실패: {e}"))?;

        let result = self.run_image_path(&temp_path, source, score_thresh, debug_trace);
        let _ = std::fs::remove_file(&temp_path);
        result
    }

    fn run_image_path(
        &self,
        image_path: &Path,
        source: &str,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let lang = self.lang_snapshot()?;
        let mut state = self
            .state
            .lock()
            .map_err(|_| "OCR server 상태 잠금 실패".to_string())?;
        let request_id = state.next_id;
        state.next_id += 1;

        let result = {
            let running = ensure_running(
                &mut state.running,
                &self.executable,
                &self.device,
                &lang,
                self.startup_timeout,
                source,
            )?;
            perform_request(
                running,
                request_id,
                image_path,
                source,
                score_thresh,
                debug_trace,
                self.request_timeout,
            )
        };

        if result.is_err() {
            shutdown_state(&mut state);
        }

        result
    }
}

fn ensure_running<'a>(
    running: &'a mut Option<RunningSidecar>,
    executable: &Path,
    device: &str,
    lang: &str,
    startup_timeout: Duration,
    source: &str,
) -> Result<&'a mut RunningSidecar, String> {
    if running.is_none() {
        *running = Some(spawn_sidecar(executable, device, lang, startup_timeout)?);
    }
    let running = running
        .as_mut()
        .ok_or("OCR server 프로세스 생성 실패".to_string())?;
    eprintln!("[OCR] OCR server 사용 ({source}, device={device})");
    Ok(running)
}

fn perform_request(
    running: &mut RunningSidecar,
    request_id: u64,
    image_path: &Path,
    source: &str,
    score_thresh: f32,
    debug_trace: bool,
    request_timeout: Duration,
) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
    let request = SidecarRequest {
        id: request_id,
        image_path: &image_path.to_string_lossy(),
        source,
        score_thresh,
        debug_trace,
    };
    let payload =
        serde_json::to_string(&request).map_err(|e| format!("OCR 요청 직렬화 실패: {e}"))?;
    writeln!(running.stdin, "{payload}")
        .and_then(|_| running.stdin.flush())
        .map_err(|e| format!("OCR server stdin 쓰기 실패: {e}"))?;

    loop {
        match running.messages.recv_timeout(request_timeout) {
            Ok(SidecarEvent::Message(SidecarMessage::Result {
                id,
                detections,
                debug_detections,
            })) if id == request_id => {
                return Ok((
                    detections
                        .into_iter()
                        .map(|item| (item.polygon, item.text))
                        .collect(),
                    debug_detections
                        .into_iter()
                        .map(|item| (item.polygon, item.text, item.score, item.accepted))
                        .collect(),
                ));
            }
            Ok(SidecarEvent::Message(SidecarMessage::Error { id, message }))
                if id == request_id =>
            {
                return Err(format!("OCR server 처리 실패: {message}"));
            }
            Ok(SidecarEvent::Message(SidecarMessage::Ready { .. })) => {}
            Ok(SidecarEvent::Message(_)) => {
                return Err("OCR server 응답 순서가 어긋남".to_string());
            }
            Ok(SidecarEvent::Closed(reason)) => {
                return Err(format!("OCR server 종료됨: {reason}"));
            }
            Err(RecvTimeoutError::Timeout) => {
                return Err(format!(
                    "OCR server 응답 시간 초과 ({}초)",
                    request_timeout.as_secs()
                ));
            }
            Err(RecvTimeoutError::Disconnected) => {
                return Err("OCR server 응답 채널 종료".to_string());
            }
        }
    }
}

fn spawn_sidecar(
    executable: &Path,
    device: &str,
    lang: &str,
    startup_timeout: Duration,
) -> Result<RunningSidecar, String> {
    let mut cmd = Command::new(executable);
    cmd.arg("--server")
        .env("PYTHON_OCR_DEVICE", device)
        .env("PYTHON_OCR_LANG", lang)
        .env("PYTHONUTF8", "1")
        .env("PYTHONIOENCODING", "utf-8")
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped());
    #[cfg(target_os = "windows")]
    cmd.creation_flags(CREATE_NO_WINDOW | DETACHED_PROCESS);

    let mut child = cmd
        .spawn()
        .map_err(|e| format!("OCR server 실행 실패 ({}): {e}", executable.display()))?;
    let stdin = child
        .stdin
        .take()
        .ok_or("OCR server stdin 파이프 생성 실패".to_string())?;
    let stdout = child
        .stdout
        .take()
        .ok_or("OCR server stdout 파이프 생성 실패".to_string())?;
    let stderr = child
        .stderr
        .take()
        .ok_or("OCR server stderr 파이프 생성 실패".to_string())?;

    let (tx, rx) = mpsc::channel();
    std::thread::spawn(move || stdout_reader_thread(stdout, tx));
    std::thread::spawn(move || stderr_reader_thread(stderr));

    let running = RunningSidecar {
        child,
        stdin,
        messages: rx,
    };
    wait_ready(running, startup_timeout)
}

fn wait_ready(
    mut running: RunningSidecar,
    startup_timeout: Duration,
) -> Result<RunningSidecar, String> {
    match running.messages.recv_timeout(startup_timeout) {
        Ok(SidecarEvent::Message(SidecarMessage::Ready { langs })) => {
            eprintln!("[OCR] OCR server 준비 완료: {}", langs.join(","));
            Ok(running)
        }
        Ok(SidecarEvent::Message(_)) => {
            shutdown_running(&mut running);
            Err("OCR server 준비 메시지가 올바르지 않음".to_string())
        }
        Ok(SidecarEvent::Closed(reason)) => {
            shutdown_running(&mut running);
            Err(format!("OCR server 시작 직후 종료됨: {reason}"))
        }
        Err(RecvTimeoutError::Timeout) => {
            shutdown_running(&mut running);
            Err(format!(
                "OCR server 준비 시간 초과 ({}초)",
                startup_timeout.as_secs()
            ))
        }
        Err(RecvTimeoutError::Disconnected) => {
            shutdown_running(&mut running);
            Err("OCR server 준비 채널 종료".to_string())
        }
    }
}

fn stdout_reader_thread(stdout: impl Read, tx: mpsc::Sender<SidecarEvent>) {
    let reader = BufReader::new(stdout);
    for line in reader.lines() {
        match line {
            Ok(line) => {
                if line.trim().is_empty() {
                    continue;
                }
                match serde_json::from_str::<SidecarMessage>(&line) {
                    Ok(message) => {
                        if tx.send(SidecarEvent::Message(message)).is_err() {
                            return;
                        }
                    }
                    Err(err) => {
                        let _ = tx.send(SidecarEvent::Closed(format!(
                            "JSON 파싱 실패: {err}; line={line}"
                        )));
                        return;
                    }
                }
            }
            Err(err) => {
                let _ = tx.send(SidecarEvent::Closed(format!("stdout 읽기 실패: {err}")));
                return;
            }
        }
    }
    let _ = tx.send(SidecarEvent::Closed("stdout EOF".to_string()));
}

fn stderr_reader_thread(mut stderr: impl Read) {
    let mut buffer = Vec::new();
    match stderr.read_to_end(&mut buffer) {
        Ok(_) if !buffer.is_empty() => {
            let text = String::from_utf8_lossy(&buffer);
            for line in text.lines() {
                eprintln!("[ocr-server] {line}");
            }
        }
        Ok(_) => {}
        Err(err) => eprintln!("[ocr-server] stderr 읽기 실패: {err}"),
    }
}

fn shutdown_running(running: &mut RunningSidecar) {
    let _ = running.child.kill();
    let _ = running.child.wait();
}

fn shutdown_state(state: &mut SidecarState) {
    if let Some(mut running) = state.running.take() {
        shutdown_running(&mut running);
    }
}

fn temp_image_path() -> PathBuf {
    let nanos = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .expect("시계가 UNIX_EPOCH 이전입니다")
        .as_nanos();
    std::env::temp_dir().join(format!("buzhidao-python-ocr-{nanos}.png"))
}
