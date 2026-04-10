# rec 배치: 너비 정렬 + 청크 처리로 LSTM 오버헤드 제거

## 문제

CUDA EP + cuDNN 활성화 후에도 rec(171박스)이 81초로 느림.

원인: `recognize_batch`가 모든 이미지를 단일 배치로 처리하면서
**max_w = 전체 최대 너비**로 패딩한다. 2560×1440 화면에서 넓은 텍스트 박스가
있으면 max_w가 수천 픽셀에 달하고, LSTM time step = max_w/4가 폭발적으로 증가한다.
좁은 이미지(100px)도 2000px까지 패딩되어 20배 느려진다.

## 해결

PaddleOCR Python의 rec 배치 전략을 적용한다:

1. **너비 오름차순 정렬** — 비슷한 너비의 이미지끼리 청크를 구성
2. **REC_BATCH_SIZE=16 단위 청크** — 청크 내 max_w로만 패딩
3. 원본 순서 복원 — 정렬된 결과를 원래 인덱스로 재배치

```
before: 1개 배치, max_w = 전체 최대 (수천 px)
after:  N개 청크, 청크별 max_w = 해당 청크 최대 (훨씬 작음)
```

## 변경 내용

### `src/ocr/rec.rs`

- `try_batch_recognize` 시그니처: `&[DynamicImage]` → `&[&DynamicImage]` (복사 제거)
- `recognize_batch`: 너비 정렬 → 청크별 배치 처리 → 원본 인덱스 복원
- `REC_BATCH_SIZE = 16` 상수 추가

## 예상 효과

171박스, max_w=2000인 경우:
- before: 171×3×48×2000 = 49.2M floats, LSTM 500 steps
- after (청크=16, 청크 max_w≈300): 16×3×48×300 = 0.7M floats, LSTM 75 steps
- 패딩 오버헤드 ~6-7배 감소

## 테스트 결과

- `cargo test` — 43개 통과
- `cargo test --features gpu` — 44개 통과
