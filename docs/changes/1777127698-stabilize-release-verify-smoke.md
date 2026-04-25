# 릴리즈 verify OCR smoke 안정화

## 배경

GitHub Actions 릴리즈 검증 matrix에서 다음 문제가 확인됐다.

- Linux CPU OCR smoke
  - 모델 다운로드는 수행됐지만 런타임 warmup에서 det/cls/rec 모델 탐지가 실패했다.
  - CI smoke 모델 루트가 워크스페이스 내부로 고정되어 있지 않아 재현성과 진단성이 낮았다.
- Linux GPU OCR smoke
  - hosted runner에는 GPU가 없는데 GPU Paddle Inference SDK로 링크한 바이너리에서 실제 OCR runtime smoke를 실행했다.
  - GPU 없는 환경에서 GPU flavor의 런타임 OCR 성공을 검증하는 것은 의미가 없고 abort 위험이 있다.
- Windows GPU native SDK 준비
  - OpenCV Windows SDK 다운로드가 SourceForge 502에 취약했다.

## 결정

- 릴리즈 smoke 모델 루트는 명시적 환경 변수로 워크스페이스 내부에 고정한다.
- GPU flavor matrix는 빌드 검증까지만 수행하고, 실제 OCR 성공 smoke는 CPU flavor에서 수행한다.
- Windows OpenCV SDK 다운로드는 일시적 네트워크 오류에 재시도한다.

## 구현

- `BUZHIDAO_PADDLE_MODEL_ROOT`
  - 지정되면 PaddleOCR 모델 다운로드/탐색 루트의 최우선 후보로 사용한다.
- `.github/workflows/release.yml`
  - OCR smoke 단계는 CPU flavor에서만 실행한다.
  - smoke 모델 루트를 `.paddle_models`로 고정한다.
- `tools/scripts/setup_paddle_inference.py`
  - 공통 다운로드 함수에 재시도 로직을 추가한다.
- `tools/scripts/test_release_workflow.py`
  - workflow가 CPU-only OCR smoke와 smoke 모델 루트 고정을 유지하는지 검증한다.

## 검증 결과

- Paddle 모델 루트 환경 변수 우선순위 단위 테스트를 추가한다.
- 다운로드 재시도 단위 테스트를 추가한다.
- release workflow 계약 테스트를 추가한다.
- `python -m unittest tools.scripts.test_setup_paddle_inference tools.scripts.test_release_workflow`
  - 결과: 25개 테스트 통과.
- `cargo test -p buzhidao --lib paddle_models`
  - 결과: 5개 테스트 통과.
- `git diff --check`
  - 결과: 통과.
