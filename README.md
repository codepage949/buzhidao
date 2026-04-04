# Buzhidao (不知道)

Buzhidao는 화면의 텍스트를 OCR(광학 문자 인식)로 추출하고, AI를 통해 번역하여 텔레그램으로 전송해주는 자동화 도구입니다. Windows의 PrintScreen 키를 후킹하여 활성 창의 이미지를 캡처하고 번역하는 과정을 자동화합니다.

## 기술 스택

### Client (Orchestrator)
- **Runtime**: [Deno](https://deno.com/)
- **Language**: TypeScript
- **Main Libraries**:
    - `clipboard-image`: 클립보드의 비트맵 이미지 읽기
    - `imgScript (imagescript)`: 이미지 리사이징 및 PNG 인코딩
    - `openai`: OpenAI API(Vercel AI Gateway 경유)를 통한 번역
    - `Deno FFI (user32.dll)`: Windows API를 이용한 키보드 후킹 및 입력 시뮬레이션
    - `@std/dotenv`: 환경 변수 관리

### OCR Server
- **Framework**: [FastAPI](https://fastapi.tiangolo.com/)
- **Language**: Python (uv 패키지 매니저 사용)
- **OCR Engine**: [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) (GPU 가속 지원)
- **Deployment**: Docker, Docker Compose

---

## 프로젝트 구조

```text
/
├── main.ts             # 클라이언트 메인 로직 (키 후킹, 펌프 루프, 워크플로우 제어)
├── deno.json           # Deno 설정 및 실행 태스크 정의
├── .env.example        # 환경 변수 설정 예시
├── src/
│   ├── detection.ts    # OCR 결과 그룹화 및 언어 필터링 로직
│   └── telegram.ts     # 텔레그램 봇 API 클라이언트 구현
├── server/             # OCR 서버 (Python/FastAPI)
│   ├── main.py         # OCR 서버 엔드포인트 및 모델 로드
│   ├── pyproject.toml  # Python 의존성 및 프로젝트 설정 (uv)
│   ├── Dockerfile      # GPU 지원 OCR 서버 빌드 설정
│   └── docker-compose.yaml
└── docs/               # 변경 이력 및 문서
```

---

## 개발 및 테스트 방법

### 1. 환경 변수 설정
루트 디렉토리에 `.env` 파일을 생성하고 필요한 값을 설정합니다. (상세 항목은 `.env.example` 참조)

### 2. OCR 서버 실행
OCR 서버는 대용량 모델을 사용하므로 Docker 환경에서의 실행을 권장합니다.

```bash
cd server
# Docker Compose 사용 시 (추천)
docker-compose up --build

# 직접 실행 시 (uv 설치 필요)
uv run main.py
```

### 3. 클라이언트 실행 (Windows)
클라이언트는 Windows API(FFI)를 사용하므로 Windows 환경에서 Deno로 실행해야 합니다.

```bash
# 개발 모드 실행 (태스크 사용)
deno task dev

# 테스트 실행
deno task test
```

---

## 릴리즈 배포 방법

1. **서버 배포**: NVIDIA Docker Runtime이 설치된 서버에서 `docker-compose`를 사용하여 배포합니다.
2. **클라이언트 배포**: `deno compile` 명령어를 사용하여 단일 실행 파일(.exe)로 빌드할 수 있습니다.
   ```bash
   deno compile --allow-env --allow-net --allow-run --allow-read --allow-write --allow-ffi --unstable-ffi main.ts
   ```

---

## 각 기능 설명

### 1. 활성 창 캡처 및 자동 번역 (PrintScreen 후킹)
- **실행 경로**: `PrintScreen` 키 입력 감지 -> `Alt + PrintScreen` 입력 시뮬레이션(활성 창 캡처) -> 클립보드 이미지 읽기 -> 1024px 너비로 리사이징 -> OCR 서버 전송 -> 근접 텍스트 그룹화 -> AI 번역 -> 텔레그램 전송.
- **상세**: 사용자가 전체 화면 캡처(`PrtSc`)를 누르면 프로그램이 이를 가로채서 현재 활성화된 창만 캡처하도록 변경합니다. 추출된 텍스트가 4개 미만이면 즉시 번역 결과를 보내고, 4개 이상이면 사용자가 선택할 수 있도록 텔레그램 인라인 버튼을 제공합니다.

### 2. 텔레그램 봇 직접 대화
- **실행 경로**: 텔레그램 메시지 수신 -> `main.ts`의 `pollTgMessage` 감지 -> AI 모델(OpenAI)에 전달 -> 번역/응답 수신 -> 텔레그램 답장.
- **상세**: 캡처 외에도 텔레그램 봇에게 직접 텍스트를 보내면 설정된 시스템 프롬프트에 따라 내용을 처리해 줍니다.

---

## 특이 사항

- **Windows API 의존성**: `user32.dll`을 직접 호출하여 로우 레벨 키보드 이벤트를 처리합니다. 따라서 다른 OS에서는 클라이언트 구동이 불가능합니다.
- **이미지 최적화**: OCR 서버의 부하를 줄이고 전송 속도를 높이기 위해 클라이언트 단에서 이미지를 리사이징하여 전송합니다.
- **텍스트 병합 알고리즘**: OCR은 단어 단위로 텍스트를 인식하는 경우가 많아, `X_DELTA`, `Y_DELTA` 값을 기준으로 인접한 텍스트들을 하나의 문장으로 병합하는 커스텀 로직(`groupDetections`)이 포함되어 있습니다.
