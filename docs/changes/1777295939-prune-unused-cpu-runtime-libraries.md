# CPU 빌드 외부 런타임 라이브러리 정리

## 목표

- CPU 빌드 기준으로 패키징되는 외부 DLL/SO 중 실제 실행에 필요 없는 파일이 포함되는지 확인한다.
- 특히 Windows OpenCV debug DLL인 `opencv_world4100d.dll` 같은 파일이 릴리즈 CPU 패키지에 들어가지 않게 한다.
- 제거 규칙은 실제 로드 의존성과 smoke 검증을 기준으로 정한다.

## 계획

1. 릴리즈/패키징 스크립트에서 외부 런타임 라이브러리를 수집하는 경로를 확인한다.
2. Windows CPU 빌드와 Linux CPU 빌드의 OpenCV/Paddle runtime 파일 목록을 확인한다.
3. debug suffix, import library, link-only 파일, 실제 런타임 불필요 파일이 산출물에 들어가는지 확인한다.
4. 불필요 파일이 포함될 수 있으면 패키징 필터를 추가한다.
5. 테스트 또는 smoke로 필터가 필요한 파일을 제거하고 필수 파일은 유지하는지 확인한다.

## 검증 기준

- CPU 릴리즈 패키지에 debug OpenCV DLL/SO가 포함되지 않는다.
- 릴리즈 실행에 필요한 Paddle/OpenCV runtime 라이브러리는 유지된다.
- 테스트는 한글 이름으로 추가하거나 기존 한글 테스트로 고정한다.

## 확인 결과

- Windows OpenCV SDK에는 릴리즈 DLL과 함께 debug DLL 및 선택 플러그인이 들어 있다.
  - `opencv_world4100.dll`: 릴리즈 runtime으로 필요
  - `opencv_world4100d.dll`: debug runtime으로 CPU 릴리즈 패키지에는 불필요
  - `opencv_videoio_msmf4100_64d.dll`: debug videoio plugin으로 불필요
  - `opencv_videoio_ffmpeg4100_64.dll`, `opencv_java4100.dll`: 현재 OCR 경로에서 직접 쓰지 않는 plugin/runtime
- 기존 `build.rs`는 OpenCV runtime 디렉터리의 shared library를 통째로 target에 복사할 수 있었다.
- `release_helper.py`도 target 디렉터리의 DLL/SO를 확장자 기준으로 모두 아카이브에 넣었기 때문에, stale 파일이 있으면 최종 패키지에 섞일 수 있었다.

## 변경

- `build.rs`
  - OpenCV runtime staging을 디렉터리 전체 복사에서 링크된 OpenCV 라이브러리명 기반 복사로 변경했다.
  - `opencv_world4100`로 링크한 경우 `opencv_world4100.dll`만 staging한다.
  - Linux split OpenCV 구성에서는 `libopencv_core.so*`, `libopencv_imgproc.so*`, `libopencv_imgcodecs.so*`처럼 링크된 이름에 대응하는 runtime만 staging한다.
  - Windows OpenCV debug runtime 이름은 staging 대상에서 제외한다.
- `tools/scripts/release_helper.py`
  - 아카이브 layout 복사 단계에서도 OpenCV debug DLL, Java DLL, videoio plugin DLL을 제외한다.
  - build target에 stale 파일이 남아 있어도 CPU 릴리즈 아카이브에 들어가지 않게 방어한다.
- `tools/scripts/test_release_helper.py`
  - OpenCV debug/plugin runtime 제외 규칙을 한글 테스트로 고정했다.

## 검증 결과

- `python -m unittest tools.scripts.test_release_helper`
  - 19개 테스트 통과
- `cargo build --release --features paddle-ffi`
  - build script 컴파일 및 release 빌드 통과
- `cargo test --release --features paddle-ffi`
  - 97개 테스트 통과
- release target OpenCV runtime 확인
  - `target/release`: `opencv_world4100.dll`만 존재
  - `target/release/deps`: `opencv_world4100.dll`만 존재

## 비고

- 검증 중 기존 release DLL을 실행 중인 프로세스가 잡고 있어 일부 overwrite 경고가 있었지만,
  최종 OpenCV runtime 목록에는 debug/plugin DLL이 남지 않았다.
