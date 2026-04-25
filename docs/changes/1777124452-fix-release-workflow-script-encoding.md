# Release workflow 스크립트 인코딩 문제 수정

## 배경

GitHub Actions 릴리즈 검증에서 두 플랫폼별 준비 단계가 실패했다.

- Windows `Prepare native SDKs`
  - Python stdout 기본 인코딩이 `cp1252`인 상태에서 `setup_paddle_inference.py`가 한글 로그를 출력해 `UnicodeEncodeError`가 발생했다.
- Linux `Install Linux build dependencies`
  - `tools/scripts/install_linux_build_deps.sh`가 CRLF 줄바꿈으로 실행되어 shebang과 `set -euo pipefail` 구문이 깨졌다.

## 결정

- Python 설치 스크립트는 시작 시 stdout/stderr를 UTF-8로 재설정한다.
- shell 스크립트는 LF 줄바꿈을 저장소 규칙으로 고정한다.
- Linux 의존성 설치 스크립트의 줄바꿈을 테스트로 검증한다.

## 구현

- `.gitattributes`
  - `*.sh text eol=lf` 등 스크립트/문서 파일의 줄바꿈 정책을 명시했다.
- `tools/scripts/setup_paddle_inference.py`
  - `configure_utf8_stdio()`를 추가하고 `main()` 시작 시 호출한다.
- `tools/scripts/install_linux_build_deps.sh`
  - LF 줄바꿈으로 정규화했다.
- `tools/scripts/test_install_linux_build_deps.py`
  - shebang 직후 LF와 CRLF 부재를 검증하는 테스트를 추가했다.

## 검증 결과

- `python -m unittest tools.scripts.test_install_linux_build_deps tools.scripts.test_setup_paddle_inference`
  - 결과: 23개 테스트 통과.
- `bash -n tools/scripts/install_linux_build_deps.sh`
  - 결과: 통과.
