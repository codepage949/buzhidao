# OCR 장치 설정 가능화

## 구현 목적

OCR 서버가 항상 GPU 장치로만 PaddleOCR를 초기화하고 있어, GPU가 없거나 CPU 실행이 필요한 환경에서 설정만으로 동작 모드를 바꿀 수 없다.

- OCR 장치를 환경 변수로 선택 가능하게 만든다.
- 기본 동작은 기존과 동일하게 GPU 우선으로 유지한다.
- 잘못된 설정값은 서버 시작 전에 명확히 실패하게 한다.

## 구현 계획

1. OCR 장치 설정을 읽는 헬퍼를 추가하고 PaddleOCR 초기화에서 사용한다.
2. 장치 설정 해석 로직 테스트를 추가한다.
3. OCR 실행 문서와 예시 환경 변수에 새 설정을 반영한다.
4. `pytest`로 OCR 서버 회귀를 확인한다.

## 구현 사항

- `ocr/main.py`에 `ocr_device()` 헬퍼를 추가하고 `OCR_DEVICE` 환경 변수로 `cpu`/`gpu`를 선택할 수 있게 했다.
- `OCR_DEVICE`가 없으면 기존 동작과 동일하게 `gpu`를 기본값으로 사용한다.
- 지원하지 않는 장치 값은 `ValueError`로 즉시 실패하게 해 잘못된 배포 설정을 빨리 드러내도록 했다.
- `ocr/test_main.py`에 장치 기본값, 공백/대소문자 정규화, 지원하지 않는 값 검증 테스트를 추가했다.
- `ocr/.env.example`, `ocr/README.md`에 새 환경 변수와 CPU 전환 방법을 문서화했다.
- `ocr/docker-compose.yaml`은 공통 실행 설정만 남기고, NVIDIA GPU 예약은 `ocr/docker-compose.gpu.yaml`로 분리했다.
- 루트 `README.md`에도 CPU/GPU 실행 방식과 수동 설정 필요 사항을 반영했다.
- `ocr/pyproject.toml`은 CPU 프로필로 바꾸고, `ocr/pyproject.gpu.toml`을 추가해 Paddle 런타임 의존성을 CPU/GPU로 분리했다.
- `ocr/Dockerfile`과 Compose 빌드 인자를 통해 CPU/GPU pyproject를 선택하도록 변경했다.
- 실행 중인 OCR 컨테이너를 대상으로 실제 `/infer/{src}` 요청을 보내는 `ocr/live_endpoint_check.py` 스모크 테스트 스크립트를 추가했다.

## 테스트 결과

- `uv run pytest`
  - Python 3.13.12 환경에서 테스트 9개 통과
- `uv run --group dev python live_endpoint_check.py --base-url http://127.0.0.1:8000 --source en`
  - 실행 중인 CPU Docker OCR 서버에 실제 HTTP 요청을 보내 200 응답과 JSON 리스트 형식을 확인
  - 첫 기동 모델 preload 지연을 고려해 재시도 로직으로 준비 완료를 대기
- `docker compose -f docker-compose.yaml -f docker-compose.gpu.yaml config`
  - GPU 실행 서비스가 `PADDLE_RUNTIME=gpu`, `OCR_DEVICE=gpu`, NVIDIA GPU 예약을 함께 갖는 설정으로 확인됨

## 추가 검토

- Paddle 런타임은 CPU/GPU pyproject로 분리했지만, 로컬에서 두 프로필을 자주 오가려면 별도 스크립트나 작업 디렉터리 분리가 있으면 더 편하다.
- Docker Compose에서는 GPU 예약을 조건부로 제거하기 어려워 GPU 전용 설정을 override 파일로 분리했다.
