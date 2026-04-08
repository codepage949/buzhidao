# Python OCR 서버 → Rust ONNX Runtime 인프로세스 포팅

## 배경

`app/`(Tauri)이 `ocr/`(Python FastAPI + PaddleOCR)에 HTTP로 통신하던 구조를
ONNX Runtime 인프로세스 추론으로 교체하여 Python/Docker 의존성을 제거한다.

## 변경 내용

### 1. ONNX 모델 변환 (scripts/)

- `scripts/export_onnx_docker.py`: Docker 내에서 `paddle2onnx`로 모델 변환
- PaddlePaddle 3.x PIR 형식(inference.json) → ONNX opset 14
- 산출물: `app/models/det.onnx`(84MB), `cls.onnx`(1MB), `rec.onnx`(81MB), `rec_dict.txt`(18383자)

### 2. Rust OCR 모듈 (app/src/ocr/)

- `mod.rs`: `OcrEngine` — 3개 ONNX 세션(Mutex 래핑) + 사전 관리, det→cls→rec 파이프라인
- `det.rs`: 텍스트 검출 — resize(960, 128배수), normalize(ImageNet), DB 후처리(flood fill + unclip)
- `cls.rs`: 방향 분류 — resize(160x80), normalize, argmax (0°/180°)
- `rec.rs`: 텍스트 인식 — resize(H=48, 비율 유지), normalize(-1~1), CTC 디코딩

### 3. 기존 코드 수정

- `services.rs`: `run_ocr()` — HTTP 호출 제거, `OcrEngine::predict()` 호출로 교체 (동기)
- `config.rs`: `api_base_url` 제거, `score_thresh: f32` 추가
- `lib.rs`: `OcrEngine`을 `Arc<OcrEngine>`으로 Tauri 상태 등록, `spawn_blocking`으로 추론
- `Cargo.toml`: `ort 2.0.0-rc.12`, `ndarray 0.17` 추가, `reqwest` multipart 피처 제거
- `tauri.conf.json`: `bundle.resources`에 `models/*` 등록
- `.env` / `.env.example`: `API_BASE_URL` → `SCORE_THRESH`

### 4. 테스트

- `rec.rs`: CTC 디코딩 (기본, 빈 입력, 중복 제거 + blank 구분)
- `det.rs`: DB 후처리 (영역 검출, 빈 히트맵, 분리 컴포넌트)

### 5. 정리

- `ocr/` 디렉토리 전체 제거 (Python FastAPI + PaddleOCR 서버, Docker 구성, 테스트 등 15개 파일)
- `app/src/ocr/mod.rs`에 E2E 통합 테스트 추가 (`모델_로드_및_세션_초기화`, `테스트_이미지_추론`)

## 후속 작업

- CUDA EP 활성화 (`ort` features에 `cuda` 추가)
- `scripts/.venv` 정리
