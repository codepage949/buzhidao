# Paddle FFI 환경변수 초기화와 Sidecar 강제 비교 경로 정리

## 배경

최근 Paddle FFI 비교 경로는 엔진 생성 인자 전달 방식과 비교 스크립트의 실행 방식이 서로
맞물려 있지 않았습니다.

- Rust는 FFI 엔진 생성 시 모델 경로와 source를 C 문자열 인자로 직접 넘기고 있었습니다.
- 비교 스크립트는 `cargo test` 기반 FFI 샘플 테스트를 여러 안전 모드 조합으로 재시도했습니다.
- 실제 비교 목적은 FFI 브리지 자체보다 "동일 입력에 대해 sidecar 기준 결과를 재사용하거나
  강제 sidecar 경로로 우회"하는 것이 더 중요해졌습니다.
- FFI 샘플 테스트 입력은 BMP를 기대하는데, 비교 스크립트는 일반 이미지 파일을 그대로 넘길 수
  있어 포맷 차이 리스크가 있었습니다.

## 결정

- FFI 엔진 생성 시 모델 경로와 source는 환경변수(`BUZHIDAO_PADDLE_FFI_MODEL_DIR`,
  `BUZHIDAO_PADDLE_FFI_SOURCE`)를 우선 사용하도록 정리한다.
- Rust `PaddleFfiEngine`는 브리지 호출에 null 인자를 넘기고, 실제 값은 환경변수에 넣어
  초기화 경로를 단순화한다.
- `BUZHIDAO_PADDLE_FFI_FORCE_SIDECAR=1`일 때는 warmup과 OCR 실행 모두 Python sidecar
  경로로 즉시 우회할 수 있게 한다.
- 비교 스크립트는 FFI safe mode 재시도/크래시 폴백을 제거하고, 현재 단일 실행 결과를
  명확하게 실패로 취급한다.
- 비교 스크립트는 FFI 샘플 테스트에 넣기 전 입력 이미지를 BMP로 맞춰 포맷 차이로 인한
  오탐을 줄인다.

## 변경 사항

### 네이티브 브리지

- `app/native/paddle_bridge/bridge.cc`
  - 모델 쌍 반환 타입을 `std::pair<std::string, std::string>`에서
    `std::pair<fs::path, fs::path>`로 변경했다.
  - 경로를 문자열로 너무 일찍 평탄화하지 않도록 수정해 후속 `parent_path()` 계산과 로그를
    더 직접적으로 처리한다.
  - `resolve_model_pair` 실패 시 빈 문자열 쌍 대신 기본 생성된 빈 `fs::path` 쌍을 반환한다.
  - `configure_predictor`는 최종 `SetModel` 직전에만 `.string()`으로 변환한다.
  - `buzhi_ocr_create`는 인자로 받은 `model_dir`/`source`가 비어 있으면
    `BUZHIDAO_PADDLE_FFI_MODEL_DIR`, `BUZHIDAO_PADDLE_FFI_SOURCE`를 사용한다.
  - 언어 선택 로직은 null/빈 source 처리와 분리해 단순화했다.
  - det/cls/rec 선택 경로와 preprocess config 로딩에서 `fs::path`를 직접 재사용한다.

### Rust FFI 엔진

- `app/src/ocr/paddle_ffi.rs`
  - `BUZHIDAO_PADDLE_FFI_FORCE_SIDECAR`가 켜져 있으면 `warmup()`은 아무 작업 없이 성공으로
    반환한다.
  - 같은 플래그가 켜진 OCR 실행은 FFI 브리지 대신 `ocr_server.py`를 직접 실행하는
    `run_sidecar_backend()`로 우회한다.
  - sidecar Python 실행 파일은 `OCR_SERVER_PYTHON` 우선, 없으면
    `ocr_server/.venv/Scripts/python.exe` 또는 `ocr_server/.venv/bin/python`, 마지막으로
    플랫폼 기본 `python`/`python3`를 사용한다.
  - sidecar 실행 시 저장소 루트를 기준으로 `ocr_server/ocr_server.py --image ... --source ...`
    형태로 호출하고, 라이브러리 충돌을 줄이기 위해 `LD_LIBRARY_PATH`,
    `DYLD_LIBRARY_PATH`는 제거한다.
  - sidecar stdout의 마지막 JSON 라인을 파싱해 기존 FFI 반환 타입과 동일한
    `(detections, debug_detections)` 구조로 변환한다.
  - 엔진 생성 시에는 모델 경로와 source를 환경변수에 기록한 뒤 `buzhi_ocr_create(null, ..., null)`
    형태로 호출한다.
  - 기존 샘플 테스트의 `BUZHIDAO_PADDLE_FFI_SAFE_MODE=1` 자동 skip 분기는 제거됐다.

### 비교 스크립트

- `scripts/compare_ocr_sidecar_ffi.py`
  - `--ffi-safe-mode` 옵션과 관련 환경변수 주입을 제거했다.
  - FFI 실행은 이제 단일 `cargo test` 호출만 수행하고, 실패 시 즉시 에러로 처리한다.
  - `ffi_env()`는 `OCR_SERVER_PYTHON`을 주입해 Rust 쪽 강제 sidecar 우회 경로가 같은 Python
    환경을 사용하도록 맞춘다.
  - `prepare_ffi_image()`를 추가해 BMP가 아닌 입력 이미지는 Pillow로 임시 BMP로 변환한 뒤
    FFI 샘플 테스트에 전달한다.
  - 비교가 끝나면 임시 BMP 파일을 정리한다.

## 영향

- FFI 엔진 생성 인터페이스가 "인자 직접 전달"보다 "환경변수 기반 초기화"에 가까워졌다.
- 강제 sidecar 모드를 통해 Rust OCR 호출부를 유지한 채 Python sidecar 결과를 직접 비교하거나
  디버깅할 수 있다.
- 비교 스크립트는 조용히 폴백하지 않고 명시적으로 실패하므로 현재 깨진 지점을 더 빨리
  드러낸다.
- BMP 변환을 추가해 이미지 포맷 차이로 인한 비교 노이즈를 줄였다.

## 주의 사항

- sidecar 강제 경로는 `ocr_server.py` CLI 출력에서 마지막 JSON 라인을 찾는 방식에 의존한다.
  CLI 출력 형식이 바뀌면 Rust 파싱 로직도 함께 수정해야 한다.
- 환경변수 기반 초기화는 프로세스 전역 상태를 사용하므로, 동일 프로세스에서 서로 다른 모델
  경로/언어로 병렬 초기화를 늘릴 경우 주의가 필요하다.
