import tempfile
import unittest
import zipfile
from io import BytesIO
from pathlib import Path
from unittest.mock import MagicMock, patch

import tools.scripts.setup_deno as setup_module
from tools.scripts.setup_deno import (
    download_archive,
    install_deno_archive,
    resolve_asset_name,
    resolve_deno_version,
)


def response(content: bytes):
    class Response(BytesIO):
        def __enter__(self):
            return self

        def __exit__(self, exc_type, exc, tb):
            return None

    return Response(content)


class SetupDenoTest(unittest.TestCase):
    def test_exact_version은_v_prefix를_붙인다(self):
        self.assertEqual(resolve_deno_version("2.5.6"), "v2.5.6")
        self.assertEqual(resolve_deno_version("v2.5.6"), "v2.5.6")

    @patch("tools.scripts.setup_deno.urllib.request.urlopen")
    def test_v2x는_latest_v2_release로_해석한다(self, urlopen_mock: MagicMock):
        urlopen_mock.return_value = response(
            b"""[
              {"tag_name": "v3.0.0", "prerelease": false},
              {"tag_name": "v2.5.6", "prerelease": false}
            ]"""
        )

        self.assertEqual(resolve_deno_version("v2.x", retry_delay_seconds=0), "v2.5.6")

    def test_platform별_asset_name을_해석한다(self):
        self.assertEqual(
            resolve_asset_name("windows", "AMD64"),
            "deno-x86_64-pc-windows-msvc.zip",
        )
        self.assertEqual(
            resolve_asset_name("linux", "x86_64"),
            "deno-x86_64-unknown-linux-gnu.zip",
        )

    @patch("tools.scripts.setup_deno.urllib.request.urlopen")
    def test_archive_다운로드는_재시도한다(self, urlopen_mock: MagicMock):
        urlopen_mock.side_effect = [
            setup_module.urllib.error.HTTPError(
                "https://example.invalid",
                502,
                "Bad Gateway",
                hdrs=None,
                fp=None,
            ),
            response(b"zip"),
        ]

        with tempfile.TemporaryDirectory() as td, patch.object(setup_module.time, "sleep") as sleep_mock:
            archive = download_archive(
                "v2.5.6",
                "deno-x86_64-unknown-linux-gnu.zip",
                Path(td),
                retry_delay_seconds=0,
            )

            self.assertEqual(archive.read_bytes(), b"zip")
            self.assertEqual(urlopen_mock.call_count, 2)
            sleep_mock.assert_called_once_with(0)

    def test_archive를_bin_디렉터리에_설치한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            archive = root / "deno.zip"
            with zipfile.ZipFile(archive, "w") as zf:
                zf.writestr("deno", b"binary")

            with patch.object(setup_module.platform, "system", return_value="Linux"):
                bin_dir = install_deno_archive(archive, root / ".deno")

            self.assertTrue((bin_dir / "deno").exists())


if __name__ == "__main__":
    unittest.main()
