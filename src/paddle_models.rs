use crate::language::normalize_upstream_source;
use std::fs;
use std::io::Cursor;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const PADDLE_MODEL_BASE_URL: &str =
    "https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0";
const PADDLE_MODEL_ROOT_ENV: &str = crate::env_keys::PADDLE_MODEL_ROOT;
const DET_MODEL_NAME: &str = "PP-OCRv5_server_det";
const CLS_MODEL_NAME: &str = "PP-LCNet_x1_0_textline_ori";

static LATIN_LANGS: &[&str] = &[
    "af", "az", "bs", "cs", "cy", "da", "de", "es", "et", "fr", "ga", "hr", "hu", "id", "is", "it",
    "ku", "la", "lt", "lv", "mi", "ms", "mt", "nl", "no", "oc", "pi", "pl", "pt", "ro", "rs_latin",
    "sk", "sl", "sq", "sv", "sw", "tl", "tr", "uz", "vi", "french", "german", "fi", "eu", "gl",
    "lb", "rm", "ca", "qu",
];
static ARABIC_LANGS: &[&str] = &["ar", "fa", "ug", "ur", "ps", "ku", "sd", "bal"];
static ESLAV_LANGS: &[&str] = &["ru", "be", "uk"];
static CYRILLIC_LANGS: &[&str] = &[
    "ru",
    "rs_cyrillic",
    "be",
    "bg",
    "uk",
    "mn",
    "abq",
    "ady",
    "kbd",
    "ava",
    "dar",
    "inh",
    "che",
    "lbe",
    "lez",
    "tab",
    "kk",
    "ky",
    "tg",
    "mk",
    "tt",
    "cv",
    "ba",
    "mhr",
    "mo",
    "udm",
    "kv",
    "os",
    "bua",
    "xal",
    "tyv",
    "sah",
    "kaa",
];
static DEVANAGARI_LANGS: &[&str] = &[
    "hi", "mr", "ne", "bh", "mai", "ang", "bho", "mah", "sck", "new", "gom", "sa", "bgc",
];
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PaddleModelSpec {
    pub(crate) det: &'static str,
    pub(crate) cls: &'static str,
    pub(crate) rec: &'static str,
    pub(crate) source: String,
}

pub(crate) async fn ensure_paddle_models_for_lang(lang: &str) -> Result<PathBuf, String> {
    let resolved = resolve_paddle_model_dir_for_lang(lang);
    if let Some(root) = resolved {
        return Ok(root);
    }

    let root = default_paddle_model_root()
        .ok_or_else(|| "PaddleOCR 캐시 루트를 결정하지 못했습니다".to_string())?;
    ensure_paddle_models_for_lang_in_root(lang, &root).await?;
    Ok(root)
}

pub(crate) async fn ensure_paddle_models_for_lang_in_root(
    lang: &str,
    root: &Path,
) -> Result<(), String> {
    fs::create_dir_all(root).map_err(|e| {
        format!(
            "PaddleOCR 캐시 디렉터리 생성 실패 ({}): {e}",
            root.display()
        )
    })?;
    let spec = model_spec_for_lang(lang);
    for model_name in [spec.det, spec.cls, spec.rec] {
        ensure_single_model(root, model_name).await?;
    }
    Ok(())
}

#[cfg_attr(
    not(all(feature = "paddle-ffi", has_paddle_inference)),
    allow(dead_code)
)]
pub(crate) fn validate_paddle_model_root_for_lang(lang: &str, root: &Path) -> Result<(), String> {
    let spec = model_spec_for_lang(lang);
    let diagnostics = [
        model_dir_diagnostic(root, "det", spec.det),
        model_dir_diagnostic(root, "cls", spec.cls),
        model_dir_diagnostic(root, "rec", spec.rec),
    ];
    let failures = diagnostics
        .iter()
        .filter(|diagnostic| !diagnostic.ok)
        .map(ModelDirDiagnostic::message)
        .collect::<Vec<_>>();

    if failures.is_empty() {
        return Ok(());
    }

    Err(format!(
        "Paddle 모델 루트 검증 실패: root={}, source={}, {}",
        root.display(),
        spec.source,
        failures.join("; ")
    ))
}

pub(crate) fn resolve_paddle_model_dir_for_lang(lang: &str) -> Option<PathBuf> {
    resolve_paddle_model_dir_for_lang_with_roots(lang, paddle_ocr_cache_roots())
}

pub(crate) fn resolve_paddle_model_dir_for_lang_with_roots<I>(
    lang: &str,
    roots: I,
) -> Option<PathBuf>
where
    I: IntoIterator<Item = PathBuf>,
{
    let spec = model_spec_for_lang(lang);
    roots.into_iter().find(|root| {
        has_model_dir(root, spec.det)
            && has_model_dir(root, spec.cls)
            && has_model_dir(root, spec.rec)
    })
}

pub(crate) fn paddle_ocr_cache_roots() -> Vec<PathBuf> {
    let mut roots = Vec::new();
    if let Some(root) = configured_paddle_model_root() {
        roots.push(root);
    }
    if let Some(home) = dirs::home_dir() {
        roots.push(home.join(".paddlex").join("official_models"));
        roots.push(home.join(".paddleocr"));
    }
    roots
}

pub(crate) fn default_paddle_model_root() -> Option<PathBuf> {
    paddle_ocr_cache_roots().into_iter().next()
}

fn configured_paddle_model_root() -> Option<PathBuf> {
    let raw = std::env::var_os(PADDLE_MODEL_ROOT_ENV)?;
    if raw.is_empty() {
        return None;
    }
    Some(PathBuf::from(raw))
}

pub(crate) fn model_spec_for_lang(lang: &str) -> PaddleModelSpec {
    let source = normalize_upstream_source(lang);
    let rec = match source.as_str() {
        "ch" | "chinese_cht" | "japan" => "PP-OCRv5_server_rec",
        "en" => "en_PP-OCRv5_mobile_rec",
        "korean" => "korean_PP-OCRv5_mobile_rec",
        "th" => "th_PP-OCRv5_mobile_rec",
        "el" => "el_PP-OCRv5_mobile_rec",
        "te" => "te_PP-OCRv5_mobile_rec",
        "ta" => "ta_PP-OCRv5_mobile_rec",
        value if LATIN_LANGS.contains(&value) => "latin_PP-OCRv5_mobile_rec",
        value if ESLAV_LANGS.contains(&value) => "eslav_PP-OCRv5_mobile_rec",
        value if ARABIC_LANGS.contains(&value) => "arabic_PP-OCRv5_mobile_rec",
        value if CYRILLIC_LANGS.contains(&value) => "cyrillic_PP-OCRv5_mobile_rec",
        value if DEVANAGARI_LANGS.contains(&value) => "devanagari_PP-OCRv5_mobile_rec",
        _ => "en_PP-OCRv5_mobile_rec",
    };

    PaddleModelSpec {
        det: DET_MODEL_NAME,
        cls: CLS_MODEL_NAME,
        rec,
        source,
    }
}

fn has_model_dir(root: &Path, model_name: &str) -> bool {
    let model_dir = root.join(model_name);
    has_inference_files_in_dir(&model_dir)
}

fn has_inference_files_in_dir(dir: &Path) -> bool {
    if !dir.is_dir() {
        return false;
    }
    let infer_json = dir.join("inference.json");
    let infer_pdmodel = dir.join("inference.pdmodel");
    let infer_params = dir.join("inference.pdiparams");
    (infer_json.is_file() || infer_pdmodel.is_file()) && infer_params.is_file()
}

#[cfg_attr(
    not(all(feature = "paddle-ffi", has_paddle_inference)),
    allow(dead_code)
)]
struct ModelDirDiagnostic {
    role: &'static str,
    model_name: &'static str,
    dir_exists: bool,
    has_model_file: bool,
    has_params_file: bool,
    ok: bool,
}

#[cfg_attr(
    not(all(feature = "paddle-ffi", has_paddle_inference)),
    allow(dead_code)
)]
impl ModelDirDiagnostic {
    fn message(&self) -> String {
        format!(
            "{}({}): dir={}, model_file={}, params={}",
            self.role, self.model_name, self.dir_exists, self.has_model_file, self.has_params_file
        )
    }
}

#[cfg_attr(
    not(all(feature = "paddle-ffi", has_paddle_inference)),
    allow(dead_code)
)]
fn model_dir_diagnostic(
    root: &Path,
    role: &'static str,
    model_name: &'static str,
) -> ModelDirDiagnostic {
    let dir = root.join(model_name);
    let dir_exists = dir.is_dir();
    let has_model_file =
        dir.join("inference.json").is_file() || dir.join("inference.pdmodel").is_file();
    let has_params_file = dir.join("inference.pdiparams").is_file();
    ModelDirDiagnostic {
        role,
        model_name,
        dir_exists,
        has_model_file,
        has_params_file,
        ok: dir_exists && has_model_file && has_params_file,
    }
}

async fn ensure_single_model(root: &Path, model_name: &str) -> Result<(), String> {
    if has_model_dir(root, model_name) {
        return Ok(());
    }

    let url = official_model_url(model_name);
    let response = reqwest::get(&url)
        .await
        .map_err(|e| format!("Paddle 모델 다운로드 실패 ({model_name}): {e}"))?;
    if !response.status().is_success() {
        return Err(format!(
            "Paddle 모델 다운로드 실패 ({model_name}): HTTP {}",
            response.status()
        ));
    }
    let bytes = response
        .bytes()
        .await
        .map_err(|e| format!("Paddle 모델 다운로드 본문 읽기 실패 ({model_name}): {e}"))?;

    unpack_model_archive(root, model_name, bytes.as_ref())
}

fn unpack_model_archive(root: &Path, model_name: &str, archive_bytes: &[u8]) -> Result<(), String> {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|e| format!("시스템 시간 오류: {e}"))?
        .as_nanos();
    let staging_root = root.join(format!(".download-{model_name}-{stamp}"));
    fs::create_dir_all(&staging_root).map_err(|e| {
        format!(
            "Paddle 모델 임시 디렉터리 생성 실패 ({}): {e}",
            staging_root.display()
        )
    })?;

    let unpack_result = (|| {
        let cursor = Cursor::new(archive_bytes);
        let mut archive = tar::Archive::new(cursor);
        archive.unpack(&staging_root).map_err(|e| {
            format!(
                "Paddle 모델 압축 해제 실패 ({model_name}, {}): {e}",
                staging_root.display()
            )
        })?;

        let extracted = locate_extracted_model_dir(&staging_root, model_name)?;
        let final_dir = root.join(model_name);
        if final_dir.exists() {
            fs::remove_dir_all(&final_dir).map_err(|e| {
                format!(
                    "기존 Paddle 모델 디렉터리 정리 실패 ({}): {e}",
                    final_dir.display()
                )
            })?;
        }
        fs::rename(&extracted, &final_dir).map_err(|e| {
            format!(
                "Paddle 모델 배치 실패 ({} -> {}): {e}",
                extracted.display(),
                final_dir.display()
            )
        })?;

        if !has_inference_files_in_dir(&final_dir) {
            return Err(format!(
                "Paddle 모델 압축 해제 결과가 유효하지 않습니다: {}",
                final_dir.display()
            ));
        }

        Ok(())
    })();

    let _ = fs::remove_dir_all(&staging_root);
    unpack_result
}

fn locate_extracted_model_dir(staging_root: &Path, model_name: &str) -> Result<PathBuf, String> {
    let expected_names = [format!("{model_name}_infer"), model_name.to_string()];
    for expected_name in &expected_names {
        let path = staging_root.join(expected_name);
        if has_inference_files_in_dir(&path) {
            return Ok(path);
        }
    }

    let entries = fs::read_dir(staging_root).map_err(|e| {
        format!(
            "Paddle 모델 압축 해제 결과를 읽지 못했습니다 ({}): {e}",
            staging_root.display()
        )
    })?;
    for entry in entries.flatten() {
        let path = entry.path();
        if has_inference_files_in_dir(&path) {
            return Ok(path);
        }
    }

    Err(format!(
        "Paddle 모델 압축 해제 결과에서 모델 디렉터리를 찾지 못했습니다 ({model_name})"
    ))
}

fn official_model_url(model_name: &str) -> String {
    format!("{PADDLE_MODEL_BASE_URL}/{model_name}_infer.tar")
}

#[cfg(test)]
mod tests {
    use super::{
        model_spec_for_lang, normalize_upstream_source, official_model_url, paddle_ocr_cache_roots,
        resolve_paddle_model_dir_for_lang_with_roots, validate_paddle_model_root_for_lang,
        PADDLE_MODEL_ROOT_ENV,
    };
    use std::fs;
    use std::path::PathBuf;
    use std::sync::{LazyLock, Mutex};
    use std::time::{SystemTime, UNIX_EPOCH};

    static ENV_LOCK: LazyLock<Mutex<()>> = LazyLock::new(|| Mutex::new(()));

    fn temp_path(prefix: &str) -> PathBuf {
        let nanos = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .expect("시계가 UNIX_EPOCH 이전입니다")
            .as_nanos();
        std::env::temp_dir().join(format!("{prefix}-{nanos}"))
    }

    fn write_model_dir(root: &PathBuf, name: &str) {
        let dir = root.join(name);
        fs::create_dir_all(&dir).expect("모델 디렉터리 생성 실패");
        fs::write(dir.join("inference.json"), b"{}").expect("inference.json 생성 실패");
        fs::write(dir.join("inference.pdiparams"), b"param")
            .expect("inference.pdiparams 생성 실패");
    }

    #[test]
    fn 업스트림_언어_정규화는_번체를_chinese_cht로_바꾼다() {
        assert_eq!(normalize_upstream_source("ch_tra"), "chinese_cht");
        assert_eq!(normalize_upstream_source("zh-TW"), "chinese_cht");
        assert_eq!(normalize_upstream_source("zh"), "ch");
        assert_eq!(normalize_upstream_source("unknown"), "en");
    }

    #[test]
    fn 업스트림_규칙으로_rec_모델명을_선택한다() {
        assert_eq!(model_spec_for_lang("ch").rec, "PP-OCRv5_server_rec");
        assert_eq!(model_spec_for_lang("ch_tra").rec, "PP-OCRv5_server_rec");
        assert_eq!(model_spec_for_lang("japan").rec, "PP-OCRv5_server_rec");
        assert_eq!(model_spec_for_lang("en").rec, "en_PP-OCRv5_mobile_rec");
        assert_eq!(model_spec_for_lang("fr").rec, "latin_PP-OCRv5_mobile_rec");
        assert_eq!(model_spec_for_lang("ru").rec, "eslav_PP-OCRv5_mobile_rec");
        assert_eq!(
            model_spec_for_lang("mn").rec,
            "cyrillic_PP-OCRv5_mobile_rec"
        );
        assert_eq!(model_spec_for_lang("ar").rec, "arabic_PP-OCRv5_mobile_rec");
        assert_eq!(
            model_spec_for_lang("hi").rec,
            "devanagari_PP-OCRv5_mobile_rec"
        );
        assert_eq!(
            model_spec_for_lang("korean").rec,
            "korean_PP-OCRv5_mobile_rec"
        );
    }

    #[test]
    fn 공식_모델_url을_구성한다() {
        assert_eq!(
            official_model_url("en_PP-OCRv5_mobile_rec"),
            "https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0/en_PP-OCRv5_mobile_rec_infer.tar"
        );
    }

    #[test]
    fn 언어별_필수_모델이_있는_캐시_루트를_선택한다() {
        let root = temp_path("buzhidao-paddle-model-root");
        fs::create_dir_all(&root).expect("캐시 루트 생성 실패");
        write_model_dir(&root, "PP-OCRv5_server_det");
        write_model_dir(&root, "PP-LCNet_x1_0_textline_ori");
        write_model_dir(&root, "latin_PP-OCRv5_mobile_rec");

        let resolved = resolve_paddle_model_dir_for_lang_with_roots("fr", vec![root.clone()]);
        assert_eq!(resolved, Some(root.clone()));

        let unresolved = resolve_paddle_model_dir_for_lang_with_roots("korean", vec![root.clone()]);
        assert_eq!(unresolved, None);

        let _ = fs::remove_dir_all(root);
    }

    #[test]
    fn 모델_루트_검증은_필수_det_cls_rec_파일을_확인한다() {
        let root = temp_path("buzhidao-paddle-model-validation");
        fs::create_dir_all(&root).expect("캐시 루트 생성 실패");
        write_model_dir(&root, "PP-OCRv5_server_det");
        write_model_dir(&root, "PP-LCNet_x1_0_textline_ori");
        write_model_dir(&root, "en_PP-OCRv5_mobile_rec");

        let result = validate_paddle_model_root_for_lang("en", &root);

        let _ = fs::remove_dir_all(root);
        assert!(result.is_ok());
    }

    #[test]
    fn 모델_루트_검증_실패는_누락된_역할을_알려준다() {
        let root = temp_path("buzhidao-paddle-model-validation-missing");
        fs::create_dir_all(&root).expect("캐시 루트 생성 실패");
        write_model_dir(&root, "PP-OCRv5_server_det");
        write_model_dir(&root, "en_PP-OCRv5_mobile_rec");

        let error = validate_paddle_model_root_for_lang("en", &root)
            .expect_err("cls 모델 누락은 오류여야 한다");

        let _ = fs::remove_dir_all(root);
        assert!(error.contains("cls(PP-LCNet_x1_0_textline_ori)"));
        assert!(error.contains("dir=false"));
    }

    #[test]
    fn 환경변수_모델_루트를_기본_캐시보다_먼저_사용한다() {
        let _guard = ENV_LOCK.lock().expect("환경 변수 테스트 락 획득 실패");
        let root = temp_path("buzhidao-env-paddle-model-root");
        let previous = std::env::var_os(PADDLE_MODEL_ROOT_ENV);
        std::env::set_var(PADDLE_MODEL_ROOT_ENV, &root);

        let roots = paddle_ocr_cache_roots();

        if let Some(previous) = previous {
            std::env::set_var(PADDLE_MODEL_ROOT_ENV, previous);
        } else {
            std::env::remove_var(PADDLE_MODEL_ROOT_ENV);
        }
        assert_eq!(roots.first(), Some(&root));
    }
}
