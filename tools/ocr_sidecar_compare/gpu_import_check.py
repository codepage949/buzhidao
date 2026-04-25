import json
import os
import sys
import traceback
from argparse import ArgumentParser
from importlib import metadata
from pathlib import Path

os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
os.environ.setdefault("DISABLE_MODEL_SOURCE_CHECK", "True")


def configure_stdio() -> None:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    if hasattr(sys.stderr, "reconfigure"):
        sys.stderr.reconfigure(encoding="utf-8", errors="replace")


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


def print_event(event_type: str, **payload: object) -> None:
    print(
        json.dumps(
            {
                "type": event_type,
                **payload,
            },
            ensure_ascii=False,
        ),
        flush=True,
    )


def import_paddle() -> int:
    try:
        import paddle

        compiled_with_cuda = bool(paddle.device.is_compiled_with_cuda())
        device_count = (
            int(paddle.device.cuda.device_count()) if compiled_with_cuda else 0
        )
        active_device = None
        tensor_place = None
        if device_count > 0:
            active_device = str(paddle.device.set_device("gpu:0"))
            tensor_place = str(paddle.to_tensor([1.0]).place)

        print_event(
            "imported_paddle",
            version=getattr(paddle, "__version__", "unknown"),
            compiled_with_cuda=compiled_with_cuda,
            compiled_with_rocm=bool(paddle.device.is_compiled_with_rocm()),
            cuda_device_count=device_count,
            active_device=active_device,
            tensor_place=tensor_place,
        )
    except Exception as exc:
        print_event(
            "import_paddle_failed",
            error=repr(exc),
            traceback=traceback.format_exc(),
        )
        return 1
    return 0


def import_paddleocr() -> int:
    try:
        from paddleocr import PaddleOCR

        print_event(
            "imported_paddleocr",
            version=metadata.version("paddleocr"),
            paddle_ocr_class=bool(PaddleOCR),
        )
    except Exception as exc:
        print_event(
            "import_paddleocr_failed",
            error=repr(exc),
            traceback=traceback.format_exc(),
        )
        return 1
    return 0


def parse_args():
    parser = ArgumentParser()
    parser.add_argument(
        "--import-order",
        choices=("paddle-first", "paddleocr-first"),
        default="paddle-first",
    )
    return parser.parse_args()


def main() -> int:
    configure_stdio()
    configure_frozen_dll_search_path()
    args = parse_args()

    print_event(
        "startup",
        executable=sys.executable,
        frozen=bool(getattr(sys, "frozen", False)),
        python=sys.version,
        import_order=args.import_order,
    )

    import_steps = (
        (import_paddle, import_paddleocr)
        if args.import_order == "paddle-first"
        else (import_paddleocr, import_paddle)
    )
    for import_step in import_steps:
        exit_code = import_step()
        if exit_code != 0:
            return exit_code

    print_event("success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
