# GPU OCR 진단 로그 및 성능 개선: cls 배치 처리 + intra_threads 설정

## 배경

`--features gpu`로 빌드해도 GPU가 사용되지 않는 것처럼 보이는 문제가 보고됐다.
진단 로그를 추가해 확인한 결과 CUDA EP는 정상 등록(`is_available() = true`)되고 있었으나
OCR 속도가 여전히 느렸다.

## CUDA 진단 로그 추가

`fail_silently()`로 CUDA EP 초기화 실패가 조용히 CPU 폴백되기 때문에
실제 동작 여부를 알 수 없었다.

`configure_execution_providers`에서 `ep::CUDA::default().is_available()`로
CUDA 가용 여부를 확인한 후 결과를 `eprintln!`으로 출력하도록 수정했다.

- 사용 가능: `[OCR] det: CUDA EP 사용`
- 사용 불가: `[OCR] det: CUDA EP 사용 불가 — CPU로 폴백 (CUDA 런타임 설치 및 PATH 등록 여부를 확인하세요)`

## 원인 분석

1. **cls/rec 개별 호출 오버헤드**
   검출된 텍스트 박스 수만큼 cls → rec를 순차적으로 GPU에 호출한다.
   GPU는 소규모 입력에 대한 커널 실행 + 메모리 전송 오버헤드가 크기 때문에,
   박스가 많을수록 오버헤드가 선형으로 누적된다.

2. **CPU 스레드와 CUDA 스트림 경합**
   기본 `intra_op_num_threads` 값은 코어 수에 맞춰 여러 스레드를 사용한다.
   GPU 빌드에서 CPU op 스레드가 많으면 CUDA 스트림과 자원을 두고 경합해 성능이 저하된다.

## 변경 내용

### 1. `src/ocr/cls.rs` — `classify_batch` 추가

모든 크롭을 `[N, 3, H, W]` 배치 텐서로 묶어 GPU에 한 번에 전달한다.
ONNX 모델이 동적 배치를 지원하지 않는 경우 순차 처리로 자동 폴백한다.

```
classify_batch(N 박스) → try_batch_classify (실패 시) → classify × N
```

`try_batch_classify`를 별도 함수로 분리한 이유:
`SessionOutputs<'_>`가 session을 빌리기 때문에, 같은 스코프에서 폴백 시
session을 재사용하면 컴파일 에러가 발생한다.
별도 함수로 분리하면 함수 반환 시점에 borrow가 즉시 해제된다.

### 2. `src/ocr/mod.rs` — `recognize_boxes` 배치 처리 적용

```
before: for each box → cls(1) → rec(1)          (N × 2 GPU 호출)
after:  cls_batch([N]) → for each box → rec(1)  (1 + N GPU 호출)
```

### 3. `src/ocr/mod.rs` — gpu 빌드에서 `with_intra_threads(1)` 설정

CUDA EP 사용 시 인트라 op CPU 스레드를 1로 제한해 CUDA 스트림과의 경합을 방지한다.

## 테스트 결과

- `cargo test` — 43개 통과
- `cargo test --features gpu` — 44개 통과
