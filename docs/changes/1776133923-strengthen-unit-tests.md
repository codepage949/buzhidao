# 단위 테스트 보강 및 저가치 테스트 정리

## 목적

- 핵심 순수 함수 커버리지 보강 (경계/오류 분기)
- 의미 없는 상수 비교성 테스트 제거
- Python 사이드카에도 최소한의 단위 테스트를 도입

## Rust (`app/`)

### 추가

- `services::ocr_pipeline`
  - `selection_rect_to_image_rect`
    - 이미지 크기 0이면 실패
    - 뷰포트 크기 0 이하면 실패
    - 음수 w/h는 좌상단으로 정규화된다
    - 뷰포트 밖 좌표는 이미지 범위 안으로 클램프된다
  - `resize_image_to_max_width`
    - 폭이 max 이하면 원본과 배율 1.0을 반환
    - max=0이면 원본을 반환
- `lib.rs`
  - `resolve_ocr_server_executable`
    - configured 경로가 실존하면 그대로 반환
    - resource_dir가 없으면 configured 그대로 반환

### 제거 (저가치)

- `config::score_thresh_기본값은_0_5다` — 상수 vs 동일 리터럴 비교
- `config::ocr_server_resize_width_기본값은_1024다` — 동일
- `config::ocr_debug_trace_기본값은_false다` — 표준 라이브러리 bool 파싱을 테스트
- `ocr::python_sidecar::실행_파일_경로를_그대로_사용한다` — engine 생성만 확인하는 smoke

## Python (`ocr_server/`)

### 리팩토링

- `paddleocr` import를 `build_ocr` 내부로 지연.
  모듈 임포트만으로는 paddleocr가 필요 없어 순수 함수 단위 테스트가 가능해진다.
  Frozen 실행/서버 모드 동작은 변함 없음.

### 추가

- `ocr_server/tests/test_pure.py` (stdlib `unittest`)
  - `parse_request`: 정상 파싱, `score_thresh` 기본값, 잘못된 JSON은 예외
  - `resolve_ocr_device`: 기본 cpu, 공백/대소문자 정규화, 잘못된 값은 `ValueError`

실행: `python -m unittest discover ocr_server/tests`

## 검증

- `cargo test --lib` 통과
- `python -m unittest discover ocr_server/tests` 통과
