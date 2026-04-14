# 오버레이 닫기 시 진행 중 OCR 결과 무효화

## 배경

PrtSc → capture → sidecar OCR → overlay emit 흐름에서, 사용자가 중간에 ESC/닫기로
오버레이를 닫아도 OCR 결과가 뒤늦게 도착해 빈 오버레이에 반영되거나
다음 세션을 오염시킬 여지가 있었다.

Rust/Tokio 환경에서 sidecar 호출을 중간에 "강제 중단"하려면 사이드카 프로세스를
kill해야 하는데, Paddle 모델 재로딩 + warmup 비용이 커서 UX가 더 나빠진다.
따라서 연산은 그대로 돌리되, 결과 emit을 무효화하는 세대(generation) 토큰 방식을 채택한다.

## 변경

- `OcrJobGen(AtomicU64)`를 앱 상태로 추가.
- `handle_prtsc` 진입 시 `fetch_add(1) + 1`로 새 세대 번호를 발급하고 로컬에 기록.
- `run_region_ocr` 진입 시 현재 세대 번호를 스냅샷(영역 선택은 같은 세션의 연속이므로 bump하지 않음).
- OCR 완료 후 emit 직전 `current == snapshot`일 때만 `ocr_result`/`ocr_error` emit.
  `emit_ocr_outcome`에 gen 비교를 감싸는 버전 추가.
- `close_overlay`에서 `fetch_add(1)`로 진행 중 작업을 무효화.

## 핫키 차단

OCR 전 구간에서 `busy = true`가 유지되므로 PrtSc는 계속 무시된다.
close_overlay 후에도 busy는 handle_prtsc 끝에서만 false로 풀린다
(즉 sidecar 응답을 받은 시점). 사용자가 취소 직후 즉시 PrtSc를 누르면
현재 OCR이 끝날 때까지 대기.

## 검증

- 단위 테스트: `should_emit(my_gen, current_gen) -> bool` 순수 함수로 추출해
  일치/불일치 시 동작을 검증.
- `cargo test --lib` 통과.
- 실제 취소 시점 UX(오버레이 즉시 닫힘, 뒤늦은 emit 없음)는 육안 검증 영역.
