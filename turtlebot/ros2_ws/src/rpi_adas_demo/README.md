> ⚠️ **이 문서는 옛 아키텍처(RPi 자체 YOLO + WebRTC + LB 데드맨)를 설명한다.**
> 현재 데모는 Jetson이 객체 후보를 만들고 Arty PS가 안전 상태를 판단하며,
> RPi는 UART로 상태를 받는다.
> 아래의 `ros2 launch rpi_adas_demo demo.launch.py` 를 그대로 쓰면
> `yolo_safety_node` 가 `/adas/safety_state` 에 두 번째 발행자로 붙어 충돌한다.
> **현재 절차는 `~/README.md` 를 볼 것.**

# Raspberry Pi 4 ROS 2 ADAS 실행 가이드

USB 카메라의 최신 프레임을 ONNX Runtime YOLOv3-tiny로 처리하고, bounding box,
클래스, confidence와 안전 상태가 그려진 영상을 MediaMTX WebRTC로 전송한다.
HTTP MJPEG와 OpenCV Darknet DNN은 사용하지 않는다.

Xbox USB 동글은 Raspberry Pi에 직접 연결하며 `joy_node`가
`Xbox One S Controller`를 직접 연다. 별도 Xbox UDP 수신 노드는 필요 없다.

조종할 때는 안전 스위치인 `LB`를 누른 상태에서 왼쪽 스틱 또는 ABXY를 사용한다.

- 왼쪽 스틱: 직진/후진/좌회전/우회전 (`stick_deadzone=0.12`와 주축 보정 적용)
- `Y`: 직진, `A`: 후진, `X`: 좌회전, `B`: 우회전
- `Back/View`: 소프트웨어 비상정지 켜기/해제

스틱을 앞뒤로 밀 때 생기는 작은 좌우 흔들림은 회전 명령에서 제거된다. 더 강한
보정이 필요하면 `cardinal_snap_ratio`를 0.45보다 높이고, 약하게 하려면 낮춘다.

## 모델

기본 실행 모델은 calibration된 INT8 QDQ 모델이다.

```text
/home/ubuntu/ros2_ws/src/rpi_adas_demo/models/
├── adas.names
├── yolov3-tiny-adas-5class-512x288-int8-qdq.onnx
└── yolov3-tiny-adas-5class-512x288-fp32.onnx
```

INT8 입력은 `float32 NCHW [1,3,288,512]`, RGB, `/255.0`이고 출력은
`[1,30,9,16]`, `[1,30,18,32]`이다.

## 최초 한 번: 런타임 설치

```bash
sudo apt update
sudo apt install -y python3-pip gstreamer1.0-tools \
  gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
  gstreamer1.0-plugins-bad gstreamer1.0-plugins-ugly \
  gstreamer1.0-rtsp

python3 -m pip install --user --upgrade 'numpy<2' onnxruntime
```

MediaMTX Releases에서 Linux ARM64 standalone archive를 받은 뒤 실행 파일을
`/home/ubuntu/mediamtx`에 둔다.

```bash
chmod +x /home/ubuntu/mediamtx
python3 -c "import onnxruntime; print(onnxruntime.get_available_providers())"
gst-inspect-1.0 x264enc
gst-inspect-1.0 rtspclientsink
/home/ubuntu/mediamtx --version
```

## 빌드

```bash
cd /home/ubuntu/ros2_ws
source /opt/ros/humble/setup.bash
colcon build --packages-select rpi_adas_demo
source install/setup.bash
```

## 최종 실행

카메라 번호를 먼저 확인한다.

```bash
v4l2-ctl --list-devices
```

그다음 하나의 터미널에서 전체 데모를 시작한다.

```bash
cd /home/ubuntu/ros2_ws
source /opt/ros/humble/setup.bash
source install/setup.bash
ros2 launch rpi_adas_demo demo.launch.py camera_index:=0
```

브라우저:

```text
http://10.10.16.200:8889/adas
```

기본값은 INT8 추론 512×288, annotated stream 1280×720, JPEG quality 80,
H.264 3000kbps이다. 추론 입력 크기는 유지하므로 검출 모델 연산량은 변하지 않는다.

```bash
ros2 launch rpi_adas_demo demo.launch.py \
  camera_index:=0
```

WebRTC 없이 안전 판단만 점검:

```bash
ros2 launch rpi_adas_demo demo.launch.py camera_index:=0 enable_webrtc:=false
```

## FP32 비교

동일 장면에서 INT8와 FPS/검출 결과를 비교할 때만 FP32 override를 사용한다.

```bash
ros2 launch rpi_adas_demo demo.launch.py camera_index:=0 \
  model_path:=/home/ubuntu/ros2_ws/src/rpi_adas_demo/models/yolov3-tiny-adas-5class-512x288-fp32.onnx
```

노드 로그의 `inference=...ms`를 비교한다. INT8 검출 누락이나 box 차이가 허용
범위를 넘으면 FP32를 사용한다.

## 상태 확인

```bash
ros2 topic hz /adas/debug_image/compressed
ros2 topic echo /adas/safety_state
ros2 topic echo /cmd_vel
```

영상 경로는 모두 depth=1/latest-frame-only이며 H.264 baseline, B-frame 0,
zero-latency 설정을 사용한다. 그래도 100ms 미만 지연은 카메라, 단일 추론 시간,
인코더와 네트워크 성능에 따라 달라지며 보장값은 아니다.

실차 시험은 먼저 바퀴를 든 상태에서 STOP과 `/cmd_vel` zero 출력을 확인한다.
이 Python/ROS 2 경로는 hard real-time 안전 제어를 대체하지 않는다.
