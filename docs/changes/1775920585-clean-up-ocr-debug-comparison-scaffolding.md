# OCR 디버그 비교용 임시 코드 정리

## 결론

`1775895633` 이후 이어진 OCR 작업 3건을 다시 점검한 결과,
실제 동작에 필요한 디버그 경로와 조사 과정에서만 쓰인 임시 비교 코드를 분리할 수 있었다.

이번 정리에서는 다음만 제거했다.

- 런타임 `batch/single` 비교 로그
- 외부 crop 기반 비교용 테스트 경로
- 비교용 helper 함수

다음은 유지했다.

- `OCR_DEBUG_TRACE`
- raw OCR 박스 표시
- crop 저장
- hover 커스텀 툴팁

즉, 실제 운영/디버깅에 쓸만한 기능은 남기고,
조사 당시 일회성으로만 필요했던 코드를 걷어냈다.

## 제거한 항목

### 1. rec batch/single 런타임 비교 로그

`src/ocr/mod.rs`

- `OCR_DEBUG_TRACE=true`일 때 각 박스마다 `batch`와 `single`을 둘 다 돌려 비교 로그를 찍던 경로를 제거했다.
- 이 경로는 문제 원인 규명에는 유용했지만, 평소 debug trace에서도 매 박스마다 single 추론을 추가로 수행해 비용이 컸다.

### 2. 외부 crop 비교 helper와 테스트

`src/ocr/rec.rs`, `src/ocr/mod.rs`

- `recognize_batch_vs_single`
- `recognize_multi_batch_vs_single`
- `BUZHIDAO_OCR_BENCH_CROP`
- `BUZHIDAO_OCR_BENCH_CROP_DIR`
- 외부 crop 비교 테스트

이들은 특정 디버그 세션을 위해 넣은 경로라 현재 상시 유지 가치가 낮다.

## 유지한 항목

### 1. `OCR_DEBUG_TRACE`

여전히 유효한 운영 디버그 기능으로 판단해 유지했다.

- raw OCR 박스와 grouped 결과 비교
- rec accept/reject 로그
- crop 저장

### 2. hover 커스텀 툴팁

기본 브라우저 `title` 툴팁 대신 전체 `group.text`를 정확히 보여주므로 유지했다.

## 파일 변경

- `src/ocr/mod.rs`
  - rec compare 로그 제거
  - 외부 crop 비교 테스트 제거
- `src/ocr/rec.rs`
  - 비교용 helper 제거

## 테스트

- `cargo check -q`
- `cargo test low_score_single_retry_기준은_0_7이다 -- --nocapture`
- `cargo test 긴_crop의_두글자_결과는_single_retry_대상이다 -- --nocapture`
- `cargo test 긴_crop의_세글자_절단도_single_retry_대상이다 -- --nocapture`
- `cargo test retry가_점수는_조금_낮아도_의미있게_길면_채택된다 -- --nocapture`
- `cargo test retry가_훨씬_긴_문장을_주면_점수차가_커도_채택된다 -- --nocapture`
- `deno task test`
