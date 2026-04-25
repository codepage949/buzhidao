# 배경

- 실제 프로그램 실행 로그에서 `emit_ms`는 거의 0이고, `ffi_ms`가 8초 이상으로 병목이었다.
- 기존 stage profiler로 확인한 결과, `ffi_ms` 대부분은 `det predictor`와 `rec predictor`가 차지했다.
- 현재 앱을 `cargo tauri dev`나 test profile로 실행하면 네이티브 bridge C++ 코드도 디버그 최적화 수준으로 빌드된다.
- 이 상태는 실제 사용자가 체감하는 FFI 속도를 과도하게 떨어뜨릴 수 있다.

# 이번 작업 목표

1. dev/test 실행에서도 네이티브 FFI bridge를 강하게 최적화해 `ffi_ms`를 낮춘다.
2. stage profiler 로그를 실제로 보이게 정리해, 이후 det/cls/rec 병목을 바로 읽을 수 있게 한다.
3. 앱 시작 warmup이 predictor 생성에만 머무르지 않고, 첫 실제 OCR 전에 `det/cls/rec` 초기화 비용을 일부 소진하게 만든다.

# 구현 계획

1. `build.rs`
   - `cc::Build`에 높은 최적화 레벨을 강제한다.
   - 디버그 빌드에서도 bridge C++가 `-O3`/`/O2` 수준으로 컴파일되게 한다.
2. `native/paddle_bridge/bridge.cc`
   - `run_pipeline profile` 로그를 `debug_log`가 아니라 `profile_log`로 남긴다.
   - `cls_ms`, `rec_ms`, `total_ms`를 stage profiler에서 즉시 확인 가능하게 한다.
3. `src/ocr/paddle_ffi.rs` + `native/paddle_bridge/bridge.{h,cc}`
   - FFI warmup을 엔진 생성만 하는 수준에서 predictor warmup까지 확장한다.
   - `det/cls/rec` predictor를 small synthetic input으로 한 번씩 실행해 첫 real OCR의 cold-start 비용을 줄인다.
4. `native/paddle_bridge/bridge.cc`
   - predictor별 `MKLDNN / NewIR / NewExecutor` 토글을 env로 분리해 반복 벤치로 실제 속도를 비교한다.
   - 이 실험 결과를 바탕으로 `rec predictor`만 `NewIR` 기본값을 `false`로 조정한다.

# 검증 계획

- `cargo test --lib -- --nocapture`
- 앱 OCR 샘플 테스트를 stage profiler와 함께 실행해 `run_pipeline profile` 수치 확인
- 필요 시 pipeline compare/benchmark 재확인

# 구현 후 기록

## 적용 내용

1. `build.rs`
   - native bridge C++ 빌드에 `opt_level(3)`을 강제했다.
   - MSVC에서는 `/O2`, 그 외 환경에서는 `-O3`를 추가해 dev/test 실행에서도 bridge가 강하게 최적화되도록 했다.
2. `native/paddle_bridge/bridge.cc`
   - `run_pipeline profile ...` 요약을 `debug_log`가 아니라 `profile_log`로 출력하게 바꿨다.
   - 이제 `BUZHIDAO_PADDLE_FFI_PROFILE_STAGES=1`만 켜면 `det_ms`, `cls_ms`, `rec_ms`, `total_ms`를 바로 확인할 수 있다.
3. `src/ocr/paddle_ffi.rs` + `native/paddle_bridge/bridge.{h,cc}`
   - `warmup()`이 엔진 생성 후 `buzhi_ocr_warmup_predictors()`를 호출하도록 바꿨다.
   - native bridge에는 predictor 전용 warmup 함수를 추가했다.
   - warmup은 blank OCR 한 번이 아니라 아래 세 단계를 직접 태운다.
     - `det predictor`: small synthetic text-like image
     - `cls predictor`: batch size 6 synthetic crop
     - `rec predictor`: batch size 6 / width up to 384 synthetic crop
   - 언어 전환이나 엔진 재생성 시에는 `warmed=false`로 되돌아가고, 실제 OCR 성공 후에도 warmed 상태를 유지한다.
4. `src/ocr/paddle_ffi.rs` 테스트
   - warmup dummy image helper와 `shutdown_state()`의 warmed reset을 검증하는 단위 테스트를 추가했다.
5. `native/paddle_bridge/bridge.cc`
   - predictor 설정을 stage별로 분기할 수 있게 정리했다.
   - env override를 지원한다.
     - `BUZHIDAO_PADDLE_FFI_MKLDNN`
     - `BUZHIDAO_PADDLE_FFI_NEW_IR`
     - `BUZHIDAO_PADDLE_FFI_NEW_EXECUTOR`
     - `BUZHIDAO_PADDLE_FFI_DET_*`
     - `BUZHIDAO_PADDLE_FFI_CLS_*`
     - `BUZHIDAO_PADDLE_FFI_REC_*`
   - 반복 벤치 결과를 근거로 `rec predictor`의 `NewIR` 기본값만 `false`로 바꿨다.

## 보류/되돌린 시도

- 동일 rec input width를 더 큰 batch로 묶는 시도를 넣어 봤지만 parity가 깨졌다.
- `test2`, `test3`, `test4`에서 exact match가 무너져 유지하지 않았다.
- 이 시도는 코드에서 되돌렸다.
- `rec predictor` warmup에 wide shape(`1680`)를 추가로 넣어 봤지만,
  첫 real OCR `run_pipeline total_ms`가 오히려 `8978ms -> 9199ms`로 흔들려 유지하지 않았다.
- startup warmup 비용만 늘고 이득이 안정적이지 않아 이 시도도 코드에서 되돌렸다.
- `rec predictor`에서 `MKLDNN`을 끄는 시도는 parity는 유지됐지만
  실제 앱 OCR 벤치가 `~10s`에서 `12~16s`대로 악화돼 유지하지 않았다.
- `rec predictor`에서 `NewExecutor`를 끄는 시도는 Paddle 3.x에서
  `InvalidArgumentError: Not find predictor_id ... memory_optimize_pass`로 초기화가 깨져 유지하지 않았다.

## 검증 결과

- `cargo test --lib -- --nocapture`
  - `85 passed; 0 failed`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: `7/7 exact match`
  - `test2.png`: `14/14 exact match`
  - `test3.png`: `18/18 exact match`
  - `test4.png`: `37/37 exact match`
- 앱 OCR 샘플 stage profile:
  - 변경 전:
    - `run_pipeline profile image=rgba_memory, boxes=64, cls_inputs=64, rec_candidates=64, rotated=6, det_ms=3540.066, crop_ms=28.105, cls_ms=451.681, rotate_ms=0.452, rec_ms=5477.765, post_ms=0.020, total_ms=9498.310`
  - predictor warmup 추가 후:
    - `run_pipeline profile image=rgba_memory, boxes=64, cls_inputs=64, rec_candidates=64, rotated=6, det_ms=3197.260, crop_ms=16.397, cls_ms=285.250, rotate_ms=4.111, rec_ms=4908.175, post_ms=0.030, total_ms=8411.397`
  - `rec predictor NewIR` 기본값 조정 후:
    - `run_pipeline profile image=rgba_memory, boxes=64, cls_inputs=64, rec_candidates=64, rotated=6, det_ms=3203.618, crop_ms=14.500, cls_ms=313.267, rotate_ms=0.429, rec_ms=5124.837, post_ms=0.013, total_ms=8656.812`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: `7/7 exact match`
  - `test2.png`: `14/14 exact match`
  - `test3.png`: `18/18 exact match`
  - `test4.png`: `37/37 exact match`
- 실제 앱 OCR 벤치(`test4.png`, warmups=1, iterations=2)
  - 기존 기본값: `10468.217ms`, `10329.637ms`
  - `rec NewIR`만 끈 실험값: `9989.263ms`, `9976.310ms`
  - 최종 기본값 반영 후(env 정리 상태): `9892.952ms`, `9895.041ms`

## 결론

- 이번 라운드에서 안전하게 넣은 것은 세 가지다.
  - dev/test 네이티브 최적화 강제
  - stage profiler 가시화
  - startup predictor warmup 추가
- 거기에 더해 predictor 설정 자체도 반복 벤치로 좁혔다.
- `rec predictor`는 `NewIR`를 켠 기본값보다 끈 쪽이 실제 앱 OCR 반복 벤치에서 더 빨랐다.
- 마지막 항목이 실제 첫 OCR 지연에 가장 직접적인 효과를 냈다.
- 같은 `test4` 샘플에서 warmup 이후 첫 real OCR `total_ms`는 `9498ms -> 8411ms`로 내려갔다.
- 현재 기본값 기준 실제 앱 OCR 벤치(`test4`, warmups=1, iterations=2)는 약 `10.4s -> 9.9s` 수준으로 내려갔다.
- 병목은 여전히 `det predictor`와 `rec predictor`지만, cold-start 비용과 `rec` 쪽 불리한 executor 설정은 일부 걷어냈다.
