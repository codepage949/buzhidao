## 배경

- `detectTextsFromClipboard`가 PNG를 디스크(`output.png`)에 쓴 뒤 다시 읽어 FormData로 전송하는 불필요한 I/O가 존재한다.
- `makeMessage`가 호출마다 시스템 프롬프트 파일을 읽어 중복 디스크 접근이 발생한다.
- `TelegramClient.post()`가 모든 응답에 대해 `response.clone().json()`을 호출하여 불필요하게 메모리를 소비하고 응답 본문을 파싱한다.
- `callbackDataMap`에서 콜백 버튼 처리 후 항목을 삭제하지 않아 장시간 실행 시 Map이 계속 증가한다.

## 목표

- `detectTextsFromClipboard`에서 `output.png` 쓰기/읽기를 제거하고 메모리 버퍼를 직접 전송한다.
- `makeMessage`에서 시스템 프롬프트를 모듈 초기화 시 한 번만 읽도록 캐싱한다.
- `TelegramClient.post()` 로그에서 `response.clone().json()` 호출을 제거한다.
- 콜백 버튼 처리 후 `callbackDataMap`에서 해당 항목을 삭제한다.

## 비목표

- 키보드 훅 동작 방식 변경
- 텔레그램 기능 추가/변경
- 탐지 알고리즘 변경

## 작업 계획

1. `main.ts`: `detectTextsFromClipboard` — `Deno.open` / `Deno.readFile` 제거, `pngImg` 버퍼 직접 사용
2. `main.ts`: `makeMessage` — `systemPrompt` 를 모듈 레벨 상수로 이동 (top-level await)
3. `src/telegram.ts`: `post()` 로그에서 `response.clone().json()` 제거
4. `main.ts`: `handleTelegramUpdate` — 콜백 처리 후 `callbackDataMap.delete()` 추가
