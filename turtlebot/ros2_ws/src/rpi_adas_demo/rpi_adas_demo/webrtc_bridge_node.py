"""Newest-frame-only ROS compressed image to MediaMTX RTSP publisher."""

from __future__ import annotations

import threading
import time

import cv2
import numpy as np
import rclpy
from rclpy.node import Node
from rclpy.qos import DurabilityPolicy, HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import CompressedImage


class WebRtcBridgeNode(Node):
    """Push annotated frames to MediaMTX; MediaMTX serves them as WebRTC."""

    def __init__(self) -> None:
        super().__init__('webrtc_bridge')
        self.declare_parameter('topic', '/adas/debug_image/compressed')
        self.declare_parameter('rtsp_url', 'rtsp://127.0.0.1:8554/adas')
        self.declare_parameter('stream_fps', 10.0)
        self.declare_parameter('bitrate_kbps', 3000)
        self.declare_parameter('gstreamer_encoder', 'x264enc')

        self._stop_event = threading.Event()
        self._frame_lock = threading.Lock()
        self._latest_jpeg: bytes | None = None
        self._latest_frame_id = 0
        image_qos = QoSProfile(
            history=HistoryPolicy.KEEP_LAST, depth=1,
            reliability=ReliabilityPolicy.BEST_EFFORT,
            durability=DurabilityPolicy.VOLATILE)
        self._sub = self.create_subscription(
            CompressedImage, str(self.get_parameter('topic').value),
            self._on_image, image_qos)
        self._thread = threading.Thread(target=self._stream_loop, daemon=True)
        self._thread.start()
        self.get_logger().info(
            f'WebRTC bridge → {self.get_parameter("rtsp_url").value}; '
            'browser URL: http://<rpi-ip>:8889/adas')

    def _on_image(self, msg: CompressedImage) -> None:
        # Never decode in the ROS callback and never enqueue multiple messages.
        with self._frame_lock:
            self._latest_jpeg = bytes(msg.data)
            self._latest_frame_id += 1

    def _stream_loop(self) -> None:
        writer = None
        writer_size = None
        last_id = 0
        next_retry = 0.0
        while not self._stop_event.is_set():
            with self._frame_lock:
                if self._latest_jpeg is None or self._latest_frame_id == last_id:
                    jpeg = None
                else:
                    jpeg = self._latest_jpeg
                    last_id = self._latest_frame_id
            if jpeg is None:
                self._stop_event.wait(0.005)
                continue

            frame = cv2.imdecode(np.frombuffer(jpeg, dtype=np.uint8), cv2.IMREAD_COLOR)
            if frame is None:
                self.get_logger().warning(
                    'Dropped invalid JPEG', throttle_duration_sec=2.0)
                continue
            size = (frame.shape[1], frame.shape[0])
            if writer is None or writer_size != size:
                if writer is not None:
                    writer.release()
                if time.monotonic() < next_retry:
                    continue
                writer = self._open_writer(size)
                writer_size = size
                if writer is None:
                    next_retry = time.monotonic() + 2.0
                    continue
            try:
                writer.write(frame)
            except Exception as exc:  # noqa: BLE001
                self.get_logger().error(
                    f'RTSP writer failed, reconnecting: {exc}',
                    throttle_duration_sec=2.0)
                writer.release()
                writer = None
                next_retry = time.monotonic() + 1.0
        if writer is not None:
            writer.release()

    def _open_writer(self, size):
        fps = max(1.0, float(self.get_parameter('stream_fps').value))
        key_interval = max(1, round(fps))
        bitrate = max(100, int(self.get_parameter('bitrate_kbps').value))
        encoder = str(self.get_parameter('gstreamer_encoder').value)
        if encoder == 'x264enc':
            encoder_part = (
                f'x264enc tune=zerolatency speed-preset=ultrafast bitrate={bitrate} '
                f'key-int-max={key_interval} bframes=0 byte-stream=true')
        elif encoder == 'v4l2h264enc':
            encoder_part = (
                f'v4l2h264enc extra-controls="controls,video_bitrate={bitrate * 1000};"')
        else:
            self.get_logger().error(f'Unsupported gstreamer_encoder: {encoder}')
            return None
        pipeline = (
            'appsrc is-live=true do-timestamp=true format=time ! '
            'queue max-size-buffers=1 leaky=downstream ! videoconvert ! '
            f'video/x-raw,format=I420 ! {encoder_part} ! '
            'video/x-h264,profile=baseline ! h264parse config-interval=-1 ! '
            f'rtspclientsink protocols=tcp latency=0 location='
            f'{self.get_parameter("rtsp_url").value}')
        writer = cv2.VideoWriter(
            pipeline, cv2.CAP_GSTREAMER, 0, fps, size, True)
        if not writer.isOpened():
            self.get_logger().error(
                'Cannot open GStreamer RTSP writer. Check MediaMTX and install '
                'gstreamer1.0-plugins-ugly gstreamer1.0-rtsp.')
            writer.release()
            return None
        self.get_logger().info(f'Publishing H.264 {size[0]}x{size[1]} @ {fps:g} FPS')
        return writer

    def destroy_node(self) -> None:
        self._stop_event.set()
        self._thread.join(timeout=2.0)
        super().destroy_node()


def main(args=None) -> None:
    rclpy.init(args=args)
    node = WebRtcBridgeNode()
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
