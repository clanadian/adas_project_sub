# PL 계약 델타 — 96x96 판

⚠️ **저장소 루트의 `pl_response.md` 는 96x96 판 문서가 아닙니다.**
그 문서는 `pl_request.md`(64x64 기준)에 답한 것이라 형상·버퍼 크기·사이클·FPS 가
전부 64 판 값입니다. **그래서 이 패키지에 넣지 않았습니다.**

설계 불변 부분(레지스터 맵 규칙, GAP 나눗수 규약, 재양자화 계약, stride/pad
의미, 무엇이 재합성 없이 바뀌는가)은 그 문서가 그대로 유효합니다.
**형상에 딸린 값만** 아래로 대체하십시오. 아래는 전부 이 트리에서 유도했습니다.

## 형상

| | 값 |
|---|---|
| ROI (논리 입력) | `96x96x3` |
| conv0 wire 입력 (PRE-PADDED) | `98x98x3` = 28,812 B |
| conv0 출력 | `96x96x16` |
| pool0 출력 | `48x48x16` |
| conv1 출력 | `48x48x32` |
| pool1 출력 | `24x24x32` |
| conv2 출력 | `24x24x64` |
| pool2 출력 (PL 최종) | `12x12x64` |
| PS 가 받을 GAP 입력 | `12x12x64`, GAP 나눗수 **144** |

⚠️ **GAP 나눗수는 해상도에 따라 다릅니다** (64 판 64 / 96 판 144 / 128 판 256).
`gap_mode: sum` + `gap_divisor_folded_into_fc: true` 규약은 그대로이고,
**양쪽에 다 적용하면 argmax 는 맞고 confidence 만 틀립니다** - 사람 눈으로
안 잡히니 `manifest.json` 을 정본으로 쓰십시오.

## 사이클 (전부 cosim 실측 @100.000 MHz)

| op | 사이클 | 출처 |
|---|---|---|
| conv0  3->16 @98x98 pre-pad | 150,894 | cosim PASS (`conv0_cosim_x.log`) |
| pool0  96x96x16 | 50,096 | cosim PASS (`pool_cosim_0.log`) |
| conv1  16->32 @48x48 | 333,768 | cosim PASS (`tr8_cosim_1.log`) |
| pool1  48x48x32 | 25,112 | cosim PASS (`pool_cosim_1.log`) |
| conv2  32->64 @24x24 | 205,188 | cosim PASS (`tr8_cosim_2.log`) |
| pool2  24x24x64 | 12,620 | cosim PASS (`pool_cosim_2.log`) |
| **ROI 1개** | **777,678** | 위 합계 |

**7.78 ms · 129 ROI/s · 프레임당 ROI 10개면 12.86 FPS**

⚠️ **PL 사이클만입니다.** PS(GAP/FC/softmax), Ethernet, ROI 당 커널 6회 시작의
s_axilite 왕복, 실제 DDR 대역폭은 들어 있지 않습니다.

## 주소맵·드라이버

`sw/arty_cls_address_map.h` 가 정본입니다.

- **베이스와 레지스터 오프셋은 64/96/128 판이 전부 동일**합니다
  (`conv 0x4000_0000` / `conv0 0x4001_0000` / `maxpool 0x4002_0000`;
  오프셋 전체를 해시해 확인).
- **버퍼 크기 상수는 해상도에 따라 다릅니다** (`WIRE_INPUT_BYTES`,
  `ACT_BUF_BYTES`, `PL_OUTPUT_BYTES`). 64 판 헤더와 diff 하면 이 세 줄이
  다르게 나오는데 **정상**입니다 — 이 트리에서는 `classifier_net.h` 의
  형상에서 유도합니다.
- 세 엔진의 레지스터 오프셋은 **서로** 다릅니다(`img_h` = 0x4c/0x40/0x28) —
  한 엔진의 오프셋을 다른 엔진에 옮기지 마십시오.

## 이 패키지의 한계 (반드시 읽으십시오)

- **실제 학습 가중치가 들어 있습니다** (`weights/`). 재합성 없이 교체 가능한
  항목이므로, 모델이 갱신되면 그 폴더만 바꿔 넣으면 됩니다.
- **u8 입력 전처리 수식(입력 scale)이 미정**입니다.
- **보드 실측이 없습니다** — DDR 실대역폭과 s_axilite 왕복 비용은 미측정입니다.
