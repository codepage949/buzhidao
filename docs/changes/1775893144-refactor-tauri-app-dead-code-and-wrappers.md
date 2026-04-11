# Tauri 앱 dead code · 불필요한 래퍼 정리

## 목적

- 중복·미사용 코드 제거
- 성능 개선 (reqwest 커넥션 풀 공유)
- 잘못된 이름/불필요한 추상 계층 제거

---

## 변경 항목

### 1. `reqwest::Client` 공유 — 성능

`call_ai`(services.rs)가 매 호출마다 `reqwest::Client::new()`를 생성하고 있었다.
`Client`는 내부 커넥션 풀을 가지므로 앱 수명 동안 공유해야 한다.

- `AppState`(또는 Tauri `manage`)에 `reqwest::Client`를 등록
- `call_ai` 시그니처를 `client: &reqwest::Client`를 받도록 변경
- `lib.rs` setup에서 `Client::new()`로 생성해 등록

### 2. `predict_with_tiles` 인라인 — 중복·오해 소지 이름

`predict_with_tiles`는 타일링을 전혀 수행하지 않는다(이전 구현에서 남은 이름).
`run_ocr` 내부에서만 호출되므로 인라인했다.

- `services.rs`에서 `predict_with_tiles` 함수 제거
- `run_ocr` 내에 det → recognize 로직 직접 작성

### 3. `capture_active_screen` 래퍼 제거 — 불필요한 추상

`platform.rs`의 `capture_active_screen`은 `capture_screen()`을 그대로 호출하는
한 줄 래퍼였다. 추상 계층 없이 직접 호출한다.

- `platform.rs`에서 `capture_active_screen` 제거
- `lib.rs`에서 `services::capture_screen` 직접 사용

### 4. 죽은 `scale` 필드 제거 — dead code

`OcrResultPayload.scale`은 항상 `1.0`으로 고정(services.rs:89).
프론트엔드(overlay.tsx)도 이 값을 좌표에 곱하지만 항등 연산이다.

- `OcrResultPayload`에서 `scale: f64` 필드 제거
- `overlay.tsx`에서 `scale` 변수 및 관련 곱셈 제거

### 5. 죽은 `box_h` 파라미터 제거 — dead code

`calc_popup_pos_from_screen`(popup.rs)의 `box_h` 파라미터는
`let _ = box_h`로 명시적으로 무시된다.

- `calc_popup_pos_from_screen`, `calc_popup_pos` 파라미터에서 `box_h` 제거
- `lib.rs`의 `select_text` 커맨드에서 `box_h` 전달 제거
- `overlay.tsx`의 `invoke("select_text", ...)` 호출에서 `boxH` 제거

### 6. `overlay_target_bounds` 인라인 — 불필요한 추상

`overlay_target_bounds`(window.rs)는 `place_overlay_window`에서만 한 번 호출되는
private 함수로, 단순 struct 생성만 담당한다.

- `place_overlay_window` 내부에 인라인
- `overlay_target_bounds` 함수 제거

---

## 파일별 변경 요약

| 파일 | 변경 내용 |
|------|-----------|
| `src/services.rs` | `predict_with_tiles` 제거·인라인, `call_ai` 시그니처 변경 |
| `src/platform.rs` | `capture_active_screen` 제거 |
| `src/window.rs` | `overlay_target_bounds` 인라인 |
| `src/popup.rs` | `box_h` 파라미터 제거 |
| `src/lib.rs` | `capture_screen` 직접 사용, `Client` 관리, `box_h` 전달 제거 |
| `ui/src/overlay.tsx` | `scale` 변수 제거, `boxH` 제거 |
