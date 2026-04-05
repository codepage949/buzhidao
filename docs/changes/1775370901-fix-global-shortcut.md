# 전역 단축키 수정 — RegisterHotKey → WH_KEYBOARD_LL (rdev)

## 문제

`tauri-plugin-global-shortcut`은 내부적으로 Windows `RegisterHotKey` API를 사용한다.
이 API는 **수식키(modifier) 없는 단독 키**(PrintScreen 등)를 전역으로 등록할 수 없다.
결과적으로 프로그램 창이 포커스된 경우에만 단축키가 동작하는 것처럼 보였다.
(창이 포커스되면 키 이벤트가 메시지 루프로 직접 전달되므로 우연히 동작함.)

## 해결

1. `rdev` 크레이트(`unstable_grab` 피처)로 교체.
   `rdev::grab`은 `SetWindowsHookEx(WH_KEYBOARD_LL, ...)`을 사용해 포커스와 무관하게 전역 인터셉트하며,
   콜백에서 `None`을 반환하면 OS 기본 동작(Windows 내장 캡처 등)도 차단한다.

2. `.device_event_filter(tauri::DeviceEventFilter::Always)` 추가.
   Tauri 기본값(`WhenFocused`)에서는 창이 포커스될 때 창 메시지 큐도 raw 키 이벤트를 처리하려 해
   rdev 훅과 충돌한다. `Always`로 설정하면 Tauri가 장치 이벤트를 자체 이벤트 루프에서 처리해
   창 메시지 큐와의 이중 처리가 사라진다.

## 변경 사항

### 제거
- `tauri-plugin-global-shortcut` 의존성
- `capabilities/default.json`의 `global-shortcut:*` 권한

### 추가
- `rdev = { version = "0.5.3", features = ["unstable_grab"] }` 의존성

### 수정 (`src-tauri/src/lib.rs`)
- `tauri_plugin_global_shortcut` 임포트 제거
- `.plugin(tauri_plugin_global_shortcut::Builder::new().build())` 제거
- `on_shortcut` 콜백 대신 `std::thread::spawn` + `rdev::grab`으로 키보드 훅 등록
- `.device_event_filter(tauri::DeviceEventFilter::Always)` 추가: Tauri가 장치 이벤트를 자체 이벤트 루프에서 처리하도록 해 창 메시지 큐가 raw 키 이벤트를 이중으로 처리하는 것을 막아 rdev WH_KEYBOARD_LL 훅과의 충돌 제거

## 아키텍처

```
[std::thread::spawn → rdev::grab (WH_KEYBOARD_LL 훅)]
    ↓ EventType::KeyPress(Key::PrintScreen)
    ├─ tauri::async_runtime::spawn → handle_prtsc
    └─ return None   → OS 기본 동작 차단 (Windows 스크린샷 저장 등)
[나머지 키 이벤트 → Some(event) → OS에 그대로 전달]
    ↓
(기존 흐름과 동일)
```

`rdev::grab`은 블로킹 루프이므로 별도 스레드에서 실행한다.
