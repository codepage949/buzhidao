# OCR 경로의 불필요한 성능 저하 요소 제거

## 배경

- OCR 엔진 자체는 FFI가 대체로 빠른데, 실제 앱 경로에서는 준비 비용과 결과 전달 비용 때문에 이점이 줄어들 수 있었다.
- 특히 임시 이미지 파일 저장, 큰 스크린샷 복제, 오버레이에 실제로 쓰지 않는 polygon payload 직렬화가 불필요한 고정 비용으로 남아 있었다.
- 이번 작업에서는 OCR 품질을 유지하면서 이 고정 비용을 줄이는 데 집중했다.

## 구현 계획

1. FFI 입력 경로에서 임시 파일 저장과 재로딩 비용을 제거한다.
2. hotkey 이후 OCR 실행 경로에서 큰 이미지 복제를 줄인다.
3. 결과 emit 단계에서 실제 UI가 쓰지 않는 데이터를 걷어낸다.
4. sidecar 대비 parity와 지연시간을 다시 확인한다.

## 구현

- Rust 메모리의 RGBA 버퍼를 네이티브 브리지로 직접 넘기도록 바꿔, 임시 OCR 이미지 파일 저장과 재로드를 제거했다.
- `CaptureInfo.image`를 공유형으로 바꿔 `pending_capture` 보관과 OCR 실행이 같은 스크린샷 데이터를 함께 쓰게 했다.
- `run_ocr()`는 입력 이미지를 소유하지 않고 빌림으로 받아, 리사이즈가 필요할 때만 새 이미지를 만들게 정리했다.
- OCR 결과를 Rust 내부에서부터 bounds 중심 구조로 바꿔, polygon 벡터 할당과 emit 직전 bounds 재변환을 제거했다.
- `debug_trace=false`일 때는 빈 `debug_detections`를 emit하지 않게 해 불필요한 payload를 더 줄였다.
- stage 계측과 backend 처리 로그는 기본 비활성으로 바꾸고, `BUZHIDAO_OCR_STAGE_LOG=1`일 때만 켜지게 했다.

## 기대 효과

- FFI 경로의 고정 준비 비용이 줄어든다.
- 큰 스크린샷에서 hotkey 이후 OCR 시작 전 복제 비용이 줄어든다.
- OCR 결과 emit과 오버레이 반영 시 직렬화 및 후처리 비용이 줄어든다.
- 평상시 실행에서 불필요한 `stderr` I/O 비용이 사라진다.

## 검증

- `cargo test --lib -- --nocapture`
- 결과: `82 passed; 0 failed`
- `python tools/scripts/ocr_sidecar_ffi.py compare --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --sidecar-format png --ffi-format png --ffi-mode pipeline`
- 결과: `test.png 7/7`, `test2.png 14/14`, `test3.png 18/18`, `test4.png 37/37 exact match`
- `python tools/scripts/ocr_sidecar_ffi.py benchmark --image testdata/ocr/test.png --image testdata/ocr/test2.png --image testdata/ocr/test3.png --image testdata/ocr/test4.png --source ch --score-thresh 0.1 --warmups 1 --iterations 3 --sidecar-format png --ffi-format png --ffi-mode pipeline`
- 결과: 결과 payload 직렬화까지 포함한 기준에서 `test.png`, `test3.png`, `test4.png`는 ffi가 더 빨랐고, `test2.png`는 sidecar가 약간 더 빨랐다.
  - `test.png`: sidecar `2107.67ms`, ffi `1901.79ms`
  - `test2.png`: sidecar `2327.71ms`, ffi `2388.89ms`
  - `test3.png`: sidecar `3366.95ms`, ffi `3342.31ms`
  - `test4.png`: sidecar `6451.63ms`, ffi `6218.99ms`
