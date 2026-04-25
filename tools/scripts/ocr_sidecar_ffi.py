import argparse
import ast
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
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


def prepare_image_paths(
    image_path: Path,
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
) -> PreparedImage:
    sidecar_format = sidecar_format.lower()
    ffi_format = ffi_format.lower()
    cleanup_paths: list[Path] = []

    identify = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            "-c",
            (
                "from PIL import Image\n"
                "import sys\n"
                "src = Image.open(sys.argv[1])\n"
                "print(f'{src.width} {src.height}')\n"
            ),
            str(image_path),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if identify.returncode != 0:
        raise RuntimeError(
            f"입력 이미지 크기 확인 실패 for {image_path.name}: {(identify.stderr or identify.stdout).strip()}"
        )
    image_width, image_height = [int(value) for value in identify.stdout.strip().split()]
    resize_max_height, resize_max_width = effective_pipeline_resize_limits(
        image_width,
        image_height,
        resize_max_height,
        resize_max_width,
        ffi_mode,
        pipeline_resize_mode,
    )

    if resize_max_height <= 0 and resize_max_width <= 0:
        sidecar_path = image_path
        if image_path.suffix.lower() == f".{sidecar_format}":
            sidecar_path = image_path
        else:
            fd, temp_name = tempfile.mkstemp(prefix="buzhidao-sidecar-", suffix=f".{sidecar_format}")
            os.close(fd)
            sidecar_path = Path(temp_name)
            cleanup_paths.append(sidecar_path)

        if image_path.suffix.lower() == f".{ffi_format}":
            ffi_path = image_path
        else:
            fd, temp_name = tempfile.mkstemp(prefix="buzhidao-ffi-", suffix=f".{ffi_format}")
            os.close(fd)
            ffi_path = Path(temp_name)
            cleanup_paths.append(ffi_path)

        if cleanup_paths:
            proc = subprocess.run(
                [
                    str(OCR_SERVER_PYTHON),
                    "-c",
                    (
                        "from PIL import Image\n"
                        "import sys\n"
                        "src = Image.open(sys.argv[1]).convert('RGBA')\n"
                        "src.save(sys.argv[2])\n"
                        "src.save(sys.argv[3])\n"
                    ),
                    str(image_path),
                    str(sidecar_path),
                    str(ffi_path),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                encoding="utf-8",
                check=False,
            )
            if proc.returncode != 0:
                for path in cleanup_paths:
                    try:
                        path.unlink(missing_ok=True)
                    except OSError:
                        pass
                raise RuntimeError(
                    f"입력 이미지 포맷 준비 실패 for {image_path.name}: {(proc.stderr or proc.stdout).strip()}"
                )
        return PreparedImage(sidecar_path, ffi_path, tuple(cleanup_paths))

    fd_sidecar, temp_sidecar_name = tempfile.mkstemp(
        prefix="buzhidao-sidecar-", suffix=f".{sidecar_format}"
    )
    os.close(fd_sidecar)
    fd_ffi, temp_ffi_name = tempfile.mkstemp(prefix="buzhidao-ffi-", suffix=f".{ffi_format}")
    os.close(fd_ffi)
    temp_sidecar = Path(temp_sidecar_name)
    temp_ffi = Path(temp_ffi_name)
    proc = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            "-c",
            (
                "from PIL import Image\n"
                "import sys\n"
                "src = Image.open(sys.argv[1]).convert('RGBA')\n"
                "max_h = int(sys.argv[4])\n"
                "max_w = int(sys.argv[5])\n"
                "ratio_h = (max_h / float(src.height)) if max_h > 0 and src.height > max_h else 1.0\n"
                "ratio_w = (max_w / float(src.width)) if max_w > 0 and src.width > max_w else 1.0\n"
                "ratio = min(ratio_h, ratio_w)\n"
                "if ratio < 1.0:\n"
                "    target = (\n"
                "        max(1, int(round(src.width * ratio))),\n"
                "        max(1, int(round(src.height * ratio))),\n"
                "    )\n"
                "    src = src.resize(target, Image.Resampling.LANCZOS)\n"
                "src.save(sys.argv[2])\n"
                "src.save(sys.argv[3])\n"
            ),
            str(image_path),
            str(temp_sidecar),
            str(temp_ffi),
            str(resize_max_height),
            str(resize_max_width),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if proc.returncode != 0:
        for path in (temp_sidecar, temp_ffi):
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        raise RuntimeError(
            f"입력 이미지 리사이즈 실패 for {image_path.name}: {(proc.stderr or proc.stdout).strip()}"
        )
    return PreparedImage(temp_sidecar, temp_ffi, (temp_sidecar, temp_ffi))


def prepare_legacy_sidecar_app_image(
    image_path: Path,
    sidecar_format: str,
) -> PreparedSingleImage:
    sidecar_format = sidecar_format.lower()
    if image_path.suffix.lower() == f".{sidecar_format}":
        fd, temp_name = tempfile.mkstemp(prefix="buzhidao-legacy-sidecar-", suffix=f".{sidecar_format}")
        os.close(fd)
        temp_path = Path(temp_name)
        cleanup_paths = (temp_path,)
    else:
        fd, temp_name = tempfile.mkstemp(prefix="buzhidao-legacy-sidecar-", suffix=f".{sidecar_format}")
        os.close(fd)
        temp_path = Path(temp_name)
        cleanup_paths = (temp_path,)

    proc = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            "-c",
            (
                "from PIL import Image\n"
                "import sys\n"
                "src = Image.open(sys.argv[1]).convert('RGBA')\n"
                "max_w = int(sys.argv[3])\n"
                "if max_w > 0 and src.width > max_w:\n"
                "    ratio = max_w / float(src.width)\n"
                "    target = (max_w, max(1, int(round(src.height * ratio))))\n"
                "    src = src.resize(target, Image.Resampling.LANCZOS)\n"
                "src.save(sys.argv[2])\n"
            ),
            str(image_path),
            str(temp_path),
            str(LEGACY_SIDECAR_APP_MAX_WIDTH),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if proc.returncode != 0:
        for path in cleanup_paths:
            try:
                path.unlink(missing_ok=True)
            except OSError:
                pass
        raise RuntimeError(
            f"legacy sidecar 이미지 준비 실패 for {image_path.name}: {(proc.stderr or proc.stdout).strip()}"
        )
    return PreparedSingleImage(temp_path, cleanup_paths)


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


def run_sidecar_once(
    image_path: Path,
    source: str,
    score_thresh: float,
    extra_env: dict[str, str] | None = None,
) -> tuple[list[dict], list[str]]:
    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)
    profile_enabled = env.get("BUZHIDAO_PADDLE_SIDECAR_PROFILE_STAGES") == "1"
    if profile_enabled:
        clear_profile_log(SIDECAR_PROFILE_LOG)
    proc = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            str(OCR_SIDECAR_COMPARE_PY),
            "--image",
            str(image_path),
            "--source",
            source,
            "--score-thresh",
            str(score_thresh),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
        env=env,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"sidecar failed for {image_path.name}: {proc.stderr}")
    lines = json_lines(proc.stdout)
    if not lines:
        raise RuntimeError(f"sidecar json output missing for {image_path.name}")
    profile_lines = []
    if profile_enabled:
        profile_lines = read_profile_log(SIDECAR_PROFILE_LOG, SIDECAR_PROFILE_PREFIX)
    return lines[-1]["detections"], profile_lines


def prepare_ffi_image(image_path: Path) -> tuple[Path, Path | None]:
    if image_path.suffix.lower() == ".bmp":
        return image_path, None
    fd, temp_name = tempfile.mkstemp(prefix="buzhidao-ffi-", suffix=".bmp")
    os.close(fd)
    temp_path = Path(temp_name)
    proc = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            "-c",
            (
                "from PIL import Image; "
                "import sys; "
                "Image.open(sys.argv[1]).convert('RGBA').save(sys.argv[2], format='BMP')"
            ),
            str(image_path),
            str(temp_path),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if proc.returncode != 0:
        try:
            temp_path.unlink(missing_ok=True)
        except OSError:
            pass
        raise RuntimeError(
            f"ffi BMP 변환 실패 for {image_path.name}: {(proc.stderr or proc.stdout).strip()}"
        )
    return temp_path, temp_path


def run_ffi_once(
    image_path: Path,
    env: dict[str, str],
    ffi_mode: str,
    source: str,
    score_thresh: float,
    cargo_profile: str,
) -> tuple[list[dict], list[str]]:
    profile_enabled = env.get("BUZHIDAO_PADDLE_FFI_PROFILE_STAGES") == "1"
    if profile_enabled:
        clear_profile_log(FFI_PROFILE_LOG)
    cargo_profile_args = ["--release"] if cargo_profile == "release" else []
    if ffi_mode == "pipeline":
        ffi_env = env.copy()
        ffi_env["BUZHIDAO_RUN_APP_OCR_SAMPLE_TEST"] = "1"
        ffi_env["BUZHIDAO_APP_OCR_TEST_IMAGE"] = str(image_path)
        ffi_env["BUZHIDAO_APP_OCR_TEST_SOURCE"] = source
        ffi_env["BUZHIDAO_APP_OCR_TEST_SCORE_THRESH"] = str(score_thresh)
        proc = subprocess.run(
            [
                "cargo",
                "test",
                *cargo_profile_args,
                "-p",
                "buzhidao",
                "--features",
                "paddle-ffi",
                "--",
                "--nocapture",
                "--exact",
                APP_OCR_COMPARE_TEST_NAME,
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            env=ffi_env,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"ffi pipeline compare failed for {image_path.name}:\n{proc.stdout}\n{proc.stderr}"
            )
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith("[APP_OCR] "):
                payload = json.loads(line[len("[APP_OCR] "):])
                profile_lines = []
                if profile_enabled:
                    profile_lines = read_profile_log(FFI_PROFILE_LOG, FFI_PROFILE_PREFIX)
                return [
                    {"polygon": normalize_detection_polygon(polygon), "text": text}
                    for polygon, text in payload.get("detections", [])
                ], profile_lines
        raise RuntimeError(f"ffi pipeline detections output missing for {image_path.name}")

    ffi_env = env.copy()
    ffi_env["BUZHIDAO_RUN_FFI_SAMPLE_TEST"] = "1"
    ffi_env["BUZHIDAO_FFI_TEST_IMAGE"] = str(image_path)
    proc = subprocess.run(
        [
            "cargo",
            "test",
            *cargo_profile_args,
            "-p",
            "buzhidao",
            "--features",
            "paddle-ffi",
            "--",
            "--nocapture",
            "--exact",
            FFI_COMPARE_TEST_NAME,
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
        env=ffi_env,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"ffi failed for {image_path.name}:\n{proc.stdout}\n{proc.stderr}")
    match = re.search(r"\[FFI\] detections=(.*)", proc.stdout)
    if not match:
        raise RuntimeError(f"ffi detections output missing for {image_path.name}")
    detections = ast.literal_eval(match.group(1))
    profile_lines = []
    if profile_enabled:
        profile_lines = read_profile_log(FFI_PROFILE_LOG, FFI_PROFILE_PREFIX)
    return [{"polygon": poly, "text": text} for poly, text in detections], profile_lines


def run_crop_compare(dump_path: Path, source: str) -> dict:
    proc = subprocess.run(
        [
            str(OCR_SERVER_PYTHON),
            str(OCR_SIDECAR_COMPARE_PY),
            "--source",
            source,
            "--compare-ffi-crop-dump",
            str(dump_path),
        ],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        encoding="utf-8",
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError(f"crop compare failed for {dump_path.name}: {proc.stderr or proc.stdout}")
    lines = json_lines(proc.stdout)
    if not lines:
        raise RuntimeError(f"crop compare json output missing for {dump_path.name}")
    return lines[-1]


def bounds_to_polygon(bounds: dict) -> list[list[float]]:
    x = float(bounds["x"])
    y = float(bounds["y"])
    width = float(bounds["width"])
    height = float(bounds["height"])
    return [
        [x, y],
        [x + width, y],
        [x + width, y + height],
        [x, y + height],
    ]


def normalize_detection_polygon(raw_polygon: object) -> list[list[float]]:
    if isinstance(raw_polygon, dict):
        return bounds_to_polygon(raw_polygon)
    if isinstance(raw_polygon, list):
        return [[float(point[0]), float(point[1])] for point in raw_polygon]
    raise TypeError(f"unsupported detection shape: {type(raw_polygon)!r}")


def bbox(polygon: list[list[float]]) -> tuple[float, float, float, float]:
    xs = [point[0] for point in polygon]
    ys = [point[1] for point in polygon]
    return min(xs), min(ys), max(xs), max(ys)


def iou(poly_a: list[list[float]], poly_b: list[list[float]]) -> float:
    ax0, ay0, ax1, ay1 = bbox(poly_a)
    bx0, by0, bx1, by1 = bbox(poly_b)
    ix0 = max(ax0, bx0)
    iy0 = max(ay0, by0)
    ix1 = min(ax1, bx1)
    iy1 = min(ay1, by1)
    iw = max(0.0, ix1 - ix0)
    ih = max(0.0, iy1 - iy0)
    inter = iw * ih
    if inter <= 0.0:
        return 0.0
    area_a = max(0.0, ax1 - ax0) * max(0.0, ay1 - ay0)
    area_b = max(0.0, bx1 - bx0) * max(0.0, by1 - by0)
    denom = area_a + area_b - inter
    return inter / denom if denom > 0.0 else 0.0


def greedy_match(sidecar: list[dict], ffi: list[dict], iou_thresh: float) -> list[tuple[int, int, float]]:
    scored: list[tuple[float, int, int]] = []
    for side_index, side_item in enumerate(sidecar):
        for ffi_index, ffi_item in enumerate(ffi):
            score = iou(side_item["polygon"], ffi_item["polygon"])
            if score >= iou_thresh:
                scored.append((score, side_index, ffi_index))
    scored.sort(reverse=True)
    pairs: list[tuple[int, int, float]] = []
    used_side = set()
    used_ffi = set()
    for score, side_index, ffi_index in scored:
        if side_index in used_side or ffi_index in used_ffi:
            continue
        used_side.add(side_index)
        used_ffi.add(ffi_index)
        pairs.append((side_index, ffi_index, score))
    return pairs


def compare_image(
    image_path: Path,
    source: str,
    score_thresh: float,
    iou_thresh: float,
    env: dict[str, str],
    compare_crop: bool,
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    cargo_profile: str,
    profile_stages: bool,
    legacy_sidecar_app_mode: bool,
) -> dict:
    pipeline_resize_mode = normalize_pipeline_resize_mode(pipeline_resize_mode)
    crop_dump_dir: Path | None = None
    sidecar_env: dict[str, str] | None = None
    ffi_env_map = env
    if compare_crop:
        crop_dump_dir = Path(tempfile.mkdtemp(prefix=f"buzhidao-crop-compare-{image_path.stem}-"))
        sidecar_env = {
            "BUZHIDAO_PADDLE_FFI_DUMP_DIR": str(crop_dump_dir),
            "BUZHIDAO_PADDLE_SIDECAR_DUMP_CROP": "1",
        }
        ffi_env_map = env.copy()
        ffi_env_map["BUZHIDAO_PADDLE_FFI_DUMP_DIR"] = str(crop_dump_dir)
    if profile_stages:
        sidecar_env = dict(sidecar_env or {})
        sidecar_env["BUZHIDAO_PADDLE_SIDECAR_PROFILE_STAGES"] = "1"
        ffi_env_map = ffi_env_map.copy()
        ffi_env_map["BUZHIDAO_PADDLE_FFI_PROFILE_STAGES"] = "1"
    if ffi_mode == "pipeline":
        ffi_env_map = ffi_env_map.copy()
        ffi_env_map["BUZHIDAO_APP_OCR_RESIZE_MODE"] = pipeline_resize_mode

    prepared = prepare_image_paths(
        image_path,
        resize_max_height,
        resize_max_width,
        sidecar_format,
        ffi_format,
        ffi_mode,
        pipeline_resize_mode,
    )
    legacy_prepared: PreparedSingleImage | None = None
    try:
        sidecar_input = prepared.sidecar_path
        if legacy_sidecar_app_mode:
            legacy_prepared = prepare_legacy_sidecar_app_image(image_path, sidecar_format)
            sidecar_input = legacy_prepared.path
        sidecar, sidecar_profile_lines = run_sidecar_once(
            sidecar_input, source, score_thresh, sidecar_env
        )
        ffi, ffi_profile_lines = run_ffi_once(
            prepared.ffi_path, ffi_env_map, ffi_mode, source, score_thresh, cargo_profile
        )
    finally:
        if legacy_prepared is not None:
            for temp_path in legacy_prepared.cleanup_paths:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass
        for temp_path in prepared.cleanup_paths:
            try:
                temp_path.unlink(missing_ok=True)
            except OSError:
                pass

    pairs = greedy_match(sidecar, ffi, iou_thresh)
    matched_side = {side_index for side_index, _, _ in pairs}
    matched_ffi = {ffi_index for _, ffi_index, _ in pairs}
    exact_matches = sum(
        1
        for side_index, ffi_index, _ in pairs
        if sidecar[side_index]["text"] == ffi[ffi_index]["text"]
    )

    output = {
        "image": image_path.name,
        "input_image": str(sidecar_input),
        "resize_max_height": resize_max_height,
        "resize_max_width": resize_max_width,
        "sidecar_mode": "legacy_04a0ea45" if legacy_sidecar_app_mode else "current_compare",
        "sidecar_format": sidecar_format,
        "ffi_format": ffi_format,
        "ffi_mode": ffi_mode,
        "sidecar_count": len(sidecar),
        "ffi_count": len(ffi),
        "matched_pairs": len(pairs),
        "exact_text_matches": exact_matches,
        "match_rate_vs_sidecar": (exact_matches / len(sidecar)) if sidecar else 1.0,
        "match_rate_vs_matched_pairs": (exact_matches / len(pairs)) if pairs else 1.0,
        "mismatches": [
            {
                "sidecar_text": sidecar[side_index]["text"],
                "ffi_text": ffi[ffi_index]["text"],
                "iou": score,
            }
            for side_index, ffi_index, score in pairs
            if sidecar[side_index]["text"] != ffi[ffi_index]["text"]
        ],
        "sidecar_only": [
            sidecar[index]["text"]
            for index in range(len(sidecar))
            if index not in matched_side
        ],
        "ffi_only": [
            ffi[index]["text"]
            for index in range(len(ffi))
            if index not in matched_ffi
        ],
    }
    if compare_crop and crop_dump_dir is not None:
        crop_compares = [
            run_crop_compare(dump_path, source)
            for dump_path in sorted(crop_dump_dir.glob("crop_*.json"))
        ]
        output["crop_dump_dir"] = str(crop_dump_dir)
        output["crop_compares"] = crop_compares
    if profile_stages:
        output["sidecar_profile_lines"] = sidecar_profile_lines
        output["ffi_profile_lines"] = ffi_profile_lines
    return output


def read_json_line(proc: subprocess.Popen[str]) -> dict:
    assert proc.stdout is not None
    while True:
        line = proc.stdout.readline()
        if not line:
            stderr = proc.stderr.read() if proc.stderr else ""
            raise RuntimeError(f"sidecar server output ended unexpectedly: {stderr}")
        line = line.strip()
        if not line.startswith("{"):
            continue
        return json.loads(line)


def benchmark_sidecar(
    images: list[Path],
    source: str,
    score_thresh: float,
    warmups: int,
    iterations: int,
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    legacy_sidecar_app_mode: bool,
) -> list[dict]:
    pipeline_resize_mode = normalize_pipeline_resize_mode(pipeline_resize_mode)
    env = os.environ.copy()
    env["PYTHON_OCR_LANG"] = source
    proc = subprocess.Popen(
        [str(OCR_SERVER_PYTHON), str(OCR_SIDECAR_COMPARE_PY), "--server"],
        cwd=REPO_ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        env=env,
    )
    try:
        ready = read_json_line(proc)
        if ready.get("type") != "ready":
            raise RuntimeError(f"unexpected sidecar server ready payload: {ready}")
        results = []
        assert proc.stdin is not None
        prepared_images = [
            prepare_image_paths(
                image,
                resize_max_height,
                resize_max_width,
                sidecar_format,
                ffi_format,
                ffi_mode,
                pipeline_resize_mode,
            )
            for image in images
        ]
        legacy_images = [
            prepare_legacy_sidecar_app_image(image, sidecar_format)
            for image in images
        ] if legacy_sidecar_app_mode else [None] * len(images)
        try:
            for image, prepared, legacy_prepared in zip(images, prepared_images, legacy_images):
                sidecar_input = legacy_prepared.path if legacy_prepared is not None else prepared.sidecar_path
                request = {
                    "id": 1,
                    "source": source,
                    "image_path": str(sidecar_input),
                    "score_thresh": score_thresh,
                }
                for _ in range(warmups):
                    proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
                    proc.stdin.flush()
                    payload = read_json_line(proc)
                    if payload.get("type") != "result":
                        raise RuntimeError(f"sidecar warmup failed for {image.name}: {payload}")
                elapsed_ms = []
                detection_count = 0
                for _ in range(iterations):
                    started = time.perf_counter()
                    proc.stdin.write(json.dumps(request, ensure_ascii=False) + "\n")
                    proc.stdin.flush()
                    payload = read_json_line(proc)
                    elapsed_ms.append((time.perf_counter() - started) * 1000.0)
                    if payload.get("type") != "result":
                        raise RuntimeError(f"sidecar benchmark failed for {image.name}: {payload}")
                    detection_count = len(payload.get("detections", []))
                results.append(
                    {
                        "image": str(image),
                        "prepared_image": str(sidecar_input),
                        "resize_max_height": resize_max_height,
                        "resize_max_width": resize_max_width,
                        "sidecar_mode": "legacy_04a0ea45" if legacy_sidecar_app_mode else "current_compare",
                        "sidecar_format": sidecar_format,
                        "ffi_format": ffi_format,
                        "detection_count": detection_count,
                        "elapsed_ms": elapsed_ms,
                    }
                )
        finally:
            for legacy_prepared in legacy_images:
                if legacy_prepared is None:
                    continue
                for temp_path in legacy_prepared.cleanup_paths:
                    try:
                        temp_path.unlink(missing_ok=True)
                    except OSError:
                        pass
            for prepared in prepared_images:
                for temp_path in prepared.cleanup_paths:
                    try:
                        temp_path.unlink(missing_ok=True)
                    except OSError:
                        pass
        return results
    finally:
        if proc.stdin is not None:
            proc.stdin.close()
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


def benchmark_ffi(
    images: list[Path],
    source: str,
    score_thresh: float,
    warmups: int,
    iterations: int,
    env: dict[str, str],
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    cargo_profile: str,
) -> list[dict]:
    pipeline_resize_mode = normalize_pipeline_resize_mode(pipeline_resize_mode)
    ffi_env = env.copy()
    prepared_images = [
        prepare_image_paths(
            path,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
        )
        for path in images
    ]
    prepared_to_original = {
        str(item.ffi_path): str(path) for path, item in zip(images, prepared_images)
    }
    if ffi_mode == "pipeline":
        ffi_env["BUZHIDAO_APP_OCR_RESIZE_MODE"] = pipeline_resize_mode
        ffi_env["BUZHIDAO_RUN_APP_OCR_BENCH"] = "1"
        ffi_env["BUZHIDAO_APP_OCR_BENCH_IMAGES_JSON"] = json.dumps(
            [str(item.ffi_path) for item in prepared_images], ensure_ascii=False
        )
        ffi_env["BUZHIDAO_APP_OCR_BENCH_SOURCE"] = source
        ffi_env["BUZHIDAO_APP_OCR_BENCH_SCORE_THRESH"] = str(score_thresh)
        ffi_env["BUZHIDAO_APP_OCR_BENCH_WARMUPS"] = str(warmups)
        ffi_env["BUZHIDAO_APP_OCR_BENCH_ITERATIONS"] = str(iterations)
        test_name = APP_OCR_BENCH_TEST_NAME
        output_prefix = "[APP_OCR_BENCH] "
    else:
        ffi_env["BUZHIDAO_RUN_FFI_BENCH"] = "1"
        ffi_env["BUZHIDAO_FFI_BENCH_IMAGES_JSON"] = json.dumps(
            [str(item.ffi_path) for item in prepared_images], ensure_ascii=False
        )
        ffi_env["BUZHIDAO_FFI_BENCH_SOURCE"] = source
        ffi_env["BUZHIDAO_FFI_BENCH_SCORE_THRESH"] = str(score_thresh)
        ffi_env["BUZHIDAO_FFI_BENCH_WARMUPS"] = str(warmups)
        ffi_env["BUZHIDAO_FFI_BENCH_ITERATIONS"] = str(iterations)
        test_name = FFI_BENCH_TEST_NAME
        output_prefix = "[FFI_BENCH] "
    try:
        cargo_profile_args = ["--release"] if cargo_profile == "release" else []
        proc = subprocess.run(
            [
                "cargo",
                "test",
                *cargo_profile_args,
                "-p",
                "buzhidao",
                "--features",
                "paddle-ffi",
                "--",
                "--nocapture",
                "--exact",
                test_name,
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            encoding="utf-8",
            check=False,
            env=ffi_env,
        )
        if proc.returncode != 0:
            raise RuntimeError(f"ffi benchmark failed:\n{proc.stdout}\n{proc.stderr}")
        results = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if line.startswith(output_prefix):
                result = json.loads(line[len(output_prefix):])
                result["prepared_image"] = result["image"]
                result["image"] = prepared_to_original.get(result["image"], result["image"])
                result["resize_max_height"] = resize_max_height
                result["resize_max_width"] = resize_max_width
                result["sidecar_format"] = sidecar_format
                result["ffi_format"] = ffi_format
                result["ffi_mode"] = ffi_mode
                results.append(result)
        if not results:
            raise RuntimeError(f"ffi benchmark output missing:\n{proc.stdout}\n{proc.stderr}")
        return results
    finally:
        for prepared in prepared_images:
            for temp_path in prepared.cleanup_paths:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass


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


def profile_ffi(
    images: list[Path],
    source: str,
    score_thresh: float,
    warmups: int,
    iterations: int,
    env: dict[str, str],
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    cargo_profile: str,
) -> list[dict]:
    profile_env = env.copy()
    profile_env["BUZHIDAO_PADDLE_FFI_PROFILE_STAGES"] = "1"
    compare_results = []
    for image_path in images:
        prepared = prepare_image_paths(
            image_path,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
        )
        try:
            detections, profile_lines = run_ffi_once(
                prepared.ffi_path,
                profile_env,
                ffi_mode,
                source,
                score_thresh,
                cargo_profile,
            )
            profile_entries = parse_profile_lines(profile_lines)
            compare_results.append(
                {
                    "image": image_path.name,
                    "ffi_mode": ffi_mode,
                    "pipeline_resize_mode": normalize_pipeline_resize_mode(
                        pipeline_resize_mode
                    ),
                    "detection_count": len(detections),
                    "profile_lines": profile_lines,
                    "profile_entries": profile_entries,
                    "pipeline_profile": latest_pipeline_profile_entry(profile_entries),
                }
            )
        finally:
            for temp_path in prepared.cleanup_paths:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass

    bench_results = {
        Path(item["image"]).name: summarize_benchmark(item)
        for item in benchmark_ffi(
            images,
            source,
            score_thresh,
            warmups,
            iterations,
            env,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
            cargo_profile,
        )
    }

    output = []
    for item in compare_results:
        current = dict(item)
        current["benchmark"] = bench_results[item["image"]]
        output.append(current)
    return output


def compare_ffi_self(
    images: list[Path],
    source: str,
    score_thresh: float,
    iou_thresh: float,
    warmups: int,
    iterations: int,
    env: dict[str, str],
    baseline_env_overrides: dict[str, str],
    candidate_env_overrides: dict[str, str],
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    cargo_profile: str,
) -> list[dict]:
    baseline_env = env.copy()
    baseline_env.update(baseline_env_overrides)
    candidate_env = env.copy()
    candidate_env.update(baseline_env_overrides)
    candidate_env.update(candidate_env_overrides)

    profile_env_baseline = baseline_env.copy()
    profile_env_baseline["BUZHIDAO_PADDLE_FFI_PROFILE_STAGES"] = "1"
    profile_env_candidate = candidate_env.copy()
    profile_env_candidate["BUZHIDAO_PADDLE_FFI_PROFILE_STAGES"] = "1"

    baseline_compare: dict[str, tuple[list[dict], list[str]]] = {}
    candidate_compare: dict[str, tuple[list[dict], list[str]]] = {}
    for image_path in images:
        prepared = prepare_image_paths(
            image_path,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
        )
        try:
            baseline_compare[image_path.name] = run_ffi_once(
                prepared.ffi_path,
                profile_env_baseline,
                ffi_mode,
                source,
                score_thresh,
                cargo_profile,
            )
            candidate_compare[image_path.name] = run_ffi_once(
                prepared.ffi_path,
                profile_env_candidate,
                ffi_mode,
                source,
                score_thresh,
                cargo_profile,
            )
        finally:
            for temp_path in prepared.cleanup_paths:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass

    baseline_bench = {
        Path(item["image"]).name: summarize_benchmark(item)
        for item in benchmark_ffi(
            images,
            source,
            score_thresh,
            warmups,
            iterations,
            baseline_env,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
            cargo_profile,
        )
    }
    candidate_bench = {
        Path(item["image"]).name: summarize_benchmark(item)
        for item in benchmark_ffi(
            images,
            source,
            score_thresh,
            warmups,
            iterations,
            candidate_env,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
            cargo_profile,
        )
    }

    output = []
    for image_path in images:
        baseline_detections, baseline_profile_lines = baseline_compare[image_path.name]
        candidate_detections, candidate_profile_lines = candidate_compare[image_path.name]
        pairs = greedy_match(baseline_detections, candidate_detections, iou_thresh)
        matched_baseline = {base_index for base_index, _, _ in pairs}
        matched_candidate = {cand_index for _, cand_index, _ in pairs}
        exact_matches = sum(
            1
            for base_index, cand_index, _ in pairs
            if baseline_detections[base_index]["text"] == candidate_detections[cand_index]["text"]
        )
        baseline_entries = parse_profile_lines(baseline_profile_lines)
        candidate_entries = parse_profile_lines(candidate_profile_lines)
        output.append(
            {
                "image": image_path.name,
                "baseline_env": baseline_env_overrides,
                "candidate_env": candidate_env_overrides,
                "ffi_mode": ffi_mode,
                "pipeline_resize_mode": normalize_pipeline_resize_mode(pipeline_resize_mode),
                "baseline_count": len(baseline_detections),
                "candidate_count": len(candidate_detections),
                "matched_pairs": len(pairs),
                "exact_text_matches": exact_matches,
                "match_rate_vs_baseline": (
                    exact_matches / len(baseline_detections) if baseline_detections else 1.0
                ),
                "mismatches": [
                    {
                        "baseline_text": baseline_detections[base_index]["text"],
                        "candidate_text": candidate_detections[cand_index]["text"],
                        "iou": score,
                    }
                    for base_index, cand_index, score in pairs
                    if baseline_detections[base_index]["text"] != candidate_detections[cand_index]["text"]
                ],
                "baseline_only": [
                    baseline_detections[index]["text"]
                    for index in range(len(baseline_detections))
                    if index not in matched_baseline
                ],
                "candidate_only": [
                    candidate_detections[index]["text"]
                    for index in range(len(candidate_detections))
                    if index not in matched_candidate
                ],
                "baseline_profile_lines": baseline_profile_lines,
                "candidate_profile_lines": candidate_profile_lines,
                "baseline_profile_entries": baseline_entries,
                "candidate_profile_entries": candidate_entries,
                "baseline_pipeline_profile": latest_pipeline_profile_entry(baseline_entries),
                "candidate_pipeline_profile": latest_pipeline_profile_entry(candidate_entries),
                "baseline_benchmark": baseline_bench[image_path.name],
                "candidate_benchmark": candidate_bench[image_path.name],
                "candidate_faster_or_equal_by_mean": (
                    candidate_bench[image_path.name]["mean_ms"]
                    <= baseline_bench[image_path.name]["mean_ms"]
                ),
                "mean_delta_ms": (
                    candidate_bench[image_path.name]["mean_ms"]
                    - baseline_bench[image_path.name]["mean_ms"]
                ),
            }
        )
    return output


def analyze_ffi_corpus(
    images: list[Path],
    source: str,
    score_thresh: float,
    env: dict[str, str],
    resize_max_height: int,
    resize_max_width: int,
    sidecar_format: str,
    ffi_format: str,
    ffi_mode: str,
    pipeline_resize_mode: str,
    cargo_profile: str,
    dict_config: Path | None,
) -> dict:
    image_results = []
    all_texts: list[str] = []
    for image_path in images:
        prepared = prepare_image_paths(
            image_path,
            resize_max_height,
            resize_max_width,
            sidecar_format,
            ffi_format,
            ffi_mode,
            pipeline_resize_mode,
        )
        try:
            detections, _ = run_ffi_once(
                prepared.ffi_path,
                env,
                ffi_mode,
                source,
                score_thresh,
                cargo_profile,
            )
        finally:
            for temp_path in prepared.cleanup_paths:
                try:
                    temp_path.unlink(missing_ok=True)
                except OSError:
                    pass
        texts = [str(item["text"]) for item in detections]
        all_texts.extend(texts)
        chars = set("".join(texts))
        image_results.append(
            {
                "image": image_path.name,
                "detection_count": len(texts),
                "unique_chars": len(chars),
                "texts_sample": texts[:10],
            }
        )

    corpus_chars = set("".join(all_texts))
    output: dict[str, object] = {
        "source": source,
        "ffi_mode": ffi_mode,
        "pipeline_resize_mode": normalize_pipeline_resize_mode(pipeline_resize_mode),
        "images": image_results,
        "corpus": {
            "image_count": len(images),
            "detection_count": len(all_texts),
            "unique_chars": len(corpus_chars),
        },
    }
    if dict_config is not None:
        rec_dict = load_rec_dict_from_config(dict_config)
        output["dict"] = {
            "config_path": str(dict_config),
            "size": len(rec_dict),
            "used_chars": len(corpus_chars),
            "coverage_pct": (len(corpus_chars) / len(rec_dict) * 100.0) if rec_dict else 0.0,
        }
    return output


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)
    format_choices = ("png", "bmp")

    verify = subparsers.add_parser("verify")
    verify.add_argument("--image", action="append", default=[])
    verify.add_argument("--source", default="ch")
    verify.add_argument("--score-thresh", type=float, default=0.1)
    verify.add_argument("--iou-thresh", type=float, default=0.5)
    verify.add_argument("--warmups", type=int, default=1)
    verify.add_argument("--iterations", type=int, default=3)
    verify.add_argument("--resize-max-height", type=int, default=0)
    verify.add_argument("--resize-max-width", type=int, default=0)
    verify.add_argument("--sidecar-format", choices=format_choices, default="png")
    verify.add_argument("--ffi-format", choices=format_choices, default="bmp")
    verify.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    verify.add_argument("--pipeline-resize-mode", choices=("width", "long-side"), default="long-side")
    verify.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    verify.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    verify_ffi = subparsers.add_parser("verify-ffi")
    verify_ffi.add_argument("--image", action="append", default=[])
    verify_ffi.add_argument("--source", default="ch")
    verify_ffi.add_argument("--score-thresh", type=float, default=0.1)
    verify_ffi.add_argument("--warmups", type=int, default=1)
    verify_ffi.add_argument("--iterations", type=int, default=3)
    verify_ffi.add_argument("--resize-max-height", type=int, default=0)
    verify_ffi.add_argument("--resize-max-width", type=int, default=0)
    verify_ffi.add_argument("--sidecar-format", choices=format_choices, default="png")
    verify_ffi.add_argument("--ffi-format", choices=format_choices, default="bmp")
    verify_ffi.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    verify_ffi.add_argument("--pipeline-resize-mode", choices=("width", "long-side"), default="long-side")
    verify_ffi.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    verify_ffi.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    profile_ffi_parser = subparsers.add_parser("profile-ffi")
    profile_ffi_parser.add_argument("--image", action="append", default=[])
    profile_ffi_parser.add_argument("--source", default="ch")
    profile_ffi_parser.add_argument("--score-thresh", type=float, default=0.1)
    profile_ffi_parser.add_argument("--warmups", type=int, default=1)
    profile_ffi_parser.add_argument("--iterations", type=int, default=3)
    profile_ffi_parser.add_argument("--resize-max-height", type=int, default=0)
    profile_ffi_parser.add_argument("--resize-max-width", type=int, default=0)
    profile_ffi_parser.add_argument("--sidecar-format", choices=format_choices, default="png")
    profile_ffi_parser.add_argument("--ffi-format", choices=format_choices, default="bmp")
    profile_ffi_parser.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    profile_ffi_parser.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    profile_ffi_parser.add_argument(
        "--pipeline-resize-mode",
        choices=("width", "long-side"),
        default="long-side",
    )
    profile_ffi_parser.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    compare_ffi_self_parser = subparsers.add_parser("compare-ffi-self")
    compare_ffi_self_parser.add_argument("--image", action="append", default=[])
    compare_ffi_self_parser.add_argument("--source", default="ch")
    compare_ffi_self_parser.add_argument("--score-thresh", type=float, default=0.1)
    compare_ffi_self_parser.add_argument("--iou-thresh", type=float, default=0.5)
    compare_ffi_self_parser.add_argument("--warmups", type=int, default=1)
    compare_ffi_self_parser.add_argument("--iterations", type=int, default=3)
    compare_ffi_self_parser.add_argument("--resize-max-height", type=int, default=0)
    compare_ffi_self_parser.add_argument("--resize-max-width", type=int, default=0)
    compare_ffi_self_parser.add_argument("--sidecar-format", choices=format_choices, default="png")
    compare_ffi_self_parser.add_argument("--ffi-format", choices=format_choices, default="bmp")
    compare_ffi_self_parser.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    compare_ffi_self_parser.add_argument(
        "--pipeline-resize-mode",
        choices=("width", "long-side"),
        default="long-side",
    )
    compare_ffi_self_parser.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    compare_ffi_self_parser.add_argument("--baseline-env", action="append", default=[])
    compare_ffi_self_parser.add_argument("--candidate-env", action="append", default=[])
    compare_ffi_self_parser.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    analyze_ffi_corpus_parser = subparsers.add_parser("analyze-ffi-corpus")
    analyze_ffi_corpus_parser.add_argument("--image", action="append", default=[])
    analyze_ffi_corpus_parser.add_argument("--source", default="ch")
    analyze_ffi_corpus_parser.add_argument("--score-thresh", type=float, default=0.1)
    analyze_ffi_corpus_parser.add_argument("--resize-max-height", type=int, default=0)
    analyze_ffi_corpus_parser.add_argument("--resize-max-width", type=int, default=0)
    analyze_ffi_corpus_parser.add_argument("--sidecar-format", choices=format_choices, default="png")
    analyze_ffi_corpus_parser.add_argument("--ffi-format", choices=format_choices, default="bmp")
    analyze_ffi_corpus_parser.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    analyze_ffi_corpus_parser.add_argument(
        "--pipeline-resize-mode",
        choices=("width", "long-side"),
        default="long-side",
    )
    analyze_ffi_corpus_parser.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    analyze_ffi_corpus_parser.add_argument("--dict-config", default="")
    analyze_ffi_corpus_parser.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    compare = subparsers.add_parser("compare")
    compare.add_argument("--image", action="append", default=[])
    compare.add_argument("--source", default="ch")
    compare.add_argument("--score-thresh", type=float, default=0.1)
    compare.add_argument("--iou-thresh", type=float, default=0.5)
    compare.add_argument("--compare-crop", action="store_true")
    compare.add_argument("--resize-max-height", type=int, default=0)
    compare.add_argument("--resize-max-width", type=int, default=0)
    compare.add_argument("--sidecar-format", choices=format_choices, default="png")
    compare.add_argument("--ffi-format", choices=format_choices, default="bmp")
    compare.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    compare.add_argument("--pipeline-resize-mode", choices=("width", "long-side"), default="long-side")
    compare.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    compare.add_argument("--profile-stages", action="store_true")
    compare.add_argument("--legacy-sidecar-app-mode", action="store_true")
    compare.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    bench = subparsers.add_parser("benchmark")
    bench.add_argument("--image", action="append", default=[])
    bench.add_argument("--source", default="ch")
    bench.add_argument("--score-thresh", type=float, default=0.1)
    bench.add_argument("--warmups", type=int, default=3)
    bench.add_argument("--iterations", type=int, default=10)
    bench.add_argument("--resize-max-height", type=int, default=0)
    bench.add_argument("--resize-max-width", type=int, default=0)
    bench.add_argument("--sidecar-format", choices=format_choices, default="png")
    bench.add_argument("--ffi-format", choices=format_choices, default="bmp")
    bench.add_argument("--ffi-mode", choices=("raw", "pipeline"), default="raw")
    bench.add_argument("--pipeline-resize-mode", choices=("width", "long-side"), default="long-side")
    bench.add_argument("--cargo-profile", choices=("debug", "release"), default="release")
    bench.add_argument("--legacy-sidecar-app-mode", action="store_true")
    bench.add_argument(
        "--auto-install-deps",
        action="store_true",
        default=os.environ.get("BUZHIDAO_AUTO_INSTALL_DEPS", "0") == "1",
    )

    return parser


def run_compare(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    results = [
        compare_image(
            image_path,
            args.source,
            args.score_thresh,
            args.iou_thresh,
            env,
            args.compare_crop,
            args.resize_max_height,
            args.resize_max_width,
            args.sidecar_format,
            args.ffi_format,
            args.ffi_mode,
            args.pipeline_resize_mode,
            args.cargo_profile,
            args.profile_stages,
            args.legacy_sidecar_app_mode,
        )
        for image_path in images
    ]
    print(json.dumps(results, ensure_ascii=False, indent=2))
    return 0


def run_verify(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)

    compare_results = {
        item["image"]: item
        for item in [
            compare_image(
                image_path,
                args.source,
                args.score_thresh,
                args.iou_thresh,
                env,
                False,
                args.resize_max_height,
                args.resize_max_width,
                args.sidecar_format,
                args.ffi_format,
                args.ffi_mode,
                args.pipeline_resize_mode,
                args.cargo_profile,
                False,
                False,
            )
            for image_path in images
        ]
    }
    sidecar_raw = benchmark_sidecar(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
        False,
    )
    ffi_raw = benchmark_ffi(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        env,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
    )
    sidecar_by_image = {Path(item["image"]).name: summarize_benchmark(item) for item in sidecar_raw}
    ffi_by_image = {Path(item["image"]).name: summarize_benchmark(item) for item in ffi_raw}

    output = []
    for image in [path.name for path in images]:
        compare = compare_results[image]
        sidecar = sidecar_by_image[image]
        ffi = ffi_by_image[image]
        output.append(
            {
                "image": image,
                "parity": compare,
                "performance": {
                    "detection_count_match": sidecar["detection_count"] == ffi["detection_count"],
                    "sidecar": sidecar,
                    "ffi": ffi,
                    "ffi_faster_or_equal_by_mean": ffi["mean_ms"] <= sidecar["mean_ms"],
                    "ffi_faster_or_equal_by_median": ffi["median_ms"] <= sidecar["median_ms"],
                    "mean_delta_ms": ffi["mean_ms"] - sidecar["mean_ms"],
                    "median_delta_ms": ffi["median_ms"] - sidecar["median_ms"],
                },
            }
        )
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def run_benchmark(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    sidecar_raw = benchmark_sidecar(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.legacy_sidecar_app_mode,
    )
    ffi_raw = benchmark_ffi(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        env,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
    )
    sidecar_by_image = {Path(item["image"]).name: summarize_benchmark(item) for item in sidecar_raw}
    ffi_by_image = {Path(item["image"]).name: summarize_benchmark(item) for item in ffi_raw}
    output = []
    for image in [path.name for path in images]:
        sidecar = sidecar_by_image[image]
        ffi = ffi_by_image[image]
        output.append(
            {
                "image": image,
                "detection_count_match": sidecar["detection_count"] == ffi["detection_count"],
                "sidecar": sidecar,
                "ffi": ffi,
                "ffi_faster_or_equal_by_mean": ffi["mean_ms"] <= sidecar["mean_ms"],
                "ffi_faster_or_equal_by_median": ffi["median_ms"] <= sidecar["median_ms"],
                "mean_delta_ms": ffi["mean_ms"] - sidecar["mean_ms"],
                "median_delta_ms": ffi["median_ms"] - sidecar["median_ms"],
            }
        )
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def run_verify_ffi(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    ffi_raw = benchmark_ffi(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        env,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
    )
    output = [summarize_benchmark(item) for item in ffi_raw]
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def run_profile_ffi(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    output = profile_ffi(
        images,
        args.source,
        args.score_thresh,
        args.warmups,
        args.iterations,
        env,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
    )
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def run_compare_ffi_self(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    output = compare_ffi_self(
        images,
        args.source,
        args.score_thresh,
        args.iou_thresh,
        args.warmups,
        args.iterations,
        env,
        parse_env_assignments(args.baseline_env),
        parse_env_assignments(args.candidate_env),
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
    )
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def run_analyze_ffi_corpus(args: argparse.Namespace) -> int:
    images = resolve_images(args.image)
    env = build_ffi_env()
    ensure_ffi_system_deps(args.auto_install_deps, env)
    dict_config = Path(args.dict_config).resolve() if args.dict_config else resolve_rec_config_path(args.source)
    output = analyze_ffi_corpus(
        images,
        args.source,
        args.score_thresh,
        env,
        args.resize_max_height,
        args.resize_max_width,
        args.sidecar_format,
        args.ffi_format,
        args.ffi_mode,
        args.pipeline_resize_mode,
        args.cargo_profile,
        dict_config,
    )
    print(json.dumps(output, ensure_ascii=False, indent=2))
    return 0


def main(argv: list[str] | None = None) -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.command == "verify":
        return run_verify(args)
    if args.command == "verify-ffi":
        return run_verify_ffi(args)
    if args.command == "profile-ffi":
        return run_profile_ffi(args)
    if args.command == "compare-ffi-self":
        return run_compare_ffi_self(args)
    if args.command == "analyze-ffi-corpus":
        return run_analyze_ffi_corpus(args)
    if args.command == "compare":
        return run_compare(args)
    if args.command == "benchmark":
        return run_benchmark(args)
    raise SystemExit(f"unknown command: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
