"""ONNX Runtime detector for raw YOLOv3-tiny heads."""

from __future__ import annotations

from dataclasses import dataclass
from typing import List, Sequence, Tuple

import cv2
import numpy as np


@dataclass
class Detection:
    class_name: str
    confidence: float
    x1: int
    y1: int
    x2: int
    y2: int


def _sigmoid(x: np.ndarray) -> np.ndarray:
    return 1.0 / (1.0 + np.exp(-np.clip(x, -30.0, 30.0)))


class YoloDetector:
    """YOLOv3-tiny inference through ONNX Runtime's ARM CPU provider."""

    def __init__(
        self,
        model_path: str,
        names_path: str,
        conf_threshold: float = 0.35,
        nms_threshold: float = 0.4,
        num_threads: int = 3,
        anchors: Sequence[Tuple[float, float]] = (
            (39, 61), (57, 60), (72, 69), (49, 117), (100, 91), (155, 158),
        ),
        masks: Sequence[Sequence[int]] = ((3, 4, 5), (0, 1, 2)),
    ) -> None:
        try:
            import onnxruntime as ort
        except ImportError as exc:
            raise RuntimeError(
                'onnxruntime is required: python3 -m pip install --user onnxruntime'
            ) from exc

        self.conf_threshold = float(conf_threshold)
        self.nms_threshold = float(nms_threshold)
        self.anchors = tuple((float(a), float(b)) for a, b in anchors)
        self.masks = tuple(tuple(int(i) for i in mask) for mask in masks)
        with open(names_path, encoding='utf-8') as names_file:
            self.names: List[str] = [line.strip() for line in names_file if line.strip()]

        options = ort.SessionOptions()
        options.intra_op_num_threads = max(1, int(num_threads))
        options.inter_op_num_threads = 1
        options.execution_mode = ort.ExecutionMode.ORT_SEQUENTIAL
        options.graph_optimization_level = ort.GraphOptimizationLevel.ORT_ENABLE_ALL
        self.session = ort.InferenceSession(
            model_path, sess_options=options, providers=['CPUExecutionProvider'])
        model_input = self.session.get_inputs()[0]
        self.input_name = model_input.name
        shape = model_input.shape
        if len(shape) != 4 or not isinstance(shape[2], int) or not isinstance(shape[3], int):
            raise ValueError(f'Expected static NCHW input, got {shape}')
        self.input_height = shape[2]
        self.input_width = shape[3]
        self.output_names = [output.name for output in self.session.get_outputs()]

    def detect(self, frame: np.ndarray) -> List[Detection]:
        frame_height, frame_width = frame.shape[:2]
        resized = cv2.resize(
            frame, (self.input_width, self.input_height), interpolation=cv2.INTER_LINEAR)
        tensor = resized[:, :, ::-1].transpose(2, 0, 1)
        tensor = np.ascontiguousarray(tensor, dtype=np.float32)[None] / 255.0
        outputs = self.session.run(self.output_names, {self.input_name: tensor})

        boxes: List[List[int]] = []
        confidences: List[float] = []
        class_ids: List[int] = []
        if len(outputs) != len(self.masks):
            raise RuntimeError(
                f'Model has {len(outputs)} output(s), expected {len(self.masks)} raw '
                'YOLO heads. Use the bundled Darknet converter.')
        for raw, mask in zip(outputs, self.masks):
            self._decode_head(
                raw, mask, frame_width, frame_height, boxes, confidences, class_ids)

        if not boxes:
            return []
        indices = cv2.dnn.NMSBoxes(
            boxes, confidences, self.conf_threshold, self.nms_threshold)
        if len(indices) == 0:
            return []

        detections: List[Detection] = []
        for index in np.asarray(indices).reshape(-1):
            index = int(index)
            x, y, width, height = boxes[index]
            class_id = class_ids[index]
            detections.append(Detection(
                class_name=(self.names[class_id]
                            if class_id < len(self.names) else str(class_id)),
                confidence=confidences[index],
                x1=max(0, x), y1=max(0, y),
                x2=min(frame_width - 1, x + width),
                y2=min(frame_height - 1, y + height),
            ))
        return detections

    def _decode_head(
        self,
        raw: np.ndarray,
        mask: Sequence[int],
        frame_width: int,
        frame_height: int,
        boxes: List[List[int]],
        confidences: List[float],
        class_ids: List[int],
    ) -> None:
        raw = np.asarray(raw)
        if raw.ndim != 4 or raw.shape[0] != 1:
            raise RuntimeError(f'Unexpected YOLO output shape: {raw.shape}')
        channels = len(mask) * (len(self.names) + 5)
        if raw.shape[1] == channels:
            raw = raw[0].reshape(
                len(mask), len(self.names) + 5, raw.shape[2], raw.shape[3]
            ).transpose(0, 2, 3, 1)
        elif raw.shape[-1] == channels:
            raw = raw[0].reshape(
                raw.shape[1], raw.shape[2], len(mask), len(self.names) + 5
            ).transpose(2, 0, 1, 3)
        else:
            raise RuntimeError(f'YOLO output channels do not match classes: {raw.shape}')

        grid_h, grid_w = raw.shape[1:3]
        grid_x, grid_y = np.meshgrid(np.arange(grid_w), np.arange(grid_h))
        scale_x = frame_width / float(grid_w)
        scale_y = frame_height / float(grid_h)
        for anchor_slot, anchor_index in enumerate(mask):
            pred = raw[anchor_slot]
            objectness = _sigmoid(pred[..., 4])
            class_scores = _sigmoid(pred[..., 5:]) * objectness[..., None]
            class_id = np.argmax(class_scores, axis=-1)
            confidence = np.max(class_scores, axis=-1)
            ys, xs = np.nonzero(confidence >= self.conf_threshold)
            if not len(xs):
                continue
            anchor_w, anchor_h = self.anchors[anchor_index]
            centers_x = (_sigmoid(pred[ys, xs, 0]) + grid_x[ys, xs]) * scale_x
            centers_y = (_sigmoid(pred[ys, xs, 1]) + grid_y[ys, xs]) * scale_y
            # Darknet anchors are expressed in the network input coordinate
            # system (512x288 for this model), not in camera-frame pixels.
            # Scale box size just like the decoded center coordinates.
            widths = (
                np.exp(np.clip(pred[ys, xs, 2], -10.0, 10.0)) * anchor_w *
                frame_width / float(self.input_width))
            heights = (
                np.exp(np.clip(pred[ys, xs, 3], -10.0, 10.0)) * anchor_h *
                frame_height / float(self.input_height))
            for i in range(len(xs)):
                width, height = float(widths[i]), float(heights[i])
                boxes.append([
                    int(centers_x[i] - width / 2),
                    int(centers_y[i] - height / 2),
                    max(1, int(width)), max(1, int(height)),
                ])
                confidences.append(float(confidence[ys[i], xs[i]]))
                class_ids.append(int(class_id[ys[i], xs[i]]))
