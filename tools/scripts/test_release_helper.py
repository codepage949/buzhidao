import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.scripts.release_helper import (
    DEFAULT_MAX_PART_BYTES,
    archive_basename,
    copy_runtime_libraries,
    create_archive,
    is_excluded_runtime_library,
    is_runtime_library,
    make_install_script,
    prepare_app_layout,
    split_archive,
)


class ReleaseHelperTest(unittest.TestCase):
    def test_아카이브_이름은_플랫폼과_플레이버를_포함한다(self):
        name = archive_basename("v1.2.3", "windows", "amd64", "gpu", "app")
        self.assertEqual(name, "buzhidao-v1.2.3-windows-amd64-gpu-app")

    def test_레이아웃_준비는_앱을_복사한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            app_binary = root / "buzhidao.exe"
            app_output_dir = root / "app-out"

            app_binary.write_bytes(b"app")

            prepare_app_layout(app_binary, app_output_dir)

            self.assertTrue((app_output_dir / "buzhidao.exe").exists())
            self.assertEqual(
                sorted(path.name for path in app_output_dir.iterdir()),
                ["buzhidao.exe"],
            )

    def test_레이아웃_준비는_런타임_라이브러리를_함께_복사한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            app_binary = root / "buzhidao.exe"
            output_dir = root / "app-out"
            app_binary.write_bytes(b"app")
            (root / "paddle_inference.dll").write_bytes(b"dll")
            (root / "libpaddle_inference.so.3").write_bytes(b"so")
            (root / "note.txt").write_text("ignore", encoding="utf-8")

            prepare_app_layout(app_binary, output_dir)

            self.assertEqual(
                sorted(path.name for path in output_dir.iterdir()),
                ["buzhidao.exe", "libpaddle_inference.so.3", "paddle_inference.dll"],
            )

    def test_runtime_library_확장자를_판별한다(self):
        self.assertTrue(is_runtime_library(Path("a.dll")))
        self.assertTrue(is_runtime_library(Path("liba.so")))
        self.assertTrue(is_runtime_library(Path("liba.so.1")))
        self.assertTrue(is_runtime_library(Path("liba.dylib")))
        self.assertFalse(is_runtime_library(Path("a.txt")))

    def test_opencv_debug와_plugin_runtime은_제외한다(self):
        self.assertTrue(is_excluded_runtime_library(Path("opencv_world4100d.dll")))
        self.assertTrue(is_excluded_runtime_library(Path("opencv_videoio_msmf4100_64d.dll")))
        self.assertTrue(is_excluded_runtime_library(Path("opencv_videoio_ffmpeg4100_64.dll")))
        self.assertTrue(is_excluded_runtime_library(Path("opencv_java4100.dll")))
        self.assertTrue(is_excluded_runtime_library(Path("libopencv_videoio.so.410")))
        self.assertTrue(is_excluded_runtime_library(Path("libopencv_java4100.so")))
        self.assertFalse(is_excluded_runtime_library(Path("opencv_world4100.dll")))
        self.assertFalse(is_excluded_runtime_library(Path("libopencv_core.so.410")))
        self.assertFalse(is_excluded_runtime_library(Path("paddle_inference.dll")))

    def test_runtime_library만_복사한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            output = root / "output"
            source.mkdir()
            output.mkdir()
            (source / "a.dll").write_bytes(b"dll")
            (source / "a.txt").write_text("ignore", encoding="utf-8")

            copied = copy_runtime_libraries(source, output)

            self.assertEqual([path.name for path in copied], ["a.dll"])
            self.assertTrue((output / "a.dll").exists())
            self.assertFalse((output / "a.txt").exists())

    def test_runtime_복사는_opencv_debug와_plugin을_제외한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            output = root / "output"
            source.mkdir()
            output.mkdir()
            for name in [
                "opencv_world4100.dll",
                "opencv_world4100d.dll",
                "opencv_videoio_ffmpeg4100_64.dll",
                "libopencv_videoio.so.410",
                "opencv_java4100.dll",
                "paddle_inference.dll",
            ]:
                (source / name).write_bytes(name.encode("utf-8"))

            copied = copy_runtime_libraries(source, output)

            self.assertEqual(
                sorted(path.name for path in copied),
                ["opencv_world4100.dll", "paddle_inference.dll"],
            )
            self.assertFalse((output / "opencv_world4100d.dll").exists())
            self.assertFalse((output / "opencv_videoio_ffmpeg4100_64.dll").exists())
            self.assertFalse((output / "libopencv_videoio.so.410").exists())
            self.assertFalse((output / "opencv_java4100.dll").exists())

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
        self.assertNotIn("ocr-sidecar-compare", content)

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
        self.assertNotIn("ocr-sidecar-compare", content)

    def test_지원하지_않는_os는_ValueError를_발생시킨다(self):
        with self.assertRaises(ValueError):
            make_install_script("macos", "amd64", "cpu", "v1.0.0")


if __name__ == "__main__":
    unittest.main()
