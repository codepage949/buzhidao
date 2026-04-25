# FFI OCR run_pipeline rec 흐름 리팩토링

## 목적

- `native/paddle_bridge/bridge.cc`의 `run_pipeline()`가 너무 많은 책임을 직접 들고 있는 상태를 줄인다.
- 이번 라운드에서 추가된 rec batch planning, rec order logging, 최종 detection/debug 적재 흐름을 함수로 분리한다.
- parity와 성능은 유지하고 구조만 정리한다.

## 범위

- 포함:
  - `RecCandidate` 구조를 함수 밖으로 올려 재사용 가능하게 정리
  - rec order logging 분리
  - rec batch planning 분리
  - rec batch 실행/결과 병합 분리
  - 최종 detection/debug 결과 적재 분리
- 제외:
  - det/cls/rec predictor 의미 변경
  - score threshold 정책 변경
  - profiling 수치 변경을 노리는 최적화

## 기대 효과

- `run_pipeline()` 본문 길이를 줄이고 읽기 단계를
  - load/det
  - crop/cls
  - rec plan/run
  - finalize
  로 더 명확히 나눌 수 있다.
- 이후 det/rec 정책 실험이나 추가 최적화 시
  rec batching과 후처리 쪽을 독립적으로 수정하기 쉬워진다.

## 검증 계획

- `cargo test --lib paddle_ffi -- --nocapture`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`

## 구현

### 1. rec 흐름 전용 helper로 분리

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `RecCandidate`를 함수 밖 구조로 승격
  - `build_rec_order()`
  - `dump_rec_candidates_if_requested()`
  - `log_rec_order()`
  - `plan_rec_batches()`
  - `log_rec_batches()`
  - `run_rec_batches_into()`
  - `append_pipeline_results()`
  - `dump_candidate_crop_if_requested()`
    를 추가해 `run_pipeline()`의 rec 관련 책임을 분리

의도:
- `run_pipeline()` 본문에서
  - rec 후보 정렬
  - rec batch 계획
  - rec batch 실행
  - 최종 detection/debug 적재
  단계를 직접 펼쳐 쓰지 않도록 정리

## 결과

- `run_pipeline()`는 여전히 전체 파이프라인 orchestration을 담당하지만,
  rec 흐름은 helper 호출 단위로 읽히도록 정리됐다.
- 이번 변경은 구조 정리만 수행했고,
  det/cls/rec 의미나 threshold 정책은 바꾸지 않았다.

## 테스트 결과

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

## 결론

- 이번 리팩토링은 필요한 수준으로 끝났다.
- 추가로 더 쪼갤 수는 있지만, 다음 단계는 구조 개선보다 실제 정책 변경이나 성능 실험에서 필요가 생길 때 진행하는 편이 맞다.
