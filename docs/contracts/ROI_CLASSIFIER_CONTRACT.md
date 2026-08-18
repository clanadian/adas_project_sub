# ROI 분류 파이프라인 계약

이 문서는 Jetson Nano, Arty Z7-20 PS, PL ROI 분류 가속기 사이의
현재 계약을 정의한다. PL 동작의 정본은 `pl/hls/HW/classifier_engine.*`이다.

## 1. 전체 데이터 흐름

```text
Jetson: V4L2 YUYV → BGR CV_8UC3
        → ROI crop/padding/resize
        → 96×96 RGB UINT8 NHWC
        → Ethernet

Arty PS: RGB UINT8 → signed INT8 양자화
         → 1픽셀 INT8 0 테두리 추가
         → 98×98×3 INT8 NHWC
         → PL 실행
         → GAP/FC/argmax
```

## 2. Jetson ROI 계약

- 입력: V4L2Capture가 반환한 BGR `CV_8UC3`
- bbox 가로·세로에 각각 15% 여백 추가
- 긴 변을 기준으로 정사각형 확장
- 프레임 밖은 검은색으로 padding
- OpenCV `INTER_LINEAR`로 `96×96` resize
- crop 후 BGR→RGB 변환
- Ethernet 전송 픽셀: `96×96×3`, RGB, UINT8, NHWC

## 3. 입력 양자화·padding 계약

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

## 4. PL 고정 구조

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

## 5. 가중치 레이아웃

| 레이어 | 입력 레이아웃 | 형식 |
|---|---|---|
| Conv0 | `[16][3][3][3]` | OIHW INT8 |
| Conv1 | `[32][3][3][16]` | WPACK `[oc][ky][kx][ic]` INT8 |
| Conv2 | `[64][3][3][32]` | WPACK `[oc][ky][kx][ic]` INT8 |

각 레이어는 INT32 bias와 `multiplier: INT32`, `shift: UINT8` requant
파라미터를 사용한다.

## 6. 클래스 계약

| index | class |
|---:|---|
| 0 | background |
| 1 | car |
| 2 | person |
| 3 | sign_warning |
| 4 | sign_prohibition |
| 5 | sign_mandatory |

## 7. PS 후처리

PL이 반환한 `12×12×64` feature map에 다음을 수행한다.

```text
GAP → FC 64×6 → logits → argmax
```

GAP의 제수 `1/144`를 GAP에서 적용할지 FC scale에 흡수할지와
FC 양자화 규칙은 학습·export 결과와 함께 최종 확정한다.
두 곳에서 중복으로 적용하지 않는다.

## 8. 미확정 항목

- Ethernet packet header와 byte order
- GAP/FC 정수 양자화 규칙
- 학습 가중치 기반 레이어별 golden input/output
- PS 빌드용 XSA 최종 배포 경로
