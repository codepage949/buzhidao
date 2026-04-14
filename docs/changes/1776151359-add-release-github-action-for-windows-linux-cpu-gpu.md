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
- GitHub Release 파일당 2 GiB 제한을 피하기 위해 앱과 `ocr_server`를 별도 아카이브로 분리한다.

생성 자산은 아래 두 종류다.

- `buzhidao-<version>-<os>-<arch>-<flavor>-app`
- `buzhidao-<version>-<os>-<arch>-<flavor>-ocr-server`

앱 아카이브에는 `buzhidao(.exe)`만 포함하고,
OCR 서버 아카이브에는 `ocr_server/` onedir 산출물 전체만 포함한다.

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
- `deno eval "import { parse } from 'jsr:@std/yaml'; parse(await Deno.readTextFile('.github/workflows/release.yml')); console.log('ok');"`
- `cargo test --manifest-path app/Cargo.toml`

## 후속 수정

### Linux 앱 빌드 실패 수정

- `app/src/platform.rs`의 `show_overlay` Linux 분기에서 `_app`으로 선언한 인자를
  `app`으로 사용하고 있어 Linux 빌드가 실패했다.
- `available_monitors()` 결과는 `drain(..)` 대신 `into_iter().next()`로 첫 모니터를
  가져오도록 정리해 타입 추론 오류도 함께 제거한다.
- 모니터를 찾지 못한 경우에는 기존 캡처 영역 기준 배치 로직으로 폴백한다.
- Linux 링크 단계에서 `xcap` 경유로 `-lgbm`이 필요하므로
  GitHub Actions apt 패키지 목록에 `libgbm-dev`를 추가한다.
- `reqwest`/`openssl-sys` 링크를 위해 `libssl-dev`도 함께 설치한다.
- `pipewire` 계열 바인딩 생성 시 `bindgen`이 `libclang.so`를 요구하므로
  `libclang-dev`도 설치한다.
- `evdev-sys`가 소스 빌드 폴백으로 빠지지 않도록 `libevdev-dev`도 설치한다.

### Actions Node 런타임 경고 정리

- GitHub Actions 경고에 맞춰 `astral-sh/setup-uv`를 Node 24 대응 메이저로 올린다.
- 현재 공식 저장소 README 기준 최신 사용 예시는 `astral-sh/setup-uv@v8.0.0`이다.

### GPU 릴리즈 자산 크기 대응

- Windows/Linux GPU 빌드에서 단일 배포 아카이브가 GitHub Release 파일당 2 GiB 제한을 넘을 수 있다.
- 이를 피하기 위해 릴리즈 패키징을 `app`/`ocr-server` 2개 자산으로 분리한다.
- 워크플로우 artifact 업로드도 `matrix`당 하나가 아니라 `app`/`ocr-server`별 개별 artifact로 나눠
  다운로드 단계에서 다시 합쳐지지 않도록 갱신한다.
- 그래도 개별 아카이브가 2 GiB를 넘는 경우를 대비해 패키징 단계에서 자동 분할한다.
- 분할 파일명은 원본 뒤에 `.part001`, `.part002`를 붙이며, 릴리즈 업로드는 이 조각 파일도 함께 포함한다.

### 릴리즈 노트에 자산 합치기 안내 추가

- 현재 Release 자산은 2 GiB 제한 회피를 위해 `app`과 `ocr-server`로 분리되지만,
  사용자가 두 자산을 어떻게 조합해야 하는지 설명이 부족하다.
- Release 노트 상단에 플랫폼/아키텍처/flavor가 같은 `app`/`ocr-server` 자산을
  함께 받아 같은 디렉터리에 압축 해제해야 한다는 설치 안내를 추가한다.
- 실행에 필요한 최종 디렉터리 구조를 `buzhidao(.exe)`와 `ocr_server/` 기준으로 명시한다.
- `.part001` 같은 분할 파일이 생성된 경우를 대비해 Windows PowerShell, Linux `cat`
  기준의 병합 예시도 Release 노트에 포함한다.
