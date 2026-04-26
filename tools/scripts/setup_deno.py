from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import stat
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path
from tempfile import gettempdir


DEFAULT_VERSION = "v2.x"
DEFAULT_INSTALL_DIR = Path(".deno")
DEFAULT_DOWNLOAD_DIR = Path(gettempdir()) / "buzhidao-deno"
DOWNLOAD_RETRY_COUNT = 3
DOWNLOAD_RETRY_DELAY_SECONDS = 5
DOWNLOAD_TIMEOUT_SECONDS = 120
GITHUB_RELEASES_API = "https://api.github.com/repos/denoland/deno/releases?per_page=50"


def configure_utf8_stdio() -> None:
    for stream_name in ("stdout", "stderr"):
        stream = getattr(sys, stream_name, None)
        if stream is None:
            continue

        reconfigure = getattr(stream, "reconfigure", None)
        if reconfigure is None:
            continue

        reconfigure(encoding="utf-8", errors="replace")


def log(message: str) -> None:
    timestamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print(f"[{timestamp}] {message}", flush=True)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Deno release binary를 재시도 보호와 함께 설치합니다.")
    parser.add_argument(
        "--version",
        default=DEFAULT_VERSION,
        help="설치할 Deno 버전입니다. 예: v2.x, v2.5.6",
    )
    parser.add_argument(
        "--install-dir",
        type=Path,
        default=DEFAULT_INSTALL_DIR,
        help="Deno 설치 대상 디렉터리입니다.",
    )
    parser.add_argument(
        "--download-dir",
        type=Path,
        default=DEFAULT_DOWNLOAD_DIR,
        help="Deno archive 다운로드 캐시 디렉터리입니다.",
    )
    parser.add_argument(
        "--retries",
        type=int,
        default=DOWNLOAD_RETRY_COUNT,
        help=f"네트워크 작업 재시도 횟수입니다. 기본값: {DOWNLOAD_RETRY_COUNT}",
    )
    parser.add_argument(
        "--retry-delay",
        type=int,
        default=DOWNLOAD_RETRY_DELAY_SECONDS,
        help=f"재시도 대기 시간(초)입니다. 기본값: {DOWNLOAD_RETRY_DELAY_SECONDS}",
    )
    parser.add_argument(
        "--timeout",
        type=int,
        default=DOWNLOAD_TIMEOUT_SECONDS,
        help=f"네트워크 timeout(초)입니다. 기본값: {DOWNLOAD_TIMEOUT_SECONDS}",
    )
    return parser.parse_args()


def is_retryable_error(exc: urllib.error.URLError) -> bool:
    if isinstance(exc, urllib.error.HTTPError):
        return exc.code == 408 or exc.code == 429 or 500 <= exc.code <= 599
    return True


def open_url_with_retry(
    url: str,
    retry_count: int = DOWNLOAD_RETRY_COUNT,
    retry_delay_seconds: int = DOWNLOAD_RETRY_DELAY_SECONDS,
    timeout_seconds: int = DOWNLOAD_TIMEOUT_SECONDS,
) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "buzhidao-ci"})
    for attempt in range(1, retry_count + 1):
        log(f"요청: {url}")
        try:
            with urllib.request.urlopen(request, timeout=timeout_seconds) as response:
                return response.read()
        except urllib.error.URLError as exc:
            if attempt >= retry_count or not is_retryable_error(exc):
                raise RuntimeError(f"요청 실패: {url} ({exc})") from exc
            log(f"요청 재시도 예정 ({attempt}/{retry_count}): {url} ({exc})")
            time.sleep(retry_delay_seconds)
    raise RuntimeError(f"요청 실패: {url}")


def resolve_deno_version(
    version: str,
    retry_count: int = DOWNLOAD_RETRY_COUNT,
    retry_delay_seconds: int = DOWNLOAD_RETRY_DELAY_SECONDS,
    timeout_seconds: int = DOWNLOAD_TIMEOUT_SECONDS,
) -> str:
    normalized = version if version.startswith("v") else f"v{version}"
    if normalized.count(".") == 2 and not normalized.endswith(".x"):
        return normalized

    if normalized.endswith(".x"):
        major = normalized[1:].split(".", 1)[0]
        payload = open_url_with_retry(
            GITHUB_RELEASES_API,
            retry_count=retry_count,
            retry_delay_seconds=retry_delay_seconds,
            timeout_seconds=timeout_seconds,
        )
        releases = json.loads(payload.decode("utf-8"))
        for release in releases:
            tag = release.get("tag_name", "")
            if tag.startswith(f"v{major}.") and release.get("prerelease") is not True:
                return tag
        raise RuntimeError(f"Deno {version}에 맞는 release를 찾지 못했습니다.")

    raise ValueError(f"지원하지 않는 Deno version 형식입니다: {version}")


def resolve_asset_name(system_name: str | None = None, machine_name: str | None = None) -> str:
    system = (system_name or platform.system()).lower()
    machine = (machine_name or platform.machine()).lower()
    if machine not in {"x86_64", "amd64"}:
        raise RuntimeError(f"지원하지 않는 Deno architecture입니다: {platform.machine()}")

    if system == "windows":
        return "deno-x86_64-pc-windows-msvc.zip"
    if system == "linux":
        return "deno-x86_64-unknown-linux-gnu.zip"
    raise RuntimeError(f"지원하지 않는 Deno OS입니다: {platform.system()}")


def download_archive(
    tag: str,
    asset_name: str,
    download_dir: Path,
    retry_count: int = DOWNLOAD_RETRY_COUNT,
    retry_delay_seconds: int = DOWNLOAD_RETRY_DELAY_SECONDS,
    timeout_seconds: int = DOWNLOAD_TIMEOUT_SECONDS,
) -> Path:
    download_dir.mkdir(parents=True, exist_ok=True)
    destination = download_dir / f"{tag}-{asset_name}"
    if destination.exists() and destination.stat().st_size > 0:
        log(f"기존 Deno archive 사용: {destination}")
        return destination

    url = f"https://github.com/denoland/deno/releases/download/{tag}/{asset_name}"
    payload = open_url_with_retry(
        url,
        retry_count=retry_count,
        retry_delay_seconds=retry_delay_seconds,
        timeout_seconds=timeout_seconds,
    )
    destination.write_bytes(payload)
    log(f"Deno archive 저장: {destination}")
    return destination


def install_deno_archive(archive_path: Path, install_dir: Path) -> Path:
    bin_dir = install_dir / "bin"
    if bin_dir.exists():
        shutil.rmtree(bin_dir)
    bin_dir.mkdir(parents=True, exist_ok=True)

    with zipfile.ZipFile(archive_path, "r") as zf:
        zf.extractall(bin_dir)

    executable = bin_dir / ("deno.exe" if platform.system().lower() == "windows" else "deno")
    if not executable.exists():
        candidates = sorted(bin_dir.rglob("deno.exe" if platform.system().lower() == "windows" else "deno"))
        if not candidates:
            raise RuntimeError(f"Deno 실행 파일을 찾지 못했습니다: {bin_dir}")
        candidate = candidates[0]
        target = bin_dir / candidate.name
        if candidate != target:
            shutil.move(str(candidate), target)
        executable = target

    if platform.system().lower() != "windows":
        mode = executable.stat().st_mode
        executable.chmod(mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
    return bin_dir


def append_github_path(bin_dir: Path) -> None:
    github_path = os.environ.get("GITHUB_PATH")
    if not github_path:
        return
    with open(github_path, "a", encoding="utf-8") as handle:
        handle.write(str(bin_dir.resolve()) + os.linesep)


def main() -> int:
    configure_utf8_stdio()
    args = parse_args()
    tag = resolve_deno_version(
        args.version,
        retry_count=args.retries,
        retry_delay_seconds=args.retry_delay,
        timeout_seconds=args.timeout,
    )
    asset_name = resolve_asset_name()
    archive_path = download_archive(
        tag,
        asset_name,
        args.download_dir,
        retry_count=args.retries,
        retry_delay_seconds=args.retry_delay,
        timeout_seconds=args.timeout,
    )
    bin_dir = install_deno_archive(archive_path, args.install_dir)
    append_github_path(bin_dir)
    log(f"Deno 설치 완료: {bin_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
