# Deno CLI → Tauri 데스크톱 앱 마이그레이션 및 구현

## 개요

Deno 기반 CLI 앱을 Tauri 2.x 데스크톱 앱으로 전환하고, 전체 기능 흐름을 완성한다.

## 변경 사항

### 제거
- Telegram 연동 전체 (`src/telegram.ts`, `main.ts` 내 Telegram 코드)
- Deno FFI 기반 키보드 훅 (`user32.dll` 직접 호출)
- `main.ts`, `deno.lock` (Tauri 앱 구조로 대체)

### 추가
- **Tauri 2.x** 앱 구조 (`src-tauri/`, `src/`, `vite.config.ts`)
- **전역 단축키** (`tauri-plugin-global-shortcut`): PrtSc 키 감지
- **스크린샷** (`screenshots` crate): OCR 전용 캡처 (오버레이 배경 표시 없음)
- **투명 오버레이 창**: 전체화면 반투명 레이어 위에 OCR 박스 표시 및 선택
- **메인 창**: 번역 결과 표시

### 유지
- Python OCR 서버 (`server/`) 전체
- 텍스트 그루핑 로직 (`src/detection.ts`)

## 아키텍처

```
[PrtSc 키]
    ↓
[Rust: spawn_blocking → 전체 화면 캡처 (OCR용)]
    ↓
[오버레이 창 즉시 표시: set_fullscreen(true) + 로딩 메시지]
    ↓
[Rust: OCR 서버로 전송 → 감지 결과 수신]
    ↓
[오버레이: ocr_result 이벤트 수신 → OCR 박스 표시]
    ↓
[사용자가 박스 클릭]
    ↓
[Rust: select_text 커맨드 → 오버레이 숨기기 + AI API 번역 요청]
    ↓
[메인 창: 번역 결과 표시]
```

## 오버레이 설계

### 투명창 방식 (스크린샷 배경 없음)

오버레이는 `transparent: true`, `decorations: false` 창으로 전체화면(`set_fullscreen(true)`)을 덮는다.
스크린샷 이미지를 배경으로 표시하지 않는다. 사용자는 실제 화면을 그대로 보며, 위에 반투명 어둠 레이어와 OCR 박스만 렌더링된다.

**이유**: 스크린샷을 PNG/JPEG로 인코딩하여 IPC로 전송하면 수백 ms 지연 발생. 불필요한 복사본이기도 함.

### OCR 박스 좌표 변환

`screenshots` 캡처 크기(`orig_width`)는 물리 픽셀, CSS는 논리 픽셀이므로 보정이 필요하다.

```ts
const cssScaleX = window.innerWidth / orig_width;   // 논리/물리 비율
const cssScaleY = window.innerHeight / orig_height;
// OCR 좌표 → CSS 픽셀
left: x * scale * cssScaleX
```

여기서 `scale = orig_width / 1024` (OCR 리사이즈 역배율).

## 창 구성

| 창 | 역할 | 특징 |
|----|------|------|
| 메인 창 | 번역 결과 표시 | 작은 크기, 항상 위, 장식 없음 |
| 오버레이 창 | OCR 박스 선택 | 전체화면(`set_fullscreen`), 투명, 항상 위 |

## Tauri 이벤트 / 커맨드

| 이름 | 방향 | 설명 |
|------|------|------|
| `overlay_show` | Rust → 오버레이 | 캡처 완료, 로딩 상태 시작 |
| `ocr_result` | Rust → 오버레이 | OCR 완료, 박스 표시 |
| `ocr_error` | Rust → 오버레이 | OCR 오류 |
| `translating` | Rust → 메인 | 번역 시작 |
| `translation_result` | Rust → 메인 | 번역 완료 |
| `translation_error` | Rust → 메인 | 번역 오류 |
| `select_text` | 오버레이 → Rust | 박스 클릭 시 호출, Rust에서 hide + 번역 일괄 처리 |

## 환경변수

| 변수 | 설명 | 기본값 |
|------|------|--------|
| `SOURCE` | 소스 언어 (`en` / `ch`) | - |
| `API_BASE_URL` | OCR 서버 URL | `http://127.0.0.1:8000` |
| `AI_GATEWAY_API_KEY` | AI Gateway API 키 | - |
| `AI_GATEWAY_MODEL` | 모델 이름 | - |
| `SYSTEM_PROMPT_PATH` | 시스템 프롬프트 파일 경로 | `.system_prompt.txt` |
| `X_DELTA` | OCR 그루핑 X 임계값 | `25` |
| `Y_DELTA` | OCR 그루핑 Y 임계값 | `225` |

## 기술 스택

- **프론트엔드**: React 19 + TypeScript + Vite
- **백엔드**: Rust + Tauri 2.x
- **런타임 / 패키지 관리**: Deno (`deno.json` imports)
- **테스트**: `deno test` (`src/detection_test.ts`)
- **전역 단축키**: `tauri-plugin-global-shortcut`
- **스크린샷**: `screenshots` crate
- **HTTP 클라이언트**: `reqwest`

## 구현 중 발생한 트러블슈팅

### 1. `os error 6` (ERROR_INVALID_HANDLE) — setup hook 패닉

`on_shortcut` 호출 후 같은 단축키에 `register`를 중복 호출하면 Windows에서 핸들 오류 발생.
`on_shortcut`이 등록과 콜백 설정을 함께 처리하므로 별도 `register` 호출 불필요.

### 2. `no reactor running` — Tokio 런타임 없음

`on_shortcut` 콜백 내부에서 `tokio::spawn` 호출 시 Tokio 런타임 컨텍스트 없음.
→ `tauri::async_runtime::spawn` 사용.

### 3. 투명 WebView2 창에서 클릭이 창 아래로 통과

Windows에서 투명 WebView2 창은 기본적으로 마우스 이벤트를 아래 창으로 통과시킨다.
두 가지 조치 필요:
- `overlay.set_ignore_cursor_events(false)` — WS_EX_TRANSPARENT 제거
- `body { background: rgba(0,0,0,0.002); }` — WebView2 픽셀 알파값 비-제로

### 4. `hide()` 후 `invoke()` 실행 안됨

JS에서 `await getCurrentWindow().hide()` 호출 시 WebView2가 서스펜드되어
이후 `invoke("translate_text")` IPC가 전달되지 않음.

→ `select_text` Rust 커맨드에서 `overlay.hide()` + 번역을 한 번에 처리.
JS는 fire-and-forget으로 `invoke("select_text", { text })` 만 호출.
