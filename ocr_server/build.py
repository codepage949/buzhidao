import shutil
import subprocess
from argparse import ArgumentParser
from importlib import metadata
from pathlib import Path


ROOT = Path(__file__).resolve().parent
ENTRY = ROOT / "ocr_server.py"
DIST = ROOT / "dist"
BUILD = ROOT / "build"
SPEC = ROOT / "ocr_server.spec"
OCR_CORE_METADATA_PACKAGES = [
    "paddlex",
    "paddleocr",
    "paddlepaddle",
    "imagesize",
    "opencv-contrib-python",
    "pyclipper",
    "pypdfium2",
    "python-bidi",
    "shapely",
]


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

    for path in (DIST, BUILD):
        if path.exists():
            shutil.rmtree(path)
    if SPEC.exists():
        SPEC.unlink()

    cmd = [
        "pyinstaller",
        "--noconfirm",
        "--clean",
        "--onefile" if args.onefile else "--onedir",
        "--collect-data",
        "paddlex",
        "--collect-data",
        "paddleocr",
        "--collect-binaries",
        "paddle",
        "--name",
        "ocr_server",
        str(ENTRY),
    ]
    for package_name in OCR_CORE_METADATA_PACKAGES:
        cmd.extend(["--recursive-copy-metadata", package_name])
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
