from __future__ import annotations

import argparse
import json
import re
from pathlib import Path


SEMVER_RE = re.compile(r"^v?(\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?)$")


def normalize_version(raw: str) -> str:
    match = SEMVER_RE.match(raw.strip())
    if not match:
        raise ValueError(f"지원하지 않는 버전 형식입니다: {raw}")
    return match.group(1)


def replace_package_version_toml(content: str, version: str) -> str:
    lines = content.splitlines(keepends=True)
    in_package = False
    replaced = False
    output: list[str] = []

    for line in lines:
        stripped = line.strip()
        if stripped == "[package]":
            in_package = True
            output.append(line)
            continue
        if stripped.startswith("[") and stripped.endswith("]"):
            in_package = False

        if in_package and stripped.startswith("version"):
            newline = "\r\n" if line.endswith("\r\n") else "\n" if line.endswith("\n") else ""
            output.append(f'version = "{version}"{newline}')
            replaced = True
            continue

        output.append(line)

    if not replaced:
        raise ValueError("Cargo.toml [package] version 항목을 찾지 못했습니다.")
    return "".join(output)


def replace_lock_package_version(content: str, package_name: str, version: str) -> str:
    lines = content.splitlines(keepends=True)
    in_package = False
    matched_package = False
    replaced = False
    output: list[str] = []

    for line in lines:
        stripped = line.strip()
        if stripped == "[[package]]":
            in_package = True
            matched_package = False
            output.append(line)
            continue

        if in_package and stripped.startswith("name = "):
            matched_package = stripped == f'name = "{package_name}"'

        if in_package and matched_package and stripped.startswith("version = "):
            newline = "\r\n" if line.endswith("\r\n") else "\n" if line.endswith("\n") else ""
            output.append(f'version = "{version}"{newline}')
            replaced = True
            matched_package = False
            continue

        output.append(line)

    if not replaced:
        raise ValueError(f"Cargo.lock package version 항목을 찾지 못했습니다: {package_name}")
    return "".join(output)


def update_tauri_conf(path: Path, version: str) -> None:
    payload = json.loads(path.read_text(encoding="utf-8"))
    payload["version"] = version
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def update_release_version(repo_root: Path, raw_version: str) -> str:
    version = normalize_version(raw_version)

    cargo_toml = repo_root / "Cargo.toml"
    cargo_lock = repo_root / "Cargo.lock"
    tauri_conf = repo_root / "tauri.conf.json"

    cargo_toml.write_text(
        replace_package_version_toml(cargo_toml.read_text(encoding="utf-8"), version),
        encoding="utf-8",
    )
    cargo_lock.write_text(
        replace_lock_package_version(cargo_lock.read_text(encoding="utf-8"), "buzhidao", version),
        encoding="utf-8",
    )
    update_tauri_conf(tauri_conf, version)
    return version


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="릴리즈 입력 버전을 Cargo/Tauri 메타데이터에 반영합니다."
    )
    parser.add_argument("version", help="릴리즈 버전. 예: v0.2.0 또는 0.2.0")
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    version = update_release_version(args.repo_root, args.version)
    print(version)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
