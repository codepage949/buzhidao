# OCR 전처리/후처리 버그 수정 — det/cls BGR 정규화 + 회전 최소 사각형 + 폴리곤 점수

## 배경

ONNX 포팅 후 중국어 텍스트 탐지 능력이 크게 저하(검출 ~9개 → Python 참조 43개).
전처리 BGR 채널 정규화 오류와 후처리 알고리즘 차이가 원인.

## 버그 및 수정

### 1. det.rs/cls.rs — BGR mean/std 인덱스 오류

PaddleOCR는 OpenCV(BGR)로 읽고 mean/std를 채널 위치 순서로 적용한다.
기존 코드는 색상 이름 기준으로 매핑해 B↔R의 mean/std가 뒤바뀌어 있었다.

```rust
// Before (잘못됨): B에 R의 mean/std 적용
tensor[[0, y, x]] = (pixel[2] * SCALE - MEAN[2]) / STD[2]; // B → 0.406/0.225
tensor[[2, y, x]] = (pixel[0] * SCALE - MEAN[0]) / STD[0]; // R → 0.485/0.229

// After (수정): 채널 위치 순서로 적용
tensor[[0, y, x]] = (pixel[2] * SCALE - MEAN[0]) / STD[0]; // B → 0.485/0.229
tensor[[2, y, x]] = (pixel[0] * SCALE - MEAN[2]) / STD[2]; // R → 0.406/0.225
```

cls.rs도 동일한 BGR 변환 적용.

### 2. det.rs — 외곽선 추적 (flood fill → Suzuki-Abe border following)

기존 flood fill 연결 컴포넌트 탐색은 인접 텍스트 줄을 하나의 거대 영역으로 합침.
OpenCV `findContours(RETR_LIST)` 동작을 모방하는 Suzuki-Abe 외곽선 추적으로 교체.

### 3. det.rs — 회전 최소 면적 사각형 (AABB → rotating calipers)

기존 축 정렬 바운딩 박스(AABB)는 기울어진 텍스트 영역에서 과도한 영역을 포함.
convex hull + 각 edge 방향 투영으로 최소 면적 회전 사각형 구현.

### 4. det.rs — 폴리곤 마스크 점수 (AABB 평균 → polygon mask 평균)

기존 `box_score_fast`는 AABB 내 모든 픽셀을 평균해 배경이 포함되면 점수가 낮아짐.
PaddleOCR 방식대로 min_area_rect 4점 폴리곤 내부만 평균하는 `box_score_poly`로 교체.

### 5. Python 비교 스크립트 추가

`scripts/compare_onnx.py` — 동일 ONNX 모델로 PaddleOCR 전처리를 적용한 참조 결과 생성.

### 6. 단위 테스트 보강

`det.rs`와 `cls.rs`에 BGR 정규화 채널 순서 검증 테스트를 추가했다.
`det.rs`에는 폴리곤 내부 평균 점수가 배경 픽셀에 희석되지 않는지 확인하는 테스트도 추가했다.

## 결과 비교 (test.jpg — 중국어 서적 2쪽)

| 항목 | 수정 전 | 수정 후 | Python 참조 |
|------|---------|---------|-------------|
| det 박스 | ~9 | 42 | 43 |
| 최종 텍스트 | ~6 | 22 | 38 |

수정 후 det 박스 수는 Python과 거의 동일(42 vs 43).
최종 텍스트 수 차이(22 vs 38)는 rec 단계 score 필터링 차이.

## 수정 파일

- `app/src/ocr/det.rs` — BGR 정규화 수정, 외곽선 추적, 회전 사각형, 폴리곤 점수
- `app/src/ocr/cls.rs` — BGR 정규화 수정, 채널 순서 테스트 추가
- `app/src/ocr/mod.rs` — 중국어 이미지 비교 테스트 추가
- `test.jpg` — 중국어 이미지 비교용 테스트 입력
- `scripts/compare_onnx.py` — 신규, Python 참조 비교 스크립트
