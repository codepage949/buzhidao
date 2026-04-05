# src/lib.rs 리팩토링

## 리팩토링 목적

`src/lib.rs` 단독으로 점검한 결과 다음 두 지점의 정리가 필요했다.

- 팝업 위치 계산 로직이 `calc_popup_pos_from_screen`와 `calc_popup_pos`에 중복돼 있다.
- 창 hide/focus 처리 로직이 `close_overlay`, `close_popup`, `handle_prtsc`에 반복된다.

이번 리팩토링은 파일 분해 없이, 중복 제거와 보조 함수 도입으로 유지보수성을 높이는 범위로 제한한다.

## 리팩토링 계획

1. 창 hide/focus 반복을 작은 헬퍼 함수로 정리한다.
2. 이미 분리된 순수 함수와 테스트 구조는 유지하되, 추가 구조 변경이 안전한지 확인한다.
3. `cargo test`, `cargo check`로 회귀가 없는지 확인한다.

## 리팩토링 사항

- `hide_window(app, label)` 헬퍼를 추가해 창 숨김 반복을 제거했다.
- `focus_window(app, label)` 헬퍼를 추가해 포커스 복구 반복을 제거했다.
- `close_overlay`, `close_popup`, `handle_prtsc`가 공통 헬퍼를 재사용하도록 정리했다.
- `calc_popup_pos_from_screen`와 테스트는 이미 분리돼 있었고, 이번에는 창 제어 중복 제거에 우선순위를 두었다.

## 테스트 결과

- `cargo test`
  - Rust 테스트 4개가 모두 통과했다.
- `cargo check`
  - 컴파일 검증이 통과했다.

## 추가 검토

- `calc_popup_pos` 본문은 여전히 `calc_popup_pos_from_screen`와 중복이 남아 있어 후속 리팩토링 후보다.
- 이번 턴에서는 주석 인코딩이 섞인 파일에서 넓은 구조 변경보다, 회귀 위험이 낮은 창 제어 중복 제거를 우선 적용했다.
