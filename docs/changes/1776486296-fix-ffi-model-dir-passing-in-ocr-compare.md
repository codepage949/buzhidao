# compare_ocr_sidecar_ffi FFI 모델 경로 전달 수정

## 배경

- `uv run python scripts/compare_ocr_sidecar_ffi.py` 실행 시 FFI 샘플 테스트가 `model_dir is empty`로 실패한다.
- `PaddleFfiEngine`는 모델 경로와 source를 FFI 함수 인자로 넘기지 않고 환경 변수에 저장한 뒤 C++ 브리지에서 `getenv()`로 읽는다.
- 이 우회 방식은 Windows FFI 경계에서 불안정하며, 비교 스크립트처럼 테스트 프로세스 안에서 바로 엔진을 생성하는 경로에서 실패할 수 있다.

## 변경 계획

- Rust `PaddleFfiEngine`가 모델 경로와 source를 `buzhi_ocr_create()`에 직접 전달하도록 수정한다.
- 비교 스크립트 재현 명령과 관련 테스트로 회귀를 확인한다.

## 구현 내용

- `app/src/ocr/paddle_ffi.rs`
  - `create_engine_locked()`에서 `BUZHIDAO_PADDLE_FFI_MODEL_DIR`, `BUZHIDAO_PADDLE_FFI_SOURCE` 환경 변수 우회 설정을 제거했다.
  - 모델 경로와 source를 `CString`으로 변환한 뒤 `buzhi_ocr_create()`에 직접 전달하도록 변경했다.
  - 경로 또는 source에 NUL 바이트가 있을 때 명확한 오류를 반환한다.
- `app/build.rs`
  - Paddle Inference 런타임 공유 라이브러리(`.dll`, `.so`, `.dylib`)를 공통 규칙으로 수집해 Cargo 프로필 디렉터리와 `deps` 디렉터리로 스테이징하도록 추가했다.
  - `lib`, `third_party/install/mklml/lib`, `third_party/install/onednn/lib`, `third_party/install/openvino/intel64`와 그 하위 공유 라이브러리 디렉터리를 재귀적으로 탐색한다.
  - 이로써 테스트 바이너리와 앱 바이너리가 실행 시점에 같은 런타임 라이브러리 배치를 사용한다.

## 검증 결과

- `uv run python scripts/compare_ocr_sidecar_ffi.py`
  - 통과. 기본 샘플 4개 비교 결과가 JSON으로 출력되었다.
- `cargo test -p buzhidao --features paddle-ffi -- --nocapture --exact "ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다"`
  - 통과. 테스트 바이너리가 정상 기동되며 DLL 로더 단계에서 더 이상 실패하지 않는다.
- `BUZHIDAO_RUN_FFI_SAMPLE_TEST=1 BUZHIDAO_FFI_TEST_IMAGE=app/testdata/ocr/test-sidecar-check.bmp cargo test -p buzhidao --features paddle-ffi -- --nocapture --exact "ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다"`
  - 통과. 실제 FFI OCR이 실행되어 detection/debug 출력까지 확인했다.
