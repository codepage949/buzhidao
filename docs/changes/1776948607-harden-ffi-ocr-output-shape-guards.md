# FFI OCR 출력 shape 방어 보강

## 구현 계획

1. `native/paddle_bridge/bridge.cc`에서 predictor 출력 shape를 믿고 바로 인덱싱하는 지점을 찾는다.
2. 잘못된 shape/출력 길이에서 out-of-bounds가 날 수 있는 곳에 길이 검증을 추가한다.
3. batch helper에서 반환 개수 불일치도 명시적으로 검사한다.
4. 기존 parity를 유지하는지 테스트한다.

## 보강 대상

- `run_det()`
  - det 출력 맵 길이 검증
- `run_cls_batch()`
  - cls 출력 길이 검증
- `run_rec()`
  - rec 단일 출력 길이 검증
- `run_rec_batch()`
  - rec batch 출력 길이 검증
- `run_cls_batches_into()`
  - batch 결과 개수 불일치 검증
- `run_rec_batches_into()`
  - batch 결과 개수 불일치 검증

## 의도

- 잘못된 모델 파일이나 예기치 않은 predictor 출력 shape가 들어와도
  조용히 잘못된 메모리를 읽지 않도록 막는다.
- 정상 경로의 parity와 성능 의미는 바꾸지 않는다.

## 구현

### 1. det 출력 길이 검증 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `run_det()`에서 `pred_h * pred_w` 기준 최소 출력 길이 검증 추가
  - multi-channel det 출력 병합 전에 `out.size()`를 확인
  - 단일 맵 후처리 경로에서도 같은 검증 추가

보완 이유:
- 기존 코드는 `out_shape`만 믿고 `out[i]`를 바로 읽고 있었다.
- predictor가 잘못된 길이의 버퍼를 돌려주면 out-of-bounds가 날 수 있었다.

### 2. cls/rec 출력 길이 검증 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `run_cls_batch()`에서 `batch_n * num_classes`보다 출력이 짧은 경우 에러 반환
  - `run_rec()`에서 `time_steps * num_classes`보다 출력이 짧은 경우 에러 반환
  - `run_rec_batch()`에서 `batch_n * time_steps * num_classes`보다 출력이 짧은 경우 에러 반환

보완 이유:
- 기존 코드는 shape 파싱만 통과하면 결과 버퍼 길이가 충분하다고 가정했다.
- 잘못된 shape/길이 조합에서 batch decode나 class 선택 중 범위를 벗어날 수 있었다.

### 3. batch helper 결과 개수 검증 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `run_cls_batches_into()`에서 `batch_results.size() != 입력 수` 검사 추가
  - `run_rec_batches_into()`에서 `batch_results.size() != 입력 수` 검사 추가

보완 이유:
- helper 내부는 하위 함수가 항상 입력 개수만큼 결과를 돌려준다고 가정하고 있었다.
- 이 가정이 깨지면 `batch_results[i - start]` 접근에서 바로 범위를 벗어날 수 있었다.

## 리스트업한 오류 가능성

- `run_det()`:
  - shape만 맞고 실제 버퍼 길이가 짧을 때 `out[i]` 접근 위험
- `run_cls_batch()`:
  - `num_classes` 계산은 됐지만 실제 출력 길이가 짧을 때 class score 접근 위험
- `run_rec()` / `run_rec_batch()`:
  - `time_steps`, `num_classes`는 파싱됐지만 logits 길이가 부족할 때 decode 접근 위험
- `run_cls_batches_into()` / `run_rec_batches_into()`:
  - 하위 함수 결과 개수가 입력 개수와 다를 때 병합 인덱싱 위험

## 테스트

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

## 결론

- 이번 보강은 정상 경로를 바꾸지 않고, 비정상 predictor 출력에서 메모리 접근 오류로 번질 수 있는 지점을 막는 데 집중했다.
- 다음 단계에서 더 보려면 crop 실패 누적이나 det/rec dump 경로의 오류 전파 정책처럼,
  현재는 조용히 넘어가는 실패를 에러로 올릴지 여부를 따로 판단하면 된다.

## 추가 보강

### 4. crop 실패 누적 감지 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `collect_cls_inputs()`에서 crop 실패 개수를 집계
  - det box가 있었는데 유효한 crop이 하나도 없으면 즉시 에러 반환
  - 일부 crop만 실패한 경우에도 skipped count를 debug log로 남김

보완 이유:
- 기존에는 crop 실패가 많아도 조용히 스킵되어 detection 감소로만 보일 수 있었다.

### 5. 문자열 할당 실패와 capacity 캐스팅 방어

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `dup_string()`을 `std::nothrow`로 전환
  - `append_detection()` / `append_debug_detection()`에서 문자열 할당 실패를 감지하고 count 롤백
  - `reserve_pipeline_output()`에서 `size_t -> int` 캐스팅 전 상한 clamp 추가
  - 결과/디버그 버퍼도 `std::nothrow` 할당으로 전환

보완 이유:
- low-memory 상황이나 비정상적으로 큰 capacity 값에서 조용히 깨질 여지를 줄이기 위해서다.

### 6. rec candidate 조립 전 결과 개수 방어

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `build_rec_candidates()`를 `bool` 반환으로 바꾸고
    `cls_results.size() != cls_inputs.size()`를 명시적으로 검증

보완 이유:
- helper 재사용 시 상위 전제 조건이 깨져도 바로 잘못된 인덱싱으로 가지 않도록 막는다.

### 7. rec layout 해석 강건성 보강

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `find_rec_layout()`가 기존 `[N,T,C]`, `[T,C]` 외에
    `[N,T,1,C]`, `[N,1,T,C]`도 해석하도록 확장

보완 이유:
- class axis가 마지막이고 singleton 축만 추가된 출력은
  의미 손상 없이 수용할 수 있는데, 기존에는 바로 실패했다.

### 8. predictor 실패 에러 메시지 일관화

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `set_error_if_empty()` helper 추가
  - `run_det()`, `run_cls()`, `run_cls_batch()`, `run_rec()`, `run_rec_batch()`에서
    predictor 실패 시 하위 레이어가 메시지를 채우지 않아도 기본 에러를 채우도록 정리

보완 이유:
- 호출 규약 차이 때문에 빈 결과만 올라오는 상황을 줄이고,
  실패 원인을 최소한의 문자열로라도 보존하기 위해서다.

### 9. rec_dict 내용 검증 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `validate_recognition_dict()` 추가
  - 빈 엔트리를 제거하고, 유효 문자가 하나도 없는 dict는 엔진 생성 단계에서 실패
  - 공백 토큰이 없으면 보강

보완 이유:
- 파일은 읽혔지만 실제 dict 내용이 비어 있는 경우 결과가 조용히 망가질 수 있었다.

### 10. dict/model class mismatch 가시화

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `run_rec()` / `run_rec_batch()`에서 `num_classes != dict.size() + 1`이면 debug log 남김

보완 이유:
- 완전한 semantic mismatch를 일반 규칙으로 차단하기는 위험하지만,
  최소한 silent corruption을 더 빨리 발견할 수 있게 했다.

### 11. dump 경로 실패 가시화

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `debug_dump_dir()`에서 dump 디렉터리 생성 실패 시 `stderr`에 즉시 경고 출력 후 dump 비활성화

보완 이유:
- 기존에는 dump 경로 문제를 조용히 삼키는 경향이 있어 디버깅 중 원인 파악이 늦어질 수 있었다.

### 12. build 경고 정리

- 파일:
  - `build.rs`
- 변경:
  - `stage_paddle_runtime_shared_libs()`의 unused variable warning 제거

보완 이유:
- 기능 오류는 아니지만, 빌드 로그에서 실제 중요한 경고를 가리지 않도록 정리했다.

## 추가 리스트업과 처리 결과

- `collect_cls_inputs()`:
  - crop 전부 실패 시 에러 승격
- `append_detection()` / `append_debug_detection()`:
  - 문자열 할당 실패 처리 추가
- `reserve_pipeline_output()`:
  - capacity clamp 추가
- `build_rec_candidates()`:
  - 입력/결과 개수 검증 추가
- `decode_ctc()` 계열:
  - dict/model class mismatch를 직접 에러로 바꾸진 않고 debug log로 가시화
- `find_rec_layout()`:
  - singleton 축이 낀 common layout 허용
- `configure_predictor()`:
  - CPU thread 수를 hardware concurrency 기준으로 clamp
- `debug_dump_dir()`:
  - dump 경로 생성 실패를 즉시 표시
- `engine->rec_dict`:
  - 내용 검증 추가
- `run_det()`:
  - predictor 실패 메시지 기본값 보강
- `run_cls_batch()` / `run_rec_batch()`:
  - predictor 실패 메시지 기본값 보강으로 규약 일관화
- `build.rs`:
  - warning 정리

## 추가 테스트

- `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 2`
  - 결과:
    - mean `9120.47ms`
    - median `9120.47ms`

## 최종 판단

- 처음 리스트업했던 후보는 전부 처리했다.
- 다만 `dict/model class mismatch`와 `det semantic mismatch`는 무조건 에러로 승격하면 기존 호환 모델을 깨뜨릴 수 있어,
  이번 단계에서는 가시화와 방어 위주로만 보강했다.
- 정상 경로 parity는 유지됐고, 짧은 벤치 기준 성능 하락도 확인되지 않았다.
