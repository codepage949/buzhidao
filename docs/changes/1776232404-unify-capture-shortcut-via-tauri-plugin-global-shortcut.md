# 캡처 단축키를 tauri-plugin-global-shortcut으로 통일

## 배경

기존 구현은 PrtSc 키를 전역 단축키로 감지하기 위해 두 가지 경로를 병행했다.

- Windows/macOS: `rdev::grab`(`WH_KEYBOARD_LL` 저수준 훅)
- Linux: `evdev_rs`로 `/dev/input/eventN`을 직접 감시

이 구조는 다음 문제가 있다.

1. Linux 데스크톱(GNOME/KDE)이 PrtSc를 시스템 스크린샷으로 선점하므로
   `evdev` 감시는 동작해도 UX상 OS 기본 동작과 충돌한다. Wayland에서는 /dev/input
   접근 권한과 보안 정책 때문에 더욱 불안정하다.
2. `rdev::grab`과 Tauri 창 메시지 큐가 충돌하여 `.device_event_filter(DeviceEventFilter::Always)`
   같은 우회가 필요했다.
3. 단축키를 사용자가 커스터마이징할 수 없다.

## 결정

전역 단축키 처리를 `tauri-plugin-global-shortcut`으로 일원화한다.
기본 조합은 플랫폼별로 다음과 같이 정한다.

- Windows / Linux: `Ctrl+Alt+A`
- macOS: `Cmd+Shift+A`

사용자는 설정 화면에서 임의의 조합(Accelerator 문자열)으로 변경할 수 있다.
PrtSc 같은 수식키 없는 단일 키는 OS API가 거부하므로 문서에서 권장하지 않는다.

## 변경 사항

### 크레이트
- `app/Cargo.toml`
  - 제거: `rdev`, Linux 전용 `evdev-rs`
  - 추가: `tauri-plugin-global-shortcut = "2"`

### 런타임
- `app/src/platform.rs`
  - `install_rdev_capture_shortcut`, `install_linux_capture_shortcut`,
    `watch_linux_input_device`, `is_capture_shortcut_pressed`,
    `is_linux_capture_shortcut_event` 전부 제거.
  - `install_capture_shortcut(app, busy, shortcut, on_trigger)`가
    플러그인으로 단일 Accelerator를 등록한다.
  - 설정 저장 시 `replace_capture_shortcut(...)`로 기존 단축키를 해제하고 새 단축키를 즉시 재등록한다.
  - 재등록이나 후속 저장이 실패하면 기존 단축키로 롤백한다.
  - 콜백은 `tauri::async_runtime::spawn`으로 async 작업을 띄운다.
  - 디버그 로그는 `SHORTCUT_DEBUG=1`일 때 플러그인 이벤트 한 줄만 찍는다.
- `app/src/lib.rs`
  - `tauri_plugin_global_shortcut::Builder::new().build()` 플러그인 등록.
  - `.device_event_filter(DeviceEventFilter::Always)` 제거(rdev 의존 제거로 불필요).
  - 설정에서 읽은 `capture_shortcut`을 `install_capture_shortcut`에 전달.

### 설정
- `app/src/config.rs`
  - `Config.capture_shortcut: String` 필드 추가.
  - `CAPTURE_SHORTCUT` 환경변수, 빈 값/미지정이면 플랫폼별 기본값.
- `app/src/settings.rs`
  - `UserSettings.capture_shortcut` 추가, trim + 빈값 시 기본값으로 복원.
  - 저장 전 `Shortcut::from_str`로 Accelerator 유효성을 검증하고, 잘못된 값은 저장을 거부.
  - `.env`의 `CAPTURE_SHORTCUT` 관리 키에 포함.
- `app/.env.example`
  - `CAPTURE_SHORTCUT=` 라인 추가 + 주석.

### 캐퍼빌리티
- `app/capabilities/default.json`
  - `global-shortcut:allow-register`, `global-shortcut:allow-unregister`,
    `global-shortcut:allow-is-registered` 권한 추가
    (플러그인은 프론트 호출 없어도 런타임 등록엔 필요 없지만, 향후 프론트 검증용으로 포함).

### UI
- `app/ui/src/settings.tsx`
  - `capture_shortcut` 입력 필드(텍스트) 추가, 저장 시 서버로 전달.
  - 안내 문구에 "Ctrl/Alt/Shift/Cmd 수식키를 포함한 조합을 권장합니다" 추가.
  - 저장 후 앱 재시작 없이 즉시 반영된다는 점을 명시.
- `README.md`
  - 기본 캡처 단축키 설명을 `PrintScreen`에서 플랫폼별 기본 Accelerator로 갱신.
  - 환경 변수 목록에 `CAPTURE_SHORTCUT`를 추가.

## 테스트

- 단위 테스트:
  - `config.rs`: `CAPTURE_SHORTCUT` 미설정 시 플랫폼별 기본값을 반환.
  - `settings.rs`: 빈 `capture_shortcut`은 기본값으로 복원.
  - `settings.rs`: 잘못된 `capture_shortcut`은 검증 단계에서 거부.
  - `settings.rs`: `save_to_env_file`이 `CAPTURE_SHORTCUT` 키를 관리.
  - `platform.rs`: 기본값/예시 Accelerator 문자열이 파싱되는지 확인.
- 실행 확인:
  - `cd app && cargo test`
  - `cd app/ui && deno test`
- 수동 검증:
  - Windows에서 `Ctrl+Alt+A`로 캡처가 트리거되는지.
  - 설정 화면에서 다른 조합으로 변경 후 즉시 반영되는지.

## 비범위

- 프론트에서 키 입력을 받아 조합을 만드는 UX는 차후 이슈.
- Wayland에서 동작하지 않을 경우의 포털 기반 fallback도 차후 이슈.

## 회고 반영 예정 (CLAUDE.md)

- rdev::grab 제거로 `.device_event_filter(Always)` 우회가 더 이상 필요 없다.
- Linux에서 PrtSc를 앱 단에서 오버라이드하려는 시도는 포기했다.
  GNOME/KDE가 PrtSc를 선점하며 Wayland 컴포지터는 저수준 키 훅 자체를 차단한다.
  대안은 수식키 조합(`Ctrl+Alt+A` 등) + `tauri-plugin-global-shortcut`이다.
