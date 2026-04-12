# screenshots → xcap 마이그레이션

## 배경

`screenshots` crate가 개발 중단되고 `xcap`으로 이관됨.
`xcap`은 Linux(X11/Wayland), macOS, Windows를 모두 지원하는 후속 라이브러리.

## 변경 사항

### Cargo.toml

- `screenshots = "0.8"` → `xcap = "0.9"`
- `image = "0.24"` → `image = "0.25"` (xcap이 image 0.25 사용)

### src/services.rs — `capture_screen()`

| 항목 | 이전 (`screenshots`) | 이후 (`xcap`) |
|------|----------------------|---------------|
| 모니터 열거 | `screenshots::Screen::all()` | `xcap::Monitor::all()` |
| 캡처 | `screen.capture()` → raw bytes | `monitor.capture_image()` → `RgbaImage` 직접 반환 |
| 위치 | `screen.display_info.x/y` | `monitor.x()` / `monitor.y()` |
| 이미지 변환 | `RgbaImage::from_raw(w, h, bytes)` 필요 | 불필요 (이미 `RgbaImage`) |

## Linux 시스템 의존성

xcap이 Linux에서 추가로 요구하는 패키지:

```bash
sudo apt install libpipewire-0.3-dev libgbm-dev
```
