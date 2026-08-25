# ROI 분류 파이프라인 계약

이 문서는 Jetson Nano, Arty Z7-20 PS, PL ROI 분류 가속기 사이의
현재 계약을 정의한다.

PL 동작의 정본은 아래 DB 구현과 이에 맞춘 INT8 가중치다.

| 변종 | PL 정본 | INT8 가중치 |
| --- | --- | --- |
| DB | `arty/pl_db/hls/HW/classifier_engine.*` | `arty/models/roi_classifier_int8_db/` |

종료된 비교용 EB 구현은 Git 태그 `eb-comparison-final`에 보존한다.

## 1. 전체 데이터 흐름

```text
Jetson: V4L2 YUYV → BGR CV_8UC3
        → ROI crop/padding/resize
        → 96×96 RGB UINT8 NHWC
        → persistent TCP client

Arty PS Linux: TCP server
         → 원본 bbox 메타데이터 + RGB UINT8 수신
         → signed INT8 양자화
         → 1픽셀 INT8 0 테두리 추가
         → 98×98×3 INT8 NHWC
         → PL 실행
         → GAP/FC/argmax
         ├→ 분류 결과 TCP 회신
         └→ bbox+class 안전 판단 → UART1 → TurtleBot Raspberry Pi
```

## 2. Jetson ROI 계약

- 입력: V4L2Capture가 반환한 BGR `CV_8UC3`
- bbox 가로·세로에 각각 15% 여백 추가
- 긴 변을 기준으로 정사각형 확장
- 프레임 밖은 검은색으로 padding
- OpenCV `INTER_LINEAR`로 `96×96` resize
- crop 후 BGR→RGB 변환
- TCP 전송 픽셀: `96×96×3`, RGB, UINT8, NHWC

## 3. Jetson–PS 통신 계약

- transport: TCP
- Jetson Nano Linux: TCP client
- Arty Z7-20 PS Linux: POSIX socket TCP server
- ROI마다 연결하지 않고 실행 중 하나의 TCP 연결을 유지한다.
- protocol version: `2`
- 요청 payload: bbox 블록 28 bytes + `96×96×3 = 27,648` bytes RGB UINT8 NHWC
- 요청에는 최소한 `frame_id`, `roi_id`, payload length를 포함한다.
- PS 응답에는 최소한 `frame_id`, `roi_id`, `class_id`, score를 포함한다.
- TCP는 메시지 경계를 보장하지 않으므로, 송신·수신 코드는 지정된
  header/payload 길이를 모두 처리할 때까지 반복한다.
- multi-byte 정수는 network byte order를 사용한다.
- 초기 구현은 ROI 요청 하나를 처리하고 결과를 회신한 뒤 다음
  ROI를 보내는 순차 방식으로 한다.

공통 header는 20 bytes이며 다음 field를 순서대로 배치한다.

| offset | size | field |
|---:|---:|---|
| 0 | 4 | magic: ASCII `ROI1` |
| 4 | 2 | version: `2` |
| 6 | 2 | message type: request `1`, response `2` |
| 8 | 4 | frame ID |
| 12 | 4 | ROI ID |
| 16 | 4 | payload size |

Request payload는 28-byte bbox 블록 뒤에 27,648-byte RGB UINT8 NHWC
이미지가 이어진다. bbox는 crop 좌표가 아니라 원본 프레임 좌표다.

| bbox offset | size | field |
|---:|---:|---|
| 0 | 4 | x (`float32`) |
| 4 | 4 | y (`float32`) |
| 8 | 4 | width (`float32`) |
| 12 | 4 | height (`float32`) |
| 16 | 4 | objectness (`float32`) |
| 20 | 4 | frame width (`uint32`) |
| 24 | 4 | frame height (`uint32`) |

`float32`도 IEEE-754 비트 패턴을 `uint32`로 옮긴 뒤 network byte order로
직렬화한다.

Response payload는 다음 12 bytes이다.

| offset | size | field |
|---:|---:|---|
| 0 | 4 | status |
| 4 | 4 | class ID |
| 8 | 4 | confidence ppm (`0..1,000,000`) |

Status는 `OK=0`, `INVALID_HEADER=1`, `INVALID_PAYLOAD=2`,
`ACCELERATOR_ERROR=3`, `POSTPROCESS_ERROR=4`를 사용한다.
오류 응답의 class ID는 `UINT32_MAX`, confidence는 0으로 보낸다.
정확한 상수·offset·직렬화 규칙의 정본은 `shared/include/roi_protocol.h`이다.

## 4. 입력 양자화·padding 계약

학습 입력은 `pixel_u8 / 255.0`이며 mean/std 정규화는 사용하지 않는다.
실제 추론 입력은 Arty PS에서 다음과 같이 변환한다.

```text
q = clamp(round(pixel_u8 × 127 / 255), 0, 127)
```

- activation/weight: signed INT8
- bias/accumulator: INT32
- symmetric, zero-point 0
- per-tensor 양자화
- 검은색 `[0,0,0]`은 INT8 0
- PS가 96×96 ROI 외곽에 INT8 0 테두리를 추가해 PL에
  `98×98×3` NHWC로 전달

## 5. PL 고정 구조

```text
98×98×3 pre-padded input
→ Conv 3→16, 3×3, stride 1 + ReLU + MaxPool 2×2
→ 48×48×16
→ Conv 16→32, 3×3, stride 1, SAME + ReLU + MaxPool 2×2
→ 24×24×32
→ Conv 32→64, 3×3, stride 1, SAME + ReLU + MaxPool 2×2
→ 12×12×64 INT8 NHWC
```

- 세 Conv 모두 일반 ReLU를 적용한다.
- PL 출력은 logits이 아니라 `12×12×64` feature map이다.
- requant: `acc × multiplier`에 arithmetic right shift를 적용한 후
  ReLU 범위 `0..127`로 clamp한다.
- 별도의 반올림 offset은 적용하지 않는다.

## 6. 가중치 레이아웃

| 레이어 | 입력 레이아웃 | 형식 |
|---|---|---|
| Conv0 | `[16][3][3][3]` | OIHW INT8 |
| Conv1 | `[32][3][3][16]` | WPACK `[oc][ky][kx][ic]` INT8 |
| Conv2 | `[64][3][3][32]` | WPACK `[oc][ky][kx][ic]` INT8 |

각 레이어는 INT32 bias와 `multiplier: INT32`, `shift: UINT8` requant
파라미터를 사용한다.

## 7. 클래스 계약

| index | class |
|---:|---|
| 0 | background |
| 1 | car |
| 2 | person |
| 3 | sign_warning |
| 4 | sign_prohibition |
| 5 | sign_mandatory |

## 8. PS 후처리

PL이 반환한 `12×12×64` feature map에 다음을 수행한다.

```text
GAP → FC 64×6 → logits → argmax
```

GAP은 144개 값을 합산하고 런타임 `gap-div=1`을 사용한다. `1/144`는 모델
export의 FC scale에 흡수돼 있으므로 PS에서 다시 나누지 않는다.

분류 결과는 Jetson 시각화에 회신하는 동시에 PS 안전 판단에도 사용한다.
현재 클래스별 제어 정책은 다음과 같다.

- car/person: bbox 위치·높이에 따라 `Clear/Slow/Stop`; Stop은 HazardLatch 적용
- sign 3종: bbox 폭이 `sign_slow_width` 이상이면 `Slow`, 그 외 `Clear`
- sign 에는 **위치 조건을 적용하지 않는다** — `zone_x`(경로 좌우)도
  `zone_y_min`(지면)도 보지 않는다. 2026-08-24 변경이며 화면 가장자리의
  표지판도 정면의 것과 같게 판단한다 (`common/src/SafetyJudge.cpp`)
- sign은 개별 정지표지판을 구분할 수 없으므로 `Stop`을 만들지 않고 래치되지 않음
- 제어 판단은 분류 confidence를 보지 않음; 분류 성공 시 argmax class를 그대로 사용
- 분류 결과가 `background`이고 경로 안의 큰 bbox면 미확정 장애물로 `Slow`,
  작거나 경로 밖이면 `Clear` (`Stop`을 만들지 않고 래치도 열지 않음)
- confidence 60% 게이트는 Jetson MJPEG **표시 전용**(`ADAS_OVERLAY_MIN_CONFIDENCE_PPM`)
- 링크·카메라 판단 불능은 래치를 우회해 즉시 `Stop`

## 9. UART 계약

- UART0: MIO 14..15, Linux console, `/dev/ttyPS0`
- UART1: EMIO, 115200 8N1, `/dev/ttyPS1`
- UART1 TXD: Arty Z7-20 PMOD JA1 (`Y18`)
- UART1 RXD: Arty Z7-20 PMOD JA2 (`Y19`)
- Arty와 Raspberry Pi는 TX/RX를 교차하고 GND를 공통으로 연결한다.
- wire frame: `0xA5`, state, CRC-8/SMBUS의 고정 3 bytes
