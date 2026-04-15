# 설정 창 재오픈·작업표시줄 노출·Vite 8 업그레이드

## 목적

- 설정 창을 한 번 닫은 뒤 다시 열리지 않는 문제를 수정한다.
- 설정 창이 Windows 작업표시줄에 나타나도록 한다.
- UI 빌드 툴체인을 최신 Vite 메이저로 올린다.

## 구현 계획

1. 설정 창 닫기 요청이 실제 `close`가 아니라 `hide`로 처리되도록 바꾼다.
2. 설정 창만 작업표시줄에 표시되도록 Tauri 창 설정을 조정한다.
3. Vite와 React 플러그인을 최신 호환 버전으로 올린다.
4. 프런트 빌드와 앱 테스트로 변경을 확인한다.

## 구현 내용

### 설정 창 닫기 동작을 hide로 전환

- 설정 페이지에서 창 닫기 버튼을 누르면 Tauri 기본 동작으로 창이 닫혀, 이후 같은 레이블의 창을 다시 열 수 없는 상태가 됐다.
- `settings.tsx`에서 `onCloseRequested`를 가로채 `preventDefault()` 후 `hide()`를 호출하도록 변경했다.
- 이로써 닫기 버튼, `Alt+F4`, 시스템 닫기 요청 모두 창 인스턴스를 유지한 채 숨김 처리되어 다시 열 수 있다.

### 설정 창 작업표시줄 노출

- `tauri.conf.json`의 settings 창 `skipTaskbar`를 `false`로 변경했다.
- 이제 설정 창은 독립 창처럼 작업표시줄에 나타난다.

### Vite 8 계열로 업그레이드

- npm 레지스트리 기준 최신 메이저를 확인한 뒤 `vite`를 `^8`, `@vitejs/plugin-react`를 `^6`으로 올렸다.
- 현재 lock 파일에는 `vite 8.0.3`, `@vitejs/plugin-react 6.0.1`으로 고정됐다.
- Deno import 버전 범위를 각각 `npm:vite@^8`, `npm:@vitejs/plugin-react@^6`로 갱신했다.
- 기존 `vite.config.ts`와 UI 코드는 추가 수정 없이 빌드 호환이 유지됐다.

## 테스트

- `deno task build`
- `cargo test --manifest-path app/Cargo.toml`

## 리팩토링 검토

- 닫기 동작 수정은 settings 프런트에만 국한돼 있어 추가 리팩토링은 필요하지 않았다.
