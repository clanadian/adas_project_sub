# Arty Z7-20 ROI 분류기 — INT8 Export

96×96×3 RGB ROI를 6개 클래스로 분류하는 모델의 **INT8 하드웨어 배포 산출물**입니다.
PL 계약(`arty/pl_db/hls/HW/classifier_engine.*`) 기준으로 생성했습니다.

FP32 학습은 `roi_classifier_fp32/`에서 진행했고, 이 디렉터리는 그 결과를 **읽기 전용으로
참조**해서 양자화만 수행합니다 (`roi_classifier_fp32/`의 파일은 수정하지 않습니다).

## 전달 산출물 (`export/`)

| 파일 | 크기 | 내용 |
|---|---|---|
| `w_conv0.bin` | 432 B | INT8 **OIHW** `[16][3][3][3]` — 전치 없음 |
| `w_conv1.bin` | 4,608 B | INT8 **WPACK** `[32][3][3][16]` = `OIHW.transpose(0,2,3,1)` |
| `w_conv2.bin` | 18,432 B | INT8 **WPACK** `[64][3][3][32]` |
| `b_conv0/1/2.bin` | 64 / 128 / 256 B | INT32 bias |
| `fc_weight.bin` | 384 B | INT8 `[6][64]` |
| `fc_bias.bin` | 24 B | INT32 `[6]` |
| `manifest.json` | — | 전 레이어 scale, requant multiplier/shift, GAP 규칙, 클래스 순서 |
| `golden_*.npy` | — | 레이어별 bit-exact golden input/output |
| `golden_vector.json` | — | golden 샘플 요약 (shape, GAP 합, logits, 최종 클래스) |
| `int8_vs_fp32_agreement.json` | — | 정확도 검증 결과 |

**WPACK 변환은 이 exporter에서 완료했습니다.** PS는 전달받은 파일을 전치 없이 그대로
DDR에 복사하면 됩니다.

## 하드웨어 계약 요약

```
논리 입력:  96×96×3 RGB UINT8
PS 전처리:  q = clamp(round(pixel × 127/255), 0, 127)   [symmetric, zero-point 0]
            → 1픽셀 zero border → 98×98×3 signed INT8 NHWC
conv0: 3→16  3×3 ReLU maxpool → 48×48×16
conv1: 16→32 3×3 ReLU maxpool → 24×24×32
conv2: 32→64 3×3 ReLU maxpool → 12×12×64  (PL 최종 출력)
requant: out = clamp((acc × multiplier) >> shift, 0, 127)
         rounding 없음 — 단순 arithmetic right shift (HLS `scaled >> shift`와 동일)
GAP/FC/argmax: PS에서 수행
```

### 레이어별 requant 파라미터

| 레이어 | multiplier (INT32) | shift |
|---|---|---|
| conv0 | 1467099144 | 38 |
| conv1 | 1160501223 | 35 |
| conv2 | 1422046702 | 38 |

### GAP 처리 규칙 (중요)

```
12×12×64 INT8 → 채널별로 144개 값을 단순 합산(평균 아님) → INT32 [64]
→ FC INT8 [6][64] + bias INT32 [6] → logits INT32 [6] → argmax
```

**PS에서 144로 나누지 마세요.** 수학적 1/144는 FC의 scale에 이미 흡수되어 있습니다
(`manifest.json`의 `gap_output_scale`). 양쪽에서 적용하면 값이 144배 작아집니다.

argmax만 필요하면 `logits_int32`를 그대로 쓰면 됩니다. 실수 confidence나 softmax가
필요할 때만 `logits_scale`을 곱하세요.

### 클래스 순서 (FC 출력 순서)

```
0: background   1: car   2: person
3: sign_warning   4: sign_prohibition   5: sign_mandatory
```

### ⚠️ requant rounding: HLS는 그대로, 보정은 bias에 반영됨

**HLS 코드는 수정할 필요 없습니다.** 현재 `scaled >> shift` (rounding 없음) 그대로
두시면 되고, 반올림 보정은 저희가 **bias 값 안에 미리 넣어서** 전달합니다.

경위: 초기 버전은 golden이 round-half-away-from-zero를 썼는데, 최신 HLS가 rounding
없이 truncate한다는 것이 확인되어 golden을 HLS에 맞췄습니다. 그런데 truncate는 레이어마다
평균 0.5 LSB씩 아래로 편향되고 3개 레이어에 누적되어, 실측 결과 **정확도가 20%p 떨어졌습니다**
(90.3% → 69.7%).

해결: `(acc × M) >> shift`에서 반올림 항 `2^(shift-1)`을 곱셈 결과에 더하는 것은
**accumulator에 `2^(shift-1)/M`을 더하는 것과 대수적으로 동일**합니다. bias는 requant
직전에 accumulator로 들어가므로, 이 값을 export 시점에 bias에 접어 넣었습니다.

| 레이어 | bias에 포함된 보정값 |
|---|---|
| conv0 | +94 |
| conv1 | +15 |
| conv2 | +97 |

`manifest.json`의 각 레이어에 `bias_includes_rounding_compensation` 필드로 명시되어
있습니다. **PS/PL은 이 값을 따로 더하면 안 됩니다** — 이미 `b_conv*.bin` 안에 들어있습니다.

결과: 정확도 90.3% 회복, HLS 데이터패스는 바이트 단위로 동일, golden과 bit-exact 유지.

## ⚠️ PL 구현 시 주의: requant 중간값은 64비트 필요

실측 결과 `acc × multiplier` 중간값이 최대 **2.18×10¹³** 까지 커집니다. INT32로는
오버플로우하므로 **곱셈·시프트 중간 변수는 반드시 64비트**여야 합니다.

accumulator 자체는 이론상 최악(모든 입력·가중치 127)에도 conv2에서 약 465만이라
INT32로 충분합니다.

## 정확도

```
FP32 (원본 모델)      94.3%
INT8 (이 산출물)      94.3%     ← FP32와 사실상 동률
INT8 vs FP32 일치율   97.7%
                      (실제 val crop 300장, 정답 라벨 기준)
```

## 캘리브레이션 개선 (90.3% → 94.3%)

이전 산출물은 **단순 abs-max calibration + CLE 없음**으로 90.3%였습니다(하드웨어가
요구하는 per-tensor 대칭 양자화 자체의 한계로 봤었음). 실측해보니 그게 아니라
calibration 방법 문제였습니다.

percentile(100/99.99/99.9/99.5/99/98) × CLE(적용/미적용) 조합을 실제 val set 300장으로
스윕한 결과:

| percentile | CLE | INT8 정확도 | FP32 대비 일치율 |
|---|---|---|---|
| 100 (이전 산출물과 동일 조건) | 미적용 | 90.3% | 90.7% |
| 99.5 | 미적용 | 93.0% | 94.7% |
| 98 | 미적용 | 88.7% | 91.0% (너무 세게 자르면 역효과) |
| 100 | **적용** | 94.0% | 97.7% |
| **99.9** | **적용** | **94.3%** | **97.7%** |
| 99.5 | 적용 | 91.0% | 93.0% (CLE 적용 후엔 세게 자를수록 오히려 손해) |

**개선의 대부분은 CLE(Cross-Layer Equalization)에서 나옵니다.** `conv0`의 채널별
가중치 크기가 **25배** 차이 나서(`cle.py`의 `channel_spread`로 실측), per-tensor
스케일이 가장 큰 채널에 맞춰지고 작은 채널은 127단계 중 몇 단계만 쓰게 됩니다. leaky
ReLU뿐 아니라 **plain ReLU도 양의 동차함수**(`ReLU(k·x)=k·ReLU(x)`, k>0)라서 CLE
전제조건(활성화 함수가 양의 스케일과 교환됨)을 그대로 만족합니다 — 레이어 L의 출력
채널 i를 1/s로 줄이고 L+1의 입력 채널 i를 s로 키우면 MaxPool·GAP도 양의 스케일과
교환되므로 **네트워크 함수는 수학적으로 완전히 동일**합니다(CLE 전후 FP32 정확도가
94.3%로 동일한 것으로 검증). CLE 적용 후 채널 스펙트럼: `conv0` 25.0x→4.7x, `conv1`
9.5x→2.8x, `conv2` 3.4x→2.5x.

Percentile calibration(99.9)은 CLE 위에 얹었을 때 소폭(94.0%→94.3%) 추가 이득이
있습니다. 너무 세게 자르면(98 이하) 오히려 나빠지는데, ReLU+maxpool 조합에서는 큰
활성화값이 maxpool에서 살아남는 진짜 신호인 경우가 많아 percentile 클리핑이 그 신호를
지워버리기 때문으로 보입니다.

QAT(양자화 인지 학습, 재학습 필요) 없이 **PTQ(export-side 변환)만으로** FP32와 동률까지
회복했으므로, 지금은 QAT가 필요 없습니다.

## 검증 완료 항목

독립적으로 재계산해서 확인한 것들:

- **WPACK 레이아웃**: 체크포인트에서 가중치를 처음부터 다시 양자화해 비교 → 완전 일치.
  PL 문서의 wire index 공식 `dst = ((oc*K+ky)*K+kx)*in_ch+ic` 로 직접 인덱싱해도 일치.
- **bit-exact 재현성**: 전달 바이너리 + 전달 입력만으로 conv0을 별도 naive 구현으로
  재계산 → shipped golden과 완전 일치.
- **GAP divisor 144**: 96×96 입력이므로 PL 출력이 12×12. (구 64×64 계약의 64를 그대로
  썼다면 2.25배 오차)
- 파일 크기 8종, INT8/INT32 범위, requant M값 오차(1e-10 수준), scale 체인 연결,
  1픽셀 zero border, ReLU/clamp 준수, FC 재계산, 클래스 순서 일관성.

## 파일 구성

```
quantize_export.py   BatchNorm fusion → CLE → percentile calibration(512장) → per-tensor
                     대칭 양자화 → requant multiplier/shift 유도 → WPACK 변환 → .bin + manifest 출력
golden_int8.py       순수 정수 연산 bit-exact golden 모델 (int8×int8→int32 누적, int64
                     requant, ReLU, int8 clamp). 레이어별 golden 생성 + FP32 대조 검증
fixed_point.py       requant 산술 유틸 (rounding 없는 arithmetic right shift, multiplier/shift 유도,
                     대칭 양자화)
cle.py               Cross-Layer Equalization (per-tensor 양자화용 채널 균등화)
```

## 재현 방법

```bash
yolo_env/bin/python roi_classifier_int8_export/quantize_export.py   # .bin + manifest 생성
yolo_env/bin/python roi_classifier_int8_export/golden_int8.py       # golden + 정확도 검증
```

기준 체크포인트: `/home/user/fpga_roi_classifier_data/runs/yolo_transfer_r96/best.pt`
(random 초기화 val 93.5% vs yolo_transfer val 95.2% 비교에서 선택된 쪽)
