# OCR 후속 보정: rec 재시도 확장, 그룹 텍스트 재조합, hover 툴팁 교체

## 결론

이번 후속 작업의 결론은 다음과 같다.

- 일부 누락처럼 보이던 문장은 `det` 문제가 아니었다.
- 일부 누락처럼 보이던 문장은 `group.text` 계산이나 기본 브라우저 툴팁 표현 때문에 잘려 보였다.
- 실제 긴 문장 오인식 중 일부는 `batch rec`가 앞부분 짧은 문자열만 내고, `single rec`는 정상인 케이스였다.

따라서 이번 작업에서는 다음을 정리했다.

- `rec` single retry 조건 및 채택 조건 확장
- `group.text`를 `members` 기준으로 최종 재조합
- hover 시 브라우저 기본 `title` 대신 커스텀 툴팁으로 전체 문자열 표시
- 이전 문서(`1775897251`)에서 타일 분할 처리 제거 반영

## 1. rec 재시도 보정 확장

`src/ocr/rec.rs`

- 기존에는 `score < 0.7`일 때만 single retry를 시도했다.
- 이제는 `width >= 120`인데 결과 문자열이 `4자 이하`인 경우도 retry 대상으로 본다.
- retry 결과가 더 긴 의미 있는 문장을 주면 점수 차가 조금 있어도 채택한다.

실제 확인된 케이스:

- batch: `0.775`, `"就好。"`
- single: `0.997`, `"就好像下雪了一样。"`

즉, batch 결과는 짧게 잘렸고 single 결과가 정답에 가까웠다.

## 2. rec 비교 trace 추가

`src/ocr/mod.rs`

- `OCR_DEBUG_TRACE=true`일 때 각 박스마다 `batch`와 `single` 결과를 함께 로그로 출력한다.
- 이를 통해 남은 문제가 `batch 전용`인지, `single도 동일하게 틀리는지`를 바로 구분할 수 있게 했다.

예:

- `[OCR][rec-compare] #09 batch=(0.775, "就好。") single=(0.997, "就好像下雪了一样。")`

## 3. 그룹 텍스트 재조합

`ui/src/detection.ts`

- `group.text`를 병합 중간값으로만 두지 않고, 마지막에 `group.members`를 읽기 순서로 다시 정렬해 재조합한다.
- 따라서 파란 박스에 속한 member가 있으면 최종 `group.text`도 그 members 기준으로 다시 계산된다.
- 부분 문자열 조각이 빠지거나, 병합 중간 상태 때문에 텍스트가 누락되는 경로를 줄였다.

추가로:

- 같은 줄 병합 판단을 adaptive gap과 중심선 거리 기준으로 보강
- 완전 중복에 가까운 nested fragment만 건너뛰도록 조정
- 그룹 후보는 첫 매칭이 아니라 더 가까운 그룹을 선택

## 4. hover 표시 보정

`ui/src/overlay.tsx`

- 마우스를 올렸을 때 보이던 텍스트가 빠져 보인 것은 브라우저 기본 `title` 툴팁 한계였다.
- 이제 기본 툴팁을 제거하고, 오버레이 내부 커스텀 패널로 `group.text` 전체를 그대로 보여준다.
- 번역에 전달되는 문자열과 hover에 보이는 문자열이 같은 소스를 쓰도록 맞췄다.

## 5. 타일 분할 제거 반영

`1775897251` 문서와 코드 기준으로 정리:

- `det` 타일 분할 경로는 제거했다.
- `DET_THRESH`, `BOX_THRESH` 환경변수화는 유지한다.
- `src/ocr/mod.rs`는 다시 단일 `detect_with_resize_long()` 호출만 사용한다.

## 파일 변경

- `docs/changes/1775897251-ocr-missing-text-ui-filter-and-det-tuning.md`
  - 타일 분할 제거 반영
- `src/ocr/det.rs`
  - 타일 분할 코드 제거
  - det 중복 박스 테스트 helper를 테스트 전용으로 정리
- `src/ocr/mod.rs`
  - det 단일 경로 복귀
  - `batch/single` 비교 trace 추가
- `src/ocr/rec.rs`
  - single retry 의심 조건 확대
  - retry 채택 조건 완화
  - 관련 테스트 추가
- `ui/src/detection.ts`
  - 그룹 후보 선택 보정
  - nested duplicate 처리 보정
  - `members` 기반 최종 `group.text` 재조합
- `ui/src/detection_test.ts`
  - 그룹핑/중복/부분 문자열 관련 테스트 추가
- `ui/src/overlay.tsx`
  - hover 커스텀 툴팁 추가

## 테스트

- `cargo check -q`
- `cargo test 연결_컴포넌트_분리_검출 -- --nocapture`
- `cargo test deduplicate_boxes_중복_박스_제거 -- --nocapture`
- `cargo test low_score_single_retry_기준은_0_7이다 -- --nocapture`
- `cargo test 긴_crop의_두글자_결과는_single_retry_대상이다 -- --nocapture`
- `cargo test 긴_crop의_세글자_절단도_single_retry_대상이다 -- --nocapture`
- `cargo test retry가_점수는_조금_낮아도_의미있게_길면_채택된다 -- --nocapture`
- `cargo test retry가_훨씬_긴_문장을_주면_점수차가_커도_채택된다 -- --nocapture`
- `cargo test 외부_crop이_있으면_rec_batch와_single을_비교한다 -- --nocapture`
- `cargo test 외부_crop이_있으면_multi_batch와_single을_비교한다 -- --nocapture`
- `deno task test`
- `deno task build`
