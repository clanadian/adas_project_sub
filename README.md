# Jetson–Arty ROI Classifier

## Architecture

```text
USB camera
    │
    ▼
Jetson Nano
  V4L2 capture → bbox proposal → square crop → 96×96 RGB UINT8
    │
    │ TCP request
    ▼
Arty Z7-20 PS (Linux)
  receive → UINT8→INT8 → 1-pixel zero padding → 98×98×3
    │
    │ DDR + AXI
    ▼
Arty Z7-20 PL
  Conv/ReLU/Pool ×3 → 12×12×64 INT8
    │
    ▼
Arty Z7-20 PS
  GAP → FC → argmax
    │
    │ TCP response: class_id, confidence
    ▼
Jetson Nano
```

## Interfaces

| 구간 | 형식 |
| --- | --- |
| Jetson → PS | `96×96×3`, RGB, UINT8, NHWC |
| PS → PL | `98×98×3`, signed INT8, NHWC, zero border |
| PL → PS | `12×12×64`, signed INT8, NHWC |
| PS → Jetson | `class_id`, `confidence_ppm` |

TCP는 persistent connection에서 ROI 한 건씩 요청·응답한다. Multi-byte 정수는
network byte order를 사용한다.

## Repository

| 경로 | 내용 |
| --- | --- |
| `jetson/` | 캡처, ROI 생성, crop, TCP client |
| `arty/ps_db/` | DB PL용 TCP server, 전처리, 가속기 제어, 후처리 |
| `arty/ps_eb/` | EB PL용 PS 구현 작업 트리 |
| `arty/pl/` | 96×96 ROI 분류 가속기 HLS 소스와 보고서 |
| `shared/` | Jetson–PS 공통 TCP 프로토콜 |
| `docs/contracts/` | 전체 데이터·하드웨어 계약 |

정본 계약: [`docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](docs/contracts/ROI_CLASSIFIER_CONTRACT.md)

## Build and test

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure

cmake -S arty/ps_db -B arty/ps_db/build
cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure
```
