from __future__ import annotations

import argparse
import json
import re
import shutil
import tempfile
import zipfile
from pathlib import Path


REQUIRED_MODEL_FILES = ("det.onnx", "cls.onnx", "rec.onnx", "rec_dict.txt")
REQUIRED_CUDA_DLLS = (
    "cudart64_12.dll",
    "cublas64_12.dll",
    "cublasLt64_12.dll",
    "cufft64_11.dll",
    "cudnn64_9.dll",
    "cudnn_ops64_9.dll",
    "cudnn_cnn64_9.dll",
    "cudnn_heuristic64_9.dll",
    "cudnn_adv64_9.dll",
    "cudnn_graph64_9.dll",
    "cudnn_engines_precompiled64_9.dll",
    "cudnn_engines_runtime_compiled64_9.dll",
)


def normalize_release_version(version: str) -> str:
    normalized = version.strip()
    if normalized.startswith("v"):
        normalized = normalized[1:]
    if not re.fullmatch(r"\d+\.\d+\.\d+", normalized):
        raise ValueError(f"지원하지 않는 버전 형식: {version}")
    return normalized


def update_cargo_version_text(text: str, version: str) -> str:
    lines = text.splitlines()
    in_package = False
    replaced = False
    updated: list[str] = []

    for line in lines:
        stripped = line.strip()
        if stripped.startswith("[") and stripped.endswith("]"):
            in_package = stripped == "[package]"
        if in_package and stripped.startswith("version = "):
            updated.append(f'version = "{version}"')
            replaced = True
            in_package = False
            continue
        updated.append(line)

    if not replaced:
        raise ValueError("[package] 섹션의 version 필드를 찾을 수 없습니다")

    return "\n".join(updated) + "\n"


def update_versions(repo_root: Path, version: str) -> None:
    cargo_toml = repo_root / "Cargo.toml"
    cargo_text = cargo_toml.read_text(encoding="utf-8")
    cargo_toml.write_text(update_cargo_version_text(cargo_text, version), encoding="utf-8")

    tauri_conf = repo_root / "tauri.conf.json"
    tauri_data = json.loads(tauri_conf.read_text(encoding="utf-8"))
    tauri_data["version"] = version
    tauri_conf.write_text(
        json.dumps(tauri_data, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def ensure_models_dir(models_dir: Path) -> None:
    missing = [name for name in REQUIRED_MODEL_FILES if not (models_dir / name).exists()]
    if missing:
        joined = ", ".join(missing)
        raise FileNotFoundError(f"필수 모델 파일 누락: {joined}")


def extract_cuda_dlls(wheel_dir: Path, output_dir: Path) -> None:
    wheels = sorted(wheel_dir.glob("*.whl"))
    if not wheels:
        raise FileNotFoundError(f"wheel 파일을 찾을 수 없습니다: {wheel_dir}")

    output_dir.mkdir(parents=True, exist_ok=True)
    wanted = set(REQUIRED_CUDA_DLLS)
    found: set[str] = set()

    for wheel_path in wheels:
        with zipfile.ZipFile(wheel_path) as zf:
            for member in zf.namelist():
                name = Path(member).name
                if name not in wanted or name in found:
                    continue
                with zf.open(member) as src, (output_dir / name).open("wb") as dst:
                    shutil.copyfileobj(src, dst)
                found.add(name)

    missing = sorted(wanted - found)
    if missing:
        joined = ", ".join(missing)
        raise FileNotFoundError(f"필수 CUDA/cuDNN DLL 추출 실패: {joined}")


def make_archive(
    repo_root: Path,
    version_tag: str,
    mode: str,
    exe_path: Path,
    output_dir: Path,
) -> Path:
    if mode not in {"cpu", "gpu"}:
        raise ValueError(f"지원하지 않는 빌드 모드: {mode}")
    if not exe_path.exists():
        raise FileNotFoundError(f"실행 파일을 찾을 수 없습니다: {exe_path}")

    models_dir = repo_root / "models"
    ensure_models_dir(models_dir)

    cuda_dir = repo_root / "cuda"
    if mode == "gpu" and not cuda_dir.exists():
        raise FileNotFoundError(f"GPU 아카이브용 cuda 디렉토리를 찾을 수 없습니다: {cuda_dir}")

    output_dir.mkdir(parents=True, exist_ok=True)
    archive_name = f"buzhidao-{version_tag}-windows-x64-{mode}.zip"
    package_root_name = archive_name.removesuffix(".zip")

    with tempfile.TemporaryDirectory(prefix="buzhidao-release-") as temp_dir:
        staging_root = Path(temp_dir) / package_root_name
        staging_root.mkdir(parents=True, exist_ok=True)

        shutil.copy2(exe_path, staging_root / "buzhidao.exe")
        shutil.copytree(models_dir, staging_root / "models")
        shutil.copy2(repo_root / "README.md", staging_root / "README.md")
        shutil.copy2(repo_root / ".env.example", staging_root / ".env.example")

        if mode == "gpu":
            shutil.copytree(cuda_dir, staging_root / "cuda")

        archive_path = output_dir / archive_name
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(staging_root.rglob("*")):
                if path.is_file():
                    zf.write(path, path.relative_to(staging_root.parent))

    return archive_path


def main() -> int:
    parser = argparse.ArgumentParser(description="릴리스 보조 스크립트")
    subparsers = parser.add_subparsers(dest="command", required=True)

    set_version_parser = subparsers.add_parser("set-version", help="Cargo/Tauri 버전 동기화")
    set_version_parser.add_argument("version", help="릴리스 버전 태그 (예: v1.2.3)")
    set_version_parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="저장소 루트 경로",
    )

    archive_parser = subparsers.add_parser("make-archive", help="Windows 배포 zip 생성")
    archive_parser.add_argument("--version", required=True, help="릴리스 버전 태그 (예: v1.2.3)")
    archive_parser.add_argument("--mode", required=True, choices=["cpu", "gpu"])
    archive_parser.add_argument("--exe", required=True, type=Path, help="빌드된 실행 파일 경로")
    archive_parser.add_argument(
        "--repo-root",
        type=Path,
        default=Path(__file__).resolve().parent.parent,
        help="저장소 루트 경로",
    )
    archive_parser.add_argument("--output-dir", required=True, type=Path)
    extract_cuda_parser = subparsers.add_parser("extract-cuda", help="wheel에서 CUDA DLL 추출")
    extract_cuda_parser.add_argument("--wheel-dir", required=True, type=Path)
    extract_cuda_parser.add_argument("--output-dir", required=True, type=Path)

    args = parser.parse_args()

    if args.command == "set-version":
        normalized = normalize_release_version(args.version)
        update_versions(args.repo_root, normalized)
        print(normalized)
        return 0

    if args.command == "make-archive":
        archive_path = make_archive(
            repo_root=args.repo_root,
            version_tag=args.version,
            mode=args.mode,
            exe_path=args.exe,
            output_dir=args.output_dir,
        )
        print(archive_path)
        return 0

    if args.command == "extract-cuda":
        extract_cuda_dlls(args.wheel_dir, args.output_dir)
        print(args.output_dir)
        return 0

    raise ValueError(f"지원하지 않는 명령: {args.command}")


if __name__ == "__main__":
    raise SystemExit(main())
