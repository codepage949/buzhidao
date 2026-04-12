use crate::services::{OcrDebugDetection, OcrDetection};
use serde::Deserialize;
use std::path::{Path, PathBuf};
use std::process::Command;

#[derive(Deserialize)]
struct RawOcrResult {
    detections: Vec<RawDetection>,
    #[serde(default)]
    debug_detections: Vec<RawDebugDetection>,
}

#[derive(Deserialize)]
struct RawDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
}

#[derive(Deserialize)]
struct RawDebugDetection {
    polygon: Vec<[f64; 2]>,
    text: String,
    score: f32,
    accepted: bool,
}

pub(crate) struct PythonSidecarEngine {
    executable: PathBuf,
}

impl PythonSidecarEngine {
    pub(crate) fn new() -> Result<Self, String> {
        let executable = std::env::var_os("PYTHON_OCR_EXECUTABLE")
            .map(PathBuf::from)
            .ok_or("PYTHON_OCR_EXECUTABLE 환경변수가 필요합니다".to_string())?;
        if !executable.exists() {
            return Err(format!(
                "Python OCR 실행 파일을 찾을 수 없습니다: {}",
                executable.display()
            ));
        }
        Ok(Self { executable })
    }

    pub(crate) fn run_image_file(
        &self,
        image_path: &Path,
        source: &str,
        score_thresh: f32,
        debug_trace: bool,
    ) -> Result<(Vec<OcrDetection>, Vec<OcrDebugDetection>), String> {
        let output = Command::new(&self.executable)
            .arg("--image")
            .arg(image_path)
            .arg("--source")
            .arg(source)
            .arg("--score-thresh")
            .arg(score_thresh.to_string())
            .arg("--debug-trace")
            .arg(if debug_trace { "true" } else { "false" })
            .output()
            .map_err(|e| format!("Python OCR 실행 실패: {e}"))?;

        if !output.status.success() {
            let stderr = String::from_utf8_lossy(&output.stderr);
            return Err(format!(
                "Python OCR 실행 파일이 실패했습니다 (code {:?}): {}",
                output.status.code(),
                stderr.trim()
            ));
        }

        let stdout = String::from_utf8(output.stdout)
            .map_err(|e| format!("Python OCR stdout UTF-8 파싱 실패: {e}"))?;
        let parsed: RawOcrResult = serde_json::from_str(&stdout)
            .map_err(|e| format!("Python OCR JSON 파싱 실패: {e}\nstdout={stdout}"))?;

        Ok((
            parsed
                .detections
                .into_iter()
                .map(|item| (item.polygon, item.text))
                .collect(),
            parsed
                .debug_detections
                .into_iter()
                .map(|item| (item.polygon, item.text, item.score, item.accepted))
                .collect(),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn python_sidecar_json을_공용_detection_형식으로_변환한다() {
        let raw = r#"{
          "detections": [
            { "polygon": [[1.0, 2.0], [3.0, 4.0]], "text": "hello" }
          ],
          "debug_detections": [
            {
              "polygon": [[5.0, 6.0], [7.0, 8.0]],
              "text": "world",
              "score": 0.9,
              "accepted": true
            }
          ]
        }"#;

        let parsed: RawOcrResult = serde_json::from_str(raw).expect("JSON 파싱 실패");
        assert_eq!(parsed.detections.len(), 1);
        assert_eq!(parsed.debug_detections.len(), 1);
    }
}
