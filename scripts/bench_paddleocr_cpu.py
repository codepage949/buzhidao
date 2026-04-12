"""PaddleOCR CPU 경로를 단일 이미지 기준으로 벤치한다.

사용 예:
    python scripts/bench_paddleocr_cpu.py benchmarks/test.png --lang en
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from paddleocr import PaddleOCR


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="PaddleOCR CPU benchmark")
    parser.add_argument("image", type=Path, help="입력 이미지 경로")
    parser.add_argument("--lang", default="en", help="PaddleOCR lang 값")
    parser.add_argument(
        "--det-model-name",
        default=None,
        help="text_detection_model_name override",
    )
    parser.add_argument(
        "--rec-model-name",
        default=None,
        help="text_recognition_model_name override",
    )
    parser.add_argument(
        "--warmup",
        action="store_true",
        help="실측 전에 한 번 더 실행해 warm 상태도 함께 본다",
    )
    return parser.parse_args()


def build_ocr(
    lang: str,
    det_model_name: str | None = None,
    rec_model_name: str | None = None,
) -> PaddleOCR:
    kwargs: dict[str, object] = {
        "use_doc_orientation_classify": False,
        "use_doc_unwarping": False,
        "use_textline_orientation": True,
        "lang": lang,
        "device": "cpu",
    }
    if det_model_name:
        kwargs["text_detection_model_name"] = det_model_name
    if rec_model_name:
        kwargs["text_recognition_model_name"] = rec_model_name

    return PaddleOCR(
        **kwargs,
    )


def run_once(ocr: PaddleOCR, image_path: Path) -> dict[str, object]:
    t0 = time.perf_counter()
    result = ocr.ocr(str(image_path))
    elapsed_ms = (time.perf_counter() - t0) * 1000.0

    lines: list[dict[str, object]] = []
    for block in result:
        if not block:
            continue
        if isinstance(block, dict):
            texts = block.get("rec_texts") or []
            scores = block.get("rec_scores") or []
            polys = block.get("rec_polys") or []
            for text, score, polygon in zip(texts, scores, polys):
                lines.append(
                    {
                        "text": text,
                        "score": float(score),
                        "polygon": polygon.tolist() if hasattr(polygon, "tolist") else polygon,
                    }
                )
            continue
        for line in block:
            polygon, rec = line
            text, score = rec
            lines.append(
                {
                    "text": text,
                    "score": float(score),
                    "polygon": polygon.tolist() if hasattr(polygon, "tolist") else polygon,
                }
            )

    return {
        "elapsed_ms": round(elapsed_ms, 1),
        "line_count": len(lines),
        "lines": lines,
    }


def main() -> None:
    args = parse_args()
    image_path = args.image.resolve()
    if not image_path.exists():
        raise SystemExit(f"이미지 없음: {image_path}")

    ocr = build_ocr(args.lang, args.det_model_name, args.rec_model_name)

    output: dict[str, object] = {
        "image": str(image_path),
        "lang": args.lang,
        "det_model_name": args.det_model_name,
        "rec_model_name": args.rec_model_name,
    }

    if args.warmup:
        output["warmup"] = run_once(ocr, image_path)

    output["measure"] = run_once(ocr, image_path)
    print(json.dumps(output, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
