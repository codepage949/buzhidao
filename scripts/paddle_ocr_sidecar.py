"""PaddleOCR CPU sidecar.

PyInstaller로 단일 실행 파일로 빌드해 Rust 앱이 외부 프로세스로 호출하는 용도.
"""

from __future__ import annotations

import argparse
import ctypes
import importlib
import json
import os
import sys
from pathlib import Path


_PADDLEX_OCR_IMPORTS = (
    "imagesize",
    "cv2",
    "pyclipper",
    "pypdfium2",
    "bidi.algorithm",
    "shapely",
    "regex",
)


def configure_runtime_library_path() -> None:
    meipass = getattr(sys, "_MEIPASS", None)
    if not meipass:
        return

    root = Path(meipass)
    lib_dirs: list[Path] = []
    for pattern in ("**/paddle/libs", "**/paddle.libs", "**"):
        for path in root.glob(pattern):
            if path.is_dir() and any(path.glob("*.so*")):
                lib_dirs.append(path)
        if lib_dirs:
            break

    seen: set[str] = set()
    deduped_dirs: list[Path] = []
    for lib_dir in lib_dirs:
        key = str(lib_dir.resolve())
        if key not in seen:
            seen.add(key)
            deduped_dirs.append(lib_dir)

    if not deduped_dirs:
        return

    current = os.environ.get("LD_LIBRARY_PATH", "")
    paths = [str(path) for path in deduped_dirs]
    if current:
        paths.append(current)
    os.environ["LD_LIBRARY_PATH"] = ":".join(paths)

    preload_order = (
        "libiomp5.so",
        "libmklml_intel.so",
        "libdnnl.so.3",
        "libcommon.so",
        "libphi.so",
        "libphi_core.so",
    )
    for lib_dir in deduped_dirs:
        for name in preload_order:
            for lib_path in sorted(lib_dir.glob(name)):
                ctypes.CDLL(str(lib_path), mode=ctypes.RTLD_GLOBAL)


configure_runtime_library_path()

from paddleocr import PaddleOCR


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="PaddleOCR sidecar")
    parser.add_argument("--image", required=True, help="입력 이미지 경로")
    parser.add_argument("--source", default="en", help="OCR 언어 (en/ch)")
    parser.add_argument("--score-thresh", type=float, default=0.5)
    parser.add_argument("--debug-trace", default="false")
    return parser.parse_args()


def to_paddle_lang(source: str) -> str:
    value = source.strip().lower()
    if value in {"ch", "zh", "zh-cn", "cn"}:
        return "ch"
    return "en"


def ensure_paddlex_ocr_runtime() -> None:
    missing: list[str] = []
    for module_name in _PADDLEX_OCR_IMPORTS:
        try:
            importlib.import_module(module_name)
        except Exception:
            missing.append(module_name)

    if missing:
        raise RuntimeError(
            f"PaddleX OCR runtime import 실패: {', '.join(missing)}"
        )

    # PyInstaller onefile 환경에선 PaddleX의 importlib.metadata 기반 extra 검사에서
    # false-negative가 날 수 있다. 위에서 실제 import가 끝났으면 OCR extra는 통과시킨다.
    import paddlex.utils.deps as paddlex_deps

    original_is_extra_available = paddlex_deps.is_extra_available

    def patched_is_extra_available(extra: str) -> bool:
        if extra in {"ocr", "ocr-core"}:
            return True
        return original_is_extra_available(extra)

    paddlex_deps.is_extra_available = patched_is_extra_available


def build_ocr(lang: str) -> PaddleOCR:
    ensure_paddlex_ocr_runtime()
    return PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        lang=lang,
        device="cpu",
    )


def main() -> int:
    args = parse_args()
    image_path = Path(args.image)
    if not image_path.exists():
        print(json.dumps({"error": f"이미지 없음: {image_path}"}, ensure_ascii=False))
        return 1

    ocr = build_ocr(to_paddle_lang(args.source))
    result = ocr.ocr(str(image_path))

    detections: list[dict[str, object]] = []
    debug_detections: list[dict[str, object]] = []

    for block in result:
        if not block:
            continue

        if isinstance(block, dict):
            texts = block.get("rec_texts") or []
            scores = block.get("rec_scores") or []
            polys = block.get("rec_polys") or []
            for text, score, polygon in zip(texts, scores, polys):
                poly = polygon.tolist() if hasattr(polygon, "tolist") else polygon
                accepted = bool(text and float(score) >= args.score_thresh)
                debug_detections.append(
                    {
                        "polygon": poly,
                        "text": text,
                        "score": float(score),
                        "accepted": accepted,
                    }
                )
                if accepted:
                    detections.append({"polygon": poly, "text": text})
            continue

        for line in block:
            polygon, rec = line
            text, score = rec
            poly = polygon.tolist() if hasattr(polygon, "tolist") else polygon
            accepted = bool(text and float(score) >= args.score_thresh)
            debug_detections.append(
                {
                    "polygon": poly,
                    "text": text,
                    "score": float(score),
                    "accepted": accepted,
                }
            )
            if accepted:
                detections.append({"polygon": poly, "text": text})

    print(
        json.dumps(
            {
                "detections": detections,
                "debug_detections": debug_detections,
            },
            ensure_ascii=False,
        )
    )
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    raise SystemExit(main())
