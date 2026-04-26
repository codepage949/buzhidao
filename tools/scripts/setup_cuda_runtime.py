from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import time
import zipfile
from dataclasses import dataclass
from pathlib import Path


SUPPORTED_PLATFORMS = ("windows", "linux")
PIP_DOWNLOAD_RETRY_COUNT = 3
PIP_DOWNLOAD_RETRY_DELAY_SECONDS = 5
PIP_DOWNLOAD_TIMEOUT_SECONDS = 120


@dataclass(frozen=True)
class PackageSet:
    packages: tuple[str, ...]
    extra_index_urls: tuple[str, ...] = ()


PACKAGE_SETS: dict[str, PackageSet] = {
    "ort-cu12": PackageSet(
        packages=(
            "nvidia-cuda-runtime-cu12==12.9.79",
            "nvidia-cublas-cu12==12.9.1.4",
            "nvidia-cufft-cu12==11.4.1.4",
            "nvidia-cudnn-cu12==9.20.0.48",
        ),
    ),
    "paddle-cu126": PackageSet(
        packages=(
            "nvidia-cublas-cu12==12.6.4.1",
            "nvidia-cuda-nvrtc-cu12==12.6.85",
            "nvidia-cuda-runtime-cu12==12.6.77",
            "nvidia-cudnn-cu12==9.5.1.17",
            "nvidia-cufft-cu12==11.3.0.4",
            "nvidia-curand-cu12==10.3.7.77",
            "nvidia-cusolver-cu12==11.7.1.2",
            "nvidia-cusparse-cu12==12.5.4.2",
        ),
    ),
}


def current_platform() -> str:
    if sys.platform.startswith("win"):
        return "windows"
    if sys.platform.startswith("linux"):
        return "linux"
    raise RuntimeError(f"지원하지 않는 OS입니다: {sys.platform}")


def normalize_platform(value: str) -> str:
    platform = current_platform() if value == "auto" else value
    if platform not in SUPPORTED_PLATFORMS:
        raise ValueError(f"지원하지 않는 platform: {value}")
    return platform


def is_cuda_library_member(member: str, platform: str) -> bool:
    parts = member.replace("\\", "/").split("/")
    if len(parts) < 4 or parts[0] != "nvidia":
        return False

    name = parts[-1]
    parent = parts[-2]
    if platform == "windows":
        return parent == "bin" and name.lower().endswith(".dll")
    if platform == "linux":
        return parent == "lib" and (".so" in name)
    raise ValueError(f"지원하지 않는 platform: {platform}")


def iter_cuda_library_members(wheel_path: Path, platform: str) -> list[str]:
    with zipfile.ZipFile(wheel_path) as zf:
        return [
            member
            for member in zf.namelist()
            if not member.endswith("/") and is_cuda_library_member(member, platform)
        ]


def extract_cuda_libraries(wheel_dir: Path, destination_dir: Path, platform: str) -> list[Path]:
    wheels = sorted(wheel_dir.glob("*.whl"))
    if not wheels:
        raise FileNotFoundError(f"wheel 파일을 찾을 수 없습니다: {wheel_dir}")

    destination_dir.mkdir(parents=True, exist_ok=True)
    extracted: list[Path] = []
    seen: set[str] = set()

    for wheel_path in wheels:
        with zipfile.ZipFile(wheel_path) as zf:
            for member in zf.namelist():
                if member.endswith("/") or not is_cuda_library_member(member, platform):
                    continue

                name = Path(member).name
                if name in seen:
                    continue

                out_path = destination_dir / name
                with zf.open(member) as src, out_path.open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                seen.add(name)
                extracted.append(out_path)

    if not extracted:
        raise FileNotFoundError(
            f"{wheel_dir}에서 {platform} CUDA 동적 라이브러리를 찾지 못했습니다."
        )
    return extracted


def clean_directory(path: Path) -> None:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)


def packages_for(package_set: str, package_overrides: list[str] | None) -> PackageSet:
    if package_overrides:
        return PackageSet(tuple(package_overrides))
    try:
        return PACKAGE_SETS[package_set]
    except KeyError as exc:
        supported = ", ".join(sorted(PACKAGE_SETS))
        raise ValueError(f"지원하지 않는 package-set: {package_set} ({supported})") from exc


def download_wheels(
    python_executable: str,
    wheelhouse: Path,
    package_set: PackageSet,
    extra_index_urls: list[str],
    retry_count: int = PIP_DOWNLOAD_RETRY_COUNT,
    retry_delay_seconds: int = PIP_DOWNLOAD_RETRY_DELAY_SECONDS,
    timeout_seconds: int = PIP_DOWNLOAD_TIMEOUT_SECONDS,
) -> None:
    wheelhouse.mkdir(parents=True, exist_ok=True)
    command = [
        python_executable,
        "-m",
        "pip",
        "download",
        "--retries",
        str(retry_count),
        "--timeout",
        str(timeout_seconds),
        "--only-binary",
        ":all:",
        "--dest",
        str(wheelhouse),
    ]
    for url in (*package_set.extra_index_urls, *extra_index_urls):
        command.extend(["--extra-index-url", url])
    command.extend(package_set.packages)

    last_error: subprocess.CalledProcessError | None = None
    for attempt in range(1, retry_count + 1):
        try:
            subprocess.run(command, check=True)
            return
        except subprocess.CalledProcessError as exc:
            last_error = exc
            if attempt >= retry_count:
                raise
            print(
                f"pip download 재시도 예정 ({attempt}/{retry_count})",
                file=sys.stderr,
            )
            time.sleep(retry_delay_seconds)

    if last_error is not None:
        raise last_error


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="NVIDIA wheel에서 CUDA/cuDNN 런타임 라이브러리를 .cuda 디렉토리로 구성합니다."
    )
    parser.add_argument(
        "--platform",
        choices=("auto", *SUPPORTED_PLATFORMS),
        default="auto",
        help="대상 플랫폼. 기본값은 현재 OS입니다.",
    )
    parser.add_argument(
        "--package-set",
        choices=sorted(PACKAGE_SETS),
        default="ort-cu12",
        help="기본 NVIDIA 패키지 묶음입니다.",
    )
    parser.add_argument(
        "--package",
        action="append",
        default=[],
        help="기본 package-set 대신 사용할 pip 패키지 spec입니다. 여러 번 지정할 수 있습니다.",
    )
    parser.add_argument(
        "--extra-index-url",
        action="append",
        default=[],
        help="pip download에 추가할 package index URL입니다.",
    )
    parser.add_argument("--destination-dir", type=Path, default=Path(".cuda"))
    parser.add_argument("--wheelhouse", type=Path, default=Path(".cuda-wheelhouse"))
    parser.add_argument("--python", default=sys.executable, help="pip download에 사용할 Python")
    parser.add_argument(
        "--no-download",
        action="store_true",
        help="pip download를 건너뛰고 wheelhouse의 기존 wheel만 추출합니다.",
    )
    parser.add_argument(
        "--download-retries",
        type=int,
        default=PIP_DOWNLOAD_RETRY_COUNT,
        help=f"pip download 외부 재시도 횟수입니다. 기본값: {PIP_DOWNLOAD_RETRY_COUNT}",
    )
    parser.add_argument(
        "--download-retry-delay",
        type=int,
        default=PIP_DOWNLOAD_RETRY_DELAY_SECONDS,
        help=f"pip download 재시도 대기 시간(초)입니다. 기본값: {PIP_DOWNLOAD_RETRY_DELAY_SECONDS}",
    )
    parser.add_argument(
        "--pip-timeout",
        type=int,
        default=PIP_DOWNLOAD_TIMEOUT_SECONDS,
        help=f"pip download timeout(초)입니다. 기본값: {PIP_DOWNLOAD_TIMEOUT_SECONDS}",
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="destination-dir와 wheelhouse를 먼저 삭제하고 다시 구성합니다.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    platform = normalize_platform(args.platform)
    if args.platform != "auto" and platform != current_platform() and not args.no_download:
        raise RuntimeError(
            "다른 OS용 wheel 다운로드는 지원하지 않습니다. 해당 OS에서 실행하거나 "
            "--no-download로 미리 받은 wheelhouse를 사용하세요."
        )

    package_set = packages_for(args.package_set, args.package)

    if args.clean:
        clean_directory(args.destination_dir)
        if not args.no_download:
            clean_directory(args.wheelhouse)

    if not args.no_download:
        download_wheels(
            args.python,
            args.wheelhouse,
            package_set,
            args.extra_index_url,
            retry_count=args.download_retries,
            retry_delay_seconds=args.download_retry_delay,
            timeout_seconds=args.pip_timeout,
        )

    extracted = extract_cuda_libraries(args.wheelhouse, args.destination_dir, platform)
    for path in extracted:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
