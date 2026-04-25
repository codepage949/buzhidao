# setup_paddle_inference.py에 Clipper2 설치 통합

## 배경

`app/build.rs`는 native OCR 빌드 시 Clipper2 C++ 소스를 찾으면 polygon unclip 후처리에
사용하고, 찾지 못하면 근사 구현으로 폴백했다.

문제는 기존 탐색 경로가 Cargo registry의 `clipper2-sys` 크레이트 소스에만 의존했다는 점이다.
`scripts/setup_paddle_inference.py`로 Paddle Inference SDK를 설치한 환경에서도
Clipper2는 별도로 준비되지 않아 경고가 계속 출력될 수 있었다.

## 결정

- `scripts/setup_paddle_inference.py`가 Paddle Inference SDK 설치와 함께
  Clipper2 C++ 소스도 내려받아 `.paddle_inference/third_party` 아래에 배치한다.
- `app/build.rs`는 Cargo registry보다 먼저 위 번들 경로를 탐색한다.
- 번들 경로가 없으면 기존 registry fallback을 유지해 플랫폼별 기존 동작을 깨지 않는다.

## 변경 사항

### 설치 스크립트
- `scripts/setup_paddle_inference.py`
  - `clipper2-sys 1.0.0` 아카이브 다운로드 로직을 추가했다.
  - 아카이브 압축을 해제하고 Clipper2 C++ 소스 존재를 검증한 뒤
    `app/.paddle_inference/third_party/clipper2-sys-1.0.0`로 복사한다.
  - Paddle Inference SDK 설치가 끝난 뒤 같은 실행 흐름에서 Clipper2 배치까지 이어지도록 했다.

### Rust 빌드 스크립트
- `app/build.rs`
  - Clipper2 탐색을 공통 함수로 정리했다.
  - `CARGO_MANIFEST_DIR/.paddle_inference/third_party/clipper2-sys-1.0.0`를
    우선 탐색하도록 변경했다.
  - 해당 경로가 없으면 기존처럼 Cargo registry의 `clipper2-sys-1.0.0`를 탐색한다.

## 검증

- `uv run python scripts/setup_paddle_inference.py`
  - 결과: 정상 완료.
  - 확인 사항: `app/.paddle_inference/third_party/clipper2-sys-1.0.0`에 Clipper2 C++ 소스 배치.

- `uv run python scripts/compare_ocr_sidecar_ffi.py`
  - 결과: 종료코드 `0`.
  - 확인 사항: 실행 로그에 `Clipper2 C++ 소스를 찾지 못했습니다` 경고가 나타나지 않음.
