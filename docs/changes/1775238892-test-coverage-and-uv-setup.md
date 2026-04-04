## 배경

- `deno task test`로 테스트를 실행할 수 없었다 (`test` 태스크 미정의).
- `src/detection_test.ts`에 기본 케이스만 존재하고 경계 조건·엣지 케이스가 누락되어 있다.
- Python 서버에 테스트가 전혀 없다.
- 서버 의존성이 `requirements.txt`로만 관리되어 uv 프로젝트 구조가 없었다.

## 목표

- `deno.json`에 `test` 태스크를 추가해 `deno task test`로 실행 가능하게 한다.
- `src/detection_test.ts`에 엣지 케이스와 경계 조건 테스트를 추가한다.
- `server/test_main.py`를 작성해 핵심 서버 로직을 테스트한다.
- `server/pyproject.toml`로 uv 프로젝트를 설정하고 `requirements*.txt`를 제거한다.
- `server/Dockerfile`을 uv 기반으로 업데이트한다.

## 비목표

- GPU/PaddleOCR 실제 추론 테스트 (환경 의존)
- 키보드 훅·클립보드 등 육안 검증이 필요한 기능 테스트

## 작업 내용

### Deno

1. `deno.json`: `test` 태스크 추가 (`deno test src/`)
2. `src/detection_test.ts`: 테스트 3개 → 11개로 확장
   - `isSourceLanguage`: 빈 문자열, 숫자/기호만 있는 경우
   - `groupDetections`: 빈 배열, 단일 탐지, 임계값 경계(안쪽/바깥쪽), 중국어 소스, 여러 독립 그룹

### Python 서버

3. `server/pyproject.toml`: uv 프로젝트 설정 신규 작성
   - 본 의존성: fastapi, paddlepaddle-gpu, paddleocr 등
   - dev 의존성: pytest, httpx (starlette 0.36.3 호환 버전 고정)
   - paddlepaddle custom index 설정 (`[tool.uv.sources]` + `[[tool.uv.index]]`)
   - pytest 설정 (`[tool.pytest.ini_options]`)
4. `server/uv.lock`: lock 파일 생성 (84개 패키지)
5. `server/Dockerfile`: uv 기반으로 업데이트 (`pip` → `uv sync --no-dev`)
6. `server/conftest.py`: paddleocr 모킹 (GPU 없는 환경에서 테스트 실행 가능)
7. `server/test_main.py`: 6개 테스트 작성
   - `save_upload_to_temp`: 확장자 보존, 내용 정확히 저장, 확장자 없으면 .png 기본값
   - `/infer/{src}`: 미지원 언어 → 400, 빈 OCR 결과 반환, 텍스트 목록 반환
8. `server/requirements.txt`, `server/requirements-dev.txt`: 삭제 (pyproject.toml로 통합)

## 비고

- `starlette==0.36.3`은 `httpx>=0.27`과 호환되지 않아 dev 의존성에서 `httpx<0.27.0`으로 고정
- `uv run --group dev pytest`로 서버 테스트 실행
