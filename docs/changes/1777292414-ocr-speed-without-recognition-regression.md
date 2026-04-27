# 인식률 하락 없는 OCR 속도 개선 계획

## 목표

- sidecar와 맞춘 현재 FFI OCR 인식률 parity를 유지한 채 속도를 더 줄인다.
- 검출 수, 매칭 수, exact text match가 달라지는 최적화는 채택하지 않는다.
- benchmark는 병렬 실행하지 않고 순차 실행해 비교한다.

## 기준선

- 직전 parity 작업 기준으로 `test.png`, `test2.png`, `test3.png`, `test4.png`는 sidecar와 FFI의 검출 수 및 텍스트가 100% 일치한다.
- 현재 FFI는 같은 네 이미지에서 sidecar보다 median 기준 빠르다.
- 다음 최적화는 sidecar parity뿐 아니라 현재 FFI 결과와의 parity도 같이 확인한다.

## 원칙

1. 인식률에 영향을 주는 모델 입력 분포를 바꾸지 않는다.
2. det resize, threshold, contour 단순화, 색공간 변환 같은 결과 변경 가능성이 큰 최적화는 후순위로 둔다.
3. 먼저 allocation, 복사, debug 비활성 경로 비용처럼 결과에 영향을 주지 않는 영역부터 줄인다.
4. 각 단계는 작은 변경 단위로 적용하고, 변경마다 `compare`를 통과해야 다음 단계로 넘어간다.
5. 속도 평가는 같은 이미지와 같은 옵션으로 순차 benchmark를 반복해 median 중심으로 본다.

## 1단계: stage별 병목 계측 강화

- `det preprocess`
- `det inference`
- `db postprocess`
- `crop`
- `cls preprocess/batch inference`
- `rec preprocess/batch inference`
- `decode`

이미 있는 profile 로그를 활용하되, 빠진 구간이 있으면 최소한으로 보강한다.
계측 자체가 일반 실행 경로를 느리게 만들지 않도록 profile 활성 시에만 문자열 조립과 로그 출력을 수행한다.

## 2단계: 낮은 위험 최적화

- rec batch 입력 버퍼 재사용
  - batch마다 `std::vector<float>`를 새로 할당하지 않고 필요한 최대 크기만 확보해 재사용한다.
  - tensor 값은 매번 명시적으로 채워 이전 batch 데이터가 섞이지 않게 한다.
- cls batch 입력 버퍼 재사용
  - cls는 고정 입력 크기라 재사용 효과가 안정적이다.
- 반복 벡터 reserve 정리
  - `prepared_inputs`, `rec_order`, `rec_batches`, 결과 벡터의 capacity를 입력 수 기준으로 미리 잡는다.
- debug 비활성 경로 비용 제거
  - debug/profile/dump가 꺼져 있을 때 문자열 생성, JSON 조립, 파일명 조립이 실행되지 않는지 확인한다.

이 단계는 픽셀, 좌표, 모델 입력 값을 바꾸지 않아야 한다.

## 3단계: 중간 위험 최적화

- crop 변환 복사 감소
  - 현재 crop 경로에서 `Image`와 `cv::Mat` 변환이 반복되는지 확인한다.
  - 가능하면 원본 `cv::Mat`에서 crop 결과를 만들고, 필요한 시점에만 `Image`로 변환한다.
- rec preprocess 복사 감소
  - crop 결과를 resize한 뒤 다시 tensor로 복사하는 흐름에서 불필요한 중간 버퍼를 줄일 수 있는지 확인한다.
  - 단, padding 값과 채널 순서가 byte-level로 유지되어야 한다.

이 단계는 crop dump 또는 rec input tensor 비교로 parity를 먼저 확인한다.

## 4단계: 높은 위험 후보

아래 항목은 속도 이득 가능성은 있으나 인식률 하락 위험이 크므로 기본 계획에서는 보류한다.

- det resize 크기 축소
- box threshold, unclip ratio, score mode 변경
- contour 단순화 조건 강화
- 색공간 변경, grayscale, sharpen 등 전처리 변경
- crop 보간 방식 변경

필요하면 별도 실험 문서와 더 넓은 이미지 세트로만 검토한다.

## 검증 기준

### parity

```powershell
python tools\scripts\ocr_sidecar_ffi.py compare --image testdata\ocr\test.png --image testdata\ocr\test2.png --image testdata\ocr\test3.png --image testdata\ocr\test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline --pipeline-resize-mode long-side --cargo-profile release
```

- 모든 이미지에서 sidecar count와 FFI count가 같아야 한다.
- 모든 이미지에서 matched pairs와 exact text matches가 같아야 한다.
- mismatch, sidecar only, FFI only가 없어야 한다.

### speed

```powershell
python tools\scripts\ocr_sidecar_ffi.py benchmark --image testdata\ocr\test.png --image testdata\ocr\test2.png --image testdata\ocr\test3.png --image testdata\ocr\test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline --pipeline-resize-mode long-side --cargo-profile release
```

- 순차 실행 결과만 사용한다.
- median 기준 현재 FFI보다 개선되는지 확인한다.
- 평균만 좋아지고 median이 나빠지는 변경은 재검토한다.

## 구현 순서 제안

1. profile stage 누락 구간을 확인하고 필요한 로그만 보강한다.
2. 현재 FFI 기준 stage별 시간을 기록한다.
3. rec batch 입력 버퍼 재사용을 적용한다.
4. cls batch 입력 버퍼 재사용을 적용한다.
5. debug 비활성 경로 비용을 제거한다.
6. 각 단계마다 `compare`와 `benchmark`를 실행해 문서에 결과를 누적한다.
7. 낮은 위험 변경으로 이득이 부족할 때만 crop/rec preprocess 복사 감소를 별도 단계로 진행한다.

## 구현 내용

- cls batch 입력과 출력에 scratch buffer를 추가해 batch마다 float vector를 새로 할당하지 않도록 했다.
- cls batch 크기가 1인 경우도 같은 batch 경로를 타게 해 pipeline 내 처리 경로를 단순화했다.
- pipeline의 cls/rec batch 루프에서 `std::vector<const Image*>`와 rec debug meta vector를 반복 생성하지 않고 재사용했다.
- CTC decode에서 `debug_enabled()`와 CTC dump 환경변수 확인을 time step마다 수행하지 않도록 한 번만 계산했다.
- rec batch 크기가 1인 경우도 단일 rec 경로로 우회하지 않고 scratch buffer를 쓰는 batch 경로를 타게 했다.

## 제외한 실험

- `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET=2400`으로 rec batch padding 폭을 제한하는 실험은 속도 개선 가능성이 있지만 텍스트가 달라져 제외했다.
  - `test.png`: `听`이 `小听`으로 변경
  - `test2.png`, `test3.png`, `test4.png`에서도 일부 exact text mismatch 발생
- rec batch shape 변경은 입력 픽셀 자체를 바꾸지 않아도 모델 출력 time step과 문맥이 달라질 수 있으므로 기본값 변경 대상에서 제외한다.
- `resize_bilinear`에서 BGRA 입력을 BGR 변환 없이 4채널 그대로 resize하는 실험은 compare parity는 유지했지만 benchmark에서 `test.png`가 sidecar보다 느려져 제외했다.
  - 작은 이미지에서는 4채널 resize 비용이 색상 변환 제거 이득보다 커질 수 있다.
  - 큰 이미지 일부에는 이득이 있어 보였지만 전체 gate를 만족하지 못해 되돌렸다.

## 검증 결과

### compare

- `test.png`: 7/7 exact
- `test2.png`: 11/11 exact
- `test3.png`: 23/23 exact
- `test4.png`: 45/45 exact

### benchmark

| 이미지 | sidecar median ms | FFI median ms | delta ms | count |
| --- | ---: | ---: | ---: | ---: |
| `test.png` | 2495.576 | 2385.765 | -109.811 | 7 |
| `test2.png` | 3207.849 | 3080.440 | -127.410 | 11 |
| `test3.png` | 3599.581 | 3411.885 | -187.696 | 23 |
| `test4.png` | 5631.860 | 5335.374 | -296.485 | 45 |

### cargo test

```powershell
cargo test --release --features paddle-ffi
```

- 97 passed.
