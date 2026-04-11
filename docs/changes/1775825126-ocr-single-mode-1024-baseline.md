# OCR 단일 모드 전환: full det only + resize_long 1024

## 배경

`fast`, `paddle_compat`, tile 병합, det 입력 스케일 실험을 비교한 결과,
현재 화면 OCR 기준선으로는 아래 조합이 가장 안정적이었다.

- full det only
- cls 전체 실행
- rec 기존 배치 유지
- det `resize_long = 1024`

반대로 다음 실험들은 유지 가치가 낮았다.

- dense tile fast path: 속도는 빨랐지만 인식률 하락
- `resize_long = 1152`: 더 느리고 인식률도 하락
- det no-upscale: 타일 비용은 줄었지만 recall 하락

## 목표

1. OCR 경로를 단일 모드로 단순화한다.
2. 환경변수 없이 동일한 기준선으로 실행되게 한다.
3. 이후 인식률 개선은 이 기준선 위에서만 진행한다.

## 구현 결과

### `src/config.rs`

- `OCR_PROFILE`, `OCR_DET_RESIZE_LONG`, `OCR_ENABLE_CLS`, `OCR_DENSE_TILE_FAST_PATH` 분기 제거
- det 입력 스케일은 상수 [`OCR_DET_RESIZE_LONG`](src\config.rs) `= 1024`로 고정

### `src/services.rs`

- `predict_with_tiles()`는 더 이상 타일 분기를 사용하지 않음
- 단일 경로:
  - `full det only`
  - `resize_long 1024`
  - `recognize_boxes()` 호출
- 로그:
  - `[OCR] det 단일 모드: full det only`

### `src/ocr/mod.rs`

- OCR 전 박스 필터 제거
- cls 샘플 생략 제거
- cls는 항상 전체 박스에 대해 실행
- 타일 det 풀과 관련 보조 코드 제거

### `src/ocr/det.rs`

- det는 `detect_with_resize_long()`만 사용
- 작은 입력도 stride 배수에 맞춰 확대 가능하게 유지
- `resize_long=1024` 기준 테스트 유지

## 검증 결과

- `cargo fmt`
- `cargo test ocr_det_resize_long_기본값은_1024다 -- --nocapture`
- `cargo test 단일_ocr_모드는_det_resize_long_1024를_사용한다 -- --nocapture`
- `cargo test 단일_ocr_모드는_cls_샘플_생략을_사용하지_않는다 -- --nocapture`
- `cargo test det_resize_long은_128_배수로_올림된다 -- --nocapture`
- `cargo test det_resize_long은_작은_입력도_stride에_맞춰_확대될_수_있다 -- --nocapture`

## 현재 기준선

- det: full det only
- det 입력 크기: `resize_long = 1024`
- cls: 전체 실행
- rec: 기존 배치 경로 유지

## 다음 단계

- 같은 이미지 셋에서 누락된 텍스트 유형을 수집
- det 후처리 임계값이나 crop 방식이 실제 누락 원인인지 분리 측정
- 성능 최적화는 다시 넣더라도 이 단일 기준선과 A/B로만 비교
