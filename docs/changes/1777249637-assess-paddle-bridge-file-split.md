# paddle bridge 파일 분리 검토

## 구현 계획

1. `native/paddle_bridge/bridge.cc`의 파일 크기와 책임 범위를 확인한다.
2. 단일 파일 유지 가능 여부와 분리 시점을 판단한다.
3. 리팩터링을 한다면 어떤 경계로 쪼개는 것이 안전한지 정리한다.

## 확인 내용

- `native/paddle_bridge/bridge.cc`는 현재 7,487줄이다.
- `native/paddle_bridge` 아래에는 `bridge.cc`와 `bridge.h`만 있다.
- 파일 안에는 다음 책임이 함께 들어 있다.
  - C ABI 엔트리포인트와 메모리 해제 API
  - Paddle predictor 생성, warmup, 실행
  - det/cls/rec 전처리와 후처리
  - 이미지 로딩, BMP/GDI+/OpenCV 변환
  - geometry, crop, rotate, resize, sampling
  - 모델 디렉터리 탐색과 설정 파싱
  - recognition dict 로딩과 검증
  - debug/profile dump, JSON 직렬화

## 판단

단일 파일로 계속 유지하는 것은 단기적으로는 가능하지만, 장기적으로는 권장하지 않는다.

현재 파일은 단순히 긴 파일이 아니라 서로 다른 변경 이유를 가진 코드가 한 번에 묶여 있다. OCR parity, 성능 최적화, 모델 탐색, 이미지 I/O, C ABI 안정성 변경이 모두 같은 파일 diff에 섞이기 때문에 리뷰 비용과 회귀 위험이 계속 커진다.

다만 즉시 대규모 분리하는 것도 위험하다. `bridge.cc`는 sidecar parity와 성능 튜닝 이력이 많고, 전처리/후처리 세부 동작이 OCR 결과에 직접 영향을 준다. 따라서 기능 변경과 구조 변경을 섞지 않고, 먼저 순수 이동 위주의 작은 단계로 나누는 편이 안전하다.

## 권장 분리 순서

1. C ABI 표면 유지
   - `bridge.h`와 `extern "C" buzhi_ocr_*` 함수는 최대한 그대로 둔다.
   - Rust FFI 계약이 흔들리지 않게 한다.

2. 저위험 유틸부터 분리
   - 문자열/환경변수/JSON 파싱
   - 파일 존재 확인과 디렉터리 탐색
   - debug/profile 로그

3. 이미지 I/O와 이미지 primitive 분리
   - `Image`, `PixelLayout`
   - BMP/GDI+ 로더
   - OpenCV 변환
   - resize/rotate/sampling

4. geometry와 DB postprocess 분리
   - `FloatPoint`, `BBox`, `MinAreaRectBox`
   - hull, min area rect, unclip, score box, connected component 처리

5. OCR stage 단위 분리
   - det preprocess/postprocess
   - cls preprocess/run
   - rec preprocess/batch/decode

6. 마지막에 pipeline 조립부 정리
   - `run_pipeline()`은 stage 호출과 결과 조립만 담당하게 만든다.

## 주의점

- 전처리 로직 이동은 OCR 결과 parity를 먼저 확인해야 한다.
- `warpAffine`/`warpPerspective` 계열 최적화나 crop/rotate 변경은 단순 이동처럼 보여도 결과가 달라질 수 있다.
- 빌드 시스템은 현재 `build.rs`가 `native/paddle_bridge/bridge.cc` 단일 파일만 컴파일한다. 파일을 나누면 `build.rs`의 cc build 입력도 함께 갱신해야 한다.
- Windows 전용 GDI+ 코드와 OpenCV 조건부 컴파일은 분리 파일에서도 같은 feature/define 조건을 유지해야 한다.

## 테스트

이번 작업은 코드 변경 없이 구조 판단 문서만 추가했으므로 실행 테스트는 수행하지 않았다.

## 현재 상태

- 46차까지 진행했다.
- `native/paddle_bridge/bridge.cc`는 빌드 입력 호환을 위한 1줄 include 파일로 축소됐다.
- C ABI facade 구현은 `native/paddle_bridge/bridge_api.cc`로 이동했다.
- `build.rs`의 Paddle bridge 파일 목록은 디렉터리 스캔 기반으로 자동 등록한다.
- `bridge_pipeline.h`, `bridge_warmup.h`의 엔진 정의 의존은 구현 파일 쪽으로 낮췄다.
- 회차별 상세 로그는 별도 문서에 연대순으로 분리했다.
- 각 채택 차수는 직전 통과 benchmark를 기준으로 detection count와 median latency guard를 확인했다.

## 최종 구조 요약

| 파일 | 책임 |
| --- | --- |
| `bridge.h` | 외부 C ABI 선언과 FFI 결과 타입 |
| `bridge.cc` | 기존 빌드 입력 호환용 include 파일 |
| `bridge_api.cc` | destroy, warmup, image/file 실행 C ABI facade |
| `bridge_create.cc` | OCR engine 생성과 모델/사전/predictor 초기화 |
| `bridge_engine.h` | engine 상태 구조체 |
| `bridge_pipeline.*` | det/cls/rec stage 호출 순서와 결과 조립 |
| `bridge_det.*`, `bridge_cls.*`, `bridge_rec.*` | OCR stage별 predictor 실행 wrapper |
| `bridge_tensor.*`, `bridge_image.*`, `bridge_crop.*`, `bridge_rotate.*`, `bridge_resample.*` | 전처리 이미지/텐서 변환 |
| `bridge_det_utils.*`, `bridge_geometry.*` | det 후처리와 geometry 계산 |
| `bridge_output.*` | FFI 결과 소유권, JSON 직렬화, native result 변환 |
| `bridge_model.*`, `bridge_config.*`, `bridge_dict.*`, `bridge_predictor_config.*` | 모델 탐색, 설정, 사전, predictor config |
| `bridge_debug_*`, `bridge_utils.*`, `bridge_fs.*` | debug dump/format, 공통 유틸, 파일 시스템 보조 |

## 최종 검증 기준

- 구조 변경 후에는 `cargo test --features paddle-ffi --no-run`을 통과해야 한다.
- OCR fixture 3개는 detection count `7/14/27`을 유지해야 한다.
- FFI benchmark는 직전 통과 결과를 기준으로 `ffi_benchmark_guard.py --max-median-ratio 1.15`를 통과해야 한다.
- benchmark guard 실패가 노이즈로 보이면 1회 재측정하고, 반복 실패하면 해당 변경을 보류하거나 되돌린다.
- 문서/코드에는 로컬 개발 환경 절대 경로를 남기지 않는다.

## 리팩터링 전후 속도 비교

- 기준: `target/ffi-bench-logs/14-before.txt`
- 현재: `target/ffi-bench-logs/45-after-header-boundary-cleanup.txt`
- guard: `--max-median-ratio 1.15` 통과

| fixture | 리팩터링 전 median | 리팩터링 후 median | ratio | detection |
| --- | ---: | ---: | ---: | --- |
| `test.png` | 2454.960 ms | 2402.586 ms | 0.979 | 7 -> 7 |
| `test2.png` | 3195.016 ms | 3225.881 ms | 1.010 | 14 -> 14 |
| `test3.png` | 5638.513 ms | 6205.697 ms | 1.101 | 27 -> 27 |

세 fixture 모두 허용 범위 안이며, 이번 리팩터링 전후 속도 차이는 노이즈 범위로 판단한다.

## 진행 로그

- 회차별 상세 기록은 `1777249637-assess-paddle-bridge-file-split-rounds.md`에 연대순으로 분리했다.
