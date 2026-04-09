# CUDA EP 활성화

## 배경

- OCR 추론은 현재 `ort` 기본 설정으로 세션을 만들고 있어 CPU Execution Provider만 사용하는 상태다.
- `ort`에 `cuda` feature를 추가하더라도 세션 생성 시 CUDA Execution Provider를 명시하지 않으면 GPU를 사용하지 않는다.
- CUDA 런타임이 없는 환경도 계속 실행 가능해야 하므로, CUDA 등록 실패 시 CPU로 안전하게 폴백되어야 한다.

## 목표

- CPU only와 GPU 빌드를 Cargo feature로 분리한다.
- GPU 빌드에서는 OCR 모델 세션 생성 시 CUDA EP를 우선 등록한다.
- CUDA 초기화 실패 시 앱이 깨지지 않고 CPU 경로로 계속 동작하게 한다.

## 구현 계획

1. `Cargo.toml`에서 프로젝트 feature로 CPU/GPU 빌드 구성을 나눈다.
2. OCR 세션 생성 로직을 공통화하고 GPU 빌드에서만 CUDA EP 우선 등록을 적용한다.
3. 세션 생성 실패 메시지가 det/cls/rec 단계별로 유지되도록 에러 경로를 정리한다.
4. CPU/GPU 두 경로 모두 테스트하고 실행 문서를 갱신한다.

## 구현 내용

- `Cargo.toml`에 프로젝트 feature `gpu = ["ort/cuda"]`를 추가하고 기본 빌드는 CPU only로 돌렸다.
- `src/ocr/mod.rs`에 `load_session` 헬퍼를 추가해 det/cls/rec 세션 생성을 공통화했다.
- `src/ocr/mod.rs`에 `configure_execution_providers`를 추가해 `gpu` feature가 켜진 빌드에서만 `ep::CUDA::default().build().fail_silently()`를 먼저 등록하도록 했다.
- CUDA EP 등록 실패 시 ONNX Runtime 기본 CPU 경로로 폴백되도록 구성했다.
- `README.md`에 CPU only / GPU 실행·테스트·빌드 명령을 추가했다.

## 테스트 계획

- `cargo test`
- `cargo test --features gpu`

## 테스트 결과

- `cargo test` 통과
  - 41개 테스트 성공
- `cargo test --features gpu` 통과
  - 42개 테스트 성공

## 리팩토링 검토

- det/cls/rec 세션 생성 중복을 `load_session`으로 줄였고, provider 분기도 `configure_execution_providers`로 분리해 추가 리팩토링 필요성은 현재 없다.
