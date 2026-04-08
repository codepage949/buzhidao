"""PaddleOCR 추론 모델을 다운로드하고 ONNX로 변환하는 스크립트.

사용법:
    cd scripts && .venv/Scripts/python.exe export_onnx.py

필요 패키지 (scripts/.venv에 설치):
    uv pip install --python scripts/.venv/Scripts/python.exe paddle2onnx

산출물:
    app/models/det.onnx
    app/models/cls.onnx
    app/models/rec.onnx
    app/models/rec_dict.txt
"""

from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tarfile
import urllib.request
from pathlib import Path

PROJ_ROOT = Path(__file__).resolve().parent.parent
APP_MODELS = PROJ_ROOT / "app" / "models"
TMP_DIR = PROJ_ROOT / "scripts" / "_export_tmp"

BASE_URL = "https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0"

MODELS = {
    "PP-OCRv5_server_det": "det",
    "PP-LCNet_x0_25_textline_ori": "cls",
    "PP-OCRv5_server_rec": "rec",
}


def download_and_extract(model_name: str) -> Path:
    """모델 tar를 다운로드하고 압축을 풀어 디렉토리를 반환한다."""
    tar_url = f"{BASE_URL}/{model_name}_infer.tar"
    tar_path = TMP_DIR / f"{model_name}_infer.tar"
    extract_dir = TMP_DIR / f"{model_name}_infer"

    if extract_dir.exists():
        print(f"  이미 존재: {extract_dir}")
        return extract_dir

    TMP_DIR.mkdir(parents=True, exist_ok=True)

    print(f"  다운로드: {tar_url}")
    urllib.request.urlretrieve(tar_url, tar_path)
    print(f"  다운로드 완료: {tar_path.stat().st_size / 1024 / 1024:.1f} MB")

    print(f"  압축 해제...")
    with tarfile.open(tar_path) as tf:
        tf.extractall(TMP_DIR)

    tar_path.unlink()

    # tar 내부에 중첩 디렉토리가 있을 수 있음
    if not extract_dir.exists():
        # 첫 번째 디렉토리 찾기
        dirs = [d for d in TMP_DIR.iterdir() if d.is_dir() and model_name in d.name]
        if dirs:
            extract_dir = dirs[0]

    return extract_dir


def convert_to_onnx(inference_dir: Path, output_path: Path, opset: int = 14):
    """paddle2onnx CLI로 변환."""
    pdmodel = list(inference_dir.glob("*.pdmodel"))
    pdiparams = list(inference_dir.glob("*.pdiparams"))

    if not pdmodel:
        # 하위 디렉토리 탐색
        pdmodel = list(inference_dir.rglob("*.pdmodel"))
        pdiparams = list(inference_dir.rglob("*.pdiparams"))

    if not pdmodel or not pdiparams:
        raise FileNotFoundError(
            f"모델 파일을 찾을 수 없음: {inference_dir}\n"
            f"내용: {list(inference_dir.rglob('*'))}"
        )

    output_path.parent.mkdir(parents=True, exist_ok=True)

    cmd = [
        sys.executable, "-m", "paddle2onnx",
        "--model_dir", str(pdmodel[0].parent),
        "--model_filename", pdmodel[0].name,
        "--params_filename", pdiparams[0].name,
        "--save_file", str(output_path),
        "--opset_version", str(opset),
        "--enable_onnx_checker", "True",
    ]
    print(f"  명령: {' '.join(cmd)}")
    subprocess.check_call(cmd)
    print(f"  -> {output_path} ({output_path.stat().st_size / 1024 / 1024:.1f} MB)")


def extract_dict(inference_dir: Path, output_path: Path):
    """inference config에서 character_dict를 추출한다."""
    # inference.json 또는 inference_config.json 탐색
    for name in ["inference.json", "config.json", "inference_config.json"]:
        config_file = inference_dir / name
        if not config_file.exists():
            for f in inference_dir.rglob(name):
                config_file = f
                break
        if config_file.exists():
            break

    if not config_file.exists():
        # YAML 파일 탐색
        yml_files = list(inference_dir.rglob("*.yml")) + list(inference_dir.rglob("*.yaml"))
        for yf in yml_files:
            print(f"  YAML 발견: {yf}")

        # dict 파일 직접 탐색
        dict_files = list(inference_dir.rglob("*dict*"))
        if dict_files:
            for df in dict_files:
                print(f"  사전 파일 발견: {df}")
                shutil.copy2(df, output_path)
                print(f"  -> {output_path}")
                return

        print(f"  [WARN] config 파일을 찾을 수 없음. 디렉토리 내용:")
        for f in sorted(inference_dir.rglob("*")):
            print(f"    {f}")
        return

    print(f"  config 파일: {config_file}")
    with open(config_file, encoding="utf-8") as f:
        cfg = json.load(f)

    # PostProcess.character_dict 경로 탐색
    char_dict = cfg.get("PostProcess", {}).get("character_dict")
    if char_dict is None:
        # 중첩 구조 탐색
        for key in cfg:
            if isinstance(cfg[key], dict) and "character_dict" in cfg[key]:
                char_dict = cfg[key]["character_dict"]
                break

    if char_dict is None:
        print(f"  [WARN] character_dict를 찾을 수 없음. config 키: {list(cfg.keys())}")
        # config 전체 출력
        print(f"  config 내용: {json.dumps(cfg, indent=2, ensure_ascii=False)[:2000]}")
        return

    output_path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(char_dict, list):
        with open(output_path, "w", encoding="utf-8") as f:
            for ch in char_dict:
                f.write(ch + "\n")
        print(f"  -> {output_path} ({len(char_dict)} chars)")
    elif isinstance(char_dict, str) and Path(char_dict).exists():
        shutil.copy2(char_dict, output_path)
        print(f"  -> {output_path} (copied from {char_dict})")
    else:
        print(f"  [WARN] character_dict 형식 미지원: {type(char_dict)}")


def main():
    APP_MODELS.mkdir(parents=True, exist_ok=True)

    for model_name, short_name in MODELS.items():
        print(f"\n{'='*60}")
        print(f"모델: {model_name} -> {short_name}.onnx")
        print(f"{'='*60}")

        print("  1. 다운로드 & 압축 해제...")
        inference_dir = download_and_extract(model_name)
        print(f"  inference dir: {inference_dir}")

        print("  2. ONNX 변환...")
        convert_to_onnx(inference_dir, APP_MODELS / f"{short_name}.onnx")

        if short_name == "rec":
            print("  3. 사전 파일 추출...")
            extract_dict(inference_dir, APP_MODELS / "rec_dict.txt")

    # cleanup
    if TMP_DIR.exists():
        shutil.rmtree(TMP_DIR)
        print("\n임시 디렉토리 삭제 완료")

    print(f"\n완료! 모델 파일:")
    for f in sorted(APP_MODELS.iterdir()):
        size = f.stat().st_size
        if size > 1024 * 1024:
            print(f"  {f.name} ({size / 1024 / 1024:.1f} MB)")
        else:
            print(f"  {f.name} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
