# 🎨 Buzhidao App UI (React & Deno)

> **Buzhidao 데스크톱 앱**의 인터랙티브 오버레이와 번역 팝업 화면을 구성하는 프런트엔드 프로젝트입니다.

---

## 🛠️ 기술 스택 (UI Tech Stack)

- ⚛️ **React 19**: 최신 버전의 React 라이브러리 활용
- ⚡ **Vite 6**: 초고속 개발 서버 및 번들링 엔진 (Multi-entry 지원)
- 🦕 **Deno**: 패키지 매니저 및 고성능 테스크 러너
- 🎨 **Vanilla CSS**: Tauri 오버레이의 성능과 투명도 최적화를 위한 순수 CSS
- 🔌 **Tauri JS API**: `@tauri-apps/api`를 통한 Rust 백엔드와의 IPC 통신
- 📝 **React Markdown**: 번역 결과를 미려하게 렌더링

---

## 📂 프로젝트 구조 (Structure)

```text
ui/
├── 🖼️ src/
│   ├── overlay.tsx     # 전체 화면 투명 오버레이 컴포넌트
│   ├── overlay.html    # 오버레이 윈도우 진입점
│   ├── popup.tsx       # 번역 결과 팝업 컴포넌트
│   ├── popup.html      # 팝업 윈도우 진입점
│   ├── app-hooks.ts    # Tauri 윈도우 제어 및 이벤트 훅
│   └── detection.ts    # OCR 데이터 구조 및 비즈니스 로직
├── 🦕 deno.json           # Deno 설정 및 테스크 정의 (scripts)
└── ⚙️ vite.config.ts      # Vite 빌드 및 다중 엔트리 설정
```

---

## 🚀 개발 및 빌드 가이드

### 1️⃣ 개발 서버 실행
```bash
# Vite 개발 서버 가동 (Deno 사용)
deno task dev
```

### 2️⃣ 프런트엔드 빌드
```bash
# 정적 파일 생성 (ui/dist/ 경로)
deno task build
```

### 3️⃣ 테스트 실행
```bash
# Deno 내장 테스트 환경을 통한 유틸리티 로직 검증
deno task test
```

---

## ✨ 핵심 기능 상세 (UI Features)

### 📸 1. OCR 오버레이 렌더링
- **실행**: `overlay.html` 로드 -> Rust에서 전송한 OCR 데이터(JSON) 수신 -> 화면에 텍스트 영역 박스 렌더링.
- **상세**: 사용자의 마우스가 올려지면 강조(Highlight) 효과가 발생하며, 클릭 시 번역을 요청합니다.

### 🌐 2. 번역 결과 팝업
- **실행**: 번역 결과 데이터 수신 -> `popup.tsx` 렌더링 -> 마크다운 형식으로 번역문 표시.
- **상세**: AI가 제공한 마크다운 문법을 파싱하여 가독성 높은 결과물을 제공합니다.

---

## 💡 특이 사항 (UI Dev Notes)

- 🪟 **투명 창 스타일**: Windows WebView2의 투명도 문제를 해결하기 위해 모든 페이지의 `body`는 `background: rgba(0,0,0,0.002);` 스타일을 필수로 유지합니다.
- ⚙️ **Vite Multi-entry**: `overlay.html`과 `popup.html`을 별도로 빌드하여 Tauri의 각 윈도우에 개별적으로 할당할 수 있도록 설정되어 있습니다.
