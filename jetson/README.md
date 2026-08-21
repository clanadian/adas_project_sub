# Jetson Nano

## Flow

```text
V4L2 YUYV
→ BGR CV_8UC3
→ bbox proposal (최대 10개)
→ 15% margin
→ square expansion + black padding
→ 96×96 INTER_LINEAR
→ BGR→RGB
→ TCP request
→ classification response
```

## Components

| 컴포넌트 | 역할 | 상태 |
| --- | --- | --- |
| `V4L2Capture` | V4L2 MMAP 캡처, YUYV→BGR | 구현·실카메라 확인 |
| `RoiProposer` | bbox 후보 생성·정렬·제한 (TensorRT YOLOv8n) | 구현·실보드 연동 확인 |
| `RoiCropper` | margin, square crop, padding, resize | 구현·테스트 |
| `RoiPreprocessor` | BGR→RGB | 구현·테스트 |
| `TcpRoiClient` | ROI 전송, 결과 수신 | 구현·실보드 연동 확인 |
| `MjpegStreamServer` | 카메라 프레임 + 분류 결과를 MJPEG-over-HTTP로 스트리밍 | 구현 |

`실보드 연동`은 Arty DB 보드에 실제 카메라 프레임을 계속 전송해 확인한
것이다. 자세한 수치는
[`../docs/DB_ARTY_BRINGUP_REPORT.md`](../docs/DB_ARTY_BRINGUP_REPORT.md) §5를
본다. `MjpegStreamServer`를 분류 루프에 안전하게 붙이는 방법(별도 스레드,
넌블로킹 소켓 쓰기)은
[`../docs/JETSON_MJPEG_STREAM_NOTES.md`](../docs/JETSON_MJPEG_STREAM_NOTES.md)에
있다.

## Proposal model

배포용 ONNX와 계약·golden은 `models/proposal/`에 있다.

```text
models/proposal/export/proposal_yolov8n.onnx
models/proposal/export/golden_test_image.jpg
models/proposal/export/golden_sample.json
models/proposal/DEPLOY_SPEC.md
```

TensorRT engine은 Jetson Nano에서 생성하며 저장소에는 커밋하지 않는다.

## TCP input/output

```text
request : frame_id, roi_id, 96×96×3 RGB UINT8
response: frame_id, roi_id, status, class_id, confidence_ppm
```

## Build and test

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure
```

JetPack 4.x의 CMake 3.10에서는 다음 구형 문법을 사용한다.

```bash
cd jetson
mkdir -p build && cd build
cmake ..
cmake --build . -- -j2
ctest --output-on-failure
```

## TensorRT

Jetson Nano의 TensorRT 8.2에서 FP16 engine을 생성한다.

```bash
cd ~/adas_project_sub/jetson/models/proposal/export
/usr/src/tensorrt/bin/trtexec \
  --onnx=proposal_yolov8n.onnx \
  --saveEngine=proposal_yolov8n_fp16.engine \
  --fp16 \
  --workspace=1024 \
  --buildOnly
```

Golden 검사:

```bash
cd ~/adas_project_sub/jetson/build
./proposal_golden_check \
  ../models/proposal/export/proposal_yolov8n_fp16.engine \
  ../models/proposal/export/golden_test_image.jpg
```

Jetson Nano 실측 결과:

```text
TensorRT 8.2.1, FP16, 320x320
median inference latency: 13.6 ms
throughput: 72.1 queries/s
golden proposal count: 8/8
maximum bbox difference: 0.995 px
maximum score difference: 0.00417
```

Run:

```bash
./jetson/build/jetson_roi_client /dev/video0 <PS_IP> 5000 \
    models/proposal/export/proposal_yolov8n_fp16.engine [mjpeg-port]
```

## 제어 (TurtleBot 안전 상태)

분류 결과로 안전 상태(`CLEAR`/`SLOW`/`STOP`)를 판단해 3-byte UART 프레임으로
Raspberry Pi에 보낸다. 송신은 **20 ms 고정 주기의 별도 스레드**이고, 분류
루프는 최신 판단만 갱신한다 — 프레임률이 ROI 개수에 따라 흔들려도 링크가
끊기지 않게 하기 위해서다.

```bash
sudo systemctl disable --now nvgetty        # ttyTHS1의 serial console 해제
ADAS_UART_PORT=/dev/ttyTHS1 \
./jetson/build/jetson_roi_client /dev/video0 <PS_IP> 5000 <engine>
```

`ADAS_UART_PORT`가 없으면 제어 계층을 켜지 않고 기존과 동일하게 동작한다.
설계·배선·캘리브레이션 절차는
[`../docs/JETSON_CONTROL_DESIGN.md`](../docs/JETSON_CONTROL_DESIGN.md).

검출 0건 프레임에서도 판단 watchdog이 죽지 않도록 heartbeat ROI를 주기적으로
보낸다(`ADAS_EMPTY_FRAME_HEARTBEAT`, 기본 켜짐) — 안 하면 빈 화면에서 Stop이
나가고 로봇이 멈춘다. 자세한 이유는 위 설계 문서 §6.

| 컴포넌트 | 역할 |
| --- | --- |
| `DetectionAdapter` | bbox(Jetson) + class(Arty) → 판단용 레코드 |
| `SafetyDecider` | 경로·거리 판단 → 정지 이벤트 래치 |
| `SafetyTransmitter` | 20 ms 송신, 판단 watchdog, STOP 즉시 송신 |
| `UartPort` | termios 래퍼 (테스트용 인터페이스 분리) |

## 측정

파이프라인 구간별 지연과 FPS는 실행 중 자동으로 수집되고 종료 시(Ctrl-C)
요약이 출력된다. 환경변수(`ADAS_MEASURE_QUIET`, `ADAS_MEASURE_CSV`,
`ADAS_TCP_NODELAY` 등)와 판정 절차는
[`../docs/FPS_MEASUREMENT_GUIDE.md`](../docs/FPS_MEASUREMENT_GUIDE.md)를 본다.

```bash
ADAS_MEASURE_QUIET=1 ADAS_MEASURE_CSV=/tmp/jetson.csv \
./jetson/build/jetson_roi_client /dev/video0 <PS_IP> 5000 <engine>
```

`RoiProposer` 모델이 없는 연결 시험에서는 네 번째 인자를 `--full-frame`으로
바꾼다. 다섯 번째 인자 `mjpeg-port`는 선택이다 - 주면
`http://<jetson-ip>:<mjpeg-port>/`로 카메라 화면 위에 ROI 박스와 분류
결과(class + confidence%)를 겹쳐서 볼 수 있다. 안 주면 스트리밍 없이 이전과
동일하게 동작한다 (추가 스레드도 뜨지 않는다).
