# PrtSc 핫키 grab 실패 시 에러 로깅 추가

## 문제

Linux에서 `rdev::grab`(`unstable_grab` = evdev 방식)은
`input` 그룹 권한 없이 실행하면 조용히 실패했다.

실패 시 `let _ = grab(...)` 으로 에러를 무시해 원인 파악이 불가능했다.

## 변경 사항

### src/platform.rs — `install_capture_shortcut()`

- `let _ = grab(...)` → `if let Err(e) = grab(...) { eprintln!(...) }`
- grab 실패 원인이 stderr에 출력되도록 수정

## Linux 권한 요구사항

`rdev::grab`(evdev)은 `/dev/input/event*` 접근 권한이 필요하다.

```bash
sudo usermod -aG input $USER
newgrp input   # 재로그인 없이 즉시 적용
```
