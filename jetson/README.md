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
→ TCP request (원본 bbox + ROI image)
→ classification response
→ 결과 overlay
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

proposal objectness 임계값은 `ADAS_PROPOSAL_CONFIDENCE` 환경변수로
조정한다(기본 `0.10`, 허용 범위 `0.0..1.0`). 이 값은 ROI 생성 단계에
적용되므로 올리면 불필요한 TCP 전송과 PL 연산도 함께 줄지만 실제 객체
후보를 놓칠 수 있다. `0.10 / 0.20 / 0.25`를 같은 장면에서 비교해 확정한다.

## TCP input/output

```text
request : frame_id, roi_id, 원본 bbox/objectness/frame 크기, 96×96×3 RGB UINT8
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

## 제어 코드의 위치와 최종 운용

Jetson에는 안전 판단과 UART 송신 코드가 없다. Jetson은 원본 bbox와 ROI를
Arty에 보내고 분류 결과를 화면에 표시한다. Arty PS가 bbox와 분류 결과를
결합해 안전 상태를 판단하고 `/dev/ttyPS1`로 TurtleBot에 전송한다.

`ADAS_SIGN_SLOW_WIDTH`, `ADAS_SLOW_HEIGHT`, `ADAS_STOP_HEIGHT`,
`ADAS_ZONE_*`, `ADAS_MIN_SCORE`, `ADAS_UART_PORT`는 모두
`ps_classifier_server` 실행 환경에 설정한다.

검출 0건 프레임에서도 판단 watchdog이 죽지 않도록 heartbeat ROI를 주기적으로
보낸다(`ADAS_EMPTY_FRAME_HEARTBEAT`, 기본 켜짐) — 안 하면 빈 화면에서 Stop이
나가고 로봇이 멈춘다. 자세한 이유는 위 설계 문서 §6.

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

성공한 분류 결과도 confidence가 기본 `600000`(60%) 미만이면 MJPEG에
bbox와 class label을 그리지 않는다. 이는 표시 전용 필터로 ROI 전송,
PL 분류, Arty 안전 판단은 건드리지 않는다. 실험 시 다음 환경변수로
`0..1000000` 범위에서 조정한다.

```bash
ADAS_OVERLAY_MIN_CONFIDENCE_PPM=600000 \
./jetson/build/jetson_roi_client /dev/video0 <PS_IP> 5000 <engine> 8080
```

proposal와 overlay 임계값은 서로 독립적이다. proposal 임계값은 ROI 생성
여부를 바꾸고, overlay 임계값은 분류가 끝난 결과를 화면에 그릴지만 정한다.
Arty의 제어 판단은 overlay 임계값의 영향을 받지 않는다.
