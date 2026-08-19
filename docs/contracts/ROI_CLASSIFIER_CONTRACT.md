# ROI 분류 파이프라인 계약

이 문서는 Jetson Nano, Arty Z7-20 PS, PL ROI 분류 가속기 사이의
현재 계약을 정의한다.

PL 구현이 담당자별로 둘로 나뉘어 있고 **산술이 서로 다르다**. PL 동작의 정본은
변종별로 아래를 본다.

| 변종 | PL 정본 | INT8 가중치 |
| --- | --- | --- |
| DB | `arty/pl_db/hls/HW/classifier_engine.*` | `arty/models/roi_classifier_int8_db/` |
| EB | `arty/pl_eb/conv_engine_tr8/HW/conv_engine.*` | `arty/models/roi_classifier_int8_eb/` |

아래 §1~ 의 데이터 형식·전송 규약은 두 변종에 공통이다. 활성화·requant 규칙은
변종마다 다르므로 각 PL 정본을 따른다.

## 1. 전체 데이터 흐름

```text
Jetson: V4L2 YUYV → BGR CV_8UC3
        → ROI crop/padding/resize
        → 96×96 RGB UINT8 NHWC
        → persistent TCP client

Arty PS Linux: TCP server
         → RGB UINT8 수신
         → signed INT8 양자화
         → 1픽셀 INT8 0 테두리 추가
         → 98×98×3 INT8 NHWC
         → PL 실행
         → GAP/FC/argmax
         → 분류 결과 TCP 회신
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
- 요청 payload: `96×96×3 = 27,648` bytes RGB UINT8 NHWC
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
| 4 | 2 | version: `1` |
| 6 | 2 | message type: request `1`, response `2` |
| 8 | 4 | frame ID |
| 12 | 4 | ROI ID |
| 16 | 4 | payload size |

Request payload는 27,648 bytes RGB UINT8 NHWC 이미지다.
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

GAP의 제수 `1/144`를 GAP에서 적용할지 FC scale에 흡수할지와
FC 양자화 규칙은 학습·export 결과와 함께 최종 확정한다.
두 곳에서 중복으로 적용하지 않는다.

## 9. 미확정 항목

- GAP/FC 정수 양자화 규칙
- 학습 가중치 기반 레이어별 golden input/output
- PS 빌드용 XSA 최종 배포 경로
