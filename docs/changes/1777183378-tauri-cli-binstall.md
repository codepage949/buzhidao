# Tauri CLI binstall 적용

## 배경

GitHub Actions release workflow에서 Tauri CLI를 `cargo install`로 설치하면 매 실행마다 소스 빌드가 발생할 수 있어 검증 및 릴리스 빌드 시간이 길어진다.

## 변경 사항

- release workflow의 `verify`와 `build` job에서 `cargo-bins/cargo-binstall` action을 먼저 설치하도록 변경했다.
- Tauri CLI 설치 명령을 `cargo install tauri-cli --version '^2' --locked`에서 `cargo binstall tauri-cli --version '^2' --no-confirm`으로 교체했다.
- `cargo-binstall` action 참조와 설치 버전을 `1.18.1`로 고정해 workflow 재현성을 유지했다.
- workflow 회귀 테스트에 Tauri CLI가 `cargo binstall`로 설치되고 기존 `cargo install tauri-cli` 경로가 남지 않는지 검증하는 항목을 추가했다.

## 검증

- `python -m unittest tools.scripts.test_release_workflow`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

