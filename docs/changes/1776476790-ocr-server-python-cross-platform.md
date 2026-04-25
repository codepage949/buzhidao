# OCR sidecar 비교 스크립트 멀티플랫폼 Python 경로 지원

## 배경

`scripts/compare_ocr_sidecar_ffi.py`는 현재 `OCR_SERVER_PYTHON`을 Windows venv 경로(`.venv/Scripts/python.exe`)로 고정해 두었기 때문에
Linux/macOS 환경에서 동작하지 않거나, 실행 경로가 다를 때 깨지는 문제가 있었다.

또한 FFI 실행용 환경 변수 `PATH` 병합이 `;`로 하드코딩되어 있어 POSIX 환경과의 호환성이 떨어졌다.

## 결정

- `OCR_SERVER_PYTHON`은 실행 환경을 탐지해 동적으로 해석한다.
  - `OCR_SERVER_PYTHON` 환경변수 우선 적용
  - `.venv/Scripts/python.exe` → `.venv/bin/python` 순으로 탐색
  - 실패 시 `python3` 또는 `python` PATH fallback
- 플랫폼 독립적인 `PATH` 병합은 `os.pathsep`로 처리한다.

## 변경 사항

### 비교 스크립트
- `scripts/compare_ocr_sidecar_ffi.py`
  - `OCR_SERVER_PYTHON`를 고정 경로가 아닌 후보 목록 기반 동적 탐지 함수로 변경
  - `shutil.which` 기반으로 기본 Python 실행 파일 fallback 추가
  - FFI 환경변수 구성에서 `PATH` 구분자를 OS별(`os.pathsep`)로 교체

## 테스트

- 자동 테스트 미실행
  - 사용자 요청 범위가 문서화/커밋 중심이어서 실행을 생략함
