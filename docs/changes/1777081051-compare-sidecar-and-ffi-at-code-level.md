# 배경

- sidecar와 ffi는 같은 Paddle 계열 OCR 파이프라인을 쓰고, 최근에는 인식률과 비교 벤치도 거의 비슷하게 맞춰 왔다.
- 그런데 실제 프로그램에서 체감 시간을 보면 sidecar가 ffi보다 크게는 약 2초까지 더 빠르게 보이는 경우가 있다.
- 현재 ffi는 `det/cls/rec` stage profiler가 이미 있으나, sidecar는 동일한 수준의 단계별 로그와 설정 덤프가 부족해 코드 레벨 비교가 비대칭이다.

# 이번 작업 목표

1. sidecar와 ffi가 실제로 어떤 설정과 단계 시간을 쓰는지 같은 기준으로 관찰 가능하게 만든다.
2. sidecar 내부 stage별 시간과 배치 계획을 ffi와 1:1로 비교할 수 있게 한다.
3. 이후 실제 속도 차이가 “외부 요인”이 아니라 “남은 코드 차이”인지 바로 좁혀갈 수 있는 기반을 만든다.

# 구현 계획

1. `tools/ocr_sidecar_compare/ocr_sidecar_compare.py`
   - sidecar pipeline 내부에 stage profiler를 추가한다.
   - `det/cls/rec/post/total` 시간과 batch 계획을 stderr/file로 남긴다.
   - lang/device/threshold/image 크기 같은 실행 설정도 함께 기록한다.
2. `tools/scripts/ocr_sidecar_ffi.py`
   - 비교 스크립트에서 sidecar stage profiler를 켜고 읽을 수 있게 한다.
   - sidecar/ffi 둘 다 stage 로그를 남기는 비교 진입점을 정리한다.
3. 테스트
   - 새 파서/로그 포맷 등 순수 로직은 테스트로 고정한다.
   - Python 문법 검증과 Rust 테스트를 통과시킨다.

# 검증 계획

- `python -m py_compile tools/ocr_sidecar_compare/ocr_sidecar_compare.py tools/scripts/ocr_sidecar_ffi.py`
- 관련 Python 단위 테스트
- 필요 시 compare/benchmark 스크립트로 stage 로그가 실제로 남는지 확인

# 구현 후 기록

## 적용 내용

1. `tools/ocr_sidecar_compare/ocr_sidecar_compare.py`
   - `BUZHIDAO_PADDLE_SIDECAR_PROFILE_STAGES=1`일 때 sidecar stage profiler를 켜도록 추가했다.
   - temp 파일 `%TEMP%/buzhi-ocr-sidecar-profile.log`와 stderr에 아래 형식의 로그를 남긴다.
     - `build_ocr settings ...`
     - `run_det profile ...`
     - `run_rec_batch profile ...`
     - `run_pipeline profile ...`
   - 기존 dump instrumentation이 dump dir가 없으면 통째로 빠져버리던 문제를 고쳐, profile 전용 실행에서도 monkey patch가 적용되게 했다.
2. `tools/scripts/ocr_sidecar_ffi.py`
   - `compare` 명령에 `--profile-stages` 옵션을 추가했다.
   - 이 옵션을 켜면 sidecar/ffi profile temp 로그를 실행 전 비우고, 실행 후 읽어서 JSON 결과에 같이 넣는다.
   - 출력 필드:
     - `sidecar_profile_lines`
     - `ffi_profile_lines`
3. `tools/ocr_sidecar_compare/tests/test_pure.py`
   - sidecar profile 메시지 포맷 helper 순수 테스트를 추가했다.

## 검증 결과

- `python -m py_compile tools/ocr_sidecar_compare/ocr_sidecar_compare.py tools/scripts/ocr_sidecar_ffi.py`
  - 통과
- `python -m unittest tools.ocr_sidecar_compare.tests.test_pure`
  - `9 passed`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --profile-stages`
  - parity `7/7 exact match`
  - sidecar profile과 ffi profile이 둘 다 결과 JSON에 포함됨
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --profile-stages`
  - parity `37/37 exact match`
  - stage 차이 요약:
    - `det_ms`: sidecar `3345.340`, ffi `3246.246`
    - `cls_ms`: sidecar `368.551`, ffi `282.776`
    - `rec_ms`: sidecar `8022.452`, ffi `5217.080`
    - `total_ms`: sidecar `11802.549`, ffi `8780.404`

## 현재 해석

- `test4` 기준으로 가장 큰 차이는 `rec_ms`다.
- `det_ms`는 차이가 작고, 현재 프로그램 체감 차이의 주원인은 detection보다 recognition 쪽일 가능성이 높다.
- 다음 라운드에서는 sidecar와 ffi의 rec batch 준비, predictor 입력 shape, decode 경로를 코드 레벨로 더 맞대조하는 것이 우선이다.

## 추가 확인: 04a0ea45 시점 sidecar 앱 경로 재현

- 사용자가 실제로 빠르다고 본 sidecar는 compare용 `tools/ocr_sidecar_compare`가 아니라, `04a0ea45` 시점의 `ocr_server`를 앱이 호출하던 경로였다.
- 그 시점 코드를 확인한 결과:
  - `app/src/ocr/mod.rs`: sidecar 백엔드는 `OCR_SERVER_RESIZE_WIDTH = 1024`를 사용했다.
  - `app/src/services/ocr_pipeline.rs`: OCR 전처리는 `1024w + Lanczos3`였고, `1024h` 제한은 없었다.
  - 현재 ffi 앱 경로는 `src/services/ocr_pipeline.rs`에서 `1024h` 기준 축소를 사용한다.
- 이를 재현하기 위해 `tools/scripts/ocr_sidecar_ffi.py`에 `--legacy-sidecar-app-mode`를 추가했다.
  - sidecar 입력만 `1024w + Lanczos`로 준비하고
  - ffi는 현재 앱의 `1024h` pipeline 경로를 그대로 유지한다.

### test5 결과

- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline --legacy-sidecar-app-mode`
  - sidecar mean `5230.74ms`
  - ffi mean `7851.35ms`
  - mean delta `+2620.61ms` (ffi가 더 느림)
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --profile-stages --legacy-sidecar-app-mode`
  - legacy sidecar input: `1024x522`
  - current ffi input: `2010x1024`
  - legacy sidecar `run_pipeline total_ms=5733.230`
  - current ffi `run_pipeline total_ms=8998.387`

### 해석 보정

- 현재 코드 기준으로 같은 입력 정책(`test5`, 현재 compare sidecar vs 현재 ffi pipeline)에서는 코어 OCR 시간 차이가 약 `100ms` 수준까지 줄어든다.
- 사용자가 실제 프로그램에서 본 `ffi가 2초 이상 느린 현상`은, 과거 sidecar 앱이 더 작은 `1024w` 입력으로 돌았기 때문에 발생했을 가능성이 높다.
- 즉 “외부 의존 라이브러리 차이”보다 “앱 전처리 정책 차이”가 먼저 설명력을 가진다.

## 추가 확인: sidecar와 ffi를 동일한 1024w 입력으로 맞춘 비교

- `tools/scripts/ocr_sidecar_ffi.py`에 `--resize-max-width` 옵션을 추가했다.
- 이제 sidecar와 ffi 모두에 같은 width cap을 적용해 입력 자체를 동일하게 맞출 수 있다.

### test5 결과

- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --resize-max-width 1024 --profile-stages`
  - parity `45/45 exact match`
  - 공통 입력 크기: `1024x522`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test5.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline --resize-max-width 1024`
  - sidecar mean `7177.19ms`
  - ffi mean `4874.00ms`
  - mean delta `-2303.19ms` (ffi가 더 빠름)

### 현재 결론

- `test5`에서 프로그램 체감으로 보이던 `ffi가 2초 이상 느린 현상`은, 동일 입력 기준으로 맞추면 재현되지 않는다.
- 오히려 같은 `1024w` 입력에서는 ffi가 약 `2.3s` 더 빠르다.
- 따라서 지금까지의 핵심 병목은 FFI 엔진 자체보다 “과거 sidecar 앱과 현재 ffi 앱이 서로 다른 전처리 해상도 정책을 사용한 것”이다.
