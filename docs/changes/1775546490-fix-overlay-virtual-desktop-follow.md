# 오버레이 가상 데스크톱 고정

## 구현 목적

`PrintScreen` 시점에 표시된 오버레이가 이후 Windows 가상 데스크톱 전환에도 따라오고 있다.

- 오버레이는 캡처 시점의 데스크톱과 모니터 위치에 고정되어야 한다.
- 데스크톱 전환에 따라 현재 데스크톱으로 다시 따라오지 않게 한다.
- 기존 OCR 오버레이 흐름과 팝업 동작은 유지한다.

## 구현 계획

1. 오버레이 표시 시 사용 중인 전체 화면 설정과 창 배치 지점을 확인한다.
2. 캡처 시점 모니터 좌표를 확보하고 오버레이를 해당 영역에 직접 배치한다.
3. 창 배치 헬퍼 테스트를 추가한다.
4. `cargo test`로 회귀를 확인한다.

## 구현 사항

- `app/src/services.rs`의 `CaptureInfo`에 캡처 시점 모니터 좌표 `x`, `y`를 추가했다.
- `app/src/lib.rs`에서 오버레이 표시 시 `set_fullscreen(true)` 호출을 제거했다.
- `app/src/window.rs`에 오버레이를 캡처 시점 모니터 영역으로 직접 배치하는 헬퍼를 추가했다.
- 오버레이는 이제 borderless topmost 창을 해당 모니터 위치와 크기에 맞춰 표시한다.
- `app/tauri.conf.json`에서 오버레이의 `skipTaskbar`를 꺼 Windows tool window 성격을 줄였다.
- 실제 확인 결과 원인은 `fullscreen`이 아니라 `skipTaskbar`였고, `skipTaskbar: false`를 유지한 채 `set_fullscreen(true)`는 다시 복구했다.
- `app/README.md`에 Windows 가상 데스크톱 전환과 관련된 오버레이 배치 방식을 문서화했다.

## 테스트 결과

- `cargo test`
  - Rust 테스트 10개 통과
  - 오버레이 배치 헬퍼 테스트 추가 포함

## 추가 검토

- 이번 변경은 `fullscreen` 제거와 모니터 영역 직접 배치에 집중했다.
- 실제 확인으로 `skipTaskbar`가 핵심 요인이었고, `fullscreen`은 재사용 가능하다는 점을 확인했다.
- 재현이 다시 생기면 다음 단계로 Win32 가상 데스크톱 API를 붙여 데스크톱 GUID 단위 고정을 검토할 수 있다.
