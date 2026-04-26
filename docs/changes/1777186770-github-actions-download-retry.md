# GitHub Actions 다운로드 재시도 보강

## 계획

릴리스 workflow에서 네트워크 다운로드가 발생할 수 있는 지점을 점검했다. 외부 GitHub Action 내부 다운로드는 각 action 구현에 맡기되, repository에서 직접 실행하는 명령과 스크립트는 명시적인 재시도 보호를 둔다.

## 점검 결과

- `setup_paddle_inference.py`: Paddle Inference, OpenCV, pyclipper 다운로드에 재시도 로직이 이미 있다.
- `setup_cuda_runtime.py`: `pip download` 호출에 명시적인 외부 재시도 보호가 없다.
- `install_linux_build_deps.sh`: `apt-get update/install`에 명시적인 재시도 보호가 없다.
- release workflow: `deno install`, `cargo binstall`은 네트워크 다운로드가 발생할 수 있지만 명시적인 wrapper가 없다.
- `cargo tauri build`와 release OCR smoke의 `cargo test`는 빌드 중 registry 접근 가능성이 있으므로 Cargo 네트워크 재시도 환경 변수를 workflow 수준에서 지정한다.

## 변경 방향

- CI shell 명령용 `tools/scripts/ci_retry.sh`를 추가한다.
- release workflow의 `deno install`과 `cargo binstall` 호출을 `ci_retry`로 감싼다.
- release workflow에 `CARGO_NET_RETRY`와 `CARGO_HTTP_TIMEOUT`을 지정한다.
- Linux build dependency 설치 스크립트의 `apt-get update/install`을 재시도 처리한다.
- CUDA runtime 구성 스크립트의 `pip download`를 pip 자체 retry/timeout과 외부 subprocess retry로 보호한다.

## 검증 계획

- retry shell 스크립트 테스트를 추가한다.
- CUDA runtime `pip download` 명령 구성과 실패 후 재시도를 테스트한다.
- release workflow 정적 테스트로 retry wrapper와 Cargo 네트워크 env가 적용되었는지 확인한다.
- `python -m unittest tools.scripts.test_release_workflow tools.scripts.test_setup_cuda_runtime`
- `bash tools/scripts/test_ci_retry.sh`
- `git diff --check`
- 변경 파일 내 로컬 절대 경로 패턴 검사

