from __future__ import annotations

import argparse
import shutil
import tarfile
import zipfile
from pathlib import Path

DEFAULT_MAX_PART_BYTES = 1_900 * 1024 * 1024
RUNTIME_LIBRARY_SUFFIXES = (".dll", ".dylib", ".so")


def archive_basename(
    version: str, os_name: str, arch: str, flavor: str, component: str
) -> str:
    return f"buzhidao-{version}-{os_name}-{arch}-{flavor}-{component}"


def prepare_app_layout(app_binary: Path, output_dir: Path) -> None:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(app_binary, output_dir / app_binary.name)
    copy_runtime_libraries(app_binary.parent, output_dir)


def is_runtime_library(path: Path) -> bool:
    name = path.name.lower()
    return name.endswith(RUNTIME_LIBRARY_SUFFIXES) or ".so." in name


def is_excluded_runtime_library(path: Path) -> bool:
    name = path.name.lower()
    if not (name.startswith("opencv_") or name.startswith("libopencv_")):
        return False
    stem = name.removesuffix(".dll")
    if name.endswith(".dll") and (stem.endswith("d") or stem.endswith("_64d")):
        return True
    if name.startswith(("opencv_java", "libopencv_java")):
        return True
    if name.startswith(("opencv_videoio_", "libopencv_videoio")):
        return True
    return False


def copy_runtime_libraries(source_dir: Path, output_dir: Path) -> list[Path]:
    copied: list[Path] = []
    if not source_dir.is_dir():
        return copied

    for path in sorted(source_dir.iterdir()):
        if not path.is_file() or not is_runtime_library(path) or is_excluded_runtime_library(path):
            continue
        target = output_dir / path.name
        if target.exists():
            continue
        shutil.copy2(path, target)
        copied.append(target)
    return copied


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


def make_install_script(os_name: str, arch: str, flavor: str, version: str) -> tuple[str, str]:
    """(파일명, 스크립트 내용) 튜플을 반환한다."""
    if os_name == "windows":
        return (
            f"install-windows-{arch}-{flavor}.ps1",
            _windows_install_script(version, arch, flavor),
        )
    if os_name == "linux":
        return (
            f"install-linux-{arch}-{flavor}.sh",
            _linux_install_script(version, arch, flavor),
        )
    raise ValueError(f"지원하지 않는 OS: {os_name}")


def _windows_install_script(version: str, arch: str, flavor: str) -> str:
    app_archive = f"buzhidao-{version}-windows-{arch}-{flavor}-app.zip"
    script_name = f"install-windows-{arch}-{flavor}.ps1"
    lines = [
        f"# buzhidao {version} windows-{arch}-{flavor} install script",
        "# Run this script in PowerShell from the directory containing the archives.",
        f"#   .\\{script_name}",
        "",
        "$archives = @(",
        f'    "{app_archive}"',
        ")",
        "$pwd_path = (Get-Location).Path",
        "",
        "# Merge split parts if present",
        "foreach ($archive in $archives) {",
        '    $parts = Get-ChildItem -Path $pwd_path -Filter "$archive.part*" |',
        "             Sort-Object Name",
        "    if ($parts.Count -gt 0) {",
        "        $outPath = Join-Path $pwd_path $archive",
        "        $out = [System.IO.File]::Create($outPath)",
        "        foreach ($part in $parts) {",
        "            $bytes = [System.IO.File]::ReadAllBytes($part.FullName)",
        "            $out.Write($bytes, 0, $bytes.Length)",
        "        }",
        "        $out.Close()",
        '        Write-Host "Merged: $archive"',
        "    }",
        "}",
        "",
        "# Extract archives to current directory",
        "foreach ($archive in $archives) {",
        "    Expand-Archive -Path (Join-Path $pwd_path $archive) -DestinationPath $pwd_path -Force",
        '    Write-Host "Extracted: $archive"',
        "}",
        'Write-Host "Done: $pwd_path"',
        "",
    ]
    return "\n".join(lines)


def _linux_install_script(version: str, arch: str, flavor: str) -> str:
    app_archive = f"buzhidao-{version}-linux-{arch}-{flavor}-app.tar.gz"
    script_name = f"install-linux-{arch}-{flavor}.sh"
    lines = [
        "#!/usr/bin/env bash",
        f"# buzhidao {version} linux-{arch}-{flavor} install script",
        "# Run this script from the directory containing the archives.",
        f"#   bash {script_name}",
        "set -euo pipefail",
        "",
        "# Merge split parts if present",
        f'for archive in "{app_archive}"; do',
        '    if [ -f "${archive}.part001" ]; then',
        '        cat "${archive}".part* > "$archive"',
        '        echo "Merged: $archive"',
        "    fi",
        "done",
        "",
        "# Extract archives to current directory",
        f'tar xzf "{app_archive}"',
        'echo "Done: $(pwd)"',
        "",
    ]
    return "\n".join(lines)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    make_archive = sub.add_parser("make-archive")
    make_archive.add_argument("--version", required=True)
    make_archive.add_argument("--os", dest="os_name", required=True)
    make_archive.add_argument("--arch", required=True)
    make_archive.add_argument("--flavor", required=True)
    make_archive.add_argument("--app-binary", required=True)
    make_archive.add_argument("--dist-dir", required=True)
    make_archive.add_argument("--format", choices=["zip", "tar.gz"], required=True)
    make_archive.add_argument("--component", choices=["app"], required=True)
    make_archive.add_argument(
        "--max-part-bytes", type=int, default=DEFAULT_MAX_PART_BYTES
    )

    make_script = sub.add_parser("make-install-script")
    make_script.add_argument("--version", required=True)
    make_script.add_argument("--os", dest="os_name", required=True)
    make_script.add_argument("--arch", required=True)
    make_script.add_argument("--flavor", required=True)

    return parser.parse_args()


def main() -> int:
    args = parse_args()

    if args.command == "make-install-script":
        filename, content = make_install_script(args.os_name, args.arch, args.flavor, args.version)
        Path(filename).write_text(content, encoding="utf-8")
        print(filename)
        return 0

    if args.command != "make-archive":
        raise ValueError(f"지원하지 않는 명령: {args.command}")

    dist_dir = Path(args.dist_dir)
    layout_dir = (
        dist_dir / f"{args.os_name}-{args.arch}-{args.flavor}-{args.component}"
    )

    prepare_app_layout(Path(args.app_binary), layout_dir)

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
