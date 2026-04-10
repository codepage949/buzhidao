# OCR 인식률 작업 분리: dense tile fast path 기본값 복구

## 배경

직전 성능 작업에서 `dense 3x3` 타일 모드에 대해 `full det`를 생략하는
tile-only fast path를 기본 경로로 적용했다.

실측 결과:

- `det 타일 3x3`: 약 `3.8s`
- `rec`: 약 `0.9s`
- 전체 박스 수: `28`

속도는 크게 개선됐지만, 사용자가 실제 인식률 저하를 확인했다.

## 문제 정의

현재 상태는 성능 실험 경로와 기본 OCR 경로가 섞여 있다.

- 성능 관점: dense tile fast path는 유효하다.
- 인식률 관점: 기본 경로에서 `full det`를 생략하면 recall 손실이 크다.

이 둘을 같은 기본 경로에 두면 이후 인식률 작업과 성능 작업이 서로 간섭한다.

## 목표

1. 인식률 작업을 별도 축으로 분리한다.
2. 기본 OCR 경로는 `full det + tile det 병합`으로 되돌린다.
3. dense tile fast path는 실험용 옵션으로만 유지한다.

## 변경 계획

### `src/config.rs`

- dense tile fast path 활성화 여부를 환경변수로 분리
- 기본값은 `false`

### `src/services.rs`

- `predict_with_tiles()`가 설정값을 받아
  dense `3x3`에서도 기본적으로 `full det + tile merge`를 수행
- fast path가 켜진 경우에만 `det 전체 생략: dense tile mode` 로그 출력

## 기대 효과

- 기본 OCR 경로의 recall 회복
- 성능 실험 경로와 인식률 작업 경로 분리
- 이후 인식률 개선 작업에서 비교 기준선이 안정화

## 검증 계획

- 설정 기본값 테스트
- dense tile fast path 분기 테스트
- 기존 타일 병합 테스트 재실행

## 구현 결과

### `src/config.rs`

- `OCR_DENSE_TILE_FAST_PATH` 환경변수 추가
- 기본값은 `false`

### `src/services.rs`

- `predict_with_tiles()`에 dense tile fast path 설정값 전달
- `tile_grid >= 3`이어도 기본값으로는 `full det + tile merge` 경로 유지
- fast path가 켜진 경우에만 `det 전체 생략: dense tile mode` 경로 사용
- 분기 판정을 별도 함수로 분리해 테스트 가능하게 정리

## 검증 결과

- `cargo fmt`
- `cargo test ocr_dense_tile_fast_path_기본값은_false다 -- --nocapture`
- `cargo test dense_tile_fast_path는_설정을_켜야만_활성화된다 -- --nocapture`
- `cargo test 타일_우선_병합 -- --nocapture`
