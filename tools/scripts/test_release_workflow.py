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

    def test_release_빌드는_rust_중간_산출물_캐시를_재사용한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertEqual(workflow.count("name: Restore Rust build cache"), 2)
        self.assertEqual(workflow.count("uses: actions/cache@v4"), 2)
        self.assertEqual(
            workflow.count("key: release-rust-${{ matrix.label }}-${{ github.run_id }}"),
            2,
        )
        self.assertEqual(
            workflow.count("release-rust-${{ matrix.label }}-"),
            4,
        )
        self.assertEqual(workflow.count("~/.cargo/registry"), 2)
        self.assertEqual(workflow.count("~/.cargo/git"), 2)
        self.assertEqual(workflow.count("\n            target\n"), 2)

    def test_릴리스_버전_커밋은_빌드_통과_후에만_main에_push한다(self):
        workflow = RELEASE_WORKFLOW.read_text(encoding="utf-8")

        self.assertIn("git bundle create release-candidate.bundle release-candidate", workflow)
        self.assertIn("name: Upload release candidate", workflow)
        self.assertEqual(workflow.count("name: Download release candidate"), 2)
        self.assertEqual(workflow.count("git fetch release-candidate.bundle release-candidate"), 2)
        self.assertEqual(workflow.count("Release candidate mismatch"), 2)
        self.assertIn("Branch moved before release publish", workflow)
        self.assertIn(
            'git push origin "${{ needs.version.outputs.release_sha }}:${{ github.ref_name }}"',
            workflow,
        )
        self.assertNotIn('git push origin "HEAD:${{ github.ref_name }}"', workflow)


if __name__ == "__main__":
    unittest.main()
