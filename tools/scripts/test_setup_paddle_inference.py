import zipfile
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import MagicMock, patch
from io import BytesIO
from tools.scripts.setup_paddle_inference import (
    DEFAULT_DOWNLOAD_DIR,
    OPENCV_CONTRIB_PYTHON_VERSION,
    PADDLE_INFERENCE_VERSION,
    PYCLIPPER_VERSION,
    SHAPELY_VERSION,
    detect_archive_mode,
    download_paddle_inference_archive,
    import_opencv_sdk,
    opencv_platform_dirname,
    resolve_archive_filename,
    resolve_paddle_inference_url,
    setup_paddle_inference,
    write_sidecar_runtime_manifest,
)
import tools.scripts.setup_paddle_inference as setup_module


def write_minimal_archive(root: Path, archive_path: Path, include_nested: bool) -> None:
    (root / "include").mkdir(parents=True, exist_ok=True)
    (root / "lib").mkdir(parents=True, exist_ok=True)
    (root / "third_party").mkdir(parents=True, exist_ok=True)
    (root / "include" / "dummy.h").write_text("1")
    (root / "lib" / "dummy.lib").write_text("1")
    (root / "third_party" / "dummy.txt").write_text("1")

    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
            for path in sorted(root.rglob("*")):
                if path.is_file():
                    arcname = path.relative_to(root)
                    if include_nested:
                        arcname = Path("payload") / arcname
                    zf.write(path, arcname.as_posix())
        return

    if archive_path.suffixes[-2:] == [".tar", ".gz"] or archive_path.suffix == ".gz":
        with tarfile.open(archive_path, "w:gz") as tf:
            if include_nested:
                tf.add(root, arcname="payload")
            else:
                tf.add(root, arcname=".")
        return

    raise ValueError(f"지원하지 않는 형식: {archive_path}")


def write_mock_opencv_sdk(root: Path) -> None:
    (root / "install" / "include" / "opencv4" / "opencv2").mkdir(parents=True, exist_ok=True)
    (root / "install" / "lib").mkdir(parents=True, exist_ok=True)
    (root / "install" / "include" / "opencv4" / "opencv2" / "core.hpp").write_text("1")
    (root / "install" / "lib" / "libopencv_core.so").write_text("1")
    (root / "install" / "lib" / "libopencv_imgproc.so").write_text("1")
    (root / "install" / "lib" / "libopencv_imgcodecs.so").write_text("1")


class SetupPaddleInferenceTest(unittest.TestCase):
    def test_지원_포맷을_탐지한다(self):
        self.assertEqual(detect_archive_mode(Path("a.zip")), "zip")
        self.assertEqual(detect_archive_mode(Path("a.tar.gz")), "tar.gz")
        self.assertEqual(detect_archive_mode(Path("a.tgz")), "tar.gz")
        self.assertEqual(detect_archive_mode(Path("a.tar.xz")), "tar.xz")
        self.assertEqual(detect_archive_mode(Path("a.tar.bz2")), "tar.bz2")
        self.assertEqual(detect_archive_mode(Path("a.tbz2")), "tar.bz2")

    def test_zip로_추출해_paddle_inference를_설치한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "payload"
            write_minimal_archive(source, root / "inference.zip", False)

            destination = root / ".paddle_inference"
            result = setup_paddle_inference(root / "inference.zip", destination)

            self.assertEqual(result, destination)
            self.assertTrue((destination / "include" / "dummy.h").exists())
            self.assertTrue((destination / "lib" / "dummy.lib").exists())
            self.assertTrue((destination / "third_party" / "dummy.txt").exists())

    def test_중첩_디렉터리_배포본을_자동_감지한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "payload"
            write_minimal_archive(source, root / "inference.zip", True)

            destination = root / ".paddle_inference_nested"
            result = setup_paddle_inference(root / "inference.zip", destination)

            self.assertEqual(result, destination)
            self.assertTrue((destination / "include" / "dummy.h").exists())
            self.assertTrue((destination / "lib" / "dummy.lib").exists())

    def test_windows_zip처럼_third_party가_layout_root_바깥이어도_복사한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "payload"
            (source / "paddle" / "include").mkdir(parents=True, exist_ok=True)
            (source / "paddle" / "lib").mkdir(parents=True, exist_ok=True)
            (source / "third_party").mkdir(parents=True, exist_ok=True)
            (source / "paddle" / "include" / "dummy.h").write_text("1")
            (source / "paddle" / "lib" / "dummy.lib").write_text("1")
            (source / "third_party" / "mklml.dll").write_text("1")

            archive_path = root / "windows-layout.zip"
            with zipfile.ZipFile(archive_path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                for path in sorted(source.rglob("*")):
                    if path.is_file():
                        zf.write(path, path.relative_to(source).as_posix())

            destination = root / ".paddle_inference"
            result = setup_paddle_inference(archive_path, destination)

            self.assertEqual(result, destination)
            self.assertTrue((destination / "include" / "dummy.h").exists())
            self.assertTrue((destination / "lib" / "dummy.lib").exists())
            self.assertTrue((destination / "third_party" / "mklml.dll").exists())

    def test_지원하지_않는_아카이브_형식은_오류를_낸다(self):
        with tempfile.TemporaryDirectory() as td:
            archive_path = Path(td) / "invalid.7z"
            archive_path.write_text("invalid", encoding="utf-8")
            with self.assertRaises(ValueError):
                setup_paddle_inference(archive_path, Path(td) / "out")

    def test_플랫폼별_URL이_정의되어_있다(self):
        system, url = resolve_paddle_inference_url()
        _, machine = setup_module.resolve_platform_key()
        self.assertIn(
            url,
            setup_module.PADDLE_INFERENCE_V3_URLS["cpu"][system][machine].values(),
        )

    @patch.object(setup_module, "resolve_platform_key", return_value=("windows", "x86_64"))
    def test_windows_gpu_URL은_cuda126을_사용한다(self, _platform_key):
        system, url = resolve_paddle_inference_url(device="gpu")

        self.assertEqual(system, "windows")
        self.assertIn("/Windows/GPU/", url)
        self.assertIn("cuda12.6", url)

    @patch.object(setup_module, "resolve_platform_key", return_value=("linux", "x86_64"))
    def test_linux_gpu_URL은_cuda126을_사용한다(self, _platform_key):
        _, url = resolve_paddle_inference_url(device="gpu")

        self.assertIn("cuda12.6", url)

    @patch.object(setup_module, "resolve_platform_key", return_value=("darwin", "arm64"))
    def test_gpu를_지원하지_않는_OS는_오류를_낸다(self, _platform_key):
        with self.assertRaises(RuntimeError):
            resolve_paddle_inference_url(device="gpu")

    @patch.object(setup_module, "resolve_platform_key", return_value=("linux", "aarch64"))
    def test_지원하지_않는_arch는_오류를_낸다(self, _platform_key):
        with self.assertRaises(RuntimeError):
            resolve_paddle_inference_url(device="cpu")

    def test_지원하지_않는_device는_오류를_낸다(self):
        with self.assertRaises(RuntimeError):
            resolve_paddle_inference_url(device="tpu")

    @patch.object(setup_module, "resolve_platform_key", return_value=("linux", "x86_64"))
    def test_아카이브_확장자별_이름을_생성한다(self, _platform_key):
        self.assertEqual(
            resolve_archive_filename("https://x/paddle_inference.zip"),
            f"paddle_inference-{PADDLE_INFERENCE_VERSION}-linux-cpu.zip",
        )
        self.assertEqual(
            resolve_archive_filename("https://x/paddle_inference.tgz", device="gpu"),
            f"paddle_inference-{PADDLE_INFERENCE_VERSION}-linux-gpu-cu126.tgz",
        )

    @patch.object(setup_module, "resolve_platform_key", return_value=("windows", "x86_64"))
    def test_cpu와_gpu_아카이브_이름은_서로_충돌하지_않는다(self, _platform_key):
        cpu_name = resolve_archive_filename("https://x/paddle_inference.zip", device="cpu")
        gpu_name = resolve_archive_filename("https://x/paddle_inference.zip", device="gpu")

        self.assertEqual(
            cpu_name,
            f"paddle_inference-{PADDLE_INFERENCE_VERSION}-windows-cpu.zip",
        )
        self.assertEqual(
            gpu_name,
            f"paddle_inference-{PADDLE_INFERENCE_VERSION}-windows-gpu-cu126.zip",
        )
        self.assertNotEqual(cpu_name, gpu_name)

    def test_지원하지_않는_다운로드_URL은_오류를_낸다(self):
        with self.assertRaises(ValueError):
            resolve_archive_filename("https://x/paddle_inference.7z")

    @patch("tools.scripts.setup_paddle_inference.platform.machine")
    @patch("tools.scripts.setup_paddle_inference.platform.system")
    def test_플랫폼키가_정상_해석된다(self, system_mock, machine_mock):
        system_mock.return_value = "Windows"
        machine_mock.return_value = "AMD64"
        self.assertEqual(setup_module.resolve_platform_key(), ("windows", "x86_64"))

    def test_opencv_플랫폼_디렉터리명을_생성한다(self):
        self.assertEqual(opencv_platform_dirname("windows", "x86_64"), "windows-x86_64")
        self.assertEqual(opencv_platform_dirname("darwin", "arm64"), "darwin-arm64")

    @patch("tools.scripts.setup_paddle_inference.urllib.request.urlopen")
    def test_다운로드는_로컬로_저장한다(self, urlopen_mock: MagicMock):
        class Response(BytesIO):
            def __enter__(self):
                return self
            def __exit__(self, exc_type, exc, tb):
                return None

        urlopen_mock.return_value = Response(b"fake-data")

        with tempfile.TemporaryDirectory() as td:
            download_dir = Path(td)
            expected = download_dir / f"paddle_inference-{PADDLE_INFERENCE_VERSION}-windows-gpu-cu126.zip"
            with patch.object(setup_module, "resolve_platform_key", return_value=("windows", "x86_64")), \
                    patch.object(
                        setup_module, "resolve_paddle_inference_url",
                        return_value=("windows", "https://example.com/path/paddle_inference.zip"),
                    ):
                result = download_paddle_inference_archive(
                    download_dir,
                    force=True,
                    device="gpu",
                )
                self.assertEqual(result, expected)
                self.assertTrue(result.exists())
                self.assertEqual(result.read_bytes(), b"fake-data")

    @patch("tools.scripts.setup_paddle_inference.urllib.request.urlopen")
    def test_기존_아카이브는_force가_아니면_재사용한다(self, urlopen_mock: MagicMock):
        with tempfile.TemporaryDirectory() as td:
            download_dir = Path(td)
            archive = download_dir / f"paddle_inference-{PADDLE_INFERENCE_VERSION}-windows-cpu.zip"
            archive.write_bytes(b"cached")

            with patch.object(setup_module, "resolve_platform_key", return_value=("windows", "x86_64")), \
                    patch.object(
                        setup_module,
                        "resolve_paddle_inference_url",
                        return_value=("windows", "https://example.com/path/paddle_inference.zip"),
                    ):
                result = download_paddle_inference_archive(download_dir, force=False)

            self.assertEqual(result, archive)
            self.assertEqual(result.read_bytes(), b"cached")
            urlopen_mock.assert_not_called()

    def test_기본_다운로드_경로는_os_임시_디렉터리_아래다(self):
        self.assertEqual(DEFAULT_DOWNLOAD_DIR.name, "buzhidao-paddle-inference")
        self.assertTrue(str(DEFAULT_DOWNLOAD_DIR).startswith(tempfile.gettempdir()))

    def test_sidecar_런타임_매니페스트를_기록한다(self):
        with tempfile.TemporaryDirectory() as td:
            destination = Path(td) / ".paddle_inference"
            manifest = write_sidecar_runtime_manifest(destination)

            self.assertTrue(manifest.exists())
            payload = manifest.read_text(encoding="utf-8")
            self.assertIn(PADDLE_INFERENCE_VERSION, payload)
            self.assertIn(OPENCV_CONTRIB_PYTHON_VERSION, payload)
            self.assertIn(PYCLIPPER_VERSION, payload)
            self.assertIn(SHAPELY_VERSION, payload)

    @patch.object(setup_module, "resolve_platform_key", return_value=("linux", "x86_64"))
    def test_기존_opencv_sdk를_플랫폼별_경로로_가져온다(self, _platform_key):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "opencv-source"
            destination = root / ".paddle_inference"
            write_mock_opencv_sdk(source)

            result = import_opencv_sdk(source, destination)

            expected = destination / "third_party" / "opencv-sdk" / "linux-x86_64"
            self.assertEqual(result, expected)
            self.assertTrue((expected / "install" / "include" / "opencv4" / "opencv2" / "core.hpp").exists())
            self.assertTrue((expected / "install" / "lib" / "libopencv_core.so").exists())


if __name__ == "__main__":
    unittest.main()
