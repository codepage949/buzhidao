# 실험적 CPU 속도 개선 경로 제거

## 배경

- CPU 영역 선택 기능 외에 추가된 속도 개선 경로가 많아지면서 코드와 문서가 복잡해졌다.
- 특히 oneDNN ORT, Python sidecar, Paddle FFI는 실험 단계였고 기본 ONNX 경로를 대체하지 못했다.
- 유지비를 줄이기 위해 실험 경로를 제거하고 기본 ONNX OCR 경로만 남긴다.

## 제거 대상

1. oneDNN ORT 경로
2. Python sidecar 경로
3. Paddle FFI 경로

## 변경 대상

- `Cargo.toml`
- `src/config.rs`
- `src/lib.rs`
- `src/ocr/mod.rs`
- `README.md`
- `.env.example`
- `.gitignore`
- `scripts/release_helper.py`
- `scripts/test_release_helper.py`
- 삭제:
  - `src/ocr/python_sidecar.rs`
  - `src/ocr/paddle_ffi.rs`
  - `scripts/build_ort_windows.py`
  - `scripts/dev_cpu_onednn.py`
  - `scripts/build_paddle_ocr_sidecar.py`
  - `scripts/paddle_ocr_sidecar.py`

## 제거 내용

- `ort`의 `onednn` feature와 oneDNN 세션 로드 경로를 제거한다.
- `OCR_BACKEND` 기반 분기와 `python_sidecar`, `paddle_ffi` 백엔드를 제거한다.
- Python sidecar supervisor, PyInstaller 빌드 스크립트, oneDNN ORT 빌드 스크립트를 제거한다.
- 릴리즈 문서와 helper에서 oneDNN/sidecar 관련 설명과 CPU DLL 포함 경로를 제거한다.
- 기본 ONNX OCR 경로만 유지한다.

## 검증 계획

- Rust 컴파일 확인
- 핵심 OCR/설정 테스트 확인
- 릴리즈 helper 테스트 확인

## 구현 결과

- `OcrBackend`를 ONNX 단일 구현으로 되돌렸다.
- `Config`에서 `OCR_BACKEND`, `PYTHON_OCR_*` 관련 설정을 제거했다.
- CPU/릴리즈 문서에서 oneDNN과 sidecar 관련 사용법을 제거했다.
- 릴리즈 helper에서 CPU 아카이브용 ORT DLL 포함 경로를 제거했다.
- 생성물 정리:
  - `build/pyinstaller-paddle-ocr`
  - `dist/paddle_ocr_sidecar`
  - `paddle_ocr_sidecar.spec`

## 검증 결과

- `cargo check`: 통과
- `cargo test`: 통과 (`63 passed`)
- `python -m unittest scripts.test_release_helper`: 통과 (`7 passed`)

## 리팩터링 확인

- 삭제 후 `OcrBackend`와 설정 경로가 단순해져 추가 리팩터링 우선순위는 낮다.
- 이번 작업과 무관하게 남아 있는 변경 파일(`src/ocr/det.rs`, `src/ocr/rec.rs`, `src/services.rs`, `src/platform.rs`, `src/popup.rs`)은 건드리지 않았다.
