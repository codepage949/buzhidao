import argparse
import collections
import json
import math
import os
import sys
import tempfile
import time
import types
from pathlib import Path

os.environ.setdefault("PADDLE_PDX_DISABLE_MODEL_SOURCE_CHECK", "True")
os.environ.setdefault("DISABLE_MODEL_SOURCE_CHECK", "True")

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

DEFAULT_LANG = "en"
SIDECAR_PROFILE_PREFIX = "[buzhi_ocr_sidecar_profile]"


def _load_langs_json() -> tuple[str, ...]:
    base = getattr(sys, "_MEIPASS", None) or Path(__file__).resolve().parent.parent
    langs_path = Path(base) / "shared" / "langs.json"
    if not langs_path.is_file():
        langs_path = Path(__file__).resolve().parents[2] / "shared" / "langs.json"
    with open(langs_path, encoding="utf-8") as f:
        entries = json.load(f)
    return tuple(entry["code"] for entry in entries)


LANGS = _load_langs_json()


def resolve_selected_lang() -> str:
    lang = os.environ.get("PYTHON_OCR_LANG", "").strip().lower()
    if lang in LANGS:
        return lang
    if lang:
        print(
            f"unsupported PYTHON_OCR_LANG: {lang} (falling back to {DEFAULT_LANG})",
            file=sys.stderr,
            flush=True,
        )
    return DEFAULT_LANG


# 1×1 24bpp BMP — PaddleOCR 모델 워밍업 전용 최소 입력
WARMUP_IMAGE = bytes.fromhex(
    "424d3a0000000000000036000000280000000100000001000000010018000000"
    "000004000000130b0000130b00000000000000000000ffffff00"
)


def configure_windows_paddle_probe_env() -> None:
    if os.name != "nt":
        return

    shim_dir = Path(tempfile.gettempdir()) / "buzhidao-paddle-shims"
    try:
        shim_dir.mkdir(parents=True, exist_ok=True)
        (shim_dir / "ccache").touch(exist_ok=True)
    except OSError:
        return

    shim = str(shim_dir)
    path = os.environ.get("PATH", "")
    entries = path.split(os.pathsep) if path else []
    if shim not in entries:
        os.environ["PATH"] = os.pathsep.join([shim, *entries]) if entries else shim

    # Paddle imports cpp_extension on startup and probes `where nvcc` / `where ccache`
    # even though we never build custom ops at runtime. Seed harmless existing paths to
    # keep that import from spawning short-lived console processes on Windows.
    os.environ.setdefault("CUDA_HOME", shim)
    os.environ.setdefault("CUDA_PATH", shim)


def configure_frozen_dll_search_path() -> None:
    if not getattr(sys, "frozen", False):
        return

    base_dir = getattr(sys, "_MEIPASS", None)
    if not base_dir:
        return

    candidates = [
        base_dir,
        os.path.join(base_dir, "paddle"),
        os.path.join(base_dir, "paddle", "libs"),
        os.path.join(base_dir, "paddle", "base"),
    ]
    nvidia_root = Path(base_dir) / "nvidia"
    if nvidia_root.is_dir():
        candidates.extend(
            str(path)
            for path in nvidia_root.glob("*/bin")
            if path.is_dir()
        )

    added = []
    for path in candidates:
        if not os.path.isdir(path):
            continue
        if hasattr(os, "add_dll_directory"):
            os.add_dll_directory(path)
        added.append(path)

    if added:
        os.environ["PATH"] = os.pathsep.join(added + [os.environ.get("PATH", "")])


def resolve_ocr_device() -> str:
    device = os.environ.get("PYTHON_OCR_DEVICE", "cpu").strip().lower()
    if device not in ("cpu", "gpu"):
        raise ValueError(
            f"unsupported PYTHON_OCR_DEVICE: {device} (expected: cpu or gpu)"
        )
    return device


def env_flag(name: str) -> bool:
    raw = os.environ.get(name, "").strip()
    return bool(raw and raw.lower() not in {"0", "false", "no", "off"})


def sidecar_profile_stages_enabled() -> bool:
    return env_flag("BUZHIDAO_PADDLE_SIDECAR_PROFILE_STAGES")


def sidecar_profile_log_path() -> Path:
    return Path(tempfile.gettempdir()) / "buzhi-ocr-sidecar-profile.log"


def format_sidecar_profile_message(event: str, fields: dict[str, object]) -> str:
    parts = [event]
    if fields:
        parts.append(
            ", ".join(f"{key}={value}" for key, value in fields.items())
        )
    return " ".join(parts)


def sidecar_profile_log(event: str, **fields: object) -> None:
    if not sidecar_profile_stages_enabled():
        return
    message = format_sidecar_profile_message(event, fields)
    line = f"{SIDECAR_PROFILE_PREFIX} {message}"
    print(line, file=sys.stderr, flush=True)
    with sidecar_profile_log_path().open("a", encoding="utf-8") as fp:
        fp.write(line + "\n")


def resolve_debug_dump_dir() -> Path | None:
    raw = os.environ.get("BUZHIDAO_PADDLE_SIDECAR_DUMP_DIR", "").strip()
    if not raw:
        raw = os.environ.get("BUZHIDAO_PADDLE_FFI_DUMP_DIR", "").strip()
    if not raw:
        return None
    return Path(raw)


def _polygon_to_list(poly) -> list[list[float]]:
    if hasattr(poly, "tolist"):
        poly = poly.tolist()
    return [[float(point[0]), float(point[1])] for point in poly]


def _install_sidecar_rec_dump(ocr) -> None:
    if not (
        env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_REC_LOGITS")
        or env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_DET")
        or env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_CROP")
        or sidecar_profile_stages_enabled()
    ):
        return

    dump_dir = resolve_debug_dump_dir()

    pipeline = getattr(getattr(ocr, "paddlex_pipeline", None), "_pipeline", None)
    if pipeline is None or getattr(pipeline, "_buzhidao_rec_dump_installed", False):
        return

    import numpy as np
    import cv2

    from paddlex.inference.pipelines.components import (
        cal_ocr_word_box,
        convert_points_to_boxes,
    )
    from paddlex.inference.pipelines.ocr.result import OCRResult

    state = {
        "pending": collections.deque(),
        "seq": 0,
        "det_seq": 0,
        "det_contour_seq": 0,
        "det_input_seq": 0,
        "predict_seq": 0,
        "det_run_seq": 0,
        "rec_run_seq": 0,
    }
    dump_rec_logits_enabled = env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_REC_LOGITS")

    def dump_rec_batch(
        batch_raw_imgs,
        batch_imgs,
        batch_preds,
        batch_meta,
        decoded_texts,
        decoded_scores,
        return_word_box,
    ) -> None:
        if not dump_rec_logits_enabled:
            return
        arrays = list(batch_preds) if isinstance(batch_preds, (tuple, list)) else [batch_preds]
        if not arrays:
            return
        probs = np.asarray(arrays[0], dtype=np.float32)
        if probs.ndim != 3:
            return

        batch_n = int(probs.shape[0])
        time_steps = int(probs.shape[1])
        num_classes = int(probs.shape[2])
        batch_w = int(max(img.shape[2] for img in batch_imgs))
        path = None
        if dump_dir is not None:
            dump_dir.mkdir(parents=True, exist_ok=True)
            path = dump_dir / (
                f"sidecar_rec_batch_logits_{state['seq']}_n{batch_n}_w{batch_w}"
                f"_ts{time_steps}_cls{num_classes}.json"
            )
            state["seq"] += 1

        items = []
        for i in range(batch_n):
            meta = batch_meta[i] if i < len(batch_meta) else {}
            raw_img = batch_raw_imgs[i]
            norm_img = batch_imgs[i]
            raw_pixels = raw_img.reshape(-1).astype(int)
            raw_channel_means = raw_img.reshape(-1, raw_img.shape[2]).mean(axis=0)
            decoded_text = decoded_texts[i] if i < len(decoded_texts) else ""
            if return_word_box and isinstance(decoded_text, (list, tuple)):
                decoded_text = decoded_text[0] if decoded_text else ""
            decoded_score = decoded_scores[i] if i < len(decoded_scores) else 0.0
            items.append(
                {
                    "index": i,
                    "original_index": int(meta.get("original_index", i)),
                    "ratio": float(meta.get("ratio", raw_img.shape[1] / float(raw_img.shape[0]))),
                    "polygon": meta.get("polygon", []),
                    "crop_box": meta.get("crop_box", []),
                    "crop_width": int(meta.get("crop_width", raw_img.shape[1])),
                    "crop_height": int(meta.get("crop_height", raw_img.shape[0])),
                    "decoded_text": decoded_text,
                    "decoded_score": float(decoded_score),
                    "image_width": int(raw_img.shape[1]),
                    "image_height": int(raw_img.shape[0]),
                    "raw_channel_means": raw_channel_means.astype(float).tolist(),
                    "raw_pixels": raw_pixels.tolist(),
                    "rec_width": int(norm_img.shape[2]),
                    "input_values": norm_img.reshape(-1).astype(float).tolist(),
                    "values": probs[i].reshape(-1).astype(float).tolist(),
                }
            )

        if path is not None:
            with path.open("w", encoding="utf-8") as fp:
                json.dump(
                    {
                        "batch_n": batch_n,
                        "batch_w": batch_w,
                        "input_shape": [batch_n, 3, int(batch_imgs[0].shape[1]), batch_w],
                        "output_shape": [batch_n, time_steps, num_classes],
                        "layout": 2,
                        "time_steps": time_steps,
                        "num_classes": num_classes,
                        "items": items,
                    },
                    fp,
                    ensure_ascii=False,
                )

    def dump_det_candidates(
        image_path: str,
        doc_preprocessor_image,
        dt_polys,
        all_subs_of_img,
        sorted_subs_info,
        angles,
    ) -> None:
        if not env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_DET"):
            return
        if dump_dir is None:
            return

        items = []
        for order_index, item in enumerate(sorted_subs_info):
            sub_img_id = item["sub_img_id"]
            crop = all_subs_of_img[sub_img_id]
            items.append(
                {
                    "order_index": order_index,
                    "original_index": sub_img_id,
                    "ratio": float(item["sub_img_ratio"]),
                    "polygon": _polygon_to_list(dt_polys[sub_img_id]),
                    "crop_width": int(crop.shape[1]),
                    "crop_height": int(crop.shape[0]),
                    "textline_orientation_angle": int(angles[sub_img_id]),
                }
            )

        path = dump_dir / (
            f"sidecar_det_candidates_{state['det_seq']}"
            f"_{Path(image_path).stem}_{doc_preprocessor_image.shape[1]}x{doc_preprocessor_image.shape[0]}.json"
        )
        state["det_seq"] += 1
        dump_dir.mkdir(parents=True, exist_ok=True)
        with path.open("w", encoding="utf-8") as fp:
            json.dump(
                {
                    "image_path": image_path,
                    "image_width": int(doc_preprocessor_image.shape[1]),
                    "image_height": int(doc_preprocessor_image.shape[0]),
                    "items": items,
                },
                fp,
                ensure_ascii=False,
            )

    def dump_crop_stage(tag, polygon, crop_quad, image) -> None:
        if not env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_CROP"):
            return
        if image is None:
            return
        if dump_dir is None:
            return
        dump_dir.mkdir(parents=True, exist_ok=True)
        stem = (
            f"sidecar_{tag}_{int(round(float(polygon[0][0])))}"
            f"_{int(round(float(polygon[0][1])))}_{int(image.shape[1])}x{int(image.shape[0])}"
        )
        with (dump_dir / f"{stem}.json").open("w", encoding="utf-8") as fp:
            json.dump(
                {
                    "input_polygon": _polygon_to_list(polygon),
                    "crop_quad": _polygon_to_list(crop_quad),
                    "output_size": [int(image.shape[1]), int(image.shape[0])],
                },
                fp,
                ensure_ascii=False,
            )
        cv2.imwrite(str(dump_dir / f"{stem}.png"), image)

    def install_crop_dump() -> None:
        if not env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_CROP"):
            return
        if dump_dir is None:
            return
        cropper = getattr(pipeline, "_crop_by_polys", None)
        if cropper is None or getattr(cropper, "_buzhidao_crop_dump_installed", False):
            return

        original_get_minarea_rect_crop = cropper.get_minarea_rect_crop

        def instrumented_get_minarea_rect_crop(this, img: np.ndarray, points: np.ndarray) -> np.ndarray:
            original_polygon = np.array(points, dtype=np.float32).reshape(-1, 2)
            bounding_box = cv2.minAreaRect(np.array(points).astype(np.int32))
            sorted_points = sorted(list(cv2.boxPoints(bounding_box)), key=lambda x: x[0])

            index_a, index_b, index_c, index_d = 0, 1, 2, 3
            if sorted_points[1][1] > sorted_points[0][1]:
                index_a = 0
                index_d = 1
            else:
                index_a = 1
                index_d = 0
            if sorted_points[3][1] > sorted_points[2][1]:
                index_b = 2
                index_c = 3
            else:
                index_b = 3
                index_c = 2

            crop_quad = np.array(
                [
                    sorted_points[index_a],
                    sorted_points[index_b],
                    sorted_points[index_c],
                    sorted_points[index_d],
                ],
                dtype=np.float32,
            )
            img_crop_width = int(
                max(
                    np.linalg.norm(crop_quad[0] - crop_quad[1]),
                    np.linalg.norm(crop_quad[2] - crop_quad[3]),
                )
            )
            img_crop_height = int(
                max(
                    np.linalg.norm(crop_quad[0] - crop_quad[3]),
                    np.linalg.norm(crop_quad[1] - crop_quad[2]),
                )
            )
            pts_std = np.float32(
                [
                    [0, 0],
                    [img_crop_width, 0],
                    [img_crop_width, img_crop_height],
                    [0, img_crop_height],
                ]
            )
            M = cv2.getPerspectiveTransform(crop_quad, pts_std)
            warped = cv2.warpPerspective(
                img,
                M,
                (img_crop_width, img_crop_height),
                borderMode=cv2.BORDER_REPLICATE,
                flags=cv2.INTER_CUBIC,
            )
            dump_crop_stage("crop_warp", original_polygon, crop_quad, warped)
            final = warped
            if final.shape[0] * 1.0 / final.shape[1] >= 1.5:
                final = np.rot90(final)
            dump_crop_stage("crop_final", original_polygon, crop_quad, final)
            return final

        cropper.get_minarea_rect_crop = types.MethodType(
            instrumented_get_minarea_rect_crop, cropper
        )
        cropper._buzhidao_crop_dump_installed = True

    def install_det_contour_dump() -> None:
        if not env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_DET"):
            return
        post_op = getattr(pipeline.text_det_model, "post_op", None)
        if post_op is None or getattr(post_op, "_buzhidao_det_dump_installed", False):
            return

        import cv2

        original_boxes_from_bitmap = post_op.boxes_from_bitmap

        def instrumented_boxes_from_bitmap(
            self,
            pred,
            _bitmap,
            dest_width,
            dest_height,
            box_thresh,
            unclip_ratio,
        ):
            bitmap = _bitmap
            height, width = bitmap.shape
            width_scale = dest_width / width
            height_scale = dest_height / height

            outs = cv2.findContours(
                (bitmap * 255).astype(np.uint8), cv2.RETR_LIST, cv2.CHAIN_APPROX_SIMPLE
            )
            if len(outs) == 3:
                contours = outs[1]
            else:
                contours = outs[0]
            _, labels = cv2.connectedComponents((bitmap * 255).astype(np.uint8), connectivity=8)

            def find_component_pixels(contour) -> tuple[list[int], list[list[int]]]:
                contour_xy = contour.reshape(-1, 2)
                label_id = 0
                for px, py in contour_xy:
                    x = int(px)
                    y = int(py)
                    if 0 <= x < width and 0 <= y < height:
                        label_id = int(labels[y, x])
                        if label_id != 0:
                            break
                    found = False
                    for dy in (-1, 0, 1):
                        for dx in (-1, 0, 1):
                            nx = x + dx
                            ny = y + dy
                            if 0 <= nx < width and 0 <= ny < height:
                                label_id = int(labels[ny, nx])
                                if label_id != 0:
                                    found = True
                                    break
                        if found:
                            break
                    if label_id != 0:
                        break
                if label_id == 0:
                    return [0, 0, 0, 0], []
                ys, xs = np.where(labels == label_id)
                if xs.size == 0:
                    return [0, 0, 0, 0], []
                x0 = int(xs.min())
                y0 = int(ys.min())
                x1 = int(xs.max()) + 1
                y1 = int(ys.max()) + 1
                pixels = [[int(x - x0), int(y - y0)] for x, y in zip(xs.tolist(), ys.tolist())]
                return [x0, y0, x1, y1], pixels

            items = []
            for index, contour in enumerate(contours[: self.max_candidates]):
                min_rect = cv2.minAreaRect(contour)
                min_rect_points = cv2.boxPoints(min_rect).astype(np.float32)
                points, sside = self.get_mini_boxes(contour)
                raw_points = contour.reshape(-1, 2).astype(float)
                component_bbox, component_pixels = find_component_pixels(contour)
                x0, y0, x1, y1 = component_bbox
                component_pred = (
                    pred[y0:y1, x0:x1].astype(np.float32).reshape(-1).tolist()
                    if x1 > x0 and y1 > y0
                    else []
                )
                component_bitmap = (
                    bitmap[y0:y1, x0:x1].astype(np.uint8).reshape(-1).tolist()
                    if x1 > x0 and y1 > y0
                    else []
                )
                item = {
                    "index": index,
                    "component_bbox": component_bbox,
                    "component_pixels": component_pixels,
                    "component_pred": component_pred,
                    "component_bitmap": component_bitmap,
                    "contour": raw_points.tolist(),
                    "min_area_rect": {
                        "center": [float(min_rect[0][0]), float(min_rect[0][1])],
                        "width": float(min_rect[1][0]),
                        "height": float(min_rect[1][1]),
                        "angle": float(min_rect[2]),
                    },
                    "rect_points": min_rect_points.tolist(),
                    "mini_box": np.asarray(points, dtype=np.float32).tolist(),
                    "mini_side": float(sside),
                }
                if sside < self.min_size:
                    item["accepted"] = False
                    item["reject_reason"] = "min_side"
                    items.append(item)
                    continue
                points = np.array(points)
                if self.score_mode == "fast":
                    box_points = points.reshape(-1, 2).astype(np.float32)
                    xmin = max(0, min(math.floor(box_points[:, 0].min()), width - 1))
                    xmax = max(0, min(math.ceil(box_points[:, 0].max()), width - 1))
                    ymin = max(0, min(math.floor(box_points[:, 1].min()), height - 1))
                    ymax = max(0, min(math.ceil(box_points[:, 1].max()), height - 1))
                    mask = np.zeros((ymax - ymin + 1, xmax - xmin + 1), dtype=np.uint8)
                    score_points = box_points.copy()
                    score_points[:, 0] = score_points[:, 0] - xmin
                    score_points[:, 1] = score_points[:, 1] - ymin
                    cv2.fillPoly(mask, score_points.reshape(1, -1, 2).astype(np.int32), 1)
                    masked_region = pred[ymin : ymax + 1, xmin : xmax + 1]
                    masked_sum = float(masked_region[mask.astype(bool)].sum()) if mask.any() else 0.0
                    item["score_bbox"] = [int(xmin), int(ymin), int(xmax), int(ymax)]
                    item["score_mask_pixels"] = int(mask.sum())
                    item["score_sum"] = masked_sum
                    score = self.box_score_fast(pred, points.reshape(-1, 2))
                else:
                    score = self.box_score_slow(pred, contour)
                item["score"] = float(score)
                if box_thresh > score:
                    item["accepted"] = False
                    item["reject_reason"] = "score"
                    items.append(item)
                    continue

                box = self.unclip(points, unclip_ratio).reshape(-1, 1, 2)
                box, expanded_sside = self.get_mini_boxes(box)
                item["unclipped_box"] = np.asarray(box, dtype=np.float32).tolist()
                item["unclipped_side"] = float(expanded_sside)
                if expanded_sside < self.min_size + 2:
                    item["accepted"] = False
                    item["reject_reason"] = "unclip_side"
                    items.append(item)
                    continue

                scaled = np.array(box)
                for i in range(scaled.shape[0]):
                    scaled[i, 0] = max(
                        0, min(round(scaled[i, 0] * width_scale), dest_width)
                    )
                    scaled[i, 1] = max(
                        0, min(round(scaled[i, 1] * height_scale), dest_height)
                    )
                item["scaled_box"] = scaled.astype(float).tolist()
                item["accepted"] = True
                items.append(item)

            dump_dir.mkdir(parents=True, exist_ok=True)
            path = dump_dir / (
                f"sidecar_det_contours_{state['det_contour_seq']}"
                f"_{dest_width}x{dest_height}_{width}x{height}.json"
            )
            state["det_contour_seq"] += 1
            with path.open("w", encoding="utf-8") as fp:
                json.dump(
                    {
                        "dest_width": int(dest_width),
                        "dest_height": int(dest_height),
                        "pred_width": int(width),
                        "pred_height": int(height),
                        "items": items,
                    },
                    fp,
                    ensure_ascii=False,
                )
            return original_boxes_from_bitmap(
                pred,
                _bitmap,
                dest_width,
                dest_height,
                box_thresh,
                unclip_ratio,
            )

        post_op.boxes_from_bitmap = types.MethodType(
            instrumented_boxes_from_bitmap, post_op
        )
        post_op._buzhidao_det_dump_installed = True

    original_det_process = pipeline.text_det_model.process

    def instrumented_det_process(
        self,
        batch_data,
        limit_side_len=None,
        limit_type=None,
        thresh=None,
        box_thresh=None,
        unclip_ratio=None,
        max_side_limit=None,
    ):
        import cv2

        det_started = time.perf_counter()
        read_started = time.perf_counter()
        batch_raw_imgs = self.pre_tfs["Read"](imgs=batch_data.instances)
        read_ms = (time.perf_counter() - read_started) * 1000.0
        resize_started = time.perf_counter()
        batch_imgs, batch_shapes = self.pre_tfs["Resize"](
            imgs=batch_raw_imgs,
            limit_side_len=limit_side_len or self.limit_side_len,
            limit_type=limit_type or self.limit_type,
            max_side_limit=(
                max_side_limit if max_side_limit is not None else self.max_side_limit
            ),
        )
        resize_ms = (time.perf_counter() - resize_started) * 1000.0
        if env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_DET"):
            if dump_dir is None:
                raise ValueError("det dump dir is required when BUZHIDAO_PADDLE_SIDECAR_DUMP_DET=1")
            dump_dir.mkdir(parents=True, exist_ok=True)
            for i, img in enumerate(batch_imgs):
                path = dump_dir / (
                    f"sidecar_det_input_{state['det_input_seq']}_{i}_"
                    f"{img.shape[1]}x{img.shape[0]}.png"
                )
                cv2.imwrite(str(path), img)
            state["det_input_seq"] += 1
        normalize_started = time.perf_counter()
        batch_imgs = self.pre_tfs["Normalize"](imgs=batch_imgs)
        normalize_ms = (time.perf_counter() - normalize_started) * 1000.0
        to_chw_started = time.perf_counter()
        batch_imgs = self.pre_tfs["ToCHW"](imgs=batch_imgs)
        to_chw_ms = (time.perf_counter() - to_chw_started) * 1000.0
        batch_started = time.perf_counter()
        x = self.pre_tfs["ToBatch"](imgs=batch_imgs)
        to_batch_ms = (time.perf_counter() - batch_started) * 1000.0
        infer_started = time.perf_counter()
        batch_preds = self.infer(x=x)
        infer_ms = (time.perf_counter() - infer_started) * 1000.0
        post_started = time.perf_counter()
        polys, scores = self.post_op(
            batch_preds,
            batch_shapes,
            thresh=thresh or self.thresh,
            box_thresh=box_thresh or self.box_thresh,
            unclip_ratio=unclip_ratio or self.unclip_ratio,
        )
        post_ms = (time.perf_counter() - post_started) * 1000.0
        total_ms = (time.perf_counter() - det_started) * 1000.0
        if sidecar_profile_stages_enabled():
            first_src_h = int(batch_raw_imgs[0].shape[0]) if batch_raw_imgs else 0
            first_src_w = int(batch_raw_imgs[0].shape[1]) if batch_raw_imgs else 0
            first_det_h = int(batch_imgs[0].shape[1]) if batch_imgs else 0
            first_det_w = int(batch_imgs[0].shape[2]) if batch_imgs else 0
            box_count = sum(len(item) for item in polys)
            sidecar_profile_log(
                "run_det profile",
                seq=state["det_run_seq"],
                batch_n=len(batch_raw_imgs),
                src=f"{first_src_w}x{first_src_h}",
                det_input=f"{first_det_w}x{first_det_h}",
                boxes=box_count,
                read_ms=f"{read_ms:.3f}",
                resize_ms=f"{resize_ms:.3f}",
                normalize_ms=f"{normalize_ms:.3f}",
                to_chw_ms=f"{to_chw_ms:.3f}",
                to_batch_ms=f"{to_batch_ms:.3f}",
                infer_ms=f"{infer_ms:.3f}",
                post_ms=f"{post_ms:.3f}",
                total_ms=f"{total_ms:.3f}",
            )
            state["det_run_seq"] += 1
        return {
            "input_path": batch_data.input_paths,
            "page_index": batch_data.page_indexes,
            "input_img": batch_raw_imgs,
            "dt_polys": polys,
            "dt_scores": scores,
        }

    original_process = pipeline.text_rec_model.process

    def instrumented_process(self, batch_data, return_word_box=False):
        rec_started = time.perf_counter()
        read_started = time.perf_counter()
        batch_raw_imgs = self.pre_tfs["Read"](imgs=batch_data.instances)
        read_ms = (time.perf_counter() - read_started) * 1000.0
        width_list = []
        for img in batch_raw_imgs:
            width_list.append(img.shape[1] / float(img.shape[0]))
        indices = np.argsort(np.array(width_list))
        resize_norm_started = time.perf_counter()
        batch_imgs = self.pre_tfs["ReisizeNorm"](imgs=batch_raw_imgs)
        resize_norm_ms = (time.perf_counter() - resize_norm_started) * 1000.0
        batch_started = time.perf_counter()
        x = self.pre_tfs["ToBatch"](imgs=batch_imgs)
        to_batch_ms = (time.perf_counter() - batch_started) * 1000.0
        infer_started = time.perf_counter()
        batch_preds = self.infer(x=x)
        infer_ms = (time.perf_counter() - infer_started) * 1000.0
        pending = state["pending"].popleft() if state["pending"] else []
        batch_num = self.batch_sampler.batch_size
        img_num = len(batch_raw_imgs)
        rec_image_shape = next(
            op["RecResizeImg"]["image_shape"]
            for op in self.config["PreProcess"]["transform_ops"]
            if "RecResizeImg" in op
        )
        img_c, img_h, img_w = rec_image_shape[:3]
        _ = img_c
        max_wh_ratio = img_w / img_h
        end_img_no = min(img_num, batch_num)
        wh_ratio_list = []
        for ino in range(0, end_img_no):
            h, w = batch_raw_imgs[indices[ino]].shape[0:2]
            wh_ratio = w * 1.0 / h
            max_wh_ratio = max(max_wh_ratio, wh_ratio)
            wh_ratio_list.append(wh_ratio)
        post_started = time.perf_counter()
        texts, scores = self.post_op(
            batch_preds,
            return_word_box=return_word_box or self.return_word_box,
            wh_ratio_list=wh_ratio_list,
            max_wh_ratio=max_wh_ratio,
        )
        post_ms = (time.perf_counter() - post_started) * 1000.0
        if self.model_name in (
            "arabic_PP-OCRv3_mobile_rec",
            "arabic_PP-OCRv5_mobile_rec",
        ):
            from bidi.algorithm import get_display

            texts = [get_display(s) for s in texts]
        dump_rec_batch(
            batch_raw_imgs,
            batch_imgs,
            batch_preds,
            pending,
            texts,
            scores,
            return_word_box or self.return_word_box,
        )
        total_ms = (time.perf_counter() - rec_started) * 1000.0
        if sidecar_profile_stages_enabled():
            batch_w = int(max(img.shape[2] for img in batch_imgs)) if batch_imgs else 0
            sidecar_profile_log(
                "run_rec_batch profile",
                seq=state["rec_run_seq"],
                batch_n=len(batch_raw_imgs),
                batch_w=batch_w,
                max_wh_ratio=f"{max_wh_ratio:.6f}",
                read_ms=f"{read_ms:.3f}",
                resize_norm_ms=f"{resize_norm_ms:.3f}",
                to_batch_ms=f"{to_batch_ms:.3f}",
                infer_ms=f"{infer_ms:.3f}",
                post_ms=f"{post_ms:.3f}",
                total_ms=f"{total_ms:.3f}",
            )
            state["rec_run_seq"] += 1
        return {
            "input_path": batch_data.input_paths,
            "page_index": batch_data.page_indexes,
            "input_img": batch_raw_imgs,
            "rec_text": texts,
            "rec_score": scores,
            "vis_font": [self.vis_font] * len(batch_raw_imgs),
        }

    def instrumented_predict(
        self,
        input,
        use_doc_orientation_classify=None,
        use_doc_unwarping=None,
        use_textline_orientation=None,
        text_det_limit_side_len=None,
        text_det_limit_type=None,
        text_det_max_side_limit=None,
        text_det_thresh=None,
        text_det_box_thresh=None,
        text_det_unclip_ratio=None,
        text_rec_score_thresh=None,
        return_word_box=None,
    ):
        pipeline_started = time.perf_counter()
        model_settings = self.get_model_settings(
            use_doc_orientation_classify, use_doc_unwarping, use_textline_orientation
        )

        if not self.check_model_settings_valid(model_settings):
            yield {"error": "the input params for model settings are invalid!"}

        text_det_params = self.get_text_det_params(
            text_det_limit_side_len,
            text_det_limit_type,
            text_det_max_side_limit,
            text_det_thresh,
            text_det_box_thresh,
            text_det_unclip_ratio,
        )

        if text_rec_score_thresh is None:
            text_rec_score_thresh = self.text_rec_score_thresh
        if return_word_box is None:
            return_word_box = self.return_word_box

        for _, batch_data in enumerate(self.batch_sampler(input)):
            image_arrays = self.img_reader(batch_data.instances)

            if model_settings["use_doc_preprocessor"]:
                doc_pre_started = time.perf_counter()
                doc_preprocessor_results = list(
                    self.doc_preprocessor_pipeline(
                        image_arrays,
                        use_doc_orientation_classify=use_doc_orientation_classify,
                        use_doc_unwarping=use_doc_unwarping,
                    )
                )
                doc_pre_ms = (time.perf_counter() - doc_pre_started) * 1000.0
            else:
                doc_preprocessor_results = [{"output_img": arr} for arr in image_arrays]
                doc_pre_ms = 0.0

            doc_preprocessor_images = [
                item["output_img"] for item in doc_preprocessor_results
            ]

            det_started = time.perf_counter()
            det_results = list(
                self.text_det_model(doc_preprocessor_images, **text_det_params)
            )
            det_ms = (time.perf_counter() - det_started) * 1000.0

            dt_polys_list = [item["dt_polys"] for item in det_results]
            dt_polys_list = [self._sort_boxes(item) for item in dt_polys_list]

            results = [
                {
                    "input_path": input_path,
                    "page_index": page_index,
                    "doc_preprocessor_res": doc_preprocessor_res,
                    "dt_polys": dt_polys,
                    "model_settings": model_settings,
                    "text_det_params": text_det_params,
                    "text_type": self.text_type,
                    "text_rec_score_thresh": text_rec_score_thresh,
                    "return_word_box": return_word_box,
                    "rec_texts": [],
                    "rec_scores": [],
                    "rec_polys": [],
                    "vis_fonts": [],
                    "debug_candidates": [],
                }
                for input_path, page_index, doc_preprocessor_res, dt_polys in zip(
                    batch_data.input_paths,
                    batch_data.page_indexes,
                    doc_preprocessor_results,
                    dt_polys_list,
                )
            ]

            indices = list(range(len(doc_preprocessor_images)))
            indices = [idx for idx in indices if len(dt_polys_list[idx]) > 0]
            crop_ms = 0.0
            cls_ms = 0.0
            rotate_ms = 0.0
            rec_ms = 0.0
            rotated_count = 0
            rec_candidate_count = 0

            if indices:
                all_subs_of_imgs = []
                chunk_indices = [0]
                for idx in indices:
                    crop_started = time.perf_counter()
                    all_subs_of_img = list(
                        self._crop_by_polys(
                            doc_preprocessor_images[idx], dt_polys_list[idx]
                        )
                    )
                    crop_ms += (time.perf_counter() - crop_started) * 1000.0
                    all_subs_of_imgs.extend(all_subs_of_img)
                    chunk_indices.append(chunk_indices[-1] + len(all_subs_of_img))
                rec_candidate_count = len(all_subs_of_imgs)

                if model_settings["use_textline_orientation"]:
                    cls_started = time.perf_counter()
                    angles = [
                        int(textline_angle_info["class_ids"][0])
                        for textline_angle_info in self.textline_orientation_model(
                            all_subs_of_imgs
                        )
                    ]
                    cls_ms += (time.perf_counter() - cls_started) * 1000.0
                    rotate_started = time.perf_counter()
                    all_subs_of_imgs = self.rotate_image(all_subs_of_imgs, angles)
                    rotate_ms += (time.perf_counter() - rotate_started) * 1000.0
                    rotated_count = sum(1 for angle in angles if int(angle) == 1)
                else:
                    angles = [-1] * len(all_subs_of_imgs)
                for i, idx in enumerate(indices):
                    res = results[idx]
                    res["textline_orientation_angles"] = angles[
                        chunk_indices[i] : chunk_indices[i + 1]
                    ]

                for i, idx in enumerate(indices):
                    all_subs_of_img = all_subs_of_imgs[
                        chunk_indices[i] : chunk_indices[i + 1]
                    ]
                    res = results[idx]
                    dt_polys = dt_polys_list[idx]
                    sub_img_info_list = [
                        {
                            "sub_img_id": img_id,
                            "sub_img_ratio": sub_img.shape[1] / float(sub_img.shape[0]),
                        }
                        for img_id, sub_img in enumerate(all_subs_of_img)
                    ]
                    sorted_subs_info = sorted(
                        sub_img_info_list, key=lambda x: x["sub_img_ratio"]
                    )
                    sorted_subs_of_img = [
                        all_subs_of_img[x["sub_img_id"]] for x in sorted_subs_info
                    ]
                    dump_det_candidates(
                        str(batch_data.input_paths[idx]),
                        doc_preprocessor_images[idx],
                        dt_polys,
                        all_subs_of_img,
                        sorted_subs_info,
                        angles[chunk_indices[i] : chunk_indices[i + 1]],
                    )
                    rec_batch_size = max(1, self.text_rec_model.batch_sampler.batch_size)
                    for start in range(0, len(sorted_subs_info), rec_batch_size):
                        chunk = sorted_subs_info[start : start + rec_batch_size]
                        chunk_subs_of_img = [
                            all_subs_of_img[item["sub_img_id"]] for item in chunk
                        ]
                        if dump_rec_logits_enabled:
                            state["pending"].append(
                                [
                                    {
                                        "original_index": item["sub_img_id"],
                                        "ratio": item["sub_img_ratio"],
                                        "polygon": _polygon_to_list(dt_polys[item["sub_img_id"]]),
                                        "crop_box": _polygon_to_list(
                                            self._crop_by_polys.get_minarea_rect(
                                                doc_preprocessor_images[idx],
                                                np.array(dt_polys[item["sub_img_id"]]).astype(np.int32).reshape(-1, 2),
                                            )[1]
                                        ),
                                        "crop_width": int(all_subs_of_img[item["sub_img_id"]].shape[1]),
                                        "crop_height": int(all_subs_of_img[item["sub_img_id"]].shape[0]),
                                    }
                                    for item in chunk
                                ]
                            )
                        rec_started = time.perf_counter()
                        for offset, rec_res in enumerate(
                            self.text_rec_model(
                                chunk_subs_of_img, return_word_box=return_word_box
                            )
                        ):
                            sub_img_id = chunk[offset]["sub_img_id"]
                            sub_img_info_list[sub_img_id]["rec_res"] = rec_res
                        rec_ms += (time.perf_counter() - rec_started) * 1000.0
                    if return_word_box:
                        res["text_word"] = []
                        res["text_word_region"] = []
                    for sno in range(len(sub_img_info_list)):
                        rec_res = sub_img_info_list[sno]["rec_res"]
                        rec_text_value = (
                            rec_res["rec_text"][0]
                            if return_word_box
                            else rec_res["rec_text"]
                        )
                        rec_score_value = float(rec_res["rec_score"])
                        accepted = rec_score_value >= text_rec_score_thresh
                        res["debug_candidates"].append(
                            {
                                "polygon": _polygon_to_list(dt_polys[sno]),
                                "text": rec_text_value,
                                "score": rec_score_value,
                                "accepted": accepted,
                            }
                        )
                        if accepted:
                            if return_word_box:
                                word_box_content_list, word_box_list = cal_ocr_word_box(
                                    rec_res["rec_text"][0],
                                    dt_polys[sno],
                                    rec_res["rec_text"][1],
                                )
                                res["text_word"].append(word_box_content_list)
                                res["text_word_region"].append(word_box_list)
                                res["rec_texts"].append(rec_res["rec_text"][0])
                            else:
                                res["rec_texts"].append(rec_res["rec_text"])
                            res["rec_scores"].append(rec_res["rec_score"])
                            res["vis_fonts"].append(rec_res["vis_font"])
                            res["rec_polys"].append(dt_polys[sno])
            post_started = time.perf_counter()
            for res in results:
                if self.text_type == "general":
                    rec_boxes = convert_points_to_boxes(res["rec_polys"])
                    res["rec_boxes"] = rec_boxes
                    if return_word_box:
                        res["text_word_boxes"] = [
                            convert_points_to_boxes(line)
                            for line in res["text_word_region"]
                        ]
                else:
                    res["rec_boxes"] = np.array([])

                post_ms = (time.perf_counter() - post_started) * 1000.0
                total_ms = (time.perf_counter() - pipeline_started) * 1000.0
                if sidecar_profile_stages_enabled():
                    image_h = int(res["doc_preprocessor_res"]["output_img"].shape[0])
                    image_w = int(res["doc_preprocessor_res"]["output_img"].shape[1])
                    sidecar_profile_log(
                        "run_pipeline profile",
                        seq=state["predict_seq"],
                        image=f"{image_w}x{image_h}",
                        boxes=len(res["dt_polys"]),
                        cls_inputs=rec_candidate_count,
                        rec_candidates=rec_candidate_count,
                        rotated=rotated_count,
                        doc_pre_ms=f"{doc_pre_ms:.3f}",
                        det_ms=f"{det_ms:.3f}",
                        crop_ms=f"{crop_ms:.3f}",
                        cls_ms=f"{cls_ms:.3f}",
                        rotate_ms=f"{rotate_ms:.3f}",
                        rec_ms=f"{rec_ms:.3f}",
                        post_ms=f"{post_ms:.3f}",
                        total_ms=f"{total_ms:.3f}",
                    )
                    state["predict_seq"] += 1
                yield OCRResult(res)

    pipeline.text_rec_model.__class__.process = instrumented_process
    install_det_contour_dump()
    install_crop_dump()
    pipeline.text_det_model.__class__.process = instrumented_det_process
    pipeline.__class__.predict = instrumented_predict
    pipeline._buzhidao_rec_dump_installed = True


def _load_paddleocr():
    try:
        from paddleocr import PaddleOCR
    except ModuleNotFoundError as exc:
        missing = exc.name or "unknown"
        print(
            (
                f"missing python dependency: {missing}\n"
                "Install OCR server dependencies with uv before building.\n"
                "Example: uv sync --group build"
            ),
            file=sys.stderr,
            flush=True,
        )
        raise
    return PaddleOCR


def build_ocr(lang: str):
    PaddleOCR = _load_paddleocr()
    ocr = PaddleOCR(
        use_doc_orientation_classify=False,
        use_doc_unwarping=False,
        use_textline_orientation=True,
        device=resolve_ocr_device(),
        lang=lang,
    )
    if (
        env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_REC_LOGITS")
        or env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_DET")
        or env_flag("BUZHIDAO_PADDLE_SIDECAR_DUMP_CROP")
        or sidecar_profile_stages_enabled()
    ):
        _install_sidecar_rec_dump(ocr)
    if sidecar_profile_stages_enabled():
        pipeline = getattr(getattr(ocr, "paddlex_pipeline", None), "_pipeline", None)
        rec_batch_size = None
        if pipeline is not None:
            rec_batch_size = getattr(
                getattr(pipeline, "text_rec_model", None),
                "batch_sampler",
                None,
            )
            rec_batch_size = getattr(rec_batch_size, "batch_size", None)
        sidecar_profile_log(
            "build_ocr settings",
            lang=lang,
            device=resolve_ocr_device(),
            use_textline_orientation=True,
            rec_batch_size=rec_batch_size if rec_batch_size is not None else "unknown",
        )
    return ocr


def _normalize_rec_logits(
    preds, batch_n: int, expected_time_steps: int | None = None, expected_num_classes: int | None = None
) -> tuple:
    import numpy as np

    arrays = list(preds) if isinstance(preds, (tuple, list)) else [preds]
    if not arrays:
        raise ValueError("empty predictor output")
    probs = np.asarray(arrays[0], dtype=np.float32)
    if probs.ndim == 2:
        probs = np.expand_dims(probs, axis=0)
    if probs.ndim != 3:
        raise ValueError(f"unexpected predictor output ndim: {probs.ndim}")

    if probs.shape[0] != batch_n:
        raise ValueError(
            f"unexpected predictor batch: {probs.shape[0]} (expected {batch_n})"
        )

    layout = 2
    if (
        expected_time_steps is not None
        and expected_num_classes is not None
        and probs.shape[1] == expected_num_classes
        and probs.shape[2] == expected_time_steps
    ):
        probs = np.transpose(probs, (0, 2, 1))
        layout = 1

    return probs, layout


def _compare_rec_values(sidecar_values, ffi_values) -> dict:
    import numpy as np

    sidecar_arr = np.asarray(sidecar_values, dtype=np.float32)
    ffi_arr = np.asarray(ffi_values, dtype=np.float32)
    if sidecar_arr.shape != ffi_arr.shape:
        raise ValueError(
            f"logit shape mismatch: {tuple(sidecar_arr.shape)} vs {tuple(ffi_arr.shape)}"
        )
    diff = np.abs(sidecar_arr - ffi_arr)
    return {
        "mean_abs_diff": float(diff.mean()) if diff.size else 0.0,
        "max_abs_diff": float(diff.max()) if diff.size else 0.0,
        "sidecar_mean": float(sidecar_arr.mean()) if sidecar_arr.size else 0.0,
        "ffi_mean": float(ffi_arr.mean()) if ffi_arr.size else 0.0,
        "sidecar_max": float(sidecar_arr.max()) if sidecar_arr.size else 0.0,
        "ffi_max": float(ffi_arr.max()) if ffi_arr.size else 0.0,
    }


def _normalize_det_logits(preds, batch_n: int):
    import numpy as np

    arrays = list(preds) if isinstance(preds, (tuple, list)) else [preds]
    if not arrays:
        raise ValueError("empty detector predictor output")
    probs = np.asarray(arrays[0], dtype=np.float32)
    if probs.ndim == 3:
        probs = np.expand_dims(probs, axis=1)
    if probs.ndim != 4:
        raise ValueError(f"unexpected detector output ndim: {probs.ndim}")
    if probs.shape[0] != batch_n:
        raise ValueError(
            f"unexpected detector batch: {probs.shape[0]} (expected {batch_n})"
        )
    return probs


def _ensure_probability_map_np(values):
    import numpy as np

    arr = np.asarray(values, dtype=np.float32)
    if arr.size == 0:
        return arr
    if float(arr.min()) < 0.0 or float(arr.max()) > 1.0:
        arr = 1.0 / (1.0 + np.exp(-arr))
    return arr


def compare_ffi_det_dump(
    ocr,
    dump_path: str,
    include_values: bool = False,
) -> dict:
    import numpy as np

    path = Path(dump_path)
    with path.open(encoding="utf-8") as fp:
        payload = json.load(fp)

    input_shape = [int(v) for v in payload["input_shape"]]
    pred_shape = [int(v) for v in payload["pred_shape"]]
    batch_n, channels, det_h, det_w = input_shape
    if channels != 3:
        raise ValueError(f"unexpected det input channels: {channels}")

    input_values = np.asarray(payload["input_values"], dtype=np.float32)
    expected_input = batch_n * channels * det_h * det_w
    if input_values.size != expected_input:
        raise ValueError(
            f"det input_values size mismatch: {input_values.size} vs {expected_input}"
        )
    batch_input = input_values.reshape(batch_n, channels, det_h, det_w)

    pipeline = getattr(getattr(ocr, "paddlex_pipeline", None), "_pipeline", None)
    if pipeline is None:
        raise ValueError("missing PaddleX pipeline")
    text_det_model = pipeline.text_det_model
    preds = text_det_model.infer(x=[batch_input])
    sidecar_raw = _normalize_det_logits(preds, batch_n)

    if sidecar_raw.shape[1] > 1:
        sidecar_map = sidecar_raw.max(axis=1)
    else:
        sidecar_map = sidecar_raw[:, 0]
    sidecar_map = _ensure_probability_map_np(sidecar_map)

    ffi_values = np.asarray(payload["values"], dtype=np.float32)
    expected_pred = int(np.prod(pred_shape))
    if ffi_values.size != expected_pred:
        raise ValueError(
            f"det values size mismatch: {ffi_values.size} vs {expected_pred}"
        )
    ffi_map = ffi_values.reshape(pred_shape)
    if ffi_map.ndim == 4:
        ffi_map = ffi_map[:, 0]
    elif ffi_map.ndim != 3:
        raise ValueError(f"unexpected ffi det map ndim: {ffi_map.ndim}")

    if sidecar_map.shape != ffi_map.shape:
        raise ValueError(
            f"det map shape mismatch: {tuple(sidecar_map.shape)} vs {tuple(ffi_map.shape)}"
        )

    diff = np.abs(sidecar_map - ffi_map)
    disagreement = np.logical_xor(sidecar_map > 0.3, ffi_map > 0.3)

    output = {
        "source_dump": path.name,
        "input_shape": input_shape,
        "pred_shape": list(sidecar_map.shape),
        "comparison": {
            "mean_abs_diff": float(diff.mean()) if diff.size else 0.0,
            "max_abs_diff": float(diff.max()) if diff.size else 0.0,
            "sidecar_mean": float(sidecar_map.mean()) if sidecar_map.size else 0.0,
            "ffi_mean": float(ffi_map.mean()) if ffi_map.size else 0.0,
            "sidecar_on": int((sidecar_map > 0.3).sum()),
            "ffi_on": int((ffi_map > 0.3).sum()),
            "threshold_disagreement": int(disagreement.sum()),
        },
    }
    if include_values:
        output["sidecar_values"] = sidecar_map.reshape(-1).astype(float).tolist()
        output["ffi_values"] = ffi_map.reshape(-1).astype(float).tolist()

    dump_dir = resolve_debug_dump_dir()
    if dump_dir is None:
        dump_dir = path.parent
    dump_dir.mkdir(parents=True, exist_ok=True)
    compare_path = dump_dir / f"sidecar_vs_ffi_{path.stem}.json"
    with compare_path.open("w", encoding="utf-8") as fp:
        json.dump(output, fp, ensure_ascii=False)
    output["compare_dump"] = str(compare_path)
    return output


def compare_ffi_rec_dump(
    ocr,
    dump_path: str,
    original_index: int | None = None,
    include_values: bool = False,
) -> dict:
    import numpy as np

    path = Path(dump_path)
    with path.open(encoding="utf-8") as fp:
        payload = json.load(fp)

    input_shape = payload["input_shape"]
    batch_n, channels, rec_h, batch_w = [int(v) for v in input_shape]
    if channels != 3:
        raise ValueError(f"unexpected rec input channels: {channels}")

    items = payload.get("items", [])
    if len(items) != batch_n:
        raise ValueError(f"item count mismatch: {len(items)} vs batch {batch_n}")

    if original_index is not None:
        filtered = [
            item
            for item in items
            if int(item.get("original_index", item.get("index", -1))) == original_index
        ]
        if not filtered:
            raise ValueError(f"original_index {original_index} not found in dump")
        items = filtered
        batch_n = len(items)

    batch_input = np.zeros((batch_n, channels, rec_h, batch_w), dtype=np.float32)
    ratios = []
    for batch_index, item in enumerate(items):
        rec_width = int(item["rec_width"])
        values = np.asarray(item["input_values"], dtype=np.float32)
        expected = channels * rec_h * rec_width
        if values.size != expected:
            raise ValueError(
                f"input_values size mismatch for item {batch_index}: {values.size} vs {expected}"
            )
        batch_input[batch_index, :, :, :rec_width] = values.reshape(
            channels, rec_h, rec_width
        )
        ratios.append(float(item.get("ratio", item["image_width"] / float(item["image_height"]))))

    pipeline = getattr(getattr(ocr, "paddlex_pipeline", None), "_pipeline", None)
    if pipeline is None:
        raise ValueError("missing PaddleX pipeline")
    text_rec_model = pipeline.text_rec_model
    preds = text_rec_model.infer(x=[batch_input])
    sidecar_probs, sidecar_layout = _normalize_rec_logits(
        preds,
        batch_n,
        int(payload.get("time_steps", 0)) or None,
        int(payload.get("num_classes", 0)) or None,
    )

    time_steps = int(sidecar_probs.shape[1])
    num_classes = int(sidecar_probs.shape[2])
    ffi_time_steps = int(payload.get("time_steps", 0)) or time_steps
    ffi_num_classes = int(payload.get("num_classes", 0)) or num_classes
    rec_image_shape = next(
        op["RecResizeImg"]["image_shape"]
        for op in text_rec_model.config["PreProcess"]["transform_ops"]
        if "RecResizeImg" in op
    )
    _, img_h, img_w = rec_image_shape[:3]
    max_wh_ratio = max(img_w / img_h, max(ratios, default=(img_w / img_h)))

    ffi_probs = np.stack(
        [
            np.asarray(item["values"], dtype=np.float32).reshape(
                ffi_time_steps, ffi_num_classes
            )
            for item in items
        ],
        axis=0,
    )
    sidecar_texts, sidecar_scores = text_rec_model.post_op(
        [sidecar_probs],
        return_word_box=False,
        wh_ratio_list=ratios,
        max_wh_ratio=max_wh_ratio,
    )
    ffi_texts, ffi_scores = text_rec_model.post_op(
        [ffi_probs],
        return_word_box=False,
        wh_ratio_list=ratios,
        max_wh_ratio=max_wh_ratio,
    )

    result_items = []
    for batch_index, item in enumerate(items):
        sidecar_values = sidecar_probs[batch_index].reshape(-1)
        ffi_values = ffi_probs[batch_index].reshape(-1)
        result_item = {
            "index": batch_index,
            "original_index": int(
                item.get("original_index", item.get("index", batch_index))
            ),
            "ratio": float(item.get("ratio", ratios[batch_index])),
            "polygon": item.get("polygon", []),
            "rec_width": int(item["rec_width"]),
            "cls_label": item.get("cls_label"),
            "cls_score": item.get("cls_score"),
            "rotated_180": item.get("rotated_180"),
            "sidecar_text": sidecar_texts[batch_index],
            "sidecar_score": float(sidecar_scores[batch_index]),
            "ffi_text": ffi_texts[batch_index],
            "ffi_score": float(ffi_scores[batch_index]),
            "comparison": _compare_rec_values(sidecar_values, ffi_values),
        }
        if include_values:
            result_item["sidecar_values"] = sidecar_values.astype(float).tolist()
            result_item["ffi_values"] = ffi_values.astype(float).tolist()
        result_items.append(result_item)

    output = {
        "source_dump": path.name,
        "input_shape": [batch_n, channels, rec_h, batch_w],
        "output_shape": [batch_n, time_steps, num_classes],
        "sidecar_layout": sidecar_layout,
        "items": result_items,
    }

    dump_dir = resolve_debug_dump_dir()
    if dump_dir is None:
        dump_dir = path.parent
    dump_dir.mkdir(parents=True, exist_ok=True)
    compare_name = f"sidecar_vs_ffi_{path.stem}"
    if original_index is not None:
        compare_name += f"_orig{original_index}"
    compare_path = dump_dir / f"{compare_name}.json"
    with compare_path.open("w", encoding="utf-8") as fp:
        json.dump(output, fp, ensure_ascii=False)
    output["compare_dump"] = str(compare_path)
    return output


def compare_ffi_crop_dump(
    dump_path: str,
    include_values: bool = False,
) -> dict:
    import cv2
    import numpy as np

    def _points_close(lhs, rhs, atol: float = 1e-4) -> bool:
        if lhs is None or rhs is None:
            return lhs == rhs
        lhs_arr = np.asarray(lhs, dtype=np.float64)
        rhs_arr = np.asarray(rhs, dtype=np.float64)
        if lhs_arr.shape != rhs_arr.shape:
            return False
        return bool(np.allclose(lhs_arr, rhs_arr, atol=atol, rtol=0.0))

    path = Path(dump_path)
    with path.open(encoding="utf-8") as fp:
        ffi = json.load(fp)

    dump_dir = resolve_debug_dump_dir()
    if dump_dir is None:
        dump_dir = path.parent

    stem_parts = path.stem.split("_")
    if len(stem_parts) < 5:
        raise ValueError(f"unexpected crop dump name: {path.name}")
    stage = "_".join(stem_parts[:2])
    ffi_x = int(stem_parts[2])
    ffi_y = int(stem_parts[3])
    ffi_w, ffi_h = map(int, stem_parts[4].split("x", 1))

    candidates = list(dump_dir.glob(f"sidecar_{stage}_*.json"))
    if not candidates:
        raise ValueError(f"matching sidecar crop dump not found for stage: {stage}")

    def _candidate_score(candidate: Path) -> tuple[int, int, int, int]:
        parts = candidate.stem.split("_")
        if len(parts) < 6:
            return (10**9, 10**9, 10**9, 10**9)
        try:
            cand_x = int(parts[3])
            cand_y = int(parts[4])
            cand_w, cand_h = map(int, parts[5].split("x", 1))
        except ValueError:
            return (10**9, 10**9, 10**9, 10**9)
        return (
            abs(cand_x - ffi_x),
            abs(cand_y - ffi_y),
            abs(cand_w - ffi_w),
            abs(cand_h - ffi_h),
        )

    sidecar_json = min(candidates, key=_candidate_score)
    with sidecar_json.open(encoding="utf-8") as fp:
        sidecar = json.load(fp)

    ffi_bmp = path.with_suffix(".bmp")
    sidecar_png = sidecar_json.with_suffix(".png")
    ffi_img = cv2.imread(str(ffi_bmp), cv2.IMREAD_COLOR)
    sidecar_img = cv2.imread(str(sidecar_png), cv2.IMREAD_COLOR)
    if ffi_img is None:
        raise ValueError(f"failed to read ffi crop bitmap: {ffi_bmp}")
    if sidecar_img is None:
        raise ValueError(f"failed to read sidecar crop bitmap: {sidecar_png}")

    output = {
        "source_dump": path.name,
        "input_polygon_match": _points_close(
            ffi.get("input_polygon"),
            sidecar.get("input_polygon"),
        ),
        "crop_quad_match": _points_close(
            ffi.get("crop_quad"),
            sidecar.get("crop_quad"),
        ),
        "output_size_match": ffi.get("output_size") == sidecar.get("output_size"),
        "ffi_shape": [int(ffi_img.shape[1]), int(ffi_img.shape[0]), int(ffi_img.shape[2])],
        "sidecar_shape": [int(sidecar_img.shape[1]), int(sidecar_img.shape[0]), int(sidecar_img.shape[2])],
    }

    if ffi_img.shape == sidecar_img.shape:
        diff = np.abs(ffi_img.astype(np.int16) - sidecar_img.astype(np.int16))
        output["comparison"] = {
            "mean_abs_diff": float(diff.mean()) if diff.size else 0.0,
            "max_abs_diff": int(diff.max()) if diff.size else 0,
            "nonzero_pixels": int(np.any(diff != 0, axis=2).sum()) if diff.ndim == 3 else int((diff != 0).sum()),
        }
        if include_values:
            output["ffi_values"] = ffi_img.reshape(-1).astype(int).tolist()
            output["sidecar_values"] = sidecar_img.reshape(-1).astype(int).tolist()
    else:
        output["comparison"] = {"shape_mismatch": True}

    compare_path = dump_dir / f"sidecar_vs_ffi_{path.stem}.json"
    with compare_path.open("w", encoding="utf-8") as fp:
        json.dump(output, fp, ensure_ascii=False)
    output["compare_dump"] = str(compare_path)
    return output


def warmup_models(ocrs: dict) -> None:
    with tempfile.NamedTemporaryFile(suffix=".bmp", delete=False) as fp:
        fp.write(WARMUP_IMAGE)
        warmup_path = fp.name

    try:
        for ocr in ocrs.values():
            try:
                ocr.predict(warmup_path, text_rec_score_thresh=0.5)
            except Exception as exc:  # pragma: no cover
                print(f"warmup failed: {exc}", file=sys.stderr, flush=True)
    finally:
        try:
            os.remove(warmup_path)
        except OSError:
            pass


def predict_image(
    ocr,
    image_path: str,
    score_thresh: float,
) -> tuple[list[dict], list[dict]]:
    result = ocr.predict(image_path, text_rec_score_thresh=score_thresh)
    if not result:
        return [], []

    item = result[0]
    polys = item.get("rec_polys", [])
    texts = item.get("rec_texts", [])
    scores = item.get("rec_scores", [1.0] * len(texts))
    debug_candidates = item.get("debug_candidates", [])

    detections = []
    debug_detections = []
    for poly, text, score in zip(polys, texts, scores):
        polygon = poly.tolist() if hasattr(poly, "tolist") else poly
        score_value = float(score)
        detections.append({"polygon": polygon, "text": text})
    if debug_candidates:
        debug_detections = debug_candidates
    else:
        for poly, text, score in zip(polys, texts, scores):
            polygon = poly.tolist() if hasattr(poly, "tolist") else poly
            score_value = float(score)
            debug_detections.append(
                {
                    "polygon": polygon,
                    "text": text,
                    "score": score_value,
                    "accepted": True,
                }
            )

    return detections, debug_detections


def emit(obj: dict) -> None:
    print(json.dumps(obj, ensure_ascii=False), flush=True)


def parse_request(line: str) -> tuple[int, str, str, float]:
    request = json.loads(line)
    return (
        int(request["id"]),
        request["source"],
        request["image_path"],
        float(request.get("score_thresh", 0.5)),
    )


def run_server() -> int:
    selected = resolve_selected_lang()
    ocrs: dict = {selected: build_ocr(selected)}
    warmup_models(ocrs)
    emit({"type": "ready", "langs": [selected]})

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            request_id, source, image_path, score_thresh = parse_request(line)
        except Exception as exc:
            emit({"type": "error", "id": -1, "message": f"invalid request: {exc}"})
            continue

        try:
            if source not in ocrs:
                raise ValueError(
                    f"selected language is '{selected}' but request source is '{source}'; "
                    "restart OCR server with the new PYTHON_OCR_LANG to switch language"
                )
            detections, debug_detections = predict_image(
                ocrs[source], image_path, score_thresh
            )
            emit(
                {
                    "type": "result",
                    "id": request_id,
                    "detections": detections,
                    "debug_detections": debug_detections,
                }
            )
        except Exception as exc:
            emit({"type": "error", "id": request_id, "message": str(exc)})

    return 0


def run_single(image_path: str, source: str, score_thresh: float) -> int:
    ocr = build_ocr(source)
    detections, debug_detections = predict_image(ocr, image_path, score_thresh)
    emit({"detections": detections, "debug_detections": debug_detections})
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", action="store_true")
    parser.add_argument("--image")
    parser.add_argument("--source", default="en", choices=LANGS)
    parser.add_argument("--score-thresh", type=float, default=0.5)
    parser.add_argument("--compare-ffi-rec-dump")
    parser.add_argument("--compare-ffi-det-dump")
    parser.add_argument("--compare-ffi-crop-dump")
    parser.add_argument("--compare-original-index", type=int)
    parser.add_argument("--compare-include-values", action="store_true")
    return parser.parse_args()


def main() -> int:
    configure_frozen_dll_search_path()
    configure_windows_paddle_probe_env()
    args = parse_args()
    if args.server:
        return run_server()
    if args.compare_ffi_rec_dump:
        ocr = build_ocr(args.source)
        emit(
            compare_ffi_rec_dump(
                ocr,
                args.compare_ffi_rec_dump,
                args.compare_original_index,
                args.compare_include_values,
            )
        )
        return 0
    if args.compare_ffi_det_dump:
        ocr = build_ocr(args.source)
        emit(
            compare_ffi_det_dump(
                ocr,
                args.compare_ffi_det_dump,
                args.compare_include_values,
            )
        )
        return 0
    if args.compare_ffi_crop_dump:
        emit(
            compare_ffi_crop_dump(
                args.compare_ffi_crop_dump,
                args.compare_include_values,
            )
        )
        return 0
    if not args.image:
        raise SystemExit("--image 또는 --server가 필요합니다")
    return run_single(args.image, args.source, args.score_thresh)


if __name__ == "__main__":
    raise SystemExit(main())
