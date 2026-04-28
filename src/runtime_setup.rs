use std::borrow::Cow;
use std::env;
use std::path::PathBuf;
use tauri::Manager;

#[cfg(target_os = "linux")]
const PORTAL_APP_ID: &str = "com.buzhidao.desktop";

#[cfg(target_os = "linux")]
fn ensure_linux_desktop_entry() -> Result<PathBuf, String> {
    use std::fs;

    let apps_dir = dirs::home_dir()
        .ok_or("HOME 디렉토리를 찾을 수 없음".to_string())?
        .join(".local/share/applications");
    fs::create_dir_all(&apps_dir).map_err(|e| format!("desktop 디렉토리 생성 실패: {e}"))?;

    let desktop_path = apps_dir.join(format!("{PORTAL_APP_ID}.desktop"));
    if desktop_path.exists() {
        return Ok(desktop_path);
    }

    let exe = env::current_exe().map_err(|e| format!("실행 파일 경로 확인 실패: {e}"))?;
    let content = format!(
        "[Desktop Entry]\nType=Application\nName=buzhidao\nExec={}\nTerminal=false\nCategories=Utility;\nStartupNotify=false\n",
        exe.display()
    );
    fs::write(&desktop_path, content)
        .map_err(|e| format!("desktop 파일 생성 실패 ({}): {e}", desktop_path.display()))?;

    Ok(desktop_path)
}

#[cfg(target_os = "linux")]
pub(crate) fn register_linux_portal_host_app() -> Result<(), String> {
    use ashpd::zbus::blocking::{Connection, Proxy};
    use ashpd::zvariant::Value;
    use std::collections::HashMap;

    let _desktop_path = ensure_linux_desktop_entry()?;
    let connection = Connection::session().map_err(|e| format!("D-Bus 세션 연결 실패: {e}"))?;
    let proxy = Proxy::new(
        &connection,
        "org.freedesktop.portal.Desktop",
        "/org/freedesktop/portal/desktop",
        "org.freedesktop.host.portal.Registry",
    )
    .map_err(|e| format!("포털 registry 프록시 생성 실패: {e}"))?;
    let options = HashMap::<&str, Value<'_>>::new();

    proxy
        .call_method("Register", &(PORTAL_APP_ID, options))
        .map_err(|e| format!("포털 host app 등록 실패: {e}"))?;

    Ok(())
}

/// GPU 빌드에서는 `.env` 최초 생성 시 `OCR_SERVER_DEVICE` 기본값이 `gpu`가 되도록 치환한다.
pub(crate) fn default_env_example() -> Cow<'static, str> {
    const BASE: &str = include_str!("../.env.example");
    #[cfg(feature = "gpu")]
    {
        Cow::Owned(BASE.replace("OCR_SERVER_DEVICE=cpu", "OCR_SERVER_DEVICE=gpu"))
    }
    #[cfg(not(feature = "gpu"))]
    {
        Cow::Borrowed(BASE)
    }
}

#[cfg(any(target_os = "windows", target_os = "linux"))]
pub(crate) fn prepend_runtime_path_var(name: &str, dirs: impl IntoIterator<Item = PathBuf>) {
    let separator = if cfg!(target_os = "windows") {
        ";"
    } else {
        ":"
    };
    let mut entries: Vec<String> = dirs
        .into_iter()
        .filter(|dir| dir.is_dir())
        .map(|dir| dir.to_string_lossy().to_string())
        .collect();
    if entries.is_empty() {
        return;
    }

    if let Some(current) = env::var_os(name) {
        let current = current.to_string_lossy();
        if !current.is_empty() {
            entries.push(current.to_string());
        }
    }
    env::set_var(name, entries.join(separator));
}

#[cfg(target_os = "windows")]
pub(crate) fn prepare_gpu_runtime_search_path(app: &tauri::App, development_build: bool) {
    if !cfg!(feature = "gpu") {
        return;
    }

    let mut dirs = Vec::new();
    if development_build {
        dirs.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(".cuda"));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(parent) = exe.parent() {
            dirs.push(parent.join(".cuda"));
        }
    }
    if let Ok(resource_dir) = app.path().resource_dir() {
        dirs.push(resource_dir.join(".cuda"));
    }
    prepend_runtime_path_var("PATH", dirs);
}

#[cfg(target_os = "linux")]
pub(crate) fn prepare_gpu_runtime_search_path(app: &tauri::App, development_build: bool) {
    if !cfg!(feature = "gpu") {
        return;
    }

    let mut dirs = Vec::new();
    if development_build {
        dirs.push(PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(".cuda"));
    }
    if let Ok(exe) = env::current_exe() {
        if let Some(parent) = exe.parent() {
            dirs.push(parent.join(".cuda"));
        }
    }
    if let Ok(resource_dir) = app.path().resource_dir() {
        dirs.push(resource_dir.join(".cuda"));
    }
    prepend_runtime_path_var("LD_LIBRARY_PATH", dirs);
}

#[cfg(not(any(target_os = "windows", target_os = "linux")))]
pub(crate) fn prepare_gpu_runtime_search_path(_app: &tauri::App, _development_build: bool) {}
