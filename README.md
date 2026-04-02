# Buzhidao (不知道)

Buzhidao는 화면의 텍스트를 OCR로 인식하고 AI를 통해 번역하여 텔레그램으로 전송해주는 도구입니다. Windows의 PrintScreen 키를 후킹하여 클립보드에 복사된 이미지를 자동으로 처리합니다.

## 기술 스택

### Client (Orchestrator)
- **Runtime**: [Deno](https://deno.com/)
- **Language**: TypeScript
- **Main Libraries**:
    - `clipboard-image`: 클립보드 이미지 읽기
    - `imagescript`: 이미지 전처리 및 인코딩
    - `openai`: AI 모델을 통한 번역 및 처리
    - `Deno FFI (user32.dll)`: Windows 키보드 이벤트 후킹 (PrintScreen)

### OCR Server
- **Framework**: [FastAPI](https://fastapi.tiangolo.com/)
- **Language**: Python
- **OCR Engine**: [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) (GPU 가속 지원)
- **Deployment**: Docker, Docker Compose

---

## 프로젝트 구조

```text
/
├── main.ts             # Deno 클라이언트 메인 로직 (키 후킹, 이미지 처리, 통신)
├── deno.json           # Deno 설정 및 작업 정의
├── server/             # OCR 서버 디렉토리
│   ├── main.py         # FastAPI 서버 메인 로직
│   ├── Dockerfile      # OCR 서버 빌드 설정
│   └── requirements.txt # Python 의존성
└── .env.example        # 환경 변수 예시
```

---

## 개발 및 테스트 방법

### 1. 환경 변수 설정
루트 디렉토리와 `server/` 디렉토리에 각각 `.env` 파일을 생성하고 아래의 모든 환경 변수를 설정합니다.

#### Root `.env` (Client 설정)
| 환경 변수 | 설명 | 예시 |
| :--- | :--- | :--- |
| `AI_GATEWAY_API_KEY` | AI 게이트웨이 인증 키 | `sk-...` |
| `AI_GATEWAY_MODEL` | 사용할 AI 모델명 | `gpt-4o` |
| `SYSTEM_PROMPT_PATH` | 번역 및 처리에 사용할 시스템 프롬프트 파일 경로 | `./prompt.txt` |
| `SOURCE` | 분석할 소스 언어 (`en`: 영어, `ch`: 중국어) | `en` |
| `TELEGRAM_API_BASE_URL` | 텔레그램 API 베이스 URL (프록시 사용 시 변경 가능) | `https://api.telegram.org` |
| `BOT_TOKEN` | 텔레그램 봇 API 토큰 | `123456:ABC...` |
| `CHAT_ID` | 결과 메시지를 전송받을 텔레그램 채팅 ID | `12345678` |
| `API_BASE_URL` | OCR 서버(FastAPI)의 베이스 URL | `http://localhost:8000` |
| `X_DELTA` | OCR 텍스트 병합을 위한 가로(X) 허용 오차 (제곱값) | `2500` |
| `Y_DELTA` | OCR 텍스트 병합을 위한 세로(Y) 허용 오차 (제곱값) | `100` |

#### Server `.env` (OCR Server 설정)
| 환경 변수 | 설명 | 예시 |
| :--- | :--- | :--- |
| `SCORE_THRESH` | OCR 인식 신뢰도 임계값 (0.0 ~ 1.0) | `0.6` |
| `HTTP_HOST` | FastAPI 서버 호스트 주소 | `0.0.0.0` |
| `HTTP_PORT` | FastAPI 서버 포트 번호 | `8000` |


### 2. OCR 서버 실행
**Docker 사용 시:**
```bash
cd server
docker-compose up --build
```

**Python 직접 실행 시:**
```bash
cd server
pip install -r requirements.txt
python main.py
```

### 3. 클라이언트 실행 (Windows 전용)
Deno가 설치되어 있어야 하며, 관리자 권한이 필요할 수 있습니다.
```bash
deno task dev
```

---

## 릴리즈 배포 방법

1. **서버 배포**: `server/docker-compose.yaml`을 사용하여 GPU 지원 환경에서 OCR 서버를 배포합니다.
2. **클라이언트 배포**: Deno가 설치된 Windows 환경에서 `main.ts`를 실행하거나 `deno compile`을 통해 실행 파일로 빌드하여 배포할 수 있습니다. (FFI 사용으로 인해 대상 환경에 `user32.dll`이 필요합니다.)

---

## 각 기능 설명

### 1. PrintScreen 자동 번역
- **실행 경로**: `PrintScreen` 키 입력 -> 클립보드 캡처 -> 이미지 리사이징 -> OCR 서버 전송 -> 텍스트 추출 -> AI 번역 -> 텔레그램 메시지 전송.
- **설명**: 사용자가 화면을 캡처하면 즉시 해당 화면 내의 텍스트를 인식하여 번역된 결과를 텔레그램으로 보내줍니다. 인식된 텍스트가 많을 경우 선택할 수 있는 인라인 버튼을 제공합니다.

### 2. 텔레그램 봇 상호작용
- **실행 경로**: 텔레그램 메시지 입력 -> AI 번역 -> 응답 전송.
- **설명**: 봇에게 직접 텍스트를 보내면 설정된 시스템 프롬프트에 따라 내용을 처리하여 답변합니다.

---

## 특이 사항

- **Windows 전용**: 클라이언트의 키보드 후킹 기능은 Windows의 `user32.dll`을 사용하므로 Windows 환경에서만 작동합니다.
- **GPU 가속**: OCR 서버는 성능을 위해 NVIDIA GPU와 CUDA 환경을 권장합니다. (CPU로도 구동 가능하나 속도가 느릴 수 있습니다.)
- **FFI 설정**: Deno 실행 시 `--allow-ffi`와 `--unstable-ffi` 플래그가 필요합니다.
