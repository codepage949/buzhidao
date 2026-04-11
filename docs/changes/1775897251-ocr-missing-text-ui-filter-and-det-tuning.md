# OCR 누락 추적: det 조정 유지 및 UI source filter 제거

## 결론

이번 작업의 최종 결론은 두 갈래다.

- `DET_THRESH`, `BOX_THRESH` 환경변수화
- UI 오버레이의 source language 재필터링 제거

실제 누락 원인은 det/rec 자체가 아니라, OCR 결과를 오버레이에서 다시 source language 기준으로 버리던 UI 필터였다.
그 외 실험 경로(`DET_AUTO_CONTRAST`, 앱/rec 디버그 저장, 기타 후처리 정합성 실험)는 이번 변경 범위에서 제외했다.

## 1. threshold 환경변수화

det 임계값을 코드 상수 대신 환경변수로 조정할 수 있게 했다.

- `DET_THRESH` 기본값: `0.2`
- `BOX_THRESH` 기본값: `0.4`

이 값들은 기능 토글이라기보다 디버깅용에 가깝다.
문제 이미지에서 det 히트맵 이진화와 박스 채택 기준을 빠르게 바꿔 보려는 목적이다.

## 2. UI source filter 제거

오버레이는 OCR이 이미 통과시킨 결과를 다시 언어 정규식으로 필터링하고 있었다.
이 때문에 실제로는 인식된 문장이 파란 박스로 표시되지 않는 문제가 있었다.

- 기존: `source=en/ch` 기준 정규식 불일치 시 오버레이에서 제외
- 변경: OCR 통과 결과는 모두 그룹핑 대상으로 유지
- `source`는 이제 영어 공백 결합 여부 같은 join 규칙에만 사용

디버깅 과정에서 `OCR_DEBUG_TRACE`도 추가했다.

- `false` 기본값에서는 기존과 동일하게 파란 박스만 표시
- `true`면 raw OCR 박스를 분홍색으로 함께 표시해 UI 필터/그룹핑 문제를 추적 가능

## 파일 변경

- `src/config.rs`: `det_thresh`, `box_thresh` 환경변수 파싱 추가
- `src/ocr/det.rs`: threshold 파라미터 전달
- `src/ocr/mod.rs`: `OcrEngine`에 threshold 전달, rec 디버그 trace 추가
- `src/lib.rs`: `OcrEngine::new()`에 threshold 전달
- `src/services.rs`: `OCR_DEBUG_TRACE`를 오버레이 payload로 전달
- `ui/src/detection.ts`: source language 재필터링 제거
- `ui/src/overlay.tsx`: raw/grouped 박스 디버그 표시 추가
- `.env.example`: `DET_THRESH`, `BOX_THRESH` 추가
- `README.md`: 환경변수 문서 갱신

## 테스트

- `cargo test 연결_컴포넌트_분리_검출 -- --nocapture`
- `cargo test deduplicate_boxes_중복_박스_제거 -- --nocapture`
- `cargo check -q`
- `deno task test`
