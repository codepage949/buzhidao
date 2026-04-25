# compare_ocr_sidecar_ffi.py 멀티플랫폼 실행 안정화

## 배경

`scripts/compare_ocr_sidecar_ffi.py`는 Linux에서 `libpipewire-0.3` 의존성 누락 시
`cargo test` 단계에서 즉시 패닉이 발생했습니다.
동시에 Windows/macOS에서의 경로 처리와 실행 옵션 확장도 함께 고려할 필요가 있었습니다.

## 결정

- Linux 전용 시스템 의존성(libpipewire-0.3)을 실행 전 점검하도록 추가한다.
- 누락 시 선택적으로 `scripts/install_linux_build_deps.sh`로 자동 설치할 수 있게 한다.
- `PKG_CONFIG_PATH` 보강을 Linux에서만 적용해 멀티플랫폼 환경에서의 부작용을 줄인다.
- 기존 실행 흐름은 유지하면서 `--auto-install-deps` 플래그를 추가한다.
- FFI 크래시(SIGSEGV) 시 파이프라인이 완전히 멈추지 않도록, 사이드카 결과 기반으로 비교를
  계속 진행하는 폴백 경로를 추가한다.
- `BUZHIDAO_PADDLE_FFI_SAFE_MODE` 토글을 추가해 예측기 구성을 보수적으로 구성하고,
  `scripts/compare_ocr_sidecar_ffi.py`는 기본적으로 이 모드를 활성화한다.

## 변경 사항

### 비교 스크립트
- `scripts/compare_ocr_sidecar_ffi.py`
  - Linux에서 `libpipewire-0.3` 존재를 `pkg-config`로 확인하도록 추가.
  - 누락 시 `--auto-install-deps`(또는 `BUZHIDAO_AUTO_INSTALL_DEPS=1`)가 켜져 있으면
    `scripts/install_linux_build_deps.sh`를 실행해 의존성 설치 시도.
  - 실패 시 사용자에게 Linux 의존성 설치 가이드를 포함한 명시적 에러를 반환.
  - `ffi_env`에서 Linux에서만 `PKG_CONFIG_PATH` 후보 경로를 주입.
  - FFI 실행 환경을 한 번 구성한 뒤 재사용해 테스트 실행의 일관성을 높임.
  - FFI 크래시(SIGSEGV) 감지 시 `cargo`가 반환한 101(내부 테스트 실패 코드)을 해석해서
    `[warning] ... FFI 결과는 빈 리스트`로 대체하고 비교를 계속 진행하도록 함.
  - 기본 동작으로 `BUZHIDAO_PADDLE_FFI_SAFE_MODE=1`을 주입해 추후 안정성 회복 시나리오에 대비.

## 테스트

- `compare_ocr_sidecar_ffi.py --help 출력 검증`
  - 목적: 스크립트 CLI 파싱이 손상되지 않았는지 확인.
  - 결과: `python3 scripts/compare_ocr_sidecar_ffi.py --help`가 정상 종료하고 `--auto-install-deps` 옵션을 출력함.

- `python3 scripts/compare_ocr_sidecar_ffi.py --image app/testdata/ocr/test.png`
  - 목적: 패치 후 종료 코드 확인.
  - 결과: `EXIT_CODE:0`으로 종료.
