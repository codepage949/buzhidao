# 설정 창을 실제로 닫고 다시 생성

## 배경

현재 설정 창은 닫기 버튼, ESC, 저장 완료 후 모두 `hide()`로만 처리한다.
이 때문에 사용자가 저장하지 않은 입력값이나 저장 실패 후 수정 중이던 값이
React 상태에 남아, 다시 설정 창을 열어도 현재 저장된 설정 대신 이전 화면 상태가 그대로 보인다.

## 결정

설정 창은 더 이상 숨기지 않고 실제로 `close()`한다.
다음에 설정 창을 열 때는 Tauri 설정에 정의된 `settings` 윈도우를 Rust에서 다시 생성한다.

창을 새로 생성해도 필수 설정 누락 안내 같은 초기 알림이 유지되도록,
설정 창용 notice payload는 앱 상태에 잠시 저장했다가 `get_user_settings` 응답에 포함해
프런트가 최초 렌더 시 함께 반영하도록 한다.

## 변경 사항

### 런타임
- `app/src/lib.rs`
  - `settings` 윈도우를 필요 시 다시 생성하는 `ensure_settings_window(...)`,
    `open_settings_window(...)` 헬퍼를 추가.
  - 트레이 메뉴와 필수 설정 누락 안내가 모두 새 헬퍼를 통해 설정 창을 연다.
  - 새로 생성되는 설정 창에도 초기 notice를 전달할 수 있도록 pending notice 상태를 추가.
  - `get_user_settings` 응답에 optional `notice`를 포함한다.

### UI
- `app/ui/src/settings.tsx`
  - 창 닫기 요청을 더 이상 `preventDefault + hide()`하지 않는다.
  - ESC와 저장 성공 후 동작을 `hide()`에서 `close()`로 변경한다.
  - 초기 설정 로드 시 `get_user_settings`가 내려준 `notice`를 함께 반영한다.

## 테스트

- `cd app && cargo test`
- `cd app/ui && deno test`

## 수동 검증

- 설정 창에서 값을 수정만 하고 저장하지 않은 뒤 닫고 다시 열면 저장된 현재 값으로 다시 표시되는지.
- 잘못된 캡처 단축키를 입력해 저장 실패시킨 뒤 닫고 다시 열면 실패 직전 입력값이 아니라 현재 저장값이 보이는지.
- 필수 설정 누락으로 설정 창이 자동 오픈될 때 안내 문구와 강조 필드가 정상 반영되는지.
