# Jetson Nano–Arty Z7-20 실시간 ROI 분류 시스템

카메라 영상에서 객체 후보를 찾아 **FPGA에 올린 INT8 CNN 가속기**로 분류하는
엣지 ADAS 파이프라인이다. GPU 보드(Jetson Nano)와 FPGA SoC 보드(Arty Z7-20)를
TCP로 연결해, **탐지는 GPU가, 분류는 FPGA가** 담당하도록 역할을 나눴다.

> 상태: **엔드투엔드 동작 확인 완료** — 실제 카메라 → Jetson → Arty PL 가속기 →
> 분류 결과 회신까지 실보드에서 검증했다 (2026-08-19).

---

## 1. 한눈에 보기

| 항목 | 내용 |
| --- | --- |
| 목적 | 차량·보행자·교통표지 6종 실시간 분류 |
| 처리 분담 | Jetson Nano = 캡처 + 객체 후보 검출 / Arty Z7-20 = ROI 분류 |
| 가속기 | Zynq-7020 PL에 직접 구현한 INT8 CNN (Vitis HLS, 100 MHz) |
| 통신 | TCP persistent connection, 자체 정의 `ROI1` 바이너리 프로토콜 |
| 클래스 | background, car, person, sign_warning, sign_prohibition, sign_mandatory |
| 검증 수준 | 온보드 **bit-exact** 골든 대조 + 실카메라 연속 구동 |

**핵심 실측 수치**

| 지표 | 값 | 측정 방법 |
| --- | --- | --- |
| 객체 후보 검출 (Jetson) | **13.6 ms** / 72.1 qps | TensorRT 8.2.1 FP16, 320×320, 중앙값 |
| ROI 분류 (Arty PL) | **6.6 ms** / ROI | 실보드 가속기 실행 시간 측정 |
| PL 출력 정확성 | **9,216 B bit-exact** (mismatch 0) | 학습 산출 골든 벡터와 온보드 바이트 대조 |
| 파이프라인 안정성 | ROI 요청 **236건 / 오류 0건** | 실카메라 20초 연속 구동 |
| INT8 분류 정확도 | **90.3 ~ 94.3 %** | 검증 세트 (부록 A) |
| PL 타이밍 | 100 MHz 마감, WNS > 0, 실패 엔드포인트 0 | Vivado post-route |

---

## 2. 시스템 구성

```text
 ┌─────────────────────────────┐        ┌──────────────────────────────────┐
 │  Jetson Nano (Ubuntu/L4T)   │        │  Arty Z7-20 (PetaLinux)          │
 │                             │        │                                  │
 │  USB 카메라 640×360 YUYV    │        │   ┌────── PS: Cortex-A9 ──────┐  │
 │        │ V4L2 MMAP          │        │   │ TCP 서버                  │  │
 │        ▼                    │        │   │ INT8 양자화 + zero pad    │  │
 │  YOLOv8n proposal (TensorRT)│        │   │ 커널 드라이버 / DMA 버퍼  │  │
 │        │ bbox 최대 10개     │        │   │ GAP → FC → argmax         │  │
 │        ▼                    │        │   └──────────┬────────────────┘  │
 │  margin·square·96×96 crop   │        │              │ AXI-Lite + DDR    │
 │        │                    │        │   ┌──────────▼────────────────┐  │
 │  TCP client ────────────────┼────────┼──▶│ PL: INT8 CNN 가속기       │  │
 │        ▲                    │  ROI1  │   │ Conv/ReLU/Pool ×3         │  │
 │  분류 결과 수신 ◀───────────┼────────┼───│ → 12×12×64 feature map    │  │
 │        │                    │        │   └───────────────────────────┘  │
 │  MJPEG 오버레이 스트리밍    │        │ PS: bbox+class 안전 판단          │
 │                             │        │     → UART1 → TurtleBot RPi       │
 └─────────────────────────────┘        └──────────────────────────────────┘
```

| 보드 | 역할 | 주요 사양 |
| --- | --- | --- |
| Jetson Nano | 캡처, 객체 후보 검출, ROI 생성, 결과 시각화 | JetPack 4.x, TensorRT 8.2, CUDA |
| Arty Z7-20 | ROI 분류 (가속기 + 후처리) | XC7Z020-1, PetaLinux, DDR3 512 MB |

---

## 3. 데이터 파이프라인

각 단계의 텐서 형상과 자료형이 문서로 고정되어 있고, 양쪽 구현이 이 계약만
지키면 독립적으로 개발·교체할 수 있다. (정본: `docs/contracts/ROI_CLASSIFIER_CONTRACT.md`)

```text
V4L2 YUYV 640×360
  → BGR CV_8UC3
  → YOLOv8n 후보 bbox (최대 10개, class-agnostic)
  → 가로·세로 15% 여백 → 긴 변 기준 정사각 확장 → 프레임 밖은 검정 padding
  → 96×96 INTER_LINEAR resize → BGR→RGB
  ── TCP ──▶ 원본 bbox 28 B + 96×96×3 RGB UINT8 (27,648 B)

  → q = clamp(round(pixel × 127 / 255), 0, 127)        ← PS, symmetric INT8
  → 1픽셀 zero border
  → 98×98×3 signed INT8 NHWC (28,812 B)                ← DDR, DMA coherent
  → PL 가속기 실행
  → 12×12×64 signed INT8 NHWC (9,216 B)                ← feature map (logits 아님)
  → GAP(합) → FC 64×6 → argmax                         ← PS
  ── TCP ──▶ class_id, confidence_ppm
```

| 구간 | 형식 | 크기 |
| --- | --- | ---: |
| Jetson → PS | bbox + 96×96×3 RGB UINT8 NHWC | 28 + 27,648 B |
| PS → PL | 98×98×3 signed INT8 NHWC (zero border) | 28,812 B |
| PL → PS | 12×12×64 signed INT8 NHWC | 9,216 B |
| PS → Jetson | status, class_id, confidence_ppm | 12 B |

---

## 4. 구성 요소

### 4.1 Jetson Nano — 캡처와 객체 후보 검출

| 컴포넌트 | 역할 |
| --- | --- |
| `V4L2Capture` | V4L2 MMAP 캡처, YUYV→BGR 변환 |
| `RoiProposer` / `TensorRtProposalEngine` | TensorRT YOLOv8n 후보 bbox 생성·정렬·개수 제한 |
| `RoiCropper` | 여백 추가, 정사각 확장, padding, 96×96 resize |
| `RoiPreprocessor` | BGR→RGB |
| `TcpRoiClient` | ROI 전송·결과 수신 (persistent connection) |
| `MjpegStreamServer` | 카메라 화면 위에 ROI 박스와 분류 결과를 겹쳐 HTTP로 스트리밍 |

**후보 검출 모델** — YOLOv8n을 `nc=1`(class-agnostic "object")로 학습해
클래스 판단은 하지 않고 **후보 위치만** 만든다. 클래스 판단은 전부 FPGA 쪽
분류기가 담당한다.

```text
입력 : images [1,3,320,320] float32 NCHW RGB [0,1]  (letterbox pad = 회색 114)
출력 : output0 [1,5,2100]  ch0-3 = box(cx,cy,w,h) 320-pixel space (디코딩 완료)
                            ch4   = objectness (sigmoid 적용 완료)
후처리: conf 0.10 → NMS iou 0.45 → 상위 10개 → 좌상단 (x,y,w,h)
```

배포 명세(`jetson/models/proposal/DEPLOY_SPEC.md`)에 전처리·좌표 변환·threshold를
실측값으로 고정하고, 순수 numpy 레퍼런스 구현(`decode_reference.py`)을 C++ 포팅
기준으로 함께 제공한다.

| 검출 성능 | 전체 val (8,827장) | 데모 서브셋 (4,249장) |
| --- | ---: | ---: |
| recall@top10 | 53.2% | **96.3%** |
| person | 92.4% | 98.7% |
| 표지판 3종 | 51.5 ~ 68.6% | 100% |

전체 수치가 낮은 것은 차량이 10대 넘게 나오는 복잡한 장면(top-10 제한)과 아주
작은 원거리 표지판 때문이며, 실제 데모 조건에 가까운 서브셋에서는 96.3%다.

### 4.2 통신 — `ROI1` 프로토콜

정본은 `shared/include/roi_protocol.h` 하나이고, Jetson(C++)과 PS(C)가 같은
헤더를 공유한다. 모든 다중 바이트 정수는 network byte order다.

**공통 헤더 20 B**

| offset | size | field |
| ---: | ---: | --- |
| 0 | 4 | magic `"ROI1"` |
| 4 | 2 | version |
| 6 | 2 | message type (request 1 / response 2) |
| 8 | 4 | frame ID |
| 12 | 4 | ROI ID |
| 16 | 4 | payload size |

**응답 payload 12 B** — `status`, `class_id`, `confidence_ppm(0..1,000,000)`

`status`는 `OK / INVALID_HEADER / INVALID_PAYLOAD / ACCELERATOR_ERROR /
POSTPROCESS_ERROR`를 구분한다. TCP가 메시지 경계를 보장하지 않으므로 송수신
양쪽 모두 지정 길이를 다 채울 때까지 반복하도록 구현했고, 헤더 검증·부분 수신·
연결 종료를 단위 테스트로 덮었다.

### 4.3 Arty PS — 전처리, 가속기 제어, 후처리

| 컴포넌트 | 역할 |
| --- | --- |
| `tcp_roi_server` | persistent TCP 서버 |
| `roi_preprocessor` | UINT8→INT8 양자화, 1픽셀 zero padding |
| `adas_classifier_drv` (커널 모듈) | AXI-Lite 매핑과 `dma_alloc_coherent()` DDR 버퍼 소유 |
| `classifier_device` | `/dev/adas_classifier` ioctl/mmap 래퍼 |
| `classifier_accelerator` | 버퍼·파라미터 설정, 실행 시작/완료 대기 |
| `classifier_model` | 가중치·bias 바이너리 로드와 크기 검사 |
| GAP / FC / argmax | PL feature map 후처리 → class_id |
| `classifier_confidence` | logits × logits_scale → softmax → `confidence_ppm` |
| `dummy_roi_service` | PL 없이 고정 결과를 돌려주는 시험용 서버 |
| `ps_safety_bridge` | bbox와 분류 결과를 프레임별로 결합해 안전 상태 판단 |
| `SafetyTransmitter` / `UartPort` | UART1으로 20 ms 주기 안전 프레임 송신 |

입력·가중치·출력 버퍼는 커널 드라이버가 `dma_alloc_coherent()`로 잡은 DDR에
있고, 사용자 공간 프로그램은 그 영역을 `mmap()`으로 직접 다룬다. PL의 AXI
master가 같은 물리 메모리를 읽고 쓰므로 프레임마다 복사가 한 번도 일어나지 않는다.

초기 브링업용 `/dev/mem` MMIO 경로도 남겨 두어, 드라이버 없이도 레지스터 단위로
가속기를 두드려 볼 수 있다.

### 4.4 Arty PL — INT8 CNN 가속기

Vitis HLS로 작성한 고정 구조 가속기다. 학습 모델의 conv 3단만 PL이 맡고,
파라미터 수가 많고 연산량이 적은 FC 단은 PS가 맡는 분담이다.

```text
98×98×3  pre-padded INT8 입력
  → Conv 3→16  3×3 s1 + ReLU + MaxPool 2×2   → 48×48×16
  → Conv 16→32 3×3 s1 SAME + ReLU + MaxPool  → 24×24×32
  → Conv 32→64 3×3 s1 SAME + ReLU + MaxPool  → 12×12×64  (INT8 NHWC)
```

- 타깃: `xc7z020-clg400-1`, Vitis HLS / Vivado 2024.2, **100 MHz**
- 산술: activation·weight = INT8, bias·accumulator = INT32,
  requant = `(acc × multiplier) >> shift` 후 clamp
- 인터페이스: 파라미터·제어는 AXI-Lite, 데이터는 AXI master ↔ DDR
- 자원: **DSP 약 90% (198~199 / 220)** 사용, post-route 타이밍 마감

가중치 레이아웃은 PL이 읽는 순서 그대로 export 단계에서 미리 변환해 둔다.
(Conv0 = OIHW, Conv1·Conv2 = WPACK `[oc][ky][kx][ic]`) — PS는 파일을 읽어
DMA 버퍼에 올리기만 하고 런타임 transpose를 하지 않는다.

### 4.5 INT8 양자화 모델

- 학습 입력은 `pixel/255.0`, mean/std 정규화 없음 → 추론 시 PS의 양자화식과 1:1 대응
- symmetric, zero-point 0, per-tensor 양자화
- 검정 픽셀 `[0,0,0]`이 INT8 0에 대응하므로 crop padding과 PL zero border가 의미상 일치
- GAP의 `1/144`는 **PS에서 나누지 않고** FC scale에 흡수 — 두 곳에서 중복 적용되는
  사고를 계약과 manifest 양쪽에 명시해 막았다
- `export/manifest.json`이 레이어별 scale, requant multiplier/shift, 클래스 순서의 정본
- 레이어별 골든 텐서(`golden_conv*_out.npy`, `golden_conv*_pool.npy`)를 함께 배포해
  PL·PS 어느 단계에서 어긋났는지 바로 짚을 수 있다

---

## 5. 검증

이 프로젝트의 검증은 "돌아간다"가 아니라 **"학습 결과와 바이트 단위로 같다"**를
기준으로 삼는다.

| 단계 | 방법 | 결과 |
| --- | --- | --- |
| 하드웨어 설정 | `check_xsa.sh` — XSA의 보드 필수 파라미터 10개 자동 대조 | 10/10 통과 |
| 부팅 / 드라이버 | UART 콘솔, `dmesg` | FSBL→로그인 정상, DMA 버퍼 probe 성공 |
| **PL 가속기 (실가중치)** | `ps_db_golden_test` — 실제 PL 출력과 학습 골든 벡터 대조 | **9,216 B bit-exact, mismatch 0**, 가속기 6,570 µs |
| **PS 전체 경로** | ROI1 클라이언트로 골든 이미지 재전송 | `status=0, class_id=2 (person)` — 기대값 일치 |
| **Jetson 실카메라 연동** | `jetson_roi_client` 20초 연속 구동 | 요청 236건 / `status=0` 236건, 오류 0 |
| 후보 검출 모델 | golden 이미지 PyTorch ↔ TensorRT 대조 | proposal 8/8 일치, bbox 최대 오차 0.995 px |
| PL 논리 | HLS csim / cosim + Vivado post-route | 비트일치, 타이밍 마감 |

`ps_db_golden_test`는 HLS 시뮬레이션이 아니라 **실리콘에서** 돌린 온보드
테스트다 — 실제 학습 가중치를 `/dev/adas_classifier`로 PL에 넣고, 나온 9,216
바이트를 학습팀 골든 `.npy`와 바이트 단위로 대조한다. 실패하면 실제 출력과
비교 리포트를 파일로 남긴다.

**소프트웨어 단위 테스트** — Jetson·PS 양쪽 모두 CTest로 묶여 있다.
프로토콜 인코딩/디코딩, 부분 수신, ROI crop 경계 조건, 양자화·padding,
레지스터 접근, 후처리 argmax를 각각 덮는다.

```bash
ctest --test-dir jetson/build      --output-on-failure
ctest --test-dir arty/ps_db/build  --output-on-failure
```

---

## 6. 엔지니어링 기록

시스템 자체만큼 **문제를 어떻게 잡았는지**가 이 프로젝트의 내용이다.

### 6.1 보드가 아예 부팅하지 않던 문제

인계받은 하드웨어 산출물(XSA)로 만든 이미지가 시리얼 콘솔에 아무것도 찍지 않았다.
증상 하나로 원인 세 개를 순서대로 갈라냈다.

| # | 원인 | 근거 |
| --- | --- | --- |
| 1 | XSA에 PS UART가 아예 빠져 있었음 (`PCW_UART_PERIPHERAL_VALID=0`) | FSBL이 출력할 장치가 물리적으로 없음 → SD·점퍼와 무관하게 무음 |
| 2 | PS 입력 클럭이 33.33 MHz로 설정 (실제 크리스털 50 MHz) | 분주비 40 적용 시 CPU 2,000 MHz — 정격 667 MHz의 3배, FSBL이 PLL 단계에서 정지 |
| 3 | DDR 물리 구성이 다른 보드 값 (32비트 버스 / 8비트 칩) | 실제 보드는 16비트 버스 / 16비트 칩 — 총 용량은 같아도 트레이닝 실패 |

### 6.2 재발 방지

- **`arty/tools/check_xsa.sh`** — XSA를 열어 크리스털·DDR 5종·UART·SD MIO 등
  보드 필수 파라미터 10개를 자동 대조한다. 하나라도 어긋나면 실패로 끝난다.
  같은 사고를 사람 눈이 아니라 스크립트가 막는다.
- **`docs/contracts/PL_HANDOFF_CHECKLIST.md`** — 하드웨어 인계본이 만족해야 할
  기준을 항목별로 고정하고, 각 항목에 실제로 발생한 사고를 근거로 남겼다.
- **SD 카드 도구** — 카드 UUID로 대상을 찾아가 반대쪽 카드를 덮어쓰는 사고를 막는다.

### 6.3 검증 자체를 검증하기

PL 최적화 과정에서 **골든 벡터가 포화(saturation)를 한 번도 밟지 않는다**는 것을
발견했다. INT8 256개 값 중 6%만 쓰고 ±127/−128은 0.0%였는데, 그 상태에서
"csim 비트일치"를 근거로 최적화를 채택하고 있었다.

증명 방법: 엔진의 clamp를 127→126으로 **일부러 망가뜨렸더니** 새로 넣은 포화
형상만 6,780/32,768 불일치를 냈고 나머지 6개 형상은 전부 통과했다. 즉 기존
테스트는 그 버그를 볼 수 없었다.

여기서 굳어진 규칙:

- **`0건`을 통과로 읽지 않는다** — 정규식이 안 맞아 0을 세고 있던 경우가 실제로 세 번 있었다.
- **부분 문자열 검사는 변이(mutation)를 통과시킨다** — 집합·앵커·경계로 검사한다.
- **검사를 고치면 그 검사에도 변이를 돌린다** — 리팩터링은 게이트를 눈멀게 한다.
- 새 검사는 반드시 **실제 결함 상태에서 FAIL 하는 것을 먼저 확인**하고 채택한다.

현재 빌드 게이트는 110여 건이며 2회 연속 동일 결과를 요구한다.

---

## 7. 빌드와 실행

### Jetson Nano

```bash
cmake -S jetson -B jetson/build
cmake --build jetson/build -j2
ctest --test-dir jetson/build --output-on-failure

# TensorRT engine 생성 (보드 현지에서)
/usr/src/tensorrt/bin/trtexec \
  --onnx=proposal_yolov8n.onnx --saveEngine=proposal_yolov8n_fp16.engine \
  --fp16 --workspace=1024 --buildOnly

# 실행 (마지막 인자는 선택 — 주면 MJPEG 오버레이 뷰어가 뜬다)
./jetson/build/jetson_roi_client /dev/video0 <ARTY_IP> 5000 \
    models/proposal/export/proposal_yolov8n_fp16.engine 8080
```

카메라만 있고 모델이 없는 연결 시험에서는 engine 인자 자리에 `--full-frame`을 준다.

### Arty Z7-20 (PS)

```bash
cmake -S arty/ps_db -B arty/ps_db/build
cmake --build arty/ps_db/build -j2
ctest --test-dir arty/ps_db/build --output-on-failure

# 보드에서: 드라이버 로드 후 서버 기동
insmod adas_classifier_drv.ko          # /dev/adas_classifier 생성
./ps_classifier_server "*" 5000 <model-export-dir> 6 1 <requant params...> <logits-scale>

# PL 없이 프로토콜만 시험
./ps_dummy_server 0.0.0.0 5000
```

### 온보드 골든 테스트

```bash
./ps_db_golden_test /opt/adas/model /dev/adas_classifier /opt/adas/golden_report 2000
# 성공 시: "9216 bytes bit-exact"
```

---

## 8. 저장소 구조

```text
jetson/                 캡처, 후보 검출, ROI crop, TCP client, MJPEG 스트리밍
  models/proposal/      YOLOv8n ONNX, 배포 명세, golden, numpy 레퍼런스 디코더
arty/
  ps_*/                 PS 애플리케이션 (TCP·전처리·가속기 제어·후처리) + 커널 드라이버
  pl_*/                 PL 가속기 HLS 소스, 테스트벤치, 골든 벡터, 합성·구현 리포트
  models/               INT8 양자화 산출물 (가중치·bias·골든·manifest)
  classifier_linux_*/   PetaLinux 프로젝트 (device tree, 드라이버 레시피)
  tools/check_xsa.sh    하드웨어 산출물 자동 검사
shared/include/         Jetson–PS 공통 TCP 프로토콜 (정본)
common/                 KR260에서 승계한 안전 판단 계층 (판단·래치·UART frame)
docs/
  contracts/            데이터·하드웨어 계약 (정본)
  bringup/                    SD 부팅·네트워크·보드 브링업 기록
  FPS_MEASUREMENT_GUIDE.md    FPS·지연 측정 계측과 절차
  PS_PIPELINE_STUDY.md        개념 ↔ 코드 대응 설명
  contracts/ROI_CLASSIFIER_CONTRACT.md   PL↔PS 연동 계약 (db/eb 공통 + 변종별 정본)
  contracts/PL_HANDOFF_CHECKLIST.md      PL 인계본 접수 기준
```

`common/`은 이전 KR260 기반 시스템에서 가져온 안전 판단 계층
(SafetyJudge / HazardLatch / UART frame)이다. 최종 DB 구성에서는 Arty PS가
이를 링크해 판단과 UART 송신을 수행한다. Jetson은 안전 판단 계층을 링크하지
않는다.

---

## 9. 남은 과제

- ~~**엔드투엔드 FPS 측정**~~ — 2026-08-21 완료. 21.6~25.7 FPS,
  27~39 ROI/s, 총 197,639 요청 오류 0. 병목은 가속기가 아니라 Jetson
  proposal 28.2 ms 다. 수치·재현 절차는
  [`E2E_MEASUREMENT_REPORT.md`](reports/E2E_MEASUREMENT_REPORT.md), 계측 정의는
  [`FPS_MEASUREMENT_GUIDE.md`](FPS_MEASUREMENT_GUIDE.md).
- ~~**rootfs 정식화**~~ — 2026-08-20 완료. DB는 서버 바이너리(`ps-classifier-server`
  레시피)가 rootfs에 편입됐고, initramfs에서 영속 ext4(`root=/dev/mmcblk0p2`)로
  전환됐다. 고정 IP도 recipe로 박혀 재부팅해도 유지된다.
- **PS 응답 지연 수정** — DB는 반영·유닛 테스트 통과 완료(2026-08-20). 응답을
  헤더+본문 한 번의 write로 합쳐 ROI 왕복 51.6ms → 9.7ms로 줄었다. 실보드
  재배포 후 tcpdump 검증은 아직. 자세한 원인·수치는
  [`PS_TCP_RESPONSE_FIX.md`](PS_TCP_RESPONSE_FIX.md).
- **제어 계층 실보드 검증** — 판단·래치·UART 송신은 구현하고 단위 테스트까지
  마쳤다. 남은 것은 배선 확인, zone·거리 임계값 캘리브레이션, 실주행이다.
  최종 제어 결선은 [`../arty/ps_db/README.md`](../arty/ps_db/README.md)에 정리했다.
- ~~**PL 구현 단일화**~~ — 2026-08-20 결정 완료. 부록 A 참조.

---

## 부록 A. PL 내부 구현 후보 (내부 사항, 결정 완료)

외부 계약(입출력 형상·프로토콜·PS 구조)은 완전히 동일한 두 구현(DB=구현 A,
EB=구현 B)을 병행 개발해 비교했다. **2026-08-20, DB(구현 A)를 배포 보드로
확정했다 — EB는 실험 트랙으로도 더 이상 쓰지 않는다.** FPS·정확도·성숙도
세 축 모두 이 시점에 DB가 우위이거나 최소 안 밀렸고, EB의 PL 속도 우위는
당시 파이프라인 구성(Jetson 캡처+YOLO propose가 먼저 병목)에서 실이득으로
이어지지 않았다. 아래 표는 그 비교 시점의 기록이다.

| | 구현 A | 구현 B |
| --- | --- | --- |
| 구조 | 단일 IP, conv+pool 융합 | conv/conv0/maxpool 3-IP, 6-op 시퀀스 |
| 활성화 | ReLU (clamp 하한 0에 접힘) | LeakyReLU 13/128, requant 이전 적용 |
| requant | `scaled >> shift`, 반올림 없음 | 반올림 항 포함 |
| ROI 1건 | 6.57 ms (**실보드 실측**) | 7.78 ms (cosim, 12.86 FPS @10 ROI) |
| LUT | 14,410 (27.1%) | 42,100 (79.1%) |
| BRAM | 129 tile (92.1%) | 50 tile (35.7%) |
| DSP | 199 (90.5%) | 198 (90.0%) |
| WNS | +0.356 ns | +0.197 ns |
| 분류 정확도 | 90.3% (INT8, n=300) | 94.3% (leaky 파인튜닝 후 test set) |
| 검증 도달점 | 실보드 bit-exact + 실카메라 연동 | cosim 비트일치 + 구현 마감 |

두 구현은 산술이 다르므로 **가중치를 서로 바꿔 넣을 수 없다.** 잘못 넣어도
오류 없이 동작하고 분류 결과만 조용히 틀리기 때문에, 배포 시점에 어느 쪽인지
확인하는 절차를 문서와 디렉터리 이름 양쪽에 박아 두었다.

구현 B의 성능 개선 과정(출력 쓰기 구조 변경으로 −13%, 중간 버퍼 제거로 추가
−2% 및 면적 회수)은 Git 태그 `eb-comparison-final`의
`arty/pl_eb/doc/2026-08-18_arty96-perf-campaign.md`에
채택·기각 근거와 함께 전부 기록되어 있다.
