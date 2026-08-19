# PS 팀 전달 사항 — Arty Z7-20 ROI Classifier PL 연동 계약

## 1. 결론

현재 PL은 PS 팀에서 전달한 `roi_classifier_int8_export_v2`의 연산 계약과 호환됩니다. PL 재합성 없이 아래 전처리, 바이너리 레이아웃, AXI-Lite 주소, GAP/FC 규칙을 PS 소프트웨어에서 지켜 주시면 됩니다.

특히 다음 네 가지를 주의해 주세요.

1. UINT8 ROI를 그대로 PL에 전달하지 말고 INT8로 양자화한 후 1픽셀 zero padding을 적용합니다.
2. `w_conv1.bin`, `w_conv2.bin`은 이미 WPACK으로 변환됐으므로 PS에서 다시 transpose하지 않습니다.
3. GAP 결과를 144로 나누지 않습니다.
4. `s_axi_control`과 `s_axi_CTRL`은 서로 다른 AXI-Lite base address입니다.

## 2. PL 입출력 계약

### 입력 전처리

원본 ROI는 `96×96×3 RGB UINT8`입니다.

각 픽셀에 다음 양자화를 적용합니다.

```text
q = clamp(round(pixel_u8 × 127 / 255), 0, 127)
```

- 결과 dtype: signed INT8
- 레이아웃: NHWC `[96][96][3]`
- 채널 순서: RGB
- zero-point: 0
- 양자화 후 외곽에 1픽셀 zero border 추가
- PL에 전달할 최종 입력: `[98][98][3]`, 28,812 bytes

OpenCV 입력이 BGR이면 반드시 RGB로 변환한 뒤 양자화해야 합니다.

### PL 출력

- 형상: `[12][12][64]`
- dtype: signed INT8
- 레이아웃: NHWC
- 크기: 9,216 bytes
- 출력은 logits가 아니라 conv2+pool2 feature map입니다.

## 3. 가중치와 bias 파일

| 파일 | 크기 | dtype 및 wire layout |
|---|---:|---|
| `w_conv0.bin` | 432 B | INT8 OIHW `[16][3][3][3]` |
| `w_conv1.bin` | 4,608 B | INT8 WPACK `[32][3][3][16]` |
| `w_conv2.bin` | 18,432 B | INT8 WPACK `[64][3][3][32]` |
| `b_conv0.bin` | 64 B | little-endian INT32 `[16]` |
| `b_conv1.bin` | 128 B | little-endian INT32 `[32]` |
| `b_conv2.bin` | 256 B | little-endian INT32 `[64]` |
| `fc_weight.bin` | 384 B | INT8 `[6][64]` |
| `fc_bias.bin` | 24 B | little-endian INT32 `[6]` |

Conv bias에는 rounding compensation이 이미 포함돼 있습니다.

- conv0: +102
- conv1: +13
- conv2: +72

PS나 PL에서 별도의 rounding 항을 추가하면 안 됩니다.

## 4. Requant 파라미터

PL 연산은 다음과 같습니다.

```text
scaled = INT64(acc_int32) × INT64(multiplier)
out = clamp(scaled >> shift, 0, 127)
```

| Layer | multiplier | shift |
|---|---:|---:|
| conv0 | 1,342,756,158 | 38 |
| conv1 | 1,322,019,071 | 35 |
| conv2 | 1,920,779,908 | 38 |

곱셈 중간값은 반드시 64-bit이며, right shift 전에 별도 반올림을 하지 않습니다.

## 5. AXI-Lite 주소 공간

현재 XSA/Vivado address map 기준입니다.

| 인터페이스 | Base address | 용도 |
|---|---:|---|
| `s_axi_control` | `0x40000000` | DDR 포인터 설정 |
| `s_axi_CTRL` | `0x40010000` | start/done, requant, bias |

### `s_axi_control` — base `0x40000000`

| Offset | 내용 |
|---:|---|
| `0x10/0x14` | `ifmap_padded` 64-bit 주소 |
| `0x1C/0x20` | `w_conv0` 64-bit 주소 |
| `0x28/0x2C` | `w_conv1` 64-bit 주소 |
| `0x34/0x38` | `w_conv2` 64-bit 주소 |
| `0x40/0x44` | `out` 64-bit 주소 |

Zynq-7000에서는 실제 DDR 주소가 32-bit 범위여도 HLS 포인터 레지스터의 상위 32-bit를 0으로 기록해 주세요.

### `s_axi_CTRL` — base `0x40010000`

| Offset | 내용 |
|---:|---|
| `0x000` | AP control: bit0 start, bit1 done, bit2 idle, bit3 ready |
| `0x010/0x014` | conv0 multiplier / shift |
| `0x01C/0x020` | conv1 multiplier / shift |
| `0x028/0x02C` | conv2 multiplier / shift |
| `0x040–0x07F` | `b_conv0[16]` INT32 |
| `0x080–0x0FF` | `b_conv1[32]` INT32 |
| `0x100–0x1FF` | `b_conv2[64]` INT32 |

가중치와 입출력은 DDR에 배치하지만, conv bias는 포인터가 아니라 위 AXI-Lite 메모리 창에 값을 직접 기록합니다.

## 6. Cache 및 실행 순서

1. 가중치 `.bin`을 연속된 DDR 영역에 복사합니다.
2. 입력 ROI를 양자화하고 `98×98×3` 버퍼에 zero padding합니다.
3. 입력과 가중치 DDR 영역에 `Xil_DCacheFlushRange()`를 수행합니다.
4. 두 AXI-Lite base에 포인터, bias, requant 값을 기록합니다.
5. AP control bit0에 1을 기록합니다.
6. done bit를 polling하거나 interrupt를 기다립니다.
7. 출력 영역에 `Xil_DCacheInvalidateRange()`를 수행합니다.
8. `12×12×64` 결과에 GAP/FC/argmax를 수행합니다.

입출력 버퍼를 두 세트로 마련하면 다음 ROI 전처리와 이전 결과 후처리를 ping-pong 방식으로 운영할 수 있습니다. 동일 버퍼를 PL 동작 중 덮어쓰면 안 됩니다.

## 7. PS 후처리

각 채널의 `12×12=144`개 INT8 값을 INT32로 합산합니다.

```c
for (c = 0; c < 64; ++c) {
    gap[c] = 0;
    for (y = 0; y < 12; ++y)
        for (x = 0; x < 12; ++x)
            gap[c] += output[(y * 12 + x) * 64 + c];
}
```

`gap[c]`를 144로 나누지 마세요. `1/144`가 FC scale에 이미 반영되어 있습니다.

```text
logit[n] = fc_bias[n] + Σ(fc_weight[n][c] × gap[c])
class_id = argmax(logit[0..5])
```

클래스 순서는 다음과 같습니다.

| ID | Class |
|---:|---|
| 0 | background |
| 1 | car |
| 2 | person |
| 3 | sign_warning |
| 4 | sign_prohibition |
| 5 | sign_mandatory |

## 8. Golden 통합 기준

전달받은 샘플의 기대 결과는 다음과 같습니다.

- 입력: `golden_input_98x98x3_int8.npy`
- PL 기대 출력: `golden_conv2_pool.npy`
- 기대 class ID: `2`
- 기대 class: `person`
- 기대 logits:

```text
[-283044, -148497, 59147, -124434, -232102, -255670]
```

보드 통합 시 PL 출력 9,216 bytes를 `golden_conv2_pool.npy`의 payload와 bit-exact 비교해 주세요. 이 비교가 통과한 뒤 GAP/FC logits와 class ID를 확인하면 PL 문제와 PS 후처리 문제를 분리할 수 있습니다.

## 9. 제공 산출물

- 하드웨어 플랫폼: `deploy/classifier_z7.xsa`
- bitstream: `deploy/classifier.bit`
- FSBL+bitstream 부팅 이미지: `deploy/BOOT.BIN`
- PS 연동 참고 코드: `deploy/classifier_run.c`, `deploy/classifier_run.h`
- PS 전달 모델 파일: `roi_classifier_int8_export_v2.tar.gz`

현재 RTL cosim 처리 시간은 ROI당 932,760 cycles입니다. 100 MHz에서 약 9.3276 ms/ROI, 10 ROI를 순차 처리하는 기준으로 약 10.72 FPS입니다. 실제 보드 FPS에는 PS 전처리, cache maintenance, ROI 개수 및 후처리 시간이 추가됩니다.
