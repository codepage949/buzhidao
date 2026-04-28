import json
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import NamedTuple


REPO_ROOT = Path(__file__).resolve().parents[2]
OCR_SIDECAR_COMPARE_PY = REPO_ROOT / "tools" / "ocr_sidecar_compare" / "ocr_sidecar_compare.py"
OCR_SIDECAR_COMPARE_PYTHON_CANDIDATES = [
    REPO_ROOT / "tools" / "ocr_sidecar_compare" / ".venv" / "Scripts" / "python.exe",
    REPO_ROOT / "tools" / "ocr_sidecar_compare" / ".venv" / "bin" / "python",
]
LINUX_INSTALL_DEPS_SCRIPT = REPO_ROOT / "tools" / "scripts" / "install_linux_build_deps.sh"
DEFAULT_IMAGES = [
    REPO_ROOT / "testdata" / "ocr" / "test.png",
    REPO_ROOT / "testdata" / "ocr" / "test2.png",
    REPO_ROOT / "testdata" / "ocr" / "test3.png",
]
APP_PIPELINE_MAX_WIDTH = 1024
APP_PIPELINE_MAX_LONG_SIDE = 1024
LEGACY_SIDECAR_APP_MAX_WIDTH = 1024
FFI_COMPARE_TEST_NAME = "ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다"
FFI_BENCH_TEST_NAME = "ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다"
APP_OCR_COMPARE_TEST_NAME = (
    "services::ocr_pipeline::tests::_1_png를_앱_ocr_경로로_실행해서_결과를_출력한다"
)
APP_OCR_BENCH_TEST_NAME = (
    "services::ocr_pipeline::tests::지정한_이미지들로_앱_ocr_경로_지연시간을_측정한다"
)


def resolve_ocr_sidecar_compare_python() -> Path:
    override = os.environ.get("OCR_SERVER_PYTHON")
    if override:
        return Path(override).resolve()
    for candidate in OCR_SIDECAR_COMPARE_PYTHON_CANDIDATES:
        if candidate.exists():
            return candidate
    fallback = shutil.which("python3") or shutil.which("python")
    if fallback is not None:
        return Path(fallback)
    return OCR_SIDECAR_COMPARE_PYTHON_CANDIDATES[0]


OCR_SERVER_PYTHON = resolve_ocr_sidecar_compare_python()
PROFILE_LOG_DIR = Path(tempfile.gettempdir())
SIDECAR_PROFILE_LOG = PROFILE_LOG_DIR / "buzhi-ocr-sidecar-profile.log"
FFI_PROFILE_LOG = PROFILE_LOG_DIR / "buzhi-ocr-ffi-profile.log"
SIDECAR_PROFILE_PREFIX = "[buzhi_ocr_sidecar_profile] "
FFI_PROFILE_PREFIX = "[buzhi_ocr_profile] "


class PreparedImage(NamedTuple):
    sidecar_path: Path
    ffi_path: Path
    cleanup_paths: tuple[Path, ...]


class PreparedSingleImage(NamedTuple):
    path: Path
    cleanup_paths: tuple[Path, ...]


def resolve_rec_config_path(source: str) -> Path | None:
    normalized = source.strip().lower()
    model_name = "PP-OCRv5_server_rec" if normalized in {"ch", "cn", "zh", "chinese"} else None
    if model_name is None:
        return None
    candidates = [
        Path.home() / ".paddlex" / "official_models" / model_name / "config.json",
        Path.home() / ".paddleocr" / model_name / "config.json",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return None


def load_rec_dict_from_config(config_path: Path) -> list[str]:
    payload = json.loads(config_path.read_text(encoding="utf-8"))
    post_process = payload.get("PostProcess") or {}
    raw_dict = post_process.get("character_dict")
    if not isinstance(raw_dict, list):
        raise ValueError(f"character_dict를 찾지 못했습니다: {config_path}")
    return [str(item) for item in raw_dict]


def parse_env_assignments(values: list[str]) -> dict[str, str]:
    env: dict[str, str] = {}
    for raw in values:
        if "=" not in raw:
            raise ValueError(f"환경변수 형식이 잘못되었습니다: {raw!r} (KEY=VALUE 필요)")
        key, value = raw.split("=", 1)
        key = key.strip()
        if not key:
            raise ValueError(f"환경변수 이름이 비어 있습니다: {raw!r}")
        env[key] = value
    return env


def normalize_pipeline_resize_mode(mode: str) -> str:
    return "width" if mode == "width" else "long-side"


def effective_resize_max_height(requested: int, ffi_mode: str) -> int:
    if requested > 0:
        return requested
    return 0


def effective_resize_max_width(requested: int, ffi_mode: str, pipeline_resize_mode: str) -> int:
    if requested > 0:
        return requested
    if ffi_mode == "pipeline" and normalize_pipeline_resize_mode(pipeline_resize_mode) == "width":
        return APP_PIPELINE_MAX_WIDTH
    return 0


def effective_pipeline_resize_limits(
    image_width: int,
    image_height: int,
    resize_max_height: int,
    resize_max_width: int,
    ffi_mode: str,
    pipeline_resize_mode: str,
) -> tuple[int, int]:
    normalized_mode = normalize_pipeline_resize_mode(pipeline_resize_mode)
    height_limit = effective_resize_max_height(resize_max_height, ffi_mode)
    width_limit = effective_resize_max_width(resize_max_width, ffi_mode, normalized_mode)
    if ffi_mode != "pipeline" or normalized_mode != "long-side":
        return height_limit, width_limit
    if image_width >= image_height:
        return 0, APP_PIPELINE_MAX_LONG_SIDE if image_width > APP_PIPELINE_MAX_LONG_SIDE else 0
    return APP_PIPELINE_MAX_LONG_SIDE if image_height > APP_PIPELINE_MAX_LONG_SIDE else 0, 0


def pkg_config_can_find(lib_name: str, env: dict[str, str]) -> tuple[bool, str]:
    pkg_config = shutil.which("pkg-config")
    if pkg_config is None:
        return False, "pkg-config 실행 파일을 찾을 수 없습니다."
    result = subprocess.run(
        [pkg_config, "--libs", "--cflags", lib_name],
        capture_output=True,
        text=True,
        encoding="utf-8",
        env=env,
        check=False,
    )
    if result.returncode == 0:
        return True, ""
    return False, (result.stderr or result.stdout).strip()


def inject_pkg_config_paths(env: dict[str, str]) -> dict[str, str]:
    if os.name != "posix" or "linux" not in sys.platform:
        return env

    common_paths = [
        "/usr/lib/x86_64-linux-gnu/pkgconfig",
        "/usr/lib/i386-linux-gnu/pkgconfig",
        "/usr/lib/aarch64-linux-gnu/pkgconfig",
        "/usr/local/lib/pkgconfig",
        "/usr/local/lib/x86_64-linux-gnu/pkgconfig",
        "/usr/share/pkgconfig",
        "/usr/lib/pkgconfig",
    ]
    merged = [p for p in common_paths if Path(p).is_dir()]
    existing = env.get("PKG_CONFIG_PATH", "")
    if existing:
        merged.extend(p for p in existing.split(os.pathsep) if p and p not in merged)
    env["PKG_CONFIG_PATH"] = os.pathsep.join(dict.fromkeys(merged))
    return env


def collect_native_lib_paths() -> list[Path]:
    extra_paths: list[Path] = [
        REPO_ROOT / ".paddle_inference" / "lib",
        REPO_ROOT / ".paddle_inference" / "third_party" / "install" / "mklml" / "lib",
        REPO_ROOT / ".paddle_inference" / "third_party" / "install" / "onednn" / "lib",
    ]
    openvino_lib = REPO_ROOT / ".paddle_inference" / "third_party" / "install" / "openvino" / "intel64"
    if openvino_lib.exists():
        extra_paths.append(openvino_lib)
    install_root = REPO_ROOT / ".paddle_inference" / "third_party" / "install"
    if install_root.exists():
        for so_file in install_root.rglob("*.so*"):
            parent = so_file.parent
            if parent not in extra_paths and "3rdparty" not in str(parent):
                extra_paths.append(parent)
    return [path for path in extra_paths if path.exists()]


def ensure_ffi_system_deps(auto_install: bool, env: dict[str, str]) -> None:
    if os.name != "posix" or "linux" not in sys.platform:
        return
    env = inject_pkg_config_paths(env)
    ok, reason = pkg_config_can_find("libpipewire-0.3", env)
    if ok:
        return
    if not auto_install:
        raise RuntimeError(
            "ffi 실행을 위해 libpipewire-0.3 시스템 라이브러리가 필요합니다.\n"
            "다음 중 하나를 실행하세요.\n"
            "  - tools/scripts/install_linux_build_deps.sh\n"
            "  - sudo apt-get install -y libpipewire-0.3-dev\n"
            f"pkg-config 오류: {reason}"
        )
    if not LINUX_INSTALL_DEPS_SCRIPT.exists():
        raise RuntimeError("Linux 의존성 설치 스크립트를 찾을 수 없습니다.")
    subprocess.run(["bash", str(LINUX_INSTALL_DEPS_SCRIPT)], check=True)
    ok, reason = pkg_config_can_find("libpipewire-0.3", env)
    if not ok:
        raise RuntimeError(
            "자동 설치 후에도 libpipewire-0.3를 찾지 못했습니다.\n"
            f"pkg-config 오류: {reason}"
        )


def build_ffi_env() -> dict[str, str]:
    env = os.environ.copy()
    lib_dirs = [str(path) for path in collect_native_lib_paths()]
    if os.name == "nt":
        base_path = env.get("PATH", "")
        env["PATH"] = os.pathsep.join(lib_dirs + ([base_path] if base_path else []))
    else:
        base_lib_path = env.get("LD_LIBRARY_PATH", "")
        env["LD_LIBRARY_PATH"] = os.pathsep.join(lib_dirs + ([base_lib_path] if base_lib_path else []))
        if sys.platform == "darwin":
            base_dyld = env.get("DYLD_LIBRARY_PATH", "")
            env["DYLD_LIBRARY_PATH"] = os.pathsep.join(lib_dirs + ([base_dyld] if base_dyld else []))
    env = inject_pkg_config_paths(env)
    env["OCR_SERVER_PYTHON"] = str(OCR_SERVER_PYTHON)
    return env


def resolve_images(raw_images: list[str]) -> list[Path]:
    if not raw_images:
        return DEFAULT_IMAGES
    return [Path(item).resolve() for item in raw_images]


def json_lines(stdout: str) -> list[dict]:
    return [
        json.loads(line.strip())
        for line in stdout.splitlines()
        if line.strip().startswith("{")
    ]


def clear_profile_log(path: Path) -> None:
    try:
        path.unlink(missing_ok=True)
    except OSError:
        pass


def read_profile_log(path: Path, prefix: str) -> list[str]:
    if not path.is_file():
        return []
    return [
        line[len(prefix) :].strip()
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
        if line.strip().startswith(prefix)
    ]


def _coerce_profile_value(raw: str) -> object:
    raw = raw.rstrip(",")
    lowered = raw.lower()
    if lowered in {"true", "false"}:
        return lowered == "true"
    try:
        if any(ch in raw for ch in (".", "e", "E")):
            return float(raw)
        return int(raw)
    except ValueError:
        return raw


def parse_profile_line(line: str) -> dict[str, object]:
    parsed: dict[str, object] = {"raw": line}
    for token in line.split():
        if "=" not in token:
            continue
        key, value = token.split("=", 1)
        parsed[key] = _coerce_profile_value(value)
    return parsed


def parse_profile_lines(lines: Iterable[str]) -> list[dict[str, object]]:
    return [parse_profile_line(line) for line in lines]


def latest_pipeline_profile_entry(entries: Iterable[dict[str, object]]) -> dict[str, object] | None:
    latest: dict[str, object] | None = None
    for entry in entries:
        raw = entry.get("raw")
        if isinstance(raw, str) and raw.startswith("run_pipeline profile "):
            latest = entry
    return latest


def summarize_benchmark(entry: dict) -> dict:
    values = [float(v) for v in entry["elapsed_ms"]]
    return {
        "image": Path(entry["image"]).name,
        "detection_count": int(entry["detection_count"]),
        "mean_ms": statistics.fmean(values),
        "median_ms": statistics.median(values),
        "min_ms": min(values),
        "max_ms": max(values),
        "samples": values,
    }
