# Buzhidao App

`app`은 Buzhidao 데스크톱 애플리케이션 본체입니다. Tauri 백엔드, 윈도우 구성, 런타임 설정, 프런트엔드 빌드 연결을 담당합니다.

## 기술 스택

- Rust
- Tauri 2.x
- `tauri-plugin-global-shortcut`
- `tauri-plugin-single-instance`
- React + TypeScript + Vite 8

## 프로젝트 구조

```text
app/
├── src/                # Tauri 백엔드, 창 제어, OCR/번역 실행 흐름
├── ui/                 # overlay / popup / loading / settings 프런트엔드
├── capabilities/       # Tauri capability 설정
├── icons/              # 앱/트레이 아이콘
├── .env.example        # 런타임 설정 예시
├── .prompt             # 시스템 프롬프트 파일
├── Cargo.toml          # Rust / Tauri 의존성
└── tauri.conf.json     # 윈도우 / 빌드 설정
```

## 개발 및 테스트 방법

앱 실행:

```bash
cd app
cargo tauri dev
```

Rust 테스트:

```bash
cd app
cargo test
```

프런트엔드 테스트:

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

## 각 기능 설명

### 앱 시작

- 시작 시 `loading` 창이 먼저 표시됩니다.
- `.env`와 `.prompt`를 준비하고 OCR 엔진 warmup이 끝나면 `loading` 창을 닫습니다.
- OCR 엔진 초기화나 warmup이 실패하면 `loading` 창이 실패 상태로 전환되고 종료 버튼이 표시됩니다.
- warmup 전에는 전역 캡처 단축키 입력을 막습니다.

### 전역 캡처

- 기본 캡처 단축키는 Windows/Linux `Ctrl+Alt+A`, macOS `Cmd+Shift+A`입니다.
- 설정 창에서 값을 바꾸면 즉시 재등록됩니다.
- 새 단축키 등록 실패나 저장 실패가 나면 이전 단축키로 롤백합니다.

### 윈도우 구성

- `overlay`: 전체 화면 OCR 결과 표시
- `popup`: 번역 결과 표시
- `loading`: 시작 warmup 진행 표시
- `settings`: 설정 편집

`settings` 창은 hide/show 재사용이 아니라 실제로 닫혔다가, 다시 열 때 새로 생성됩니다.

### 설정 관리

- 런타임 설정은 `.env`와 `.prompt`에서 읽습니다.
- `get_user_settings`, `save_user_settings` 커맨드로 프런트와 동기화합니다.
- 필수 설정 누락 시 설정 창을 자동으로 열고 안내 메시지와 강조 필드를 전달합니다.
- settings 창은 OCR busy 상태를 구독하며, OCR 진행 중에는 저장 버튼을 전체 비활성화합니다.
- OCR 종료 시 settings 창의 저장 버튼은 자동으로 다시 활성화됩니다.

### 트레이

- 시스템 트레이에서 `설정…`, `종료` 메뉴를 제공합니다.
- 단일 인스턴스 모드라 이미 실행 중이면 기존 앱 포커스 경로를 사용합니다.

## 특이 사항

- `bundle.active`는 현재 `false`입니다.
- 개발 빌드에서는 `ui`의 Vite dev server(`http://localhost:1420`)를 사용합니다.
- `OCR_SERVER_EXECUTABLE`을 비우면 기본 sidecar 경로와 번들 리소스 fallback을 순서대로 탐색합니다.
