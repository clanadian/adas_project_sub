"""Reference pre/post-processing for the class-agnostic proposal model —
pure numpy, no ultralytics/torch dependency, meant to be read (and ported to
C++) line by line rather than imported as a black box.

Confirmed empirically against this project's nc=1 YOLOv8n architecture
(see roi_proposal_jetson history): ONNX output shape for imgsz=320 is
[1, 5, 2100] — 4 box channels (cx, cy, w, h, already decoded to INPUT-PIXEL
space, i.e. 0..320, NOT normalized 0..1) + 1 objectness/class channel
(already sigmoid-activated, i.e. already a 0..1 score — do not apply sigmoid
again), across 2100 anchor points (40x40 + 20x20 + 10x10 grid cells for
strides 8/16/32). This is standard Ultralytics detection export behavior,
not something specific to nc=1.

Preprocessing MUST use Ultralytics' letterbox padding color (114, 114, 114)
gray, not black — that's what the model is trained against internally, and
mismatching it here is a real, easy-to-miss train/inference divergence.
"""
from __future__ import annotations

from dataclasses import dataclass

import numpy as np

LETTERBOX_PAD_VALUE = 114  # Ultralytics default gray padding, all 3 channels


@dataclass
class LetterboxParams:
    scale: float          # original-pixels -> model-input-pixels
    pad_x: float           # left padding, in model-input pixels
    pad_y: float           # top padding, in model-input pixels
    orig_w: int
    orig_h: int


def letterbox_preprocess(frame_bgr: np.ndarray, imgsz: int) -> tuple[np.ndarray, LetterboxParams]:
    """frame_bgr: HxWx3 uint8, BGR (raw camera frame, e.g. 640x360).
    Returns (model_input, params) where model_input is 1x3xImgszxImgsz
    float32 in [0,1], RGB, ready for the ONNX model's "images" input."""
    h, w = frame_bgr.shape[:2]
    scale = min(imgsz / w, imgsz / h)
    new_w, new_h = round(w * scale), round(h * scale)
    pad_x, pad_y = (imgsz - new_w) / 2, (imgsz - new_h) / 2

    import cv2  # local import: this file is pure-numpy except for the resize itself

    resized = cv2.resize(frame_bgr, (new_w, new_h), interpolation=cv2.INTER_LINEAR)
    canvas = np.full((imgsz, imgsz, 3), LETTERBOX_PAD_VALUE, dtype=np.uint8)
    top, left = round(pad_y - 0.1), round(pad_x - 0.1)
    canvas[top:top + new_h, left:left + new_w] = resized

    rgb = cv2.cvtColor(canvas, cv2.COLOR_BGR2RGB).astype(np.float32) / 255.0
    chw = rgb.transpose(2, 0, 1)[None, ...]  # 1x3xHxW
    return chw, LetterboxParams(scale, left, top, w, h)


def decode_output(
    raw_output: np.ndarray,  # [1, 5, N] as documented above
    params: LetterboxParams,
    conf_threshold: float,
    nms_iou: float,
    max_proposals: int = 10,
) -> list[tuple[float, float, float, float, float]]:
    """Returns up to max_proposals of (x, y, width, height, score) in the
    ORIGINAL frame's pixel coordinate system, (x, y) = top-left corner —
    NOT Ultralytics' native center-xywh, per the deployment contract."""
    preds = raw_output[0].T  # [N, 5]: cx, cy, w, h, score (input-pixel space)
    scores = preds[:, 4]
    keep = scores >= conf_threshold
    preds, scores = preds[keep], scores[keep]
    if len(preds) == 0:
        return []

    cx, cy, w, h = preds[:, 0], preds[:, 1], preds[:, 2], preds[:, 3]
    x1, y1, x2, y2 = cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2

    # Undo letterbox: model-input-pixel space -> original frame pixel space.
    x1 = (x1 - params.pad_x) / params.scale
    y1 = (y1 - params.pad_y) / params.scale
    x2 = (x2 - params.pad_x) / params.scale
    y2 = (y2 - params.pad_y) / params.scale
    x1, x2 = np.clip(x1, 0, params.orig_w), np.clip(x2, 0, params.orig_w)
    y1, y2 = np.clip(y1, 0, params.orig_h), np.clip(y2, 0, params.orig_h)

    order = np.argsort(-scores)
    x1, y1, x2, y2, scores = x1[order], y1[order], x2[order], y2[order], scores[order]

    keep_idx = _nms(x1, y1, x2, y2, scores, nms_iou)[:max_proposals]
    return [
        (float(x1[i]), float(y1[i]), float(x2[i] - x1[i]), float(y2[i] - y1[i]), float(scores[i]))
        for i in keep_idx
    ]


def _nms(x1, y1, x2, y2, scores, iou_threshold) -> list[int]:
    areas = (x2 - x1) * (y2 - y1)
    order = list(range(len(scores)))  # already sorted by score desc by caller
    keep = []
    while order:
        i = order.pop(0)
        keep.append(i)
        remaining = []
        for j in order:
            xx1, yy1 = max(x1[i], x1[j]), max(y1[i], y1[j])
            xx2, yy2 = min(x2[i], x2[j]), min(y2[i], y2[j])
            inter = max(0.0, xx2 - xx1) * max(0.0, yy2 - yy1)
            iou = inter / (areas[i] + areas[j] - inter) if (areas[i] + areas[j] - inter) > 0 else 0.0
            if iou <= iou_threshold:
                remaining.append(j)
        order = remaining
    return keep
