# 전 프로젝트 핵심 모듈 리팩토링

## 리팩토링 목적

전체 프로젝트를 점검한 결과, 기능 추가보다 다음 구조 개선이 우선이었다.

- Rust: 팝업 위치 계산 로직이 테스트용 순수 함수와 실제 함수에 중복돼 있다.
- UI: 오버레이와 팝업이 Tauri 이벤트 구독 해제와 ESC 키 바인딩을 각각 반복 구현하고 있다.
- OCR: 지원 언어 목록과 OCR score threshold 해석이 흩어져 있어 변경 지점이 분산돼 있다.

이번 리팩토링은 다음을 목표로 한다.

- 중복 제거
- 설정 해석의 단일화
- 공통 UI 이벤트 처리 모듈화

## 리팩토링 계획

1. Rust의 팝업 위치 계산을 순수 함수 하나로 정리한다.
2. UI 공통 이벤트 처리를 별도 유틸로 분리한다.
3. OCR 설정성 로직을 상수/헬퍼로 모은다.
4. 기존 테스트와 빌드 검증으로 회귀가 없는지 확인한다.

## 리팩토링 사항

- `ui/src/app-hooks.ts`
  - Tauri 이벤트 cleanup과 window `keydown` 바인딩을 공통 훅으로 분리했다.
- `ui/src/overlay.tsx`
  - 이벤트 구독 정리와 ESC 키 처리의 중복 `useEffect`를 공통 훅 호출로 대체했다.
- `ui/src/popup.tsx`
  - 이벤트 구독 정리와 ESC 키 처리의 중복 `useEffect`를 공통 훅 호출로 대체했다.
- `ocr/main.py`
  - 지원 언어 목록을 `SUPPORTED_LANGS` 상수로 모았다.
  - 업로드 기본 확장자를 `DEFAULT_UPLOAD_SUFFIX`로 분리했다.
  - OCR score threshold 해석을 `score_threshold()` 헬퍼로 단일화했다.
- `src/lib.rs`
  - 팝업 위치 계산은 이미 테스트 가능한 순수 함수가 분리돼 있었고, 추가 구조 변경은 이번 범위에서 보류했다.

## 테스트 계획

- `cargo test`
- `deno test --config ui/deno.json ui/src/detection_test.ts`
- `deno task --config ui/deno.json build`
- `cargo check`

## 테스트 결과

- `cargo test`
  - Rust 테스트가 통과했다.
- `deno test --config ui/deno.json ui/src/detection_test.ts`
  - UI 테스트가 통과했다.
- `deno task --config ui/deno.json build`
  - 프런트엔드 빌드가 통과했다.
- `cargo check`
  - Rust 컴파일 검증이 통과했다.

## 추가 검토

- OCR의 `uv run pytest`는 현재 로컬 Python 3.14 환경과 `paddlepaddle-gpu==3.2.2` wheel 호환성 문제로 이번 리팩토링 검증 대상에서 제외했다.
- Rust `calc_popup_pos` 본문 중복은 추가 정리 후보지만, 이번에는 UI/OCR 공통 처리 정리에 우선순위를 뒀다.
