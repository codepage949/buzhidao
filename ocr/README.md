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
├── 🐍 main.py                 # FastAPI 앱 엔트리포인트 및 추론 로직
├── 🧪 test_main.py            # API 동작 검증 테스트 (pytest)
├── 📦 pyproject.toml          # CPU 실행용 uv 프로젝트 설정
├── 📦 pyproject.gpu.toml      # GPU 실행용 uv 프로젝트 설정
├── 🐳 Dockerfile              # OCR 서버 컨테이너 빌드 레시피
├── 🚀 docker-compose.yaml     # CPU 기본 실행 설정
└── ⚡ docker-compose.gpu.yaml # NVIDIA GPU 예약 및 GPU 빌드 override
```

---

## 🚀 개발 및 서버 가이드

### 1️⃣ 환경 변수 설정
`ocr/.env` 파일을 생성하여 동작 옵션을 제어할 수 있습니다:
```env
HTTP_HOST=0.0.0.0
HTTP_PORT=8000
OCR_DEVICE=cpu
SCORE_THRESH=0.5
```

- `OCR_DEVICE`: `gpu` 또는 `cpu`. 코드 기본값은 `gpu`지만, 실제 실행 환경에 맞게 사용자가 직접 설정해야 합니다.
- `pyproject.toml`: CPU용 Paddle 런타임(`paddlepaddle`)을 설치합니다.
- `pyproject.gpu.toml`: GPU용 Paddle 런타임(`paddlepaddle-gpu`)을 설치합니다.

### 2️⃣ 로컬 서버 실행 (uv)
```bash
# 서버 기동
uv run uvicorn main:app --host 0.0.0.0 --port 8000
```

### 3️⃣ Docker 배포 (권장)
```bash
# CPU 실행
docker compose up --build -d

# GPU 실행
docker compose -f docker-compose.yaml -f docker-compose.gpu.yaml up --build -d
```
*주의: GPU 가속을 위해 호스트에 NVIDIA Container Toolkit이 필요하며, 이때 GPU override와 `OCR_DEVICE=gpu`를 함께 맞춰야 합니다.*

### 4️⃣ 실행 중인 Docker OCR 엔드포인트 테스트
```bash
# CPU 컨테이너를 먼저 올린 뒤
uv run --group dev python live_endpoint_check.py --base-url http://127.0.0.1:8000 --source en --wait-seconds 240

# GPU 컨테이너를 먼저 올린 뒤
uv run --group dev python live_endpoint_check.py --base-url http://127.0.0.1:8000 --source en --wait-seconds 240
```
- 첫 기동은 OCR 모델 다운로드와 preload 때문에 준비까지 시간이 걸릴 수 있습니다. `--wait-seconds`로 대기 시간을 늘릴 수 있습니다.

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

- ⚡ **프로필 분리**: CPU는 `pyproject.toml`, GPU는 `pyproject.gpu.toml`을 사용해 Paddle 런타임 자체를 분리합니다.
- 🧪 **엔드포인트 테스트**: 실행 중인 OCR 컨테이너에 `live_endpoint_check.py`로 실제 HTTP 요청을 보내 스모크 테스트할 수 있습니다.
- 🧰 **장치 전환**: 자동 전환은 하지 않으며, 설치 프로필과 `OCR_DEVICE`를 같은 방향으로 맞춰야 합니다.
- 🧹 **보안**: 처리 완료 후 서버의 임시 이미지는 즉시 삭제하여 메모리 및 저장소 효율을 유지합니다.
