# GPU 런타임 및 Paddle Inference 구성 옵션 추가

## 배경

- 과거 릴리스 workflow에는 NVIDIA wheel에서 CUDA/cuDNN DLL을 추출해 `cuda/` 디렉토리를 구성하는 `extract-cuda` 기능이 있었다.
- 현재 `tools/scripts/release_helper.py`에는 해당 기능이 남아 있지 않고, 로컬에서 재사용하기 어렵다.
- 사용자가 요청한 대상은 `.cuda` 디렉토리에 필요한 라이브러리를 구성하는 독립 스크립트다.
- Windows와 Linux만 지원하면 된다.
- GPU 앱 빌드는 CUDA 런타임만으로는 부족하고, Paddle Inference SDK 자체도 GPU 빌드로 구성되어야 한다.

## 구현 계획

1. 기존 문서와 이전 커밋의 `extract-cuda` 구현을 참고한다.
2. `tools/scripts/setup_cuda_runtime.py`를 추가한다.
3. 기본값은 현재 OS를 감지해 Windows/Linux만 허용한다.
4. `pip download`로 NVIDIA wheel을 wheelhouse에 내려받고, wheel 내부 `nvidia/*/bin` 또는 `nvidia/*/lib`에서 동적 라이브러리를 추출한다.
5. 다운로드 없이 기존 wheelhouse만 사용하는 `--no-download` 경로를 지원해 테스트 가능하게 만든다.
6. `tools/scripts/setup_paddle_inference.py`에 GPU SDK 선택 옵션을 추가한다.
7. GPU 빌드에서 `.cuda` 런타임 경로를 자동 반영한다.
8. 핵심 추출/URL 선택 로직을 단위 테스트로 검증한다.

## 구현 내용

### CUDA 런타임

- `tools/scripts/setup_cuda_runtime.py`를 추가했다.
- 기본 출력 디렉토리는 `.cuda`이고, `--destination-dir`로 변경할 수 있다.
- 기본 wheelhouse는 `.cuda-wheelhouse`이고, `--wheelhouse`로 변경할 수 있다.
- 기본 package set은 `ort-cu12`다.
  - `nvidia-cuda-runtime-cu12==12.9.79`
  - `nvidia-cublas-cu12==12.9.1.4`
  - `nvidia-cufft-cu12==11.4.1.4`
  - `nvidia-cudnn-cu12==9.20.0.48`
- `paddle-cu126` package set을 제공한다.
  - Paddle Inference GPU CUDA 12.6 배포본에 맞춰 CUDA 12.6 + cuDNN 9.5.1 계열 NVIDIA wheel을 사용한다.
- Windows에서는 wheel 내부 `nvidia/*/bin/*.dll`만 추출한다.
- Linux에서는 wheel 내부 `nvidia/*/lib/*.so*`만 추출한다.
- GPU feature 빌드에서는 `build.rs`가 `.cuda`의 DLL/SO를 Cargo 프로필 출력 폴더로 복사한다.
- 앱 시작 시 Windows는 `.cuda`를 `PATH` 앞쪽에 추가하고, Linux는 `.cuda`를 `LD_LIBRARY_PATH` 앞쪽에 추가한다.
  - Paddle Inference가 `cudnn64_9.dll` 등 CUDA/cuDNN 라이브러리를 동적 로드할 때 `.cuda`를 찾지 못하는 문제를 막기 위함이다.
- `--no-download`를 지원해 기존 wheelhouse에서 추출만 수행할 수 있다.
- `--clean`을 지원해 대상 디렉토리와 wheelhouse를 재구성할 수 있다.
- `--package`를 여러 번 지정하면 기본 package set 대신 직접 지정한 pip package spec을 사용한다.
- `tools/scripts/test_setup_cuda_runtime.py`를 추가해 Windows/Linux 라이브러리 판별과 추출을 검증한다.

### Paddle Inference SDK

- `tools/scripts/setup_paddle_inference.py`에 `--device cpu|gpu` 옵션을 추가했다.
- 기존 기본값은 `--device cpu`로 유지한다.
- `--device gpu`는 Windows/Linux x86_64만 지원한다.
- GPU SDK는 CUDA 12.6/cuDNN 9.5.1 계열만 지원한다.
- Windows/Linux 모두 Paddle Inference 3.2.2 CUDA 12.6 배포본을 사용한다.
- CPU/GPU 아카이브 캐시가 충돌하지 않도록 다운로드 파일명에 device와 CUDA 변형을 포함한다.

## 확인한 Paddle Inference GPU SDK

- Windows CUDA 12.6: `3.2.2/cxx_c/Windows/GPU/x86-64_cuda12.6_cudnn9.5.1_trt10.5.0.18_mkl_avx_vs2019/paddle_inference.zip`
- Linux CUDA 12.6: `3.2.2/cxx_c/Linux/GPU/x86-64_gcc11.2_avx_mkl_cuda12.6_cudnn9.5.1-trt10.5.0.18/paddle_inference.tgz`

## 사용 예시

```powershell
python tools/scripts/setup_cuda_runtime.py --platform windows --destination-dir .cuda --clean
python tools/scripts/setup_cuda_runtime.py --package-set paddle-cu126 --destination-dir .cuda --clean
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir .paddle_inference
```

```bash
python tools/scripts/setup_cuda_runtime.py --platform linux --destination-dir .cuda --clean
python tools/scripts/setup_cuda_runtime.py --no-download --wheelhouse wheelhouse --destination-dir .cuda
python tools/scripts/setup_paddle_inference.py --device gpu --destination-dir .paddle_inference
```

## 검증

- `python -m unittest tools.scripts.test_setup_cuda_runtime`
  - 통과
  - 7개 테스트 통과
- `python -m unittest tools.scripts.test_setup_paddle_inference`
  - 통과
  - 16개 테스트 통과
- `python -m py_compile tools/scripts/setup_cuda_runtime.py tools/scripts/test_setup_cuda_runtime.py tools/scripts/setup_paddle_inference.py tools/scripts/test_setup_paddle_inference.py`
  - 통과
- `cargo test --features gpu default_env_example -- --nocapture`
  - 통과
- `target/debug/cudnn64_9.dll`, `target/debug/deps/cudnn64_9.dll` 복사 확인
  - 통과
