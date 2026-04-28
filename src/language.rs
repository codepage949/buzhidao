use std::sync::LazyLock;

#[derive(serde::Deserialize)]
struct LangEntry {
    code: String,
}

static SUPPORTED_LANG_CODES: LazyLock<Vec<String>> = LazyLock::new(|| {
    let entries: Vec<LangEntry> = serde_json::from_str(include_str!("../shared/langs.json"))
        .expect("shared/langs.json 파싱 실패");
    entries.into_iter().map(|entry| entry.code).collect()
});

pub(crate) fn is_supported_source(source: &str) -> bool {
    SUPPORTED_LANG_CODES.iter().any(|code| code == source)
}

pub(crate) fn normalize_app_source(source: &str) -> String {
    let source = source.trim().to_ascii_lowercase();
    if source.is_empty() {
        return "en".to_string();
    }
    if is_supported_source(&source) {
        return source;
    }
    if source == "en" || source == "eng" || source == "english" {
        return "en".to_string();
    }
    if source == "cn" || source == "zh" || source == "chi" || source == "chinese" {
        return "ch".to_string();
    }
    if source.starts_with("ch_") || source.starts_with("zh-") || source.starts_with("zh_") {
        return "ch".to_string();
    }
    "en".to_string()
}

pub(crate) fn normalize_upstream_source(source: &str) -> String {
    let normalized = source.trim().to_ascii_lowercase();
    if normalized.is_empty() {
        return "en".to_string();
    }
    if normalized == "cn" || normalized == "zh" || normalized == "chi" || normalized == "chinese" {
        return "ch".to_string();
    }
    if normalized == "ch_tra"
        || normalized == "chinese_cht"
        || normalized == "zh-tw"
        || normalized == "zh_tw"
        || normalized == "zh-hant"
        || normalized == "zh_hant"
    {
        return "chinese_cht".to_string();
    }
    if normalized.starts_with("zh-") || normalized.starts_with("zh_") {
        return "ch".to_string();
    }
    if normalized == "en" || normalized == "eng" || normalized == "english" {
        return "en".to_string();
    }
    if is_supported_source(&normalized) {
        return normalized;
    }
    "en".to_string()
}

#[cfg(test)]
mod tests {
    use super::{normalize_app_source, normalize_upstream_source};

    #[test]
    fn 앱_언어_정규화는_지원_코드와_중국어_alias를_처리한다() {
        assert_eq!(normalize_app_source(" ch "), "ch");
        assert_eq!(normalize_app_source("cn"), "ch");
        assert_eq!(normalize_app_source("zh-CN"), "ch");
        assert_eq!(normalize_app_source("ch_tra"), "ch_tra");
        assert_eq!(normalize_app_source("unknown"), "en");
    }

    #[test]
    fn 업스트림_언어_정규화는_모델명_규칙에_맞춘다() {
        assert_eq!(normalize_upstream_source("ch_tra"), "chinese_cht");
        assert_eq!(normalize_upstream_source("zh-TW"), "chinese_cht");
        assert_eq!(normalize_upstream_source("cn"), "ch");
        assert_eq!(normalize_upstream_source("unknown"), "en");
    }
}
