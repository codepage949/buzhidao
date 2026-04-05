# Buzhidao (不知道)

Buzhidao는 화면의 텍스트를 OCR로 추출하고 AI로 번역하여 오버레이와 팝업 UI로 제공하는 도구입니다.

## 기술 스택

- **Desktop App**: Tauri 2.x, Rust, React, TypeScript
- **OCR Server**: FastAPI, Python, PaddleOCR
- **AI Integration**: OpenAI/Claude API (AI Gateway)
- **Infrastructure**: Docker, Docker Compose

---

## 프로젝트 구조

```text
.
├── app/                # Tauri 데스크톱 애플리케이션
│   ├── src/            # Rust 백엔드 로직
│   └── ui/             # React/TypeScript 프런트엔드
└── ocr/                # FastAPI OCR 서버
```

---

## 개발 및 테스트 방법

### 1. OCR 서버 실행
```bash
cd ocr
# uv를 이용한 로컬 실행
uv run main.py

# 또는 Docker Compose 사용
docker-compose up --build
```

### 2. 데스크톱 앱 실행
```bash
cd app
# 개발 모드 실행
cargo tauri dev
```

### 3. 테스트 실행
- **App (Rust)**: `cargo test` (app/ 디렉토리)
- **App UI**: `deno task test` (app/ui/ 디렉토리)
- **OCR Server**: `pytest` (ocr/ 디렉토리)

---

## 릴리즈 배포 방법

1. **OCR 서버**: NVIDIA Docker Runtime 환경에서 Docker Compose를 사용하여 배포합니다.
2. **데스크톱 앱**: `cargo tauri build` 명령어로 OS별 설치 파일을 생성합니다.

---

## 각 기능 설명

### 1. 전역 단축키를 이용한 화면 캡처 및 OCR
- **실행 경로**: `PrintScreen` 키 입력 -> 화면 캡처 -> OCR 서버 전송 -> 결과 수신.
- **상세**: 사용자가 어떤 창에서든 `PrtSc`를 누르면 현재 화면을 캡처하고 OCR 서버로 전송하여 텍스트 영역을 식별합니다.

### 2. 오버레이 텍스트 선택 및 번역
- **실행 경로**: 오버레이 UI 표시 -> 텍스트 블록 클릭 -> AI 번역 요청 -> 결과 팝업 표시.
- **상세**: OCR 결과가 투명 오버레이 창에 표시됩니다. 사용자가 특정 텍스트를 클릭하면 AI가 해당 내용을 번역하여 별도의 팝업 창에 보여줍니다.

---

## 특이 사항

- **Windows 최적화**: 전역 키 후킹(`rdev`) 및 오버레이 투명도 처리는 Windows 환경을 기준으로 최적화되어 있습니다.
- **GPU 가속**: OCR 서버는 PaddleOCR을 사용하며, 성능을 위해 NVIDIA GPU 가속을 권장합니다.
