# Paddle FFI OCR 입력 포맷 동작 통일

## 구현 계획

- OCR FFI 진입 경로에서 이미지 파일 입력이 플랫폼마다 다르게 처리되는 지점을 확인한다.
- Windows 전용 이미지 로더에 의존하지 않도록 Rust 레이어에서 공통 입력 포맷 정규화를 추가한다.
- 앱 내부 임시 OCR 이미지 저장 포맷도 같은 기준에 맞춰 정리한다.
- 공통 입력 정규화 로직에 대한 테스트를 추가하고 컴파일 및 테스트 통과를 확인한다.

## 변경 사항

- `src/ocr/mod.rs`와 `src/ocr/paddle_ffi.rs`를 수정해 Paddle FFI OCR 입력 이미지를 전 플랫폼에서 동일하게 처리한다.
- Windows에서만 PNG 등이 허용되고 Linux/macOS에서는 BMP만 허용되던 동작 차이를 제거한다.
- `src/ocr/paddle_ffi.rs`에서 `PreparedOcrImage`를 추가해 BMP/DIB 입력은 그대로 사용하고, 그 외 포맷은 Rust `image` 크레이트로 로드 후 임시 BMP로 변환해서 FFI에 전달하도록 변경했다.
- `src/ocr/mod.rs`의 앱 내부 임시 OCR 이미지 저장 포맷도 PNG에서 BMP로 변경해 앱 경로와 직접 파일 경로가 같은 기준으로 동작하도록 맞췄다.
- 입력 정규화 로직에 대한 한글 테스트를 추가했다.
- 추가 확인 결과 `image` 크레이트는 RGBA 이미지를 BMP로 저장할 때 `32bpp + compression=3(BITFIELDS)` 헤더를 쓸 수 있었고, 네이티브 BMP 로더는 `compression=0`만 허용해 `"압축된 BMP는 현재 지원되지 않습니다"` 오류가 발생했다.
- 이를 해결하기 위해 BMP 저장 전 이미지를 RGB8로 강제 변환해서 전 플랫폼에서 `24bpp + compression=0` 무압축 BMP만 생성하도록 수정했다.
- 테스트도 파일 확장자만 보지 않고 BMP 헤더의 `bits_per_pixel`과 `compression` 값을 직접 검증하도록 강화했다.

## 테스트 계획

- 공통 입력 정규화 로직에 대한 단위 테스트를 추가한다.
- `cargo test`로 관련 테스트 통과를 확인한다.

## 테스트 결과

- `cargo test png_입력은_bmp로_변환해_공통_경로를_사용한다 -- --nocapture` 통과
- `cargo test bmp_입력은_추가_변환_없이_그대로_사용한다 -- --nocapture` 통과
- `cargo check` 통과
- `cargo test 임시_ocr_이미지는_bmp로_저장한다 -- --nocapture`는 현재 빌드 설정에서 `has_paddle_inference` 조건부 모듈이 제외되어 실행 대상이 없었음
