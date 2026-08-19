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
| `V4L2Capture` | V4L2 MMAP 캡처, YUYV→BGR | 구현 |
| `RoiProposer` | bbox 후보 생성·정렬·제한 | 모델 대기 |
| `RoiCropper` | margin, square crop, padding, resize | 구현·테스트 |
| `RoiPreprocessor` | BGR→RGB | 구현·테스트 |
| `TcpRoiClient` | ROI 전송, 결과 수신 | 구현·왕복 테스트 |

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
./jetson/build/jetson_roi_client /dev/video0 <PS_IP> 5000
```

`RoiProposer` 모델이 없는 연결 시험에서는 마지막에 `--full-frame`을 추가한다.
