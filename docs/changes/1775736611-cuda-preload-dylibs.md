# CUDA + cuDNN DLL 번들: 툴킷 미설치 환경에서 GPU 가속

## 배경

NVIDIA GPU가 있어도 CUDA 툴킷을 설치하지 않으면 `cudart64_12.dll` 등
런타임 DLL이 없어서 CUDA EP가 동작하지 않는다.

nvidia-smi의 "CUDA Version" 표시는 드라이버가 지원하는 최대 버전이며
툴킷(cudart, cublas 등) 설치 여부와 무관하다.

또한 CUDA DLL만으로는 부족하다. PaddleOCR 모델은 Conv2D 연산이 지배적인데,
CUDA EP는 Conv2D를 cuDNN 없이는 실행할 수 없어 cuDNN이 없으면
모든 노드가 CPU로 폴백된다.

## 해결 전략

CUDA DLL을 번들로 배포하는 방식은 일반적인 관행이다.
NVIDIA는 cudart, cublas, cufft, cuDNN 등 핵심 런타임 DLL의 재배포를 허용한다.
(Ollama, llama.cpp 기반 앱 등 많은 AI 앱이 동일 방식을 사용한다.)

`ort::ep::cuda::preload_dylibs(cuda_dir, cudnn_dir)`를 이용하면
시스템 PATH보다 특정 디렉토리를 우선 탐색하도록 할 수 있다.

CUDA DLL과 cuDNN DLL은 동일한 `cuda/` 디렉토리에 함께 둔다.

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
2. `CUDA_PATH` 또는 `CUDA_HOME` 환경변수 → `{경로}/bin` — 개발 시 툴킷 설치 경로
3. 아무것도 없으면 호출 생략 — ORT가 시스템 PATH에서 자동 탐색

CUDA와 cuDNN 모두 같은 디렉토리에서 로드한다:
```rust
cuda::preload_dylibs(Some(dir), Some(dir))
```

## 번들 DLL 목록

모두 `cuda/` 디렉토리 하나에 위치시킨다.

**CUDA 런타임** (PyPI `nvidia-cuda-runtime-cu12`, `nvidia-cublas-cu12`, `nvidia-cufft-cu12`)
- `cudart64_12.dll` (~0.6 MB)
- `cublas64_12.dll` (~98 MB)
- `cublasLt64_12.dll` (~638 MB)
- `cufft64_11.dll` (~274 MB)

**cuDNN 9** (PyPI `nvidia-cudnn-cu12`) — Conv2D 가속에 필수
- `cudnn64_9.dll`
- `cudnn_ops64_9.dll`
- `cudnn_cnn64_9.dll`
- `cudnn_heuristic64_9.dll`
- `cudnn_adv64_9.dll`
- `cudnn_graph64_9.dll`
- `cudnn_engines_precompiled64_9.dll`
- `cudnn_engines_runtime_compiled64_9.dll`

출처: PyPI wheel에서 추출. NVIDIA Redistributable License 하에 재배포 가능.

## 개발 환경 DLL 취득 방법

```powershell
# PyPI wheel (ZIP 포맷)에서 DLL 추출
$pkgs = @(
    @{ url='...nvidia_cuda_runtime_cu12...win_amd64.whl' },
    @{ url='...nvidia_cublas_cu12...win_amd64.whl' },
    @{ url='...nvidia_cufft_cu12...win_amd64.whl' },
    @{ url='...nvidia_cudnn_cu12...win_amd64.whl' }
)
# 각 whl을 zip으로 저장 후 Expand-Archive, nvidia/**/*.dll 을 cuda/ 로 복사
```

## 테스트 결과

- `cargo test` — 43개 통과
- `cargo test --features gpu` — 44개 통과
