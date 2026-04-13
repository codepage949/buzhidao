# app/ocr_server 구조 분리와 Python Paddle OCR 통합

## 배경

`benchmarks/1.png` 기준 비교에서 이전 Python Paddle 서버 경로가
현재 Rust ONNX 경로보다 속도와 인식률 모두 더 좋았다.

특히 이전 버전은 다음 조합으로 동작했다.

- `paddlepaddle==3.2.2`
- `paddleocr==3.3.0`
- `use_doc_orientation_classify=False`
- `use_doc_unwarping=False`
- `use_textline_orientation=True`
- 요청 전에 이미지를 `1024w`로 축소
- 장기 실행 프로세스에서 `predict()` 호출

## 목표

1. 현재 앱의 OCR 경로를 Python sidecar 하나로 단순화한다.
2. 예전 서버처럼 OCR 입력 이미지를 `1024w` 기준으로 축소한다.
3. 축소된 좌표를 다시 원본 화면 좌표로 복원한다.
4. 영역 선택 OCR 코드는 남기되 일반 사용자 흐름에서는 숨긴다.
5. Tauri 앱과 OCR 서버를 디렉토리 단위로 분리해 관리 범위를 명확히 한다.
6. PyInstaller 빌드 산출물을 앱에서 바로 사용할 수 있게 정리한다.

## 변경 내용

### 프로젝트 구조

- 루트 Tauri 앱을 `app/` 아래로 이동
- OCR sidecar를 `ocr_server/` 프로젝트로 분리
- 루트 기준 실행/문서 흐름을 `app/`와 `ocr_server/` 두 축으로 재정리

### `app/src/config.rs`

- `OCR_SERVER_EXECUTABLE` 설정 추가
- `OCR_SERVER_DEVICE` 설정 추가 (`cpu` / `gpu`, 기본 `cpu`)
- 잘못된 `OCR_SERVER_DEVICE` 값은 앱 시작 전에 즉시 실패하도록 검증
- OCR server startup/request timeout 설정 추가
- OCR 입력 `1024w` 기준 상수 추가

### `app/Cargo.toml`

- `ort`, `ndarray`, `rayon` 의존성 제거
- GPU feature 제거

### `app/src/ocr/mod.rs`

- OCR backend를 Python sidecar 단일 구현으로 축소
- ONNX det/cls/rec 경로 제거

### `app/src/ocr/python_sidecar.rs`

- `ocr_server.exe` persistent process manager 추가
- stdio JSONL 프로토콜로 요청/응답 처리
- 실패 시 프로세스를 폐기하고 다음 요청에서 재기동
- dev 기본 실행 파일 경로를 `../ocr_server/dist/ocr_server/ocr_server.exe`로 사용
- 번들 실행 시에는 Tauri resource dir의 `ocr_server.exe`를 우선 사용
- onedir 번들 리소스에서는 `resource_dir/ocr_server/ocr_server.exe`도 해석하도록 보강
- sidecar spawn 시 `PYTHONUTF8=1`, `PYTHONIOENCODING=utf-8`를 주입
- sidecar spawn 시 `OCR_SERVER_DEVICE` 값을 `PYTHON_OCR_DEVICE`로 전달

### `ocr_server/`

- PaddleOCR 3.3.0 기준 `ocr_server.py` 추가
- GPU import 최소 검증용 `gpu_import_check.py` 추가
- `pyproject.toml`, `build.py`, `README.md` 추가
- `uv` dependency group을 `cpu` / `gpu`로 분리
- CPU 빌드는 `paddlepaddle==3.2.2`, GPU 빌드는 `paddlepaddle-gpu==3.2.2` 사용
- GPU 의존성은 `https://www.paddlepaddle.org.cn/packages/stable/cu118/` 인덱스로 고정
- Windows GPU 의존성은 같은 Paddle `cu118` 인덱스의 `nvidia-* cu11` wheel로 고정
- Windows에서는 `nvidia-cudnn-cu11==8.9.4.19`를 사용해 `cudnn64_8.dll`을 번들
- `uv sync --group build --group cpu`, `uv run --group build --group cpu python build.py` 기준 정리
- GPU 빌드는 `uv sync --group build --group gpu`, `uv run --group build --group gpu python build.py --gpu`
- GPU PyInstaller 산출물은 CPU와 분리된 `dist/ocr_server_gpu/ocr_server_gpu.exe` 경로로 생성
- `uv run --group build --group gpu python build.py --gpu --target gpu-import-check`로 최소 import 검증 exe를 별도 생성
- 최소 검증 exe는 `paddle`의 CUDA 활성화 상태와 `paddleocr` import 가능 여부를 JSON 로그로 출력
- 기본 PyInstaller 산출물은 `onedir`로 변경
- `--onefile`은 선택 옵션으로만 유지
- `doc orientation / unwarp off`, `textline orientation on`
- `predict()` 기반 persistent `--server` 모드 지원
- 시작 시 `en`, `ch` 모델 preload + warmup 수행
- 잘못된 `PYTHON_OCR_DEVICE` 값은 OCR 서버 시작 전에 즉시 실패하도록 검증
- PyInstaller에서 `paddlex`, `paddleocr`, `paddle` 데이터/바이너리/metadata를 수집하도록 보강
- GPU 빌드에서는 `nvidia.cublas`, `nvidia.cuda_runtime`, `nvidia.cudnn` 등 `nvidia/*/bin` DLL도 함께 수집
- frozen 실행 시 `_MEIPASS/paddle/libs`, `_MEIPASS/nvidia/*/bin` 등을 DLL search path에 추가
- stdout/stderr를 UTF-8 `replace`로 재설정해 Windows `cp949` 인코딩 오류를 피함

### `app/src/services.rs`

- Python backend 입력 이미지를 `1024w` 기준으로 축소
- OCR 결과 polygon을 원본 화면 좌표로 다시 스케일 복원
- ORT용 `det_resize_long` 로직 제거

### `app/.env.example`

- OCR server 실행 파일 경로 예시만 남김
- `OCR_SERVER_DEVICE` 예시 추가

### `app/ui/src/overlay.tsx`

- 결과 화면에서 빈 영역 드래그로 재선택이 시작되지 않게 변경
- 영역 선택 UI는 `overlay_select_region` 이벤트가 있을 때만 동작

## 기대 효과

- 더 이상 ORT runtime, ONNX 모델, CPU/GPU backend 분기를 관리하지 않아도 된다.
- 예전 서버와 같은 `1024w` 축소 전략을 현재 앱에서도 재현한다.
- 결과 좌표는 원본 화면 기준으로 유지되어 overlay 동작이 깨지지 않는다.
- 영역 선택 OCR 기능은 코드에 남지만 일반 사용자는 보지 않게 된다.
- 프로젝트 구조가 `app/`와 `ocr_server/`로 분리되어 관리 범위가 명확해진다.
- `ocr_server`가 PyInstaller 산출물만으로 기동 가능해지고, Windows에서 필요한 DLL/metadata 누락 문제를 줄인다.
- 기본 `onedir` 빌드로 `onefile` 대비 시작 비용을 줄일 수 있다.

## 검증

- `cd app && cargo check`
- `cd app && cargo test`
- `cd app/ui && deno task test`
- `python -m py_compile ocr_server/build.py ocr_server/ocr_server.py`
- `python -m py_compile ocr_server/build.py ocr_server/ocr_server.py ocr_server/gpu_import_check.py`
- `uv sync -p 3.13 --group build --group gpu`
- `uv run --group build --group cpu python build.py`
- `uv run --group build --group gpu python build.py --gpu`
- `uv run --group build --group gpu python build.py --gpu --target gpu-import-check`
- `ocr_server/dist/gpu_import_check/gpu_import_check.exe` 실행 시 `paddle` CUDA 활성화 상태와 `paddleocr` import 로그 확인
- `cd app && cargo test`
- `ocr_server/dist/ocr_server/ocr_server.exe --server` 실행 시 `ready` 응답 확인
- `ocr_server/dist/ocr_server_gpu/ocr_server_gpu.exe --server` 실행 유지 확인
