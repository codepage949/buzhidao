# buzhidao

윈도우에서 `PrtSc` 키를 눌러 현재 창을 캡처하면, OCR로 텍스트를 추출한 뒤 LLM으로 번역해서 텔레그램 봇으로 보내주는 도구입니다.

## 구성

- 클라이언트: Deno 기반 윈도우 앱
- 서버: FastAPI + PaddleOCR
- 전달 채널: Telegram Bot API
- 번역: OpenAI 호환 API (`ai-gateway.vercel.sh`)

## 동작 방식

1. 윈도우에서 현재 활성 창을 선택합니다.
2. `PrtSc` 키를 누르면 클라이언트가 화면 이미지를 가져옵니다.
3. 클라이언트가 OCR 서버(`/infer/{src}`)로 이미지를 전송합니다.
4. 추출된 텍스트를 번역 프롬프트와 함께 LLM에 전달합니다.
5. 번역 결과를 텔레그램 봇이 지정된 채팅방으로 전송합니다.

텍스트 조각이 많을 경우에는 바로 번역하지 않고, 텔레그램 인라인 버튼으로 번역할 문장을 선택하게 됩니다.

## 요구 사항

### 클라이언트

- Windows
- Deno

클라이언트는 `user32.dll`과 클립보드 접근을 사용하므로 사실상 윈도우 전용입니다.

### 서버

- Python 3
- PaddleOCR 실행 환경
- GPU 지원 환경 권장

현재 서버 코드는 PaddleOCR를 `device="gpu"`로 고정해서 사용하므로, GPU가 없는 환경에서는 그대로 실행되지 않을 수 있습니다.

## 환경변수 설정

### 클라이언트

루트 디렉터리에서 `.env.example`을 복사해 `.env`를 만든 뒤 값을 채웁니다.

```bash
cp .env.example .env
```

주요 항목:

- `SOURCE`: OCR 대상 언어. `en` 또는 `ch`
- `API_BASE_URL`: OCR 서버 주소. 예: `http://127.0.0.1:8000`
- `AI_GATEWAY_API_KEY`: 번역에 사용할 API 키
- `AI_GATEWAY_MODEL`: 사용할 모델 이름
- `SYSTEM_PROMPT_PATH`: 시스템 프롬프트 파일 경로
- `TELEGRAM_API_BASE_URL`: 기본값 `https://api.telegram.org`
- `BOT_TOKEN`: BotFather로 발급받은 텔레그램 봇 토큰
- `CHAT_ID`: 번역 결과를 받을 채팅 ID
- `X_DELTA`, `Y_DELTA`: OCR로 분리된 텍스트를 한 문장으로 묶는 기준

`CHAT_ID`를 모르면 비워 둔 상태로 먼저 실행한 뒤, 봇에 아무 메시지나 보내면 해당 ID를 다시 봇이 알려줍니다. 이후 `.env`에 반영하고 재실행하면 됩니다.

`SYSTEM_PROMPT_PATH`에 지정한 파일은 별도로 직접 만들어야 합니다.

### 서버

`server/.env.example`을 복사해 `server/.env`를 만듭니다.

```bash
cp server/.env.example server/.env
```

주요 항목:

- `HTTP_HOST`: 서버 바인드 주소
- `HTTP_PORT`: 서버 포트
- `SCORE_THRESH`: OCR 인식 점수 임계값

## 실행 방법

### 1. OCR 서버 실행

#### 로컬 Python 실행

```bash
cd server
pip install -r requirements.txt
python main.py
```

기본 포트는 `8000`입니다.

#### Docker Compose 실행

```bash
cd server
docker compose up --build
```

`docker-compose.yaml`에는 NVIDIA GPU 예약 설정이 포함되어 있습니다.

### 2. 클라이언트 실행

루트 디렉터리에서 실행합니다.

```bash
deno task dev
```

## 사용법

1. 번역하고 싶은 창을 활성화합니다.
2. `PrtSc` 키를 누릅니다.
3. OCR 결과가 적으면 바로 번역 결과가 텔레그램으로 전송됩니다.
4. OCR 결과가 많으면 텔레그램에서 원하는 문장을 선택해 개별 번역할 수 있습니다.

## 주의 사항

- 클라이언트는 윈도우 전용입니다.
- 서버는 현재 GPU 사용을 전제로 작성되어 있습니다.
- `API_BASE_URL`에는 포트까지 포함해야 합니다. 예: `http://127.0.0.1:8000`
- 시스템 프롬프트 파일이 없으면 번역 요청 단계에서 실패합니다.
