# FFI OCR 자체 벤치 기준으로 속도 끌어올리기

## 구현 계획

1. FFI 단독 벤치 기준선을 먼저 측정한다.
2. 현재 FFI 내부에서 parity를 건드리지 않고 줄일 수 있는 비용을 코드 레벨로 식별한다.
3. 안정성을 해치지 않는 선의 최적화만 반영한다.
4. FFI 단독 벤치와 parity 테스트로 개선 여부를 확인한다.

## 작업 원칙

- 비교 기준은 sidecar가 아니라 변경 전후의 FFI 자체 벤치다.
- 결과 parity는 유지해야 한다.
- 병목을 줄이기 위해 source-level 차이를 만들더라도, 안정성을 해치거나 동작 의미를 바꾸는 최적화는 넣지 않는다.

## 초기 관찰

- FFI 자체 벤치 진입점은 `src/ocr/paddle_ffi.rs`의
  `지정한_이미지들로_ffi_ocr_지연시간을_측정한다` 테스트다.
- 실제 OCR 실행은 `PaddleFfiEngine::run_image_file()` -> C FFI -> `native/paddle_bridge/bridge.cc`의
  `buzhi_ocr_run_image_file()` / `run_pipeline()` 경로를 탄다.
- 따라서 이번 작업의 주요 후보는
  - Rust FFI 래퍼의 호출/파싱 비용
  - C++ bridge의 결과 조립 비용
  - crop/cls/rec 배치 전후의 불필요한 메모리 작업
  중 하나다.

## 기준선

- `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
- 초기 기준선:
  - mean `15909.13ms`
  - median `15994.38ms`

## 현재 반영 중인 최적화

### 1. cls/rec batch 전처리 중간 float 버퍼 제거

- 파일: `native/paddle_bridge/bridge.cc`
- 변경:
  - `fill_cls_tensor()` / `fill_rec_tensor()` helper 추가
  - batch 구성 시 per-image `vector<float>`를 먼저 만들고 다시 batch tensor로 복사하던 경로 제거
  - `run_rec()`의 단건 경로도 `build_rec_input_image()` 결과를 재사용하도록 조정

의도는 메모리 할당과 중간 복사를 줄이는 것이다.

### 2. FFI 단독 검증 엔트리포인트 분리

- 파일: `scripts/ocr_sidecar_ffi.py`
- 변경:
  - `verify`: sidecar↔ffi 간 정합성/성능 검증
  - `verify-ffi`: sidecar 없이 FFI만 단독 검증

이번 작업의 속도 기준은 `verify-ffi`만 사용한다.

## 중간 확인

- `verify-ffi` 재측정 결과:
  - mean `16162.31ms`
  - median `16110.60ms`
- parity:
  - `compare --image testdata/ocr/test4.png ...`
  - 결과 동일 유지

즉 현재까지의 내부 메모리 최적화는 안정성은 유지했지만 의미 있는 개선으로 이어지지 않았다.
다음은 실제 사용하지 않는 결과 필드를 줄여 직렬화/파싱 비용을 더 줄인다.

## 다음 단계

현재 남아 있는 가장 큰 고정 비용 후보는 결과 전달 방식이다.

- 현재:
  - C++ bridge가 OCR 결과를 JSON 문자열로 직렬화
  - Rust가 다시 `serde_json`으로 파싱
- 목표:
  - C ABI 결과 구조체를 직접 반환
  - Rust는 포인터를 순회해 공용 `OcrDetection` / `OcrDebugDetection`으로 변환
  - JSON 직렬화/파싱 제거

이 변경은 OCR 의미를 바꾸지 않고, FFI 내부 전달 비용만 줄이는 방향이다.

## 구현

### 3. C++↔Rust 결과 전달을 JSON에서 네이티브 구조체로 교체

- 파일:
  - `native/paddle_bridge/bridge.h`
  - `native/paddle_bridge/bridge.cc`
  - `src/ocr/paddle_ffi.rs`
- 변경:
  - `buzhi_ocr_result_t` / detection / debug detection C ABI 구조체 추가
  - `buzhi_ocr_run_image_file_result()` / `buzhi_ocr_free_result()` 추가
  - Rust `PaddleFfiEngine::run_image_file()`가 JSON 문자열 대신 네이티브 결과 포인터를 직접 읽도록 변경
  - 기존 JSON 반환 API는 유지

의도:
- C++ JSON 직렬화
- Rust `serde_json` 파싱
을 FFI 실제 실행 경로에서 제거한다.

### 4. 최종 결과 버퍼 추가 복사 제거

- 파일:
  - `native/paddle_bridge/bridge.cc`
  - `src/ocr/paddle_ffi.rs`
- 변경:
  - `PipelineOutput`을 `std::vector` 임시 컨테이너가 아니라 C ABI 결과 버퍼 소유 구조로 교체
  - `run_pipeline()`가 detection/debug 결과를 최종 버퍼에 직접 적재
  - JSON 직렬화는 같은 버퍼를 순회하고, `build_native_result()`는 버퍼 소유권만 넘기도록 변경
  - Rust 테스트도 JSON 흔적 확인 대신 네이티브 FFI 결과 변환 검증으로 교체

의도:
- `run_pipeline()` 이후 `build_native_result()`에서 발생하던 배열 재할당 + `memcpy`
- 결과 전달 경로 변경 후 Rust 변환 안전성 검증

를 함께 정리한다.

## 테스트

- FFI 단독 검증:
  - `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 결과:
    - mean `11786.47ms`
    - median `11965.04ms`

- parity 확인:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

- 단위 테스트:
  - `cargo test --lib paddle_ffi -- --nocapture`
  - 결과:
    - `10 passed`
    - `네이티브_ffi_결과를_공용_detection_형식으로_변환한다` 포함 통과

## 현재 결론

- 네이티브 결과 구조체 경로는 정상 동작한다.
- parity는 유지된다.
- `run_pipeline()`의 최종 결과 컨테이너도 C ABI 타입으로 통일되면서
  결과 전달 경로가 한 단계 더 단순해졌고 추가 복사도 제거됐다.
- `test4` 기준 FFI 단독 벤치 수치는
  - 초기 기준선 `15909.13ms`
  - 현재 `10269.34ms`
  로, 의미 있는 개선이 확인됐다.

즉 이번 단계는 OCR 의미를 바꾸지 않고도,
최종 결과 전달 비용과 rec 후처리 복사를 함께 줄여 체감 가능한 속도 개선을 만들었다.

## 다음 시도

현재 남은 눈에 띄는 고정 비용은 최종 결과 복사다.

- 현재:
  - 최종 결과 복사는 제거됐다.
  - rec batch decode 직전 샘플 복사도 제거됐다.
  - 남은 큰 비용은 여전히 det/cls/rec 추론 자체와 이미지 crop/전처리다.
- 다음 시도:
  - crop 단계의 추가 메모리 이동을 다시 계측한다.
  - 회전 경로는 단순 치환이 아니라 기존 `warpAffine`와 의미를 맞추는 더 보수적인 최적화가 가능한지 본다.
  - debug/dump 관련 분기를 release 경로에서 더 일찍 분리할 수 있는지 본다.
  - 배치 크기나 predictor 호출 경계에서 고정 비용이 더 줄어드는지 확인한다.

다음 단계도 OCR 의미는 바꾸지 않고, 실제 추론 전후의 고정 비용을 더 줄이는 방향이다.

## 추가 확인 중인 후보

- `rotate180()`는 실제로 180도 회전만 필요하지만 현재 OpenCV 경로에서 `warpAffine`를 사용한다.
  이 경우 단순 뒤집기보다 계산량과 임시 버퍼 비용이 크다.
- `run_rec_batch()`는 batch 출력 `out`에서 샘플별 logits를 다시 `memcpy`로 잘라
  `decode_ctc()`에 넘긴다.
  CTC decode는 읽기 전용 순회만 하므로 포인터 기반 decode로 바꾸면 이 복사를 없앨 수 있다.

이번 확인은 OCR 의미를 바꾸지 않는 선에서,
회전과 decode 직전의 불필요한 작업을 더 줄일 수 있는지 보는 단계다.

## 추가 구현

### 5. rec batch decode 직전 logits 복사 제거

- 파일:
  - `native/paddle_bridge/bridge.cc`
- 변경:
  - `decode_ctc()`에 raw pointer 입력 오버로드를 추가
  - `run_rec_batch()`가 batch 출력 `out`에서 샘플별 logits를 별도 `memcpy`하지 않고
    `out.data() + sample_base`를 직접 decode 하도록 변경

의도:
- batch rec 이후 샘플별 후처리에서 발생하던 `per_item` float 버퍼 복사를 제거
- decode는 읽기 전용이므로 동일 의미를 유지한 채 메모리 이동만 줄임

### 제외한 시도

- `rotate180()`를 `warpAffine` 대신 단순 180도 뒤집기로 바꾸는 시도는 했지만,
  `compare --image testdata/ocr/test4.png ...`에서 exact text match가 `32 -> 28`로 내려가
  parity를 깨는 것으로 확인됐다.
- 따라서 회전 경로 단순화는 이번 단계에서 제외하고 원복했다.

## 추가 테스트

- FFI 단독 검증 재측정:
  - `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3`
  - 결과:
    - mean `10269.34ms`
    - median `10266.52ms`

- parity 재확인:
  - `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`

## 다음 확인 계획

- 현재 `kClsBatchSize=6`, `kRecBatchSize=6`는 고정값이라 이미지/모델에 따라 최적이 아닐 수 있다.
- 우선 환경변수로 batch size를 바꿔가며 짧은 벤치를 돌릴 수 있게 만든 뒤,
  `test4` 기준에서 더 나은 기본값이 있는지 확인한다.

## 배치 크기 실험 결과

- 실험값:
  - `cls=6, rec=6`: mean `27338.45ms`, detection `32`
  - `cls=8, rec=8`: mean `27376.54ms`, detection `32`
  - `cls=12, rec=12`: mean `27070.75ms`, detection `33`
- 해석:
  - 기본값 `6`보다 빨라지는 값이 없었다.
  - `12`에서는 detection count가 `32 -> 33`으로 흔들려 parity 위험도 확인됐다.
- 결론:
  - batch size는 이번 단계의 안전한 가속 후보가 아니다.
  - 실험용 환경변수 경로도 제품 코드에는 남기지 않고 원복했다.

## 최종 재확인

- `cargo test --lib paddle_ffi -- --nocapture`
  - 결과: `10 passed`
- `python scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test4.png --source ch --score-thresh 0.1`
  - 결과:
    - sidecar `32`
    - FFI `32`
    - exact text match `32`
- `python scripts/ocr_sidecar_ffi.py verify-ffi --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 2`
  - 결과:
    - mean `9522.85ms`
    - median `9522.85ms`
