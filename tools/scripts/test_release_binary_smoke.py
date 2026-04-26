import os
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.scripts.release_binary_smoke import (
    SMOKE_ARG,
    binary_name_for_os,
    extract_archive,
    find_release_binary,
    run_archive_smoke,
    run_extracted_binary_smoke,
    smoke_env,
)


def write_shell_binary(path: Path, body: str) -> None:
    path.write_text("#!/usr/bin/env sh\n" + body, encoding="utf-8")
    path.chmod(0o755)


class ReleaseBinarySmokeTest(unittest.TestCase):
    def test_os별_실행파일명을_결정한다(self):
        self.assertEqual(binary_name_for_os("linux"), "buzhidao")
        self.assertEqual(binary_name_for_os("windows"), "buzhidao.exe")

    def test_지원하지_않는_os는_실패한다(self):
        with self.assertRaises(ValueError):
            binary_name_for_os("macos")

    def test_zip_아카이브를_압축해제한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            archive = root / "app.zip"
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("buzhidao.exe", "app")

            output = root / "out"
            output.mkdir()
            extract_archive(archive, output)

            self.assertTrue((output / "buzhidao.exe").exists())

    def test_tar_gz_아카이브를_압축해제한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            source.mkdir()
            (source / "buzhidao").write_text("app", encoding="utf-8")
            archive = root / "app.tar.gz"
            with tarfile.open(archive, "w:gz") as tf:
                tf.add(source, arcname=".")

            output = root / "out"
            output.mkdir()
            extract_archive(archive, output)

            self.assertTrue((output / "buzhidao").exists())

    def test_압축해제된_실행파일을_찾는다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            nested = root / "nested"
            nested.mkdir()
            (nested / "buzhidao").write_text("app", encoding="utf-8")

            self.assertEqual(find_release_binary(root, "linux"), nested / "buzhidao")

    def test_smoke_env는_ocr_환경변수를_구성한다(self):
        env = smoke_env(
            {"PATH": "base"},
            Path("image.png"),
            Path("models"),
            "en",
            "cpu",
        )

        self.assertEqual(env["PATH"], "base")
        self.assertEqual(env["BUZHIDAO_RELEASE_OCR_SMOKE_IMAGE"], "image.png")
        self.assertEqual(env["BUZHIDAO_PADDLE_MODEL_ROOT"], "models")
        self.assertEqual(env["BUZHIDAO_RELEASE_OCR_SMOKE_SOURCE"], "en")
        self.assertEqual(env["OCR_SERVER_DEVICE"], "cpu")

    def test_압축해제된_linux_바이너리를_smoke_인자로_실행한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            binary = root / "buzhidao"
            image = root / "image.png"
            models = root / "models"
            image.write_bytes(b"png")
            models.mkdir()
            write_shell_binary(
                binary,
                'test "$1" = "--release-ocr-smoke"\n'
                'test "$BUZHIDAO_RELEASE_OCR_SMOKE_IMAGE" = "image.png"\n'
                'echo "ok"\n',
            )

            result = run_extracted_binary_smoke(
                binary,
                Path("image.png"),
                models,
                base_env={"PATH": os.environ.get("PATH", "")},
            )

            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.command[-1], SMOKE_ARG)
            self.assertIn("ok", result.stdout)

    def test_smoke_실패_exit_code를_오류로_반환한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            binary = root / "buzhidao"
            image = root / "image.png"
            models = root / "models"
            image.write_bytes(b"png")
            models.mkdir()
            write_shell_binary(binary, 'echo "failed" >&2\nexit 7\n')

            with self.assertRaisesRegex(RuntimeError, "exit=7"):
                run_extracted_binary_smoke(
                    binary,
                    image,
                    models,
                    base_env={"PATH": os.environ.get("PATH", "")},
                )

    def test_아카이브에서_실제_바이너리를_실행한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "source"
            source.mkdir()
            write_shell_binary(source / "buzhidao", 'echo "archive ok"\n')
            archive = root / "app.tar.gz"
            with tarfile.open(archive, "w:gz") as tf:
                tf.add(source, arcname=".")

            image = root / "image.png"
            models = root / "models"
            image.write_bytes(b"png")
            models.mkdir()

            result = run_archive_smoke(archive, "linux", image, models)

            self.assertIn("archive ok", result.stdout)


if __name__ == "__main__":
    unittest.main()
