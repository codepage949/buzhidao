import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

from tools.scripts.setup_cuda_runtime import (
    PackageSet,
    clean_directory,
    download_wheels,
    extract_cuda_libraries,
    is_cuda_library_member,
    normalize_platform,
    packages_for,
)
import tools.scripts.setup_cuda_runtime as setup_module


def write_wheel(path: Path, members: dict[str, bytes]) -> None:
    with zipfile.ZipFile(path, "w") as zf:
        for name, content in members.items():
            zf.writestr(name, content)


class SetupCudaRuntimeTest(unittest.TestCase):
    def test_windows는_nvidia_bin_dll만_대상으로_본다(self):
        self.assertTrue(is_cuda_library_member("nvidia/cublas/bin/cublas64_12.dll", "windows"))
        self.assertFalse(is_cuda_library_member("nvidia/cublas/lib/libcublas.so.12", "windows"))
        self.assertFalse(is_cuda_library_member("other/cublas/bin/cublas64_12.dll", "windows"))

    def test_linux는_nvidia_lib_so만_대상으로_본다(self):
        self.assertTrue(is_cuda_library_member("nvidia/cublas/lib/libcublas.so.12", "linux"))
        self.assertTrue(is_cuda_library_member("nvidia/cudnn/lib/libcudnn.so", "linux"))
        self.assertFalse(is_cuda_library_member("nvidia/cublas/bin/cublas64_12.dll", "linux"))

    def test_windows_wheel에서_dll을_추출한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wheelhouse = root / "wheelhouse"
            output = root / ".cuda"
            wheelhouse.mkdir()
            write_wheel(
                wheelhouse / "nvidia_cuda_runtime.whl",
                {
                    "nvidia/cuda_runtime/bin/cudart64_12.dll": b"dll",
                    "nvidia/cuda_runtime/lib/libignored.so": b"so",
                    "not-nvidia/bin/ignored.dll": b"ignored",
                },
            )

            extracted = extract_cuda_libraries(wheelhouse, output, "windows")

            self.assertEqual([path.name for path in extracted], ["cudart64_12.dll"])
            self.assertEqual((output / "cudart64_12.dll").read_bytes(), b"dll")

    def test_linux_wheel에서_so를_추출한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wheelhouse = root / "wheelhouse"
            output = root / ".cuda"
            wheelhouse.mkdir()
            write_wheel(
                wheelhouse / "nvidia_cudnn.whl",
                {
                    "nvidia/cudnn/lib/libcudnn.so.9": b"so",
                    "nvidia/cudnn/bin/cudnn64_9.dll": b"dll",
                },
            )

            extracted = extract_cuda_libraries(wheelhouse, output, "linux")

            self.assertEqual([path.name for path in extracted], ["libcudnn.so.9"])
            self.assertEqual((output / "libcudnn.so.9").read_bytes(), b"so")

    def test_같은_파일명은_처음_발견한_항목만_유지한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wheelhouse = root / "wheelhouse"
            output = root / ".cuda"
            wheelhouse.mkdir()
            write_wheel(
                wheelhouse / "a.whl",
                {"nvidia/a/bin/shared.dll": b"first"},
            )
            write_wheel(
                wheelhouse / "b.whl",
                {"nvidia/b/bin/shared.dll": b"second"},
            )

            extracted = extract_cuda_libraries(wheelhouse, output, "windows")

            self.assertEqual([path.name for path in extracted], ["shared.dll"])
            self.assertEqual((output / "shared.dll").read_bytes(), b"first")

    def test_package_override는_기본_package_set을_대체한다(self):
        package_set = packages_for("ort-cu12", ["custom-package==1"])
        self.assertEqual(package_set.packages, ("custom-package==1",))
        self.assertEqual(package_set.extra_index_urls, ())

    def test_지원하지_않는_package_set은_오류를_낸다(self):
        with self.assertRaises(ValueError):
            packages_for("unknown", None)

    def test_paddle_cuda12_package_set은_cuda126_계열을_사용한다(self):
        package_set = packages_for("paddle-cu126", None)

        self.assertIn("nvidia-cuda-runtime-cu12==12.6.77", package_set.packages)
        self.assertIn("nvidia-cudnn-cu12==9.5.1.17", package_set.packages)

    def test_빈_wheelhouse는_오류를_낸다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wheelhouse = root / "wheelhouse"
            wheelhouse.mkdir()

            with self.assertRaises(FileNotFoundError):
                extract_cuda_libraries(wheelhouse, root / ".cuda", "windows")

    def test_cuda_라이브러리가_없는_wheel은_오류를_낸다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            wheelhouse = root / "wheelhouse"
            wheelhouse.mkdir()
            write_wheel(wheelhouse / "empty.whl", {"nvidia/cudnn/LICENSE": b"text"})

            with self.assertRaises(FileNotFoundError):
                extract_cuda_libraries(wheelhouse, root / ".cuda", "windows")

    def test_clean_directory는_기존_내용을_지우고_다시_생성한다(self):
        with tempfile.TemporaryDirectory() as td:
            target = Path(td) / "target"
            target.mkdir()
            (target / "old.dll").write_bytes(b"old")

            clean_directory(target)

            self.assertTrue(target.is_dir())
            self.assertFalse((target / "old.dll").exists())

    @patch.object(setup_module.subprocess, "run")
    def test_download_wheels는_package_set과_extra_index를_pip_download에_전달한다(self, run_mock):
        with tempfile.TemporaryDirectory() as td:
            wheelhouse = Path(td) / "wheelhouse"
            package_set = PackageSet(
                packages=("a==1", "b==2"),
                extra_index_urls=("https://base.example/simple",),
            )

            download_wheels(
                "python",
                wheelhouse,
                package_set,
                ["https://extra.example/simple"],
            )

            command = run_mock.call_args.args[0]
            self.assertEqual(
                command[:6],
                ["python", "-m", "pip", "download", "--only-binary", ":all:"],
            )
            self.assertIn(str(wheelhouse), command)
            self.assertIn("https://base.example/simple", command)
            self.assertIn("https://extra.example/simple", command)
            self.assertEqual(command[-2:], ["a==1", "b==2"])
            self.assertTrue(run_mock.call_args.kwargs["check"])

    @patch.object(setup_module, "current_platform", return_value="windows")
    def test_auto_platform은_현재_platform으로_정규화한다(self, _current_platform):
        self.assertEqual(normalize_platform("auto"), "windows")

    def test_지원하지_않는_platform은_오류를_낸다(self):
        with self.assertRaises(ValueError):
            normalize_platform("darwin")


if __name__ == "__main__":
    unittest.main()
