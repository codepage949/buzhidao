# 🧠 Buzhidao OCR Server (FastAPI)

> **Buzhidao OCR 서버**는 캡처된 화면 이미지를 분석하여 텍스트의 좌표와 내용을 추출하는 핵심 엔진입니다.

---

## 🛠️ 기술 스택 (OCR Tech Stack)

- ⚡ **FastAPI**: 고성능 Python 웹 프레임워크 (Asynchronous)
- 🔍 **PaddleOCR**: 강력한 딥러닝 기반 다국어 OCR 엔진
- 🐍 **Python 3.13**: 최신 Python 런타임 환경
- 📦 **uv**: 초고속 Python 패키지 매니저
- 🐳 **Docker & GPU**: NVIDIA CUDA 가속 지원을 통한 고성능 추론

---

## 📂 프로젝트 구조 (Structure)

```text
ocr/
├── 🐍 main.py             # FastAPI 앱 엔트리포인트 및 추론 로직
├── 🧪 test_main.py        # API 동작 검증 테스트 (pytest)
├── 📦 pyproject.toml      # uv를 이용한 의존성 관리 설정
├── 🐳 Dockerfile          # NVIDIA CUDA 기반 Docker 빌드 레시피
└── 🚀 docker-compose.yaml # 서비스 실행 및 GPU 패스스루 설정
```

---

## 🚀 개발 및 서버 가이드

### 1️⃣ 환경 변수 설정
`ocr/.env` 파일을 생성하여 동작 옵션을 제어할 수 있습니다:
```env
HTTP_HOST=0.0.0.0
HTTP_PORT=8000
SCORE_THRESH=0.5
```

### 2️⃣ 로컬 서버 실행 (uv)
```bash
# 서버 기동
uv run uvicorn main:app --host 0.0.0.0 --port 8000
```

### 3️⃣ Docker 배포 (권장)
```bash
# GPU 지원 모델 빌드 및 실행
docker-compose up --build -d
```
*주의: GPU 가속을 위해 호스트에 NVIDIA Container Toolkit이 필요합니다.*

---

## ✨ 핵심 기능 상세 (OCR Features)

### 📸 1. 이미지 텍스트 추출 (Inference)
- **API**: `POST /infer/{src}` (src: 이미지 파일 경로 또는 바디)
- **동작**: 업로드된 이미지를 PaddleOCR 모델에 전달하여 텍스트 위치(Polygon)와 내용, 신뢰도를 반환합니다.

### 🚀 2. 모델 리소스 관리
- **동작**: 서버 시작 시(`lifespan`) 필요한 OCR 모델을 메모리에 미리 로드하여 첫 요청의 지연 시간을 최소화합니다.
- **언어**: 영어(`en`) 및 중국어(`ch`) 모델을 기본적으로 활용합니다.

---

## 💡 특이 사항 (OCR Dev Notes)

- ⚡ **GPU 가속**: `paddlepaddle-gpu` 라이브러리를 통해 CUDA 환경에서 0.1초 이내의 빠른 추론 성능을 보장합니다.
- 🧹 **보안**: 처리 완료 후 서버의 임시 이미지는 즉시 삭제하여 메모리 및 저장소 효율을 유지합니다.
