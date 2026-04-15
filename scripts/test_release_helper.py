import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.release_helper import (
    DEFAULT_MAX_PART_BYTES,
    archive_basename,
    create_archive,
    make_install_script,
    prepare_app_layout,
    prepare_ocr_server_layout,
    split_archive,
)


class ReleaseHelperTest(unittest.TestCase):
    def test_아카이브_이름은_플랫폼과_플레이버를_포함한다(self):
        name = archive_basename("v1.2.3", "windows", "amd64", "gpu", "ocr-server")
        self.assertEqual(name, "buzhidao-v1.2.3-windows-amd64-gpu-ocr-server")

    def test_레이아웃_준비는_앱과_ocr_server를_복사한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            app_binary = root / "buzhidao.exe"
            ocr_server_dir = root / "dist" / "ocr_server"
            app_output_dir = root / "app-out"
            ocr_output_dir = root / "ocr-out"

            app_binary.write_bytes(b"app")
            (ocr_server_dir / "_internal").mkdir(parents=True)
            (ocr_server_dir / "ocr_server.exe").write_bytes(b"ocr")
            (ocr_server_dir / "_internal" / "data.txt").write_text("x", encoding="utf-8")

            prepare_app_layout(app_binary, app_output_dir)
            prepare_ocr_server_layout(ocr_server_dir, ocr_output_dir)

            self.assertTrue((app_output_dir / "buzhidao.exe").exists())
            self.assertFalse((app_output_dir / "ocr_server").exists())
            self.assertTrue((ocr_output_dir / "ocr_server" / "ocr_server.exe").exists())
            self.assertTrue((ocr_output_dir / "ocr_server" / "_internal" / "data.txt").exists())

    def test_zip_아카이브를_생성한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source_dir = root / "src"
            source_dir.mkdir()
            (source_dir / "file.txt").write_text("hello", encoding="utf-8")
            archive_path = root / "out.zip"

            create_archive(source_dir, archive_path)

            with zipfile.ZipFile(archive_path) as zf:
                self.assertEqual(zf.namelist(), ["file.txt"])

    def test_tar_gz_아카이브를_생성한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source_dir = root / "src"
            nested = source_dir / "dir"
            nested.mkdir(parents=True)
            (nested / "file.txt").write_text("hello", encoding="utf-8")
            archive_path = root / "out.tar.gz"

            create_archive(source_dir, archive_path)

            with tarfile.open(archive_path, "r:gz") as tf:
                names = sorted(member.name for member in tf.getmembers())
                self.assertIn("./dir/file.txt", names)

    def test_작은_아카이브는_분할하지_않는다(self):
        with tempfile.TemporaryDirectory() as td:
            archive_path = Path(td) / "out.zip"
            archive_path.write_bytes(b"hello")

            parts = split_archive(archive_path, DEFAULT_MAX_PART_BYTES)

            self.assertEqual(parts, [archive_path])
            self.assertTrue(archive_path.exists())

    def test_큰_아카이브는_part_파일로_자동_분할한다(self):
        with tempfile.TemporaryDirectory() as td:
            archive_path = Path(td) / "out.tar.gz"
            original = b"abcdefghij"
            archive_path.write_bytes(original)

            parts = split_archive(archive_path, 4)

            self.assertFalse(archive_path.exists())
            self.assertEqual(
                [part.name for part in parts],
                ["out.tar.gz.part001", "out.tar.gz.part002", "out.tar.gz.part003"],
            )
            rebuilt = b"".join(part.read_bytes() for part in parts)
            self.assertEqual(rebuilt, original)


class InstallScriptTest(unittest.TestCase):
    def test_windows_스크립트_파일명은_ps1이다(self):
        filename, _ = make_install_script("windows", "amd64", "gpu", "v1.0.0")
        self.assertEqual(filename, "install-windows-amd64-gpu.ps1")

    def test_windows_스크립트는_올바른_아카이브명을_포함한다(self):
        _, content = make_install_script("windows", "amd64", "cpu", "v1.0.0")
        self.assertIn("buzhidao-v1.0.0-windows-amd64-cpu-app.zip", content)
        self.assertIn("buzhidao-v1.0.0-windows-amd64-cpu-ocr-server.zip", content)

    def test_windows_스크립트는_Get_Location을_사용한다(self):
        _, content = make_install_script("windows", "amd64", "cpu", "v1.0.0")
        self.assertIn("(Get-Location).Path", content)

    def test_windows_스크립트는_상대경로로_Create를_호출하지_않는다(self):
        _, content = make_install_script("windows", "amd64", "cpu", "v1.0.0")
        # .NET 메서드에 직접 상대 경로 문자열을 넘기면 PowerShell $PWD가 무시된다
        self.assertNotIn('[System.IO.File]::Create(".\\', content)
        self.assertNotIn("[System.IO.File]::Create('.\\", content)

    def test_linux_스크립트_파일명은_sh이다(self):
        filename, _ = make_install_script("linux", "amd64", "cpu", "v1.0.0")
        self.assertEqual(filename, "install-linux-amd64-cpu.sh")

    def test_linux_스크립트는_shebang으로_시작한다(self):
        _, content = make_install_script("linux", "amd64", "cpu", "v1.0.0")
        self.assertTrue(content.startswith("#!/usr/bin/env bash"))

    def test_linux_스크립트는_올바른_아카이브명을_포함한다(self):
        _, content = make_install_script("linux", "amd64", "gpu", "v1.0.0")
        self.assertIn("buzhidao-v1.0.0-linux-amd64-gpu-app.tar.gz", content)
        self.assertIn("buzhidao-v1.0.0-linux-amd64-gpu-ocr-server.tar.gz", content)

    def test_지원하지_않는_os는_ValueError를_발생시킨다(self):
        with self.assertRaises(ValueError):
            make_install_script("macos", "amd64", "cpu", "v1.0.0")


if __name__ == "__main__":
    unittest.main()
