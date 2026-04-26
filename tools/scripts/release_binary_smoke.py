from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tarfile
import tempfile
import zipfile
from dataclasses import dataclass
from pathlib import Path


DEFAULT_TIMEOUT_SECONDS = 300
SMOKE_ARG = "--release-ocr-smoke"


@dataclass(frozen=True)
class SmokeResult:
    command: list[str]
    returncode: int
    stdout: str
    stderr: str


def configure_utf8_stdio() -> None:
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is None:
            continue

        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue

        reconfigure(encoding="utf-8", errors="replace")


def binary_name_for_os(os_name: str) -> str:
    if os_name == "windows":
        return "buzhidao.exe"
    if os_name == "linux":
        return "buzhidao"
    raise ValueError(f"지원하지 않는 OS: {os_name}")


def extract_archive(archive_path: Path, destination: Path) -> None:
    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path) as zf:
            zf.extractall(destination)
        return

    if archive_path.suffixes[-2:] == [".tar", ".gz"]:
        with tarfile.open(archive_path, "r:gz") as tf:
            try:
                tf.extractall(destination, filter="data")
            except TypeError:
                tf.extractall(destination)
        return

    raise ValueError(f"지원하지 않는 아카이브 형식: {archive_path}")


def find_release_binary(extracted_dir: Path, os_name: str) -> Path:
    name = binary_name_for_os(os_name)
    direct = extracted_dir / name
    if direct.is_file():
        return direct

    matches = sorted(path for path in extracted_dir.rglob(name) if path.is_file())
    if matches:
        return matches[0]

    raise FileNotFoundError(f"릴리즈 실행 파일을 찾지 못했습니다: {name}")


def smoke_env(
    base_env: dict[str, str],
    image_path: Path,
    model_root: Path,
    source: str,
    device: str,
) -> dict[str, str]:
    env = base_env.copy()
    env["BUZHIDAO_RELEASE_OCR_SMOKE_IMAGE"] = str(image_path)
    env["BUZHIDAO_PADDLE_MODEL_ROOT"] = str(model_root)
    env["BUZHIDAO_RELEASE_OCR_SMOKE_SOURCE"] = source
    env["OCR_SERVER_DEVICE"] = device
    return env


def run_extracted_binary_smoke(
    binary_path: Path,
    image_path: Path,
    model_root: Path,
    source: str = "en",
    device: str = "cpu",
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS,
    base_env: dict[str, str] | None = None,
) -> SmokeResult:
    if os.name != "nt":
        mode = binary_path.stat().st_mode
        binary_path.chmod(mode | 0o755)

    command = [str(binary_path), SMOKE_ARG]
    proc = subprocess.run(
        command,
        cwd=binary_path.parent,
        env=smoke_env(base_env or os.environ, image_path, model_root, source, device),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=timeout_seconds,
        check=False,
    )
    result = SmokeResult(command, proc.returncode, proc.stdout, proc.stderr)
    if proc.returncode != 0:
        raise RuntimeError(format_smoke_failure(result))
    return result


def run_archive_smoke(
    archive_path: Path,
    os_name: str,
    image_path: Path,
    model_root: Path,
    source: str = "en",
    device: str = "cpu",
    timeout_seconds: int = DEFAULT_TIMEOUT_SECONDS,
) -> SmokeResult:
    with tempfile.TemporaryDirectory(prefix="buzhidao-release-smoke-") as td:
        extracted_dir = Path(td)
        extract_archive(archive_path, extracted_dir)
        binary_path = find_release_binary(extracted_dir, os_name)
        return run_extracted_binary_smoke(
            binary_path,
            image_path,
            model_root,
            source=source,
            device=device,
            timeout_seconds=timeout_seconds,
        )


def format_smoke_failure(result: SmokeResult) -> str:
    return "\n".join(
        [
            f"릴리즈 바이너리 OCR smoke 실패: exit={result.returncode}",
            f"command={' '.join(result.command)}",
            "stdout:",
            result.stdout.strip(),
            "stderr:",
            result.stderr.strip(),
        ]
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--archive", required=True)
    parser.add_argument("--os", dest="os_name", choices=["windows", "linux"], required=True)
    parser.add_argument("--image", required=True)
    parser.add_argument("--model-root", required=True)
    parser.add_argument("--source", default="en")
    parser.add_argument("--device", default="cpu", choices=["cpu", "gpu"])
    parser.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT_SECONDS)
    return parser.parse_args()


def main() -> int:
    configure_utf8_stdio()
    args = parse_args()
    try:
        result = run_archive_smoke(
            Path(args.archive).resolve(),
            args.os_name,
            Path(args.image).resolve(),
            Path(args.model_root).resolve(),
            source=args.source,
            device=args.device,
            timeout_seconds=args.timeout,
        )
    except subprocess.TimeoutExpired as exc:
        print(f"릴리즈 바이너리 OCR smoke 시간 초과: {exc.timeout}s")
        return 1
    except Exception as exc:
        print(str(exc))
        return 1

    print(f"릴리즈 바이너리 OCR smoke 통과: {' '.join(result.command)}")
    if result.stdout.strip():
        print(result.stdout.strip())
    if result.stderr.strip():
        print(result.stderr.strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
