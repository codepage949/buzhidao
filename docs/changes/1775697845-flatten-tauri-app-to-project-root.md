# Tauri 앱을 app 디렉토리에서 프로젝트 루트로 평탄화

## 배경

이전에는 Tauri 앱이 `app/` 하위 프로젝트로 분리되어 있었고,
루트는 여러 워크스페이스 성격의 파일을 감싸는 상위 디렉토리 역할을 했다.

지금은 OCR 서버가 제거되어 별도 서브프로젝트 경계의 필요성이 크게 줄었고,
`cargo tauri dev`, `cargo test`, `deno task test` 같은 주요 개발 명령도
루트에서 바로 실행되는 구조가 더 단순하다.

## 구현 계획

1. `app/`의 Tauri 프로젝트 파일을 루트로 이동한다.
2. 산출물 디렉터리 `app/target`은 이동하지 않고 루트에서 재생성되도록 둔다.
3. 스크립트, README, 문서, 테스트 경로를 루트 기준으로 갱신한다.
4. Rust 및 UI 테스트로 구조 변경이 실제로 동작하는지 검증한다.

## 구현

- 루트로 이동
  - `Cargo.toml`, `Cargo.lock`, `build.rs`, `tauri.conf.json`
  - `src/`, `models/`, `icons/`, `capabilities/`, `gen/`
  - `.env`, `.env.example`
- 루트 기준 경로 갱신
  - `scripts/export_onnx.py`의 모델 산출물 경로를 `models/`로 변경
  - `scripts/compare_onnx.py`의 모델 탐색 경로를 `models/`로 변경
  - 루트 `README.md`를 기존 루트 README의 기능/기술 스택/시작하기 구조를 최대한 유지하면서 현재 구조 기준으로 재작성
  - `ui/README.md`의 `app/ui` 경로 설명을 `ui/` 기준으로 수정
  - `.gitignore`의 모델 산출물 경로를 `models/` 기준으로 수정
- 예외 처리
  - `app/target/`은 이동하지 않고 루트 `target/`에서 재생성되도록 둠
  - 실행 중인 `cargo`/`deno` 프로세스로 잠긴 `app/ui`와 `app/target`은 즉시 삭제하지 못해, 루트 구조로 복사 전환 후 `app/` 전체를 ignore 처리
  - Git 인덱스에서는 기존 `app/ui`, `app/README.md`, `app/.gitignore`를 제거해 저장소 구조를 루트 기준으로 정리
  - `ui/node_modules`는 복사하지 않고 `deno install`로 재생성해야 함

## 테스트 계획

- `cargo test`
- `cd ui && deno task test`
- `python scripts/export_onnx.py --print-only`

## 테스트 결과

- `cargo test`
  - 41개 테스트 전부 통과
- `cd ui && deno task test`
  - 11개 테스트 전부 통과
- `uv run --no-project --python 3.11 scripts/export_onnx.py --print-only`
  - Docker 명령이 루트 `models/` 마운트 기준으로 정상 출력됨
- `cargo tauri dev`
  - 초기에는 복사된 `ui/node_modules` 때문에 `rollup` 누락 오류가 발생
  - `ui/node_modules` 삭제 후 `deno install` 재실행으로 해결
  - 이후 Vite는 포트 충돌(`1420 already in use`)까지 진행되어 의존성 해소 확인

## 리팩토링 검토

- 주요 경로 의존은 모두 루트 기준으로 정리되어 추가 리팩토링 필요성은 크지 않다.
- 다만 실행 중인 프로세스 때문에 남아 있는 로컬 `app/` 잔재는 프로세스 종료 후 삭제하면 더 깔끔하다.
