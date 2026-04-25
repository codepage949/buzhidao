# FFI OCR crop 비용 계측부터 다시 시작

## 구현 계획

1. `run_pipeline()`에서 det, crop, cls, rec, postprocess 구간 시간을 각각 분리 계측한다.
2. `crop_to_bbox()`와 회전 경로가 실제 전체 지연시간에서 차지하는 비중을 확인한다.
3. 비중이 큰 구간만 다음 최적화 대상으로 좁힌다.
4. 계측 결과를 바탕으로 안전한 최적화만 반영하고 parity를 다시 확인한다.

## 작업 원칙

- 다음 라운드의 첫 목적은 최적화가 아니라 계측이다.
- `compare` parity가 흔들리는 최적화는 속도가 빨라도 채택하지 않는다.
- `warpAffine`/`warpPerspective` 계열 치환은 계측과 parity 근거가 같이 있을 때만 진행한다.

## 현재 가설

- 남은 가장 큰 고정 비용 후보는 `native/paddle_bridge/bridge.cc`의 `crop_to_bbox()` 경로다.
- box 수만큼 `warpPerspective`와 `cv_mat_to_image_bgra()`가 반복되므로,
  추론 전 이미지 메모리 이동이 누적될 가능성이 높다.
- 회전 경로도 후보지만, 단순 치환은 이미 parity를 깬 적이 있으므로
  먼저 실제 비중을 확인해야 한다.

## 시작 상태

- 직전 커밋: `e349c27` (`perf: FFI OCR 후처리 복사 비용 줄이기`)
- 최근 FFI 단독 수치:
  - `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 2`
  - mean `9522.85ms`
  - median `9522.85ms`

## 구현

### 1. `run_pipeline()` 단계별 계측 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `BUZHIDAO_PADDLE_FFI_PROFILE_STAGES=1`일 때
    `load / det / crop / cls / rotate / rec / post` 시간을 각각 측정
  - 계측 요약은 `run_pipeline profile ...` 한 줄로 출력
  - `TRACE`가 같이 켜져 있을 때 기존 debug 로그에도 같은 요약을 남기도록 연결

의도:
- 다음 최적화 후보를 감으로 고르지 않고
  실제 단계별 시간 비중으로 좁히기

## 테스트

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`

- 단계별 계측:
  - `BUZHIDAO_PADDLE_FFI_PROFILE_STAGES=1 BUZHIDAO_PADDLE_FFI_TRACE=1 BUZHIDAO_RUN_FFI_BENCH=1 ... cargo test --features paddle-ffi -- --nocapture --exact "ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다"`
  - `test4` 결과:
    - total `11077.303ms`
    - load `63.806ms`
    - det `5881.566ms`
    - crop `30.542ms`
    - cls `374.787ms`
    - rotate `0.630ms`
    - rec `4724.480ms`
    - post `0.009ms`
    - boxes `68`
    - rotated `6`

## 현재 결론

- 초기 가설과 달리 `crop_to_bbox()`는 병목이 아니다.
  `68`개 box 기준 전체에서 약 `30ms` 수준이라 최적화 우선순위가 낮다.
- 실제 큰 비용은
  - det 약 `5.9s`
  - rec 약 `4.7s`
  두 predictor 실행 구간이다.
- 회전 경로도 `0.63ms` 수준이라,
  parity 리스크를 감수할 만한 이득이 현재 측정으로는 보이지 않는다.

즉 다음 최적화는 crop/rotate가 아니라
det·rec predictor 호출 자체, 또는 그 직전 전처리와 입력 shape 쪽을 봐야 한다.

## 다음 단계

- `run_det()` 내부를
  - det preprocess
  - det predictor
  - det postprocess
  로 분리 계측한다.
- `run_rec_batch()` 내부를
  - rec input prepare
  - rec tensor fill
  - rec predictor
  - rec decode
  로 분리 계측한다.

이 단계의 목적은
`det 5.9s`, `rec 4.7s` 안에서 실제로 줄일 수 있는 구간이 어디인지
한 단계 더 좁히는 것이다.

## 다음 최적화 가설

- `run_rec_batch()` 계측상 `prepare/fill/decode`는 매우 작고,
  실제 비용은 대부분 `predictor_ms`다.
- 특히 `batch_w`가 `588`, `801`, `1444`처럼 커지는 묶음에서 predictor 시간이 급격히 증가한다.
- 현재는 `rec_order`를 ratio 순으로 정렬한 뒤 무조건 `6`개씩 자르기 때문에,
  폭이 큰 후보 몇 개가 같은 배치에 묶이면서 padding 낭비가 커진다.

따라서 다음 시도는
fixed `6`개 배치 대신, 추정 `rec width` 기준으로
batch 폭 예산을 넘지 않도록 rec 묶음을 더 잘게 자르는 것이다.

## 추가 구현

### 2. det/rec 내부 세부 계측 추가

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `run_det()`에
    - preprocess
    - predictor
    - postprocess
    계측 추가
  - `run_rec_batch()`에
    - prepare
    - fill
    - predictor
    - decode
    계측 추가

의도:
- `det`와 `rec` 안에서도 실제로 줄일 수 있는 구간을 더 정확히 좁히기

### 3. rec 배치를 width budget 기준으로 재구성

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `estimate_rec_input_width()` helper 추가
  - 기존 `kRecBatchSize=6` 고정 슬라이싱 대신
    추정 `rec width * batch_count <= 3000` 예산을 넘지 않도록 rec batch를 분리
  - ratio 정렬 순서는 그대로 유지하고, batch 경계만 padding 친화적으로 조정

의도:
- wide sample 몇 개가 같은 배치에 묶여 `batch_w`가 과도하게 커지는 상황을 줄이기
- predictor 의미는 바꾸지 않고 rec padding 낭비만 줄이기

## 추가 테스트

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`

- parity:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

- FFI 단독 벤치:
  - `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 2`
  - 결과:
    - mean `9325.78ms`
    - median `9325.78ms`

- 세부 계측 재확인:
  - `run_det profile`
    - preprocess `222.360ms`
    - predictor `5700.041ms`
    - postprocess `13.286ms`
  - `run_pipeline rec_batches`
    - `#0:6@320 | ... | #8:6@366 | #9:5@544 | #10:4@630 | #11:3@801 | #12:2@1444`
  - 큰 rec batch 예시:
    - `5@544`: predictor `427.938ms`
    - `4@630`: predictor `413.369ms`
    - `3@801`: predictor `395.582ms`
    - `2@1444`: predictor `461.744ms`
  - 최종 `run_pipeline profile`
    - total `10790.468ms`
    - det `5809.274ms`
    - rec `4527.711ms`

## 갱신된 결론

- `crop`과 `rotate`는 여전히 병목이 아니다.
- `det`는 predictor 자체가 거의 전부이고, preprocess/postprocess는 상대적으로 작다.
- `rec`도 predictor가 대부분이지만,
  wide sample을 덜 공격적으로 묶는 것만으로 padding 낭비를 줄일 수 있었다.
- 이번 변경 후 `test4` 기준 FFI 단독 수치는
  - 기존 `9522.85ms`
  - 현재 `9325.78ms`
  로 추가 개선됐다.

즉 현재 남은 최우선 후보는 rec batch padding은 일부 줄였고,
다음은 det predictor 자체 또는 det 입력 shape 쪽이다.

## 추가 탐색

### 4. det 입력 해상도 축소 가능성 확인

- 확인:
  - `PP-OCRv5_server_det/inference.yml`에는 `DetResizeForTest.resize_long: 960`이 들어 있다.
  - 현재 FFI 기본 경로는 sidecar parity를 맞추기 위해 사실상 원본 크기에 가까운 det 입력을 유지하고 있다.
- 실험:
  - 모델 기본 `resize_long`을 그대로 적용하는 시도는 속도는 크게 좋아졌지만 parity가 크게 깨졌다.
    - `compare` 결과: exact text match `32 -> 7`
    - `verify-ffi` 결과: mean 약 `4204.37ms`
  - 그래서 기본 동작 변경은 즉시 원복했다.
  - 대신 빠른 후속 탐색용으로
    `BUZHIDAO_PADDLE_FFI_DET_RESIZE_LONG`
    env override를 추가해 후보 값을 외부에서 주입할 수 있게 했다.
- 추가 후보 스캔:
  - `1792`, `2048`, `2304`, `2560`을 각각 확인했다.
  - 결과:
    - `1792`: exact `12`
    - `2048`: exact `9`
    - `2304`: exact `13`
    - `2560`: exact `11`

결론:
- det 입력 해상도를 줄이는 방향은 `test4` 기준에서 속도는 좋아져도 sidecar parity가 크게 무너진다.
- 즉 det는 현재 이미지군에서 원본에 가까운 입력을 요구하는 쪽에 가깝고,
  안전한 기본 최적화 후보로 채택하기 어렵다.

### 5. Paddle CPU thread 수 튜닝 확인

- 확인:
  - predictor 설정은 CPU에서 `SetCpuMathLibraryNumThreads(10)`으로 고정돼 있었다.
- 구현:
  - `BUZHIDAO_PADDLE_FFI_CPU_THREADS` env override를 추가해서
    code edit 없이 thread 수를 비교할 수 있게 했다.
- 짧은 벤치:
  - `6`: mean `9257.35ms`
  - `8`: mean `9227.32ms`
  - `10`: mean `9271.11ms`
  - `12`: mean `9312.28ms`
  - `16`: mean `9371.87ms`
- 해석:
  - 이 머신에서는 `8`이 약간 나아 보였지만 차이가 작고 노이즈도 있다.
  - 기본값을 `8`로 잠깐 바꿔 재측정했을 때는 이득이 재현되지 않아 원복했다.

결론:
- CPU thread 수는 미세 조정 후보는 맞지만,
  현재 측정만으로 기본값을 바꿀 만큼 확실한 개선은 아니다.
- 다만 env override를 남겨 두었기 때문에,
  이후 더 긴 벤치나 다른 머신에서 재검증하기는 쉬워졌다.

## 현재 판단

- 안전하게 반영할 수 있는 저위험 코드 최적화는 사실상 소진됐다.
- 남은 큰 후보인 det predictor 입력 축소는 parity 비용이 너무 크다.
- predictor thread 수는 미세 조정 여지는 있지만,
  기본값을 바꿀 만큼 재현성 있는 개선은 아직 없다.

즉 지금 시점에서는
`bridge.cc` 내부 메모리 이동을 더 깎는 것보다,
이미 확보한 계측과 override를 바탕으로
더 넓은 이미지 셋에서 det 정책과 runtime 설정을 다시 검증하는 편이 맞다.
