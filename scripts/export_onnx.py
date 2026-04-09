"""Docker 컨테이너에서 PaddleOCR 추론 모델을 ONNX로 변환하는 호스트 진입점.

권장 실행 방법:
    python scripts/export_onnx.py

요구 사항:
    - Docker 설치 및 실행 중

산출물:
    app/models/det.onnx
    app/models/cls.onnx
    app/models/rec.onnx
    app/models/rec_dict.txt
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

PROJ_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS_DIR = PROJ_ROOT / "scripts"
APP_MODELS = PROJ_ROOT / "app" / "models"

DEFAULT_IMAGE = "python:3.11-slim"
CONTAINER_SCRIPTS_DIR = "/workspace/scripts"
CONTAINER_OUT_DIR = "/workspace/out"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Docker 컨테이너에서 PaddleOCR 추론 모델을 ONNX로 변환합니다."
    )
    parser.add_argument(
        "--docker-bin",
        default="docker",
        help="Docker 실행 파일 경로 또는 이름 (기본값: docker)",
    )
    parser.add_argument(
        "--image",
        default=DEFAULT_IMAGE,
        help=f"사용할 Docker 이미지 (기본값: {DEFAULT_IMAGE})",
    )
    parser.add_argument(
        "--print-only",
        action="store_true",
        help="실행하지 않고 docker 명령만 출력합니다.",
    )
    return parser.parse_args()


def print_docker_install_guide() -> None:
    print("Docker를 찾을 수 없습니다.")
    print("Docker Desktop 또는 Docker Engine을 설치한 뒤 다시 실행하세요.")
    print("  https://docs.docker.com/get-docker/")


def build_docker_command(args: argparse.Namespace) -> list[str]:
    scripts_mount = f"{SCRIPTS_DIR.resolve()}:{CONTAINER_SCRIPTS_DIR}"
    models_mount = f"{APP_MODELS.resolve()}:{CONTAINER_OUT_DIR}"
    inner_script = (
        "apt-get update && "
        "apt-get install -y --no-install-recommends libgomp1 && "
        "rm -rf /var/lib/apt/lists/* && "
        "python -m pip install --no-cache-dir --upgrade pip && "
        "python -m pip install --no-cache-dir paddlepaddle paddle2onnx packaging setuptools && "
        f"python {CONTAINER_SCRIPTS_DIR}/export_onnx_docker.py"
    )
    return [
        args.docker_bin,
        "run",
        "--rm",
        "-v",
        scripts_mount,
        "-v",
        models_mount,
        args.image,
        "sh",
        "-lc",
        inner_script,
    ]


def main() -> int:
    args = parse_args()
    docker_bin = shutil.which(args.docker_bin) if Path(args.docker_bin).name == args.docker_bin else args.docker_bin
    if docker_bin is None:
        print_docker_install_guide()
        return 1

    APP_MODELS.mkdir(parents=True, exist_ok=True)
    args.docker_bin = docker_bin
    cmd = build_docker_command(args)

    print("실행 명령:")
    print(" ".join(cmd))
    if args.print_only:
        return 0

    try:
        subprocess.check_call(cmd, cwd=PROJ_ROOT)
    except subprocess.CalledProcessError as exc:
        print("")
        print("Docker 기반 모델 변환에 실패했습니다.")
        return exc.returncode or 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
