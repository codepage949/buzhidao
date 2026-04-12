# Python Sidecar OCR 전환 현황 정리

## 배경

CPU 환경에서 현재 Rust + ONNX Runtime OCR 경로는 체감상 너무 느리다.
`benchmarks/test.png` 기준으로 Python PaddleOCR native CPU는 1초 내외로 끝나는데,
동일 계열 ONNX 모델을 ONNX Runtime CPU로 돌리면 수십 초까지 늘어났다.

즉 병목은 Rust 언어 자체보다 `변환된 ONNX 모델 + ONNX Runtime CPU` 조합에 가깝다.

## 구현 계획

1. 기존 OCR 엔진을 backend 추상화로 바꿔 ONNX 외 경로를 붙일 수 있게 한다.
2. Python PaddleOCR를 외부 sidecar 프로세스로 호출하는 경로를 추가한다.
3. sidecar를 PyInstaller로 패키징해 Python 없는 배포 가능성을 검토한다.
4. 성능과 안정성 결과를 문서에 남기고, 실패 지점도 함께 기록한다.

## 구현 사항

### OCR backend 추상화

- `OCR_BACKEND` 설정으로 `onnx`, `python_sidecar`, `paddle_ffi`를 분기할 수 있게 했다.
- 앱 state와 OCR 실행 경로를 `OcrBackend` 추상화 기준으로 정리했다.
- CPU 모드 판정도 backend 단위로 처리한다.

### Python PaddleOCR sidecar

- `scripts/paddle_ocr_sidecar.py`를 추가했다.
  - 이미지 파일 경로를 받아 PaddleOCR CPU 추론을 수행한다.
  - 결과는 기존 앱이 소비하는 공용 JSON 형식으로 출력한다.
- `src/ocr/python_sidecar.rs`를 추가했다.
  - 외부 실행 파일을 호출한다.
  - stdout JSON을 `OcrDetection` / `OcrDebugDetection` 형식으로 변환한다.
- Rust 앱은 이미지 버퍼를 임시 PNG로 저장한 뒤 sidecar에 넘긴다.

### Paddle Inference FFI 시도

- `native/paddle_bridge`, `src/ocr/paddle_ffi.rs`, `build.rs`에 Paddle C++ inference 브리지 스캐폴드를 추가했다.
- `PADDLE_INFERENCE_DIR`, `PADDLE_MODEL_DIR`를 받아 predictor 생성까지 시도했다.
- 그러나 실제 SDK/모델 조합에서 predictor 생성 단계가 안정화되지 않았고,
  현재 활성 경로는 아니다.

### PyInstaller 패키징 시도

- `scripts/build_paddle_ocr_sidecar.py`를 추가했다.
- `paddlex[ocr]` extras, metadata, Paddle native 라이브러리 번들링 문제를 순서대로 보완했다.
- `onefile`은 추출/로딩 단계에서 더 불안정해 `onedir`로 전환했다.

## 확인된 결과

### 성공한 것

- Python 가상환경에서 `scripts/paddle_ocr_sidecar.py`를 직접 실행하면 정상 동작한다.
- `benchmarks/test.png` 기준 direct Python sidecar는 OCR JSON을 정상 반환한다.
- Rust 앱은 `python_sidecar` backend를 호출할 준비가 되어 있다.

### 실패한 것

- PyInstaller로 만든 frozen 실행 파일은 아직 안정화되지 않았다.
- extras 누락, metadata 누락, `libmklml_intel.so` 누락 문제는 해결했지만,
  최종적으로 Paddle native runtime이 frozen 환경에서 `SIGILL`로 죽는다.
- 같은 머신에서 direct Python은 정상이라, 현재 blocker는
  `PyInstaller + Paddle native runtime` 조합 쪽으로 본다.

## 테스트

- `cargo check`
- `cargo test python_sidecar_json을_공용_detection_형식으로_변환한다 -- --nocapture`
- `python3 -B`로 `scripts/paddle_ocr_sidecar.py`, `scripts/build_paddle_ocr_sidecar.py` 문법 확인
- direct Python sidecar 실행 검증
  - `DISABLE_MODEL_SOURCE_CHECK=True .venv-sidecar/bin/python scripts/paddle_ocr_sidecar.py --image benchmarks/test.png --source en --score-thresh 0.5 --debug-trace false`

## 현재 결론

- 단기적으로는 `python_sidecar` 전략 자체는 유효하다.
- 다만 배포 형태를 PyInstaller frozen binary로 바로 가져가는 건 아직 불안정하다.
- 다음 단계 우선순위는 아래 둘 중 하나다.
  1. Rust sidecar 호출 규약을 넓혀 `python <script>` 형태도 지원한다.
  2. PyInstaller 대신 다른 배포 방식(zip된 venv, launcher, 별도 런타임 동봉)을 검토한다.
