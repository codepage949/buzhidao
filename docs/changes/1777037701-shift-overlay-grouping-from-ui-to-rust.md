# 배경

- 최근 최적화 후에도 fixture 기반 pipeline 벤치에서는 ffi가 앞서지만, 실제 프로그램 체감에서는 sidecar가 더 빠르게 느껴질 수 있다는 피드백이 있었다.
- 현재 앱 경로를 보면 `ocr_result`를 받은 뒤 오버레이 프런트가 다시 `groupDetectionsTraceWithBounds()`를 수행한다.
- 이 계산은 OCR이 끝난 뒤 UI 스레드에서 실행되므로, 실제 사용자는 결과가 준비된 뒤에도 추가 지연으로 체감할 수 있다.

# 이번 작업 목표

1. 오버레이에 필요한 그룹핑 결과를 Rust에서 미리 계산한다.
2. emit payload에서 프런트가 다시 계산할 raw OCR detection 직렬화를 제거한다.
3. 오버레이는 `ocr_result` 수신 후 바로 렌더링만 하도록 바꾼다.

# 구현 계획

1. `src/services/ocr_pipeline.rs`
   - detection grouping 로직을 Rust로 옮긴다.
   - `OcrResultPayload`에 오버레이 렌더용 그룹 payload를 추가한다.
   - raw `detections`는 런타임 로직에는 유지하되 직렬화에서는 제외한다.
2. `src/lib.rs`
   - 기존 `ocr_result` emit은 그대로 두되, 더 가벼워진 payload를 직접 보낸다.
3. `ui/src/pages/overlay/index.tsx`
   - `groupDetectionsTraceWithBounds()` 계산을 제거한다.
   - Rust가 보내준 그룹 목록을 그대로 렌더한다.

# 검증 계획

- `cargo test --lib -- --nocapture`
- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
- `python tools/scripts/ocr_sidecar_ffi.py compare ... --ffi-mode pipeline`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark ... --ffi-mode pipeline`

# 구현 후 기록

## 적용 내용

1. `src/services/ocr_pipeline.rs`
   - 오버레이 grouping 로직을 Rust로 옮겼다.
   - `OcrGroupPayload`와 `groups` 필드를 추가했다.
   - raw `detections`는 내부 로직과 검증용으로 유지하되, 앱 `ocr_result` 직렬화에서는 제외했다.
   - `offset_ocr_result`와 `scale_ocr_result`도 grouped bounds를 함께 보정하도록 맞췄다.
   - 관련 단위 테스트를 추가했다.
2. `ui/src/pages/overlay/index.tsx`
   - `groupDetectionsTraceWithBounds()`를 제거했다.
   - Rust가 보낸 `groups`를 그대로 렌더하도록 바꿨다.

## 검증 결과

- `cargo test --lib -- --nocapture`
  - `85 passed; 0 failed`
- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
  - 통과
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: `7/7 exact match`
  - `test2.png`: `14/14 exact match`
  - `test3.png`: `18/18 exact match`
  - `test4.png`: `37/37 exact match`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: sidecar `2688.25ms`, ffi `3299.83ms`
  - `test2.png`: sidecar `3800.63ms`, ffi `3239.73ms`
  - `test3.png`: sidecar `5402.66ms`, ffi `5651.05ms`
  - `test4.png`: sidecar `9576.76ms`, ffi `7716.74ms`

## 해석

- parity는 유지됐다.
- 이번 변경은 앱의 체감 지연을 줄이기 위해 프런트가 하던 grouping 계산을 Rust로 이동한 것이다.
- 따라서 위 벤치는 “OCR + grouped payload 생성” 비용을 더 많이 반영한다.
- 일부 fixture에서 수치가 느려진 것은 실제로 일을 덜 한 것이 아니라, 오버레이 UI가 하던 계산을 Rust 쪽으로 옮겼기 때문이다.
