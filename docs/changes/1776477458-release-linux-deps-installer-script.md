# 릴리즈용 Linux 시스템 의존성 설치 스크립트 분리

## 배경

`.github/workflows/release.yml`의 Linux 빌드 단계에는 `apt-get` 기반 시스템 라이브러리 설치 로직이 직접 작성되어 있었고,
동일한 의존성 설치 절차를 로컬에서 재현하거나 재사용하기 어려웠습니다.
특히 `libpipewire-0.3-dev` 누락으로 `libspa-sys` 빌드가 실패하는 케이스가 반복적으로 발생할 수 있어,
CI와 로컬 설치 경로를 통일할 필요가 있었습니다.

## 결정

- Linux 전용 빌드 의존성 설치를 스크립트로 분리한다.
- 릴리즈 워크플로우는 이 스크립트만 호출하도록 변경한다.
- 스크립트는 `apt-get` 존재 여부를 확인하고, Linux 환경에서 필요한 패키지를 일괄 설치한다.

## 변경 사항

- `scripts/install_linux_build_deps.sh`
  - 기존 workflow에 내장되어 있던 Linux 패키지 목록을 동일하게 이동.
  - `apt-get` 부재 시 안내 메시지로 중단.
  - 배포/CI 외부에서도 동일 목록 재사용 가능.
- `.github/workflows/release.yml`
  - Linux 의존성 설치 step에서 직접 `apt-get` 명령을 제거하고
    `bash scripts/install_linux_build_deps.sh` 호출로 교체.

## 테스트

- 자동 테스트 없음
  - 스크립트는 설치 실행을 수행하므로, 실제 실행은 CI/배포 환경에서의 검증으로 대체.
