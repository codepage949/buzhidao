# 오버레이 및 그룹핑 성능 리팩토링

## 목표

최근 OCR 후속 작업 이후 기능은 맞았지만,
오버레이 hover 상호작용과 그룹 후처리 경로에 불필요한 재계산이 남아 있었다.

이번 리팩토링의 목표는 다음 두 가지다.

- hover 상태 변경 시 오버레이의 비싼 계산 재실행 최소화
- 그룹 완료 후 텍스트/멤버 재조합 비용 축소

## 계획

### 1. 오버레이 계산 메모이제이션

`ui/src/overlay.tsx`

- `hoveredIdx`가 바뀔 때마다 `groupDetectionsTraceWithBounds()`와 `rawItems` 생성이 다시 실행되지 않도록 `useMemo`로 고정
- OCR payload가 바뀔 때만 그룹/디버그 박스를 다시 계산

### 2. 그룹 후처리 정렬 비용 축소

`ui/src/detection.ts`

- 그룹 완료 후 `members`를 여러 번 복사/정렬하지 않도록 정리
- 최종 `group.text` 조합 시 정렬 결과를 재사용

## 기대 효과

- hover 이동 시 오버레이 렌더 비용 감소
- 많은 OCR 박스가 있는 화면에서 그룹 후처리 비용 감소

## 테스트

- `deno task test`
- `deno task build`
- `cargo check -q`
