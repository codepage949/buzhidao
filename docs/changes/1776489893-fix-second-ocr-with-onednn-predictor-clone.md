# 두 번째 OCR에서 oneDNN predictor 예외 수정

## 배경

Paddle FFI CPU 경로는 oneDNN(MKLDNN)을 켠 predictor를 엔진에 장기 재사용한다.
그런데 첫 번째 OCR 뒤 두 번째 OCR을 수행하면 `onednn_op.conv2d_transpose` 예외와
`predictor 실행 예외: Unknown exception`이 발생했다.

이번 작업에서는 oneDNN은 유지하되 OCR 사이 predictor 내부 상태가 누적되지 않도록
실행 단위를 분리하고, 더 이상 사용하지 않을 `BUZHIDAO_PADDLE_FFI_SAFE_MODE`
분기를 제거한다.

## 구현 계획

1. native Paddle bridge의 predictor 재사용 경로와 safe mode 분기를 확인한다.
2. oneDNN은 유지한 채 각 OCR 실행마다 fresh predictor clone을 사용하도록 수정한다.
3. `BUZHIDAO_PADDLE_FFI_SAFE_MODE` 분기를 제거한다.
4. 테스트와 빌드로 회귀가 없는지 확인한다.

## 예상 변경 범위

- `native/paddle_bridge/bridge.cc`
- `docs/changes/...`

## 테스트 계획

- `cargo test`
- `deno task test` (`ui/`)
- `deno task build` (`ui/`)

## 구현 내용

- `native/paddle_bridge/bridge.cc`에 predictor clone 헬퍼를 추가했다.
- `run_pipeline()` 시작 시 det/cls/rec predictor를 base predictor에서 각각 clone해
  해당 OCR 실행 전용 predictor로 사용하도록 바꿨다.
  - oneDNN은 유지하되 이전 OCR의 predictor 내부 상태가 다음 OCR 실행으로
    누적되지 않도록 분리했다.
- `configure_predictor()`에서 `BUZHIDAO_PADDLE_FFI_SAFE_MODE` 분기와 관련 로그를 제거했다.
  - CPU 경로는 계속 oneDNN(`EnableMKLDNN`, `SetMkldnnCacheCapacity`)을 사용한다.

## 테스트 결과

- 통과: `cargo test`
- 통과: `deno task test` (`ui/`)
- 통과: `deno task build` (`ui/`)
