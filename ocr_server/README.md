# OCR Server

`ocr_server`는 Buzhidao 앱이 호출하는 PaddleOCR 기반 sidecar executable 프로젝트입니다. PyInstaller로 실행 파일을 만들고, 앱에서는 이 실행 파일을 별도 프로세스로 띄워 OCR을 수행합니다.

## 기술 스택

- Python 3.13 + uv
- PaddleOCR
- PaddleX
- PaddlePaddle / PaddlePaddle GPU
- PyInstaller

## 프로젝트 구조

```text
ocr_server/
├── ocr_server.py         # OCR sidecar 진입점
├── build.py              # PyInstaller 빌드 스크립트
├── gpu_import_check.py   # GPU import 최소 검증용 엔트리
├── pyproject.toml        # 의존성 / 그룹 설정
├── tests/                # 순수 Python 테스트
└── README.md
```

## 개발 및 테스트 방법

CPU 의존성 준비:

```bash
uv sync -p 3.13 --group build --group cpu
```

GPU 의존성 준비:

```bash
uv sync -p 3.13 --group build --group gpu
```

단위 테스트:

```bash
uv run python -m unittest tests.test_pure
```

## 릴리즈 배포 방법

CPU 빌드:

```bash
uv run --group build --group cpu python build.py
```

GPU 빌드:

```bash
uv run --group build --group gpu python build.py --gpu
```

GPU import 최소 검증 빌드:

```bash
uv run --group build --group gpu python build.py --gpu --target gpu-import-check
```

기본 산출물:

```text
ocr_server/dist/ocr_server/ocr_server.exe
ocr_server/dist/gpu_import_check/gpu_import_check.exe
```

기본 PyInstaller 모드는 `onedir`이고, 필요하면 `--onefile`을 추가할 수 있습니다.

## 각 기능 설명

### OCR sidecar 실행

- 앱이 sidecar 프로세스를 실행하면 `ocr_server.py`가 진입점이 됩니다.
- Windows에서는 Paddle DLL probing을 위해 실행 전 환경을 조정합니다.
- 시작 시 OCR 모델을 로드하고 warmup을 수행합니다.

### OCR 요청 처리

- 앱은 이미지 경로, 언어, score threshold가 포함된 요청을 sidecar에 보냅니다.
- sidecar는 PaddleOCR로 예측하고, 텍스트/박스 결과를 표준 출력 기반 프로토콜로 반환합니다.
- sidecar 실행 파일이 없거나 실행에 실패하면 앱 쪽에 오류를 전달합니다.

### 디바이스 선택

- 런타임 `OCR_SERVER_DEVICE` 값에 따라 `cpu` 또는 `gpu` 경로를 선택합니다.
- 잘못된 값은 테스트로 검증되는 실패 경로를 가집니다.

### GPU import 검증

- `gpu_import_check.py`는 Paddle/PaddleOCR import와 CUDA 활성화 여부를 빠르게 확인하는 용도입니다.
- Windows GPU 번들에 필요한 CUDA/cuDNN DLL 포함 여부를 확인할 때 유용합니다.

## 특이 사항

- GPU 그룹은 Paddle 전용 `cu118` 인덱스를 사용합니다.
- Windows GPU 빌드에서는 `nvidia-* cu11` wheel도 함께 설치해 필요한 DLL을 번들합니다.
- 앱은 설정된 실행 경로를 우선 사용하고, 없으면 번들 리소스나 앱 옆 `ocr_server` 폴더를 fallback으로 탐색합니다.
