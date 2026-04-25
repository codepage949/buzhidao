# cargo tauri dev 기본 Paddle FFI 활성화

## 배경

현재 앱 OCR 백엔드는 Paddle FFI 단일 모드인데 `Cargo.toml`의 기본 feature가 비어 있어
루트에서 `cargo tauri dev`를 실행하면 `paddle-ffi` 없이 빌드된다.
그 결과 시작 warmup 단계에서
`FFI 단일 모드에서는 paddle-ffi feature와 Paddle Inference 링크가 필요합니다`
오류가 발생한다.

## 구현 계획

1. 기본 feature 구성과 OCR 모듈의 조건부 컴파일 경로를 확인한다.
2. `cargo tauri dev` 기본 실행이 Paddle FFI 모드로 올라오도록 Cargo feature를 조정한다.
3. README의 개발 실행 안내를 기본 동작에 맞게 정리한다.
4. 테스트로 기본 feature 빌드가 유지되는지 확인한다.

## 예상 변경 범위

- `Cargo.toml` 기본 feature에 `paddle-ffi` 추가
- `README.md`의 개발 실행 안내 보정
- `ui/vite.config.ts`의 `@shared` alias를 루트 이동 구조에 맞게 수정

## 테스트 계획

- `cargo test`
- `deno task test` (`ui/`)

## 구현 내용

- `Cargo.toml`의 기본 feature를 `["paddle-ffi"]`로 변경했다.
  - 이제 루트에서 `cargo tauri dev`를 실행하면 기본적으로 Paddle FFI 빌드가 활성화된다.
- `README.md`에 기본 개발 실행이 Paddle FFI를 포함한다는 점과
  `.paddle_inference` 준비가 필요하다는 점을 명시했다.
- Paddle FFI 실행 예시도 기본 feature 기준에 맞춰
  `cargo tauri dev --features paddle-ffi` 대신 `cargo tauri dev`로 정리했다.
- `ui/vite.config.ts`의 `@shared` alias를 `../shared`로 수정해
  `ui/src/settings.tsx`의 `@shared/langs.json` import가 루트 이동 후에도 해석되도록 했다.
- 기본 feature 활성화 후 드러난 언어 정규화 문제를 함께 수정했다.
  - `src/ocr/paddle_ffi.rs`에서 `ch_tra` 같은 지원 언어 코드를
    alias 축약보다 먼저 보존하도록 정규화 순서를 조정했다.

## 테스트 결과

- 통과: `cargo test`
- 통과: `deno task test` (`ui/`)
- 통과: `deno task build` (`ui/`)
