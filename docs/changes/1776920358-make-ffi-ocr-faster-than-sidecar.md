# FFI OCR 속도를 sidecar 이상으로 끌어올리기

## 구현 계획

1. sidecar와 FFI의 호출 구조 차이를 코드 레벨에서 식별한다.
2. 속도에 직접 영향을 주는 구조 차이부터 제거한다.
3. 벤치와 관련 테스트로 회귀 없이 개선 여부를 확인한다.

## 초기 코드 비교 결과

### 1. FFI는 추론마다 predictor를 clone하고 있었다

- 파일: `native/paddle_bridge/bridge.cc`
- 함수: `run_pipeline()`
- 기존 동작:
  - `engine->det_predictor->Clone()`
  - `engine->rec_predictor->Clone()`
- sidecar는 서버 시작 시 생성한 predictor를 계속 재사용한다.

즉 FFI는 sidecar에 없는 predictor clone 비용을 매 OCR 호출마다 추가로 내고 있었다.

### 2. FFI Rust 래퍼는 원본 이미지를 매번 BMP로 재인코딩하고 있었다

- 파일: `src/ocr/paddle_ffi.rs`
- 함수: `PreparedOcrImage::new()`
- 기존 동작:
  - PNG/JPG 입력을 `image` 크레이트로 다시 읽음
  - 임시 BMP로 저장
  - C++ bridge는 그 BMP를 다시 읽음

반면 sidecar는 원본 파일 경로를 그대로 OpenCV/Paddle 경로에 넘긴다.

즉 FFI는 sidecar에 없는 이미지 재디코드 + BMP 재인코드 + 재디코드 비용을 매 호출마다 추가로 내고 있었다.

## 구현

### 1. predictor clone 제거

- `native/paddle_bridge/bridge.cc`
  - `clone_predictor()` 경로 제거
  - `run_pipeline()`가 `engine->det_predictor` / `engine->rec_predictor`를 직접 사용하도록 변경
  - `buzhi_ocr_engine`에 `run_mutex`를 추가해 같은 엔진의 동시 실행은 직렬화

이 변경으로 sidecar와 같은 predictor 재사용 구조로 맞춘다.

### 2. 원본 이미지 경로 직접 사용

- `src/ocr/paddle_ffi.rs`
  - `PreparedOcrImage` 제거
  - `run_image_file()`가 입력 이미지 경로를 그대로 FFI에 전달

이 변경으로 sidecar와 같은 이미지 로드 구조로 맞춘다.

### 3. 벤치 프로파일을 sidecar와 같은 최적화 기준으로 맞춤

- `scripts/ocr_sidecar_ffi.py`
  - FFI benchmark를 `cargo test --release`로 실행하도록 변경

sidecar는 최적화된 Python wheel / native binary를 사용한다.
FFI만 `cargo test` 기본 test profile(debug)로 측정하면 비교 기준이 다르다.
속도 비교는 FFI도 최적화 프로파일에서 재는 것이 맞다.

### 4. 일반 실행에서 불필요한 debug 결과 직렬화 제거

- `native/paddle_bridge/bridge.cc`
  - `debug_trace=false`일 때 accepted detection을 `debug_detections`로 다시 누적하지 않도록 변경

일반 OCR 실행과 벤치에서는 debug trace가 꺼져 있다.
이 경우 sidecar parity 확인을 위해 넣었던 debug payload를 다시 만들 이유가 없으므로 제거한다.

### 5. batch 구성 시 crop 이미지 복사 제거

- `native/paddle_bridge/bridge.cc`
  - `run_cls_batch()` / `run_rec_batch()`가 `Image` 복사본 대신 `const Image*`를 받도록 변경
  - `run_pipeline()`에서 classifier/recognizer 배치 구성 시 crop 이미지를 다시 복사하지 않고 기존 crop 객체를 직접 참조하도록 변경

sidecar는 crop numpy array를 그대로 batch에 넣는다.
FFI가 batch 구성 때마다 crop bitmap을 다시 복사하는 것은 sidecar에 없는 추가 비용이었다.

### 6. dump/trace가 꺼져 있을 때 recognizer debug meta 생성 제거

- `native/paddle_bridge/bridge.cc`
  - `describe_crop_to_bbox()`와 `batch_meta` 구성을 `debug_trace` 또는 `BUZHIDAO_PADDLE_FFI_DUMP_REC_LOGITS`가 켜진 경우에만 수행하도록 변경

평상시 실행에서는 recognizer dump를 만들지 않으므로, 관련 메타데이터도 만들지 않는 것이 sidecar와 더 가깝다.

## 보류한 시도

### OpenCV 경로의 내부 3채널화

- `native/paddle_bridge/bridge.cc`

OpenCV 경로에서 `BGRA 4채널` 중간 표현을 줄이기 위해 내부 `BGR 3채널` 유지 패치를 시도했다.
하지만 `test2.png` 기준으로 sidecar `14` / FFI `11`로 parity가 깨졌다.

이 시도는 현재 유지하지 않는다.
원칙은 `코드 레벨 차이를 없애는 것`이므로, parity가 깨지는 3채널 최적화는 되돌리고 이후 더 좁은 단계로 다시 시도한다.

## 테스트 및 벤치

- Rust 테스트:
  - `cargo test -p buzhidao 원본_경로를_그대로_사용한다 -- --nocapture`
  - 결과: 통과

- parity 확인:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test2.png --source ch --score-thresh 0.1`
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test3.png --source ch --score-thresh 0.1`
  - 결과: sidecar/FFI detection 수와 텍스트 모두 일치

- 벤치:
  - `python scripts/ocr_sidecar_ffi.py benchmark --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 최종 확인 결과:
    - `test.png`
      - sidecar mean `4626.06ms`
      - FFI mean `3886.67ms`
    - `test2.png`
      - sidecar mean `6379.86ms`
      - FFI mean `4945.53ms`
    - `test3.png`
      - sidecar mean `8265.22ms`
      - FFI mean `8086.94ms`

현재 샘플 기준으로 FFI는 sidecar보다 최소 같거나 더 빠르다.

## test4 추가 후 재검증

- 정합성:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과: sidecar `32`, FFI `32`, exact text match `32`
- 성능:
  - `python scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 최초 결과:
    - sidecar mean `16469.31ms`
    - FFI mean `16993.71ms`
    - delta `+524.39ms`

즉 `test4`에서는 parity는 유지되지만 FFI가 다시 느려졌다.

## test4 기준 추가 코드 비교

### 7. FFI는 crop마다 원본 전체를 다시 BGRA→BGR로 변환하고 있었다

- 파일: `native/paddle_bridge/bridge.cc`
- 함수: `crop_to_bbox()`, `run_pipeline()`
- 기존 동작:
  - `run_pipeline()`는 원본 이미지를 `Image(BGRA)`로 들고 있음
  - 각 detection polygon마다 `crop_to_bbox(img, ...)` 호출
  - `crop_to_bbox()` 내부에서 매번 `image_to_cv_mat_bgr(img)` 실행
  - 즉 crop 수만큼 원본 전체 이미지를 반복 변환

반면 sidecar는 OpenCV가 읽은 원본 `BGR ndarray`를 그대로 들고 있고,
각 polygon crop은 그 동일한 원본 버퍼를 기준으로 `warpPerspective()`를 수행한다.

`test4`처럼 detection 수가 많은 샘플에서는 이 차이가 그대로 누적된다.

### 8. 원본 BGR Mat 재사용으로 crop 경로를 sidecar와 맞춤

- 파일: `native/paddle_bridge/bridge.cc`
- 변경:
  - OpenCV 경로용 `crop_to_bbox(const cv::Mat& img_bgr, ...)` overload 추가
  - `run_pipeline()` 시작 시 원본 `Image`를 BGR `cv::Mat`으로 한 번만 변환
  - crop loop에서는 그 동일한 `cv::Mat`을 재사용

이 변경은 단순 최적화가 아니라 sidecar와 같은 원본 이미지 사용 구조로 맞추는 작업이다.

### 9. dump가 꺼져 있을 때 crop 중간 bitmap 변환 제거

- 파일: `native/paddle_bridge/bridge.cc`
- 함수: `crop_to_bbox(const cv::Mat& img_bgr, ...)`
- 기존 동작:
  - `crop_warp` dump가 꺼져 있어도 `cv_mat_to_image_bgra(cropped)`를 먼저 수행
  - 이후 최종 반환용으로 다시 한 번 `cv_mat_to_image_bgra(cropped)` 수행

즉 일반 벤치에서도 crop 하나당 BGRA 변환이 두 번 일어났다.
sidecar는 dump를 켜지 않으면 이런 중간 bitmap 직렬화를 만들지 않는다.

- 변경:
  - dump 디렉터리가 있을 때만 `crop_warp`용 BGRA 변환 수행
  - 일반 실행에서는 최종 반환용 변환만 남김

## test4 재검증 결과

- parity 재확인:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과: sidecar `32`, FFI `32`, exact text match `32`

- 최종 benchmark:
  - `python scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 결과:
    - sidecar mean `21078.79ms`
    - FFI mean `16799.24ms`
    - mean delta `-4279.55ms`
    - sidecar median `19919.82ms`
    - FFI median `16786.10ms`
    - median delta `-3133.72ms`

현재 `test4` 기준으로도
- 결과 동일
- FFI가 sidecar보다 더 빠름
을 만족한다.

## test4 추가 재검증

- parity 재확인:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과: sidecar `32`, FFI `32`, exact text match `32`

- benchmark 재확인:
  - `python scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 결과:
    - sidecar mean `19817.18ms`
    - FFI mean `16321.59ms`
    - mean delta `-3495.59ms`
    - sidecar median `19273.96ms`
    - FFI median `16430.87ms`
    - median delta `-2843.09ms`

재검증에서도 `test4` 기준으로 결과 동일성과 속도 우위가 유지됐다.
