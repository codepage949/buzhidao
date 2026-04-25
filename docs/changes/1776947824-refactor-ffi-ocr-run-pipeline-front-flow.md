# FFI OCR run_pipeline 앞단 흐름 리팩토링

## 목적

- `native/paddle_bridge/bridge.cc`의 `run_pipeline()` 앞부분에 남아 있는 군더더기를 줄인다.
- crop 수집, cls batch 실행, rec candidate 생성 단계를 helper로 분리한다.
- 구조만 정리하고 parity와 성능 의미는 유지한다.

## 범위

- 포함:
  - crop 결과 수집 helper
  - cls batch 실행 helper
  - rec candidate 생성 helper
- 제외:
  - det/cls/rec predictor 정책 변경
  - profiling 기준 변경
  - batch size나 threshold 변경

## 기대 효과

- `run_pipeline()`를
  - load/det
  - crop/cls
  - rec
  - finalize
  단계로 더 읽기 쉽게 만든다.
- 이후 det/crop/cls 쪽 정책 실험을 할 때 수정 범위를 좁히기 쉬워진다.

## 검증 계획

- `cargo test --lib paddle_ffi -- --nocapture`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`

## 구현

### 1. 앞단 crop/cls 흐름 helper 분리

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `ClsPreparedInput` 구조 추가
  - `collect_cls_inputs()`
  - `run_cls_batches_into()`
  - `build_rec_candidates()`
    를 추가해 `run_pipeline()` 앞단에서 직접 펼쳐 쓰던 crop/cls/rec-candidate 조립 흐름을 분리

의도:
- `run_pipeline()` 본문에서 det 직후 이어지는 긴 전처리 덩어리를 줄이고,
  “cls 입력 수집 -> cls 실행 -> rec 후보 생성” 순서를 helper 이름으로 드러내기

## 결과

- `run_pipeline()`는 이전보다 앞단 orchestration을 짧게 읽을 수 있게 됐다.
- crop과 cls batch 처리 로직은 동작을 유지한 채 helper로 분리됐다.
- rec 단계 helper와도 결이 맞아져 함수 흐름이 앞/뒤로 더 대칭적으로 정리됐다.

## 테스트 결과

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

## 결론

- 이번 라운드까지로 `run_pipeline()`의 앞단과 rec 흐름에서 눈에 띄는 군더더기는 대부분 정리됐다.
- 추가 분리는 가능하지만, 지금부터는 구조적 이득보다 함수 수 증가가 더 커질 가능성이 있다.
- 다음 리팩토링은 필요가 생길 때 profiling/override 묶음이나 predictor 설정 helper 쪽을 선택적으로 정리하면 된다.
