# Buzhidao (不知道)

> Buzhidao는 화면의 텍스트를 OCR로 추출하고 AI로 번역하여, 오버레이와 팝업 UI로 보여주는 Tauri 데스크톱 앱입니다.

---

## 주요 기능

### 1. 원클릭 화면 캡처 및 OCR
- 단축키: `PrintScreen` 전역 후킹
- 동작: 어떤 화면에서든 `PrtSc`를 누르면 즉시 현재 화면을 캡처하고, 앱 내부 ONNX OCR 엔진으로 텍스트 영역을 검출합니다.

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
- **ONNX Runtime (`ort`)**: 앱 내부 OCR 추론

### OCR Model Tooling
- **PaddleOCR / paddle2onnx**: 모델 변환 기준
- **Docker**: 운영체제 차이를 피한 ONNX 모델 변환 환경

---

## 프로젝트 구조

```text
.
├── src/            # Rust 백엔드: 윈도우 제어, OCR, 번역 서비스
├── ui/             # React 프런트엔드: 오버레이 및 팝업 UI
├── models/         # ONNX OCR 모델 산출물
├── icons/          # 앱 아이콘
├── capabilities/   # Tauri 권한 설정
├── scripts/        # 모델 변환 및 비교 스크립트
└── docs/changes/   # 변경 기록
```

---

## 시작하기

### 1. 환경 변수 준비

루트의 `.env` 파일을 만들고 필요한 값을 설정합니다.

주요 항목:
- `SOURCE`
- `AI_GATEWAY_API_KEY`
- `AI_GATEWAY_MODEL`
- `SYSTEM_PROMPT_PATH` (선택)
- `WORD_GAP`
- `LINE_GAP`
- `DET_THRESH` (선택, 기본 0.2)
- `BOX_THRESH` (선택, 기본 0.4)
- `OCR_DEBUG_TRACE` (선택, 기본 `false`) : 터미널에 `rec` accept/reject 로그 출력, 오버레이에 raw 박스 표시

예시는 [.env.example](.env.example)에 있습니다.

### 2. OCR 모델 준비

모델이 없으면 Docker 기반 변환 스크립트로 생성합니다.

```bash
python scripts/export_onnx.py
```

산출물:
- `models/det.onnx`
- `models/cls.onnx`
- `models/rec.onnx`
- `models/rec_dict.txt`

### 3. 데스크톱 앱 실행

CPU only 개발 실행:

```bash
cargo tauri dev
```

GPU 개발 실행:

```bash
cargo tauri dev --features gpu
```

프런트엔드 의존성이 비어 있거나 디렉터리 이동 직후라면 한 번 실행합니다.

```bash
cd ui
deno install
cd ..
```

---

## 테스트 및 품질

- Rust 앱 테스트:

```bash
cargo test
```

- GPU 빌드 경로 테스트:

```bash
cargo test --features gpu
```

- UI 테스트:

```bash
cd ui
deno task test
```

---

## 릴리즈

CPU only 빌드:

```bash
cargo tauri build
```

GPU 빌드:

```bash
cargo tauri build --features gpu
```

GPU 빌드는 CUDA/cuDNN이 준비된 배포 환경을 대상으로 별도 아티팩트로 관리하는 것이 안전합니다.

GitHub Actions 릴리즈:

- `.github/workflows/release.yml`의 수동 실행(`workflow_dispatch`)로 Windows 릴리즈를 생성합니다.
- 입력 버전 태그는 `v1.2.3` 형식을 사용하고, 내부 `Cargo.toml`/`tauri.conf.json` 버전은 `1.2.3`으로 동기화됩니다.
- 산출물은 두 개로 분리됩니다.
  - `windows-x64-cpu`: 기본 배포용, `models/` 포함
  - `windows-x64-gpu`: GPU 대상 배포용, `models/` + `cuda/` 포함
- 모델은 릴리스 workflow 내부의 Ubuntu job에서 Docker 기반으로 직접 ONNX 변환해 생성합니다.
- GPU 릴리즈 job은 CI에서 NVIDIA PyPI wheel을 내려받아 필요한 CUDA/cuDNN DLL만 추출해 포함합니다.
- 기본 배포는 CPU only를 권장하고, GPU ZIP은 NVIDIA 드라이버가 준비된 환경에서만 배포합니다.
