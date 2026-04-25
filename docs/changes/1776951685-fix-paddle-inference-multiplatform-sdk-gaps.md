# .paddle_inference 멀티플랫폼 준비 부족분 보완

## 목표

- `.paddle_inference` 계층에서 macOS용 `paddle_inference` 라이브러리 탐지가 빠지는 문제를 해소한다.
- OpenCV를 `.paddle_inference` 내부 canonical 경로 하나로 통일한다.
- Linux/macOS도 시스템 패키지 fallback 없이 `.paddle_inference` 아래 SDK만 사용하도록 정리한다.

## 범위

- `build.rs`
- `tools/scripts/setup_paddle_inference.py`
- `tools/scripts/test_setup_paddle_inference.py`
- `README.md`

## 구현 계획

1. macOS `.dylib`를 포함하도록 `paddle_inference` 라이브러리 탐지 로직을 보완한다.
2. OpenCV 링크 로직을 `opencv_world` 단일 라이브러리와 모듈형 라이브러리 조합 모두 지원하도록 확장한다.
3. 비Windows 플랫폼에서도 기존 OpenCV SDK를 `.paddle_inference/third_party/opencv-sdk/<platform>` 아래로 들여올 수 있게 준비 스크립트를 확장한다.
4. OpenCV는 legacy 경로나 시스템 fallback 없이 `.paddle_inference` canonical 경로만 사용하게 한다.
5. 준비 스크립트 테스트와 Rust 빌드 검증을 수행한다.

## 구현 결과

- `build.rs`에서 `paddle_inference` 라이브러리 탐지를 `.lib`, `.so`, `.a`, `.dylib`까지 포괄하도록 보완했다.
- `build.rs`의 OpenCV 링크 로직을 확장해 다음 순서로 탐지하게 했다.
  - `.paddle_inference/third_party/opencv-sdk/<platform>`
- OpenCV는 `opencv_world` 단일 라이브러리뿐 아니라 `opencv_core`, `opencv_imgproc`, `opencv_imgcodecs` 조합도 허용하도록 바꿨다.
- `tools/scripts/setup_paddle_inference.py`에 `--opencv-sdk-dir` 옵션을 추가해 Linux/macOS에서도 기존 OpenCV SDK를 `.paddle_inference/third_party/opencv-sdk/<platform>` 아래로 가져올 수 있게 했다.
- Windows 자동 배치 경로도 `.paddle_inference/third_party/opencv-sdk/windows-x86_64`를 기준으로 맞췄다.
- OpenCV는 더 이상 legacy 경로나 시스템 `pkg-config`를 보지 않고 `.paddle_inference` 내부 canonical 경로만 사용한다.
- `README.md`에 Linux/macOS OpenCV 준비 방법과 canonical 경로를 문서화했다.

## 검증 결과

- `python -m unittest tools.scripts.test_setup_paddle_inference` 통과
- `cargo test` 통과
