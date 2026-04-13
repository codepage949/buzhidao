import argparse
import json
import os
import sys
import tempfile

os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
os.environ.setdefault("DISABLE_MODEL_SOURCE_CHECK", "True")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

try:
    from paddleocr import PaddleOCR
except ModuleNotFoundError as exc:
    missing = exc.name or "unknown"
    print(
        (
            f"missing python dependency: {missing}\n"
            "Install OCR server dependencies with uv before building.\n"
            "Example: uv sync --group build"
        ),
        file=sys.stderr,
        flush=True,
    )
    raise


LANGS = ("en", "ch")


WARMUP_IMAGE = bytes(
    [
        0x42,
        0x4D,
        0x3A,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x36,
        0x00,
        0x00,
        0x00,
        0x28,
        0x00,
        0x00,
        0x00,
        0x01,
        0x00,
        0x00,
        0x00,
        0x01,
        0x00,
        0x00,
        0x00,
        0x01,
        0x00,
        0x18,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x04,
        0x00,
        0x00,
        0x00,
        0x13,
        0x0B,
        0x00,
        0x00,
        0x13,
        0x0B,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0x00,
        0xFF,
        0xFF,
        0xFF,
        0x00,
    ]
)


def configure_frozen_dll_search_path() -> None:
    if not getattr(sys, "frozen", False):
        return

    base_dir = getattr(sys, "_MEIPASS", None)
    if not base_dir:
        return

    candidates = [
        base_dir,
        os.path.join(base_dir, "paddle"),
        os.path.join(base_dir, "paddle", "libs"),
        os.path.join(base_dir, "paddle", "base"),
    ]

    added = []
    for path in candidates:
        if not os.path.isdir(path):
            continue
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(path)
        added.append(path)

    if added:
        os.environ["PATH"] = os.pathsep.join(added + [os.environ.get("PATH", "")])


def build_ocr(lang: str) -> PaddleOCR:
    return PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device=os.environ.get("PYTHON_OCR_DEVICE", "cpu"),
        lang=lang,
    )


def warmup_models(ocrs: dict[str, PaddleOCR]) -> None:
    with tempfile.NamedTemporaryFile(suffix=".bmp", delete=False) as fp:
        fp.write(WARMUP_IMAGE)
        warmup_path = fp.name

    try:
        for ocr in ocrs.values():
            try:
                ocr.predict(warmup_path, text_rec_score_thresh=0.5)
            except Exception as exc:  # pragma: no cover
                print(f"warmup failed: {exc}", file=sys.stderr, flush=True)
    finally:
        try:
            os.remove(warmup_path)
        except OSError:
            pass


def predict_image(
    ocr: PaddleOCR,
    image_path: str,
    score_thresh: float,
) -> tuple[list[dict], list[dict]]:
    result = ocr.predict(image_path, text_rec_score_thresh=score_thresh)
    if not result:
        return [], []

    item = result[0]
    polys = item.get("rec_polys", [])
    texts = item.get("rec_texts", [])
    scores = item.get("rec_scores", [1.0] * len(texts))

    detections = []
    debug_detections = []
    for poly, text, score in zip(polys, texts, scores):
        polygon = poly.tolist() if hasattr(poly, "tolist") else poly
        score_value = float(score)
        detections.append({"polygon": polygon, "text": text})
        debug_detections.append(
            {
                "polygon": polygon,
                "text": text,
                "score": score_value,
                "accepted": True,
            }
        )

    return detections, debug_detections


def run_server() -> int:
    ocrs = {lang: build_ocr(lang) for lang in LANGS}
    warmup_models(ocrs)
    print(json.dumps({"type": "ready", "langs": list(LANGS)}), flush=True)

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
            request_id = int(request["id"])
            source = request["source"]
            image_path = request["image_path"]
            score_thresh = float(request.get("score_thresh", 0.5))
        except Exception as exc:
            print(
                json.dumps(
                    {"type": "error", "id": -1, "message": f"invalid request: {exc}"}
                ),
                flush=True,
            )
            continue

        try:
            detections, debug_detections = predict_image(
                ocrs[source], image_path, score_thresh
            )
            print(
                json.dumps(
                    {
                        "type": "result",
                        "id": request_id,
                        "detections": detections,
                        "debug_detections": debug_detections,
                    },
                    ensure_ascii=False,
                ),
                flush=True,
            )
        except Exception as exc:
            print(
                json.dumps(
                    {"type": "error", "id": request_id, "message": str(exc)},
                    ensure_ascii=False,
                ),
                flush=True,
            )

    return 0


def run_single(image_path: str, source: str, score_thresh: float) -> int:
    ocr = build_ocr(source)
    detections, debug_detections = predict_image(ocr, image_path, score_thresh)
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


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", action="store_true")
    parser.add_argument("--image")
    parser.add_argument("--source", default="en", choices=LANGS)
    parser.add_argument("--score-thresh", type=float, default=0.5)
    return parser.parse_args()


def main() -> int:
    configure_frozen_dll_search_path()
    args = parse_args()
    if args.server:
        return run_server()
    if not args.image:
        raise SystemExit("--image 또는 --server가 필요합니다")
    return run_single(args.image, args.source, args.score_thresh)


if __name__ == "__main__":
    raise SystemExit(main())
