# 미사용 페이지 HTML 중복 파일 제거

## 목표

- `ui/src`에 남아 있는 미사용 HTML 중복 파일을 제거한다.
- UI 문서를 실제 엔트리 구조와 맞춘다.

## 범위

- `ui/src/pages/*/index.html`
- `ui/README.md`

## 구현 계획

1. `ui/src/pages/*/index.html`가 실제로 참조되지 않는지 확인한다.
2. 루트 compatibility entry가 이미 존재하는 상태에서 중복 HTML 4개를 제거한다.
3. `ui/README.md`를 실제 실행 엔트리 구조에 맞게 수정한다.
4. 프런트 테스트와 빌드로 회귀를 확인한다.

## 구현 결과

- `ui/src/pages/loading/index.html`, `overlay/index.html`, `popup/index.html`, `settings/index.html`를 제거했다.
- 실행 엔트리는 `ui/src/loading.html`, `overlay.html`, `popup.html`, `settings.html`만 유지하도록 정리했다.
- `ui/README.md`를 실제 구조에 맞게 수정해, 루트 HTML이 Tauri가 여는 엔트리이고 `pages/`는 로직/테스트 디렉토리라는 점을 명시했다.

## 검증 결과

- `deno task test` 통과
- `deno task build` 통과
