# 현재 OCR 기준 성능 최적화

## 목표

- sidecar 비교를 기준으로 삼지 않고, 현재 FFI OCR의 인식 결과와 속도를 기준선으로 둔다.
- 인식률에 영향을 줄 수 있는 모델 입력 분포, threshold, resize 정책은 바꾸지 않는다.
- 외부 라이브러리를 추가하거나 교체하지 않고 우리 코드의 고정비를 줄인다.

## 구현 계획

1. det 입력/출력 tensor 경로의 반복 allocation을 줄인다.
   - det 입력 tensor를 `std::vector<float>`로 매번 새로 만들지 않고 scratch buffer에 채운다.
   - predictor 출력도 scratch buffer를 통해 받아 후처리에 넘긴다.
2. rec batch 전처리와 결과 컨테이너의 반복 allocation을 줄인다.
   - batch별 `prepared_inputs`, `prepared_widths`, `shape`, `results`를 scratch에 재사용한다.
   - rec 입력 pixel/tensor 값과 batch shape 결정은 기존과 동일하게 유지한다.
3. cls batch 입력/출력 scratch를 엔진 단위로 재사용한다.
   - batch 크기와 tensor shape 정책은 유지한다.
   - OCR 호출마다 새로 잡던 cls tensor/output/batch image 버퍼를 재사용한다.
4. crop 경로의 낮은 위험 복사/할당 비용을 줄인다.
   - OpenCV crop에서 4개 점을 담기 위한 동적 vector 할당을 고정 크기 버퍼로 대체한다.
   - `warpPerspective`, 보간 방식, border 처리, 회전 조건은 유지한다.

## 검증 계획

- 현재 결과 기준 검증:
  - FFI 자체 benchmark 또는 release smoke를 사용한다.
  - sidecar compare는 사용하지 않는다.
- 단위/빌드 검증:
  - `cargo test --lib`
  - 가능한 환경이면 `cargo test --release --features paddle-ffi`
- 성능 확인:
  - `tools/scripts/ocr_sidecar_ffi.py benchmark-ffi` 또는 기존 FFI bench test를 사용해 median을 비교한다.

## 주의 사항

- rec batch width budget 기본값 변경은 과거 exact text mismatch가 있었으므로 이번 범위에서 제외한다.
- crop 보간 방식 변경, det resize 변경, 전처리 색공간 변경은 인식률 회귀 위험이 커서 제외한다.

## 구현 내용

- det 전처리 경로에 `preprocess_det_into_buffer()`와 `fill_det_tensor()`를 추가했다.
  - 기존 `std::vector<float>` 반환 경로는 유지하되, pipeline 경로는 scratch buffer에 직접 tensor를 채운다.
  - det predictor 출력도 scratch buffer로 받아 후처리한다.
- det 후처리 helper에 pointer 기반 overload를 추가했다.
  - `ensure_probability_map`, `log_det_map_stats`, `db_postprocess`, `score_box`가 `std::vector<float>` 없이도 동작한다.
  - 기존 vector 기반 API는 wrapper로 남겨 호출부 호환성을 유지했다.
- `buzhi_ocr_engine`에 det/rec scratch를 보관하게 했다.
  - warmup과 실제 pipeline이 같은 scratch capacity를 재사용한다.
  - Rust 쪽 FFI 엔진 락이 OCR 호출을 직렬화하므로 native scratch 공유는 현재 실행 모델과 맞다.
- rec batch 경로에서 `prepared_inputs`, `prepared_widths`, input shape vector를 scratch에 보관해 batch 반복 allocation을 줄였다.
  - rec batch width, resize 방식, padding 값, tensor fill 방식은 유지했다.
- cls batch 경로에서 input/output scratch, input shape, batch image pointer buffer를 엔진에 보관해 OCR 호출 간 재사용한다.
  - cls batch size, resize 방식, tensor fill 방식은 유지했다.
- OpenCV crop 경로에서 4개 crop 점을 `std::vector<cv::Point2f>` 대신 고정 크기 `std::array<cv::Point2f, 4>`로 구성했다.
  - `warpPerspective`, `INTER_CUBIC`, `BORDER_REPLICATE`, 세로 crop 회전 조건은 그대로 유지했다.

## 검증 결과

### 단위 테스트

```bash
cargo test --lib
```

- 100 passed.

### FFI 단독 benchmark

```bash
python3 tools/scripts/ocr_sidecar_ffi.py verify-ffi \
  --image testdata/ocr/test.png \
  --image testdata/ocr/test2.png \
  --image testdata/ocr/test3.png \
  --image testdata/ocr/test4.png \
  --source ch \
  --score-thresh 0.1 \
  --warmups 1 \
  --iterations 3 \
  --ffi-mode pipeline \
  --pipeline-resize-mode long-side \
  --cargo-profile release
```

변경 전 코드를 같은 명령으로 측정한 뒤, 이번 변경 적용 상태를 다시 측정했다.

| 이미지 | detection count | 변경 전 median ms | 변경 후 median ms | 개선 |
| --- | ---: | ---: | ---: | ---: |
| `test.png` | 7 | 2527.948 | 2498.993 | 1.1% |
| `test2.png` | 11 | 3232.932 | 2984.881 | 7.7% |
| `test3.png` | 23 | 3853.910 | 3414.309 | 11.4% |
| `test4.png` | 45 | 5727.006 | 4979.776 | 13.0% |

sidecar 비교는 수행하지 않았다. 이번 검증은 현재 FFI 자체의 detection count와 지연시간만 확인했다.

### 개선 후 새 baseline

위 개선 상태를 새 기준으로 잡고 남은 병목을 다시 확인했다.

```bash
python3 tools/scripts/ocr_sidecar_ffi.py profile-ffi \
  --image testdata/ocr/test.png \
  --image testdata/ocr/test2.png \
  --image testdata/ocr/test3.png \
  --image testdata/ocr/test4.png \
  --source ch \
  --score-thresh 0.1 \
  --warmups 1 \
  --iterations 1 \
  --ffi-mode pipeline \
  --pipeline-resize-mode long-side \
  --cargo-profile release
```

프로파일상 남은 시간은 대부분 `det_ms`와 `rec_ms` predictor 실행 시간이었다. `crop_ms`, `rotate_ms`, 후처리 고정비는 전체 지연시간 대비 작아 추가 개선 여지가 제한적이었다.

### 제외한 후보

`REC_BATCH_WIDTH_BUDGET`은 batch 폭을 줄여 일부 케이스에서 빨라질 수 있지만 exact text가 깨져 제외했다.

| 후보 | 결과 |
| --- | --- |
| `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET=4096` | `test.png`에서 `听` -> `小听`, `test2.png`에서 여러 text mismatch |
| `BUZHIDAO_PADDLE_FFI_REC_BATCH_WIDTH_BUDGET=8192` | `test.png`에서 `听` -> `小听` |

rec batch size 변경도 후보로 검토했지만 제외했다.

| 후보 | 결과 |
| --- | --- |
| batch size 4 | `test.png`에서 `听` -> `小听`, `test2.png`에서 `中國語 []+` 후미 기호 손실 |
| batch size 8 | `test2.png`에서 `召开中文词语处理机。` -> `召开印中文词语处理机。`, `中國語 []+` 후미 기호 손실 |

rec predictor는 batch shape 변화에 따라 수치가 미세하게 달라지고 CTC decode 결과까지 바뀔 수 있으므로, 기본 batch 정책은 유지한다.

### cls scratch 추가 후 FFI 단독 benchmark

```bash
python3 tools/scripts/ocr_sidecar_ffi.py verify-ffi \
  --image testdata/ocr/test.png \
  --image testdata/ocr/test2.png \
  --image testdata/ocr/test3.png \
  --image testdata/ocr/test4.png \
  --source ch \
  --score-thresh 0.1 \
  --warmups 1 \
  --iterations 3 \
  --ffi-mode pipeline \
  --pipeline-resize-mode long-side \
  --cargo-profile release
```

| 이미지 | detection count | 이번 수정 전 median ms | cls scratch 후 median ms | 누적 개선 |
| --- | ---: | ---: | ---: | ---: |
| `test.png` | 7 | 2527.948 | 2464.488 | 2.5% |
| `test2.png` | 11 | 3232.932 | 2923.257 | 9.6% |
| `test3.png` | 23 | 3853.910 | 3434.871 | 10.9% |
| `test4.png` | 45 | 5727.006 | 4938.241 | 13.8% |

첫 개선 상태와 비교하면 cls scratch의 추가 효과는 작고 일부 샘플은 측정 노이즈 범위다. 다만 기본 인식 경로와 shape를 유지하면서 호출 간 allocation을 줄이는 변경이라 유지한다.
