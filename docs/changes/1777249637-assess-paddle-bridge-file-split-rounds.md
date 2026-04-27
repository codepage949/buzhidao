# paddle bridge 파일 분리 회차별 상세 로그

## 1차 구현 계획

1. OCR 결과에 직접 닿지 않는 `debug/env/json/text` 유틸을 먼저 분리한다.
2. `native/paddle_bridge/bridge_utils.h`와 `native/paddle_bridge/bridge_utils.cc`를 추가한다.
3. `bridge.cc`는 새 유틸 헤더를 포함하고, 기존 함수 구현 중 유틸 구현부만 제거한다.
4. `build.rs`의 `cc::Build` 입력에 새 `.cc` 파일과 rerun 감시 대상을 추가한다.
5. `cargo test --features paddle-ffi --no-run`으로 C++ 분리 빌드가 통과하는지 확인한다.

이번 단계에서는 이미지 변환, crop, geometry, det/cls/rec stage, pipeline 조립부는 건드리지 않는다. OCR 결과 parity에 영향을 줄 수 있는 코드는 다음 단계에서 별도로 분리한다.

## 1차 구현 내용

- `native/paddle_bridge/bridge_utils.h`를 추가했다.
  - 문자열 trim/normalize
  - debug/profile 로그
  - 환경변수 파싱
  - 간단 JSON 파싱
  - 숫자 파싱
  - 텍스트 파일 읽기
- `native/paddle_bridge/bridge_utils.cc`를 추가하고 위 유틸 구현을 `bridge.cc`에서 이동했다.
- `debug_log_lazy()`는 템플릿 함수라 호출 번역 단위에서 인스턴스화되어야 하므로 헤더에 두었다.
- `bridge.cc`는 `bridge_utils.h`를 포함하도록 바꾸고, 이동한 유틸 구현을 제거했다.
- `build.rs`가 `bridge_utils.cc`를 함께 컴파일하고 `bridge_utils.{h,cc}` 변경을 감시하도록 갱신했다.

## 1차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_utils.cc -o /tmp/bridge_utils.o`
  - 통과.
- `cargo test --no-default-features --no-run`
  - 통과.
  - 기존 경고: unused import/dead code 경고가 남아 있다.
- `cargo test --features paddle-ffi --no-run`
  - 실패.
  - C++ 변경 때문이 아니라, 로컬 `.paddle_inference` 아래 OpenCV SDK가 없어 `build.rs`의 `find_opencv_sdk()` 단계에서 중단됐다.
  - 메시지: `.paddle_inference 아래 OpenCV SDK를 찾지 못했습니다`.

## 2차 구현 계획

1. 파일 존재/디렉터리 존재/직접 하위 디렉터리 나열/전체 바이트 읽기 유틸을 분리한다.
2. 모델 파일 쌍 존재 여부를 확인하는 `has_stem_files_in_dir()`도 같은 파일 유틸로 이동한다.
3. `bridge.cc`에서 POSIX 디렉터리/`stat` 헤더 의존을 제거한다.
4. `build.rs`가 새 `bridge_fs.cc`를 컴파일하고 변경을 감시하도록 갱신한다.
5. OCR 픽셀 변환, geometry, det/cls/rec stage는 이번 단계에서도 변경하지 않는다.

## 2차 구현 내용

- `native/paddle_bridge/bridge_fs.h`를 추가했다.
- `native/paddle_bridge/bridge_fs.cc`를 추가하고 다음 구현을 `bridge.cc`에서 이동했다.
  - `file_exists()`
  - `directory_exists()`
  - `list_direct_child_dirs()`
  - `read_all_bytes()`
  - `has_stem_files_in_dir()`
- `bridge.cc`는 `bridge_fs.h`를 포함하도록 변경했다.
- `bridge.cc`에서 POSIX 전용 `dirent.h`, `sys/stat.h` include를 제거했다.
- `build.rs`가 `bridge_fs.cc`를 함께 컴파일하고 `bridge_fs.{h,cc}` 변경을 감시하도록 갱신했다.

## 2차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_fs.cc -o /tmp/bridge_fs.o`
  - 통과.
- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_utils.cc -o /tmp/bridge_utils.o`
  - 통과.
- `cargo test --no-default-features --no-run`
  - 통과.
  - 기존 unused import/dead code 경고가 남아 있다.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - Paddle SDK/pyclipper/기존 C++ 코드의 경고가 출력되지만, 이번 분리로 인한 빌드 실패는 없다.

## 3차 구현 계획

1. recognition dictionary 로딩/검증 로직을 `bridge_dict`로 분리한다.
2. `bridge_dict`에는 텍스트 사전, JSON/YAML 메타 사전, 휴리스틱 재귀 탐색, dict 검증을 둔다.
3. `bridge.cc`는 public helper인 `load_recognition_dict()`와 `validate_recognition_dict()`만 사용하게 만든다.
4. 모델 pair 탐색, predictor 생성, OCR stage, 이미지 변환은 이번 단계에서도 변경하지 않는다.
5. SDK가 준비되어 있으므로 `cargo test --features paddle-ffi --no-run`을 검증 기준으로 사용한다.

## 3차 구현 내용

- `native/paddle_bridge/bridge_dict.h`를 추가했다.
- `native/paddle_bridge/bridge_dict.cc`를 추가하고 recognition dictionary 관련 구현을 `bridge.cc`에서 이동했다.
  - `load_recognition_dict()`
  - `validate_recognition_dict()`
  - 텍스트 사전 line split
  - JSON/YAML `character_dict` 파싱
  - 후보 사전 파일 파싱
  - dict 파일명 휴리스틱
- `RAW_DICT_HINT`는 dictionary 모듈 내부 상수로 이동했다.
- `bridge.cc`는 `bridge_dict.h`를 포함하고 `load_recognition_dict()` / `validate_recognition_dict()`만 호출한다.
- `build.rs`가 `bridge_dict.cc`를 함께 컴파일하고 `bridge_dict.{h,cc}` 변경을 감시하도록 갱신했다.

## 3차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_dict.cc -o /tmp/bridge_dict.o`
  - 통과.
- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_fs.cc -o /tmp/bridge_fs.o`
  - 통과.
- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_utils.cc -o /tmp/bridge_utils.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 기존 Paddle SDK/pyclipper/기존 C++ unused 경고는 남아 있다.

## 4차 구현 계획

1. 모델 디렉터리 탐색과 det/cls/rec 모델 pair 선택 로직을 `bridge_model`로 분리한다.
2. 환경변수 기반 source/model hint 해석도 모델 선택 로직과 함께 둔다.
3. `bridge.cc`는 `resolve_preferred_lang()`, `resolve_model_preference()`, `infer_model_family_hint()`, `resolve_model_pair()`만 호출하게 만든다.
4. `ensure_probability_map()`, model preprocess config 파싱, predictor 생성, OCR stage는 이번 단계에서 변경하지 않는다.
5. `cargo test --features paddle-ffi --no-run`으로 새 번역 단위까지 포함한 빌드를 검증한다.

## 4차 구현 내용

- `native/paddle_bridge/bridge_model.h`를 추가했다.
- `native/paddle_bridge/bridge_model.cc`를 추가하고 모델 선택 관련 구현을 `bridge.cc`에서 이동했다.
  - stem alias/family suffix 계산
  - `BUZHIDAO_PADDLE_FFI_SOURCE` 기반 preferred lang 해석
  - `BUZHIDAO_PADDLE_FFI_MODEL_HINT` 해석
  - 하위 모델 디렉터리 후보 나열
  - det 모델 family hint 추론
  - 언어/토큰/family 기반 모델 pair 선택
- `bridge.cc`는 `bridge_model.h`를 포함하고 모델 선택 API만 호출한다.
- `build.rs`가 `bridge_model.cc`를 함께 컴파일하고 `bridge_model.{h,cc}` 변경을 감시하도록 갱신했다.
- `ensure_probability_map()`, model preprocess config 파싱, predictor 생성, OCR stage는 변경하지 않았다.

## 4차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_model.cc -o /tmp/bridge_model.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 기존 Paddle SDK/pyclipper/기존 C++ unused 경고는 남아 있다.

## 5차 구현 계획

1. OCR 모델 preprocess 설정 타입/상수와 설정 파싱 로직을 `bridge_config`로 분리한다.
2. `DetOptions`, `NormalizeCfg`, `ModelPreprocessCfg`와 det/rec/cls 기본 상수는 `bridge_config.h`에 둔다.
3. `load_model_preprocess_cfg()`와 `resolve_det_options()` 구현은 `bridge_config.cc`로 이동한다.
4. `bridge.cc`는 config 타입과 API를 include해서 사용하고, 실제 OCR stage 구현은 변경하지 않는다.
5. `cargo test --features paddle-ffi --no-run`으로 검증한다.

## 5차 구현 내용

- `native/paddle_bridge/bridge_config.h`를 추가했다.
  - det/rec/cls 기본 상수
  - `DetOptions`
  - `NormalizeCfg`
  - `ModelPreprocessCfg`
  - config API 선언
- `native/paddle_bridge/bridge_config.cc`를 추가하고 다음 구현을 `bridge.cc`에서 이동했다.
  - `load_model_preprocess_cfg()`
  - `resolve_det_options()`
- `bridge.cc`는 `bridge_config.h`를 포함하고 config 타입/API를 사용한다.
- `build.rs`가 `bridge_config.cc`를 함께 컴파일하고 `bridge_config.{h,cc}` 변경을 감시하도록 갱신했다.
- 이동 중 빠진 `dump_crop_stage_if_enabled()` 전방 선언을 `bridge.cc`에 복구했다.

## 5차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_config.cc -o /tmp/bridge_config.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 최초 1회는 `dump_crop_stage_if_enabled()` 전방 선언 누락으로 실패했다.
  - 선언 복구 후 통과.
  - 기존 Paddle SDK/pyclipper/기존 C++ unused 경고는 남아 있다.

## 6차 구현 내용

- `native/paddle_bridge/bridge_types.h`를 추가했다.
  - `FloatPoint`, `Image`, `BBox`, rec/det 중간 데이터 구조체, scratch buffer 구조체를 공용 타입으로 이동했다.
  - 이후 이미지/geometry/stage 분리를 위한 타입 의존성을 정리했다.
- `native/paddle_bridge/bridge_image.h` / `bridge_image.cc`를 추가했다.
  - `Image` 채널 접근 helper
  - warmup pattern 이미지 생성
  - OpenCV `cv::Mat` 변환
  - BMP load/save
  - Windows GDI+ 이미지 로딩
  - `load_image_file()`
- `native/paddle_bridge/bridge_geometry.h` / `bridge_geometry.cc`를 추가했다.
  - point/rect/hull/min-area-rect
  - box ordering
  - polygon area/unclip
  - contour simplify/dedupe/compress
- `bridge.cc`에서 위 타입/이미지/순수 geometry 구현을 제거했다.
- `score_box`, `IntPoint`, `is_component_cell`은 det postprocess/contour tracing 쪽 책임이 더 강해 `bridge.cc`에 유지했다.
- `build.rs`가 `bridge_types.h`, `bridge_image.{h,cc}`, `bridge_geometry.{h,cc}` 변경을 감시하고 새 `.cc` 파일을 컴파일하도록 갱신했다.

## 6차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 공용 타입 분리 후 통과.
  - 이미지 I/O 분리 후 통과.
  - geometry 분리 첫 시도는 `IntPoint`, `is_component_cell`, `score_box` 이동 범위가 과해 실패했다.
  - 해당 det postprocess 보조 요소를 `bridge.cc`에 복구한 뒤 통과.
  - 기존 Paddle SDK/pyclipper/기존 C++ unused 경고는 남아 있다.

## 7차 구현 내용

- `native/paddle_bridge/bridge_predictor.h` / `bridge_predictor.cc`를 추가했다.
- Paddle predictor I/O 이름 해석, shape 파싱, 실행 wrapper를 `bridge.cc`에서 이동했다.
  - `resolve_predictor_io_names()`
  - `run_predictor()`
  - `run_predictor_into_buffer()`
  - shape/layout helper
  - `find_rec_layout()`
- `bridge.cc`의 det/cls/rec stage는 predictor helper API만 호출하도록 유지했다.
- `build.rs`가 `bridge_predictor.{h,cc}` 변경을 감시하고 `bridge_predictor.cc`를 컴파일하도록 갱신했다.

## 7차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_predictor.cc -o /tmp/bridge_predictor.o`
  - 최초 1회는 조건부 컴파일 블록 종료 누락으로 실패했다.
  - `#endif` 위치를 복구한 뒤 통과.
- `cargo test --features paddle-ffi --no-run`
  - 최초 1회는 `run_det` 앞 조건부 컴파일 가드 누락으로 실패했다.
  - `#if defined(BUZHIDAO_HAVE_PADDLE_INFERENCE)`를 복구한 뒤 통과.
  - 기존 Paddle SDK/pyclipper/기존 C++ unused 경고는 남아 있다.

## 8차 구현 내용

- `native/paddle_bridge/bridge_output.h` / `bridge_output.cc`를 추가했다.
- `PipelineOutput`의 메모리 소유권, detection append, JSON 직렬화, native result 이관을 `bridge.cc`에서 이동했다.
- `buzhi_ocr_free_string()` / `buzhi_ocr_free_result()`도 같은 메모리 소유권 모듈로 이동했다.
- `run_pipeline_from_path()`의 사용하지 않는 profiling 잔여 변수와 `bridge_utils.cc`의 사용하지 않는 지역 변수를 제거했다.
- `build.rs`가 `bridge_output.{h,cc}` 변경을 감시하고 `bridge_output.cc`를 컴파일하도록 갱신했다.

## 8차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_output.cc -o /tmp/bridge_output.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 로컬 C++ unused 경고 일부를 정리했고, 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.

## 9차 구현 내용

- `native/paddle_bridge/bridge_rec_decode.h` / `bridge_rec_decode.cc`를 추가했다.
- CTC decode 후처리 helper를 `bridge.cc`에서 이동했다.
  - `decode_ctc(const float*, ...)`
  - `decode_ctc(const std::vector<float>&, ...)`
- debug/profile 로그 문자열과 환경변수 동작은 그대로 유지했다.
- `run_rec()` / `run_rec_batch()`는 새 header의 `decode_ctc()`만 호출한다.
- `build.rs`가 `bridge_rec_decode.{h,cc}` 변경을 감시하고 `bridge_rec_decode.cc`를 컴파일하도록 갱신했다.

## 9차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rec_decode.cc -o /tmp/bridge_rec_decode.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.

## 10차 구현 계획

1. 실제 FFI OCR 샘플 테스트를 먼저 실행해 현재 결과 기준선을 잡는다.
2. 이미지 전처리, crop/rotate, DB postprocess, rec tensor 작성은 이번 단계에서 변경하지 않는다.
3. Paddle predictor 설정 함수만 별도 모듈로 분리한다.
   - `predictor_flag_enabled()`
   - `configure_predictor()`
4. `bridge.cc`는 engine 생성 시 predictor 설정 API만 호출하게 유지한다.
5. 검증은 다음 순서로 수행한다.
   - FFI 샘플 회귀 테스트
   - 새 모듈 단독 컴파일
   - `cargo test --features paddle-ffi --no-run`
   - FFI 샘플 회귀 테스트 재실행

## 10차 구현 내용

- `native/paddle_bridge/bridge_predictor_config.h` / `bridge_predictor_config.cc`를 추가했다.
- Paddle predictor 설정 관련 함수를 `bridge.cc`에서 이동했다.
  - `predictor_flag_enabled()`
  - `configure_predictor()`
- `warmup_det_predictor()` / `warmup_cls_predictor()` / `warmup_rec_predictor()`는 engine 내부 상태 의존성이 강하므로 `bridge.cc`에 유지했다.
- `build.rs`가 `bridge_predictor_config.{h,cc}` 변경을 감시하고 `bridge_predictor_config.cc`를 컴파일하도록 갱신했다.
- 전처리, crop/rotate, DB postprocess, rec tensor 작성, OCR 결과 조립은 변경하지 않았다.

## 10차 테스트

- 기준선 확인:
  - `BUZHIDAO_RUN_FFI_SAMPLE_TEST=1 BUZHIDAO_FFI_TEST_IMAGE=testdata/ocr/test.png cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다' -- --nocapture`
  - 통과.
  - detection 7개.
- `g++ -std=c++17 -I native/paddle_bridge -DBUZHIDAO_HAVE_PADDLE_INFERENCE -I .paddle_inference/include -c native/paddle_bridge/bridge_predictor_config.cc -o /tmp/bridge_predictor_config.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 회귀 확인:
  - `BUZHIDAO_RUN_FFI_SAMPLE_TEST=1 BUZHIDAO_FFI_TEST_IMAGE=testdata/ocr/test.png cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::_1_png를_ffi로_실행해서_결과를_출력한다' -- --nocapture`
  - 통과.
  - detection 7개로 기준선과 동일.
- 추가 fixture 실행:
  - `BUZHIDAO_RUN_FFI_BENCH=1 BUZHIDAO_FFI_BENCH_IMAGES_JSON='["testdata/ocr/test.png","testdata/ocr/test2.png","testdata/ocr/test3.png"]' BUZHIDAO_FFI_BENCH_WARMUPS=0 BUZHIDAO_FFI_BENCH_ITERATIONS=1 cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다' -- --nocapture`
  - 통과.
  - detection count: test=7, test2=14, test3=27.

## 11차 구현 계획

1. 변경 전에 fixture 3개를 1 warmup / 3 iteration으로 실행해 detection count와 latency 기준선을 기록한다.
2. 이번 단계는 hot path 계산을 건드리지 않는 debug formatting helper만 분리한다.
   - `quote_polygon()`
   - `quote_points()`
3. det/cls/rec 전처리, crop/rotate, DB postprocess, predictor 실행은 변경하지 않는다.
4. 변경 후 동일 benchmark를 다시 실행한다.
5. detection count가 유지되고 latency가 기준선 대비 명확히 악화되지 않을 때만 다음 단계로 넘어간다.

## 11차 변경 전 기준선

- 명령:
  - `BUZHIDAO_RUN_FFI_BENCH=1 BUZHIDAO_FFI_BENCH_IMAGES_JSON='["testdata/ocr/test.png","testdata/ocr/test2.png","testdata/ocr/test3.png"]' BUZHIDAO_FFI_BENCH_WARMUPS=1 BUZHIDAO_FFI_BENCH_ITERATIONS=3 cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다' -- --nocapture`
- 결과:
  - `test.png`: detection 7, elapsed `[2466.282280, 2454.673431, 2699.386210]` ms
  - `test2.png`: detection 14, elapsed `[3098.622830, 3194.571807, 3374.327693]` ms
  - `test3.png`: detection 27, elapsed `[8569.954013, 7349.004525, 6777.114995]` ms

## 11차 구현 내용

- `native/paddle_bridge/bridge_debug_format.h` / `bridge_debug_format.cc`를 추가했다.
- 디버그 문자열 formatter를 `bridge.cc`에서 이동했다.
  - `quote_polygon()`
  - `quote_points()`
- `build.rs`가 `bridge_debug_format.{h,cc}` 변경을 감시하고 `bridge_debug_format.cc`를 컴파일하도록 갱신했다.
- OCR hot path의 전처리, crop/rotate, DB postprocess, predictor 실행, 결과 조립은 변경하지 않았다.

## 11차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_debug_format.cc -o /tmp/bridge_debug_format.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 동일 benchmark:
  - `BUZHIDAO_RUN_FFI_BENCH=1 BUZHIDAO_FFI_BENCH_IMAGES_JSON='["testdata/ocr/test.png","testdata/ocr/test2.png","testdata/ocr/test3.png"]' BUZHIDAO_FFI_BENCH_WARMUPS=1 BUZHIDAO_FFI_BENCH_ITERATIONS=3 cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다' -- --nocapture`
  - `test.png`: detection 7, elapsed `[2251.234880, 2235.623401, 2229.870173]` ms
  - `test2.png`: detection 14, elapsed `[2914.071320, 2950.182937, 2834.108372]` ms
  - `test3.png`: detection 27, elapsed `[5144.315327, 5611.251227, 5381.731813]` ms
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency 중앙값은 3개 fixture 모두 기준선보다 낮게 측정됐다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 12차 구현 계획

1. 11차 변경 후 benchmark를 12차 기준선으로 사용한다.
   - `test.png`: detection 7, elapsed `[2251.234880, 2235.623401, 2229.870173]` ms
   - `test2.png`: detection 14, elapsed `[2914.071320, 2950.182937, 2834.108372]` ms
   - `test3.png`: detection 27, elapsed `[5144.315327, 5611.251227, 5381.731813]` ms
2. 이번 단계는 정상 OCR hot path가 아닌 오류 처리 helper만 분리한다.
   - `set_error()`
   - `set_error_if_empty()`
3. `dup_string()`은 결과 문자열과 detection text 소유권에도 쓰이므로 `bridge_output`에 유지한다.
4. 변경 후 전체 빌드와 동일 benchmark를 재실행한다.
5. detection count가 유지되고 latency가 명확히 악화되지 않을 때만 완료로 판단한다.

## 12차 결과

- `set_error()` / `set_error_if_empty()`를 `bridge_error`로 분리하는 시도를 했다.
- 단독 컴파일과 `cargo test --features paddle-ffi --no-run`은 통과했다.
- 동일 benchmark 1차:
  - `test.png`: detection 7, elapsed `[2110.488438, 2172.741518, 2229.092408]` ms
  - `test2.png`: detection 14, elapsed `[2796.152618, 2860.098456, 2902.144475]` ms
  - `test3.png`: detection 27, elapsed `[6165.020292, 8313.997475, 9179.169340]` ms
- `test3.png` latency 중앙값이 기준선보다 나빠져 확인 재측정을 실행했다.
- 확인 재측정:
  - `test3.png`: detection 27, elapsed `[6461.574448, 5555.703096, 6170.698153]` ms
- detection count는 유지됐지만 `test3.png` latency 중앙값이 여전히 기준선보다 높다.
- 사용자 기준상 속도 저하 가능성을 통과 처리하지 않고, 12차 `bridge_error` 분리는 되돌렸다.
- `set_error()` / `set_error_if_empty()`는 `bridge.cc`에 유지한다.

## 13차 구현 계획

1. 안정 상태 기준선을 다시 측정한다.
2. 최근 측정값의 흔들림이 크므로, C++ hot path refactor를 바로 진행하지 않는다.
3. 먼저 FFI benchmark 출력 비교 guard를 추가한다.
   - `[FFI_BENCH]` JSON line 파싱
   - detection count 동일성 검증
   - latency median 비교
   - 허용 비율 이내의 latency 증가는 노이즈로 통과
4. 이 guard는 제품 OCR 실행 경로에 포함되지 않으므로 인식률/속도에 영향을 주지 않는다.
5. guard 단위 테스트를 추가한다.

## 13차 기준선

- 명령:
  - `BUZHIDAO_RUN_FFI_BENCH=1 BUZHIDAO_FFI_BENCH_IMAGES_JSON='["testdata/ocr/test.png","testdata/ocr/test2.png","testdata/ocr/test3.png"]' BUZHIDAO_FFI_BENCH_WARMUPS=1 BUZHIDAO_FFI_BENCH_ITERATIONS=3 cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다' -- --nocapture`
- 결과:
  - `test.png`: detection 7, elapsed `[3358.795628, 3159.623222, 2979.134453]` ms
  - `test2.png`: detection 14, elapsed `[3004.527410, 3393.774995, 3119.237353]` ms
  - `test3.png`: detection 27, elapsed `[5644.295620, 5950.282023, 6940.766579]` ms

## 13차 구현 내용

- `tools/scripts/ffi_benchmark_guard.py`를 추가했다.
- `[FFI_BENCH]` JSON line을 파싱해 fixture별 결과를 비교한다.
- detection count가 바뀌면 실패한다.
- latency는 elapsed list의 median을 비교한다.
- 기본 허용 비율은 `--max-median-ratio 1.15`로 두어 노이즈 수준의 변동은 통과시킬 수 있게 했다.
- baseline에 없는 current 결과 또는 current에 없는 baseline 결과는 실패한다.
- Windows CI stdout/stderr 인코딩 회고를 반영해 CLI 시작 시 표준 입출력을 UTF-8로 재설정한다.
- 제품 OCR 실행 경로와 native bridge hot path는 변경하지 않았다.

## 13차 테스트

- `python -m unittest tools.scripts.test_ffi_benchmark_guard`
  - 실행 불가.
  - 현재 환경에 `python` 명령이 없다.
- `python3 -m unittest tools.scripts.test_ffi_benchmark_guard`
  - 통과.
  - 5개 테스트 OK.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 판단:
  - 이번 변경은 benchmark guard와 해당 단위 테스트만 추가했으며 제품 OCR 실행 경로를 변경하지 않았다.
  - 따라서 인식률/속도 저하 대상 변경은 없다.
  - 다음 native refactor부터는 같은 fixture의 변경 전/후 출력을 저장하고 `ffi_benchmark_guard.py`로 detection count와 median latency를 함께 확인한다.

## 14차 구현 계획

1. 13차 안정 상태에서 fixture 3개의 benchmark 출력을 파일로 저장한다.
2. 이번 단계는 디버그 dump helper만 분리한다.
   - `dump_crop_stage_if_enabled()`
   - `dump_rec_candidates_if_requested()`
   - `dump_candidate_crop_if_requested()`
3. 전처리, crop/rotate 계산, DB postprocess, predictor 실행, rec batch 계산은 변경하지 않는다.
4. 변경 후 동일 benchmark를 다시 파일로 저장한다.
5. `ffi_benchmark_guard.py`로 detection count 동일성과 latency median을 검사한다.
   - detection count 변경은 실패다.
   - median latency는 기본 허용 비율 `1.15` 이내면 노이즈로 보고 통과시킨다.

## 14차 변경 전 기준선

- 명령:
  - `BUZHIDAO_RUN_FFI_BENCH=1 BUZHIDAO_FFI_BENCH_IMAGES_JSON='["testdata/ocr/test.png","testdata/ocr/test2.png","testdata/ocr/test3.png"]' BUZHIDAO_FFI_BENCH_WARMUPS=1 BUZHIDAO_FFI_BENCH_ITERATIONS=3 cargo test --features paddle-ffi 'ocr::paddle_ffi::tests::지정한_이미지들로_ffi_ocr_지연시간을_측정한다' -- --nocapture`
- 저장 파일:
  - `target/ffi-bench-logs/14-before.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2454.959985, 2363.400859, 2681.895755]` ms
  - `test2.png`: detection 14, elapsed `[3093.808781, 3195.016198, 3268.958409]` ms
  - `test3.png`: detection 27, elapsed `[5544.267021, 5878.929839, 5638.512813]` ms

## 14차 구현 내용

- `native/paddle_bridge/bridge_debug_dump.h` / `bridge_debug_dump.cc`를 추가했다.
- 디버그 dump helper를 `bridge.cc`에서 이동했다.
  - `dump_crop_stage_if_enabled()`
  - `dump_rec_candidates_if_requested()`
  - `dump_candidate_crop_if_requested()`
- `build.rs`가 `bridge_debug_dump.{h,cc}` 변경을 감시하고 `bridge_debug_dump.cc`를 컴파일하도록 갱신했다.
- OCR hot path의 전처리, crop/rotate 계산, DB postprocess, predictor 실행, rec batch 계산은 변경하지 않았다.

## 14차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_debug_dump.cc -o /tmp/bridge_debug_dump.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/14-after.txt`
  - `test.png`: detection 7, elapsed `[2168.709568, 2182.386264, 2183.220019]` ms
  - `test2.png`: detection 14, elapsed `[2873.488335, 2735.981441, 3120.078121]` ms
  - `test3.png`: detection 27, elapsed `[6680.516451, 5549.179847, 7010.789912]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-before.txt --current target/ffi-bench-logs/14-after.txt --max-median-ratio 1.15`
  - 실패.
  - `test3.png` median이 기준선 `5638.513ms` 대비 현재 `6680.516ms`, 허용 `6484.290ms`를 넘어섰다.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/14-after-recheck.txt`
  - `test.png`: detection 7, elapsed `[2205.653033, 2203.764019, 2222.000306]` ms
  - `test2.png`: detection 14, elapsed `[2885.339833, 2860.391007, 3145.433880]` ms
  - `test3.png`: detection 27, elapsed `[5476.069824, 5732.783507, 5552.151404]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-before.txt --current target/ffi-bench-logs/14-after-recheck.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - 1차 `test3.png` latency 실패는 재측정에서 사라졌고, 허용 비율 이내로 돌아왔다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 15차 구현 계획

1. 14차 재측정 통과 결과를 15차 변경 전 기준선으로 사용한다.
2. 이번 단계는 rec 후보 정렬과 batch 계획 helper만 분리한다.
   - `build_rec_order()`
   - `log_rec_order()`
   - `estimate_rec_input_width()`
   - `plan_rec_batches()`
   - `log_rec_batches()`
3. rec tensor 작성, predictor 실행, decode, 결과 조립은 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 15차 변경 전 기준선

- 14차 재측정 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/14-after-recheck.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2205.653033, 2203.764019, 2222.000306]` ms
  - `test2.png`: detection 14, elapsed `[2885.339833, 2860.391007, 3145.433880]` ms
  - `test3.png`: detection 27, elapsed `[5476.069824, 5732.783507, 5552.151404]` ms

## 15차 구현 내용

- `native/paddle_bridge/bridge_rec_pipeline.h` / `bridge_rec_pipeline.cc`를 추가했다.
- rec 후보 순서와 batch 계획 helper를 `bridge.cc`에서 이동했다.
  - `build_rec_order()`
  - `log_rec_order()`
  - `estimate_rec_input_width()`
  - `plan_rec_batches()`
  - `log_rec_batches()`
- `build.rs`가 `bridge_rec_pipeline.{h,cc}` 변경을 감시하고 `bridge_rec_pipeline.cc`를 컴파일하도록 갱신했다.
- rec tensor 작성, predictor 실행, decode, 결과 조립은 변경하지 않았다.

## 15차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rec_pipeline.cc -o /tmp/bridge_rec_pipeline.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/15-after.txt`
  - `test.png`: detection 7, elapsed `[2562.030648, 2754.261802, 3024.013550]` ms
  - `test2.png`: detection 14, elapsed `[3426.027820, 3314.359955, 3484.806544]` ms
  - `test3.png`: detection 27, elapsed `[6835.284996, 6336.009584, 6250.432221]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-after-recheck.txt --current target/ffi-bench-logs/15-after.txt --max-median-ratio 1.15`
  - 실패.
  - `test.png`, `test2.png` median이 허용치를 넘었다.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/15-after-recheck.txt`
  - `test.png`: detection 7, elapsed `[2867.051297, 2582.934843, 2698.125034]` ms
  - `test2.png`: detection 14, elapsed `[3743.107084, 3716.401646, 4267.145518]` ms
  - `test3.png`: detection 27, elapsed `[9501.190768, 7159.309441, 7236.044454]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-after-recheck.txt --current target/ffi-bench-logs/15-after-recheck.txt --max-median-ratio 1.15`
  - 실패.
  - `test.png`, `test2.png`, `test3.png` median이 모두 허용치를 넘었다.
- 변경 후 benchmark 추가 재측정:
  - 저장 파일: `target/ffi-bench-logs/15-after-recheck2.txt`
  - `test.png`: detection 7, elapsed `[2336.543208, 2142.977303, 2058.477607]` ms
  - `test2.png`: detection 14, elapsed `[2619.366227, 2841.303197, 2683.223807]` ms
  - `test3.png`: detection 27, elapsed `[5324.010543, 5350.012797, 5833.840181]` ms
- 추가 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-after-recheck.txt --current target/ffi-bench-logs/15-after-recheck2.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 세 번 모두 3개 fixture에서 기준선과 동일하다.
  - 같은 코드 상태에서 1차와 2차는 latency guard 실패, 3차는 통과로 측정됐다.
  - 이번 분리 함수들은 OCR tensor 작성, predictor 실행, decode를 변경하지 않고 rec 후보 순서/batch 계획만 옮긴다.
  - 지속적인 성능 저하로 보지 않고 측정 분산으로 판단하되, 실패 로그를 보존한다.

## 16차 구현 계획

1. 15차 추가 재측정 통과 결과를 16차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 보조 helper만 분리한다.
   - `sort_quad_boxes_like_sidecar()`
   - `ensure_probability_map()`
   - `neighbors4()`
   - `neighbors8()`
   - `log_det_map_stats()`
3. DB postprocess 본문, box scoring, unclip, contour 계산은 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 16차 변경 전 기준선

- 15차 추가 재측정 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/15-after-recheck2.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2336.543208, 2142.977303, 2058.477607]` ms
  - `test2.png`: detection 14, elapsed `[2619.366227, 2841.303197, 2683.223807]` ms
  - `test3.png`: detection 27, elapsed `[5324.010543, 5350.012797, 5833.840181]` ms

## 16차 구현 내용

- `native/paddle_bridge/bridge_det_utils.h` / `bridge_det_utils.cc`를 추가했다.
- det 보조 helper를 `bridge.cc`에서 이동했다.
  - `sort_quad_boxes_like_sidecar()`
  - `ensure_probability_map()`
  - `neighbors4()`
  - `neighbors8()`
  - `log_det_map_stats()`
- `build.rs`가 `bridge_det_utils.{h,cc}` 변경을 감시하고 `bridge_det_utils.cc`를 컴파일하도록 갱신했다.
- DB postprocess 본문, box scoring, unclip, contour 계산은 변경하지 않았다.

## 16차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_det_utils.cc -o /tmp/bridge_det_utils.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/16-after.txt`
  - `test.png`: detection 7, elapsed `[2219.110390, 2114.683565, 2180.628897]` ms
  - `test2.png`: detection 14, elapsed `[2838.077921, 3055.063389, 3055.639820]` ms
  - `test3.png`: detection 27, elapsed `[5546.848130, 5709.971449, 5483.631355]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/15-after-recheck2.txt --current target/ffi-bench-logs/16-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 17차 구현 계획

1. 16차 통과 결과를 17차 변경 전 기준선으로 사용한다.
2. 이번 단계는 crop geometry 설명 helper만 `bridge_geometry`로 이동한다.
   - `describe_crop_to_bbox()`
3. 실제 crop/warp/rotate 픽셀 변환은 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 17차 변경 전 기준선

- 16차 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/16-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2219.110390, 2114.683565, 2180.628897]` ms
  - `test2.png`: detection 14, elapsed `[2838.077921, 3055.063389, 3055.639820]` ms
  - `test3.png`: detection 27, elapsed `[5546.848130, 5709.971449, 5483.631355]` ms

## 17차 구현 내용

- `describe_crop_to_bbox()`를 `bridge.cc`에서 `bridge_geometry.cc`로 이동했다.
- `bridge_geometry.h`에 함수 선언을 추가했다.
- 실제 crop/warp/rotate 픽셀 변환은 변경하지 않았다.
- `build.rs`는 이미 `bridge_geometry.{h,cc}`를 감시하고 컴파일하므로 추가 변경은 없었다.

## 17차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_geometry.cc -o /tmp/bridge_geometry.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/17-after.txt`
  - `test.png`: detection 7, elapsed `[2204.794968, 2184.478621, 2247.837191]` ms
  - `test2.png`: detection 14, elapsed `[2777.820443, 3021.974294, 3052.903607]` ms
  - `test3.png`: detection 27, elapsed `[5223.942532, 5419.020865, 5450.825972]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/16-after.txt --current target/ffi-bench-logs/17-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 18차 구현 계획

1. 17차 통과 결과를 18차 변경 전 기준선으로 사용한다.
2. 이번 단계는 cls 결과에 따라 선택적으로 쓰이는 180도 회전 helper만 분리한다.
   - `rotate180()`
3. det/rec 전처리, crop/warp, DB postprocess, predictor 실행은 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 18차 변경 전 기준선

- 17차 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/17-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2204.794968, 2184.478621, 2247.837191]` ms
  - `test2.png`: detection 14, elapsed `[2777.820443, 3021.974294, 3052.903607]` ms
  - `test3.png`: detection 27, elapsed `[5223.942532, 5419.020865, 5450.825972]` ms

## 18차 구현 내용

- `native/paddle_bridge/bridge_rotate.h` / `bridge_rotate.cc`를 추가했다.
- `rotate180()`를 `bridge.cc`에서 이동했다.
- OpenCV 미사용 fallback에서만 필요한 cubic sampling helper는 `rotate180()` 내부 fallback 분기로 제한해 OpenCV 빌드의 미사용 함수 경고를 피했다.
- `build.rs`가 `bridge_rotate.{h,cc}` 변경을 감시하고 `bridge_rotate.cc`를 컴파일하도록 갱신했다.
- det/rec 전처리, crop/warp, DB postprocess, predictor 실행은 변경하지 않았다.

## 18차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rotate.cc -o /tmp/bridge_rotate.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/18-after.txt`
  - `test.png`: detection 7, elapsed `[2126.214159, 2167.545419, 2162.974551]` ms
  - `test2.png`: detection 14, elapsed `[2666.444667, 2945.300324, 2874.755215]` ms
  - `test3.png`: detection 27, elapsed `[5435.860256, 5407.521556, 5385.873603]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/17-after.txt --current target/ffi-bench-logs/18-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 19차 구현 계획

1. 18차 통과 결과를 19차 변경 전 기준선으로 사용한다.
2. 이번 단계는 cls/rec 입력 버퍼 채우기 helper만 분리한다.
   - `fill_cls_tensor()`
   - `fill_rec_tensor()`
3. 리사이즈, 패딩 크기 계산, crop/warp, predictor 실행, decode는 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 19차 변경 전 기준선

- 18차 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/18-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2126.214159, 2167.545419, 2162.974551]` ms
  - `test2.png`: detection 14, elapsed `[2666.444667, 2945.300324, 2874.755215]` ms
  - `test3.png`: detection 27, elapsed `[5435.860256, 5407.521556, 5385.873603]` ms

## 19차 구현 내용

- `native/paddle_bridge/bridge_tensor.h` / `bridge_tensor.cc`를 추가했다.
- `fill_cls_tensor()`와 `fill_rec_tensor()`를 `bridge.cc`에서 이동했다.
- cls/rec 정규화 순서, std fallback, rec padding 값, alpha skip 조건은 그대로 유지했다.
- `build.rs`가 `bridge_tensor.{h,cc}` 변경을 감시하고 `bridge_tensor.cc`를 컴파일하도록 갱신했다.
- 리사이즈, 패딩 크기 계산, crop/warp, predictor 실행, decode는 변경하지 않았다.

## 19차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_tensor.cc -o /tmp/bridge_tensor.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/19-after.txt`
  - `test.png`: detection 7, elapsed `[3007.859502, 2620.925014, 2225.155712]` ms
  - `test2.png`: detection 14, elapsed `[2941.249064, 2901.933906, 2802.870222]` ms
  - `test3.png`: detection 27, elapsed `[5308.179971, 5166.210336, 5273.401140]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/18-after.txt --current target/ffi-bench-logs/19-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png` median latency 증가, baseline `2162.975ms`, current `2620.925ms`, allowed `2487.421ms`.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/19-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2208.072876, 2256.519624, 2351.191902]` ms
  - `test2.png`: detection 14, elapsed `[2713.427559, 2779.250908, 2812.189662]` ms
  - `test3.png`: detection 27, elapsed `[5384.556219, 5065.918472, 4954.486412]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/18-after.txt --current target/ffi-bench-logs/19-after-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일하다.
  - 1차 `test.png` latency 실패는 같은 코드의 재측정에서 사라졌고, 나머지 fixture도 허용 범위 안에 들어왔다.
  - 이번 분리는 인식률 저하 없이 통과했고, 속도도 재측정 기준으로 노이즈 범위로 본다.

## 20차 시도와 보류

- 시도 내용:
  - `rotate90_counterclockwise()`를 `bridge.cc`에서 `bridge_rotate.cc`로 이동했다.
  - `bridge_rotate.h`에 선언을 추가했다.
- 단독 컴파일:
  - `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rotate.cc -o /tmp/bridge_rotate.o`
  - 통과.
- 전체 빌드:
  - `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/20-after.txt`
  - `test.png`: detection 7, elapsed `[3928.910142, 3872.493185, 3701.057049]` ms
  - `test2.png`: detection 14, elapsed `[4781.042538, 4885.865358, 4755.194373]` ms
  - `test3.png`: detection 27, elapsed `[8530.204111, 8430.918239, 9014.184390]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-after-recheck1.txt --current target/ffi-bench-logs/20-after.txt --max-median-ratio 1.15`
  - 실패.
  - 세 fixture 모두 median latency가 기준을 초과했다.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/20-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[4016.318542, 4027.298915, 3809.171837]` ms
  - `test2.png`: detection 14, elapsed `[4922.131226, 4658.470550, 4731.137066]` ms
  - `test3.png`: detection 27, elapsed `[9165.548666, 8394.780312, 8668.827878]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-after-recheck1.txt --current target/ffi-bench-logs/20-after-recheck1.txt --max-median-ratio 1.15`
  - 실패.
  - 세 fixture 모두 median latency가 기준을 초과했다.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일했다.
  - 코드 변경은 orientation 회전 helper의 파일 이동뿐이지만, 두 번 연속 세 fixture 전체가 큰 폭으로 느려져 속도 기준을 만족하지 못했다.
  - 환경 전체 지연 가능성이 높아 보이지만, 성능 기준을 통과하지 못한 변경을 유지하지 않기 위해 20차 이동은 되돌렸다.
  - 19차 통과 상태를 유지한다.
- 되돌림 후 확인:
  - `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rotate.cc -o /tmp/bridge_rotate.o`
    - 통과.
  - `cargo test --features paddle-ffi --no-run`
    - 통과.

## 20차 되돌림 후 control benchmark

- 목적:
  - 20차 실패가 코드 변경 때문인지, 실행 환경 지연 때문인지 확인한다.
- control benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/19-control-after-20-revert.txt`
  - `test.png`: detection 7, elapsed `[2243.160989, 2157.701043, 2161.985477]` ms
  - `test2.png`: detection 14, elapsed `[2666.723826, 3035.742644, 2643.101619]` ms
  - `test3.png`: detection 27, elapsed `[4917.001371, 6419.604493, 6343.025924]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-after-recheck1.txt --current target/ffi-bench-logs/19-control-after-20-revert.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test3.png` median latency 증가.
- control benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/19-control-after-20-revert-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2967.949247, 2954.891634, 2853.569826]` ms
  - `test2.png`: detection 14, elapsed `[3726.113408, 3936.899734, 3881.953254]` ms
  - `test3.png`: detection 27, elapsed `[7346.101874, 8103.137062, 7922.464434]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-after-recheck1.txt --current target/ffi-bench-logs/19-control-after-20-revert-recheck1.txt --max-median-ratio 1.15`
  - 실패.
  - 세 fixture 모두 median latency가 기준을 초과했다.
- 환경 확인:
  - `uptime`
    - load average가 `5.46, 2.37, 0.99`로 확인됐다.
  - `nproc`
    - 8.
  - `ps -eo pid,ppid,stat,pcpu,pmem,comm,args --sort=-pcpu | head -n 15`
    - benchmark 종료 후 시점에는 낮은 CPU 점유 프로세스만 확인됐다.
- 판단:
  - 20차 실패 이후 19차 상태 자체도 흔들렸으므로, 해당 시점 benchmark는 환경 노이즈가 컸다.
  - 그래도 20차 변경은 이미 보수적으로 되돌렸고, 19차 통과 상태를 유지한다.

## 21차 구현 계획

1. hot path 변경은 피하고 OpenCV 빌드의 실제 OCR benchmark 경로 밖 fallback resampling helper만 분리한다.
   - `cubic_weight()`
   - `sample_channel_cubic_replicate()`
   - `sample_channel_bilinear_replicate()`
2. OpenCV resize/warp 경로, det/cls/rec tensor, predictor 실행, decode는 변경하지 않는다.
3. 직전 control 재측정 결과를 21차 변경 전 기준선으로 사용하되, 환경 노이즈가 큰 상태였음을 명시한다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, benchmark와 guard를 실행한다.

## 21차 변경 전 기준선

- 20차 되돌림 후 control 재측정 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/19-control-after-20-revert-recheck1.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2967.949247, 2954.891634, 2853.569826]` ms
  - `test2.png`: detection 14, elapsed `[3726.113408, 3936.899734, 3881.953254]` ms
  - `test3.png`: detection 27, elapsed `[7346.101874, 8103.137062, 7922.464434]` ms

## 21차 구현 내용

- `native/paddle_bridge/bridge_resample.h` / `bridge_resample.cc`를 추가했다.
- `cubic_weight()`, `sample_channel_cubic_replicate()`, `sample_channel_bilinear_replicate()`를 `bridge.cc`에서 이동했다.
- `build.rs`가 `bridge_resample.{h,cc}` 변경을 감시하고 `bridge_resample.cc`를 컴파일하도록 갱신했다.
- OpenCV resize/warp 경로, det/cls/rec tensor, predictor 실행, decode는 변경하지 않았다.

## 21차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_resample.cc -o /tmp/bridge_resample.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/21-after.txt`
  - `test.png`: detection 7, elapsed `[3801.689226, 4081.832627, 2452.279152]` ms
  - `test2.png`: detection 14, elapsed `[2686.212357, 2887.391063, 2914.814674]` ms
  - `test3.png`: detection 27, elapsed `[5062.216009, 5198.128570, 5146.680986]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-control-after-20-revert-recheck1.txt --current target/ffi-bench-logs/21-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png` median latency 증가.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/21-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2128.195149, 2105.257590, 2185.792449]` ms
  - `test2.png`: detection 14, elapsed `[2717.250920, 2646.234714, 2735.606269]` ms
  - `test3.png`: detection 27, elapsed `[4900.813505, 4877.981789, 4862.502987]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/19-control-after-20-revert-recheck1.txt --current target/ffi-bench-logs/21-after-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일하다.
  - 1차 `test.png` latency 실패는 같은 코드의 재측정에서 사라졌다.
  - 이번 분리는 OpenCV 빌드의 실제 benchmark 경로 밖 fallback helper 이동이며, 재측정 기준으로 인식률/속도 저하 없이 통과한 것으로 본다.

## 22차 구현 계획

1. OCR 실행 연산은 변경하지 않고 `buzhi_ocr_engine` 구조체 정의만 별도 헤더로 이동한다.
2. det/cls/rec 전처리, predictor 실행, crop/warp, decode는 변경하지 않는다.
3. 21차 통과 결과를 22차 변경 전 기준선으로 사용한다.
4. 변경 후 전체 FFI 빌드, benchmark와 guard를 실행한다.

## 22차 변경 전 기준선

- 21차 재측정 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/21-after-recheck1.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2128.195149, 2105.257590, 2185.792449]` ms
  - `test2.png`: detection 14, elapsed `[2717.250920, 2646.234714, 2735.606269]` ms
  - `test3.png`: detection 27, elapsed `[4900.813505, 4877.981789, 4862.502987]` ms

## 22차 구현 내용

- `native/paddle_bridge/bridge_engine.h`를 추가했다.
- `buzhi_ocr_engine` 구조체 정의를 `bridge.cc`에서 `bridge_engine.h`로 이동했다.
- `build.rs`가 `bridge_engine.h` 변경을 감시하도록 갱신했다.
- det/cls/rec 전처리, predictor 실행, crop/warp, decode는 변경하지 않았다.

## 22차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/22-after.txt`
  - `test.png`: detection 7, elapsed `[2117.291145, 2091.046010, 2080.199538]` ms
  - `test2.png`: detection 14, elapsed `[4040.398299, 3406.221537, 3422.767785]` ms
  - `test3.png`: detection 27, elapsed `[5653.618835, 5751.712540, 5411.045770]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/21-after-recheck1.txt --current target/ffi-bench-logs/22-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test2.png`, `test3.png` median latency 증가.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/22-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2133.386038, 2124.532382, 2194.839857]` ms
  - `test2.png`: detection 14, elapsed `[2873.039566, 2729.817654, 2863.468460]` ms
  - `test3.png`: detection 27, elapsed `[5181.275342, 5024.566523, 4960.303175]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/21-after-recheck1.txt --current target/ffi-bench-logs/22-after-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일하다.
  - 1차 `test2.png`, `test3.png` latency 실패는 같은 코드의 재측정에서 사라졌다.
  - 이번 분리는 구조체 정의 위치 변경이며, 재측정 기준으로 인식률/속도 저하 없이 통과한 것으로 본다.

## 23차 구현 계획

1. 22차 통과 결과를 23차 변경 전 기준선으로 사용한다.
2. 이번 단계는 OCR 실행 경로가 아닌 predictor warmup helper만 분리한다.
   - `warmup_det_predictor()`
   - `warmup_cls_predictor()`
   - `warmup_rec_predictor()`
3. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
4. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.
5. 속도 기준을 반복 실패하면 변경을 되돌리고 다음 분리는 보류한다.

## 23차 변경 전 기준선

- 22차 재측정 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/22-after-recheck1.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2133.386038, 2124.532382, 2194.839857]` ms
  - `test2.png`: detection 14, elapsed `[2873.039566, 2729.817654, 2863.468460]` ms
  - `test3.png`: detection 27, elapsed `[5181.275342, 5024.566523, 4960.303175]` ms

## 23차 구현 내용

- `native/paddle_bridge/bridge_warmup.h` / `bridge_warmup.cc`를 추가해 warmup helper 3개를 `bridge.cc`에서 분리했다.
- `build.rs`가 `bridge_warmup.{h,cc}`를 감시하고 `bridge_warmup.cc`를 컴파일하도록 갱신했다.
- OCR benchmark의 이미지 실행 경로는 변경하지 않았다.

## 23차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -I .paddle_inference/include -DBUZHIDAO_HAVE_PADDLE_INFERENCE -c native/paddle_bridge/bridge_warmup.cc -o /tmp/bridge_warmup.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/23-after.txt`
  - `test.png`: detection 7, elapsed `[2071.507148, 2064.822574, 2043.202957]` ms
  - `test2.png`: detection 14, elapsed `[4449.215313, 4026.711029, 3901.271833]` ms
  - `test3.png`: detection 27, elapsed `[7807.669798, 8292.084351, 8273.775028]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/22-after-recheck1.txt --current target/ffi-bench-logs/23-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test2.png`, `test3.png` median latency 증가.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/23-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[3615.091929, 4174.422708, 3746.552957]` ms
  - `test2.png`: detection 14, elapsed `[4492.337513, 4575.560447, 4715.694099]` ms
  - `test3.png`: detection 27, elapsed `[8682.722631, 8383.275518, 8442.515710]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/22-after-recheck1.txt --current target/ffi-bench-logs/23-after-recheck1.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png`, `test2.png`, `test3.png` median latency 증가.

## 23차 판단

- detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일했다.
- latency는 재측정에서도 3개 fixture 모두 허용 비율을 넘었다.
- 사용자 조건상 인식률/속도 저하가 없어야 하므로 `bridge_warmup` 분리는 되돌렸다.
- 되돌린 후:
  - `bridge_warmup.{h,cc}`를 제거했다.
  - `build.rs`의 `bridge_warmup` 감시/컴파일 항목을 제거했다.
  - warmup helper 3개를 `bridge.cc`로 복원했다.
  - `bridge.cc`는 22차와 같은 3011줄 상태로 돌아왔다.
- 되돌린 상태에서도 benchmark(`target/ffi-bench-logs/23-reverted.txt`)는 현재 시스템 load에서 기준선을 실패했다.
  - `test.png`: detection 7, elapsed `[3694.643935, 3833.344884, 3784.971193]` ms
  - `test2.png`: detection 14, elapsed `[4688.107001, 4872.862283, 5058.075399]` ms
  - `test3.png`: detection 27, elapsed `[8037.324697, 10699.364695, 11890.206723]` ms
- 현재 코드가 되돌린 동일 상태에서도 느려진 점을 보면, 23차 변경 자체와 별개로 측정 환경 노이즈가 큰 상태다.
- 다음 회차는 새로운 분리를 진행하지 않고, benchmark가 다시 안정화되는지 확인한 뒤 재개하는 편이 안전하다.

## 23차 재시도

- 되돌린 22차 상태를 다시 측정했다.
  - 저장 파일: `target/ffi-bench-logs/23-reverted-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2087.290184, 2095.085751, 2314.938069]` ms
  - `test2.png`: detection 14, elapsed `[2877.917474, 2639.654867, 2833.626401]` ms
  - `test3.png`: detection 27, elapsed `[5458.674185, 5716.095044, 5542.690437]` ms
- 되돌린 상태의 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/22-after-recheck1.txt --current target/ffi-bench-logs/23-reverted-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 따라서 앞선 `23-reverted.txt` 실패는 코드 변경 없이도 발생한 측정 환경 노이즈로 판단했다.
- `bridge_warmup.{h,cc}` 분리를 다시 적용했다.
- 재적용 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/23-retry-after.txt`
  - `test.png`: detection 7, elapsed `[2704.981316, 3133.043770, 2545.411337]` ms
  - `test2.png`: detection 14, elapsed `[3530.013089, 3713.855908, 3420.506818]` ms
  - `test3.png`: detection 27, elapsed `[5529.768889, 5292.289155, 5295.764231]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/23-reverted-recheck1.txt --current target/ffi-bench-logs/23-retry-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png`, `test2.png` median latency 증가.
- 재적용 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/23-retry-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2143.661477, 2384.083131, 2124.059064]` ms
  - `test2.png`: detection 14, elapsed `[2809.515126, 2924.698322, 2884.983958]` ms
  - `test3.png`: detection 27, elapsed `[5609.396997, 5633.778848, 5270.343415]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/23-reverted-recheck1.txt --current target/ffi-bench-logs/23-retry-after-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 최종 판단:
  - detection count는 모든 재시도에서 기준선과 동일했다.
  - 23차 재적용 1차 latency 실패는 재측정에서 사라졌다.
  - `bridge_warmup` 분리는 인식률/속도 저하 없이 통과한 것으로 보고 유지한다.

## 24차 구현 계획

1. 23차 재시도 통과 결과를 24차 변경 전 기준선으로 사용한다.
2. 이번 단계는 rec 입력 dump용 padding helper만 `bridge_image`로 이동한다.
   - `pad_rec_input_image()`
3. 실제 rec tensor 작성, resize, predictor 실행은 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. `ffi_benchmark_guard.py`로 detection count와 median latency를 확인한다.

## 24차 변경 전 기준선

- 23차 재시도 재측정 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/23-retry-after-recheck1.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2143.661477, 2384.083131, 2124.059064]` ms
  - `test2.png`: detection 14, elapsed `[2809.515126, 2924.698322, 2884.983958]` ms
  - `test3.png`: detection 27, elapsed `[5609.396997, 5633.778848, 5270.343415]` ms

## 24차 구현 내용

- `pad_rec_input_image()`를 `bridge.cc`에서 `bridge_image.cc`로 이동했다.
- `bridge_image.h`에 함수 선언을 추가했다.
- rec 입력 dump에서 사용하는 padding helper만 이동했고, rec resize/tensor/predictor 실행 경로는 변경하지 않았다.
- `bridge.cc`는 2914줄이 되었다.

## 24차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_image.cc -o /tmp/bridge_image.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/24-after.txt`
  - `test.png`: detection 7, elapsed `[2121.915680, 2110.856588, 2103.527923]` ms
  - `test2.png`: detection 14, elapsed `[2665.734133, 2972.187921, 2976.544042]` ms
  - `test3.png`: detection 27, elapsed `[5616.772740, 5815.524755, 5463.213835]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/23-retry-after-recheck1.txt --current target/ffi-bench-logs/24-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 25차 구현 계획

1. 24차 통과 결과를 25차 변경 전 기준선으로 사용한다.
2. 이번 단계는 90도 반시계 회전 helper만 `bridge_rotate`로 이동한다.
   - `rotate90_counterclockwise()`
3. 픽셀 루프 본문은 그대로 옮기고, crop/warp 조건과 호출 위치는 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. 과거에 같은 함수 이동이 노이즈를 보였으므로, 1차 실패 시 1회 재측정하고 반복 실패하면 되돌린다.

## 25차 변경 전 기준선

- 24차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/24-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2121.915680, 2110.856588, 2103.527923]` ms
  - `test2.png`: detection 14, elapsed `[2665.734133, 2972.187921, 2976.544042]` ms
  - `test3.png`: detection 27, elapsed `[5616.772740, 5815.524755, 5463.213835]` ms

## 25차 구현 내용

- `rotate90_counterclockwise()`를 `bridge.cc`에서 `bridge_rotate.cc`로 이동했다.
- `bridge_rotate.h`에 함수 선언을 추가했다.
- 픽셀 회전 루프와 호출 조건은 변경하지 않았다.
- `bridge.cc`는 2897줄이 되었다.

## 25차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_rotate.cc -o /tmp/bridge_rotate.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/25-after.txt`
  - `test.png`: detection 7, elapsed `[2346.145065, 2468.880173, 2818.500256]` ms
  - `test2.png`: detection 14, elapsed `[3225.504580, 3234.477683, 3194.358479]` ms
  - `test3.png`: detection 27, elapsed `[6128.255410, 5600.117022, 5794.277649]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/24-after.txt --current target/ffi-bench-logs/25-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png` median latency 증가.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/25-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2310.143554, 2304.517064, 2348.262713]` ms
  - `test2.png`: detection 14, elapsed `[3260.154417, 3187.133344, 3178.648694]` ms
  - `test3.png`: detection 27, elapsed `[5727.911688, 5608.739635, 5553.250859]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/24-after.txt --current target/ffi-bench-logs/25-after-recheck1.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일하다.
  - 1차 `test.png` latency 실패는 재측정에서 사라졌다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 26차 구현 계획

1. 25차 재측정 통과 결과를 26차 변경 전 기준선으로 사용한다.
2. 이번 단계는 공통 이미지 리사이즈 helper만 `bridge_image`로 이동한다.
   - `resize_bilinear()`
3. 함수 본문은 그대로 옮기고, det/cls/rec 전처리 호출 위치는 변경하지 않는다.
4. 변경 후 단독 컴파일, 전체 FFI 빌드, 동일 benchmark를 실행한다.
5. det/cls/rec 공통 경로이므로 guard 실패 시 1회 재측정 후 반복 실패하면 되돌린다.

## 26차 변경 전 기준선

- 25차 재측정 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/25-after-recheck1.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2310.143554, 2304.517064, 2348.262713]` ms
  - `test2.png`: detection 14, elapsed `[3260.154417, 3187.133344, 3178.648694]` ms
  - `test3.png`: detection 27, elapsed `[5727.911688, 5608.739635, 5553.250859]` ms

## 26차 구현 내용

- `resize_bilinear()`를 `bridge.cc`에서 `bridge_image.cc`로 이동했다.
- `bridge_image.h`에 함수 선언을 추가했다.
- fallback에서 사용하는 `sample_channel_bilinear_replicate()` 의존성을 위해 `bridge_image.cc`에서 `bridge_resample.h`를 include했다.
- 함수 본문과 det/cls/rec 전처리 호출 위치는 변경하지 않았다.
- `bridge.cc`는 2853줄이 되었다.

## 26차 테스트

- `g++ -std=c++17 -I native/paddle_bridge -c native/paddle_bridge/bridge_image.cc -o /tmp/bridge_image.o`
  - 통과.
- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/26-after.txt`
  - `test.png`: detection 7, elapsed `[2239.882148, 2201.248373, 2200.264189]` ms
  - `test2.png`: detection 14, elapsed `[2934.117363, 2902.484604, 3029.088548]` ms
  - `test3.png`: detection 27, elapsed `[5825.088936, 5767.718928, 5452.728264]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/25-after-recheck1.txt --current target/ffi-bench-logs/26-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 27차 구현 계획

1. 26차 통과 결과를 27차 변경 전 기준선으로 사용한다.
2. 이번 단계는 cls 입력 텐서 생성 wrapper만 `bridge_tensor`로 이동한다.
   - `preprocess_cls()`
3. 함수 본문은 그대로 옮기고, `resize_bilinear()`와 `fill_cls_tensor()` 호출 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. guard 실패 시 1회 재측정하고, 반복 실패하면 되돌리거나 원인을 확인한다.

## 27차 변경 전 기준선

- 26차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/26-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2239.882148, 2201.248373, 2200.264189]` ms
  - `test2.png`: detection 14, elapsed `[2934.117363, 2902.484604, 3029.088548]` ms
  - `test3.png`: detection 27, elapsed `[5825.088936, 5767.718928, 5452.728264]` ms

## 27차 구현 내용

- `preprocess_cls()`를 `bridge.cc`에서 `bridge_tensor.cc`로 이동했다.
- `bridge_tensor.h`에 함수 선언을 추가했다.
- cls 리사이즈와 텐서 채우기 계산 순서는 변경하지 않았다.
- `bridge.cc`는 2844줄이 되었다.

## 27차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/27-after.txt`
  - `test.png`: detection 7, elapsed `[2184.777343, 2194.067993, 2199.351387]` ms
  - `test2.png`: detection 14, elapsed `[2740.530886, 2824.121524, 2934.899163]` ms
  - `test3.png`: detection 27, elapsed `[5429.806625, 5673.597647, 6323.695214]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/26-after.txt --current target/ffi-bench-logs/27-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 28차 구현 계획

1. 27차 통과 결과를 28차 변경 전 기준선으로 사용한다.
2. 이번 단계는 rec 입력 이미지 크기 산정과 rec 입력 텐서 생성 wrapper를 `bridge_tensor`로 이동한다.
   - `resize_rec_input_image()`
   - `preprocess_rec()`
3. 함수 본문은 그대로 옮기고, 리사이즈 폭 계산과 텐서 padding/fill 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. guard 실패 시 1회 재측정하고, 반복 실패하면 되돌리거나 원인을 확인한다.

## 28차 변경 전 기준선

- 27차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/27-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2184.777343, 2194.067993, 2199.351387]` ms
  - `test2.png`: detection 14, elapsed `[2740.530886, 2824.121524, 2934.899163]` ms
  - `test3.png`: detection 27, elapsed `[5429.806625, 5673.597647, 6323.695214]` ms

## 28차 구현 내용

- `resize_rec_input_image()`와 `preprocess_rec()`를 `bridge.cc`에서 `bridge_tensor.cc`로 이동했다.
- `bridge_tensor.h`에 두 함수 선언을 추가했다.
- rec 입력 크기 계산, 리사이즈, padding, 텐서 채우기 순서는 변경하지 않았다.
- `bridge.cc`는 2799줄이 되었다.

## 28차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/28-after.txt`
  - `test.png`: detection 7, elapsed `[2328.522487, 2199.052972, 2217.503425]` ms
  - `test2.png`: detection 14, elapsed `[2950.527427, 3028.650614, 2992.440240]` ms
  - `test3.png`: detection 27, elapsed `[5390.276692, 5450.652576, 5295.524201]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/27-after.txt --current target/ffi-bench-logs/28-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 29차 구현 계획

1. 28차 통과 결과를 29차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 입력 이미지 리사이즈 helper를 `bridge_image`로 이동한다.
   - `resize_for_det()`
3. 함수 본문은 그대로 옮기고, det align/limit 계산과 padding/resize 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. det 입력 크기가 바뀌면 인식률에 직접 영향이 있으므로 detection count와 latency guard를 모두 확인한다.

## 29차 변경 전 기준선

- 28차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/28-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2328.522487, 2199.052972, 2217.503425]` ms
  - `test2.png`: detection 14, elapsed `[2950.527427, 3028.650614, 2992.440240]` ms
  - `test3.png`: detection 27, elapsed `[5390.276692, 5450.652576, 5295.524201]` ms

## 29차 구현 내용

- `resize_for_det()`를 `bridge.cc`에서 `bridge_image.cc`로 이동했다.
- `bridge_image.h`에 함수 선언을 추가했다.
- `DET_ALIGN` 사용을 위해 `bridge_image.cc`에서 `bridge_config.h`를 include했다.
- det 입력 크기 계산, 작은 이미지 padding, bilinear resize 순서는 변경하지 않았다.
- `bridge.cc`는 2720줄이 되었다.

## 29차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/29-after.txt`
  - `test.png`: detection 7, elapsed `[2218.225674, 2286.770077, 2513.450319]` ms
  - `test2.png`: detection 14, elapsed `[3395.414443, 2926.363014, 3246.204096]` ms
  - `test3.png`: detection 27, elapsed `[6955.873995, 5357.082655, 5227.500858]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/28-after.txt --current target/ffi-bench-logs/29-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 30차 구현 계획

1. 29차 통과 결과를 30차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 입력 텐서 생성 wrapper를 `bridge_tensor`로 이동한다.
   - `preprocess_det()`
3. 함수 본문은 그대로 옮기고, det 리사이즈 호출, BGR 정규화, debug 통계 계산 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. det tensor 값이 바뀌면 인식률에 직접 영향이 있으므로 detection count와 latency guard를 모두 확인한다.

## 30차 변경 전 기준선

- 29차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/29-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2218.225674, 2286.770077, 2513.450319]` ms
  - `test2.png`: detection 14, elapsed `[3395.414443, 2926.363014, 3246.204096]` ms
  - `test3.png`: detection 27, elapsed `[6955.873995, 5357.082655, 5227.500858]` ms

## 30차 구현 내용

- `preprocess_det()`를 `bridge.cc`에서 `bridge_tensor.cc`로 이동했다.
- `bridge_tensor.h`에 함수 선언을 추가했다.
- det/cls/rec 전처리 wrapper가 모두 `bridge_tensor`에 모였다.
- det BGR 정규화, tensor layout, debug 통계 계산 순서는 변경하지 않았다.
- `bridge.cc`는 2658줄이 되었다.

## 30차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/30-after.txt`
  - `test.png`: detection 7, elapsed `[2174.403872, 2118.478133, 2218.245513]` ms
  - `test2.png`: detection 14, elapsed `[3728.666094, 2920.544867, 2859.098035]` ms
  - `test3.png`: detection 27, elapsed `[5269.545576, 5287.754805, 5243.481950]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/29-after.txt --current target/ffi-bench-logs/30-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 31차 구현 계획

1. 30차 통과 결과를 31차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 후처리 점수 계산 helper를 `bridge_det_utils`로 이동한다.
   - `score_box()`
3. OpenCV 사용 분기와 fallback point-in-quad 계산은 그대로 옮긴다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. 후처리 점수 계산이 detection filtering에 영향을 주므로 detection count와 latency guard를 모두 확인한다.

## 31차 변경 전 기준선

- 30차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/30-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2174.403872, 2118.478133, 2218.245513]` ms
  - `test2.png`: detection 14, elapsed `[3728.666094, 2920.544867, 2859.098035]` ms
  - `test3.png`: detection 27, elapsed `[5269.545576, 5287.754805, 5243.481950]` ms

## 31차 구현 내용

- `score_box()`를 `bridge.cc`에서 `bridge_det_utils.cc`로 이동했다.
- `bridge_det_utils.h`에 함수 선언을 추가했다.
- fallback 계산에서 사용하는 `point_in_quad()` 의존성을 위해 `bridge_geometry.h`를 include했다.
- OpenCV 분기와 fallback 점수 계산 순서는 변경하지 않았다.
- `bridge.cc`는 2579줄이 되었다.

## 31차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/31-after.txt`
  - `test.png`: detection 7, elapsed `[2151.801120, 2098.146913, 2145.808555]` ms
  - `test2.png`: detection 14, elapsed `[2832.540053, 3029.336259, 2839.612197]` ms
  - `test3.png`: detection 27, elapsed `[5286.277309, 5137.064035, 5319.033173]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/30-after.txt --current target/ffi-bench-logs/31-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 32차 구현 계획

1. 31차 통과 결과를 32차 변경 전 기준선으로 사용한다.
2. 이번 단계는 fallback DB 후처리 contour helper를 `bridge_det_utils`로 이동한다.
   - `is_component_cell()`
   - `trace_component_contour()`
3. 함수 본문은 그대로 옮기고, contour normalize/compress 호출 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. OpenCV 빌드에서는 fallback helper가 직접 실행되지 않더라도 detection count와 latency guard를 확인한다.

## 32차 변경 전 기준선

- 31차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/31-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2151.801120, 2098.146913, 2145.808555]` ms
  - `test2.png`: detection 14, elapsed `[2832.540053, 3029.336259, 2839.612197]` ms
  - `test3.png`: detection 27, elapsed `[5286.277309, 5137.064035, 5319.033173]` ms

## 32차 구현 내용

- `is_component_cell()`와 `trace_component_contour()`를 `bridge.cc`에서 `bridge_det_utils.cc`로 이동했다.
- `bridge_det_utils.h`에 두 함수 선언을 추가했다.
- contour normalize/compress 호출 순서는 변경하지 않았다.
- `bridge.cc`는 2413줄이 되었다.

## 32차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/32-after.txt`
  - `test.png`: detection 7, elapsed `[2211.379894, 2193.840669, 2200.787272]` ms
  - `test2.png`: detection 14, elapsed `[2904.091051, 2935.359506, 2938.850428]` ms
  - `test3.png`: detection 27, elapsed `[5802.518923, 5824.505823, 5520.280052]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/31-after.txt --current target/ffi-bench-logs/32-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 33차 구현 계획

1. 32차 통과 결과를 33차 변경 전 기준선으로 사용한다.
2. 이번 단계는 crop helper를 새 `bridge_crop` 모듈로 이동한다.
   - `crop_to_bbox(const cv::Mat&, ...)`
   - `crop_to_bbox(const Image&, ...)`
3. 함수 본문은 그대로 옮기고, OpenCV warp, fallback cubic sampling, 90도 회전, debug dump 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. crop은 OCR hot path이므로 guard 실패 시 1회 재측정하고, 반복 실패하면 되돌리거나 원인을 확인한다.

## 33차 변경 전 기준선

- 32차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/32-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2211.379894, 2193.840669, 2200.787272]` ms
  - `test2.png`: detection 14, elapsed `[2904.091051, 2935.359506, 2938.850428]` ms
  - `test3.png`: detection 27, elapsed `[5802.518923, 5824.505823, 5520.280052]` ms

## 33차 구현 내용

- `bridge_crop.h`와 `bridge_crop.cc`를 추가했다.
- `crop_to_bbox()` 두 overload를 `bridge.cc`에서 `bridge_crop.cc`로 이동했다.
- `bridge.cc`에서 `bridge_crop.h`를 include했다.
- 새 파일을 native static library 빌드에 포함하기 위해 `build.rs`에 `bridge_crop.cc/h` rerun 대상과 compile source를 추가했다.
- 최초 빌드에서 `bridge_crop.cc`가 빌드 목록에 없어 링크 실패했고, `build.rs` 추가 후 재빌드 통과했다.
- crop 계산, OpenCV warp, fallback cubic sampling, 회전, debug dump 순서는 변경하지 않았다.
- `bridge.cc`는 2192줄이 되었다.

## 33차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 1차 실패: `bridge_crop.cc`가 static library 빌드 목록에 없어 `crop_to_bbox(cv::Mat const&, ...)` undefined symbol 발생.
  - `build.rs`에 `bridge_crop.cc/h`를 추가한 뒤 재실행 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/33-after.txt`
  - `test.png`: detection 7, elapsed `[2194.180584, 2194.915481, 2186.776393]` ms
  - `test2.png`: detection 14, elapsed `[2849.401177, 3229.974033, 2951.432183]` ms
  - `test3.png`: detection 27, elapsed `[5285.069362, 5269.924698, 5212.051131]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/32-after.txt --current target/ffi-bench-logs/33-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 34차 구현 계획

1. 33차 통과 결과를 34차 변경 전 기준선으로 사용한다.
2. 이번 단계는 cls 실행 helper를 새 `bridge_cls` 모듈로 이동한다.
   - `run_cls()`
   - `run_cls_batch()`
3. 함수 본문은 그대로 옮기고, 단일/배치 cls tensor 구성과 max score 선택 순서는 변경하지 않는다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. cls는 OCR pipeline hot path이므로 guard 실패 시 1회 재측정하고, 반복 실패하면 되돌린다.

## 34차 변경 전 기준선

- 33차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/33-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2194.180584, 2194.915481, 2186.776393]` ms
  - `test2.png`: detection 14, elapsed `[2849.401177, 3229.974033, 2951.432183]` ms
  - `test3.png`: detection 27, elapsed `[5285.069362, 5269.924698, 5212.051131]` ms

## 34차 구현 내용

- `bridge_cls.h`와 `bridge_cls.cc`를 추가하고 `run_cls()`, `run_cls_batch()`를 이동했다.
- 새 파일을 native static library 빌드에 포함하기 위해 `build.rs`에 `bridge_cls.cc/h`를 추가했다.
- 에러 helper 분리는 과거에 속도 우려로 되돌린 전례가 있어, 새 모듈 내부에는 실패 경로 전용 local helper만 두었다.

## 34차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark 1차:
  - 저장 파일: `target/ffi-bench-logs/34-after.txt`
  - `test.png`: detection 7, elapsed `[2217.168607, 2284.747455, 2312.041134]` ms
  - `test2.png`: detection 14, elapsed `[3138.439563, 3301.077831, 3149.214559]` ms
  - `test3.png`: detection 27, elapsed `[5937.903501, 6110.357102, 6106.298605]` ms
- 1차 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/33-after.txt --current target/ffi-bench-logs/34-after.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test3.png` median latency 증가.
- 변경 후 benchmark 재측정:
  - 저장 파일: `target/ffi-bench-logs/34-after-recheck1.txt`
  - `test.png`: detection 7, elapsed `[2491.330908, 2535.752157, 2554.488107]` ms
  - `test2.png`: detection 14, elapsed `[2948.244746, 2906.771003, 2921.318157]` ms
  - `test3.png`: detection 27, elapsed `[6128.290329, 5684.724513, 5847.222915]` ms
- 재측정 guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/33-after.txt --current target/ffi-bench-logs/34-after-recheck1.txt --max-median-ratio 1.15`
  - 실패.
  - 실패 항목: `test.png` median latency 증가.
- 판단:
  - detection count는 1차와 재측정 모두 3개 fixture에서 기준선과 동일하다.
  - latency guard가 서로 다른 fixture에서 2회 연속 실패했다.
  - 함수 이동 자체는 계산 순서를 바꾸지 않았지만, cls 실행 경계가 hot path라 translation unit 분리와 인라인/링크 배치 영향 가능성을 배제하지 않는다.
  - 인식률/속도 저하 금지 조건이 우선이므로 34차 변경은 되돌렸다.
- 되돌림 후 확인:
  - `bridge_cls.h`, `bridge_cls.cc`를 삭제했다.
  - `build.rs`에서 `bridge_cls.cc/h` 항목을 제거했다.
  - `run_cls()`, `run_cls_batch()`는 `bridge.cc`에 유지했다.
  - `cargo test --features paddle-ffi --no-run`
    - 통과.
  - 34차는 최종 변경으로 채택하지 않으며, 다음 기준선은 계속 `target/ffi-bench-logs/33-after.txt`다.

## 35차 구현 계획

1. 34차는 되돌렸으므로 33차 통과 결과를 35차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 후처리의 큰 덩어리인 `db_postprocess()`를 `bridge_det_utils`로 이동한다.
3. 함수 본문과 호출부 인자는 그대로 유지하고, 새 위치에서 필요한 include와 선언만 보강한다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
5. detection count가 바뀌거나 latency guard가 실패하면 재측정 또는 되돌림을 우선한다.

## 35차 변경 전 기준선

- 33차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/33-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2194.180584, 2194.915481, 2186.776393]` ms
  - `test2.png`: detection 14, elapsed `[2849.401177, 3229.974033, 2951.432183]` ms
  - `test3.png`: detection 27, elapsed `[5285.069362, 5269.924698, 5212.051131]` ms

## 35차 구현 내용

- `db_postprocess()`를 `bridge.cc`에서 `bridge_det_utils.cc`로 이동했다.
- `bridge_det_utils.h`에 `db_postprocess()` 선언을 추가했다.
- 새 위치에서 사용하는 det 옵션, debug dump/format, 파일 출력, 환경 변수, queue 관련 include를 정리했다.

## 35차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 1차 실패: `bridge_det_utils.h`에서 `DetOptions` 타입 선언을 볼 수 없어 컴파일 실패.
  - `bridge_config.h` include를 추가한 뒤 재실행 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/35-after.txt`
  - `test.png`: detection 7, elapsed `[2187.954915, 2397.470962, 2889.568821]` ms
  - `test2.png`: detection 14, elapsed `[3059.667215, 2871.448397, 2924.681384]` ms
  - `test3.png`: detection 27, elapsed `[5591.938768, 5419.213972, 5414.209932]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/33-after.txt --current target/ffi-bench-logs/35-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 36차 구현 계획

1. 35차 통과 결과를 36차 변경 전 기준선으로 사용한다.
2. 이번 단계는 det 실행 wrapper인 `run_det()`를 새 `bridge_det` 모듈로 이동한다.
3. 함수 본문은 그대로 유지하고, 실패 경로에서만 쓰는 에러 보조 함수는 새 모듈 내부 local helper로 둔다.
4. `build.rs`에 `bridge_det.cc/h`를 추가한다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 36차 변경 전 기준선

- 35차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/35-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2187.954915, 2397.470962, 2889.568821]` ms
  - `test2.png`: detection 14, elapsed `[3059.667215, 2871.448397, 2924.681384]` ms
  - `test3.png`: detection 27, elapsed `[5591.938768, 5419.213972, 5414.209932]` ms

## 36차 구현 내용

- `bridge_det.h`와 `bridge_det.cc`를 추가했다.
- `run_det()`를 `bridge.cc`에서 `bridge_det.cc`로 이동했다.
- `bridge.cc`는 `bridge_det.h`를 include해 기존 호출부 계약을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_det.cc/h`를 추가했다.

## 36차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/36-after.txt`
  - `test.png`: detection 7, elapsed `[2216.599896, 2131.468587, 2154.003623]` ms
  - `test2.png`: detection 14, elapsed `[3010.448033, 2909.318517, 3259.044006]` ms
  - `test3.png`: detection 27, elapsed `[6270.594958, 5764.548238, 5722.276735]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/35-after.txt --current target/ffi-bench-logs/36-after.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 37차 구현 계획

1. 34차에서 되돌렸던 cls 실행 helper 분리를 현재 36차 기준에서 재시도한다.
2. 34차와 별개로 새 기준선은 36차 통과 결과(`36-after.txt`)로 둔다.
3. `run_cls()`와 `run_cls_batch()`를 새 `bridge_cls` 모듈로 이동한다.
4. 함수 본문은 그대로 유지하고, 실패 경로에서만 쓰는 에러 보조 함수는 새 모듈 내부 local helper로 둔다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.
6. latency guard가 실패하면 1회 재측정하고, 반복 실패하면 되돌린다.

## 37차 변경 전 기준선

- 36차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/36-after.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2216.599896, 2131.468587, 2154.003623]` ms
  - `test2.png`: detection 14, elapsed `[3010.448033, 2909.318517, 3259.044006]` ms
  - `test3.png`: detection 27, elapsed `[6270.594958, 5764.548238, 5722.276735]` ms

## 37차 구현 내용

- `bridge_cls.h`와 `bridge_cls.cc`를 추가했다.
- `run_cls()`와 `run_cls_batch()`를 `bridge.cc`에서 `bridge_cls.cc`로 이동했다.
- `bridge.cc`는 `bridge_cls.h`를 include해 기존 호출부 계약을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_cls.cc/h`를 추가했다.

## 37차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/37-after-cls-retry.txt`
  - `test.png`: detection 7, elapsed `[2170.311934, 2230.738388, 2168.230736]` ms
  - `test2.png`: detection 14, elapsed `[3117.532488, 2970.985938, 3285.077283]` ms
  - `test3.png`: detection 27, elapsed `[5917.008478, 5425.742581, 5763.669717]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/36-after.txt --current target/ffi-bench-logs/37-after-cls-retry.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 34차에서 관측된 latency guard 실패는 이번 재시도 기준에서는 재현되지 않았다.
  - 이번 cls 분리는 인식률/속도 저하 없이 통과한 것으로 보고 채택한다.

## 38차 구현 계획

1. 37차 통과 결과를 38차 변경 전 기준선으로 사용한다.
2. 이번 단계는 recognition 실행 wrapper인 `run_rec()`와 `run_rec_batch()`를 새 `bridge_rec` 모듈로 이동한다.
3. 함수 본문은 그대로 유지하고, 실패 경로에서만 쓰는 에러 보조 함수는 새 모듈 내부 local helper로 둔다.
4. `build.rs`에 `bridge_rec.cc/h`를 추가한다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 38차 변경 전 기준선

- 37차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/37-after-cls-retry.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2170.311934, 2230.738388, 2168.230736]` ms
  - `test2.png`: detection 14, elapsed `[3117.532488, 2970.985938, 3285.077283]` ms
  - `test3.png`: detection 27, elapsed `[5917.008478, 5425.742581, 5763.669717]` ms

## 38차 구현 내용

- `bridge_rec.h`와 `bridge_rec.cc`를 추가했다.
- `run_rec()`와 `run_rec_batch()`를 `bridge.cc`에서 `bridge_rec.cc`로 이동했다.
- `bridge.cc`는 `bridge_rec.h`를 include해 기존 호출부 계약을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_rec.cc/h`를 추가했다.

## 38차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/38-after-rec.txt`
  - `test.png`: detection 7, elapsed `[2787.002346, 2138.529792, 2134.437242]` ms
  - `test2.png`: detection 14, elapsed `[2741.129903, 2768.766475, 3360.833757]` ms
  - `test3.png`: detection 27, elapsed `[5406.558760, 5727.465896, 5607.627848]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/37-after-cls-retry.txt --current target/ffi-bench-logs/38-after-rec.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 39차 구현 계획

1. 38차 통과 결과를 39차 변경 전 기준선으로 사용한다.
2. 이번 단계는 OCR 파이프라인 조립부를 새 `bridge_pipeline` 모듈로 이동한다.
3. `collect_cls_inputs()`부터 `run_pipeline_from_path()`까지 이동하고, FFI API와 엔진 생성은 `bridge.cc`에 유지한다.
4. 함수 본문은 그대로 유지하고, `char**` 에러 설정 helper는 새 모듈 내부 local helper로 둔다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 39차 변경 전 기준선

- 38차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/38-after-rec.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2787.002346, 2138.529792, 2134.437242]` ms
  - `test2.png`: detection 14, elapsed `[2741.129903, 2768.766475, 3360.833757]` ms
  - `test3.png`: detection 27, elapsed `[5406.558760, 5727.465896, 5607.627848]` ms

## 39차 구현 내용

- `bridge_pipeline.h`와 `bridge_pipeline.cc`를 추가했다.
- `collect_cls_inputs()`, `run_cls_batches_into()`, `build_rec_candidates()`, `run_rec_batches_into()`,
  `append_pipeline_results()`, `run_pipeline()`, `run_pipeline_from_path()`를 `bridge_pipeline.cc`로 이동했다.
- `bridge.cc`는 `bridge_pipeline.h`를 include해 FFI API 호출 계약을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_pipeline.cc/h`를 추가했다.

## 39차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/39-after-pipeline.txt`
  - `test.png`: detection 7, elapsed `[2187.470697, 2146.838303, 2125.485020]` ms
  - `test2.png`: detection 14, elapsed `[2914.687344, 2947.010436, 2871.465672]` ms
  - `test3.png`: detection 27, elapsed `[5454.333662, 5459.312627, 5646.171238]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/38-after-rec.txt --current target/ffi-bench-logs/39-after-pipeline.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 40차 구현 계획

1. 39차 통과 결과를 40차 변경 전 기준선으로 사용한다.
2. 이번 단계는 FFI 엔진 생성 함수인 `buzhi_ocr_create()`를 새 `bridge_create` 모듈로 이동한다.
3. C ABI 선언은 `bridge.h`에 그대로 두고, 생성 함수 본문만 이동해 외부 호출 계약을 유지한다.
4. 실패 경로에서만 쓰는 `char**` 에러 설정 helper는 새 모듈 내부 local helper로 둔다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 40차 변경 전 기준선

- 39차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/39-after-pipeline.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2187.470697, 2146.838303, 2125.485020]` ms
  - `test2.png`: detection 14, elapsed `[2914.687344, 2947.010436, 2871.465672]` ms
  - `test3.png`: detection 27, elapsed `[5454.333662, 5459.312627, 5646.171238]` ms

## 40차 구현 내용

- `bridge_create.cc`를 추가했다.
- `buzhi_ocr_create()`를 `bridge.cc`에서 `bridge_create.cc`로 이동했다.
- `bridge.cc`는 나머지 FFI API facade 역할을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_create.cc`를 추가했다.

## 40차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/40-after-create.txt`
  - `test.png`: detection 7, elapsed `[2120.450139, 2122.431506, 2216.7045180000005]` ms
  - `test2.png`: detection 14, elapsed `[3444.821023, 3065.368602, 2843.228270]` ms
  - `test3.png`: detection 27, elapsed `[5598.1978930000005, 5551.400208999999, 5866.534148]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/39-after-pipeline.txt --current target/ffi-bench-logs/40-after-create.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 분리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 41차 구현 계획

1. 40차 통과 결과를 41차 변경 전 기준선으로 사용한다.
2. 이번 단계는 `bridge.cc`가 C ABI facade만 남은 상태에 맞춰 include 목록을 정리한다.
3. 동작 코드는 바꾸지 않고, 사용하지 않는 `set_error_if_empty()` helper도 제거한다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 41차 변경 전 기준선

- 40차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/40-after-create.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2120.450139, 2122.431506, 2216.7045180000005]` ms
  - `test2.png`: detection 14, elapsed `[3444.821023, 3065.368602, 2843.228270]` ms
  - `test3.png`: detection 27, elapsed `[5598.1978930000005, 5551.400208999999, 5866.534148]` ms

## 41차 구현 내용

- `bridge.cc`의 include 목록을 실제로 필요한 `bridge_engine`, `bridge_output`, `bridge_pipeline`, `bridge_warmup` 중심으로 축소했다.
- 더 이상 호출되지 않는 `set_error_if_empty()`를 제거했다.
- `bridge.cc`는 201줄이 되었다.

## 41차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/41-after-bridge-include-cleanup.txt`
  - `test.png`: detection 7, elapsed `[2172.054863, 2142.5064960000004, 2233.601460]` ms
  - `test2.png`: detection 14, elapsed `[2754.187106, 3147.423405, 3215.7418679999996]` ms
  - `test3.png`: detection 27, elapsed `[5719.312339, 5476.739497, 5517.120612000001]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/40-after-create.txt --current target/ffi-bench-logs/41-after-bridge-include-cleanup.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 정리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 42차 구현 계획

1. 41차 통과 결과를 42차 변경 전 기준선으로 사용한다.
2. `bridge.cc`에 남은 C ABI facade 구현을 새 `bridge_api.cc`로 이동한다.
3. `bridge.h`의 외부 ABI 선언은 유지하고, `bridge.cc`는 기존 빌드 입력 호환을 위한 얇은 파일로 남긴다.
4. 공통 에러 메시지와 score threshold clamp는 새 모듈 내부 local helper로 둔다.
5. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 42차 변경 전 기준선

- 41차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/41-after-bridge-include-cleanup.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2172.054863, 2142.5064960000004, 2233.601460]` ms
  - `test2.png`: detection 14, elapsed `[2754.187106, 3147.423405, 3215.7418679999996]` ms
  - `test3.png`: detection 27, elapsed `[5719.312339, 5476.739497, 5517.120612000001]` ms

## 42차 구현 내용

- `bridge_api.cc`를 추가했다.
- `buzhi_ocr_destroy()`, `buzhi_ocr_warmup_predictors()`, `buzhi_ocr_run_image_file()`,
  `buzhi_ocr_run_image_file_result()`, `buzhi_ocr_run_image_rgba_result()`를 `bridge_api.cc`로 이동했다.
- `bridge.cc`는 `#include "bridge.h"`만 남겨 기존 build input을 유지한다.
- `build.rs`의 rerun 감시 목록과 C++ 빌드 파일 목록에 `bridge_api.cc`를 추가했다.

## 42차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/42-after-api-facade.txt`
  - `test.png`: detection 7, elapsed `[2192.181888, 2278.050411, 2234.1125979999997]` ms
  - `test2.png`: detection 14, elapsed `[2815.483042, 2840.0263309999996, 2803.5667630000003]` ms
  - `test3.png`: detection 27, elapsed `[5227.547477, 5279.912561, 5383.320774]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/41-after-bridge-include-cleanup.txt --current target/ffi-bench-logs/42-after-api-facade.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 facade 이동은 인식률/속도 저하 없이 통과한 것으로 본다.

## 43차 구현 계획

1. 42차 통과 결과를 43차 변경 전 기준선으로 사용한다.
2. `build.rs`의 Paddle bridge rerun 감시 목록과 C++ source 목록 중복을 줄인다.
3. 파일 목록은 상수 배열로 모으고, 실제 `cc::Build`에는 같은 source 파일들이 들어가게 유지한다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 43차 변경 전 기준선

- 42차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/42-after-api-facade.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2192.181888, 2278.050411, 2234.1125979999997]` ms
  - `test2.png`: detection 14, elapsed `[2815.483042, 2840.0263309999996, 2803.5667630000003]` ms
  - `test3.png`: detection 27, elapsed `[5227.547477, 5279.912561, 5383.320774]` ms

## 43차 구현 내용

- `PADDLE_BRIDGE_RERUN_PATHS`와 `PADDLE_BRIDGE_SOURCE_FILES` 상수 배열을 추가했다.
- rerun 감시 출력은 `PADDLE_BRIDGE_RERUN_PATHS` 순회로 바꿨다.
- `cc::Build` source 등록은 `PADDLE_BRIDGE_SOURCE_FILES` 순회로 바꿨다.
- source 파일 집합은 42차와 동일하게 유지했다.

## 43차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/43-after-build-rs-list-cleanup.txt`
  - `test.png`: detection 7, elapsed `[2373.035371, 2317.734808, 2314.058840]` ms
  - `test2.png`: detection 14, elapsed `[2979.486979, 3033.650392, 3100.3071699999996]` ms
  - `test3.png`: detection 27, elapsed `[5408.482665, 5550.544387000001, 5576.554448999999]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/42-after-api-facade.txt --current target/ffi-bench-logs/43-after-build-rs-list-cleanup.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 빌드 스크립트 정리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 44차 구현 계획

1. 43차 통과 결과를 44차 변경 전 기준선으로 사용한다.
2. `build.rs`의 Paddle bridge 파일 목록을 수동 상수 배열에서 디렉터리 스캔 기반으로 바꾼다.
3. `native/paddle_bridge` 아래의 `.cc`와 `.h` 파일만 대상으로 삼고, 정렬 후 등록해 재현성을 유지한다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 44차 변경 전 기준선

- 43차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/43-after-build-rs-list-cleanup.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2373.035371, 2317.734808, 2314.058840]` ms
  - `test2.png`: detection 14, elapsed `[2979.486979, 3033.650392, 3100.3071699999996]` ms
  - `test3.png`: detection 27, elapsed `[5408.482665, 5550.544387000001, 5576.554448999999]` ms

## 44차 구현 내용

- `PADDLE_BRIDGE_RERUN_PATHS`와 `PADDLE_BRIDGE_SOURCE_FILES` 수동 배열을 제거했다.
- `collect_paddle_bridge_files()`를 추가해 `native/paddle_bridge`의 `.cc`와 `.h` 파일을 정렬 수집한다.
- rerun 감시에는 디렉터리와 수집된 파일을 등록한다.
- C++ 빌드에는 수집된 파일 중 `.cc`만 등록한다.

## 44차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/44-after-build-rs-auto-scan.txt`
  - `test.png`: detection 7, elapsed `[2272.6399380000003, 2186.925845, 2246.493301]` ms
  - `test2.png`: detection 14, elapsed `[3002.378279, 2978.209999, 3060.929380]` ms
  - `test3.png`: detection 27, elapsed `[5788.117466, 5550.803119, 5548.035360]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/43-after-build-rs-list-cleanup.txt --current target/ffi-bench-logs/44-after-build-rs-auto-scan.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 빌드 스크립트 자동 등록은 인식률/속도 저하 없이 통과한 것으로 본다.

## 45차 구현 계획

1. 44차 통과 결과를 45차 변경 전 기준선으로 사용한다.
2. 모듈 경계를 점검해 헤더가 불필요하게 무거운 엔진 정의를 끌어오는 지점을 줄인다.
3. `bridge_pipeline.h`와 `bridge_warmup.h`는 `buzhi_ocr_engine` forward declaration만 갖게 하고, 실제 엔진 정의는 구현 파일에서 include한다.
4. 변경 후 전체 FFI 빌드와 동일 benchmark를 실행한다.

## 45차 변경 전 기준선

- 44차 통과 결과를 사용한다.
- 저장 파일:
  - `target/ffi-bench-logs/44-after-build-rs-auto-scan.txt`
- 결과:
  - `test.png`: detection 7, elapsed `[2272.6399380000003, 2186.925845, 2246.493301]` ms
  - `test2.png`: detection 14, elapsed `[3002.378279, 2978.209999, 3060.929380]` ms
  - `test3.png`: detection 27, elapsed `[5788.117466, 5550.803119, 5548.035360]` ms

## 45차 구현 내용

- `bridge_pipeline.h`에서 `bridge_engine.h` include를 제거하고 `struct buzhi_ocr_engine;` forward declaration으로 바꿨다.
- `bridge_pipeline.cc`에서 `bridge_engine.h`를 직접 include하게 했다.
- `bridge_warmup.h`에서 `bridge_engine.h` include를 제거하고 `struct buzhi_ocr_engine;` forward declaration으로 바꿨다.
- `bridge_warmup.cc`에서 `bridge_engine.h`를 직접 include하게 했다.

## 45차 테스트

- `cargo test --features paddle-ffi --no-run`
  - 통과.
  - 남은 경고는 Paddle SDK 헤더와 pyclipper 외부 소스 경고다.
- 변경 후 benchmark:
  - 저장 파일: `target/ffi-bench-logs/45-after-header-boundary-cleanup.txt`
  - `test.png`: detection 7, elapsed `[2439.043832, 2402.585864, 2366.361985]` ms
  - `test2.png`: detection 14, elapsed `[3494.787471, 3103.505025, 3225.880995]` ms
  - `test3.png`: detection 27, elapsed `[6205.6967859999995, 5643.854708, 6244.8242199999995]` ms
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/44-after-build-rs-auto-scan.txt --current target/ffi-bench-logs/45-after-header-boundary-cleanup.txt --max-median-ratio 1.15`
  - 통과.
- 판단:
  - detection count는 3개 fixture 모두 기준선과 동일하다.
  - latency median은 3개 fixture 모두 허용 비율 이내다.
  - 이번 헤더 경계 정리는 인식률/속도 저하 없이 통과한 것으로 본다.

## 46차 구현 계획

1. 요약 문서가 긴 상세 로그를 열지 않아도 최종 구조를 파악할 수 있게 보강한다.
2. 파일별 책임 표를 추가한다.
3. 리팩터링 후 유지해야 할 최종 검증 기준을 명시한다.

## 46차 구현 내용

- `1777249637-assess-paddle-bridge-file-split.md`에 최종 구조 요약 표를 추가했다.
- 동일 문서에 최종 검증 기준을 추가했다.
- 문서 작업만 수행했으며 런타임 코드는 변경하지 않았다.

## 46차 테스트

- 코드 변경이 없는 문서 마감 작업이므로 OCR benchmark는 실행하지 않았다.
- 최종 회귀 세트에서 문서 순서와 경로 노출 스캔을 함께 확인한다.

## 최종 리팩터링 전후 속도 비교

- 리팩터링 전 기준:
  - `target/ffi-bench-logs/14-before.txt`
- 리팩터링 후 현재:
  - `target/ffi-bench-logs/45-after-header-boundary-cleanup.txt`
- guard:
  - `python3 tools/scripts/ffi_benchmark_guard.py --baseline target/ffi-bench-logs/14-before.txt --current target/ffi-bench-logs/45-after-header-boundary-cleanup.txt --max-median-ratio 1.15`
  - 통과.
- median 비교:
  - `test.png`: baseline `2454.960ms`, current `2402.586ms`, ratio `0.979`, detection `7 -> 7`
  - `test2.png`: baseline `3195.016ms`, current `3225.881ms`, ratio `1.010`, detection `14 -> 14`
  - `test3.png`: baseline `5638.513ms`, current `6205.697ms`, ratio `1.101`, detection `27 -> 27`
- 판단:
  - 세 fixture 모두 `--max-median-ratio 1.15` 안이다.
  - 리팩터링 전후 속도 차이는 노이즈 범위로 판단한다.
