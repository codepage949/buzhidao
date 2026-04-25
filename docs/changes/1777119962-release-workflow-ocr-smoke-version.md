# 릴리즈 workflow에 OCR smoke와 버전 갱신 반영

## 배경

- 현재 release workflow는 최신 코드베이스에서 필요한 `.paddle_inference`, OpenCV SDK, `.cuda` 준비 단계를 실행하지 않는다.
- `build.rs`는 `.paddle_inference`와 OpenCV SDK를 전제로 빌드하고, GPU feature 빌드는 `.cuda` 런타임을 같이 사용한다.
- 릴리즈 산출물은 앱 실행 파일만 압축하고 있어, 빌드 출력 폴더에 복사된 Paddle/OpenCV/CUDA 런타임 DLL/SO를 함께 담지 못할 수 있다.
- 사용자는 빌드 후 OCR 성공 테스트를 거친 뒤, 성공한 경우에만 `Cargo.toml` 버전을 업데이트하고 배포를 이어가길 원한다.

## 구현 계획

1. 릴리즈 workflow를 최신 코드베이스 빌드 전제에 맞춘다.
   - CPU/GPU flavor별로 `setup_paddle_inference.py`를 실행한다.
   - GPU flavor는 `setup_cuda_runtime.py --package-set paddle-cu126`를 실행한다.
   - Linux는 apt OpenCV dev 패키지를 `.paddle_inference/third_party/opencv-sdk/linux-x86_64` 형태로 연결할 최소 SDK 레이아웃을 만든다.
2. 빌드 후 OCR smoke test를 추가한다.
   - GUI 앱 실행 대신 FFI OCR 경로를 `cargo test`로 실행한다.
   - CI runner에는 GPU가 없으므로 GPU flavor도 `OCR_SERVER_DEVICE=cpu` smoke로 OCR 엔진 생성, warmup, 1회 OCR 실행 성공을 검증한다.
3. OCR smoke가 모두 통과한 뒤 버전을 갱신한다.
   - `vX.Y.Z` 입력을 `X.Y.Z`로 정규화한다.
   - `Cargo.toml`, `Cargo.lock`, `tauri.conf.json` 버전을 같이 갱신한다.
   - 변경이 있으면 릴리즈 브랜치에 버전 커밋을 push하고, 이후 배포는 해당 커밋을 기준으로 진행한다.
4. 실제 릴리즈 산출물 빌드는 버전 갱신 커밋에서 다시 수행한다.
   - 빌드 출력 폴더의 런타임 DLL/SO를 앱 아카이브에 함께 포함한다.
5. 핵심 스크립트와 release helper 동작을 단위 테스트로 검증한다.

## 구현 내용

- `.github/workflows/release.yml`
  - 기존 단일 `build -> release` 흐름을 `verify -> version -> build -> release`로 분리했다.
  - `verify` job은 CPU/GPU, Windows/Linux matrix에서 native SDK를 준비하고 앱을 빌드한 뒤 OCR smoke를 실행한다.
  - `version` job은 모든 verify matrix가 성공한 뒤 `Cargo.toml`, `Cargo.lock`, `tauri.conf.json` 버전을 갱신하고 필요하면 릴리즈 브랜치에 커밋을 push한다.
  - 실제 릴리즈용 `build` job은 버전 갱신 커밋을 checkout해 다시 빌드하고 아카이브를 만든다.
  - `release` job의 태그와 GitHub Release는 버전 갱신 커밋을 기준으로 생성한다.
- native SDK 준비
  - workflow에서 `tools/scripts/setup_paddle_inference.py`를 실행해 `.paddle_inference`를 구성한다.
  - GPU flavor에서는 `tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126`로 `.cuda`를 구성한다.
  - Linux는 `libopencv-dev`를 설치하고, 필요한 OpenCV header/lib만 `.opencv-sdk-ci`에 배치해 `--opencv-sdk-dir`로 가져온다.
- OCR smoke
  - `services::ocr_pipeline::tests::릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다`를 추가했다.
  - `BUZHIDAO_RUN_RELEASE_OCR_SMOKE=1`일 때만 실제 실행한다.
  - 모델을 먼저 보장한 뒤 FFI OCR engine 생성, warmup, 흰색 이미지 1회 OCR 실행이 성공하는지 확인한다.
  - GitHub-hosted runner에는 GPU가 없으므로 workflow smoke는 `OCR_SERVER_DEVICE=cpu`로 실행한다.
- 버전 갱신 스크립트
  - `tools/scripts/update_release_version.py`를 추가했다.
  - 입력 `vX.Y.Z`를 `X.Y.Z`로 정규화한다.
  - `Cargo.toml`, `Cargo.lock`, `tauri.conf.json`을 같이 갱신한다.
- 릴리즈 아카이브
  - `tools/scripts/release_helper.py`의 앱 레이아웃 생성이 앱 실행 파일뿐 아니라 같은 출력 폴더의 런타임 DLL/SO도 함께 복사하도록 보완했다.
  - GPU/CPU 빌드에서 `build.rs`가 출력 폴더에 복사한 Paddle/OpenCV/CUDA 런타임을 아카이브에 포함하기 위함이다.
- Linux 빌드 의존성
  - `tools/scripts/install_linux_build_deps.sh`에 `libopencv-dev`를 추가했다.

## 검증

- `python -m unittest tools.scripts.test_release_helper tools.scripts.test_update_release_version`
  - 통과
  - 22개 테스트 통과
- `python -m unittest tools.scripts.test_release_helper tools.scripts.test_update_release_version tools.scripts.test_setup_cuda_runtime tools.scripts.test_setup_paddle_inference`
  - 통과
  - 57개 테스트 통과
- `python -m py_compile tools/scripts/release_helper.py tools/scripts/update_release_version.py tools/scripts/test_release_helper.py tools/scripts/test_update_release_version.py`
  - 통과
- `python -m py_compile tools/scripts/release_helper.py tools/scripts/test_release_helper.py tools/scripts/update_release_version.py tools/scripts/test_update_release_version.py tools/scripts/setup_cuda_runtime.py tools/scripts/setup_paddle_inference.py`
  - 통과
- `bash -n tools/scripts/install_linux_build_deps.sh`
  - 통과
- `cargo test --features gpu --lib 릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다 -- --nocapture`
  - 통과
  - 기본 gate off 경로 확인
- `BUZHIDAO_RUN_RELEASE_OCR_SMOKE=1`, `OCR_SERVER_DEVICE=cpu` 상태에서 `cargo test --features gpu --lib 릴리즈_ocr_smoke는_모델_보장후_1회_ocr를_성공한다 -- --nocapture`
  - 통과
  - 실제 OCR smoke 경로 확인
- `cargo test --features gpu --lib -- --nocapture`
  - 통과
  - 90개 테스트 통과
- `git diff --check`
  - 통과
- 로컬에는 Ruby/PyYAML/actionlint가 없어 workflow YAML 파싱 검증은 수행하지 못했다.
