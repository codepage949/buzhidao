# Config 기본값 죽은 코드 제거

## 문제

`cargo build` 시 아래 경고가 발생했다.

- `DEFAULT_OCR_SERVER_DEVICE` is never used
- `DEFAULT_AI_GATEWAY_MODEL` is never used
- `Config::default_runtime()` is never used

현재 설정 로딩은 `.env` 기반으로만 동작하므로,
예전 하드코딩 기본값 경로 일부가 테스트 전용 죽은 코드로 남아 있었다.

## 변경

- `app/src/config.rs`
  - 사용되지 않는 `Config::default_runtime()` 제거
  - 그 경로에서만 쓰이던 미사용 상수 제거
  - 테스트는 실제 런타임 경로(`from_env_file`)와 파서/기본값 함수 기준으로 유지
- `app/src/settings.rs`
  - `.env` 단일화 이후 더 이상 쓰지 않는 `settings.json` 저장/로드 보조 함수 제거
  - `sanitized()`와 `bootstrap_defaults()` 제거
  - 관련 테스트는 `validate()`와 `.env` 저장 경로 기준으로 정리

## 검증

- `cargo test --lib`
