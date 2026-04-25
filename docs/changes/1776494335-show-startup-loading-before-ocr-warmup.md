# 시작 로딩 창을 OCR warmup보다 먼저 표시

## 구현 계획

- 시작 시 OCR 엔진 생성과 warmup 호출 순서를 확인한다.
- 로딩 창이 보이기 전에 동기 warmup이 실행되는 경로를 제거한다.
- 엔진 생성과 warmup 책임을 분리해 기존 비동기 warmup 경로가 시작 로딩 창을 먼저 표시하도록 정리한다.
- 관련 테스트와 컴파일로 회귀가 없는지 확인한다.

## 변경 사항

- `src/ocr/paddle_ffi.rs`와 관련 호출 경로를 수정해 시작 시 로딩 창이 OCR warmup보다 먼저 표시되도록 변경한다.
- 원인은 `PaddleFfiEngine::new()`가 내부에서 즉시 `warmup()`를 호출해 `setup` 단계에서 동기적으로 블로킹되던 구조였다.
- `PaddleFfiEngine::new()`에서는 엔진 상태만 구성하고 바로 반환하도록 변경했다.
- 실제 warmup은 기존 시작 비동기 경로의 `engine.warmup()`에서만 수행되도록 유지해, `show_loading_window(...)`가 먼저 실행될 수 있게 했다.
- 이에 맞춰 생성 시 즉시 warmup하지 않는다는 테스트를 추가했다.

## 테스트 계획

- 엔진 생성 시 즉시 warmup하지 않는 동작을 테스트한다.
- `cargo test`와 `cargo check`로 검증한다.

## 테스트 결과

- `cargo test paddle_ffi_엔진_생성은_warmup_없이도_성공한다 -- --nocapture` 통과
- `cargo test paddle_ffi_엔진_생성후_raw_상태는_아직_비어있다 -- --nocapture` 통과
- `cargo check` 통과
- 이번 검증은 공통 Rust 코드 경로와 테스트 기준이며, Windows/macOS/Linux 실제 GUI 실행 육안 확인은 별도다.
