# Linux Wayland 포털 캡처 경로 추가

## 배경

Wayland, 특히 GNOME Wayland 환경에서는 `xcap`의 Wayland 캡처 경로가
`wlroots` screencopy 프로토콜에 의존해
`Cannot find required wayland protocol` 오류로 실패한다.

단축키는 정상 동작하더라도 실제 화면 캡처 단계에서 막혀
OCR 흐름을 진행할 수 없다.

## 구현 계획

1. Linux Wayland 세션을 감지해 기존 `xcap` 경로와 분기한다.
2. Wayland에서는 `xdg-desktop-portal ScreenCast`로 PipeWire 첫 프레임을 가져온다.
3. 캡처 결과를 `CaptureInfo`로 변환해 기존 OCR 흐름과 연결한다.
4. `restore_token`을 저장해 승인 반복을 줄인다.
5. 세션 감지와 프레임 포맷 변환을 테스트한다.

## 변경 대상

- `Cargo.toml`
  - Wayland 포털 및 PipeWire 의존성 추가
- `src/services.rs`
  - Linux Wayland 분기 추가
  - 포털 스크린샷 호출 및 이미지 로드 구현
  - 세션 감지 및 URI 파싱 헬퍼 추가
- 테스트
  - 핵심 분기/파싱 로직 검증

## 참고

- `org.freedesktop.portal.Screenshot`
- `ashpd` crate의 Screenshot 요청 API

## 구현 내용

### `Cargo.toml`

- `ashpd`를 `tokio`, `screencast` 기능과 함께 사용
- `pipewire` 추가

### `src/services.rs`

- `capture_screen()`을 async로 변경
- Linux Wayland 세션에서는 `ScreenCast`를 우선 사용
- `create_session -> select_sources -> start -> open_pipe_wire_remote` 순서로 세션을 연다
- PipeWire 첫 비디오 프레임을 받아 `RGBA` 이미지로 변환한다
- 지원 포맷은 우선 `RGBA`, `RGBx`, `BGRx`
- 성공한 `ScreenCast` 세션의 `restore_token`을 `~/.config/buzhidao/wayland-screencast-token`에 저장한다
- 다음 실행에서는 저장된 토큰으로 먼저 재사용을 시도하고, 실패 시 토큰을 폐기한 뒤 새 세션을 연다
- Linux Wayland가 아니면 기존 `xcap` 캡처를 `spawn_blocking`으로 유지
- Wayland 세션 감지와 PipeWire 프레임 변환 헬퍼 추가

### `src/lib.rs`

- Linux 실행 시 host portal registry 등록과 screenshot permission 상태 로그 추가
- `handle_prtsc()`가 async `capture_screen()`을 직접 await하도록 유지

## 테스트

- `wayland_세션이면_포털_캡처를_사용한다`
- `bgrx_프레임을_rgba_이미지로_변환한다`
- `restore_token_저장_경로는_config_dir_아래다`
- `캡처_단축키는_linux_sysrq_키코드도_감지한다`

## 현재 제약

- Wayland ScreenCast는 첫 승인 시 화면 공유 선택/허용 UI가 나타날 수 있다.
- 현재 구현은 첫 프레임 한 장만 사용한다.
- PipeWire 포맷은 현재 `RGBA`, `RGBx`, `BGRx`만 지원한다.
- GNOME Wayland에서 `org.gnome.Shell.Screenshot` 직접 호출은 이 환경 기준 `AccessDenied`로 막혀 단기 우회로로 사용할 수 없다.
