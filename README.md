# Buzhidao (不知道)

> Buzhidao는 화면의 텍스트를 OCR로 추출하고 AI로 번역해 오버레이와 팝업 UI로 보여주는 Tauri 데스크톱 앱입니다.

## 기술 스택

### Desktop App
- Rust
- Tauri 2.x
- React 19 + TypeScript
- Deno + Vite 8

### OCR / AI
- PaddleOCR / PaddlePaddle
- reqwest 기반 AI Gateway 호출

### Tooling
- uv
- PyInstaller
- cargo / cargo-tauri

## 프로젝트 구조

```text
.
├── app/
│   ├── src/                 # Tauri 백엔드, 창 제어, OCR/번역 실행 흐름
│   ├── ui/                  # overlay / popup / loading / settings 프런트엔드
│   ├── capabilities/        # Tauri capability 설정
│   ├── .env.example         # 런타임 설정 예시
│   └── tauri.conf.json      # Tauri 윈도우/빌드 설정
├── ocr_server/              # OCR sidecar 빌드 프로젝트
├── docs/changes/            # 작업 변경 기록
└── scripts/                 # 보조 스크립트
```

## 개발 및 테스트 방법

### 환경 변수 준비

개발 빌드(`cargo tauri dev`)에서는 `app/.env`를 준비합니다. 주요 항목은 다음과 같습니다.

- `SOURCE`
- `AI_GATEWAY_API_KEY`
- `AI_GATEWAY_MODEL`
- `WORD_GAP`
- `LINE_GAP`
- `OCR_DEBUG_TRACE`
- `OCR_SERVER_DEVICE`
- `OCR_SERVER_EXECUTABLE`
- `OCR_SERVER_STARTUP_TIMEOUT_SECS`
- `OCR_SERVER_REQUEST_TIMEOUT_SECS`
- `CAPTURE_SHORTCUT`

예시는 [app/.env.example](app/.env.example)에 있습니다.

### OCR 서버 준비

CPU 빌드:

```bash
cd ocr_server
uv sync -p 3.13 --group build --group cpu
uv run --group build --group cpu python build.py
```

GPU 빌드:

```bash
cd ocr_server
uv sync -p 3.13 --group build --group gpu
uv run --group build --group gpu python build.py --gpu
```

산출물 기본 경로는 `ocr_server/dist/ocr_server/ocr_server.exe`입니다.

### 앱 실행

```bash
cd app
cargo tauri dev
```

프런트엔드 의존성이 없거나 디렉터리를 새로 옮긴 직후라면 먼저 실행합니다.

```bash
cd app/ui
deno install
```

### 테스트

Rust 테스트:

```bash
cd app
cargo test
```

UI 테스트:

```bash
cd app/ui
deno task test
```

## 릴리즈 배포 방법

기본 빌드:

```bash
cd app
cargo tauri build
```

GPU 앱 빌드:

```bash
cd app
cargo tauri build --features gpu
```

개발 빌드는 `app/.env`를 읽고, 배포 빌드는 앱 데이터 디렉터리의 `.env`를 읽습니다. 두 경로를 동시에 읽지 않습니다. OCR 서버 실행 파일은 설정값 경로를 우선 사용한 뒤 번들 리소스나 앱 옆 `ocr_server` 폴더를 fallback으로 탐색합니다.

## 각 기능 설명

### 1. 앱 시작과 초기 준비

- 앱 시작 직후 `loading` 창이 먼저 뜹니다.
- 백엔드가 `.env`, `.prompt`, OCR 엔진을 초기화하고 OCR warmup을 수행합니다.
- warmup이 끝날 때까지 전역 캡처 단축키 입력은 막혀 있습니다.
- OCR 엔진 초기화나 warmup이 실패하면 `loading` 창이 실패 상태로 전환되고, 종료 버튼이 표시됩니다.
- 준비가 끝나면 `loading` 창이 닫히고 캡처 단축키가 활성화됩니다.

### 2. 전역 캡처 단축키

- 기본 단축키는 Windows/Linux `Ctrl+Alt+A`, macOS `Cmd+Shift+A`입니다.
- 사용자는 설정 창에서 `CAPTURE_SHORTCUT` 값을 바꿔 다른 Accelerator 조합으로 변경할 수 있습니다.
- 저장 시 단축키 형식을 검증하고, 유효한 값이면 기존 핫키를 해제한 뒤 새 핫키를 즉시 등록합니다.
- 등록 실패나 후속 저장 실패가 나면 이전 단축키로 롤백합니다.

### 3. 전체 화면 캡처와 오버레이 표시

- 전역 단축키를 누르면 현재 화면을 캡처합니다.
- 기존 `popup` 창은 먼저 숨기고, `overlay` 창을 화면에 맞춰 다시 배치한 뒤 표시합니다.
- 오버레이는 OCR 실행 중 로딩 상태를 보여주고, OCR 결과가 오면 텍스트 그룹 박스를 렌더링합니다.
- 이미 오버레이가 떠 있거나 busy 상태면 추가 단축키 입력은 무시됩니다.

### 4. OCR 결과 탐색

- 사용자는 OCR 결과 박스를 직접 클릭해 번역할 수 있습니다.
- 영역 재선택 관련 코드는 내부에 남아 있지만 현재 제품 기능으로는 노출하지 않습니다.
- 따라서 현재 지원 경로는 OCR 결과 박스를 클릭해 번역하는 흐름입니다.

### 5. 번역 팝업

- 텍스트 박스를 클릭하면 선택된 텍스트를 AI Gateway로 보내 번역합니다.
- 번역 결과는 `popup` 창에 표시됩니다.
- 팝업 위치는 선택 박스 주변 화면 여유 공간에 맞춰 계산됩니다.
- 팝업 닫기 버튼이나 ESC는 팝업만 닫고, 오버레이는 유지합니다.

### 6. 오버레이 종료와 OCR 취소

- 오버레이 ESC나 닫기 동작은 `overlay`와 `popup`을 함께 숨깁니다.
- 이때 진행 중 OCR 결과는 세대 토큰으로 무효화되어, 뒤늦게 응답이 와도 화면에 반영되지 않습니다.
- 따라서 사용자가 취소 후 다시 새 캡처를 시작해도 이전 결과가 섞이지 않습니다.

### 7. 설정 창 실행 경로

- 설정 창은 트레이 메뉴 `설정…`으로 열 수 있습니다.
- 필수 설정이 비어 있는 상태에서 캡처 단축키를 누르면 설정 창이 자동으로 열리고, 누락 필드가 강조됩니다.
- 설정 창은 더 이상 `hide()`로 숨기지 않고 실제로 `close()`됩니다.
- 다시 열 때는 Rust가 `settings` 창을 새로 생성하므로, 저장하지 않은 이전 입력 상태가 남지 않고 현재 저장된 설정값으로 다시 표시됩니다.
- OCR 진행 중에는 설정 저장 버튼이 비활성화되며, `OCR 진행 중에는 설정을 저장할 수 없습니다.` 안내 문구가 표시됩니다.
- OCR이 끝나면 설정 저장 버튼은 자동으로 다시 활성화됩니다.

### 8. 시스템 트레이 경로

- 앱은 시스템 트레이 아이콘을 등록합니다.
- 트레이 메뉴에서 `설정…`으로 설정 창을 열고, `종료`로 앱을 종료할 수 있습니다.
- 단일 인스턴스 플러그인이 켜져 있어 이미 실행 중일 때 다시 실행하면 기존 창 포커스 경로를 사용합니다.

## 특이 사항

- `PrintScreen` 같은 modifier 없는 단일 키는 전역 등록 기본값으로 사용하지 않습니다.
- 설정 창의 OCR 장치 변경은 저장은 즉시 되지만 OCR 서버 재시작은 하지 않으므로 다음 앱 실행부터 적용됩니다.
- Wayland 환경에서는 전역 단축키가 데스크톱 정책에 따라 제한될 수 있습니다.
