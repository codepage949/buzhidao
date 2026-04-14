from __future__ import annotations

import argparse
import shutil
import tarfile
import zipfile
from pathlib import Path

DEFAULT_MAX_PART_BYTES = 1_900 * 1024 * 1024


def archive_basename(
    version: str, os_name: str, arch: str, flavor: str, component: str
) -> str:
    return f"buzhidao-{version}-{os_name}-{arch}-{flavor}-{component}"


def prepare_app_layout(app_binary: Path, output_dir: Path) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(app_binary, output_dir / app_binary.name)


def prepare_ocr_server_layout(ocr_server_dir: Path, output_dir: Path) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    shutil.copytree(ocr_server_dir, output_dir / "ocr_server")


def create_archive(source_dir: Path, archive_path: Path) -> None:
    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(source_dir.rglob("*")):
                if path.is_file():
                    zf.write(path, path.relative_to(source_dir))
        return

    suffixes = archive_path.suffixes
    if len(suffixes) >= 2 and suffixes[-2:] == [".tar", ".gz"]:
        with tarfile.open(archive_path, "w:gz") as tf:
            tf.add(source_dir, arcname=".")
        return

    raise ValueError(f"지원하지 않는 아카이브 형식: {archive_path}")


def split_archive(archive_path: Path, max_part_bytes: int) -> list[Path]:
    if max_part_bytes <= 0:
        raise ValueError("max_part_bytes는 0보다 커야 합니다.")

    if archive_path.stat().st_size <= max_part_bytes:
        return [archive_path]

    parts: list[Path] = []
    with archive_path.open("rb") as source:
        index = 1
        while True:
            chunk = source.read(max_part_bytes)
            if not chunk:
                break

            part_path = archive_path.with_name(f"{archive_path.name}.part{index:03d}")
            part_path.write_bytes(chunk)
            parts.append(part_path)
            index += 1

    archive_path.unlink()
    return parts


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    make_archive = sub.add_parser("make-archive")
    make_archive.add_argument("--version", required=True)
    make_archive.add_argument("--os", dest="os_name", required=True)
    make_archive.add_argument("--arch", required=True)
    make_archive.add_argument("--flavor", required=True)
    make_archive.add_argument("--app-binary", required=True)
    make_archive.add_argument("--ocr-server-dir", required=True)
    make_archive.add_argument("--dist-dir", required=True)
    make_archive.add_argument("--format", choices=["zip", "tar.gz"], required=True)
    make_archive.add_argument(
        "--component", choices=["app", "ocr-server"], required=True
    )
    make_archive.add_argument(
        "--max-part-bytes", type=int, default=DEFAULT_MAX_PART_BYTES
    )

    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.command != "make-archive":
        raise ValueError(f"지원하지 않는 명령: {args.command}")

    dist_dir = Path(args.dist_dir)
    layout_dir = (
        dist_dir / f"{args.os_name}-{args.arch}-{args.flavor}-{args.component}"
    )

    if args.component == "app":
        prepare_app_layout(Path(args.app_binary), layout_dir)
    else:
        prepare_ocr_server_layout(Path(args.ocr_server_dir), layout_dir)

    stem = archive_basename(
        args.version, args.os_name, args.arch, args.flavor, args.component
    )
    extension = ".zip" if args.format == "zip" else ".tar.gz"
    archive_path = dist_dir / f"{stem}{extension}"
    create_archive(layout_dir, archive_path)
    for path in split_archive(archive_path, args.max_part_bytes):
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
