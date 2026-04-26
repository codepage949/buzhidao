import unittest
from pathlib import Path


RELEASE_WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release.yml"


class ReleaseWorkflowTest(unittest.TestCase):
    def test_ocr_smoke는_cpu_matrix에서만_실행하고_모델_루트를_고정한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("if: matrix.flavor == 'cpu'", workflow)
        self.assertIn("export BUZHIDAO_PADDLE_MODEL_ROOT=\"$PWD/.paddle_models\"", workflow)

    def test_native_sdk_준비는_python_unbuffered로_실행한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("python -u tools/scripts/setup_paddle_inference.py", workflow)
        self.assertNotIn("python tools/scripts/setup_paddle_inference.py", workflow)

    def test_tauri_cli는_binstall로_설치한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("uses: cargo-bins/cargo-binstall@v1.18.1", workflow)
        self.assertIn('version: "1.18.1"', workflow)
        self.assertIn("cargo binstall tauri-cli --version '^2' --no-confirm", workflow)
        self.assertNotIn("cargo install tauri-cli", workflow)


if __name__ == "__main__":
    unittest.main()
