"""PaddleOCR 추론 모델을 다운로드하고 ONNX로 변환하는 스크립트.
Docker 컨테이너 내부에서 실행한다.

사용법 (호스트에서):
    docker run --rm -v %cd%/app/models:/out -v %cd%/scripts:/scripts python:3.12-slim bash -c \
      "pip install paddle2onnx paddlepaddle && python /scripts/export_onnx_docker.py"

산출물 (/out = app/models/):
    det.onnx, cls.onnx, rec.onnx, rec_dict.txt
"""

from __future__ import annotations

import json
import shutil
import tarfile
import urllib.request
from pathlib import Path

OUT_DIR = Path("/out")
TMP_DIR = Path("/tmp/paddle_export")

BASE_URL = "https://paddle-model-ecology.bj.bcebos.com/paddlex/official_inference_model/paddle3.0.0"

MODELS = {
    "PP-OCRv5_server_det": "det",
    "PP-LCNet_x0_25_textline_ori": "cls",
    "PP-OCRv5_server_rec": "rec",
}


def download_and_extract(model_name: str) -> Path:
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

    with tarfile.open(tar_path) as tf:
        tf.extractall(TMP_DIR)

    tar_path.unlink()

    if not extract_dir.exists():
        dirs = [d for d in TMP_DIR.iterdir() if d.is_dir() and model_name in d.name]
        if dirs:
            extract_dir = dirs[0]

    return extract_dir


def convert_to_onnx(inference_dir: Path, output_path: Path, opset: int = 14):
    import paddle2onnx

    pdiparams = list(inference_dir.rglob("*.pdiparams"))
    if not pdiparams:
        raise FileNotFoundError(f"파라미터 파일 없음: {list(inference_dir.rglob('*'))}")

    # PaddlePaddle 3.x: .pdmodel 또는 inference.json (PIR 형식)
    pdmodel = list(inference_dir.rglob("*.pdmodel"))
    if pdmodel:
        model_file = str(pdmodel[0])
    else:
        # PIR JSON 형식 — paddle2onnx는 inference.json도 지원
        json_files = [f for f in inference_dir.rglob("inference.json")]
        if json_files:
            model_file = str(json_files[0])
        else:
            raise FileNotFoundError(f"모델 파일 없음: {list(inference_dir.rglob('*'))}")

    params_file = str(pdiparams[0])

    print(f"  모델: {model_file}")
    print(f"  파라미터: {params_file}")

    output_path.parent.mkdir(parents=True, exist_ok=True)

    paddle2onnx.export(
        model_file,
        params_file,
        save_file=str(output_path),
        opset_version=opset,
    )

    print(f"  -> {output_path} ({output_path.stat().st_size / 1024 / 1024:.1f} MB)")


def extract_dict(inference_dir: Path, output_path: Path):
    # 직접 dict 파일 검색
    dict_files = list(inference_dir.rglob("*dict*"))
    if dict_files:
        shutil.copy2(dict_files[0], output_path)
        line_count = sum(1 for _ in open(output_path, encoding="utf-8"))
        print(f"  -> {output_path} ({line_count} chars)")
        return

    # config에서 추출
    for name in ["inference.json", "config.json"]:
        for config_file in inference_dir.rglob(name):
            with open(config_file, encoding="utf-8") as f:
                cfg = json.load(f)
            char_dict = cfg.get("PostProcess", {}).get("character_dict")
            if isinstance(char_dict, list):
                output_path.parent.mkdir(parents=True, exist_ok=True)
                with open(output_path, "w", encoding="utf-8") as f:
                    for ch in char_dict:
                        f.write(ch + "\n")
                print(f"  -> {output_path} ({len(char_dict)} chars)")
                return

    print("  [WARN] 사전 파일을 찾지 못함. 디렉토리 내용:")
    for f in sorted(inference_dir.rglob("*")):
        if f.is_file():
            print(f"    {f.relative_to(inference_dir)}")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for model_name, short_name in MODELS.items():
        print(f"\n{'='*60}")
        print(f"모델: {model_name} -> {short_name}.onnx")
        print(f"{'='*60}")

        inference_dir = download_and_extract(model_name)
        convert_to_onnx(inference_dir, OUT_DIR / f"{short_name}.onnx")

        if short_name == "rec":
            extract_dict(inference_dir, OUT_DIR / "rec_dict.txt")

    print(f"\n완료! 모델 파일:")
    for f in sorted(OUT_DIR.iterdir()):
        size = f.stat().st_size
        if size > 1024 * 1024:
            print(f"  {f.name} ({size / 1024 / 1024:.1f} MB)")
        else:
            print(f"  {f.name} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
