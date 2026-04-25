# 배경

- FFI가 워밍업된 모델을 재사용하고 있음에도 실제 프로그램 경로에서는 sidecar 대비 이점이 제한적으로 보였다.
- 현재 병목 후보는 세 군데였다.
  - Rust `DynamicImage -> RGBA8` 변환에서 불필요한 복사가 발생한다.
  - `1024h` 축소에 `Lanczos3`를 사용해 OCR 전처리 대비 과한 CPU 비용을 지불한다.
  - Rust 쪽 엔진 상태 락이 이미 전체 FFI 호출을 직렬화하는데, 네이티브 브리지에서도 `run_mutex`로 다시 직렬화하고 있다.

# 이번 작업 목표

1. FFI 입력 준비 과정의 불필요한 메모리 복사를 줄인다.
2. OCR 전처리 리사이즈 비용을 줄이되 parity는 유지한다.
3. 중복 락을 제거해 FFI 호출 경로의 고정비를 낮춘다.

# 구현 계획

1. `src/ocr/mod.rs`
   - 이미지가 이미 `RGBA8`면 버퍼를 복사하지 않고 그대로 FFI에 전달한다.
   - 다른 포맷일 때만 `to_rgba8()`로 변환한다.
2. `src/services/ocr_pipeline.rs`
   - `1024h` 축소 필터를 더 가벼운 필터로 조정한다.
   - 관련 테스트를 유지하고 parity 벤치로 결과를 다시 확인한다.
3. `native/paddle_bridge/bridge.cc`
   - `run_mutex` 사용 지점을 제거한다.
   - Rust `PaddleFfiEngine.state` 락이 이미 호출 전체를 보호한다는 전제에 맞춰 중복 직렬화를 없앤다.

# 검증 계획

- `cargo test --lib -- --nocapture`
- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
- `python tools/scripts/ocr_sidecar_ffi.py compare ... --ffi-mode pipeline`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark ... --ffi-mode pipeline`

# 구현 후 기록

## 적용 내용

1. `src/ocr/mod.rs`
   - `prepared_rgba_image()`를 추가했다.
   - 입력 이미지가 이미 `ImageRgba8`면 `to_rgba8()`로 새 버퍼를 만들지 않고 그대로 빌려서 FFI에 넘긴다.
   - 이 경로를 보장하는 단위 테스트 `rgba8_이미지는_복사하지_않고_그대로_재사용한다`를 추가했다.
2. `src/services/ocr_pipeline.rs`
   - `1024h` 축소 필터를 `Lanczos3`에서 `Triangle`로 낮췄다.
   - OCR 전처리용 축소 비용을 줄이되, parity는 fixture 비교로 다시 확인했다.
3. `native/paddle_bridge/bridge.cc`
   - 네이티브 `run_mutex`와 세 군데의 잠금 지점을 제거했다.
   - Rust `PaddleFfiEngine.state` 락이 이미 FFI 호출 전체를 직렬화하고 있어서, 네이티브 락은 중복이었다.
4. `tools/scripts/ocr_sidecar_ffi.py`
   - pipeline compare 모드에서 앱 payload가 polygon이 아니라 bounds를 내보내는 현재 형식에 맞춰 정규화 로직을 추가했다.

## 검증 결과

- `cargo test --lib -- --nocapture`
  - `83 passed; 0 failed`
- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
  - 통과
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: `7/7 exact match`
  - `test2.png`: `14/14 exact match`
  - `test3.png`: `18/18 exact match`
  - `test4.png`: `37/37 exact match`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`
  - `test.png`: sidecar `2227.39ms`, ffi `1872.43ms`
  - `test2.png`: sidecar `3013.65ms`, ffi `2315.53ms`
  - `test3.png`: sidecar `3857.15ms`, ffi `3296.14ms`
  - `test4.png`: sidecar `7911.59ms`, ffi `6535.47ms`

## 결론

- parity는 네 fixture 모두 유지됐다.
- 이번 변경 이후 pipeline 기준 측정에서는 네 fixture 모두 ffi가 더 빨랐다.
