"""
yolo_safety_node  —  Camera → ONNX Runtime YOLOv3-tiny → /adas/safety_state

Replaces the KR260 RPU UART path for RPi-local development.
Inference runs in a background thread so the ROS2 event loop stays responsive.

Published topics
  /adas/safety_state        std_msgs/Int32   0=CLEAR 1=SLOW 2=STOP
  /adas/debug_image/compressed  sensor_msgs/CompressedImage  (optional, JPEG)

Parameters (all settable from launch file)
  model_path        str   — converted .onnx file (required)
  names_path        str   — class names file (required)
  camera_index      int   — V4L2 device index (default 0)
  cam_width         int   — requested capture width  (default 1280)
  cam_height        int   — requested capture height (default 720)
  conf_threshold    float — detection confidence gate (default 0.35)
  nms_threshold     float — NMS IoU threshold (default 0.4)
  use_roi           bool  — restrict safety judgement to ROI (default false)
  roi_x_min         float — ROI left fraction (default 0.3)
  roi_x_max         float — ROI right fraction (default 0.7)
  stop_duration     float — STOPPING hold seconds (default 3.0)
  miss_frames       int   — HANDLED drain frame count (default 10)
  publish_debug_image bool — publish annotated JPEG (default false)
  publish_fps       float — maximum annotated-image rate (default 10)
  debug_jpeg_quality int — web-view JPEG quality (default 80)
"""

from __future__ import annotations

import threading
import time

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import CompressedImage
from std_msgs.msg import Int32

from rpi_adas_demo.config import (
    CONF_THRESHOLD,
    MISS_FRAME_THRESHOLD,
    NMS_THRESHOLD,
    ROI_X_MAX,
    ROI_X_MIN,
    STOP_DURATION_SEC,
    USE_ROI,
    SafetyState,
)
from rpi_adas_demo.detector import YoloDetector
from rpi_adas_demo.safety_fsm import SafetyFSM
from rpi_adas_demo.safety_judge import SafetyJudge


class YoloSafetyNode(Node):
    def __init__(self) -> None:
        super().__init__('yolo_safety_node')

        self.declare_parameter('model_path', '')
        self.declare_parameter('names_path', '')
        self.declare_parameter('camera_index', 0)
        self.declare_parameter('cam_width', 1280)
        self.declare_parameter('cam_height', 720)
        self.declare_parameter('camera_fourcc', 'MJPG')
        self.declare_parameter('camera_fps', 30.0)
        self.declare_parameter('conf_threshold', CONF_THRESHOLD)
        self.declare_parameter('nms_threshold', NMS_THRESHOLD)
        self.declare_parameter('use_roi', USE_ROI)
        self.declare_parameter('roi_x_min', ROI_X_MIN)
        self.declare_parameter('roi_x_max', ROI_X_MAX)
        self.declare_parameter('stop_duration', STOP_DURATION_SEC)
        self.declare_parameter('miss_frames', MISS_FRAME_THRESHOLD)
        self.declare_parameter('publish_debug_image', False)
        self.declare_parameter('publish_fps', 10.0)
        self.declare_parameter('debug_jpeg_quality', 80)
        self.declare_parameter('inference_threads', 3)

        model = self.get_parameter('model_path').value
        names = self.get_parameter('names_path').value
        if not model or not names:
            self.get_logger().fatal(
                'model_path and names_path must be set.'
            )
            raise RuntimeError('Model paths not configured')

        conf = float(self.get_parameter('conf_threshold').value)
        nms = float(self.get_parameter('nms_threshold').value)

        self.get_logger().info(f'Loading ONNX model from {model}')
        self._detector = YoloDetector(
            model_path=model, names_path=names,
            conf_threshold=conf, nms_threshold=nms,
            num_threads=int(self.get_parameter('inference_threads').value),
        )
        self._judge = SafetyJudge(
            conf_threshold=conf,
            use_roi=bool(self.get_parameter('use_roi').value),
            roi_x_min=float(self.get_parameter('roi_x_min').value),
            roi_x_max=float(self.get_parameter('roi_x_max').value),
        )
        self._fsm = SafetyFSM(
            stop_duration=float(self.get_parameter('stop_duration').value),
            miss_frame_threshold=int(self.get_parameter('miss_frames').value),
        )

        cam_idx = int(self.get_parameter('camera_index').value)
        # V4L2 plus a one-frame buffer avoids showing frames that accumulated
        # while CPU inference was running.
        self._cap = cv2.VideoCapture(cam_idx, cv2.CAP_V4L2)
        if not self._cap.isOpened():
            self.get_logger().fatal(f'Cannot open camera index {cam_idx}')
            raise RuntimeError('Camera open failed')
        fourcc = str(self.get_parameter('camera_fourcc').value)
        if len(fourcc) == 4:
            self._cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*fourcc))
        self._cap.set(cv2.CAP_PROP_FRAME_WIDTH, int(self.get_parameter('cam_width').value))
        self._cap.set(cv2.CAP_PROP_FRAME_HEIGHT, int(self.get_parameter('cam_height').value))
        self._cap.set(
            cv2.CAP_PROP_FPS, float(self.get_parameter('camera_fps').value))
        self._cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self.get_logger().info(
            f'Camera {cam_idx}: '
            f'{int(self._cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x'
            f'{int(self._cap.get(cv2.CAP_PROP_FRAME_HEIGHT))}'
        )

        self._pub_state = self.create_publisher(Int32, '/adas/safety_state', 10)

        self._pub_debug = None
        if bool(self.get_parameter('publish_debug_image').value):
            image_qos = QoSProfile(
                history=HistoryPolicy.KEEP_LAST, depth=1,
                reliability=ReliabilityPolicy.BEST_EFFORT,
                durability=DurabilityPolicy.VOLATILE)
            self._pub_debug = self.create_publisher(
                CompressedImage, '/adas/debug_image/compressed', image_qos)
            self.get_logger().info('Debug image: /adas/debug_image/compressed')

        self._stop_event = threading.Event()
        self._frame_lock = threading.Lock()
        self._latest_frame: np.ndarray | None = None
        self._latest_frame_id = 0
        self._debug_lock = threading.Lock()
        self._latest_debug_frame: np.ndarray | None = None
        self._latest_debug_id = 0
        self._capture_thread = threading.Thread(target=self._capture_loop, daemon=True)
        self._thread = threading.Thread(target=self._detection_loop, daemon=True)
        self._debug_thread = threading.Thread(target=self._debug_loop, daemon=True)
        self._capture_thread.start()
        self._thread.start()
        if self._pub_debug is not None:
            self._debug_thread.start()
        self.get_logger().info('yolo_safety_node running')

    # ------------------------------------------------------------------
    def _capture_loop(self) -> None:
        """Continuously overwrite the frame slot: inference always gets newest."""
        consecutive_failures = 0
        while not self._stop_event.is_set() and self._cap.isOpened():
            ret, frame = self._cap.read()
            if not ret:
                consecutive_failures += 1
                if consecutive_failures >= 10:
                    self.get_logger().error('Camera read failed 10 times — stopping')
                    self._stop_event.set()
                    return
                time.sleep(0.05)
                continue
            consecutive_failures = 0
            with self._frame_lock:
                self._latest_frame = frame
                self._latest_frame_id += 1

    def _detection_loop(self) -> None:
        last_frame_id = 0
        while not self._stop_event.is_set():
            try:
                with self._frame_lock:
                    if self._latest_frame is None or self._latest_frame_id == last_frame_id:
                        frame = None
                    else:
                        frame = self._latest_frame.copy()
                        last_frame_id = self._latest_frame_id
                if frame is None:
                    time.sleep(0.005)
                    continue

                started = time.monotonic()
                detections = self._detector.detect(frame)
                inference_ms = (time.monotonic() - started) * 1000.0
                judge_result = self._judge.judge(
                    detections, frame.shape[1], frame.shape[0])
                fsm_result = self._fsm.update(judge_result)

                msg = Int32(data=int(fsm_result.safety_state))
                self._pub_state.publish(msg)

                self.get_logger().info(
                    f'{fsm_result.safety_state.name}  '
                    f'fsm={fsm_result.fsm_state.name}  {fsm_result.reason}  '
                    f'inference={inference_ms:.1f}ms',
                    throttle_duration_sec=2.0,
                )

                if self._pub_debug is not None:
                    annotated = self._annotate(frame, detections, fsm_result)
                    with self._debug_lock:
                        # Single overwrite slot: an encoder that falls behind never
                        # accumulates stale frames.
                        self._latest_debug_frame = annotated
                        self._latest_debug_id += 1

            except Exception as exc:  # noqa: BLE001
                self.get_logger().error(f'Detection loop error: {exc}', throttle_duration_sec=1.0)
                import traceback
                traceback.print_exc()

    def _annotate(self, frame, detections, fsm_result):
        scale = max(1.0, frame.shape[0] / 360.0)
        box_thickness = max(2, round(2 * scale))
        label_scale = 0.5 * scale
        label_thickness = max(1, round(scale))
        for det in detections:
            color = (0, 255, 0) if det.confidence >= 0.5 else (0, 200, 255)
            cv2.rectangle(
                frame, (det.x1, det.y1), (det.x2, det.y2), color, box_thickness)
            cv2.putText(
                frame, f'{det.class_name} {det.confidence:.2f}',
                (det.x1, max(det.y1 - round(6 * scale), 0)),
                cv2.FONT_HERSHEY_SIMPLEX, label_scale, color,
                label_thickness, cv2.LINE_AA,
            )
        state_color = {
            SafetyState.CLEAR: (0, 255, 0),
            SafetyState.SLOW: (0, 200, 255),
            SafetyState.STOP: (0, 0, 255),
        }.get(fsm_result.safety_state, (255, 255, 255))
        cv2.putText(
            frame, f'STATE: {fsm_result.safety_state.name}',
            (round(10 * scale), round(35 * scale)),
            cv2.FONT_HERSHEY_SIMPLEX, 1.0 * scale, state_color,
            max(2, round(2 * scale)), cv2.LINE_AA,
        )
        cv2.putText(
            frame, fsm_result.reason,
            (round(10 * scale), round(65 * scale)),
            cv2.FONT_HERSHEY_SIMPLEX, 0.55 * scale, (200, 200, 200),
            max(1, round(scale)), cv2.LINE_AA,
        )
        return frame

    def _debug_loop(self) -> None:
        publish_fps = max(0.1, float(self.get_parameter('publish_fps').value))
        interval = 1.0 / publish_fps
        quality = max(1, min(100, int(self.get_parameter('debug_jpeg_quality').value)))
        last_id = 0
        next_publish = time.monotonic()
        while not self._stop_event.is_set():
            delay = next_publish - time.monotonic()
            if delay > 0:
                self._stop_event.wait(min(delay, 0.1))
                continue
            with self._debug_lock:
                if self._latest_debug_frame is None or self._latest_debug_id == last_id:
                    frame = None
                else:
                    frame = self._latest_debug_frame.copy()
                    last_id = self._latest_debug_id
            next_publish = max(next_publish + interval, time.monotonic())
            if frame is None:
                continue
            self._publish_debug(frame, quality)

    def _publish_debug(self, frame, quality: int) -> None:
        ok, buf = cv2.imencode('.jpg', frame, [cv2.IMWRITE_JPEG_QUALITY, quality])
        if not ok:
            return
        img_msg = CompressedImage()
        img_msg.header.stamp = self.get_clock().now().to_msg()
        img_msg.format = 'jpeg'
        img_msg.data = buf.tobytes()
        self._pub_debug.publish(img_msg)

    # ------------------------------------------------------------------
    def destroy_node(self) -> None:
        self._stop_event.set()
        self._capture_thread.join(timeout=1.0)
        self._thread.join(timeout=3.0)
        if self._pub_debug is not None:
            self._debug_thread.join(timeout=1.0)
        if self._cap.isOpened():
            self._cap.release()
        super().destroy_node()


def main(args=None) -> None:
    rclpy.init(args=args)
    node = YoloSafetyNode()
    try:
        rclpy.spin(node)
    except KeyboardInterrupt:
        pass
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


if __name__ == '__main__':
    main()
