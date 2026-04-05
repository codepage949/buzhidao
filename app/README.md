# Buzhidao App (不知道)

이 디렉토리는 Buzhidao의 Tauri 데스크톱 앱을 포함합니다. 앱은 화면의 텍스트를 OCR로 추출하고, AI로 번역한 뒤 오버레이와 팝업 UI로 보여줍니다.

## 기술 스택

### Desktop App
- **Runtime**: [Tauri 2.x](https://tauri.app/)
- **Language**: Rust, TypeScript
- **Main Libraries**:
    - `tauri`: 데스크톱 앱 셸 및 윈도우 관리
    - `rdev`: Windows 전역 키 후킹
    - `reqwest`: OCR 서버 및 AI 게이트웨이 통신
    - `screenshots`, `image`: 화면 캡처와 PNG 변환
    - `React`, `Vite`, `Deno`: 오버레이/팝업 프런트엔드

### OCR Server
- **Framework**: [FastAPI](https://fastapi.tiangolo.com/)
- **Language**: Python (uv 패키지 매니저 사용)
- **OCR Engine**: [PaddleOCR](https://github.com/PaddlePaddle/PaddleOCR) (GPU 가속 지원)
- **Deployment**: Docker, Docker Compose

---

## 프로젝트 구조

```text
app/
├── Cargo.toml
├── Cargo.lock
├── build.rs
├── tauri.conf.json
├── .env.example
├── icons/
├── capabilities/
├── src/
├── ui/
│   └── src/
└── ../ocr/             # OCR 서버 (루트 기준 형제 디렉토리)
```

---

## 개발 및 테스트 방법

### 1. 환경 변수 설정
`app/` 디렉토리에 `.env` 파일을 생성하고 필요한 값을 설정합니다. (상세 항목은 `.env.example` 참조)

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
Tauri 앱과 프런트엔드는 모두 `app/` 아래에 있습니다.

```bash
# 개발 모드 실행
cargo tauri dev

# Rust 테스트 실행
cargo test

# 프런트엔드 테스트 실행
deno task --config ui/deno.json test
```

---

## 릴리즈 배포 방법

1. **서버 배포**: NVIDIA Docker Runtime이 설치된 서버에서 `docker-compose`를 사용하여 배포합니다.
2. **클라이언트 배포**: Tauri 번들을 생성합니다.
   ```bash
   cargo tauri build
   ```

---

## 각 기능 설명

### 1. 화면 OCR 오버레이 실행
- **실행 경로**: `PrintScreen` 키 입력 감지 -> 전체 화면 캡처 -> OCR 서버 전송 -> 오버레이 표시 -> 텍스트 선택 -> AI 번역 -> 팝업 표시.
- **상세**: 사용자가 `PrtSc`를 누르면 앱이 키를 가로채 화면을 캡처하고 OCR 결과를 오버레이로 띄웁니다. 사용자가 영역을 클릭하면 번역 팝업이 표시됩니다.

---

## 특이 사항

- **Windows 의존성**: `rdev` 기반 전역 키보드 훅과 Tauri 오버레이 창 동작은 Windows 사용 환경을 전제로 합니다.
- **UI 빌드 경로**: Tauri 앱은 `app/`에서 실행되며 프런트엔드 정적 파일은 `ui/dist`를 참조합니다.
- **이미지 최적화**: OCR 서버 부하를 줄이기 위해 가로 폭이 1024px를 넘는 캡처 이미지는 축소 후 전송합니다.
