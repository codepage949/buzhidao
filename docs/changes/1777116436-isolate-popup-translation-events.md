# 팝업 번역 이벤트 독립 처리

## 배경

- OCR 인식 영역을 짧은 시간에 여러 번 누르면 여러 `select_text` 요청이 동시에 진행될 수 있다.
- 현재 팝업 이벤트는 `translating`, `translation_result`, `translation_error`만 구분하고 작업 식별자가 없다.
- 따라서 먼저 누른 영역의 번역 응답이 늦게 도착하면, 나중에 누른 영역의 팝업 상태를 덮을 수 있다.

## 구현 계획

1. 번역 요청마다 증가하는 request id를 발급한다.
2. `translating`, `translation_result`, `translation_error` 이벤트 payload에 request id를 포함한다.
3. 팝업은 현재 활성 request id와 일치하는 이벤트만 반영하고, 오래된 이벤트는 무시한다.
4. stale 이벤트 필터링 로직을 테스트로 고정한다.
5. Rust check와 프런트 테스트/빌드로 회귀를 확인한다.

## 구현 내용

- `src/lib.rs`에 `TranslationRequestSeq`를 추가해 `select_text` 호출마다 증가하는 request id를 발급한다.
- `translating`, `translation_result`, `translation_error` 이벤트 payload에 request id를 포함한다.
- `ui/src/pages/popup/index.tsx`에서 현재 활성 request id를 `useRef`로 보관한다.
- 팝업은 현재 활성 request id와 일치하는 결과/오류 이벤트만 반영하고, 오래된 이벤트는 무시한다.
- `ui/src/pages/popup/request.ts`에 이벤트 반영 여부 판단 로직을 분리했다.
- `ui/src/pages/popup/request_test.ts`에 활성 요청만 반영하는 테스트를 추가했다.
- `ui/deno.json`의 `test` task에 팝업 request 테스트를 포함했다.
- 전체 프런트 테스트 task가 현재 `RawDetection` 타입과 맞지 않는 기존 fixture에서 멈춰, `ui/src/lib/detection_test.ts`의 `det()` helper를 bbox 객체 형식으로 보정했다.

## 검증

- `cargo check --lib`
  - 통과
- `deno task test`
  - 통과
  - 35개 테스트 통과
- `deno task build`
  - 통과
