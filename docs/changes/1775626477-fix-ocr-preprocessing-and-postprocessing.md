# OCR 전처리/후처리 및 앱 입력 조건 보정 — det/cls BGR 정규화 + PaddleOCR식 unclip/warp crop + 화면 OCR 해상도/점수 임계값

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
중간 단계까지 수정 후에도 최종 텍스트 수 차이(22 vs 38)는 rec crop 및 unclip 정합성 차이로 남아 있었다.

## 추가 원인 및 수정

### 7. services.rs — 전체 화면 선행 축소 제거

앱은 캡처한 전체 화면을 먼저 가로 1024로 축소한 뒤 OCR을 수행하고 있었다.
det는 내부에서 이미 `resize_long=960` 전처리를 하므로, 이 선행 축소는 det 이득 없이 rec crop 해상도만 떨어뜨린다.

특히 임의 화면의 작은 UI 텍스트는 det 박스가 살아 있어도 crop 단계에서 글자가 무너져 최종 결과가 많이 사라졌다.

→ `run_ocr`가 원본 캡처 이미지를 그대로 `engine.predict()`에 전달하도록 수정했다.
좌표 보정용 `scale`도 `1.0`으로 단순화했다.

### 8. config.rs — 기본 OCR score threshold 완화

실제 앱 기본값이 `SCORE_THRESH=0.8`이라, 인식된 텍스트도 많이 탈락하고 있었다.
비교 테스트와 Python 참조 기준에 맞춰 기본값을 `0.5`로 낮췄다.

환경변수로 여전히 덮어쓸 수 있다.

### 9. mod.rs — 기울어진 박스에만 warp crop 적용

축 정렬 crop만 사용하면 실제 화면의 기울어진 텍스트는 rec 입력에서 잘리는 경우가 남는다.
다만 모든 박스에 warp crop을 강제하면 수평 텍스트 회귀가 발생했다.

→ 혼합 전략으로 조정했다.
- 수평에 가까운 박스는 기존 axis-aligned crop 유지
- 충분히 기울어진 박스만 det가 생성한 4점 순서를 그대로 사용해 bilinear warp crop 적용

중요한 점은 det 박스의 원래 점 순서를 다시 재정렬하지 않는 것이다.
재정렬을 넣으면 일부 박스에서 순서가 틀어져 중국어 회귀 테스트가 22개 → 9개로 악화됐다.
원래 순서를 유지하도록 수정 후 회귀가 복구됐다.

### 10. det.rs — PaddleOCR식 unclip에 더 가까운 에지 오프셋 확장

기존 Rust 구현의 `unclip`은 중심점 기준 스케일링이라, 긴 텍스트 박스나 작은 글씨 박스에서 PaddleOCR의 polygon offset과 거동 차이가 컸다.

PaddleOCR `db_postprocess.py`는 `distance = area * unclip_ratio / perimeter`를 구한 뒤 `pyclipper` offset으로 폴리곤을 바깥으로 민다.
Rust에서는 동일한 distance 공식을 유지하고, convex polygon 각 변을 바깥쪽으로 평행 이동한 뒤 인접 선분 교차점으로 새 꼭짓점을 계산하도록 바꿨다.

이는 `pyclipper` 완전 동일 구현은 아니지만, 기존 중심 스케일링보다 PaddleOCR의 박스 확장 방식에 훨씬 가깝다.

### 11. mod.rs — bilinear quad 매핑을 homography 기반 perspective warp로 교체

기존 warp crop은 사각형 내부 bilinear 좌표 보간이라, PaddleOCR `get_rotate_crop_image`의 `getPerspectiveTransform + warpPerspective`와 다르다.

이를 4점 homography 계산 후 역투영 sampling하는 방식으로 교체했다.
수평 박스는 기존 axis-aligned crop을 유지하고, 기울어진 박스에서만 perspective warp를 적용한다.

이 변경 후 중국어 회귀 테스트의 최종 텍스트 수가 `22 -> 38`로 개선되어 Python 참조(`38`)와 같아졌다.

### 12. services.rs — 작은 글씨 누락 대응용 2x2 타일 OCR 추가

후처리를 상당 부분 맞춘 뒤에도 실제 화면의 작은 글씨 누락은 남았다.
원인은 전체 화면가 det 입력의 `resize_long=960`으로 들어가며 작은 UI 텍스트가 너무 작아지는 데 있다.

이를 줄이기 위해 큰 화면에서는 다음 전략을 추가했다.
- 전체 화면 OCR 1회 유지
- 2x2 오버랩 타일(`128px`) OCR 추가 수행
- 타일 결과를 원본 좌표계로 되돌려 병합
- 동일/과도 겹침 결과는 간단한 bbox IoU 기반 중복 제거

이 변경은 작은 글씨 누락을 줄이기 위한 입력 해상도 보강이며, 후처리 정합성과는 별도의 축이다.

### 13. services.rs — 큰 화면에서 3x3 적응형 타일로 확장

2x2 타일만으로도 개선되지만, 2560px급 이상의 큰 화면에서는 각 타일 안에서도 작은 UI 텍스트가 여전히 작을 수 있다.

그래서 타일 전략을 적응형으로 바꿨다.
- 일반 큰 화면: 2x2 타일
- 더 큰 화면(`>= 2400px`): 3x3 타일
- 3x3에서는 overlap도 더 크게 잡아 경계 누락을 줄임

이 단계는 누락 감소를 위한 해상도 보강을 한 번 더 밀어붙인 것이다.

### 14. services.rs — 크기만 다른 과도 중첩 박스 병합 강화

타일 OCR을 추가한 뒤 작은 글씨 누락은 줄었지만, 같은 위치에서 크기만 다른 박스가 함께 남는 경우가 있었다.
기존 중복 제거는 `IoU`, 작은 박스 기준 포함 비율, 같은 텍스트일 때의 중심 거리 정도만 보아서 이런 케이스를 충분히 접지 못했다.

그래서 중복 판정을 다음처럼 강화했다.
- 큰 박스 기준 겹침 비율(`intersection / max(area)`) 추가
- bbox 가로/세로 축 겹침 비율 추가
- bbox 면적 비율과 중심 거리까지 함께 보아 "거의 같은 위치의 큰 박스/작은 박스"를 같은 검출로 판단
- 텍스트가 완전히 같지 않아도, 공백/일부 문장부호를 제거했을 때 부분 문자열 관계이고 기하학적으로 강하게 겹치면 같은 검출로 판단
- 특히 같은 줄에서 긴 박스의 우측 일부만 다시 잡힌 박스도, 세로 정렬과 가로 중첩이 충분하면 중복으로 병합
- 반대로, 같은 텍스트라도 위아래 다른 줄처럼 세로 분리가 있는 경우는 유지

즉 이번 단계는 누락 보완을 위해 늘어난 박스 수를 다시 정리해 실제 화면 체감 중복을 줄이기 위한 병합 규칙 보강이다.

## 수정 파일

- `app/src/ocr/det.rs` — BGR 정규화 수정, 외곽선 추적, 회전 사각형, 폴리곤 점수
- `app/src/ocr/cls.rs` — BGR 정규화 수정, 채널 순서 테스트 추가
- `app/src/ocr/mod.rs` — 중국어 이미지 비교 테스트 추가, 혼합 crop(수평 axis-aligned / 기울기 homography warp) 적용
- `app/src/services.rs` — 화면 OCR 선행 축소 제거, 적응형 타일 OCR(2x2/3x3) + 중복 제거 강화
- `app/src/config.rs` — 기본 OCR score threshold를 0.5로 조정, 기본값 테스트 추가
- `test.jpg` — 중국어 이미지 비교용 테스트 입력
- `scripts/compare_onnx.py` — 신규, Python 참조 비교 스크립트
