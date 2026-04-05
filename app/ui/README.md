# Buzhidao App UI (不知道)

Buzhidao 데스크톱 앱의 오버레이 및 팝업 화면을 구성하는 React/TypeScript 프런트엔드 프로젝트입니다.

## 기술 스택

- **Framework**: React 19
- **Build Tool**: Vite 6
- **Task Runner**: Deno
- **Styling**: Vanilla CSS (Tauri 오버레이 최적화)
- **API**: Tauri JavaScript API (@tauri-apps/api)
- **Markdown**: react-markdown (번역 결과 렌더링용)

---

## 프로젝트 구조

```text
app/ui/
├── src/
│   ├── overlay.tsx     # 전체 화면 투명 오버레이 컴포넌트
│   ├── overlay.html    # 오버레이 윈도우 엔트리
│   ├── popup.tsx       # 번역 결과 팝업 컴포넌트
│   ├── popup.html      # 팝업 윈도우 엔트리
│   ├── app-hooks.ts    # Tauri 이벤트 및 윈도우 훅
│   └── detection.ts    # OCR 데이터 구조 및 변환 로직
├── deno.json           # Deno 설정 및 테스크 정의
└── vite.config.ts      # Vite 빌드 설정 (다중 엔트리 지원)
```

---

## 개발 및 테스트 방법

### 1. 개발 서버 실행
```bash
# Deno를 이용한 Vite 개발 서버 실행
deno task dev
```

### 2. 빌드 실행
```bash
# 정적 파일 생성 (app/ui/dist/)
deno task build
```

### 3. 테스트 실행
```bash
# 로직 테스트 (Deno Test)
deno task test
```

---

## 릴리즈 배포 방법

1. `deno task build` 명령으로 빌드된 결과물은 `app/ui/dist`에 저장됩니다.
2. Tauri 백엔드가 해당 경로의 정적 파일을 읽어 데스크톱 애플리케이션에 포함합니다.

---

## 각 기능 설명

### 1. 화면 OCR 오버레이 렌더링
- **실행 경로**: `overlay.html` 로드 -> Rust에서 전송한 OCR 데이터(JSON) 수신 -> 캔버스 또는 DOM 요소로 텍스트 영역 렌더링.
- **상세**: 수신된 OCR 결과를 기반으로 화면 전체에 투명한 레이어를 띄우고, 각 텍스트 블록의 위치와 내용을 시각화합니다.

### 2. 텍스트 선택 및 번역 트리거
- **실행 경로**: 오버레이 요소 클릭 -> Tauri IPC(`invoke`) 호출 -> 번역 API 결과 대기 -> 팝업 창 호출.
- **상세**: 사용자가 특정 텍스트를 클릭하면 Rust 백엔드에 해당 텍스트의 번역을 요청하는 신호를 보냅니다.

---

## 특이 사항

- **배경 투명성 및 마우스 관통**: Windows에서의 투명 윈도우 마우스 이벤트 처리를 위해 `body { background: rgba(0,0,0,0.002); }` 스타일이 강제 적용되어 있습니다.
- **다중 엔트리 포인트**: `vite.config.ts`를 통해 `overlay.html`과 `popup.html` 두 개의 서로 다른 윈도우 엔트리를 생성하도록 구성되어 있습니다.
