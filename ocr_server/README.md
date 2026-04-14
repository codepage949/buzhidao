# OCR Server

`ocr_server`는 PaddleOCR 기반 sidecar executable 프로젝트입니다.

CPU 빌드 환경 준비:

```bash
uv sync -p 3.13 --group build --group cpu
```

`paddleocr`가 요구하는 `paddlex[ocr-core]`도 이 단계에서 함께 설치됩니다.

GPU 빌드 환경 준비:

```bash
uv sync -p 3.13 --group build --group gpu
```

`gpu` 그룹은 `pyproject.toml`에 설정된
`https://www.paddlepaddle.org.cn/packages/stable/cu118/` 인덱스를 사용합니다.
Windows에서는 같은 인덱스의 `nvidia-* cu11` wheel도 함께 설치해
`cudnn64_8.dll`, `cublas64_11.dll`, `cudart64_110.dll` 등을 번들합니다.

CPU PyInstaller 빌드:

```bash
uv run --group build --group cpu python build.py
```

GPU PyInstaller 빌드:

```bash
uv run --group build --group gpu python build.py --gpu
```

GPU 기본 산출물도 CPU와 같은 경로/파일명에 생성됩니다.

```text
ocr_server/dist/ocr_server/ocr_server.exe
```

GPU import 최소 검증 실행 파일 빌드:

```bash
uv run --group build --group gpu python build.py --gpu --target gpu-import-check
```

빌드 후 실행:

```bash
.\dist\gpu_import_check\gpu_import_check.exe
```

성공하면 `paddle`의 CUDA 활성화 상태와 `paddleocr` import 결과를 JSON 로그로 출력합니다.

기본값은 `onedir` 빌드입니다. `onefile`이 필요하면 `--onefile`을 추가합니다.

```bash
uv run --group build --group cpu python build.py --onefile
```

산출물:

```text
ocr_server/dist/ocr_server/ocr_server.exe
ocr_server/dist/gpu_import_check/gpu_import_check.exe
```
