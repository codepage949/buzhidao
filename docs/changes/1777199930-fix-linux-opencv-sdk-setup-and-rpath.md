# Linux OpenCV SDK 자동 구성과 런타임 로더 수정

## 배경

릴리즈 OCR smoke 테스트는 `LD_LIBRARY_PATH`에 OpenCV SDK 경로를 직접 추가한 상태로 실행된다.
그래서 smoke는 통과하지만, 릴리즈 아카이브에서 꺼낸 `buzhidao`를 직접 실행하면 동적 로더가
`libopencv_core.so.406`을 찾지 못해 앱 시작 전 단계에서 실패할 수 있다.

근본 원인은 Linux에서 `setup_paddle_inference.py`가 OpenCV SDK를 직접 구성하지 않고,
release workflow가 임시 SDK를 수동으로 만들고 있었다는 점이다. 이 임시 구성은 버전이 붙은
런타임 `.so.*`까지 안정적으로 보존하는 책임이 스크립트 밖에 있었다.

## 계획

1. `setup_paddle_inference.py`가 Linux에서 시스템 `libopencv-dev` 설치 결과를 읽어
   `.paddle_inference/third_party/opencv-sdk/linux-x86_64`를 직접 구성하게 한다.
2. OpenCV 링크용 `.so`와 런타임 로더가 요구하는 버전 `.so.*`를 모두 SDK `install/lib`에 복사한다.
3. release workflow의 Linux OpenCV 임시 SDK 수동 구성 로직을 제거하고 준비 스크립트에 위임한다.
4. Linux 빌드에서 실행 파일이 자기 위치의 공유 라이브러리를 찾도록 `$ORIGIN` rpath를 링크 옵션에 추가한다.
5. Linux SDK 자동 구성 단위 테스트를 추가한다.

## 구현

- `tools/scripts/setup_paddle_inference.py`
  - Linux에서 `/usr/include/opencv4` 등 시스템 OpenCV 헤더 경로와 `/usr/lib/<arch>-linux-gnu` 등
    라이브러리 경로를 탐색한다.
  - `opencv_core`, `opencv_imgproc`, `opencv_imgcodecs`의 `lib*.so*` 파일을 모두 canonical SDK
    `install/lib`에 복사한다.
  - OpenCV 헤더나 라이브러리가 없으면 `tools/scripts/install_linux_build_deps.sh` 실행을 안내하는
    명확한 오류를 낸다.
- `.github/workflows/release.yml`
  - Linux용 임시 `.opencv-sdk-ci` 생성과 `--opencv-sdk-dir` 전달을 제거했다.
- `build.rs`
  - Linux 링크 옵션에 `$ORIGIN` rpath를 추가했다.

## 테스트

- `python3 -m unittest tools.scripts.test_setup_paddle_inference` 통과.
- `python3 -m unittest tools.scripts.test_release_helper tools.scripts.test_release_workflow` 통과.
- `cargo check --no-default-features` 통과.
- 로컬 환경에는 OpenCV 헤더가 없어 실제 시스템 복사 검증은
  `tools/scripts/install_linux_build_deps.sh` 선행 필요 오류로 중단되는 것을 확인했다.
