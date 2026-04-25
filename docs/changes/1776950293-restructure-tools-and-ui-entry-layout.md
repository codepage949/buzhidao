# 구조 개선 구현 계획

## 목표

- 루트 디렉토리에서 도구성 자산을 분리해 앱 본체와 보조 프로젝트의 경계를 더 분명하게 만든다.
- 비교·진단 전용 Python sidecar 프로젝트를 `tools/` 아래로 이동해 제품 코드와 역할을 분리한다.
- `ui/src`를 화면 단위 디렉토리로 재구성해 관련 HTML, 진입 코드, 보조 로직, 테스트를 함께 배치한다.

## 범위

- `scripts/` → `tools/scripts/`
- `ocr_sidecar_compare/` → `tools/ocr_sidecar_compare/`
- `ui/src` 내 평평한 엔트리 파일 구조 → `pages/`, `lib/` 기반 구조
- 위 변경에 따라 README, 빌드 설정, 테스트 경로, 워크플로, 스크립트 내부 경로를 함께 수정

## 제외 범위

- `.claude`, `.codex`, `.gemini`, `AGENTS.md`, `CLAUDE.md`, `GEMINI.md`는 현재 도구/협업 환경과 직접 연결될 수 있어 이번 구조 변경에서는 이동하지 않는다.
- `docs/index.html`, `docs/changes/`의 추가 재구성은 이번 작업 범위에서 제외한다.

## 구현 순서

1. 새 디렉토리 레이아웃을 만들고 파일을 이동한다.
2. Rust/Tauri/Vite/Deno/Python/GitHub Actions 참조 경로를 모두 갱신한다.
3. 문서 경로 설명을 실제 구조에 맞게 정리한다.
4. Rust 테스트, UI 테스트, Python 테스트/헬프 실행으로 경로 회귀를 확인한다.

## 구현 결과

- `scripts/`를 `tools/scripts/`로 이동하고 관련 README, Rust 빌드 메시지, GitHub Actions, Python 테스트 import를 새 경로에 맞게 갱신했다.
- `ocr_sidecar_compare/`를 `tools/ocr_sidecar_compare/`로 이동하고 비교 스크립트, sidecar 프로젝트, 테스트, 문서가 루트 `shared/` 자산을 계속 참조하도록 경로를 수정했다.
- `ui/src`를 `lib/`와 `pages/overlay|popup|loading|settings` 구조로 재배치하고, 각 HTML/엔트리/보조 로직/테스트를 화면 단위로 묶었다.
- UI 테스트 이름을 한글로 정리하고, 테스트 경로를 새 디렉토리 구조에 맞게 갱신했다.

## 검증 결과

- `cargo test` 통과
- `deno task test` 통과
- `python -m unittest tools.scripts.test_release_helper tools.scripts.test_setup_paddle_inference` 통과
- `python -m unittest discover tools/ocr_sidecar_compare/tests` 통과

## 비고

- 구조 변경 직후 `ui` 빌드는 stale `node_modules` 상태에서 실패할 수 있었다.
- `ui/node_modules`를 삭제하고 다시 구성하면 빌드가 정상 동작했다.
- 따라서 이번 변경에서 추가 패키지 의존성은 필요하지 않았고, 문제 원인은 구조 변경이 아니라 프런트엔드 캐시 상태였다.
