# 앱 OCR 실제 경로 stage 계측 추가

## 배경

- 사용자는 벤치 결과와 달리 실제 프로그램 실행에서는 Python sidecar가 더 빠르게 느껴진다고 보고했다.
- 현재 비교 스크립트 벤치는 OCR 엔진 중심의 시간을 본다.
- 반면 실제 앱 체감은 캡처, 이미지 준비, 임시 파일 저장, OCR 호출, 결과 emit까지 포함될 수 있다.
- 따라서 실제 앱 OCR 경로에 stage 단위 계측을 넣어 어디서 시간이 쓰이는지 직접 확인해야 한다.

## 구현 계획

1. 앱 OCR 런타임 경로에서 계측이 필요한 stage를 정의한다.
2. `capture`, `spawn_blocking 대기`, `resize`, `temp BMP 저장`, `FFI 호출`, `emit` 시간을 로그로 남긴다.
3. 계측 포맷은 해석하기 쉬운 고정 문자열로 통일한다.
4. 핵심 계측 문자열 생성 로직은 테스트 가능하게 분리하고 단위 테스트를 추가한다.
5. 실행 후 로그를 읽는 방법과 기대 포맷을 문서에 정리한다.

## 구현

- `src/lib.rs`의 `handle_prtsc()`에 실제 앱 OCR 경로 계측을 추가했다.
- 다음 stage를 로그로 남긴다.
  - `capture_ms`: 스크린샷 캡처 완료까지
  - `spawn_wait_ms`: `spawn_blocking(run_ocr)` 전체 대기 시간
  - `emit_ms`: OCR 결과 emit 시간
  - `total_ms`: 핫키 처리 한 사이클 전체 시간
- `src/ocr/mod.rs`에는 백엔드 단계 계측을 추가했다.
  - `prepare_image_ms`: FFI 입력용 RGBA 준비 시간
  - `ffi_ms`: FFI OCR 호출 시간
- 로그 포맷은 `[OCR_STAGE] ...` 접두사로 통일했다.
- 앱/백엔드 로그 문자열 생성 함수는 각각 테스트로 고정했다.
- `tools/scripts/ocr_sidecar_ffi.py`에는 `--ffi-mode pipeline`을 추가해 GUI 없이 `run_ocr()` 경로까지 포함한 비교를 할 수 있게 했다.
- `pipeline` 모드에서는 sidecar 입력도 앱과 동일하게 `1024h` 규칙으로 전처리해, 실제 앱 경로와 다른 입력을 넣어 생기던 parity 분기를 막았다.
- FFI 실제 앱 경로의 고정 비용을 줄이기 위해 임시 OCR 파일을 없애고, Rust 메모리 RGBA 버퍼를 네이티브 브리지로 직접 넘기도록 바꿨다.
- `CaptureInfo.image`를 공유형으로 바꿔 hotkey 경로에서 `pending_capture` 보관과 OCR 실행이 같은 캡처 이미지를 복제 없이 함께 쓰게 했다.
- `run_ocr()`는 입력 이미지를 빌림으로 받아, 리사이즈가 필요할 때만 새 이미지를 만들도록 정리했다.
- 평상시 `debug_trace=false`일 때는 빈 `debug_detections`를 직렬화하지 않게 해 emit payload를 줄였다.

## 로그 예시

```text
[OCR_STAGE] app phase=capture_hotkey capture_ms=12 spawn_wait_ms=345 emit_ms=6
[OCR_STAGE] app phase=capture_hotkey total_ms=370
[OCR_STAGE] backend image=1919x1024 prepare_image_ms=18 ffi_ms=321
```

## 해석 기준

- `capture_ms`가 크면 화면 캡처 단계가 병목일 가능성이 크다.
- `spawn_wait_ms`가 크고 `ffi_ms`가 비슷하면 OCR 스레드 대기 또는 앱 내부 준비 비용을 의심한다.
- `prepare_image_ms`가 크면 FFI 입력 버퍼 준비 비용이 체감 지연의 원인일 수 있다.
- `emit_ms`가 크면 OCR 자체보다 UI 전달 쪽 비용을 먼저 봐야 한다.

## 검증

- `cargo test --lib -- --nocapture`
- 결과: `82 passed; 0 failed`
- `python -m py_compile tools/scripts/ocr_sidecar_ffi.py`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format bmp --ffi-format bmp --ffi-mode pipeline`
- 결과: `test3.png 18/18 exact match`, `test4.png 37/37 exact match`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format bmp --ffi-format bmp --ffi-mode pipeline`
- 결과: `test3.png`에서는 sidecar가 더 빠르고, `test4.png`에서는 ffi가 더 빠르다.
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
- 결과: `test.png 7/7`, `test2.png 14/14`, `test3.png 18/18`, `test4.png 37/37 exact match`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`
- 결과: 네 fixture 모두에서 ffi가 sidecar보다 더 빨랐다.
- 실제 오버레이가 쓰는 방식에 맞춰 결과 payload를 polygon 대신 bounds 중심으로 줄인 뒤에는, `run_ocr()` 이후 payload 직렬화까지 포함한 기준에서도 네 fixture 모두 ffi가 더 빨랐다.
  - `test.png`: sidecar `2035.27ms`, ffi `1881.15ms`
  - `test2.png`: sidecar `2430.06ms`, ffi `2410.16ms`
  - `test3.png`: sidecar `3774.57ms`, ffi `3360.86ms`
  - `test4.png`: sidecar `6472.43ms`, ffi `6385.55ms`
