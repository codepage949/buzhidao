# FFI 단일 OCR 모드 전환과 sidecar 비교 스크립트 유지

## 배경

- 앱 런타임은 이미 Paddle FFI 경로가 주 실행 경로인데도,
  `python_sidecar` 백엔드와 sidecar 강제 우회 경로가 남아 있어 초기화/설정/문서가 이중 구조였다.
- 이 상태에서는 앱이 실제로 어떤 OCR 실행 경로를 쓰는지 불명확하고,
  멀티플랫폼 대응 시에도 앱 런타임과 비교 스크립트의 책임 경계가 흐려진다.
- 다만 `scripts/compare_ocr_sidecar_ffi.py`는 FFI 결과를 sidecar 기준과 비교해야 하므로,
  Python sidecar 자체는 CLI 스크립트로 계속 실행 가능해야 한다.

## 결정

- 앱 런타임 OCR 백엔드는 Paddle FFI 단일 모드로 고정한다.
- Rust 앱 코드에서 `python_sidecar` 백엔드와 관련 설정값(`OCR_BACKEND`,
  `OCR_SERVER_EXECUTABLE`, sidecar timeout 등)을 제거한다.
- FFI가 빌드되지 않았거나 Paddle Inference가 링크되지 않은 빌드에서는
  앱 시작 시 명시적으로 오류를 반환해 잘못된 배포 구성을 바로 드러낸다.
- Python sidecar는 `ocr_sidecar_compare/ocr_sidecar_compare.py` CLI로 유지하고,
  `scripts/compare_ocr_sidecar_ffi.py`가 멀티플랫폼 Python 해석기로 직접 실행한다.

## 구현 계획

1. 앱 OCR 선택 로직을 FFI 단일 모드로 단순화한다.
2. sidecar 관련 설정/경로 해석/테스트/문서를 앱 런타임 기준에서 제거한다.
3. 비교 스크립트는 기존처럼 sidecar CLI를 직접 실행하되, Rust 런타임 우회에 의존하지 않게 정리한다.
4. Rust 테스트와 비교 스크립트 도움말 실행으로 회귀를 확인한다.

## 변경 사항

### sidecar 프로젝트 명칭과 배포 범위

- 비교/진단 전용이라는 역할이 드러나도록 프로젝트 디렉터리를 `ocr_server`에서
  `ocr_sidecar_compare`로 옮겼다.
- 비교 sidecar 엔트리 스크립트도 `ocr_sidecar_compare.py`로 맞췄다.
- GitHub Release에서는 sidecar를 더 이상 별도 자산으로 빌드/배포하지 않도록 정리했다.

### 앱 런타임 OCR

- `app/src/ocr/mod.rs`
  - `python_sidecar` 백엔드 분기를 제거하고 Paddle FFI 단일 모드로 정리했다.
  - FFI가 빌드되지 않은 구성에서는 `Unsupported` 상태로 초기화한 뒤,
    warmup/run 시 명시적 오류를 반환하게 했다.
  - `OCR_BACKEND` 환경변수 기반 선택 로직과 관련 테스트를 제거했다.

- `app/src/ocr/paddle_ffi.rs`
  - `BUZHIDAO_PADDLE_FFI_FORCE_SIDECAR` 강제 우회 경로를 제거했다.
  - Rust 런타임에서 Python sidecar를 직접 실행하던 fallback 코드와 Python 경로 해석을 제거했다.

- `app/src/ocr/python_sidecar.rs`
  - 앱 런타임 백엔드 구현 파일을 삭제했다.

### 설정과 초기화

- `app/src/config.rs`
  - `OCR_SERVER_EXECUTABLE`, `OCR_SERVER_STARTUP_TIMEOUT_SECS`,
    `OCR_SERVER_REQUEST_TIMEOUT_SECS` 설정 필드를 제거했다.
  - 기본 sidecar 실행 파일 경로 계산 로직을 제거했다.

- `app/src/lib.rs`
  - sidecar 실행 파일 fallback 해석 함수와 관련 테스트를 제거했다.
  - 앱 setup 단계에서 sidecar 경로를 보정하던 초기화 코드를 제거했다.

- `app/src/settings.rs`
  - 제거된 sidecar 인프라 필드에 의존하던 테스트 fixture와 assertion을 정리했다.

### 문서와 비교 스크립트 경계

- `app/.env.example`, `app/README.md`, `README.md`
  - 앱 OCR이 Paddle FFI 단일 모드임을 명시했다.
  - sidecar 실행 파일/타임아웃/백엔드 선택 설정 설명을 제거했다.
  - 비교용 sidecar는 `ocr_sidecar_compare/ocr_sidecar_compare.py`를 스크립트로 직접 실행한다고 기록했다.

- `scripts/compare_ocr_sidecar_ffi.py`
  - 새 디렉터리/엔트리 이름에 맞춰 경로 상수를 갱신했다.
  - 현재 구조에서 비교 스크립트는 계속
    `ocr_sidecar_compare/ocr_sidecar_compare.py`를 멀티플랫폼 Python 해석기로 직접 실행한다.

- `.github/workflows/release.yml`, `scripts/release_helper.py`
  - sidecar 빌드/아카이브/업로드/설치 스크립트 생성을 릴리즈 파이프라인에서 제거했다.

## 테스트

- `cargo test -p buzhidao`
  - 통과. 62개 테스트가 모두 성공했다.
- `python scripts/compare_ocr_sidecar_ffi.py --help`
  - 통과. 비교 스크립트 CLI가 정상 실행되고 옵션 목록을 출력했다.
- `python -m unittest scripts.test_release_helper`
  - 통과. 릴리즈 헬퍼가 앱 단일 자산 기준으로 동작함을 확인했다.
- `python -m unittest discover ocr_sidecar_compare/tests`
  - 통과. sidecar 비교 프로젝트의 순수 Python 테스트가 정상 동작했다.
