# overlay fullscreen 동작 멀티플랫폼 통일

## 구현 계획

- 현재 overlay 표시 로직에서 플랫폼별 fullscreen 동작 차이가 생기는 지점을 확인한다.
- 특별한 제약이 없다면 Linux도 다른 플랫폼과 같은 fullscreen 기준으로 표시하도록 정리한다.
- 플랫폼 공통으로 유지할 수 있는 최소 테스트를 추가하거나 기존 테스트를 보강한다.
- 컴파일 및 관련 테스트로 변경이 깨지지 않는지 확인한다.

## 변경 사항

- `src/platform.rs`의 overlay 표시 로직을 수정해 플랫폼별로 다른 fullscreen 동작을 통일한다.
- Linux에서 top bar가 남는 원인이 되는 수동 모니터 크기 배치 경로를 제거하거나 축소한다.
- Linux 전용 수동 모니터 배치 분기를 제거하고, 모든 플랫폼에서 `place_overlay_window(...)` 후 `set_fullscreen(true)`를 요청하도록 통일했다.
- overlay fullscreen 정책을 `should_force_overlay_fullscreen()`로 분리해 테스트 가능한 형태로 정리했다.
- 그 결과 Linux에서도 overlay가 작업 영역이 아니라 fullscreen 기준으로 표시되도록 맞췄다.

## 테스트 계획

- overlay 표시 정책 판단 로직에 대한 단위 테스트를 추가한다.
- `cargo test`와 `cargo check`로 검증한다.

## 테스트 결과

- `cargo test overlay는_플랫폼과_무관하게_fullscreen을_요청한다 -- --nocapture` 통과
- `cargo test 기본_capture_shortcut_accelerator는_파싱된다 -- --nocapture` 통과
- `cargo check` 통과
