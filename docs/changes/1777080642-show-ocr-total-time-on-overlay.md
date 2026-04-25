# 배경

- 실제 프로그램에서 sidecar와 FFI의 체감 속도를 비교하기 위해, 오버레이 UI에서 OCR 총 소요 시간을 바로 볼 필요가 있었다.
- Rust stage 로그만으로는 사용자가 실제로 보는 오버레이 표시 시점과 완전히 일치하지 않으므로, 오버레이가 직접 경과 시간을 표시하도록 한다.

# 이번 작업 목표

1. 오버레이 상단에 OCR 및 결과 표시까지 걸린 총 시간을 보여준다.
2. hotkey OCR과 영역 선택 OCR 모두 같은 방식으로 시간을 표시한다.
3. 시간 계산 로직은 테스트 가능한 순수 함수로 분리한다.

# 구현 계획

1. `ui/src/pages/overlay/index.tsx`
   - 오버레이 로딩 시작 시각을 저장한다.
   - `ocr_result` 수신 시점에 경과 시간을 계산해 ready 상태에 포함한다.
   - 오버레이 상단 좌측에 `OCR + 결과 Nms` 배지를 렌더링한다.
2. `ui/src/pages/overlay/timing.ts`
   - 경과 시간을 계산하는 순수 helper를 추가한다.
3. `ui/src/pages/overlay/timing_test.ts`
   - helper에 대한 단위 테스트를 추가한다.

# 구현 후 기록

## 적용 내용

1. `ui/src/pages/overlay/index.tsx`
   - `overlay_show` 또는 영역 OCR 제출 시 `performance.now()` 기준 시작 시각을 저장한다.
   - `ocr_result` 수신 시 `measureOverlayOcrElapsedMs()`로 경과 시간을 계산한다.
   - ready 상태에서 오버레이 상단 좌측에 `OCR + 결과 {ms}ms` 배지를 표시한다.
   - `overlay_select_region`, `ocr_error` 등 흐름에서는 필요 없는 시작 시각을 정리한다.
2. `ui/src/pages/overlay/timing.ts`
   - 시작 시각이 없거나 음수 경과 시간이 나오면 `null`을 반환하는 helper를 추가했다.
3. `ui/src/pages/overlay/timing_test.ts`
   - `null` 처리, 반올림 처리, 음수 경과시간 배제 케이스를 검증하는 테스트를 추가했다.

## 검증 결과

- `deno test src/pages/overlay/timing_test.ts src/pages/overlay/close_test.ts`
  - `7 passed; 0 failed`
- `deno task build`
  - production build 성공

## 비고

- 이번 표시 시간은 오버레이 기준의 체감 시간이다.
- hotkey OCR에서는 `overlay_show` 이후 `ocr_result`까지의 시간이고,
  영역 OCR에서는 선택 제출 직후부터 `ocr_result`까지의 시간이다.
- Rust stage log의 `capture_ms`를 포함한 전체 end-to-end와는 정의가 다르지만,
  사용자가 실제로 오버레이에서 기다리는 시간을 직접 비교하는 데는 더 적합하다.
