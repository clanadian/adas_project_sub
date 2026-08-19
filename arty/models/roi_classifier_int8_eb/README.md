# Arty Z7-20 ROI 분류기 — INT8 Export (**arty_96 변종용**)

> ⚠️ **PL 프로토타입이 두 종류라, 가중치 세트도 두 벌입니다.**
> 이 패키지는 `hls/arty_96_classifier/` 엔진 **전용**입니다.
> 다른 엔진(`team_arty_96/z7_classifier_64_hls/`)용은 별도 패키지
> `roi_classifier_int8_export/`에 있습니다.
> **두 패키지는 파일 크기·dtype·레이아웃이 완전히 동일해서 형식 검사로는
> 섞여도 안 걸립니다.** 반드시 엔진에 맞는 쪽을 쓰세요.

| | 이 패키지 (arty_96) | 다른 패키지 (z7_64) |
|---|---|---|
| conv0/conv1 활성화 | **leaky ReLU 13/128** | plain ReLU |
| conv2 활성화 | **linear (없음)** | plain ReLU |
| requant 반올림 | **round_shift (ties away from zero)** | 없음 (truncate `>>`) |
| 출력 saturate | **`[-128, 127]`** | `[0, 127]` |
| bias 반올림 보정 | **0 (없음)** | +102 / +13 / +72 |
| FP32 체크포인트 | leaky 파인튜닝본 | ReLU 원본 |

참조: `conv_engine.cpp:76`, `conv0_engine.cpp:47`의 `apply_activation()`

## 산출물 (`export/`)

| 파일 | 크기 | 내용 |
|---|---|---|
| `w_conv0.bin` | 432 B | INT8 **OIHW** `[16][3][3][3]` — 전치 없음 |
| `w_conv1.bin` | 4,608 B | INT8 **WPACK** `[32][3][3][16]` = `OIHW.transpose(0,2,3,1)` |
| `w_conv2.bin` | 18,432 B | INT8 **WPACK** `[64][3][3][32]` |
| `b_conv0/1/2.bin` | 64 / 128 / 256 B | INT32 bias (**보정값 없음**) |
| `fc_weight.bin` | 384 B | INT8 `[6][64]` |
| `fc_bias.bin` | 24 B | INT32 `[6]` |
| `manifest.json` | — | 전 레이어 scale·requant·활성화·GAP 규칙·클래스 순서 |
| `golden_*.npy` | — | 레이어별 bit-exact golden |
| `golden_vector.json` | — | golden 요약 (shape, GAP 합, logits, 최종 클래스) |
| `int8_vs_fp32_agreement.json` | — | 정확도 검증 결과 |

WPACK 변환은 exporter에서 완료했습니다. PS는 전치 없이 그대로 DDR 복사하면 됩니다.

## 데이터패스

```
논리 입력:  96×96×3 RGB UINT8
PS 전처리:  q = clamp(round(pixel × 127/255), 0, 127)   [symmetric, zero-point 0]
            → 1픽셀 zero border → 98×98×3 signed INT8 NHWC

conv0: 3→16  3×3 → leaky 13/128 → requant → maxpool → 48×48×16
conv1: 16→32 3×3 → leaky 13/128 → requant → maxpool → 24×24×32
conv2: 32→64 3×3 → (활성화 없음)  → requant → maxpool → 12×12×64  (PL 출력)

활성화:  if acc < 0:  acc = round_shift(acc × 13, 7)      # conv0/conv1만
requant: out = saturate(round_shift(acc × multiplier, shift))  → [-128, 127]
round_shift(x,s):  x≥0 → (x + (1<<(s-1))) >> s
                   x<0 → -(((-x) + (1<<(s-1))) >> s)
```

**출력에 음수가 나오는 게 정상입니다.** `[0,127]`이 아니라 `[-128,127]`이고,
conv2는 활성화가 없어 음수가 압축 없이 그대로 GAP로 들어갑니다.

### 레이어별 requant 파라미터

| 레이어 | multiplier (INT32) | shift |
|---|---|---|
| conv0 | 1545298110 | 37 |
| conv1 | 1525725976 | 36 |
| conv2 | 1924470265 | 39 |

### GAP / FC

```
12×12×64 INT8 → 채널별 144개 단순 합산(평균 아님) → INT32 [64]
→ FC INT8 [6][64] + bias INT32 [6] → logits INT32 [6] → argmax
```

**PS에서 144로 나누지 마세요** — 1/144는 FC scale(`gap_output_scale`)에 이미
흡수돼 있습니다. 양쪽에서 적용하면 144배 작아집니다.

### 클래스 순서 (FC 출력 순서)

```
0: background   1: car   2: person
3: sign_warning   4: sign_prohibition   5: sign_mandatory
```

## PL 구현 시 주의

- **requant 중간값은 64비트 필요**: `acc × multiplier`가 최대 2×10¹³ 수준까지
  커져 INT32로는 오버플로우합니다. (엔진이 이미 `ap_int<64>`라고 확인해주셨습니다.)
  accumulator 자체는 최악의 경우도 465만 수준이라 INT32로 충분합니다.
- **bias에 아무것도 더하지 마세요.** 이 변종은 HW가 이미 반올림하므로 보정값이
  0입니다(`bias_includes_rounding_compensation: 0`). 다른 패키지와 헷갈리지 마세요.

## 정확도

```
FP32 (leaky 파인튜닝본)   94.3%
INT8 (이 산출물)          89.7%
INT8 vs FP32 일치율       92.7%
                          (실제 val crop 300장, 정답 라벨 기준)
```

## 이 변종을 만들며 필요했던 추가 작업

같은 체크포인트를 그냥 재양자화하면 안 되는 것들이 있어서, 아래 세 가지를
추가로 처리했습니다. **어느 것도 하드웨어 계약을 건드리지 않습니다** — 파일 형식,
dtype, 레이아웃, 연산 순서 전부 동일하고 숫자만 달라집니다.

### 1. leaky 파인튜닝 (필수)

기존 체크포인트는 ReLU로 학습된 것이라, leaky 하드웨어에 그대로 넣으면
**66.5%로 떨어집니다**. ReLU는 음수를 0으로 죽이는 전제로 필터가 맞춰져 있는데
leaky는 음수를 0.1배로 살려 보내기 때문입니다. 3 epoch 파인튜닝으로 회복했습니다.

```
파인튜닝 전 66.5% → epoch1 94.5% → epoch3 94.8% (val), test 94.3%
```

### 2. Cross-Layer Equalization (필수)

conv0의 **출력 채널별 가중치 크기가 24배** 차이나서, per-tensor 스케일이 가장 큰
채널에 맞춰지고 작은 채널은 127단계 중 5단계만 쓰게 됩니다. 가중치 양자화만으로
93.5% → 75.5%로 떨어졌습니다.

leaky ReLU가 양의 스케일에 homogeneous(`leaky(k·x)=k·leaky(x)`)라는 성질을 이용해,
레이어 L의 출력 채널 i를 1/s로 줄이고 L+1의 입력 채널 i를 s로 키웠습니다. MaxPool과
GAP도 양의 스케일과 교환되므로 **네트워크 함수는 수학적으로 완전히 동일**합니다
(FP32 정확도가 전후 93.5%로 동일한 것으로 검증).

```
채널 편차  conv0 24.2x → 4.3x,  conv1 9.6x → 2.6x,  conv2 3.7x → 2.6x
가중치 양자화 정확도  75.5% → 93.5%
```

### 3. Percentile calibration

abs-max는 드문 outlier 하나가 스케일을 잡아먹습니다. leaky/linear는 음수까지
해상도를 나눠 써야 해서 ReLU보다 훨씬 불리합니다. 99.5 percentile로 변경했습니다
(100/99.99/99.9/99.5/99 스윕에서 최적).

## 검증 완료 항목

전달 파일만 가지고 독립적으로 재계산해 확인:

- **산술이 PL 레퍼런스와 일치**: `model_response.md` §4의 `round_shift` /
  `apply_activation` 구현과 20,009개 값 × 5개 (multiplier, shift) 조합 ×
  leaky on/off 전부 bit-exact 일치
- **bit-exact 재현성**: 전달 바이너리 + 전달 입력으로 conv0을 별도 naive 구현으로
  재계산 → shipped golden과 완전 일치
- **CLE 함수 보존**: FP32 정확도가 CLE 전후 93.5%로 동일
- 파일 크기 8종, WPACK wire index 공식(`dst=((oc*K+ky)*K+kx)*in_ch+ic`),
  signed 범위, golden shape 체인, GAP=합산, FC 재계산, 클래스 순서

## 파일 구성

```
finetune_leaky.py    ReLU 체크포인트 → leaky 활성화로 파인튜닝
cle.py               Cross-Layer Equalization (per-tensor 양자화용 채널 균등화)
fixed_point.py       round_shift(ties away from zero), apply_activation(leaky+requant+saturate),
                     multiplier/shift 유도, 대칭 양자화
quantize_export.py   BN fusion → CLE → percentile calibration → 양자화
                     → WPACK 변환 → .bin + manifest
golden_int8.py       순수 정수 bit-exact golden 모델 + FP32 대조 검증
```

## 재현 방법

```bash
yolo_env/bin/python roi_classifier_int8_export_arty96/finetune_leaky.py --epochs 3
yolo_env/bin/python roi_classifier_int8_export_arty96/quantize_export.py
yolo_env/bin/python roi_classifier_int8_export_arty96/golden_int8.py
```

기준 체크포인트: `/home/user/fpga_roi_classifier_data/runs/leaky_finetune_r96/best.pt`
