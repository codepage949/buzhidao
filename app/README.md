# Buzhidao App (不知道)

Buzhidao 데스크톱 애플리케이션은 Tauri 2.0 프레임워크를 기반으로 하며, 사용자의 화면 캡처, OCR 결과 표시(오버레이), AI 번역 결과 제공(팝업) 기능을 담당합니다.

## 기술 스택

- **Core**: Tauri 2.x, Rust
- **Keyboard Hook**: `rdev` (unstable_grab 활성화)
- **Capture**: `screenshots`, `image`
- **Network**: `reqwest`
- **Configuration**: `dotenvy`
- **UI Framework**: React 19, Vite 6, Deno (Task runner)

---

## 프로젝트 구조

```text
app/
├── src/                # Rust 백엔드 소스 코드
│   ├── main.rs         # 엔트리포인트 및 윈도우 관리
│   ├── lib.rs          # Tauri 핸들러 및 핵심 로직
│   ├── window.rs       # 윈도우 생성 및 제어 (오버레이/팝업)
│   └── services.rs     # OCR 서버 및 AI 게이트웨이 통신
├── ui/                 # 프런트엔드 (React/Vite)
├── icons/              # 앱 및 트레이 아이콘
└── capabilities/       # Tauri 2.x 보안 및 권한 설정
```

---

## 개발 및 테스트 방법

### 1. 환경 변수 설정
`app/.env` 파일을 생성하고 다음 필수 항목을 설정합니다:
- `OCR_URL`: OCR 서버 엔드포인트
- `AI_GATEWAY_URL`: AI 번역 게이트웨이 엔드포인트

### 2. 개발 실행
```bash
# Tauri 개발 서버 실행
cargo tauri dev
```

### 3. 테스트 실행
- **Rust 테스트**: `cargo test`
- **프런트엔드 테스트**: `cd ui && deno task test`

---

## 릴리즈 배포 방법

1. `cargo tauri build` 명령을 실행하여 설치 프로그램을 생성합니다.
2. 윈도우용 번들 결과물은 `app/target/release/bundle/msi` 경로에 생성됩니다.

---

## 각 기능 설명

### 1. 전역 캡처 모드 진입
- **실행 경로**: `PrintScreen` 입력 -> 전용 Rust 훅(`rdev`) 감지 -> 전체 화면 캡처 -> OCR 서버 API 요청.
- **상세**: 앱이 실행 중이면 어떤 환경에서도 `PrtSc` 키로 OCR 분석을 시작할 수 있습니다. 캡처된 이미지는 최적화되어 서버로 전송됩니다.

### 2. OCR 오버레이 및 상호작용
- **실행 경로**: 서버 결과 수신 -> 투명 오버레이 윈도우 생성/표시 -> 텍스트 블록 렌더링 -> 클릭 시 번역 요청.
- **상세**: 화면 위로 투명한 오버레이 창이 나타나 식별된 텍스트 영역을 시각화합니다. 사용자가 마우스로 클릭한 영역의 텍스트가 번역 대상이 됩니다.

### 3. 번역 결과 팝업 표시
- **실행 경로**: 번역 API 완료 -> 팝업 윈도우 포커스 -> 결과 표시.
- **상세**: 번역된 내용은 별도의 팝업 창에 표시되며, 사용자는 이를 확인하고 `ESC` 또는 닫기 버튼으로 닫을 수 있습니다.

---

## 특이 사항

- **Tauri 오버레이 마우스 이벤트**: 투명 WebView2 창에서 마우스 이벤트가 무시되지 않도록 `set_ignore_cursor_events(false)`와 미세한 배경색(`rgba(0,0,0,0.002)`) 처리가 적용되어 있습니다.
- **포커스 관리**: 오버레이와 팝업 사이의 원활한 `ESC` 키 처리를 위해 포커스가 전환될 때마다 이벤트 리스너가 동기화됩니다.
