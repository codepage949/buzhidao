import sys
from unittest.mock import MagicMock

# paddleocr는 GPU 환경에서만 설치 가능하므로 테스트 시 모킹
sys.modules["paddleocr"] = MagicMock()
