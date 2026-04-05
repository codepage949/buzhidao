# 🦀 Buzhidao Desktop App (Tauri)

> **Buzhidao 데스크톱 애플리케이션**은 사용자 화면을 캡처하고, OCR 결과를 시각화하며, AI 번역 결과를 제공하는 핵심 클라이언트입니다.

---

## 🛠️ 기술 스택 (App Tech Stack)

- 🦀 **Rust**: 시스템 레벨 제어 및 Tauri 백엔드 로직
- ⚡ **Tauri 2.x**: 안전하고 경량화된 데스크톱 앱 프레임워크
- ⌨️ **rdev**: `unstable_grab` 모드를 통한 전역 키보드 훅 (`PrintScreen`)
- 📸 **screenshots & image**: 멀티 모니터 지원 화면 캡처 및 이미지 처리
- 🌐 **reqwest**: OCR 서버 및 AI 게이트웨이와 고성능 비동기 통신
- ⚛️ **React 19 & Vite 6**: 유연한 오버레이 및 팝업 UI 구성

---

## 📂 프로젝트 구조 (Structure)

```text
app/
├── 🦀 src/                # Rust 백엔드 소스 코드
│   ├── main.rs         # 앱 엔트리포인트 및 윈도우 루프 관리
│   ├── lib.rs          # Tauri 커맨드 및 핵심 비즈니스 로직
│   ├── window.rs       # 투명 오버레이/팝업 윈도우 생성 및 제어
│   └── services.rs     # 외부 서버(OCR, AI) 연동 서비스
├── 🎨 ui/                 # 프런트엔드 (React/Vite/Deno)
├── 🖼️ icons/              # 시스템 및 트레이 아이콘
└── 🔒 capabilities/       # Tauri 2.0 보안 권한 설정
```

---

## 🚀 개발 및 실행 가이드

### 1️⃣ 환경 변수 설정
`app/.env` 파일을 생성하고 다음 필수 항목을 설정합니다:
```env
# 번역 소스 언어 설정 (en / ch)
SOURCE=en
# OCR 서버 URL
API_BASE_URL=http://127.0.0.1:8000
# AI Gateway API 키 및 모델
AI_GATEWAY_API_KEY=your_key_here
AI_GATEWAY_MODEL=alibaba/qwen-2.5-72b-instruct
# 시스템 프롬프트 및 OCR 그루핑 설정
SYSTEM_PROMPT_PATH=.system_prompt.txt
X_DELTA=25
Y_DELTA=225
```

### 2️⃣ 개발 모드 실행
```bash
# Tauri 개발 서버 기동 (자동 리로드 지원)
cargo tauri dev
```

### 3️⃣ 테스트 실행
```bash
# 백엔드(Rust) 단위 및 통합 테스트
cargo test

# 프런트엔드(UI) 테스트
cd ui && deno task test
```

---

## ✨ 핵심 기능 상세 (Deep Dive)

### 📸 1. 전역 캡처 흐름
- **실행**: `PrintScreen` 입력 -> `rdev` 훅 감지 -> `screenshots` 라이브러리로 모든 모니터 캡처 -> 이미지 리사이징 후 OCR 서버 전송.
- **특징**: 앱이 포커스되지 않은 상태에서도 키 입력을 가로채어 즉각적인 캡처를 수행합니다.

### 🖼️ 2. 인터랙티브 오버레이
- **실행**: OCR 서버 응답 수신 -> 투명한 전체 화면 윈도우 생성 -> React UI에 데이터 전달.
- **특징**: `set_ignore_cursor_events(false)` 처리를 통해 투명 창임에도 마우스 클릭 이벤트를 정확하게 수집합니다.

### 🌐 3. AI 번역 팝업
- **실행**: 오버레이에서 텍스트 클릭 -> `invoke` 호출 -> Rust 서비스에서 AI 번역 수행 -> 결과 팝업 표시.

---

## 💡 특이 사항 (Dev Notes)

- 🪟 **WebView2 서스펜드 방지**: 윈도우를 숨기거나(hide) 보이기(show) 전에 Rust 커맨드 하나에서 IPC를 일괄 처리하여 WebView2가 서스펜드되지 않도록 설계되었습니다.
- 🎨 **투명도 트릭**: Windows WebView2에서는 `set_ignore_cursor_events(false)` 호출만으로는 완전 투명(Alpha 0) 영역의 클릭 관통을 막기 어려운 경우가 있습니다. 이를 방지하기 위해 `rgba(0,0,0,0.002)` 수준의 비-제로(Non-zero) 배경색을 병행하여 마우스 이벤트를 확실히 캡처합니다.
