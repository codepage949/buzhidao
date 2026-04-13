# OCR Server

`ocr_server`는 PaddleOCR 기반 sidecar executable 프로젝트입니다.

CPU 빌드 환경 준비:

```bash
uv sync --group build --group cpu
```

`paddleocr`가 요구하는 `paddlex[ocr-core]`도 이 단계에서 함께 설치됩니다.

GPU 빌드 환경 준비:

```bash
uv sync --group build --group gpu
```

`gpu` 그룹은 `pyproject.toml`에 설정된
`https://www.paddlepaddle.org.cn/packages/stable/cu118/` 인덱스를 사용합니다.

CPU PyInstaller 빌드:

```bash
uv run --group build --group cpu python build.py
```

GPU PyInstaller 빌드:

```bash
uv run --group build --group gpu python build.py --gpu
```

기본값은 `onedir` 빌드입니다. `onefile`이 필요하면 `--onefile`을 추가합니다.

```bash
uv run --group build --group cpu python build.py --onefile
```

산출물:

```text
ocr_server/dist/ocr_server/ocr_server.exe
```
