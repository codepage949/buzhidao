from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
import tarfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from tempfile import TemporaryDirectory, gettempdir
from typing import Dict, Optional

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_DOWNLOAD_DIR = Path(gettempdir()) / "buzhidao-paddle-inference"
PADDLE_INFERENCE_VERSION = "3.2.2"
OPENCV_CONTRIB_PYTHON_VERSION = "4.10.0.84"
OPENCV_VERSION = "4.10.0"
PYCLIPPER_VERSION = "1.4.0"
SHAPELY_VERSION = "2.1.2"
OPENCV_WINDOWS_URL = (
    "https://sourceforge.net/projects/opencvlibrary/files/4.10.0/"
    "opencv-4.10.0-windows.exe/download"
)
PYCLIPPER_SOURCE_URL = (
    "https://files.pythonhosted.org/packages/f6/21/"
    "3c06205bb407e1f79b73b7b4dfb3950bd9537c4f625a68ab5cc41177f5bc/"
    f"pyclipper-{PYCLIPPER_VERSION}.tar.gz"
)

DEFAULT_PADDLE_DEVICE = "cpu"
PADDLE_GPU_CUDA_VARIANT = "cu126"
PADDLE_INFERENCE_V3_URLS: Dict[str, Dict[str, Dict[str, Dict[str, str]]]] = {
    "cpu": {
        "windows": {
            "x86_64": {
                "default": f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/Windows/CPU/x86-64_avx-mkl-vs2019/paddle_inference.zip",
            },
        },
        "linux": {
            "x86_64": {
                "default": f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/Linux/CPU/gcc8.2_avx_mkl/paddle_inference.tgz",
            },
        },
        "darwin": {
            "x86_64": {
                "default": f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/MacOS/x86-64_clang_avx_accelerate_blas/paddle_inference.tgz",
            },
            "arm64": {
                "default": f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/MacOS/m1_clang_noavx_accelerate_blas/paddle_inference.tgz",
            },
        },
    },
    "gpu": {
        "windows": {
            "x86_64": {
                "cu126": f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/Windows/GPU/x86-64_cuda12.6_cudnn9.5.1_trt10.5.0.18_mkl_avx_vs2019/paddle_inference.zip",
            },
        },
        "linux": {
            "x86_64": {
                PADDLE_GPU_CUDA_VARIANT: f"https://paddle-inference-lib.bj.bcebos.com/{PADDLE_INFERENCE_VERSION}/cxx_c/Linux/GPU/x86-64_gcc11.2_avx_mkl_cuda12.6_cudnn9.5.1-trt10.5.0.18/paddle_inference.tgz",
            },
        },
    },
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=f"Paddle Inference {PADDLE_INFERENCE_VERSION} 아카이브를 자동 다운로드 받아 .paddle_inference 레이아웃으로 정리합니다."
    )
    parser.add_argument(
        "--destination-dir",
        default=str((REPO_ROOT / ".paddle_inference")),
        help="설치 대상 루트 경로 (기본값: .paddle_inference)",
    )
    parser.add_argument(
        "--download-dir",
        default=str(DEFAULT_DOWNLOAD_DIR),
        help=f"SDK 아카이브 저장 위치 (기본값: {DEFAULT_DOWNLOAD_DIR})",
    )
    parser.add_argument(
        "--force-download",
        action="store_true",
        help="기존 파일이 있어도 항상 다시 다운로드",
    )
    parser.add_argument(
        "--device",
        choices=("cpu", "gpu"),
        default=DEFAULT_PADDLE_DEVICE,
        help="Paddle Inference SDK 종류 (기본값: cpu)",
    )
    parser.add_argument(
        "--opencv-sdk-dir",
        default=None,
        help="기존 OpenCV SDK 루트 경로. 지정하면 .paddle_inference/third_party/opencv-sdk/<platform> 아래로 복사합니다.",
    )
    return parser.parse_args()


def archive_suffixes(path: Path) -> list[str]:
    return [suffix.lower() for suffix in path.suffixes]


def detect_archive_mode(path: Path) -> str:
    lower_name = path.name.lower()
    if lower_name.endswith(".zip"):
        return "zip"
    if lower_name.endswith(".tar.gz") or lower_name.endswith(".tgz"):
        return "tar.gz"
    if lower_name.endswith(".tar.xz"):
        return "tar.xz"
    if lower_name.endswith(".tar.bz2") or lower_name.endswith(".tbz2"):
        return "tar.bz2"
    raise ValueError(f"지원하지 않는 아카이브 형식: {path}")


def resolve_platform_key() -> tuple[str, str]:
    system = platform.system().lower()
    machine = platform.machine().lower()

    if system == "darwin":
        if machine in {"x86_64", "amd64"}:
            return "darwin", "x86_64"
        if machine in {"arm64", "aarch64"}:
            return "darwin", "arm64"
        return "darwin", "x86_64"

    if system == "linux":
        if machine in {"x86_64", "amd64"}:
            return "linux", "x86_64"
        return "linux", machine

    if system == "windows":
        if machine in {"amd64", "x86_64", "x64", "x86-64"}:
            return "windows", "x86_64"
        return "windows", machine

    return system, machine


def opencv_platform_dirname(system: str, machine: str) -> str:
    return f"{system}-{machine}"


def resolve_paddle_inference_url(
    device: str = DEFAULT_PADDLE_DEVICE,
) -> tuple[str, str]:
    system, machine = resolve_platform_key()
    device_urls = PADDLE_INFERENCE_V3_URLS.get(device)
    if device_urls is None:
        raise RuntimeError(f"지원되지 않는 Paddle Inference device입니다: {device}")

    platform_urls = device_urls.get(system)
    if platform_urls is None:
        raise RuntimeError(f"{device} Paddle Inference SDK를 지원하지 않는 OS입니다: {platform.system()}")

    arch_urls = platform_urls.get(machine)
    if arch_urls is None:
        raise RuntimeError(
            f"{device} Paddle Inference SDK를 지원하지 않는 플랫폼 조합입니다: "
            f"OS={platform.system()}, arch={platform.machine()}"
        )

    variant = "default" if device == "cpu" else PADDLE_GPU_CUDA_VARIANT
    url = arch_urls.get(variant)
    if url is None:
        supported = ", ".join(sorted(arch_urls))
        raise RuntimeError(
            f"지원되지 않는 Paddle Inference SDK 변형입니다: device={device}. "
            f"지원 변형: {supported}"
        )
    return system, url


def resolve_archive_filename(
    url: str,
    device: str = DEFAULT_PADDLE_DEVICE,
) -> str:
    ext = ".tgz"
    if url.endswith(".zip"):
        ext = ".zip"
    elif url.endswith(".tar.gz"):
        ext = ".tar.gz"
    elif url.endswith(".tar.xz"):
        ext = ".tar.xz"
    elif url.endswith(".tar.bz2"):
        ext = ".tar.bz2"
    elif not url.endswith(".tgz"):
        raise ValueError(f"지원하지 않는 다운로드 URL입니다: {url}")

    system, _ = resolve_platform_key()
    variant = device if device == "cpu" else f"{device}-{PADDLE_GPU_CUDA_VARIANT}"
    return f"paddle_inference-{PADDLE_INFERENCE_VERSION}-{system}-{variant}{ext}"


def download_paddle_inference_archive(
    download_dir: Path,
    force: bool = False,
    device: str = DEFAULT_PADDLE_DEVICE,
) -> Path:
    _, url = resolve_paddle_inference_url(device=device)
    filename = resolve_archive_filename(url, device=device)
    destination = download_dir / filename

    if destination.exists() and destination.stat().st_size > 0 and not force:
        print(f"기존 아카이브 사용: {destination}")
        return destination

    download_dir.mkdir(parents=True, exist_ok=True)
    print(f"다운로드: {url}")
    try:
        with urllib.request.urlopen(url, timeout=120) as response:
            with destination.open("wb") as target:
                shutil.copyfileobj(response, target)
    except urllib.error.URLError as exc:
        raise RuntimeError(f"다운로드 실패: {url} ({exc})") from exc

    print(f"저장: {destination}")
    return destination


def download_file(url: str, destination: Path, force: bool = False) -> Path:
    if destination.exists() and destination.stat().st_size > 0 and not force:
        print(f"기존 아카이브 사용: {destination}")
        return destination

    destination.parent.mkdir(parents=True, exist_ok=True)
    print(f"다운로드: {url}")
    try:
        with urllib.request.urlopen(url, timeout=120) as response:
            with destination.open("wb") as target:
                shutil.copyfileobj(response, target)
    except urllib.error.URLError as exc:
        raise RuntimeError(f"다운로드 실패: {url} ({exc})") from exc

    print(f"저장: {destination}")
    return destination


def extract_archive(archive_path: Path, to_dir: Path) -> None:
    mode = detect_archive_mode(archive_path)
    if mode == "zip":
        with zipfile.ZipFile(archive_path, "r") as zf:
            zf.extractall(to_dir)
        return

    tar_mode = {"tar.gz": "r:gz", "tar.xz": "r:xz", "tar.bz2": "r:bz2"}[mode]
    with tarfile.open(archive_path, tar_mode) as tf:
        tf.extractall(to_dir)


def resolve_candidate_roots(base_dir: Path) -> list[Path]:
    candidates = [base_dir]
    candidates.extend([base_dir / "paddle", base_dir / "paddle_inference"])
    for child in base_dir.iterdir():
        if child.is_dir():
            candidates.append(child)
    # 중복 제거
    seen = set[Path]()
    ordered = []
    for candidate in candidates:
        candidate_resolved = candidate.resolve()
        if candidate_resolved not in seen and candidate.exists():
            seen.add(candidate_resolved)
            ordered.append(candidate)
    return ordered


def resolve_layout_root(base_dir: Path) -> Optional[Path]:
    for candidate in resolve_candidate_roots(base_dir):
        if candidate.joinpath("include").is_dir() and candidate.joinpath("lib").is_dir():
            return candidate
    return None


def resolve_third_party_root(base_dir: Path, layout_root: Path) -> Optional[Path]:
    candidates = [
        layout_root / "third_party",
        layout_root.parent / "third_party",
        base_dir / "third_party",
    ]
    for candidate in candidates:
        if candidate.is_dir():
            return candidate
    return None


def replace_directory(src: Path, dst: Path) -> None:
    if dst.exists():
        if dst.is_dir():
            shutil.rmtree(dst)
        else:
            raise ValueError(f"목적지가 파일입니다: {dst}")
    shutil.copytree(src, dst)


def merge_directory(src: Path, dst: Path) -> None:
    if not src.is_dir():
        raise ValueError(f"소스 디렉터리가 없습니다: {src}")
    dst.mkdir(parents=True, exist_ok=True)
    for child in src.iterdir():
        target = dst / child.name
        if child.is_dir():
            if target.exists():
                if not target.is_dir():
                    raise ValueError(f"목적지가 디렉터리가 아닙니다: {target}")
                shutil.rmtree(target)
            shutil.copytree(child, target)
        else:
            if target.exists() and target.is_dir():
                raise ValueError(f"목적지가 파일이 아닙니다: {target}")
            shutil.copy2(child, target)


def write_sidecar_runtime_manifest(destination_dir: Path) -> Path:
    manifest_path = destination_dir / "third_party" / "sidecar-runtime-manifest.json"
    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    manifest_path.write_text(
        json.dumps(
            {
                "paddle_inference": PADDLE_INFERENCE_VERSION,
                "paddleocr": "3.3.0",
                "paddlex_ocr_core": "3.3.13",
                "paddlepaddle": PADDLE_INFERENCE_VERSION,
                "opencv_contrib_python": OPENCV_CONTRIB_PYTHON_VERSION,
                "opencv": OPENCV_VERSION,
                "pyclipper": PYCLIPPER_VERSION,
                "shapely": SHAPELY_VERSION,
            },
            ensure_ascii=False,
            indent=2,
        ),
        encoding="utf-8",
    )
    print(f"sidecar 런타임 매니페스트 기록: {manifest_path}")
    return manifest_path


def setup_pyclipper_cpp(download_dir: Path, destination_dir: Path, force: bool = False) -> Path:
    archive_path = download_file(
        PYCLIPPER_SOURCE_URL,
        download_dir / f"pyclipper-{PYCLIPPER_VERSION}.tar.gz",
        force=force,
    )

    with TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        print(f"pyclipper 아카이브 추출: {archive_path}")
        with tarfile.open(archive_path, "r:gz") as tf:
            tf.extractall(tmp_path)

        source_root = tmp_path / f"pyclipper-{PYCLIPPER_VERSION}"
        src_dir = source_root / "src"
        required = [
            src_dir / "clipper.cpp",
            src_dir / "clipper.hpp",
            src_dir / "extra_defines.hpp",
        ]
        missing = [path for path in required if not path.exists()]
        if missing:
            raise RuntimeError(
                "pyclipper C++ 소스 배치를 확인하지 못했습니다: "
                + ", ".join(str(path) for path in missing)
            )

        pyclipper_dst = destination_dir / "third_party" / f"pyclipper-{PYCLIPPER_VERSION}"
        replace_directory(source_root, pyclipper_dst)
        print(f"pyclipper C++ 소스 배치 완료: {pyclipper_dst}")
        return pyclipper_dst


def path_has_opencv_headers(root: Path) -> bool:
    return any(
        candidate.is_dir()
        for candidate in [
            root / "include" / "opencv2",
            root / "include" / "opencv4" / "opencv2",
            root / "opencv" / "build" / "include" / "opencv2",
            root / "opencv" / "build" / "include" / "opencv4" / "opencv2",
            root / "build" / "include" / "opencv2",
            root / "install" / "include" / "opencv2",
            root / "install" / "include" / "opencv4" / "opencv2",
        ]
    )


def path_has_opencv_libs(root: Path) -> bool:
    patterns = ["*opencv_world*.*", "*opencv_core*.*", "*opencv_imgproc*.*", "*opencv_imgcodecs*.*"]
    return any(any(root.rglob(pattern)) for pattern in patterns)


def validate_opencv_sdk_dir(root: Path) -> None:
    if not root.is_dir():
        raise RuntimeError(f"OpenCV SDK 경로가 디렉터리가 아닙니다: {root}")
    if not path_has_opencv_headers(root):
        raise RuntimeError(f"OpenCV SDK 헤더 경로를 찾지 못했습니다: {root}")
    if not path_has_opencv_libs(root):
        raise RuntimeError(f"OpenCV SDK 라이브러리 경로를 찾지 못했습니다: {root}")


def import_opencv_sdk(source_dir: Path, destination_dir: Path) -> Path:
    system, machine = resolve_platform_key()
    platform_dir = (
        destination_dir
        / "third_party"
        / "opencv-sdk"
        / opencv_platform_dirname(system, machine)
    )
    validate_opencv_sdk_dir(source_dir)
    platform_dir.parent.mkdir(parents=True, exist_ok=True)
    replace_directory(source_dir, platform_dir)
    print(f"OpenCV SDK 배치 완료: {platform_dir}")
    return platform_dir


def setup_opencv_sdk(
    download_dir: Path,
    destination_dir: Path,
    force: bool = False,
    source_dir: Path | None = None,
) -> Path | None:
    if source_dir is not None:
        return import_opencv_sdk(source_dir, destination_dir)

    system, machine = resolve_platform_key()
    if system != "windows":
        print(
            "OpenCV SDK 자동 다운로드는 현재 Windows만 지원합니다. "
            "Linux/macOS는 --opencv-sdk-dir로 기존 SDK를 가져와 .paddle_inference 아래에 배치하세요."
        )
        return None

    archive_path = download_file(
        OPENCV_WINDOWS_URL,
        download_dir / f"opencv-{OPENCV_VERSION}-windows.exe",
        force=force,
    )
    opencv_root = (
        destination_dir
        / "third_party"
        / "opencv-sdk"
        / opencv_platform_dirname(system, machine)
    )
    if path_has_opencv_headers(opencv_root) and path_has_opencv_libs(opencv_root) and not force:
        print(f"기존 OpenCV SDK 사용: {opencv_root}")
        return opencv_root

    if opencv_root.exists():
        shutil.rmtree(opencv_root)
    opencv_root.parent.mkdir(parents=True, exist_ok=True)

    print(f"OpenCV SDK 추출: {archive_path} -> {opencv_root}")
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            (
                "Start-Process "
                f"-FilePath '{archive_path}' "
                f"-ArgumentList '-o{opencv_root}','-y' "
                "-WindowStyle Hidden -Wait"
            ),
        ],
        check=True,
    )
    validate_opencv_sdk_dir(opencv_root)
    print(f"OpenCV SDK 배치 완료: {opencv_root}")
    return opencv_root


def setup_paddle_inference(archive_path: Path, destination_dir: Path) -> Path:
    if not archive_path.is_file():
        raise FileNotFoundError(f"아카이브를 찾을 수 없습니다: {archive_path}")

    with TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        print(f"아카이브 추출: {archive_path}")
        extract_archive(archive_path, tmp_path)

        layout_root = resolve_layout_root(tmp_path)
        if layout_root is None:
            # 일부 배포본은 단일 최상위 폴더 안에 실제 레이아웃이 들어있을 수 있다.
            for nested in tmp_path.iterdir():
                if not nested.is_dir():
                    continue
                layout_root = resolve_layout_root(nested)
                if layout_root is not None:
                    break
        if layout_root is None:
            raise RuntimeError(
                f"지원 가능한 include/lib 배치를 찾지 못했습니다: {tmp_path}"
            )

        destination_dir.mkdir(parents=True, exist_ok=True)
        replace_directory(layout_root / "include", destination_dir / "include")
        replace_directory(layout_root / "lib", destination_dir / "lib")

        source_third_party = resolve_third_party_root(tmp_path, layout_root)
        if source_third_party is not None:
            merge_directory(source_third_party, destination_dir / "third_party")

        print(f"Paddle Inference 배치 완료: {destination_dir}")
        print(f"감지된 배치: {layout_root}")
        print(f"권장 설치 경로: {destination_dir}")
        return destination_dir


def main() -> int:
    args = parse_args()
    destination_dir = Path(args.destination_dir)
    download_dir = Path(args.download_dir)
    archive_path = download_paddle_inference_archive(
        download_dir,
        force=args.force_download,
        device=args.device,
    )
    setup_paddle_inference(archive_path, destination_dir)
    write_sidecar_runtime_manifest(destination_dir)
    setup_opencv_sdk(
        download_dir,
        destination_dir,
        force=args.force_download,
        source_dir=Path(args.opencv_sdk_dir) if args.opencv_sdk_dir else None,
    )
    setup_pyclipper_cpp(download_dir, destination_dir, force=args.force_download)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
