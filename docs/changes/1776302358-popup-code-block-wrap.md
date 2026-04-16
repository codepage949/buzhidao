# 팝업 코드 블록 줄바꿈 강제

## 배경

번역 팝업에서 마크다운 코드 블록(`pre`)이 긴 줄이면 가로 스크롤이
생겨 팝업 가독성이 떨어진다. 팝업은 너비가 좁고 번역 결과 위주라
가로 스크롤보다는 자동 줄바꿈이 더 적합하다.

## 변경

- `app/ui/src/popup.tsx`의 `.markdown-body pre` 스타일
  - `overflow-x: auto` → `overflow-x: hidden`
  - `white-space: pre-wrap` 적용
  - `word-break: break-word` / `overflow-wrap: anywhere` 로 초장문
    토큰도 줄바꿈 강제
- `.markdown-body pre code`에도 동일하게 적용하여 react-markdown이
  감싸는 `<code>` 요소도 줄바꿈 되도록.

## 테스트

육안 검증(긴 코드 포함된 번역 결과 표시).
