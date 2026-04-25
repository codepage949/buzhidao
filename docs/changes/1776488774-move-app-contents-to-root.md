# app 내용 루트 이동

## 배경

현재 Tauri 앱 본체가 `app/` 하위에 들어 있어 실행, 문서, 워크플로우가 모두 `app/` 기준 경로를 전제한다.
이번 작업에서는 앱 소스와 설정을 프로젝트 루트로 올려 루트 기준으로 개발/빌드/테스트할 수 있게 정리한다.

## 구현 계획

1. `app/` 하위에서 루트로 올릴 소스/설정/문서 디렉터리를 확정한다.
2. 루트 기준으로 충돌 가능한 파일을 확인하고, 기존 루트 파일은 앱 루트 파일 기준으로 정리한다.
3. GitHub Actions, 스크립트, README, 예제 환경 파일 등 `app/...` 경로 참조를 루트 기준으로 수정한다.
4. Rust 테스트와 UI 테스트를 루트 기준으로 실행해 구조 변경이 깨지지 않았는지 확인한다.

## 예상 변경 범위

- `app/`의 Tauri 프로젝트 파일을 루트로 이동
- 루트 `README.md`를 앱 루트 기준 구조와 실행 명령으로 갱신
- `.github/workflows/release.yml`, `scripts/setup_paddle_inference.py` 등 경로 고정 참조 수정
- 빌드 산출물 성격의 `app/target`은 이동하지 않고 루트에서 재생성되도록 유지

## 구현 내용

- `app/`의 Tauri 프로젝트 소스와 설정을 루트로 이동했다.
  - `Cargo.toml`, `Cargo.lock`, `build.rs`, `tauri.conf.json`
  - `src/`, `ui/`, `capabilities/`, `icons/`, `native/`, `testdata/`
  - `.env.example`, 로컬 개발용 `.env`, `.prompt`, `.prompt.txt`, `.paddle_inference`, `gen/`
- 루트 `README.md`를 루트 기준 구조와 명령으로 갱신했다.
- `ui/README.md`, `.env.example`, `native/paddle_bridge/bridge.cc`,
  `scripts/setup_paddle_inference.py`, `.github/workflows/release.yml`의
  `app/...` 경로 전제를 루트 기준으로 수정했다.
- `.env.example`의 Paddle FFI 안내 주석은 루트 이동 뒤 중복되는 설명을 줄이도록 정리했다.
- 루트 이동으로 깨진 상대 경로를 보정했다.
  - `src/settings.rs`: `shared/langs.json` include 경로 수정
  - `src/ocr/paddle_ffi.rs`: `shared/langs.json` include 경로 수정
- 루트에서 새로 생성되는 `target/`과 기존 잔여 빌드 산출물 `app/target/`을 모두 `.gitignore`로 유지했다.

## 테스트 계획

- `cargo test`
- `deno task test` (`ui/`)

## 테스트 결과

- 통과: `cargo test`
- 통과: `deno task test` (`ui/`)
