# predictor 부분 재사용 검토

## 배경

현재는 두 번째 OCR에서 oneDNN predictor 예외를 막기 위해
det/cls/rec predictor를 모두 OCR 실행마다 clone해서 사용한다.
이 방식은 안정적이지만 재사용 이점을 일부 포기한다.

재사용을 다시 검토하되, oneDNN 예외가 난 경로를 그대로 되돌리지 않고
입력 shape가 고정인 predictor부터 부분 재사용하는 방향으로 좁힌다.

## 구현 계획

1. det/cls/rec predictor의 입력 shape 변화 특성을 구분한다.
2. 고정 shape라 상대적으로 안전한 predictor만 재사용하도록 조정한다.
3. 나머지 predictor는 clone 전략을 유지해 안정성을 보존한다.
4. 테스트와 빌드로 회귀가 없는지 확인한다.

## 예상 변경 범위

- `native/paddle_bridge/bridge.cc`
- `docs/changes/...`

## 테스트 계획

- `cargo test`
- `deno task test` (`ui/`)
- `deno task build` (`ui/`)

## 구현 내용

- `native/paddle_bridge/bridge.cc`에서 predictor 재사용 전략을 부분 조정했다.
- `cls` predictor는 재사용으로 되돌렸다.
  - `cls` 입력 shape는 `1x3x80x160`으로 고정이라 det/rec보다 재사용 위험이 낮다.
- `det` predictor는 계속 OCR 실행마다 clone한다.
  - 실제 예외 로그의 `onednn_op.conv2d_transpose`와 가변 입력 shape 특성을 고려했다.
- `rec` predictor도 계속 clone을 유지한다.
  - batch width가 입력마다 달라지는 dynamic shape 경로라 현 단계에서는 보수적으로 유지했다.

## 테스트 결과

- 통과: `cargo test`
- 통과: `deno task test` (`ui/`)
- 통과: `deno task build` (`ui/`)
