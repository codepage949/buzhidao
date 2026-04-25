# GPU 런타임 구성 테스트 보완 검토

## 배경

- GPU 런타임 구성 스크립트와 Paddle Inference GPU SDK 선택 로직을 추가했다.
- 이후 `cudnn64_9.dll` 동적 로드 실패를 계기로 `.cuda` 경로 자동 반영 로직도 추가했다.
- 이번 작업은 기존 구현을 크게 바꾸기보다, 테스트가 부족하거나 잘못된 부분이 없는지 영역을 나눠 점진적으로 점검하고 필요한 테스트를 보완하는 것이다.

## 구현 계획

1. `setup_cuda_runtime.py` 테스트를 점검한다.
   - package set 선택, override, 오류 경로, clean/no-download의 검증 공백을 확인한다.
2. `setup_paddle_inference.py` 테스트를 점검한다.
   - CPU 기본값 유지, GPU 단일 CUDA 12.6 경로, unsupported OS/arch 오류, 다운로드 파일명 충돌 방지 검증을 확인한다.
3. Rust 런타임 경로 로직을 점검한다.
   - `.cuda` 경로 prepend 로직과 환경 변수 병합 동작을 단위 테스트할 수 있는지 확인한다.
   - build.rs의 파일 복사 로직은 cargo build 통합 동작이라 직접 단위 테스트가 어려우므로, 검증 가능한 범위를 명확히 기록한다.
4. README/변경 문서/CLI help의 용어 일관성을 점검한다.
   - `paddle-cu126`, `--device gpu`, `cu126` 기준으로 잔여 불일치가 없는지 확인한다.
5. 수정 후 관련 테스트와 정적 검사를 실행한다.

## 구현 내용

- `tools/scripts/test_setup_cuda_runtime.py`
  - 지원하지 않는 `package-set` 오류 테스트를 추가했다.
  - 빈 wheelhouse와 CUDA 라이브러리가 없는 wheel 오류 테스트를 추가했다.
  - `clean_directory()`가 기존 내용을 제거하고 디렉터리를 재생성하는지 검증했다.
  - `download_wheels()`가 package set과 extra index를 `pip download` 명령에 반영하는지 검증했다.
  - `normalize_platform("auto")`와 미지원 platform 오류 경로를 검증했다.
- `tools/scripts/test_setup_paddle_inference.py`
  - 미지원 architecture와 미지원 device 오류 테스트를 추가했다.
  - CPU/GPU 아카이브 캐시 파일명이 충돌하지 않는지 검증했다.
  - 지원하지 않는 다운로드 URL 확장자 오류 테스트를 추가했다.
  - 기존 아카이브가 있고 `force=False`일 때 다운로드를 건너뛰는지 검증했다.
- `src/lib.rs`
  - `.cuda` 런타임 경로 prepend 로직이 존재하는 디렉터리만 환경 변수 앞쪽에 추가하는지 단위 테스트를 추가했다.
- `build.rs`
  - Cargo build script 직접 단위 테스트는 별도 harness 없이는 어렵다.
  - 기존 검증 방식처럼 GPU feature 빌드 실행 후 출력 폴더의 DLL 복사 여부로 확인하는 범위로 둔다.
- 문서/CLI 용어 점검
  - 현재 README, 최신 변경 문서, 스크립트에는 `--cuda`, `cu118`, `paddle-cu11` 잔여 참조가 없다.
  - 과거 변경 문서에는 당시 Python sidecar 기준 `cu118` 기록이 남아 있지만, 이번 작업 범위에서는 수정하지 않는다.

## 검증

- `python -m unittest tools.scripts.test_setup_cuda_runtime`
  - 통과
  - 14개 테스트 통과
- `python -m unittest tools.scripts.test_setup_paddle_inference`
  - 통과
  - 21개 테스트 통과
- `python -m unittest tools.scripts.test_setup_cuda_runtime tools.scripts.test_setup_paddle_inference`
  - 통과
  - 35개 테스트 통과
- `cargo test --features gpu 런타임_경로는_존재하는_디렉터리만_앞쪽에_추가한다 -- --nocapture`
  - 통과
- `cargo test --features gpu --lib -- --nocapture`
  - 통과
  - 89개 테스트 통과
- `python -m py_compile tools/scripts/setup_cuda_runtime.py tools/scripts/test_setup_cuda_runtime.py tools/scripts/setup_paddle_inference.py tools/scripts/test_setup_paddle_inference.py`
  - 통과
- `git diff --check`
  - 통과
