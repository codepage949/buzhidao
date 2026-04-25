import json
import tempfile
import unittest
from pathlib import Path

from tools.scripts.update_release_version import (
    normalize_version,
    replace_lock_package_version,
    replace_package_version_toml,
    update_release_version,
)


class UpdateReleaseVersionTest(unittest.TestCase):
    def test_v_prefix를_제거해_semver를_정규화한다(self):
        self.assertEqual(normalize_version("v1.2.3"), "1.2.3")
        self.assertEqual(normalize_version("1.2.3"), "1.2.3")

    def test_지원하지_않는_버전은_오류를_낸다(self):
        with self.assertRaises(ValueError):
            normalize_version("release-1")

    def test_cargo_toml_package_version만_갱신한다(self):
        content = '[package]\nname = "buzhidao"\nversion = "0.1.0"\n\n[dependencies]\nversion = "x"\n'

        updated = replace_package_version_toml(content, "1.2.3")

        self.assertIn('version = "1.2.3"', updated)
        self.assertIn('[dependencies]\nversion = "x"', updated)

    def test_cargo_lock의_buzhidao_package_version만_갱신한다(self):
        content = (
            '[[package]]\nname = "other"\nversion = "0.1.0"\n'
            '[[package]]\nname = "buzhidao"\nversion = "0.1.0"\n'
        )

        updated = replace_lock_package_version(content, "buzhidao", "1.2.3")

        self.assertIn('name = "other"\nversion = "0.1.0"', updated)
        self.assertIn('name = "buzhidao"\nversion = "1.2.3"', updated)

    def test_repo_버전_파일들을_함께_갱신한다(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            (root / "Cargo.toml").write_text(
                '[package]\nname = "buzhidao"\nversion = "0.1.0"\n',
                encoding="utf-8",
            )
            (root / "Cargo.lock").write_text(
                '[[package]]\nname = "buzhidao"\nversion = "0.1.0"\n',
                encoding="utf-8",
            )
            (root / "tauri.conf.json").write_text(
                json.dumps({"productName": "buzhidao", "version": "0.1.0"}),
                encoding="utf-8",
            )

            normalized = update_release_version(root, "v1.2.3")

            self.assertEqual(normalized, "1.2.3")
            self.assertIn('version = "1.2.3"', (root / "Cargo.toml").read_text(encoding="utf-8"))
            self.assertIn('version = "1.2.3"', (root / "Cargo.lock").read_text(encoding="utf-8"))
            tauri = json.loads((root / "tauri.conf.json").read_text(encoding="utf-8"))
            self.assertEqual(tauri["version"], "1.2.3")


if __name__ == "__main__":
    unittest.main()
