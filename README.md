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
    │ TCP response: class_id, confidence_ppm
    ▼
Jetson Nano
  DetectionAdapter → SafetyDecider → UART → Raspberry Pi (TurtleBot)
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
| `jetson/` | 캡처, ROI 생성, crop, TCP client, 안전 판단 + UART 제어 계층 |
| `common/` | KR260에서 가져온 안전 판단 로직(`SafetyJudge`/`HazardLatch`/`UartFrame`), Jetson이 링크 |
| `arty/ps_db/` | DB PL용 TCP server, 전처리, 가속기 제어, 후처리 |
| `arty/ps_eb/` | EB PL용 TCP server, 전처리, 3-IP 6-op 가속기 제어, 후처리 |
| `arty/pl_db/` | DB 96×96 ROI 분류 가속기 HLS 소스와 보고서 |
| `arty/pl_eb/` | EB 96×96 ROI 분류 가속기 HLS 소스, 골든, PS7 프리셋, PS 인계 산출물(bitstream·golden·weights, 소스와 한 트리로 병합됨) |
| `arty/models/` | INT8 양자화 산출물 (db·eb 각각, 교환 불가) |
| `arty/classifier_linux_db/`, `arty/classifier_linux_eb/` | 각 보드용 PetaLinux 프로젝트 (XSA, 커널 드라이버 레시피, rootfs) |
| `arty/deploy/` | SD 카드 굽기·검사 스크립트(`burn_sd.sh`, `inspect_sd.sh`) |
| `arty/tools/` | XSA 하드웨어 설정 검증(`check_xsa.sh`) |
| `shared/` | Jetson–PS 공통 TCP 프로토콜 |
| `docs/contracts/` | 전체 데이터·하드웨어 계약 |

정본 계약: [`docs/contracts/ROI_CLASSIFIER_CONTRACT.md`](docs/contracts/ROI_CLASSIFIER_CONTRACT.md)

주요 문서:

| 문서 | 내용 |
| --- | --- |
| [`docs/ARTY_SD_BOOT_USAGE.md`](docs/ARTY_SD_BOOT_USAGE.md) | DB/EB SD 부팅, Jetson 연결 사용법 |
| [`docs/ARTY_NETWORK_SETUP.md`](docs/ARTY_NETWORK_SETUP.md) | Arty 네트워크 인터페이스 설정 |
| [`docs/JETSON_CONTROL_DESIGN.md`](docs/JETSON_CONTROL_DESIGN.md) | 안전 판단·UART 제어 계층 설계, KR260 대비 변경분 |
| [`docs/DB_EB_VERIFICATION_SUMMARY.md`](docs/DB_EB_VERIFICATION_SUMMARY.md) | DB/EB 검증 결과 비교 |
| [`docs/contracts/PL_HANDOFF_CHECKLIST.md`](docs/contracts/PL_HANDOFF_CHECKLIST.md) | PL 인계 산출물 인수 기준 |

`108`개 파일 규모의 최신 팀 인계 내역(작업 시점 스냅샷, 이후 변경은 반영 안 됨)은
[`CHANGED_FILES.md`](CHANGED_FILES.md) 참고.

## Build and test

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure

cmake -S arty/ps_db -B arty/ps_db/build
cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure

cmake -S arty/ps_eb -B arty/ps_eb/build
cmake --build arty/ps_eb/build -j2
ctest --test-dir arty/ps_eb/build --output-on-failure
```
