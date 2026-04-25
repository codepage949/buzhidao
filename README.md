# Buzhidao App

프로젝트 루트가 Buzhidao 데스크톱 애플리케이션 본체입니다. Tauri 백엔드, 윈도우 구성, 런타임 설정, 프런트엔드 빌드 연결을 담당합니다.

## 기술 스택

- Rust
- Tauri 2.x
- `tauri-plugin-global-shortcut`
- `tauri-plugin-single-instance`
- React + TypeScript + Vite 8

## 프로젝트 구조

```text
.
├── src/                # Tauri 백엔드, 창 제어, OCR/번역 실행 흐름
├── ui/                 # overlay / popup / loading / settings 프런트엔드
├── tools/              # 비교/배포/설치용 보조 프로젝트와 스크립트
├── capabilities/       # Tauri capability 설정
├── icons/              # 앱/트레이 아이콘
├── .env.example        # 런타임 설정 예시
├── .prompt             # 시스템 프롬프트 파일
├── Cargo.toml          # Rust / Tauri 의존성
└── tauri.conf.json     # 윈도우 / 빌드 설정
```

## 개발 및 테스트 방법

앱 실행:

```bash
cargo tauri dev
```

기본 개발 실행은 `paddle-ffi` feature를 포함합니다. 따라서 `.paddle_inference`가 준비되어 있어야 warmup이 정상 완료됩니다.

Rust 테스트:

```bash
cargo test
```

프런트엔드 테스트:

```bash
cd ui
deno task test
```

## 릴리즈 배포 방법

기본 빌드:

```bash
cargo tauri build
```

GPU 앱 빌드:

```powershell
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir .paddle_inference
python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda
cargo tauri build --features gpu
```

## 각 기능 설명

### 앱 시작

- 시작 시 `loading` 창이 먼저 표시됩니다.
- `.env`와 `.prompt`를 준비하고 OCR 엔진 warmup이 끝나면 `loading` 창을 닫습니다.
- OCR 엔진 초기화나 warmup이 실패하면 `loading` 창이 실패 상태로 전환되고 종료 버튼이 표시됩니다.
- warmup 전에는 전역 캡처 단축키 입력을 막습니다.

### 전역 캡처

- 기본 캡처 단축키는 Windows/Linux `Ctrl+Alt+A`, macOS `Cmd+Shift+A`입니다.
- 설정 창에서 값을 바꾸면 즉시 재등록됩니다.
- 새 단축키 등록 실패나 저장 실패가 나면 이전 단축키로 롤백합니다.

### 윈도우 구성

- `overlay`: 전체 화면 OCR 결과 표시
- `popup`: 번역 결과 표시
- `loading`: 시작 warmup 진행 표시
- `settings`: 설정 편집

`settings` 창은 hide/show 재사용이 아니라 실제로 닫혔다가, 다시 열 때 새로 생성됩니다.

### 설정 관리

- 개발 빌드는 프로젝트 루트의 `.env`와 `.prompt`를 읽고, 배포 빌드는 앱 데이터 디렉터리의 `.env`와 `.prompt`를 읽습니다.
- 두 경로를 동시에 읽지 않고, 빌드 모드에 따라 하나만 선택합니다.
- `get_user_settings`, `save_user_settings` 커맨드로 프런트와 동기화합니다.
- 필수 설정 누락 시 설정 창을 자동으로 열고 안내 메시지와 강조 필드를 전달합니다.
- settings 창은 OCR busy 상태를 구독하며, OCR 진행 중에는 저장 버튼을 전체 비활성화합니다.
- OCR 종료 시 settings 창의 저장 버튼은 자동으로 다시 활성화됩니다.

### 트레이

- 시스템 트레이에서 `설정…`, `종료` 메뉴를 제공합니다.
- 단일 인스턴스 모드라 이미 실행 중이면 기존 앱 포커스 경로를 사용합니다.

## 특이 사항

- `bundle.active`는 현재 `false`입니다.
- 개발 빌드에서는 `ui`의 Vite dev server(`http://localhost:1420`)를 사용합니다.
- 앱 OCR 백엔드는 Paddle FFI 단일 모드입니다. Python sidecar는 앱 런타임 백엔드로 사용하지 않습니다.
- 기본 PaddleOCR 캐시는 `%USERPROFILE%\\.paddlex\\official_models`(Windows) 또는 `~/.paddlex/official_models`(Linux/macOS)입니다. 과거 설치 경로인 `~/.paddleocr`도 호환됩니다.
- 선택된 OCR 언어에 필요한 공식 det/cls/rec 모델이 캐시에 없으면, 앱이 PaddleOCR upstream PP-OCRv5 규칙에 맞는 모델을 자동 다운로드합니다.
- 모델 캐시에는 `det/cls/rec` 파일/폴더(예: `det`, `rec`) 또는 공식 서브디렉터리(예: `PP-OCRv5_server_det`, `PP-LCNet_x1_0_textline_ori`)가 포함되어야 합니다.
- Paddle Inference 배포 패키지 루트는 `.paddle_inference`이며,
  `.../paddle/include + .../paddle/lib` 형태(또는 `{root}/include + {root}/lib`, `{root}/paddle_inference/include + {root}/paddle_inference/lib`)여야 합니다. Paddle 소스 트리(`.../Paddle`)를 그대로 두면 링크되지 않습니다.
- `.paddle_inference`가 비어있으면 OS별 최신 v3 패키지를 프로젝트 루트에 내려받아 배치합니다.
- `.paddle_inference` 최상위 또는 `.paddle_inference/paddle`에 `include`/`lib`가 있도록 정리해야 합니다.
- Linux/macOS는 `tools/scripts/setup_paddle_inference.py`로 아카이브를 바로 정리할 수 있습니다.
- Windows/Linux GPU용 Paddle Inference SDK는 `tools/scripts/setup_paddle_inference.py --device gpu`로 받을 수 있습니다. GPU SDK는 CUDA 12.6/cuDNN 9.5.1 계열로 고정합니다.
- GPU 실행에 필요한 CUDA 런타임 DLL/SO는 `tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126`로 `.cuda` 아래에 구성할 수 있습니다.
- GPU feature 빌드는 `.cuda`의 런타임 DLL/SO를 출력 폴더로 복사하고, 앱 시작 시 `.cuda`를 런타임 검색 경로에 추가합니다.
- `tools/scripts/setup_paddle_inference.py`는 sidecar 기준 런타임 버전 매니페스트를 같이 기록하고, `pyclipper 1.4.0` C++ 소스를 `.paddle_inference/third_party/` 아래에 내려받습니다.
- Windows에서는 `OpenCV 4.10.0` SDK를 `.paddle_inference/third_party/opencv-sdk/windows-x86_64` 아래에 자동 배치합니다.
- Linux/macOS에서는 `--opencv-sdk-dir`로 기존 OpenCV SDK를 `.paddle_inference/third_party/opencv-sdk/<platform>` 아래로 가져와야 합니다.
- SDK 아카이브는 기본적으로 OS 임시 디렉터리 아래 `buzhidao-paddle-inference/`에 내려받습니다. 저장 위치를 바꾸려면 `--download-dir`를 지정합니다.
- `tools/scripts/ocr_sidecar_ffi.py`는 sidecar/FFI 비교와 벤치의 단일 엔트리포인트입니다. 이 경로는 `OCR_SERVER_PYTHON` 또는 `tools/ocr_sidecar_compare/.venv`의 플랫폼별 Python 실행 파일을 사용합니다.

Paddle FFI 실행 시 `.paddle_inference` 배치는 필요합니다. 다만 런타임 라이브러리 경로 처리는 플랫폼별로 다릅니다.

- Windows: `build.rs`가 `.paddle_inference` 아래 DLL을 `target/debug`와 `target/debug/deps`로 복사하므로, 보통 `cargo tauri dev` 전에 `PATH`를 직접 추가할 필요는 없습니다.
- Linux/macOS: 런타임 라이브러리 검색 경로를 직접 설정해야 하며, OpenCV는 `.paddle_inference/third_party/opencv-sdk/<platform>` 아래에 준비되어 있어야 합니다.

```powershell
$inferenceDir = Join-Path (Get-Location) ".paddle_inference"
python tools/scripts/setup_paddle_inference.py --destination-dir $inferenceDir
cargo tauri dev
```

```powershell
# Windows GPU
$inferenceDir = Join-Path (Get-Location) ".paddle_inference"
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir $inferenceDir
python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda
cargo tauri dev --features gpu
```

```bash
# Linux/macOS: Paddle Inference v3 아카이브를 자동 다운로드해 정리
inferenceDir="$(pwd)/.paddle_inference"
python tools/scripts/setup_paddle_inference.py --destination-dir "$inferenceDir"

# Linux GPU: Paddle Inference SDK와 CUDA 런타임 모두 CUDA 12.6 계열 사용
# python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir "$inferenceDir"
# python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda

# Linux/macOS: 필요하면 기존 OpenCV SDK를 .paddle_inference 아래로 가져오기
# python tools/scripts/setup_paddle_inference.py --destination-dir "$inferenceDir" --opencv-sdk-dir "/path/to/opencv-sdk"

# Linux: 런타임 라이브러리 검색 경로
export LD_LIBRARY_PATH="$(pwd)/.cuda:$inferenceDir/lib:$inferenceDir/paddle/lib:$inferenceDir/paddle_inference/lib:${LD_LIBRARY_PATH:-}"
# macOS: 런타임 라이브러리 검색 경로
export DYLD_LIBRARY_PATH="$inferenceDir/lib:$inferenceDir/paddle/lib:$inferenceDir/paddle_inference/lib:${DYLD_LIBRARY_PATH:-}"

cargo tauri dev
```
