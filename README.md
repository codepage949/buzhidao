# 🧭 Buzhidao (不知道)

> Buzhidao는 화면의 텍스트를 OCR로 추출하고 AI로 번역하여, 직관적인 오버레이와 팝업 UI를 통해 제공하는 화면 번역 도구입니다. 🚀

---

## ✨ 주요 기능 (Key Features)

### 📸 1. 원클릭 화면 캡처 및 OCR
- **단축키**: `PrintScreen` (전역 후킹)
- **동작**: 어떤 화면에서든 `PrtSc` 키를 누르면 즉시 현재 화면을 캡처하고 OCR 서버로 전송하여 텍스트 영역을 완벽하게 식별합니다.

### 🖼️ 2. 인터랙티브 오버레이 UI
- **동작**: OCR 분석 결과가 화면 전체에 투명 오버레이 형태로 나타납니다.
- **상세**: 식별된 각 텍스트 블록은 마우스로 상호작용이 가능하며, 클릭 시 즉시 AI 번역 단계로 진입합니다.

### 🌐 3. 스마트 AI 번역 및 팝업
- **동작**: 선택한 텍스트를 AI 모델을 통해 번역하고 별도의 팝업 창에 표시합니다.
- **상세**: 단순 텍스트 번역을 넘어, 문맥에 맞는 자연스러운 결과를 제공하며 마크다운 형식을 지원합니다.

---

## 🛠️ 기술 스택 (Tech Stack)

### 💻 Desktop App (Frontend & Backend)
- 🦀 **Rust**: 고성능 시스템 로직 및 Tauri 백엔드
- ⚡ **Tauri 2.x**: 경량 데스크톱 프레임워크
- ⚛️ **React 19 & TypeScript**: 현대적인 UI 라이브러리 및 타입 안정성
- 🧪 **Deno**: 효율적인 프런트엔드 테스트 및 태스크 러너

### 🧠 OCR Server
- ⚡ **FastAPI**: 고성능 Python 웹 프레임워크
- 🔍 **PaddleOCR**: 강력한 다국어 지원 OCR 엔진
- 🐳 **Docker**: CPU/GPU 실행 구성을 분리한 OCR 서버 배포

---

## 📂 프로젝트 구조 (Project Structure)

```text
.
├── 🦀 app/                # Tauri 데스크톱 애플리케이션 (Rust/React)
│   ├── 🛠️ src/            # Rust 백엔드: 윈도우 관리, 키 후킹, API 통신
│   └── 🎨 ui/             # React 프런트엔드: 오버레이 및 팝업 UI
└── 🧠 ocr/                # FastAPI OCR 서버 (Python/PaddleOCR)
```

---

## 🚀 시작하기 (Getting Started)

### 1️⃣ OCR 서버 실행
```bash
cd ocr
# CPU 실행
docker compose up --build -d

# GPU 실행
docker compose -f docker-compose.yaml -f docker-compose.gpu.yaml up --build -d
```

```bash
# OCR CPU 엔드포인트 테스트
uv run --directory ocr --group dev python live_endpoint_check.py --base-url http://127.0.0.1:8000 --source en --wait-seconds 240

# OCR GPU 엔드포인트 테스트
uv run --directory ocr --group dev python live_endpoint_check.py --base-url http://127.0.0.1:8000 --source en --wait-seconds 240
```

### 2️⃣ 데스크톱 앱 실행
```bash
cd app
# 개발 모드 실행 (Tauri)
cargo tauri dev
```

---

## 🧪 테스트 및 품질 (Testing)

- **App (Rust)**: `cargo test` (app/ 디렉토리)
- **App UI**: `deno task test` (app/ui/ 디렉토리)
- **OCR Server**: `pytest` (ocr/ 디렉토리)

---

## 📦 릴리즈 배포 (Release)

1. **OCR 서버**: CPU/GPU별로 분리된 Paddle 런타임 프로필을 사용합니다. GPU 배포는 NVIDIA Container Toolkit과 `docker-compose.gpu.yaml`, `OCR_DEVICE=gpu`가 함께 필요합니다.
2. **데스크톱 앱**: `cargo tauri build` 명령어로 OS별 설치 파일(MSI 등)을 생성합니다.

---

## 💡 특이 사항 (Notes)

- 🪟 **Windows 최적화**: 전역 키 후킹(`rdev`) 및 오버레이 투명도 처리는 Windows 환경에 최적화되어 있습니다.
- 🚀 **성능**: 자동 전환은 하지 않습니다. CPU/GPU용 설치 프로필과 `OCR_DEVICE` 설정을 같은 방향으로 직접 맞춰야 합니다.
