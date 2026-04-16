# PaddleOCR 다국어 지원 + 언어 변경 시 sidecar 재시작

## 배경

PaddleOCR은 80여 종의 언어 모델을 제공한다. 기존 구현은 `en`/`ch`로
하드코딩되어 확장성이 없었다. 또한 다국어를 지원하면 모든 언어 모델을
상주시킬 수 없으므로 "동시에 1언어만 로드"를 정책으로 굳힌다.

## 정책

- 언제든 OCR 서버 프로세스에는 **정확히 1개 언어**의 PaddleOCR 인스턴스만
  존재한다 (VRAM/RAM 상한 고정).
- 언어 변경 시 sidecar 프로세스를 종료하고 다음 OCR 실행 시 새 언어로
  재스폰한다. (전체 앱 재시작 불필요.)
- 요청 `source`가 선택 언어와 다르면 에러. (지연 로드 안전망 제거.)

## 변경

### ocr_server (Python)

- `LANGS` 리스트를 PaddleOCR 다국어 가이드 기준 80개 코드로 확장.
- `run_server()`가 선택 언어(`PYTHON_OCR_LANG`) 하나만 빌드·웜업.
- 요청 `source`가 선택 언어와 다르면 오류 응답. 지연 로드 제거.

### app (Rust)

- `PythonSidecarEngine.lang`을 `Mutex<String>`로 변경.
- `set_lang(lang)` 메서드 추가: 현재 lang과 다르면 내부 값을 바꾸고
  실행 중인 sidecar를 종료.
- `OcrBackend::set_lang` 위임 추가.
- `save_user_settings`에서 `source`가 변경되었으면:
  1. 캡처 busy 플래그를 true로 설정
  2. 기존 `loading` 창을 `show()`로 재사용 (초기 웜업과 동일 UI)
  3. 백그라운드에서 `set_lang` + `warmup` 수행 (즉시 재시작/재웜업)
  4. 완료되면 loading 숨김, busy 해제
- 초기 웜업 완료 시 loading을 `close()`가 아니라 `hide()`로 변경해
  언어 변경 시점에 재사용 가능하도록.
- `capabilities/default.json`에 `core:window:allow-show` 추가.
- `settings::normalize_source`를 PaddleOCR 전체 언어 화이트리스트로 교체.
  미지원·빈 값은 기본 `en`로 폴백.

### ui (React)

- `source` 필드를 RadioRow → `<select>` 콤보 박스로 변경.
- 지원 언어 목록(코드 + 한국어 라벨)을 공용 상수로 정의.

## 테스트

- Rust: `normalize_source`가 화이트리스트 내 언어는 그대로, 외부 값은
  `en`으로 폴백하는지 단위 테스트.
- Python/UI는 단위 테스트 대상 아님(의존/육안 검증).

## 호환성

- 기존 `.env`에 `SOURCE=en`/`SOURCE=ch`로 저장된 값은 그대로 유효.
- OCR 서버 바이너리 재빌드가 필요하다(Python 소스 변경).
