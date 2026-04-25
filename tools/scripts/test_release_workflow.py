import unittest
from pathlib import Path


RELEASE_WORKFLOW = Path(__file__).resolve().parents[2] / ".github" / "workflows" / "release.yml"


class ReleaseWorkflowTest(unittest.TestCase):
    def test_ocr_smoke는_cpu_matrix에서만_실행하고_모델_루트를_고정한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("if: matrix.flavor == 'cpu'", workflow)
        self.assertIn("export BUZHIDAO_PADDLE_MODEL_ROOT=\"$PWD/.paddle_models\"", workflow)


if __name__ == "__main__":
    unittest.main()
