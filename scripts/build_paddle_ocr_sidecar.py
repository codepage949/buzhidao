"""PyInstaller로 PaddleOCR sidecar 실행 파일을 만든다."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
SCRIPT = ROOT / "scripts" / "paddle_ocr_sidecar.py"
DIST = ROOT / "dist"
BUILD = ROOT / "build" / "pyinstaller-paddle-ocr"
HIDDEN_IMPORTS = [
    "imagesize",
    "cv2",
    "pyclipper",
    "pypdfium2",
    "bidi.algorithm",
    "shapely",
    "regex",
]
COPY_METADATA = [
    "paddlex",
    "paddleocr",
    "paddlepaddle",
    "imagesize",
    "opencv-contrib-python",
    "pyclipper",
    "pypdfium2",
    "python-bidi",
    "shapely",
    "regex",
]
EXCLUDE_MODULES = [
    "sklearn",
    "scipy",
    "tokenizers",
    "tiktoken",
    "sentencepiece",
    "openpyxl",
    "premailer",
    "jinja2",
    "lxml",
]


def main() -> int:
    cmd = [
        sys.executable,
        "-m",
        "PyInstaller",
        "--noconfirm",
        "--clean",
        "--onedir",
        "--name",
        "paddle_ocr_sidecar",
        "--collect-data",
        "paddlex",
        "--collect-data",
        "paddleocr",
        "--collect-binaries",
        "paddle",
        "--distpath",
        str(DIST),
        "--workpath",
        str(BUILD),
        str(SCRIPT),
    ]
    for hidden_import in HIDDEN_IMPORTS:
        cmd.extend(["--hidden-import", hidden_import])
    for package in COPY_METADATA:
        cmd.extend(["--copy-metadata", package])
    for module_name in EXCLUDE_MODULES:
        cmd.extend(["--exclude-module", module_name])
    print(" ".join(cmd))
    return subprocess.call(cmd, cwd=ROOT)


if __name__ == "__main__":
    raise SystemExit(main())
