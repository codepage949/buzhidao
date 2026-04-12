from __future__ import annotations

import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.release_helper import (
    REQUIRED_CUDA_DLLS,
    extract_cuda_dlls,
    make_archive,
    normalize_release_version,
    update_cargo_version_text,
    update_versions,
)


class ReleaseHelperTest(unittest.TestCase):
    def test_v_접두사가_있어도_버전을_정규화한다(self) -> None:
        self.assertEqual(normalize_release_version("v1.2.3"), "1.2.3")

    def test_지원하지_않는_버전_형식은_거부한다(self) -> None:
        with self.assertRaises(ValueError):
            normalize_release_version("1.2")

    def test_cargo_package_버전만_교체한다(self) -> None:
        cargo_text = """[package]
name = "buzhidao"
version = "0.1.0"

[dependencies]
tauri = { version = "2", features = ["tray-icon"] }
"""
        updated = update_cargo_version_text(cargo_text, "1.2.3")

        self.assertIn('version = "1.2.3"', updated)
        self.assertIn('tauri = { version = "2", features = ["tray-icon"] }', updated)

    def test_set_version은_cargo와_tauri버전을_함께_갱신한다(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "Cargo.toml").write_text(
                '[package]\nname = "buzhidao"\nversion = "0.1.0"\n',
                encoding="utf-8",
            )
            (root / "tauri.conf.json").write_text(
                json.dumps({"productName": "buzhidao", "version": "0.1.0"}, ensure_ascii=False),
                encoding="utf-8",
            )

            update_versions(root, "1.2.3")

            self.assertIn('version = "1.2.3"', (root / "Cargo.toml").read_text(encoding="utf-8"))
            tauri_data = json.loads((root / "tauri.conf.json").read_text(encoding="utf-8"))
            self.assertEqual(tauri_data["version"], "1.2.3")

    def test_gpu_아카이브는_cuda를_포함한다(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "models").mkdir()
            for name in ("det.onnx", "cls.onnx", "rec.onnx", "rec_dict.txt"):
                (root / "models" / name).write_text("x", encoding="utf-8")
            (root / "cuda").mkdir()
            (root / "cuda" / "cudart64_12.dll").write_text("dll", encoding="utf-8")
            (root / "README.md").write_text("# test\n", encoding="utf-8")
            (root / ".env.example").write_text("AI_GATEWAY_API_KEY=\n", encoding="utf-8")
            exe_path = root / "buzhidao.exe"
            exe_path.write_text("exe", encoding="utf-8")

            archive_path = make_archive(root, "v1.2.3", "gpu", exe_path, root / "dist")

            with zipfile.ZipFile(archive_path) as zf:
                names = set(zf.namelist())

            self.assertIn("buzhidao-v1.2.3-windows-x64-gpu/buzhidao.exe", names)
            self.assertIn("buzhidao-v1.2.3-windows-x64-gpu/cuda/cudart64_12.dll", names)

    def test_extract_cuda는_wheel에서_필수_dll을_모은다(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            wheel_dir = root / "wheelhouse"
            wheel_dir.mkdir()
            output_dir = root / "cuda"

            first_half = REQUIRED_CUDA_DLLS[: len(REQUIRED_CUDA_DLLS) // 2]
            second_half = REQUIRED_CUDA_DLLS[len(REQUIRED_CUDA_DLLS) // 2 :]

            for index, members in enumerate((first_half, second_half), start=1):
                wheel_path = wheel_dir / f"part{index}.whl"
                with zipfile.ZipFile(wheel_path, "w") as zf:
                    for dll_name in members:
                        zf.writestr(f"nvidia/bin/{dll_name}", b"dll")

            extract_cuda_dlls(wheel_dir, output_dir)

            extracted = {path.name for path in output_dir.glob("*.dll")}
            self.assertEqual(extracted, set(REQUIRED_CUDA_DLLS))

    def test_cpu_아카이브는_cuda를_제외한다(self) -> None:
        with tempfile.TemporaryDirectory() as temp_dir:
            root = Path(temp_dir)
            (root / "models").mkdir()
            for name in ("det.onnx", "cls.onnx", "rec.onnx", "rec_dict.txt"):
                (root / "models" / name).write_text("x", encoding="utf-8")
            (root / "README.md").write_text("# test\n", encoding="utf-8")
            (root / ".env.example").write_text("AI_GATEWAY_API_KEY=\n", encoding="utf-8")
            exe_path = root / "buzhidao.exe"
            exe_path.write_text("exe", encoding="utf-8")

            archive_path = make_archive(root, "v1.2.3", "cpu", exe_path, root / "dist")

            with zipfile.ZipFile(archive_path) as zf:
                names = set(zf.namelist())

            self.assertIn("buzhidao-v1.2.3-windows-x64-cpu/models/det.onnx", names)
            self.assertFalse(any("/cuda/" in name for name in names))


if __name__ == "__main__":
    unittest.main()
