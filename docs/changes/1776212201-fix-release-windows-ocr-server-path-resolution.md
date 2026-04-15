# 릴리즈 Windows OCR server 경로 해석 수정

## 목적

- GitHub Release에서 분리 배포한 Windows `app`/`ocr_server` 자산을 같은 디렉터리에 풀었을 때
  앱 시작 직후 종료되는 문제를 수정한다.

## 구현 계획

1. 릴리즈 배포 레이아웃에서 OCR server 실행파일을 찾는 경로 해석 로직을 보강한다.
2. `buzhidao.exe` 옆의 `ocr_server/ocr_server.exe` sibling 구조를 테스트로 고정한다.
3. 기존 번들/개발 경로 해석과 충돌하지 않는지 `cargo test`로 확인한다.

## 설계

### 원인

- 현재 릴리즈 자산은 `buzhidao.exe`와 `ocr_server/ocr_server.exe`를 같은 상위 디렉터리에 두도록 안내한다.
- 하지만 앱 시작 시 OCR server 실행파일 경로 해석은 설정값 자체 경로 또는 `resource_dir`만 우선 확인한다.
- Release 자산을 수동으로 합친 구조에서는 `resource_dir` 기반 탐색만으로 sibling `ocr_server` 폴더를 찾지 못해
  `OcrBackend::new(...)` 단계에서 즉시 실패하고 앱이 종료될 수 있다.
- 또한 릴리즈 첫 실행 시 생성되는 `.env`는 `AI_GATEWAY_API_KEY=`, `AI_GATEWAY_MODEL=`처럼
  빈 값을 기본으로 두는데, 현재 설정 로드는 이 값을 앱 시작 단계에서 필수로 강제한다.
- 이 경우 OCR warmup 이전 `Config::from_env_file(...)`에서 즉시 실패해 로딩 표시가 보이기 전에
  앱이 종료될 수 있다.
- 더불어 기본 `.env.example`의 `SYSTEM_PROMPT=다음을 한국어로 번역하세요.`처럼
  공백이 포함된 문자열 값이 그대로 기록되면 `dotenvy` 파서가 라인을 해석하지 못해
  setup 단계에서 즉시 종료될 수 있다.

### 수정 방향

- 설정 경로가 없고 `resource_dir` 후보도 없으면 현재 실행 중인 앱 바이너리의 디렉터리를 기준으로
  sibling `ocr_server/<file_name>` 경로를 추가 탐색한다.
- 기존 번들 리소스 경로와 이미 존재하는 설정 경로는 우선순위를 유지한다.
- AI Gateway 설정은 앱 시작 시점에는 빈 문자열을 허용하고,
  실제 사용 진입점(PrtSc, 설정 저장)에서만 필수 검증을 수행한다.
- `.env.example`과 설정 저장 로직은 공백/개행이 있는 문자열 값을
  `dotenvy`가 읽을 수 있는 형태로 escape해서 기록한다.
- 이후 시스템 프롬프트는 `.env`에서 분리해 `.prompt` 파일로 관리한다.
- 개발 빌드는 저장소 루트의 `.prompt`, 배포 빌드는 앱 데이터 디렉터리의 `.prompt`를 읽는다.
- `.prompt`가 없으면 기본 프롬프트를 생성하고, 설정 저장 시에도 `.prompt`를 함께 갱신한다.

## 검증 계획

- `cargo test --manifest-path app/Cargo.toml`
