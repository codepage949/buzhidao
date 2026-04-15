# Buzhidao (不知道)

> Buzhidao는 화면의 텍스트를 OCR로 추출하고 AI로 번역하여, 오버레이와 팝업 UI로 보여주는 Tauri 데스크톱 앱입니다.

---

## 주요 기능

### 1. 원클릭 화면 캡처 및 OCR
- 단축키: 전역 조합키 기본값 `Ctrl+Alt+A` (macOS는 `Cmd+Shift+A`)
- 동작: 어떤 화면에서든 캡처 단축키를 누르면 즉시 현재 화면을 캡처하고, 외부 `ocr_server` 실행 파일로 텍스트 영역을 검출합니다.

### 2. 인터랙티브 오버레이 UI
- 동작: OCR 분석 결과가 화면 전체에 투명 오버레이로 표시됩니다.
- 상세: 검출된 각 텍스트 블록은 클릭 가능하며, 선택 즉시 번역 단계로 이어집니다.

### 3. 스마트 AI 번역 및 팝업
- 동작: 선택한 텍스트를 AI 모델로 번역해 별도 팝업에 표시합니다.
- 상세: 단순 치환이 아니라 문맥에 맞는 번역을 목표로 하며, 마크다운 렌더링을 지원합니다.

---

## 기술 스택

### Desktop App
- **Rust**: 시스템 로직, OCR 파이프라인, Tauri 백엔드
- **Tauri 2.x**: 데스크톱 앱 프레임워크
- **React 19 + TypeScript**: 오버레이 및 팝업 UI
- **Deno + Vite 6**: 프런트엔드 개발 서버, 빌드, 테스트

### OCR Model Tooling
- **PaddleOCR / PaddlePaddle**: OCR 서버 추론
- **PyInstaller**: OCR 서버 실행 파일 패키징
- **uv**: OCR 서버 Python 환경 관리

---

## 프로젝트 구조

```text
.
├── app/            # Tauri 앱: Rust 백엔드 + React UI
├── ocr_server/     # PaddleOCR sidecar 프로젝트 (uv + PyInstaller)
└── docs/changes/   # 변경 기록
```

---

## 시작하기

### 1. 환경 변수 준비

`app/.env` 파일을 만들고 필요한 값을 설정합니다.

주요 항목:
- `SOURCE`
- `AI_GATEWAY_API_KEY`
- `AI_GATEWAY_MODEL`
- `SYSTEM_PROMPT_PATH` (선택)
- `WORD_GAP`
- `LINE_GAP`
- `OCR_DEBUG_TRACE` (선택, 기본 `false`) : 터미널에 `rec` accept/reject 로그 출력, 오버레이에 raw 박스 표시
- `OCR_SERVER_DEVICE` (선택, 기본 `cpu`) : `cpu` 또는 `gpu`
- `OCR_SERVER_EXECUTABLE`
- `CAPTURE_SHORTCUT` (선택) : Tauri Accelerator 문자열. 비우면 플랫폼 기본값 사용

예시는 [app/.env.example](app/.env.example)에 있습니다.

### 2. OCR 서버 준비

`ocr_server`는 `uv`와 `PyInstaller`로 빌드합니다.

```bash
cd ocr_server
uv sync -p 3.13 --group build --group cpu
uv run --group build --group cpu python build.py
```

산출물: `ocr_server/dist/ocr_server/ocr_server.exe`

GPU 빌드는 `paddlepaddle-gpu` 그룹을 사용합니다.

```bash
cd ocr_server
uv sync -p 3.13 --group build --group gpu
uv run --group build --group gpu python build.py --gpu
```

`gpu` 그룹은 `ocr_server/pyproject.toml`에 설정된
`https://www.paddlepaddle.org.cn/packages/stable/cu118/` 인덱스를 사용합니다.
앱에서는 `app/.env`에 `OCR_SERVER_DEVICE=gpu`를 함께 설정해야 합니다.
Windows GPU 빌드에서는 같은 인덱스의 `nvidia-* cu11` wheel을 함께 설치해
`cudnn64_8.dll` 등 CUDA/cuDNN DLL을 PyInstaller 산출물에 포함합니다.
GPU 산출물도 `ocr_server/dist/ocr_server/ocr_server.exe`에 생성됩니다.
앱의 장치 선택 UI는 런타임 `.env`가 아니라 앱 빌드 feature로 결정됩니다.
GPU 앱은 `cargo tauri build --features gpu`처럼 빌드해야 합니다.

기본 빌드는 `onedir`입니다. `onefile`이 필요하면 `python build.py --onefile`을 사용합니다.

### 3. 데스크톱 앱 실행

```bash
cd app
cargo tauri dev
```

프런트엔드 의존성이 비어 있거나 디렉터리 이동 직후라면 한 번 실행합니다.

```bash
cd app/ui
deno install
```

---

## 테스트 및 품질

- Rust 앱 테스트:

```bash
cd app
cargo test
```

- UI 테스트:

```bash
cd app/ui
deno test
```

---

## 릴리즈

```bash
cd app
cargo tauri build
```
