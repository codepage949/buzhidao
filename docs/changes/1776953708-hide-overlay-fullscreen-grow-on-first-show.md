# 오버레이 최초 표시 시 전체화면 전환 플래시 제거

## 목표

- 오버레이 창이 처음 보일 때 일반 크기에서 전체화면으로 커지는 과정이 보이는 현상을 줄인다.
- 오버레이 표시 순서를 조정해 사용자가 최초 표시 플래시를 보지 않게 한다.

## 범위

- `src/platform.rs`

## 구현 계획

1. 오버레이 표시 시 런타임 fullscreen 토글이 실제로 필요한지 확인한다.
2. `tauri.conf.json`의 기본 fullscreen 상태를 유지하면서 표시 시 토글을 제거한다.
3. Rust 테스트로 회귀를 확인한다.

## 구현 결과

- 오버레이는 이미 `tauri.conf.json`에서 fullscreen 창으로 생성되므로, 표시 시점의 runtime fullscreen 토글을 제거했다.
- `place_overlay_window()`에서 `set_fullscreen(false)`를 제거해 숨겨진 오버레이를 일반 창으로 되돌리지 않게 했다.
- `show_overlay()`에서도 `set_fullscreen(true)`를 다시 호출하지 않도록 정리해, 최초 표시 때 fullscreen 전환 애니메이션이 개입하지 않게 했다.

## 검증 결과

- `cargo test` 통과

## 비고

- 이 이슈의 본질은 최초 표시 플래시의 육안 확인이므로, 자동 테스트 외에 실제 앱 실행에서 체감 개선 여부를 한 번 확인하는 것이 좋다.
