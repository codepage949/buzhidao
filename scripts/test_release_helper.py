import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from scripts.release_helper import (
    archive_basename,
    create_archive,
    prepare_app_layout,
    prepare_ocr_server_layout,
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


if __name__ == "__main__":
    unittest.main()
