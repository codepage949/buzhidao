# FFI 성능 저해 요소 단계별 정리

## 배경

- 현재 FFI 경로의 큰 병목은 여전히 `rec predictor_ms`다.
- 다만 predictor 자체를 바꾸기 전에, 현재 코드에서 불필요하게 드는 고정 비용이 남아 있는지 다시 정리할 필요가 있다.
- 이번 라운드는 “품질을 건드리지 않는 저위험 군더더기 제거”를 다시 한 번 점검하는 단계다.

## 목표

- sidecar 비교가 아니라 FFI 자체에서 남아 있는 불필요한 오버헤드를 줄인다.
- 한 번에 전부 바꾸지 않고, hot path 기준으로 단계별로 나눠서 처리한다.
- parity에 영향을 주지 않는 변경만 유지한다.

## 단계별 계획

### 1단계: hot path 로그 오버헤드 제거

- 대상
  - `run_det`
  - `run_rec`
  - `run_rec_batch`
  - `run_pipeline`
- 확인 포인트
  - debug가 꺼져 있어도 문자열 조립이 먼저 일어나는 `debug_log(...)`
  - 반복 batch에서 매번 발생하는 mismatch/debug 문자열 생성
- 성공 기준
  - 기능 변화 없음
  - debug off 기본 경로에서 불필요한 문자열 조립 제거

### 2단계: predictor I/O 메타데이터 재조회 제거

- 대상
  - `run_predictor`
  - `run_predictor_into_buffer`
- 확인 포인트
  - 매 호출마다 `GetInputNames()` / `GetOutputNames()`를 다시 읽는 비용
  - predictor별 고정 메타데이터를 캐시할 수 있는지
- 성공 기준
  - 기능 변화 없음
  - predictor 호출 경계의 반복 조회 제거

### 3단계: 결과 확인

- 검증
  - `cargo test --lib ocr_pipeline -- --nocapture`
  - `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
  - 필요시 `profile-ffi`로 기존 기준 fixture 재확인
- 판단 기준
  - 테스트 통과
  - parity 흔들림 없음
  - 코드 복잡도 대비 의미 없는 변경은 남기지 않음

## 이번 라운드에서 유지할 것

- 도구화된 `compare-ffi-self`
- 도구화된 `analyze-ffi-corpus`
- modern OneDNN API 정리

## 이번 라운드에서 정리 대상

- debug off 기본 경로에서 의미 없이 조립되는 로그 문자열
- predictor 호출마다 반복되는 입력/출력 이름 조회

## 실제 반영 내용

### 1단계: hot path 로그 오버헤드 제거

- `native/paddle_bridge/bridge.cc`
  - `debug_log_lazy(...)` helper를 추가했다.
  - debug off 기본 경로에서는 문자열을 먼저 조립하지 않도록 바꿨다.
- 적용한 구간
  - `db_postprocess`의 reject/accept/debug summary
  - `run_det output_shape`
  - `run_rec` / `run_rec_batch`의 dict-model mismatch
  - `collect_cls_inputs`의 skipped crop 로그
  - `build_rec_candidates`의 cls debug 로그
  - `append_pipeline_results`의 accepted/rejected box debug 로그
  - `run_pipeline`의 det/result summary 로그
- 해석
  - 기존에는 `debug_log("..."+to_string(...))` 형태라 debug가 꺼져 있어도 문자열 조립 비용이 먼저 들었다.
  - 이번 변경으로 기본 경로에서는 그 비용이 빠진다.

### 2단계: predictor I/O 메타데이터 재조회 제거

- `native/paddle_bridge/bridge.cc`
  - `PredictorIoNames` 구조체를 추가했다.
  - `resolve_predictor_io_names(...)`로 predictor pointer 기준 입력/출력 이름을 캐시한다.
  - `run_predictor`와 `run_predictor_into_buffer`는 더 이상 매 호출마다
    `GetInputNames()` / `GetOutputNames()`를 다시 읽지 않는다.
- 해석
  - det/cls/rec predictor는 수명이 길고 입출력 이름도 고정이다.
  - 따라서 batch마다 같은 메타데이터를 다시 조회할 이유가 없다.

## 검증

### 테스트

```powershell
cargo test --lib ocr_pipeline -- --nocapture
```

- 결과: 통과

### 프로파일 확인

```powershell
python tools/scripts/ocr_sidecar_ffi.py profile-ffi --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 2 --sidecar-format png --ffi-format png --ffi-mode pipeline --cargo-profile release
```

- 결과
  - detection count: `34`
  - pipeline profile:
    - `det_ms=880.223`
    - `cls_ms=240.586`
    - `rec_ms=3843.669`
    - `total_ms=4970.150`
  - benchmark mean: `4409.5049ms`

## 정리

- 이번 라운드는 모델/정책 변경 없이, 기본 경로의 쓸데없는 고정 비용을 제거하는 정리였다.
- 남긴 것은:
  - lazy debug 문자열 조립
  - predictor I/O 이름 캐시
- 다음 라운드에서 다시 큰 개선을 노리려면, 이제는 다시 `rec predictor_ms` 자체나 모델 축으로 넘어가야 한다.
