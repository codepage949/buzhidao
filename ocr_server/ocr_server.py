import argparse
import json
import os
import sys
import tempfile
from pathlib import Path

os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
os.environ.setdefault("DISABLE_MODEL_SOURCE_CHECK", "True")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_LANG = "en"


def _load_langs_json() -> tuple[str, ...]:
    base = getattr(sys, "_MEIPASS", None) or Path(__file__).resolve().parent.parent
    langs_path = Path(base) / "shared" / "langs.json"
    if not langs_path.is_file():
        langs_path = Path(__file__).resolve().parent.parent / "shared" / "langs.json"
    with open(langs_path, encoding="utf-8") as f:
        entries = json.load(f)
    return tuple(entry["code"] for entry in entries)


LANGS = _load_langs_json()


def resolve_selected_lang() -> str:
    lang = os.environ.get("PYTHON_OCR_LANG", "").strip().lower()
    if lang in LANGS:
        return lang
    if lang:
        print(
            f"unsupported PYTHON_OCR_LANG: {lang} (falling back to {DEFAULT_LANG})",
            file=sys.stderr,
            flush=True,
        )
    return DEFAULT_LANG


# 1×1 24bpp BMP — PaddleOCR 모델 워밍업 전용 최소 입력
WARMUP_IMAGE = bytes.fromhex(
    "424d3a0000000000000036000000280000000100000001000000010018000000"
    "000004000000130b0000130b00000000000000000000ffffff00"
)


def configure_windows_paddle_probe_env() -> None:
    if os.name != "nt":
        return

    shim_dir = Path(tempfile.gettempdir()) / "buzhidao-paddle-shims"
    try:
        shim_dir.mkdir(parents=True, exist_ok=True)
        (shim_dir / "ccache").touch(exist_ok=True)
    except OSError:
        return

    shim = str(shim_dir)
    path = os.environ.get("PATH", "")
    entries = path.split(os.pathsep) if path else []
    if shim not in entries:
        os.environ["PATH"] = os.pathsep.join([shim, *entries]) if entries else shim

    # Paddle imports cpp_extension on startup and probes `where nvcc` / `where ccache`
    # even though we never build custom ops at runtime. Seed harmless existing paths to
    # keep that import from spawning short-lived console processes on Windows.
    os.environ.setdefault("CUDA_HOME", shim)
    os.environ.setdefault("CUDA_PATH", shim)


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
    nvidia_root = Path(base_dir) / "nvidia"
    if nvidia_root.is_dir():
        candidates.extend(
            str(path)
            for path in nvidia_root.glob("*/bin")
            if path.is_dir()
        )

    added = []
    for path in candidates:
        if not os.path.isdir(path):
            continue
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(path)
        added.append(path)

    if added:
        os.environ["PATH"] = os.pathsep.join(added + [os.environ.get("PATH", "")])


def resolve_ocr_device() -> str:
    device = os.environ.get("PYTHON_OCR_DEVICE", "cpu").strip().lower()
    if device not in ("cpu", "gpu"):
        raise ValueError(
            f"unsupported PYTHON_OCR_DEVICE: {device} (expected: cpu or gpu)"
        )
    return device


def _load_paddleocr():
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
    return PaddleOCR


def build_ocr(lang: str):
    PaddleOCR = _load_paddleocr()
    return PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device=resolve_ocr_device(),
        lang=lang,
    )


def warmup_models(ocrs: dict) -> None:
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
    ocr,
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


def emit(obj: dict) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def parse_request(line: str) -> tuple[int, str, str, float]:
    request = json.loads(line)
    return (
        int(request["id"]),
        request["source"],
        request["image_path"],
        float(request.get("score_thresh", 0.5)),
    )


def run_server() -> int:
    selected = resolve_selected_lang()
    ocrs: dict = {selected: build_ocr(selected)}
    warmup_models(ocrs)
    emit({"type": "ready", "langs": [selected]})

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request_id, source, image_path, score_thresh = parse_request(line)
        except Exception as exc:
            emit({"type": "error", "id": -1, "message": f"invalid request: {exc}"})
            continue

        try:
            if source not in ocrs:
                raise ValueError(
                    f"selected language is '{selected}' but request source is '{source}'; "
                    "restart OCR server with the new PYTHON_OCR_LANG to switch language"
                )
            detections, debug_detections = predict_image(
                ocrs[source], image_path, score_thresh
            )
            emit(
                {
                    "type": "result",
                    "id": request_id,
                    "detections": detections,
                    "debug_detections": debug_detections,
                }
            )
        except Exception as exc:
            emit({"type": "error", "id": request_id, "message": str(exc)})

    return 0


def run_single(image_path: str, source: str, score_thresh: float) -> int:
    ocr = build_ocr(source)
    detections, debug_detections = predict_image(ocr, image_path, score_thresh)
    emit({"detections": detections, "debug_detections": debug_detections})
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
    configure_windows_paddle_probe_env()
    args = parse_args()
    if args.server:
        return run_server()
    if not args.image:
        raise SystemExit("--image 또는 --server가 필요합니다")
    return run_single(args.image, args.source, args.score_thresh)


if __name__ == "__main__":
    raise SystemExit(main())
