# Tauri 앱을 app 디렉토리로 이동

## 구현 목적

루트에 흩어진 Tauri 앱 관련 파일을 `app/` 디렉토리 아래로 모아 프로젝트 경계를 분명히 한다.

- Tauri 전용 파일 집합을 한 디렉토리로 묶어 구조를 명확하게 만든다.
- `ui/`, `capabilities/`, 환경 파일까지 Tauri 앱 경계 안으로 포함한다.
- 이동 후에도 기존 빌드와 테스트 흐름이 유지되도록 상대 경로를 정리한다.

## 구현 계획

1. `.env`, `.env.example`, `README.md`, `ui/`, `capabilities/`를 `app/`으로 이동한다.
2. `tauri.conf.json`의 프런트엔드 경로를 새 위치에 맞게 다시 수정한다.
3. `README.md`와 `.gitignore`를 새 구조에 맞게 갱신한다.
4. `cargo test`, `cargo check`, `deno task --config ui/deno.json test`로 구조 변경을 검증한다.

## 구현 사항

- 루트의 Tauri 앱 파일을 `app/` 디렉토리 아래로 이동했다.
- `app/tauri.conf.json`에서 `beforeDevCommand`, `beforeBuildCommand`, `frontendDist` 경로를 `app/` 기준으로 조정했다.
- `.env`, `.env.example`, `README.md`, `ui/`, `capabilities/`도 `app/` 아래로 이동했다.
- `app/README.md`의 프로젝트 구조와 실행 방법을 현재 디렉토리 구조에 맞게 갱신했다.
- 루트 `.gitignore`에 `app/.env.example` 추적 예외를 추가했다.

## 테스트 결과

- `cargo test`
  - `app/` 기준 실행, 테스트 4개 통과
- `cargo check`
  - `app/` 기준 컴파일 검증 통과
- `deno task --config ui/deno.json test`
  - `app/` 기준 실행, 프런트엔드 테스트 통과

## 추가 검토

- 현재 변경은 디렉토리 구조 이동과 상대 경로 보정이 핵심이라 추가 리팩토링 이득이 크지 않다.
- 후속 작업이 있다면 루트에서 Tauri 명령을 래핑하는 스크립트 추가 정도가 후보지만, 이번 범위에는 포함하지 않았다.
