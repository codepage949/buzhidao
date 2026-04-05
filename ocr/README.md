# Buzhidao OCR Server (Unknown)

Buzhidao 데스크톱 앱에서 전송한 화면 캡처 이미지를 분석하여 텍스트 영역과 내용을 추출하는 FastAPI 기반 OCR 서버입니다.

## 기술 스택

- **Framework**: FastAPI
- **Language**: Python 3.13
- **OCR Engine**: PaddleOCR (GPU 가속 지원)
- **Package Manager**: uv
- **Deployment**: Docker, Docker Compose

---

## 프로젝트 구조

```text
ocr/
├── main.py             # FastAPI 서버 엔트리포인트 및 OCR 로직
├── test_main.py        # API 기능 테스트 코드
├── pyproject.toml      # uv 의존성 및 프로젝트 설정
├── Dockerfile          # NVIDIA CUDA 기반 Docker 이미지 빌드 설정
└── docker-compose.yaml # 서비스 실행 설정 (GPU 패스스루 포함)
```

---

## 개발 및 테스트 방법

### 1. 환경 변수 설정
`ocr/.env` 파일을 생성하고 다음 필수 항목을 설정합니다:
- `HTTP_HOST`: 서버 호스트 (기본 `0.0.0.0`)
- `HTTP_PORT`: 서버 포트 (기본 `8000`)
- `SCORE_THRESH`: OCR 인식 신뢰도 임계값 (예: `0.5`)

### 2. 로컬 개발 실행 (uv 필요)
```bash
# 의존성 설치 및 서버 실행
uv run uvicorn main:app --host 0.0.0.0 --port 8000
```

### 3. 테스트 실행
```bash
# pytest를 이용한 API 테스트
uv run pytest
```

---

## 릴리즈 배포 방법

NVIDIA GPU가 있는 서버에서 Docker Compose를 사용하여 배포하는 것을 권장합니다.

```bash
# Docker 컨테이너 빌드 및 실행
docker-compose up --build -d
```
*주의: 호스트 시스템에 NVIDIA Container Toolkit이 설치되어 있어야 GPU 가속이 동작합니다.*

---

## 각 기능 설명

### 1. 이미지 텍스트 추출 (OCR)
- **실행 경로**: `POST /infer/{src}` 호출 -> 이미지 임시 저장 -> PaddleOCR 모델 추론 -> 결과(좌표 및 텍스트) 반환.
- **상세**: 영어(`en`) 및 중국어(`ch`) 모델을 지원하며, 캡처된 이미지 내의 모든 텍스트 블록 위치와 텍스트 내용을 JSON 형태로 반환합니다.

### 2. 모델 프리로딩 (Lifespan)
- **실행 경로**: 서버 시작 시 -> 지원 언어별 OCR 모델 로드 -> 샘플 이미지 추론 수행.
- **상세**: 첫 요청 시의 지연 시간을 줄이기 위해 서버 기동 단계에서 OCR 모델을 메모리에 미리 로드하고 초기화합니다.

---

## 특이 사항

- **GPU 가속**: 성능 향상을 위해 `paddlepaddle-gpu` 라이브러리를 사용하며, CUDA 환경에서 최적의 속도를 보장합니다.
- **임시 파일 관리**: 업로드된 이미지는 분석 직후 서버에서 안전하게 삭제됩니다.
