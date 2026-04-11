# rec batch 오인식 보정: single retry 및 디버그 경로 추가

## 결론

이번 추가 작업의 핵심 결론은 다음과 같다.

- 문제 문장은 `det` 누락이 아니었다.
- `crop_box`도 정상이었다.
- `grouping/UI filter` 문제도 아니었다.
- 실제 원인은 `rec`의 다중 이미지 batch 경로에서만 발생하는 오인식이었다.

같은 crop에 대해:

- `single rec` 결과: 정상
- `multi-image batch rec` 결과: 오인식 + 낮은 score

따라서 `rec` low-score 결과에 대해 `single rec`로 재시도하는 보정 경로를 추가했다.

## 1. 원인 추적

문제 crop를 직접 저장해 비교한 결과:

- crop 이미지는 정상
- batch 1장 비교는 의미가 없었다
  - `recognize_batch()`는 입력이 1장이면 곧바로 `recognize()`로 빠진다
- 실제 앱과 같은 다중 crop batch에서만 오인식이 재현됐다

실제 비교 예:

- 문제 crop: `09-reject-s0_470-___n____.png`
- multi batch 결과: `score=0.469`, `text="'['n。']'"`
- single 결과: `score=0.999`, `text="的感觉都没有。"`

즉, 배치 경로의 결과가 잘못되더라도 single 경로는 같은 crop를 정상적으로 읽는다.

## 2. rec 정합성 보정

### 2-1. 긴 문장 너비 상한 완화

`src/ocr/rec.rs`

- `MAX_W`를 `768`에서 `3200`으로 상향
- `target_width` 계산을 참조 구현과 같은 `+0.5` 반올림으로 수정

이 변경으로 긴 문장이 과도하게 수평 압축되는 문제를 줄였다.

### 2-2. low-score single retry 추가

`src/ocr/rec.rs`

- batch 결과 중 `score < 0.7`인 항목만 single 경로로 재시도
- single 결과가 더 좋으면 그 결과로 교체
- 고신뢰 batch 결과는 그대로 유지

이 방식은 전체를 single로 돌리지 않으면서도,
실제 문제였던 batch 전용 오인식 케이스를 바로 교정한다.

## 3. 디버그 경로 보강

문제 원인을 좁히기 위해 디버그 경로를 추가했다.

### Rust side

- `OCR_DEBUG_TRACE=true`일 때 rec 박스별 crop를 `target/ocr-debug-crops`에 저장
- 파일명에 index / accept 여부 / score / text 일부를 포함
- `debug_detections` payload에 `text`, `score`, `accepted`를 함께 전달

### UI side

- 분홍 박스: accept 전 raw rec 결과
- 파란 박스: grouping 후 결과
- 디버그 라벨에 score / text / member 정보를 표시

이로써 `det → rec → accept/reject → grouping` 중 어느 단계에서 문제가 생기는지 구분 가능해졌다.

## 파일 변경

- `src/ocr/rec.rs`
  - `MAX_W=3200`
  - `target_width` 반올림 정합성 수정
  - `score < 0.7` single retry 추가
  - batch/single 비교용 테스트 helper 추가
- `src/ocr/mod.rs`
  - debug crop 저장
  - raw rec debug payload 생성
  - 외부 crop 기준 batch/single 비교 테스트 추가
- `src/services.rs`
  - `debug_detections` payload 추가
- `ui/src/overlay.tsx`
  - raw rec 디버그 표시 강화
- `ui/src/detection.ts`
  - trace group 구조 유지

## 테스트 / 검증

- `cargo check -q`
- `deno task test`
- `cargo test target_width는_MAX_W로_상한된다 -- --nocapture`
- `cargo test target_width는_참조구현처럼_round_half_up을_쓴다 -- --nocapture`
- `cargo test 외부_crop이_있으면_rec_batch와_single을_비교한다 -- --nocapture`
- `cargo test 외부_crop이_있으면_multi_batch와_single을_비교한다 -- --nocapture`

## 후속 정리

최종 동작이 안정화되면 아래 디버그 경로는 축소 가능하다.

- `OCR_DEBUG_TRACE`
- `target/ocr-debug-crops` 저장
- raw/group/member 라벨 표시
