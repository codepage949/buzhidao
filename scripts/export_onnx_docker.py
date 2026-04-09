"""PaddleOCR 추론 모델을 다운로드하고 ONNX로 변환하는 컨테이너 내부 스크립트."""

from __future__ import annotations

import json
import shutil
import tarfile
import urllib.request
from pathlib import Path

OUT_DIR = Path("/workspace/out")
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

    print("  압축 해제...")
    with tarfile.open(tar_path) as tf:
        tf.extractall(TMP_DIR)

    tar_path.unlink()

    if not extract_dir.exists():
        dirs = [d for d in TMP_DIR.iterdir() if d.is_dir() and model_name in d.name]
        if dirs:
            extract_dir = dirs[0]

    return extract_dir


def resolve_model_files(inference_dir: Path) -> tuple[Path, Path]:
    pdiparams = list(inference_dir.rglob("*.pdiparams"))
    if not pdiparams:
        raise FileNotFoundError(
            f"파라미터 파일을 찾을 수 없음: {inference_dir}\n"
            f"내용: {list(inference_dir.rglob('*'))}"
        )

    pdmodel = list(inference_dir.rglob("*.pdmodel"))
    if pdmodel:
        return pdmodel[0], pdiparams[0]

    json_models = list(inference_dir.rglob("inference.json"))
    if json_models:
        return json_models[0], pdiparams[0]

    raise FileNotFoundError(
        f"모델 파일을 찾을 수 없음: {inference_dir}\n"
        f"내용: {list(inference_dir.rglob('*'))}"
    )


def convert_to_onnx(inference_dir: Path, output_path: Path, opset: int = 14):
    import paddle2onnx

    model_file, params_file = resolve_model_files(inference_dir)

    print(f"  모델: {model_file}")
    print(f"  파라미터: {params_file}")

    output_path.parent.mkdir(parents=True, exist_ok=True)
    paddle2onnx.export(
        str(model_file),
        str(params_file),
        save_file=str(output_path),
        opset_version=opset,
        enable_onnx_checker=True,
    )
    print(f"  -> {output_path} ({output_path.stat().st_size / 1024 / 1024:.1f} MB)")


def extract_dict_from_yaml(config_file: Path, output_path: Path) -> bool:
    lines = config_file.read_text(encoding="utf-8").splitlines()

    in_character_dict = False
    character_dict_indent = 0
    char_dict: list[str] = []

    for line in lines:
        stripped = line.strip()
        indent = len(line) - len(line.lstrip(" "))
        content = line[indent:]

        if not stripped:
            continue

        if stripped == "character_dict:":
            in_character_dict = True
            character_dict_indent = indent
            continue

        if in_character_dict:
            if indent == 0:
                break
            if indent >= character_dict_indent and content.startswith("- "):
                char_dict.append(content[2:])
                continue
            break

    if not char_dict:
        return False

    with output_path.open("w", encoding="utf-8") as file:
        for ch in char_dict:
            file.write(ch + "\n")
    print(f"  -> {output_path} ({len(char_dict)} chars, from {config_file.name})")
    return True


def extract_dict(inference_dir: Path, output_path: Path):
    output_path.parent.mkdir(parents=True, exist_ok=True)

    dict_files = list(inference_dir.rglob("*dict*"))
    if dict_files:
        shutil.copy2(dict_files[0], output_path)
        line_count = sum(1 for _ in output_path.open(encoding="utf-8"))
        print(f"  -> {output_path} ({line_count} chars)")
        return

    for name in ["inference.yml", "inference.yaml"]:
        for config_file in inference_dir.rglob(name):
            print(f"  config 파일: {config_file}")
            if extract_dict_from_yaml(config_file, output_path):
                return
            print("  [WARN] YAML에서 character_dict를 찾지 못함.")

    config_file: Path | None = None
    for name in ["inference.json", "config.json", "inference_config.json"]:
        for candidate in inference_dir.rglob(name):
            config_file = candidate
            break
        if config_file is not None:
            break

    if config_file is None:
        print("  [WARN] 사전 파일을 찾지 못함. 디렉토리 내용:")
        for path in sorted(inference_dir.rglob("*")):
            if path.is_file():
                print(f"    {path.relative_to(inference_dir)}")
        return

    print(f"  config 파일: {config_file}")
    with config_file.open(encoding="utf-8") as file:
        cfg = json.load(file)

    char_dict = cfg.get("PostProcess", {}).get("character_dict")
    if char_dict is None:
        for value in cfg.values():
            if isinstance(value, dict) and "character_dict" in value:
                char_dict = value["character_dict"]
                break

    if isinstance(char_dict, list):
        with output_path.open("w", encoding="utf-8") as file:
            for ch in char_dict:
                file.write(ch + "\n")
        print(f"  -> {output_path} ({len(char_dict)} chars)")
        return

    if isinstance(char_dict, str):
        candidate = Path(char_dict)
        if not candidate.is_absolute():
            candidate = config_file.parent / candidate
        if candidate.exists():
            shutil.copy2(candidate, output_path)
            print(f"  -> {output_path} (copied from {candidate})")
            return

    print("  [WARN] character_dict를 찾지 못함.")


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    for model_name, short_name in MODELS.items():
        print(f"\n{'=' * 60}")
        print(f"모델: {model_name} -> {short_name}.onnx")
        print(f"{'=' * 60}")

        print("  1. 다운로드 & 압축 해제...")
        inference_dir = download_and_extract(model_name)
        print(f"  inference dir: {inference_dir}")

        print("  2. ONNX 변환...")
        convert_to_onnx(inference_dir, OUT_DIR / f"{short_name}.onnx")

        if short_name == "rec":
            print("  3. 사전 파일 추출...")
            extract_dict(inference_dir, OUT_DIR / "rec_dict.txt")

    if TMP_DIR.exists():
        shutil.rmtree(TMP_DIR)
        print("\n임시 디렉토리 삭제 완료")

    print("\n완료! 모델 파일:")
    for path in sorted(OUT_DIR.iterdir()):
        size = path.stat().st_size
        if size > 1024 * 1024:
            print(f"  {path.name} ({size / 1024 / 1024:.1f} MB)")
        else:
            print(f"  {path.name} ({size / 1024:.1f} KB)")


if __name__ == "__main__":
    main()
