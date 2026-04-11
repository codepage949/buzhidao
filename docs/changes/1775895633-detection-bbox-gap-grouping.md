# OCR 그루핑 알고리즘 개선 및 설정 이름 변경

## 배경

기존 알고리즘의 문제점:
1. **제곱 비교 의미 왜곡** — `X_DELTA=25`가 실제론 |dx| ≤ 5px 임계값. 사용자는 픽셀 단위로 인식하지만 코드는 픽셀² 비교
2. **앵커 드리프트** — 버킷 앵커가 마지막 합친 박스의 `leftBottom`으로 갱신. 줄이 길어질수록 앵커가 밀려 다음 줄 감지 실패 가능
3. **정렬 없는 그리디** — OCR 출력 순서에 의존. 읽기 순서(Y→X)가 보장되지 않음
4. **영어 단어 공백 없음** — 병합 시 `text + text`로 단어가 붙음
5. **불명확한 변수명** — `X_DELTA`, `Y_DELTA`는 "변화량"이라는 범용 의미만 가짐

## 새 알고리즘

### bbox 간격 기반 병합 조건 (canMerge)

앵커 포인트 대신 실제 bounding box 간격으로 판단.
좌우 양방향 대칭 처리 — item이 group 왼쪽에 있어도 X 범위 겹침 기준으로 판단:

```
canMerge(groupBounds, itemBounds, wordGap, lineGap):
  xNear     = item.x <= group.right + wordGap AND item.right >= group.x - wordGap
  yOverlap  = Y 범위가 겹침 (같은 줄)
  yAdjacent = 아이템 상단이 그룹 하단~(하단+lineGap) 사이 (인접 줄)

  return xNear AND (yOverlap OR yAdjacent)
```

초기 구현에서 `item.x - group.right <= wordGap` (단방향) 조건을 사용했으나,
item이 group 왼쪽에 있을 때 음수가 되어 무조건 통과하는 버그 발생.
예) 오른쪽 컬럼(x=500) 아래 줄에 왼쪽 컬럼(x=0)이 잘못 병합됨.
→ X 범위 겹침 기준으로 교체.

### 처리 순서

1. 소스 언어 필터
2. Y→X 오름차순 정렬 (읽기 순서 보장, 입력 순서 무관)
3. 그리디 병합 (canMerge)
4. 영어: 단어 사이 공백 삽입, 중국어: 붙임

## 설정값 변경

| 항목 | 이전 | 이후 |
|------|------|------|
| `X_DELTA` (픽셀²) | `(x2-x1)² ≤ X_DELTA` | — 제거 |
| `Y_DELTA` (픽셀²) | `(y2-y1)² ≤ Y_DELTA` | — 제거 |
| `WORD_GAP` (픽셀) | — | 수평 간격 임계값 (기본 20px) |
| `LINE_GAP` (픽셀) | — | 줄 간격 임계값 (기본 15px) |

## 파일 변경

| 파일 | 내용 |
|------|------|
| `ui/src/detection.ts` | 알고리즘 전면 교체, canMerge 버그 수정 |
| `ui/src/detection_test.ts` | 테스트 헬퍼 및 케이스 재작성 (17개) |
| `src/config.rs` | `x_delta`/`y_delta` → `word_gap`/`line_gap` |
| `src/services.rs` | `OcrResultPayload` 필드명 변경 |
| `ui/src/overlay.tsx` | 페이로드 필드명 변경 |
| `.env.example` | `WORD_GAP=20`, `LINE_GAP=15` |
| `README.md` | 환경변수 목록 갱신 |
