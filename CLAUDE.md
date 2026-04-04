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
