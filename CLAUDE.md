## 회고

### Tauri 투명 오버레이 창 (Windows)

투명 WebView2 창에서 마우스 이벤트가 아래 창으로 통과하는 문제가 있다.
두 가지를 모두 적용해야 한다:
1. Rust: `window.set_ignore_cursor_events(false)` 명시 호출
2. HTML: `body { background: rgba(0,0,0,0.002); }` — 픽셀 알파값 비-제로

### Tauri WebView2 서스펜드 문제

오버레이에서 `await getCurrentWindow().hide()` 후 `invoke()`를 호출하면
WebView2가 서스펜드되어 IPC가 전달되지 않는다.
오버레이 닫기 + 후속 작업은 Rust 커맨드 하나에서 일괄 처리할 것.

### tauri-plugin-global-shortcut 중복 등록

`on_shortcut`은 OS 등록 + 콜백 설정을 함께 처리한다.
같은 단축키로 `register`를 추가 호출하면 `os error 6` 패닉 발생.

### 전역 단축키 콜백에서 비동기 작업

`on_shortcut` 콜백은 Tokio 런타임 밖에서 실행된다.
`tokio::spawn` 대신 `tauri::async_runtime::spawn` 사용.

### rdev::grab + Tauri 전역 단축키 충돌 (Windows)

`tauri-plugin-global-shortcut`(RegisterHotKey)은 수식키 없는 PrintScreen을 전역 등록할 수 없다.
`rdev::grab`(WH_KEYBOARD_LL)으로 교체했으나, Tauri 기본 설정에서 창이 포커스될 때
창 메시지 큐가 raw 키 이벤트를 이중 처리해 훅과 충돌한다.

→ `.device_event_filter(tauri::DeviceEventFilter::Always)` 추가로 해결.
Tauri가 장치 이벤트를 자체 이벤트 루프에서 처리하게 해 창 메시지 큐와의 이중 처리를 막는다.

### 팝업 창 포커스와 오버레이 키 이벤트 단절

팝업 창을 표시하며 `set_focus()`를 호출하면 포커스가 팝업으로 이동한다.
이후 오버레이 창의 `keydown` 이벤트 리스너는 이벤트를 받지 못해 ESC 등 키 처리가 동작하지 않는다.

→ 팝업 창에도 동일한 키 핸들러를 추가해야 한다.
특히 오버레이·팝업을 함께 닫는 동작(ESC)은 두 창 모두에서 처리할 것.

### 팝업 닫기 동작 범위

팝업의 닫기 버튼과 ESC는 팝업만 닫아야 한다.
오버레이 유지가 필요한 흐름에서 `close_overlay`를 재사용하면 의도치 않게 전체 UI가 사라진다.

→ 팝업 전용 닫기 동작은 별도 Rust 커맨드로 분리하고,
필요하면 `popup.hide()` 후 `overlay.set_focus()`로 포커스를 복구할 것.

### Docker 실행 구성과 테스트 경로 분리

사용자가 "컨테이너로 올라간 프로그램 대상 테스트"를 원할 때
실행용 `docker-compose`에 테스트 서비스를 섞지 말 것.

→ 배포/실행 Compose는 그대로 두고,
호스트 스크립트나 별도 테스트 진입점으로 실행 중인 컨테이너의 HTTP 엔드포인트를 검증할 것.
