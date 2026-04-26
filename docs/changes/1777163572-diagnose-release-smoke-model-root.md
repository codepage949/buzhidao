# 릴리즈 OCR smoke 모델 루트 진단 보강

## 배경

`linux-amd64-cpu` 릴리즈 verify에서 OCR smoke가 모델 보장 후 FFI warmup 단계에서 실패했다.

실패 메시지는 Paddle predictor 실행 오류가 아니라 C++ FFI가 전달받은 모델 루트 아래에서
det/cls/rec 추론 모델을 찾지 못했을 때 발생하는 메시지였다.

## 결정

- release smoke는 FFI 엔진 생성 전에 Rust가 보장한 모델 루트의 det/cls/rec 상태를 검증한다.
- 검증 실패 시 어떤 모델 디렉터리와 어떤 파일이 누락됐는지 에러에 포함한다.
- FFI trace는 smoke 기본 동작을 바꾸지 않도록 외부에서 명시적으로 켤 때만 사용한다.
- C++ FFI에서는 모델 파일 탐색을 마친 뒤에 Paddle `Config`를 포함한 engine 객체를 생성한다.
- Linux trace 로그 파일 경로 생성은 `std::filesystem::temp_directory_path()` 대신 `TMPDIR`/`/tmp` 문자열 경로를 사용한다.

## 구현

- `src/paddle_models.rs`
  - 언어별 모델 루트 검증 함수를 추가한다.
  - det/cls/rec 각 모델 디렉터리와 `inference.json` 또는 `inference.pdmodel`, `inference.pdiparams` 존재 여부를 진단한다.
- `src/services/ocr_pipeline.rs`
  - release smoke에서 모델 보장 직후 모델 루트를 검증한다.
- `native/paddle_bridge/bridge.cc`
  - `buzhi_ocr_engine` 생성을 det/cls/rec 모델 pair 탐색 이후로 늦춘다.
  - Linux 모델 탐색 경로의 파일/디렉터리 확인과 1단계 하위 디렉터리 열거는 POSIX API로 수행한다.
  - trace/profile 로그 파일 생성에서 `std::filesystem::temp_directory_path()` 의존을 제거한다.

## 검증 결과

- 모델 루트 검증 성공/실패 단위 테스트를 추가한다.
- release smoke 주변 단위 테스트를 실행한다.
- `cargo test -p buzhidao --lib paddle_models`
  - 결과: 7개 테스트 통과.
- `cargo test -p buzhidao --lib 릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다`
  - 결과: 1개 테스트 통과.
- `python -m unittest tools.scripts.test_release_workflow`
  - 결과: 1개 테스트 통과.
- Docker `ubuntu:24.04` 컨테이너에서 Linux CPU FFI smoke를 실행했다.
  - trace 비활성 조건 결과: release OCR smoke 통과.
  - `BUZHIDAO_PADDLE_FFI_TRACE=1` 조건 결과: release OCR smoke 통과.
- `git diff --check`
  - 결과: 통과.
