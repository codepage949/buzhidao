import os
import shutil
import subprocess
from argparse import ArgumentParser
from importlib import metadata
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PROJECT_ROOT = ROOT.parents[1]
DIST = ROOT / "dist"
BUILD = ROOT / "build"
SHARED_LANGS_JSON = PROJECT_ROOT / "shared" / "langs.json"
OCR_CORE_METADATA_PACKAGES = [
    "paddlex",
    "paddleocr",
    "imagesize",
    "opencv-contrib-python",
    "pyclipper",
    "pypdfium2",
    "python-bidi",
    "shapely",
]
GPU_RUNTIME_PACKAGES = [
    "nvidia.cublas",
    "nvidia.cuda_nvrtc",
    "nvidia.cuda_runtime",
    "nvidia.cudnn",
    "nvidia.cufft",
    "nvidia.curand",
    "nvidia.cusolver",
    "nvidia.cusparse",
]
BUILD_TARGETS = {
    "ocr-sidecar-compare": {
        "entry": ROOT / "ocr_sidecar_compare.py",
        "name": "ocr_sidecar_compare",
        "collect_data": ["paddlex", "paddleocr"],
        "collect_binaries": ["paddle"],
        "metadata_packages": [],
        "recursive_metadata_packages": OCR_CORE_METADATA_PACKAGES,
    },
    "gpu-import-check": {
        "entry": ROOT / "gpu_import_check.py",
        "name": "gpu_import_check",
        "collect_data": ["paddleocr", "paddlex"],
        "collect_binaries": ["paddle"],
        "metadata_packages": ["paddlepaddle-gpu", "paddleocr", "paddlex"],
        "recursive_metadata_packages": [],
    },
}


def parse_args():
    parser = ArgumentParser()
    parser.add_argument(
        "--gpu",
        action="store_true",
        help="Build with the GPU dependency set (requires paddlepaddle-gpu in the uv environment).",
    )
    parser.add_argument(
        "--onefile",
        action="store_true",
        help="Build a onefile executable instead of the default onedir layout.",
    )
    parser.add_argument(
        "--target",
        choices=sorted(BUILD_TARGETS.keys()),
        default="ocr-sidecar-compare",
        help="Select which executable entrypoint to build.",
    )
    return parser.parse_args()


def ensure_runtime_dependency(use_gpu: bool) -> None:
    package_name = "paddlepaddle-gpu" if use_gpu else "paddlepaddle"
    group_name = "gpu" if use_gpu else "cpu"
    try:
        metadata.version(package_name)
    except metadata.PackageNotFoundError as exc:
        raise SystemExit(
            f"{package_name} is not installed in the current uv environment. "
            f"Run `uv sync --group build --group {group_name}` first."
        ) from exc


def main() -> int:
    args = parse_args()
    ensure_runtime_dependency(args.gpu)
    target = BUILD_TARGETS[args.target]
    target_name = target["name"]
    spec = ROOT / f"{target_name}.spec"
    runtime_package = "paddlepaddle-gpu" if args.gpu else "paddlepaddle"

    for path in (DIST, BUILD):
        if path.exists():
            shutil.rmtree(path)
    if spec.exists():
        spec.unlink()

    cmd = [
        "pyinstaller",
        "--noconfirm",
        "--clean",
        "--noconsole",
        "--onefile" if args.onefile else "--onedir",
        "--name",
        str(target_name),
        "--add-data",
        f"{SHARED_LANGS_JSON}{os.pathsep}shared",
        str(target["entry"]),
    ]
    for package_name in target["collect_data"]:
        cmd.extend(["--collect-data", package_name])
    binary_packages = [*target["collect_binaries"]]
    if args.gpu:
        binary_packages.extend(GPU_RUNTIME_PACKAGES)

    for package_name in binary_packages:
        cmd.extend(["--collect-binaries", package_name])
    metadata_packages = [*target["metadata_packages"]]
    recursive_metadata_packages = [*target["recursive_metadata_packages"]]
    if args.target == "ocr-sidecar-compare":
        metadata_packages.append(runtime_package)

    for package_name in metadata_packages:
        cmd.extend(["--copy-metadata", package_name])
    for package_name in recursive_metadata_packages:
        cmd.extend(["--recursive-copy-metadata", package_name])
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
