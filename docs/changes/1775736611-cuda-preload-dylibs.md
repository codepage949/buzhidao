# CUDA DLL 선탐색: 툴킷 미설치 환경 지원

## 배경

NVIDIA GPU가 있어도 CUDA 툴킷을 설치하지 않으면 `cudart64_12.dll` 등
런타임 DLL이 없어서 CUDA EP가 동작하지 않는다.

nvidia-smi의 "CUDA Version" 표시는 드라이버가 지원하는 최대 버전이며
툴킷(cudart, cublas 등) 설치 여부와 무관하다.

## 해결 전략

CUDA DLL을 번들로 배포하는 방식은 일반적인 관행이다.
NVIDIA는 cudart, cublas, cufft 등 핵심 런타임 DLL의 재배포를 허용한다.
(Ollama, llama.cpp 기반 앱 등 많은 AI 앱이 동일 방식을 사용한다.)

`ort::ep::cuda::preload_dylibs`를 이용하면 시스템 PATH보다 특정 디렉토리를
우선 탐색하도록 할 수 있다.

## 변경 내용

### `Cargo.toml`
```
gpu = ["ort/cuda", "ort/preload-dylibs"]
```
- `ort/preload-dylibs`: `ep::cuda::preload_dylibs` API 활성화

### `src/lib.rs` — `preload_cuda_dylibs_early()`

`run()` 진입 시, OcrEngine(세션 생성) 이전에 호출된다.

탐색 순서:
1. `<실행파일 디렉토리>/cuda/` — 번들 배포 시 DLL 위치
2. `CUDA_PATH` 또는 `CUDA_HOME` 환경변수 `→ {경로}/bin` — 개발 시 툴킷 설치 경로
3. 아무것도 없으면 호출 생략 — ORT가 시스템 PATH에서 자동 탐색

## 사용 방법

### 개발 환경: CUDA 툴킷 설치
CUDA 12 툴킷을 설치하면 자동으로 `CUDA_PATH`가 설정되고
`{CUDA_PATH}/bin/cudart64_12.dll` 등이 탐색된다.

또는 `target/debug/cuda/`에 DLL을 직접 복사해도 된다.

### 배포: DLL 번들
다음 DLL을 `<앱설치 디렉토리>/cuda/`에 위치시킨다:

**필수 (CUDA 런타임)**
- `cudart64_12.dll`
- `cublasLt64_12.dll`
- `cublas64_12.dll`
- `cufft64_11.dll`

**권장 (cuDNN — Conv 연산 가속)**
- `cudnn64_9.dll`
- `cudnn_ops64_9.dll`
- `cudnn_cnn64_9.dll`
- 기타 cudnn_*.dll

출처: CUDA 12 및 cuDNN 9 설치 디렉토리의 `bin/` 폴더.
NVIDIA Redistributable License 하에 재배포 가능.

## 테스트 결과

- `cargo test` — 43개 통과
- `cargo test --features gpu` — 44개 통과
