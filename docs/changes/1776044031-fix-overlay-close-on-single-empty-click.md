# 오버레이 빈 영역 단일 클릭 닫기 회귀 수정

## 배경

- 기존에는 오버레이의 빈 영역을 한 번 클릭하면 오버레이가 닫혔다.
- 현재는 OCR 결과가 표시된 뒤 첫 빈 클릭이 무시되어 두 번 클릭해야 닫히는 회귀가 발생했다.

## 원인

- `ui/src/overlay.tsx`에서 `suppressNextCloseRef`가 영역 선택 제출 직후뿐 아니라
  `ocr_result`, `ocr_error` 이벤트를 받을 때도 다시 `true`로 설정되고 있었다.
- 그 결과 OCR 결과가 화면에 표시된 뒤 첫 배경 클릭이 의도치 않게 소비됐다.

## 변경 대상

- `ui/src/overlay.tsx`
- `ui/src/overlay_close.ts`
- `ui/src/overlay_close_test.ts`
- `ui/deno.json`

## 수정 내용

- 닫기 억제 플래그 전이 규칙을 순수 함수로 분리했다.
- 영역 선택 제출 시에만 다음 배경 클릭 1회를 억제한다.
- `ocr_result`, `ocr_error` 수신 시에는 억제 플래그를 해제해 첫 빈 클릭으로 바로 닫히게 했다.

## 검증 계획

- 닫기 억제 규칙 단위 테스트
- 기존 UI 유틸 테스트와 함께 Deno 테스트 통과 확인
