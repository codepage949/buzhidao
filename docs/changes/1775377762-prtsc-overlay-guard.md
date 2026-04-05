# PrtSc 중복 입력 방지 및 오버레이 중 무시

## 변경 목적

- PrtSc를 짧은 순간에 여러 번 눌러도 첫 번째 키에만 동작
- 오버레이가 표시된 동안 PrtSc 입력을 억제(None 반환)하고 무시

## 기존 동작

`grab` 콜백에서 PrtSc를 항상 `handle_prtsc`로 스폰하고, 내부에서 `busy.swap(true)`로
이미 처리 중인 경우만 차단. OCR 완료 후 `busy=false`가 되면 오버레이가 떠있어도
다음 PrtSc가 새 캡처를 시작하는 문제 있음.

## 변경 내용

`grab` 콜백에서 스폰 직전에 두 조건을 추가 확인:

1. `overlay.is_visible()` → `true`면 억제(None)만 하고 스폰하지 않음
2. `busy.load(SeqCst)` → `true`면 마찬가지로 스폰하지 않음

OS 기본 동작(스크린샷 저장)은 어느 경우에도 항상 `None`으로 차단.

## 변경 파일

- `src-tauri/src/lib.rs` — `run()` 내 grab 콜백 수정
