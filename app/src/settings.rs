use crate::config::Config;
use serde::{Deserialize, Serialize};
use std::path::Path;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq)]
pub(crate) struct UserSettings {
    pub source: String,
    pub score_thresh: f32,
    pub ocr_server_device: String,
    pub ai_gateway_api_key: String,
    pub ai_gateway_model: String,
    pub system_prompt: String,
    pub word_gap: i32,
    pub line_gap: i32,
}

impl UserSettings {
    pub(crate) fn from_config(cfg: &Config) -> Self {
        Self {
            source: cfg.source.clone(),
            score_thresh: cfg.score_thresh,
            ocr_server_device: cfg.ocr_server_device.clone(),
            ai_gateway_api_key: cfg.ai_gateway_api_key.clone(),
            ai_gateway_model: cfg.ai_gateway_model.clone(),
            system_prompt: cfg.system_prompt.clone(),
            word_gap: cfg.word_gap,
            line_gap: cfg.line_gap,
        }
    }

    pub(crate) fn validate(mut self) -> Self {
        self.source = normalize_source(&self.source);
        self.ocr_server_device = normalize_device(&self.ocr_server_device);
        self.score_thresh = self.score_thresh.clamp(0.0, 1.0);
        self.word_gap = self.word_gap.max(0);
        self.line_gap = self.line_gap.max(0);
        self.ai_gateway_api_key = self.ai_gateway_api_key.trim().to_string();
        self.ai_gateway_model = self.ai_gateway_model.trim().to_string();
        self.system_prompt = self.system_prompt.trim().to_string();
        self
    }

    pub(crate) fn apply_to(&self, cfg: &mut Config) {
        cfg.source = self.source.clone();
        cfg.score_thresh = self.score_thresh;
        cfg.ocr_server_device = self.ocr_server_device.clone();
        cfg.ai_gateway_api_key = self.ai_gateway_api_key.clone();
        cfg.ai_gateway_model = self.ai_gateway_model.clone();
        cfg.system_prompt = self.system_prompt.clone();
        cfg.word_gap = self.word_gap;
        cfg.line_gap = self.line_gap;
    }
}

fn normalize_source(value: &str) -> String {
    let v = value.trim().to_ascii_lowercase();
    if v == "ch" {
        "ch".to_string()
    } else {
        "en".to_string()
    }
}

fn normalize_device(value: &str) -> String {
    let v = value.trim().to_ascii_lowercase();
    if v == "gpu" {
        "gpu".to_string()
    } else {
        "cpu".to_string()
    }
}

pub(crate) fn save_to_env_file(path: &Path, settings: &UserSettings) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("환경 파일 디렉토리 생성 실패: {e}"))?;
    }

    let existing = std::fs::read_to_string(path).unwrap_or_default();
    let newline = if existing.contains("\r\n") { "\r\n" } else { "\n" };
    let mut lines = if existing.is_empty() {
        Vec::new()
    } else {
        existing
            .replace("\r\n", "\n")
            .split('\n')
            .map(str::to_string)
            .collect::<Vec<_>>()
    };

    let managed = managed_env_entries(settings);
    for (key, value) in &managed {
        let rendered = format!("{key}={value}");
        let mut updated = false;
        for line in &mut lines {
            if env_key_of(line).is_some_and(|current| current == *key) {
                *line = rendered.clone();
                updated = true;
                break;
            }
        }
        if !updated {
            lines.push(rendered);
        }
    }

    let mut content = lines.join(newline);
    if !content.is_empty() && !content.ends_with(newline) {
        content.push_str(newline);
    }
    std::fs::write(path, content).map_err(|e| format!("환경 파일 저장 실패: {e}"))?;
    Ok(())
}

pub(crate) fn materialize_env_file(path: &Path, env_example: &str) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)
            .map_err(|e| format!("환경 파일 디렉토리 생성 실패: {e}"))?;
    }

    let existing = std::fs::read_to_string(path).unwrap_or_default();
    let merged = merge_env_example_with_existing(env_example, &existing);
    std::fs::write(path, merged).map_err(|e| format!("환경 파일 생성 실패: {e}"))?;
    Ok(())
}

fn merge_env_example_with_existing(env_example: &str, existing: &str) -> String {
    let newline = detect_newline(env_example).or_else(|| detect_newline(existing)).unwrap_or("\n");
    let existing_entries = parse_env_entries(existing);
    let mut lines = env_example
        .replace("\r\n", "\n")
        .split('\n')
        .map(str::to_string)
        .collect::<Vec<_>>();

    for line in &mut lines {
        if let Some(key) = env_key_of(line) {
            if let Some(value) = existing_entries.get(key) {
                *line = format!("{key}={value}");
            }
        }
    }

    for (key, value) in existing_entries {
        if !lines
            .iter()
            .any(|line| env_key_of(line).is_some_and(|current| current == key))
        {
            lines.push(format!("{key}={value}"));
        }
    }

    let mut content = lines.join(newline);
    if !content.ends_with(newline) {
        content.push_str(newline);
    }
    content
}

fn managed_env_entries(settings: &UserSettings) -> [(&'static str, String); 8] {
    [
        ("SOURCE", settings.source.clone()),
        ("SCORE_THRESH", settings.score_thresh.to_string()),
        ("OCR_SERVER_DEVICE", settings.ocr_server_device.clone()),
        ("AI_GATEWAY_API_KEY", settings.ai_gateway_api_key.clone()),
        ("AI_GATEWAY_MODEL", settings.ai_gateway_model.clone()),
        (
            "SYSTEM_PROMPT",
            settings
                .system_prompt
                .replace('\\', "\\\\")
                .replace('\n', "\\n"),
        ),
        ("WORD_GAP", settings.word_gap.to_string()),
        ("LINE_GAP", settings.line_gap.to_string()),
    ]
}

fn env_key_of(line: &str) -> Option<&str> {
    let trimmed = line.trim_start();
    if trimmed.is_empty() || trimmed.starts_with('#') {
        return None;
    }
    let (key, _) = trimmed.split_once('=')?;
    Some(key.trim())
}

fn parse_env_entries(text: &str) -> std::collections::BTreeMap<String, String> {
    let mut entries = std::collections::BTreeMap::new();
    for line in text.replace("\r\n", "\n").split('\n') {
        let trimmed = line.trim();
        if trimmed.is_empty() || trimmed.starts_with('#') {
            continue;
        }
        if let Some((key, value)) = trimmed.split_once('=') {
            entries.insert(key.trim().to_string(), value.to_string());
        }
    }
    entries
}

fn detect_newline(text: &str) -> Option<&'static str> {
    if text.contains("\r\n") {
        Some("\r\n")
    } else if text.contains('\n') {
        Some("\n")
    } else {
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn base_config() -> Config {
        Config {
            source: "en".to_string(),
            score_thresh: 0.5,
            ocr_debug_trace: false,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: "k".to_string(),
            ai_gateway_model: "m".to_string(),
            system_prompt: "p".to_string(),
            word_gap: 20,
            line_gap: 15,
            ocr_server_executable: "x".to_string(),
            ocr_server_startup_timeout_secs: 30,
            ocr_server_request_timeout_secs: 20,
        }
    }

    #[test]
    fn sanitized는_score_thresh를_0과_1_사이로_클램프한다() {
        let s = UserSettings {
            score_thresh: 2.5,
            ..UserSettings::from_config(&base_config())
        }
        .validate();
        assert!((s.score_thresh - 1.0).abs() < f32::EPSILON);

        let s2 = UserSettings {
            score_thresh: -1.0,
            ..UserSettings::from_config(&base_config())
        }
        .validate();
        assert!((s2.score_thresh - 0.0).abs() < f32::EPSILON);
    }

    #[test]
    fn sanitized는_source를_en_또는_ch로_정규화한다() {
        let cfg = base_config();
        let a = UserSettings {
            source: "  CH  ".to_string(),
            ..UserSettings::from_config(&cfg)
        }
        .validate();
        assert_eq!(a.source, "ch");

        let b = UserSettings {
            source: "japanese".to_string(),
            ..UserSettings::from_config(&cfg)
        }
        .validate();
        assert_eq!(b.source, "en");
    }

    #[test]
    fn sanitized는_device를_cpu_또는_gpu로_정규화한다() {
        let cfg = base_config();
        let a = UserSettings {
            ocr_server_device: " GpU ".to_string(),
            ..UserSettings::from_config(&cfg)
        }
        .validate();
        assert_eq!(a.ocr_server_device, "gpu");

        let b = UserSettings {
            ocr_server_device: "cuda".to_string(),
            ..UserSettings::from_config(&cfg)
        }
        .validate();
        assert_eq!(b.ocr_server_device, "cpu");
    }

    #[test]
    fn sanitized는_음수_gap을_0으로_보정한다() {
        let s = UserSettings {
            word_gap: -3,
            line_gap: -7,
            ..UserSettings::from_config(&base_config())
        }
        .validate();
        assert_eq!(s.word_gap, 0);
        assert_eq!(s.line_gap, 0);
    }

    #[test]
    fn apply_to는_user_편집_필드만_config에_반영한다() {
        let mut cfg = base_config();
        let s = UserSettings {
            source: "ch".to_string(),
            score_thresh: 0.8,
            ocr_server_device: "gpu".to_string(),
            ai_gateway_api_key: "new-key".to_string(),
            ai_gateway_model: "new-model".to_string(),
            system_prompt: "한국어로 요약".to_string(),
            word_gap: 30,
            line_gap: 25,
        };
        s.apply_to(&mut cfg);

        assert_eq!(cfg.source, "ch");
        assert!((cfg.score_thresh - 0.8).abs() < f32::EPSILON);
        assert_eq!(cfg.ocr_server_device, "gpu");
        assert_eq!(cfg.ai_gateway_api_key, "new-key");
        assert_eq!(cfg.ai_gateway_model, "new-model");
        assert_eq!(cfg.system_prompt, "한국어로 요약");
        assert_eq!(cfg.word_gap, 30);
        assert_eq!(cfg.line_gap, 25);
        // 인프라 필드는 건드리지 않음
        assert_eq!(cfg.ocr_server_executable, "x");
        assert_eq!(cfg.ocr_server_startup_timeout_secs, 30);
    }

    #[test]
    fn save_to_env_file은_관리_키를_갱신한다() {
        let dir = std::env::temp_dir().join(format!(
            "buzhidao-env-settings-{}",
            std::time::SystemTime::now()
                .duration_since(std::time::UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        std::fs::create_dir_all(&dir).expect("임시 디렉토리 생성 실패");
        let path = dir.join(".env");
        std::fs::write(&path, "# comment\nSOURCE=ch\nAI_GATEWAY_MODEL=old\n")
            .expect(".env 작성 실패");

        let settings = UserSettings {
            source: "en".to_string(),
            score_thresh: 0.8,
            ocr_server_device: "cpu".to_string(),
            ai_gateway_api_key: "secret".to_string(),
            ai_gateway_model: "new-model".to_string(),
            system_prompt: "첫 줄\n둘째 줄".to_string(),
            word_gap: 20,
            line_gap: 15,
        };
        save_to_env_file(&path, &settings).expect(".env 저장 실패");

        let text = std::fs::read_to_string(&path).expect(".env 읽기 실패");
        assert!(text.contains("SOURCE=en"));
        assert!(text.contains("AI_GATEWAY_MODEL=new-model"));
        assert!(text.contains("AI_GATEWAY_API_KEY=secret"));
        assert!(text.contains("SYSTEM_PROMPT=첫 줄\\n둘째 줄"));

        let _ = std::fs::remove_dir_all(dir);
    }

    #[test]
    fn materialize_env_file은_example을_기준으로_기존값을_덮어쓴다() {
        let example = "# comment\nSOURCE=en\nAI_GATEWAY_MODEL=base\nWORD_GAP=20\n";
        let existing = "AI_GATEWAY_MODEL=override\nEXTRA_KEY=1\n";

        let merged = merge_env_example_with_existing(example, existing);

        assert!(merged.contains("# comment"));
        assert!(merged.contains("SOURCE=en"));
        assert!(merged.contains("AI_GATEWAY_MODEL=override"));
        assert!(merged.contains("WORD_GAP=20"));
        assert!(merged.contains("EXTRA_KEY=1"));
    }
}
