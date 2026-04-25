# 구조 변경 후 UI 엔트리 페이지 누락 수정

## 목표

- 구조 변경 이후 앱 실행 시 `overlay.html`, `loading.html`, `popup.html`, `settings.html`를 찾지 못하는 문제를 해결한다.
- Tauri 창 URL과 Vite 엔트리 경로를 다시 일치시켜 dev/build 모두 같은 경로로 열리게 한다.

## 범위

- `ui/src/*.html`
- `ui/vite.config.ts`

## 구현 계획

1. Tauri가 여는 루트 HTML 엔트리 파일이 실제로 존재하는지 확인한다.
2. 구조 변경 후 사라진 루트 HTML 엔트리를 compatibility entry로 복구한다.
3. Vite 입력 경로를 compatibility entry 기준으로 맞춘다.
4. 프런트엔드 빌드와 테스트로 경로 회귀를 확인한다.

## 구현 결과

- `ui/src/overlay.html`, `loading.html`, `popup.html`, `settings.html`를 루트 compatibility entry로 다시 추가했다.
- 각 compatibility entry는 새 구조의 `pages/.../index.ts(x)`를 직접 불러오도록 연결했다.
- `ui/vite.config.ts`의 Rollup 입력 경로도 루트 compatibility entry 기준으로 되돌려, dev/build 모두 `overlay.html`, `loading.html`, `popup.html`, `settings.html`를 안정적으로 제공하게 했다.
- Tauri 설정은 그대로 유지하면서 구조 변경 전과 같은 창 URL 계약을 복구했다.

## 검증 결과

- `deno task test` 통과
- `deno task build` 통과
