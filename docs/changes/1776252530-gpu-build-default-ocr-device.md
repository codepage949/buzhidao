# GPU 빌드에서 .env 미존재 시 OCR_SERVER_DEVICE 기본값을 gpu로 설정

## 배경

GPU 빌드(`--features gpu`)에서 처음 앱을 실행하면 `.env`가 없어 `.env.example`을 기준으로
`.env`가 생성된다. 기존에는 빌드 종류에 관계없이 `.env.example`의 `OCR_SERVER_DEVICE=cpu`가
그대로 적용되어, GPU 빌드 사용자가 별도로 설정을 변경해야 했다.

## 결정

GPU 빌드 시 `.env` 최초 생성 시점의 `OCR_SERVER_DEVICE` 기본값을 `gpu`로 한다.
`.env.example`은 CPU 기준 레퍼런스 파일로 그대로 유지한다.

## 변경 사항

### `app/src/lib.rs`

- `ENV_EXAMPLE: &str` 상수를 `default_env_example()` 함수로 교체.
- `#[cfg(feature = "gpu")]` 빌드에서 `OCR_SERVER_DEVICE=cpu`를 `OCR_SERVER_DEVICE=gpu`로
  치환한 문자열을 반환한다.
- CPU 빌드에서는 `.env.example` 원문을 그대로 반환한다.

## 테스트

- `app/src/lib.rs` 내 `default_env_example` 함수 단위 테스트:
  현재 feature 설정에 따라 기대 값을 검증한다.
  (GPU 빌드: `OCR_SERVER_DEVICE=gpu` 포함, CPU 빌드: `OCR_SERVER_DEVICE=cpu` 포함)
