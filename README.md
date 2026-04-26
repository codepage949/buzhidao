# Buzhidao

Buzhidao는 화면 캡처 영역의 텍스트를 OCR로 인식하고 번역 팝업으로 보여 주는 Tauri 데스크톱 앱입니다.

## 기술 스택

- Rust 2021, Tauri 2
- React 19, TypeScript, Vite 8, Deno 2
- Paddle Inference FFI OCR 백엔드

## 프로젝트 구조

```text
.
├── src/                    # Tauri 백엔드, 설정, 창 제어, OCR/번역 실행 흐름
├── native/paddle_bridge/   # Paddle Inference C++ FFI 브리지
├── ui/                     # overlay / popup / loading / settings 프런트엔드
├── tools/scripts/          # SDK 구성, CUDA 런타임 구성, 릴리즈 보조 스크립트
├── tools/ocr_sidecar_compare/
│   └── ...                 # Python sidecar 비교/벤치용 보조 프로젝트
├── capabilities/           # Tauri capability 설정
├── icons/                  # 앱/트레이 아이콘
├── testdata/ocr/           # OCR smoke/parity 테스트 이미지
├── .env.example            # 런타임 설정 예시
├── Cargo.toml              # Rust/Tauri 의존성 및 feature
└── tauri.conf.json         # Tauri 창/빌드 설정
```

## 개발 및 테스트 방법

### 기본 준비

`.env.example`을 참고해 `.env`를 만들고, 번역 요청에 사용할 `.prompt`를 준비합니다. 개발 빌드는 프로젝트 루트의 `.env`/`.prompt`를 읽고, 배포 빌드는 앱 데이터 디렉터리의 `.env`/`.prompt`를 읽습니다.

CPU OCR 개발 환경:

```bash
python tools/scripts/setup_paddle_inference.py --destination-dir .paddle_inference
cargo tauri dev
```

Windows/Linux GPU OCR 개발 환경:

```bash
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir .paddle_inference
python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda
cargo tauri dev --features gpu
```

Linux에서 실행 시 런타임 라이브러리 검색 경로가 필요하면 아래처럼 지정합니다.

```bash
export LD_LIBRARY_PATH="$(pwd)/.cuda:$(pwd)/.paddle_inference/lib:$(pwd)/.paddle_inference/paddle/lib:${LD_LIBRARY_PATH:-}"
cargo tauri dev
```

### 테스트

Rust 테스트:

```bash
cargo test
```

GPU feature 포함 Rust 테스트:

```bash
cargo test --features gpu
```

프런트엔드 테스트:

```bash
cd ui
deno task test
```

스크립트 테스트:

```bash
python -m unittest \
  tools.scripts.test_release_helper \
  tools.scripts.test_update_release_version \
  tools.scripts.test_setup_cuda_runtime \
  tools.scripts.test_setup_paddle_inference \
  tools.scripts.test_release_binary_smoke \
  tools.scripts.test_release_workflow
```

릴리즈 OCR smoke는 앱 아카이브를 만든 뒤 실제 실행 파일을 압축 해제해 실행합니다.

```bash
python tools/scripts/release_binary_smoke.py \
  --archive dist/buzhidao-v0.5.0-linux-amd64-cpu-app.tar.gz \
  --os linux \
  --image testdata/ocr/test.png \
  --model-root .paddle_models \
  --source ch \
  --device cpu
```

## 릴리즈 배포 방법

수동 로컬 빌드:

```bash
cargo tauri build --no-bundle
```

GPU 로컬 빌드:

```bash
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir .paddle_inference
python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda
cargo tauri build --no-bundle --features gpu
```

GitHub Actions 릴리즈는 `.github/workflows/release.yml`의 `workflow_dispatch`로 실행합니다.

1. 입력 버전 예: `v0.5.0`
2. Windows/Linux, CPU/GPU 조합별로 SDK 구성, 프런트 빌드, 앱 빌드, OCR smoke 테스트를 수행합니다.
3. 검증이 모두 성공하면 `Cargo.toml`, `Cargo.lock`, `tauri.conf.json` 버전을 갱신해 커밋합니다.
4. 갱신된 커밋 기준으로 앱 아카이브와 설치 스크립트를 만들고 GitHub Release를 생성합니다.

릴리즈 자산 이름은 `buzhidao-<version>-<os>-<arch>-<flavor>-app.zip` 또는 `.tar.gz` 형식입니다. GPU 자산이 커지면 `.part001` 형태로 분할될 수 있으며, 함께 업로드되는 설치 스크립트가 병합과 압축 해제를 처리합니다.

## 각 기능 설명

### 앱 시작과 설정

- 시작 시 `loading` 창을 먼저 표시하고 OCR warmup이 끝난 뒤 닫습니다.
- 필수 설정이 없거나 OCR warmup이 실패하면 `loading` 또는 `settings`에서 원인을 보여 줍니다.

### 캡처와 오버레이

- 기본 전역 단축키는 Windows/Linux `Ctrl+Alt+A`, macOS `Cmd+Shift+A`입니다.
- 캡처 후 `overlay` 전체 화면 창에 OCR 영역을 표시합니다.

### OCR 실행 경로

- 앱 런타임 OCR 백엔드는 Paddle FFI 단일 경로입니다.
- 필요한 PaddleOCR det/cls/rec 모델이 캐시에 없으면 앱이 PP-OCRv5 공식 모델을 자동 다운로드합니다.

### 트레이와 단일 인스턴스

- 시스템 트레이에서 `설정...`, `종료` 메뉴를 제공합니다.
- 앱이 이미 실행 중이면 새 프로세스를 띄우지 않고 기존 인스턴스 포커스 경로를 사용합니다.

## 특이 사항

- `tauri.conf.json`의 `bundle.active`는 현재 `false`라 릴리즈 workflow도 `cargo tauri build --no-bundle` 기준으로 앱 아카이브를 만듭니다.
- Paddle Inference SDK는 `.paddle_inference`, GPU CUDA 런타임은 `.cuda`에 둡니다. GPU SDK와 CUDA 런타임은 CUDA 12.6/cuDNN 9.5.1 계열로 맞춥니다.
- Windows는 빌드 과정에서 필요한 DLL을 출력 디렉터리로 복사합니다. Linux는 필요 시 `LD_LIBRARY_PATH`에 `.cuda`와 `.paddle_inference` 라이브러리 경로를 포함해야 합니다.
- Linux 빌드에는 OpenCV, PipeWire, WebKitGTK 등 시스템 패키지가 필요하며 `tools/scripts/install_linux_build_deps.sh`가 CI와 로컬 준비 경로를 공유합니다.
