# auto-download-missing-paddleocr-models

## 배경

- 선택된 OCR 언어에 맞는 PaddleOCR 모델이 로컬 캐시에 없으면 앱이 즉시 실패했다.
- Python `paddleocr` 패키지처럼 언어별 모델 선택 규칙과 공식 다운로드 경로를 따라 자동 보충할 필요가 있다.

## 변경

- PaddleOCR upstream의 PP-OCRv5 언어별 rec 모델 선택 규칙을 기준으로 언어 코드별 필요 모델을 계산한다.
- 기본 cache root(`~/.paddlex/official_models`, 과거 `~/.paddleocr` 호환)를 기준으로 det/cls/rec 모델 존재 여부를 판단한다.
- 누락된 공식 모델은 BOS 공식 배포 URL에서 자동 다운로드 후 cache root에 압축 해제한다.
- 앱 시작 시와 OCR 언어 변경 시 모델 보장을 먼저 수행한 뒤 FFI backend warmup을 진행한다.
- 여러 rec 모델이 함께 존재해도 native selector가 언어군에 맞는 모델을 우선 선택하도록 보정한다.
- `ch_tra`는 upstream 토큰인 `chinese_cht`로 매핑해 번체 모델 다운로드와 선택이 같은 기준으로 동작하게 맞춘다.
- 공식 rec 모델의 `character_dict`가 `config.json`뿐 아니라 `inference.yml`에만 있는 경우도 읽도록 native 사전 로더를 보강한다.
- `inference.yml`의 `character_dict` 리스트가 key와 같은 들여쓰기를 쓰는 공식 모델도 읽도록 YAML 파서를 수정한다.
- `build.rs`, `src/ocr/mod.rs`, `src/ocr/paddle_ffi.rs`에는 동작 변경 없이 포맷팅만 반영됐다.

## 검증

- `cargo test 공식_캐시의_다국어_rec_모델로_엔진을_생성할_수_있다 -- --nocapture`
- `deno task test` in `ui`
- `deno task build` in `ui`

## 비고

- 단위 테스트로 언어별 모델 매핑과 cache root 선택은 검증했다.
- FFI 재현 테스트에서 `fr`와 `ch_tra` 엔진 생성이 모두 통과했다.
- 전체 `cargo test`는 현재 환경에서 `RC.EXE`가 PATH에 없어 Tauri Windows resource 단계에서 중단됐다.
- 실제 네트워크 다운로드 경로는 공식 BOS URL 규칙을 그대로 사용하지만, 이번 턴에서는 대용량 실다운로드 smoke test까지는 수행하지 않았다.
