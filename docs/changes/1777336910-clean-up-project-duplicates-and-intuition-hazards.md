# 프로젝트 중복과 직관성 저해 요소 정리

## 목표

- 프로젝트 전체 점검에서 찾은 중복, 잔존 코드, 직관성을 해치는 이름을 정리한다.
- `tools/ocr_sidecar_compare/.venv` 정리는 이번 범위에서 제외한다.
- 한 번에 하나씩 변경하고, 각 단계마다 기존 동작이 무너지지 않음을 테스트로 확인한다.

## 작업 순서

1. 프런트에 남은 OCR 그룹핑 구현을 제거한다.
2. 언어 코드 정규화와 지원 언어 목록 로딩을 Rust 단일 모듈로 합친다.
3. `src/lib.rs`의 과도한 책임을 작은 모듈로 분리한다.
4. Paddle 모델 탐색 테스트용 복사 구현을 실제 모델 탐색 코드로 대체한다.
5. 더 이상 쓰지 않는 legacy FFI JSON 반환 API를 제거한다.
6. `tools/scripts/ocr_sidecar_ffi.py`의 거대한 단일 파일을 기능별 모듈로 나눈다.
7. UTF-8 BOM이 남은 소스 파일을 정리한다.
8. native OCR 코드의 `sidecar` 기준 이름을 현재 도메인 기준 이름으로 바꾼다.
9. 환경변수 key 문자열을 한곳에 모아 관리한다.

## 검증 계획

- 각 단계 후 해당 영역의 단위 테스트를 실행한다.
- Rust 변경 후 `cargo test --lib`를 실행한다.
- UI 변경 후 Deno/Vite 테스트 또는 타입 검사를 실행한다.
- Python tooling 변경 후 관련 `tools/scripts/test_*.py`를 실행한다.
- 최종적으로 `cargo test --lib`, 관련 Python 테스트, `git diff --check`, 로컬 절대 경로 노출 검사를 실행한다.

## 진행 기록

- 시작 시점 작업 트리는 clean 상태다.
- 1단계: 프런트의 잔존 OCR 그룹핑 구현과 전용 테스트를 제거했다.
  - UI는 Rust가 내려준 `groups` payload 타입만 사용한다.
  - 검증:
    - `deno task test`: 17 passed.
    - `deno task build`: 성공.
- 2단계: Rust 언어 정규화와 `shared/langs.json` 파싱을 `src/language.rs`로 단일화했다.
  - 설정 저장, Paddle 모델 선택, FFI 엔진 언어 변경이 같은 app source 정규화 규칙을 사용한다.
  - Paddle 모델 선택용 upstream source 정규화는 같은 모듈 안에 별도 함수로 유지했다.
  - 검증:
    - `cargo test --lib`: 102 passed.
- 3단계: 런타임 부트스트랩 코드를 `src/lib.rs`에서 `src/runtime_setup.rs`로 분리했다.
  - GPU 런타임 검색 경로 준비, Linux 포털 host app 등록, `.env.example` materialize 기본값 선택을 앱 명령 모듈 밖으로 옮겼다.
  - 검증:
    - `cargo test --lib`: 102 passed.
- 4단계: `src/lib.rs`의 테스트 전용 Paddle 모델 탐색 복사 구현을 제거했다.
  - 테스트는 `paddle_models`의 실제 언어별 resolver와 validator를 직접 사용한다.
  - 오래된 `det/cls/rec` 축약 폴더 fixture는 현재 공식 모델 폴더 구조 fixture로 바꿨다.
  - 검증:
    - `cargo test --lib`: 102 passed.
- 5단계: legacy FFI JSON 반환 API를 제거했다.
  - `buzhi_ocr_run_image_file` C ABI와 Rust extern 선언을 제거하고, native result API만 남겼다.
  - 해당 API만 쓰던 JSON 직렬화 helper도 함께 제거했다.
  - 검증:
    - `cargo test --lib`: 102 passed.
- 6단계: `tools/scripts/ocr_sidecar_ffi.py`의 공통 helper를 `tools/scripts/ocr_sidecar_ffi_common.py`로 분리했다.
  - repo 경로/테스트명 상수, FFI 실행 환경 구성, 이미지 목록 해석, 프로파일 로그 파싱, 벤치 요약을 별도 모듈로 옮겼다.
  - 기존 CLI 진입점과 subcommand 구성은 유지했다.
  - 검증:
    - `python3 -m py_compile tools/scripts/ocr_sidecar_ffi.py tools/scripts/ocr_sidecar_ffi_common.py`: 성공.
    - `python3 tools/scripts/ocr_sidecar_ffi.py --help`: 성공.
- 7단계: UTF-8 BOM이 남아 있던 소스 파일을 정리했다.
  - 대상: `tools/scripts/__init__.py`, `tools/scripts/ocr_sidecar_ffi.py`, `tools/scripts/release_helper.py`, `tools/scripts/test_release_helper.py`, `native/paddle_bridge/bridge.h`.
  - 검증:
    - 각 파일의 선두 byte가 BOM이 아님을 확인.
    - `python3 -m py_compile tools/scripts/ocr_sidecar_ffi.py tools/scripts/ocr_sidecar_ffi_common.py tools/scripts/release_helper.py tools/scripts/test_release_helper.py tools/scripts/__init__.py`: 성공.
    - `cargo test --lib`: 102 passed.
- 8단계: native OCR 코드의 내부 `sidecar` 기준 이름을 현재 동작 기준 이름으로 바꿨다.
  - `sort_quad_boxes_like_sidecar` -> `sort_quad_boxes_reading_order`.
  - `order_crop_box_like_sidecar` -> `order_crop_box_for_perspective_crop`.
  - rec predictor 주석의 기준도 sidecar가 아닌 PaddleOCR decode 규칙으로 표현했다.
  - 검증:
    - 기존 native 함수명 잔존 검색 결과 없음.
    - `cargo test --lib`: 102 passed.
- 9단계: 환경변수 key 문자열을 상수 위치로 모았다.
  - Rust 앱/테스트/릴리즈 smoke key를 `src/env_keys.rs`로 모았다.
  - native Paddle FFI key를 `native/paddle_bridge/bridge_env.h`로 모았다.
  - 플랫폼 표준 key(`TMPDIR`, `XDG_SESSION_TYPE`, `CARGO_MANIFEST_DIR`)와 사용자 안내 문구의 예시는 그대로 두었다.
  - 검증:
    - native `std::getenv("BUZHIDAO_PADDLE_FFI...")` 잔존 검색 결과 없음.
    - `cargo test --lib`: 성공.

## 최종 검증

- `cargo test --lib`: 102 passed.
- `deno task test` (`ui/`): 17 passed.
- `deno task build` (`ui/`): 성공.
- `python3 -m py_compile tools/scripts/ocr_sidecar_ffi.py tools/scripts/ocr_sidecar_ffi_common.py tools/scripts/release_helper.py tools/scripts/test_release_helper.py tools/scripts/__init__.py`: 성공.
- `python3 tools/scripts/ocr_sidecar_ffi.py --help`: 성공.
- FFI 단독 릴리즈 벤치 재측정(`verify-ffi`, warmups 1, iterations 3, `test.png`/`test2.png`/`test3.png`): 직전 기준 대비 median 2.2-9.3% 빠름.
- `cargo build --release`: 성공.
- release OCR smoke(`target/release/buzhidao --release-ocr-smoke`, `testdata/ocr/test.png`, source `ch`, CPU): 성공. `detections=7`, `recognized_texts=7`.
- `git diff --check`: 성공.
- BOM 재스캔: 발견 없음.
- 로컬 절대 경로 문자열 스캔: 발견 없음.
