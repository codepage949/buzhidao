"""ONNX 모델로 PaddleOCR 방식 전처리를 적용해 참조 결과를 생성한다.

사용법:
    pip install onnxruntime numpy opencv-python Pillow
    python scripts/compare_onnx.py test.jpg
"""

from __future__ import annotations

import sys
from pathlib import Path

import cv2
import numpy as np
import onnxruntime as ort

MODELS_DIR = Path(__file__).resolve().parent.parent / "app" / "models"

# det 파라미터 (PaddleOCR inference.yml 기반)
DET_THRESH = 0.3
DET_BOX_THRESH = 0.6
DET_UNCLIP_RATIO = 1.5
DET_MAX_CANDIDATES = 1000
DET_MIN_SIZE = 3.0

MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)


# ── det ──────────────────────────────────────────────────────────────────────

def det_preprocess(img_bgr: np.ndarray) -> tuple[np.ndarray, int, int]:
    """PaddleOCR det 전처리: resize(960, 128배수) + normalize(BGR, ImageNet)"""
    h, w = img_bgr.shape[:2]
    resize_long = 960
    ratio = resize_long / max(h, w)
    new_h = int(h * ratio)
    new_w = int(w * ratio)

    stride = 128
    new_h = ((new_h + stride - 1) // stride) * stride
    new_w = ((new_w + stride - 1) // stride) * stride

    resized = cv2.resize(img_bgr, (new_w, new_h))

    # normalize: BGR 순서 그대로, mean/std를 채널 위치 순서로 적용
    img_f = resized.astype(np.float32) / 255.0
    img_f = (img_f - MEAN) / STD

    # HWC → CHW, batch 추가
    tensor = img_f.transpose(2, 0, 1)[np.newaxis, ...]
    return tensor.astype(np.float32), new_h, new_w


def det_postprocess(
    pred: np.ndarray, pred_h: int, pred_w: int, src_h: int, src_w: int
) -> list[np.ndarray]:
    """DB 후처리: 히트맵 → 바운딩 박스"""
    bitmap = (pred > DET_THRESH).astype(np.uint8)
    contours, _ = cv2.findContours(bitmap, cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE)

    w_scale = src_w / pred_w
    h_scale = src_h / pred_h

    boxes = []
    for contour in contours[:DET_MAX_CANDIDATES]:
        if len(contour) < 4:
            continue

        rect = cv2.minAreaRect(contour)
        box = cv2.boxPoints(rect)
        sside = min(rect[1])
        if sside < DET_MIN_SIZE:
            continue

        # box score
        mask = np.zeros((pred_h, pred_w), dtype=np.uint8)
        cv2.fillPoly(mask, [contour], 1)
        score = pred[mask == 1].mean()
        if score < DET_BOX_THRESH:
            continue

        # unclip
        area = cv2.contourArea(contour)
        peri = cv2.arcLength(contour, True)
        if peri < 1e-6:
            continue
        distance = area * DET_UNCLIP_RATIO / peri

        # 센터 기준 확장
        cx, cy = box.mean(axis=0)
        expanded = []
        for pt in box:
            dx, dy = pt[0] - cx, pt[1] - cy
            dist = np.sqrt(dx**2 + dy**2)
            if dist < 1e-6:
                expanded.append(pt)
            else:
                scale = (dist + distance) / dist
                expanded.append([cx + dx * scale, cy + dy * scale])
        expanded = np.array(expanded, dtype=np.float32)

        # 원본 좌표로 변환
        expanded[:, 0] = np.clip(expanded[:, 0] * w_scale, 0, src_w)
        expanded[:, 1] = np.clip(expanded[:, 1] * h_scale, 0, src_h)

        exp_rect = cv2.minAreaRect(expanded)
        if min(exp_rect[1]) < DET_MIN_SIZE + 2:
            continue

        final_box = cv2.boxPoints(exp_rect)
        boxes.append(final_box)

    return boxes


def run_det(session: ort.InferenceSession, img_bgr: np.ndarray) -> list[np.ndarray]:
    src_h, src_w = img_bgr.shape[:2]
    tensor, pred_h, pred_w = det_preprocess(img_bgr)

    input_name = session.get_inputs()[0].name
    output = session.run(None, {input_name: tensor})
    pred = output[0][0, 0]  # [1,1,H,W] → [H,W]

    return det_postprocess(pred, pred_h, pred_w, src_h, src_w)


# ── cls ──────────────────────────────────────────────────────────────────────

def cls_preprocess(img_bgr: np.ndarray) -> np.ndarray:
    resized = cv2.resize(img_bgr, (160, 80))
    img_f = resized.astype(np.float32) / 255.0
    img_f = (img_f - MEAN) / STD
    tensor = img_f.transpose(2, 0, 1)[np.newaxis, ...]
    return tensor.astype(np.float32)


def run_cls(session: ort.InferenceSession, img_bgr: np.ndarray) -> int:
    tensor = cls_preprocess(img_bgr)
    input_name = session.get_inputs()[0].name
    output = session.run(None, {input_name: tensor})
    return int(np.argmax(output[0]))


# ── rec ──────────────────────────────────────────────────────────────────────

def rec_preprocess(img_bgr: np.ndarray) -> np.ndarray:
    h, w = img_bgr.shape[:2]
    target_h = 48
    ratio = w / h
    target_w = min(int(target_h * ratio + 0.5), 3200)

    resized = cv2.resize(img_bgr, (target_w, target_h))
    img_f = resized.astype(np.float32) / 255.0 / 0.5 - 1.0
    tensor = img_f.transpose(2, 0, 1)[np.newaxis, ...]
    return tensor.astype(np.float32)


def ctc_decode(logits: np.ndarray, dictionary: list[str]) -> tuple[str, float]:
    """CTC 디코딩"""
    time_steps = logits.shape[0]
    text = []
    scores = []
    prev_idx = -1

    for t in range(time_steps):
        idx = int(np.argmax(logits[t]))
        val = float(logits[t, idx])

        if idx != 0 and idx != prev_idx:
            if idx - 1 < len(dictionary):
                text.append(dictionary[idx - 1])
            scores.append(val)
        prev_idx = idx

    avg_score = sum(scores) / len(scores) if scores else 0.0
    return "".join(text), avg_score


def run_rec(
    session: ort.InferenceSession, img_bgr: np.ndarray, dictionary: list[str]
) -> tuple[str, float]:
    tensor = rec_preprocess(img_bgr)
    input_name = session.get_inputs()[0].name
    output = session.run(None, {input_name: tensor})

    logits = output[0]
    if logits.ndim == 3:
        logits = logits[0]  # [1, T, C] → [T, C]

    return ctc_decode(logits, dictionary)


# ── pipeline ─────────────────────────────────────────────────────────────────

def crop_box(img: np.ndarray, box: np.ndarray) -> np.ndarray:
    x_min = max(0, int(box[:, 0].min()))
    y_min = max(0, int(box[:, 1].min()))
    x_max = min(img.shape[1], int(np.ceil(box[:, 0].max())))
    y_max = min(img.shape[0], int(np.ceil(box[:, 1].max())))
    return img[y_min:y_max, x_min:x_max]


def main():
    if len(sys.argv) < 2:
        print(f"사용법: python {sys.argv[0]} <이미지 경로>")
        sys.exit(1)

    img_path = sys.argv[1]
    img_bgr = cv2.imread(img_path)
    if img_bgr is None:
        print(f"이미지 로드 실패: {img_path}")
        sys.exit(1)

    print(f"이미지: {img_path} ({img_bgr.shape[1]}x{img_bgr.shape[0]})")

    # 세션 로드
    det_sess = ort.InferenceSession(str(MODELS_DIR / "det.onnx"))
    cls_sess = ort.InferenceSession(str(MODELS_DIR / "cls.onnx"))
    rec_sess = ort.InferenceSession(str(MODELS_DIR / "rec.onnx"))

    dict_path = MODELS_DIR / "rec_dict.txt"
    dictionary = dict_path.read_text(encoding="utf-8").splitlines()

    # det
    boxes = run_det(det_sess, img_bgr)
    print(f"\n검출 박스: {len(boxes)}개")

    # det → cls → rec
    results = []
    for i, box in enumerate(boxes):
        cropped = crop_box(img_bgr, box)
        if cropped.size == 0:
            continue

        label = run_cls(cls_sess, cropped)
        if label == 1:
            cropped = cv2.rotate(cropped, cv2.ROTATE_180)

        text, score = run_rec(rec_sess, cropped, dictionary)
        if score >= 0.5 and text:
            results.append((text, score, box.tolist()))
            print(f"  [{i:3d}] score={score:.3f} text={text}")

    print(f"\n최종 인식: {len(results)}개 텍스트 영역")


if __name__ == "__main__":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8")
    main()
