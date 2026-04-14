# GitHub Release Workflow 추가 (Windows/Linux, CPU/GPU, amd64)

## 목적

GitHub Actions에서 수동 릴리즈를 실행해 다음 타깃의 아카이브를 자동 생성하고
GitHub Release에 게시한다.

- Windows amd64 CPU
- Windows amd64 GPU
- Linux amd64 CPU
- Linux amd64 GPU

macOS는 이번 범위에서 제외한다.

## 설계

### 트리거

- `workflow_dispatch`
- 입력: `version` (예: `v0.1.0`)

### 잡 구성

1. `prepare`
   - 태그 중복 여부 확인
   - 릴리즈 기준 SHA 캡처

2. `build`
   - OS/플레이버 매트릭스 빌드
   - `ocr_server`와 `app`을 각각 빌드
   - 아카이브 생성 후 artifact 업로드

3. `release`
   - build artifact 다운로드
   - 태그 생성
   - GitHub Release 생성

### 빌드 매트릭스

| os | arch | flavor | runner |
|----|------|--------|--------|
| windows | amd64 | cpu | `windows-latest` |
| windows | amd64 | gpu | `windows-latest` |
| linux | amd64 | cpu | `ubuntu-24.04` |
| linux | amd64 | gpu | `ubuntu-24.04` |

### OCR server 빌드 전략

- CPU: 기존 `uv sync --group build --group cpu` 사용
- GPU: 기존 `uv sync --group build --group gpu` 사용
- `tool.uv.environments`를 Windows/Linux amd64로 확장해 하나의 `uv.lock`으로 두 플랫폼을 함께 잠근다.

### Linux runner 의존성

- Tauri/Linux 빌드를 위해 `libwebkit2gtk-4.1-dev`, `libgtk-3-dev`, `libayatana-appindicator3-dev`, `librsvg2-dev`, `patchelf` 설치
- `pipewire` 크레이트 빌드를 위해 `libpipewire-0.3-dev` 설치
- OCR/Paddle 런타임 보조 패키지로 `libgl1`, `libgeos-dev`, `libgomp1`, `libglib2.0-0` 설치

### 아카이브 구성

- Windows: `.zip`
- Linux: 실행 권한 보존을 위해 `.tar.gz`

아카이브 루트에는 아래를 포함한다.

- `buzhidao(.exe)`
- `ocr_server/` onedir 산출물 전체

앱은 런타임에 `resource_dir/ocr_server/ocr_server(.exe)` 후보를 찾을 수 있으므로
위 구조로 배포 아카이브를 맞춘다.

### 헬퍼 스크립트

패키징 로직은 `scripts/release_helper.py`로 분리한다.

- 아카이브 파일명 생성
- 배포 디렉토리 레이아웃 준비
- `.zip` / `.tar.gz` 생성

### Actions 런타임

- Node 20 deprecation 경고를 피하기 위해 `actions/checkout`, `actions/setup-python`,
  `actions/upload-artifact`, `actions/download-artifact`, `denoland/setup-deno`를
  최신 major로 올린다.

## 검증 계획

- `python -m unittest scripts.test_release_helper`
- `python -c "import yaml; yaml.safe_load(open('.github/workflows/release.yml', 'r', encoding='utf-8'))"`
