import json
import os
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import ocr_server  # noqa: E402


class ParseRequestTests(unittest.TestCase):
    def test_정상_요청을_튜플로_파싱한다(self):
        line = json.dumps(
            {"id": 7, "source": "en", "image_path": "a.png", "score_thresh": 0.25}
        )
        self.assertEqual(ocr_server.parse_request(line), (7, "en", "a.png", 0.25))

    def test_score_thresh_누락_시_기본값_0_5를_사용한다(self):
        line = json.dumps({"id": 1, "source": "ch", "image_path": "x.png"})
        self.assertEqual(ocr_server.parse_request(line), (1, "ch", "x.png", 0.5))

    def test_잘못된_json은_예외를_발생시킨다(self):
        with self.assertRaises(json.JSONDecodeError):
            ocr_server.parse_request("not json")

    def test_필수_필드_누락은_keyerror다(self):
        line = json.dumps({"id": 2, "source": "en"})
        with self.assertRaises(KeyError):
            ocr_server.parse_request(line)


class ResolveOcrDeviceTests(unittest.TestCase):
    def setUp(self):
        self._saved = os.environ.get("PYTHON_OCR_DEVICE")

    def tearDown(self):
        if self._saved is None:
            os.environ.pop("PYTHON_OCR_DEVICE", None)
        else:
            os.environ["PYTHON_OCR_DEVICE"] = self._saved

    def test_환경변수가_없으면_cpu다(self):
        os.environ.pop("PYTHON_OCR_DEVICE", None)
        self.assertEqual(ocr_server.resolve_ocr_device(), "cpu")

    def test_공백과_대소문자를_정규화한다(self):
        os.environ["PYTHON_OCR_DEVICE"] = "  GpU  "
        self.assertEqual(ocr_server.resolve_ocr_device(), "gpu")

    def test_잘못된_값은_value_error다(self):
        os.environ["PYTHON_OCR_DEVICE"] = "cuda"
        with self.assertRaises(ValueError):
            ocr_server.resolve_ocr_device()


if __name__ == "__main__":
    unittest.main()
