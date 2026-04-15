# overlay 창 fullscreen 적용

## 변경

- `app/tauri.conf.json`의 overlay 창에 `fullscreen: true` 추가
- 고정 크기/좌표(`width`, `height`, `x`, `y`) 제거
  - fullscreen 모드에서는 OS가 전체 화면 크기를 강제하므로 불필요

## 기대 효과

- 모니터 해상도와 무관하게 overlay가 항상 전체 화면을 덮는다.
- Windows/macOS/Linux 모두에서 동일하게 동작한다.
