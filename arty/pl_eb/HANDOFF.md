# arty96_pl_handoff — Arty Z7-20 ROI 분류 가속기 (96×96)

**트리:** `hls/arty_96_classifier` · **파트:** `xc7z020clg400-1` · **툴:** Vivado / Vitis HLS 2024.2 (빌드 로그에서 읽음)
**이 파일은 `python/build_arty_deliverable.py` 가 생성했습니다** — 표의 모든 숫자는
impl 리포트와 cosim 로그에서 읽은 것이고 손으로 쓴 값은 없습니다.

## 무엇이 들어 있나

```
bitstream/arty96_classifier_v1.xsa
reports/                           impl 면적·타이밍, synth 면적
golden/                            PS 검증용 더미 golden + manifest.json + CONTRACT.txt
sw/                                드라이버 3종 + 주소맵 + PL 시퀀서 참조 구현
`PL_CONTRACT_DELTA.md` — 이 판의 형상·사이클 (루트 `pl_response.md` 는 64 판 문서라 넣지 않았습니다)
```

## op 별 사이클 (전부 cosim 실측)

| op | 사이클 | 출처 |
|---|---|---|
| conv0  3->16 @98x98 pre-pad | 150,894 | cosim PASS (`conv0_cosim_x.log`) |
| pool0  96x96x16 | 50,096 | cosim PASS (`pool_cosim_0.log`) |
| conv1  16->32 @48x48 | 333,768 | cosim PASS (`tr8_cosim_1.log`) |
| pool1  48x48x32 | 25,112 | cosim PASS (`pool_cosim_1.log`) |
| conv2  32->64 @24x24 | 205,188 | cosim PASS (`tr8_cosim_2.log`) |
| pool2  24x24x64 | 12,620 | cosim PASS (`pool_cosim_2.log`) |
| **ROI 1개** | **777,678** | 위 합계 |

**7.78 ms · 129 ROI/s · 프레임당 ROI 10개면 12.86 FPS** (@100.000 MHz)

⚠️ **PL 사이클만입니다.** PS(GAP/FC/softmax), Ethernet 전송, ROI 당 s_axilite
6회 왕복, 실제 DDR 대역폭(Arty DDR3 는 16비트 폭)은 들어 있지 않습니다.

## 면적·타이밍 (Vivado impl 실측)

| | 값 |
|---|---|
| LUT | 42100 (53200%) |
| FF | 43409 (106400%) |
| Slice | **13076 (13300%)** |
| BRAM | 50 (140%) |
| DSP | 198 (220%) |
| WNS @100.000 MHz | **+0.197 ns**, 실패 엔드포인트 0/169318 |
| WHS | +0.013 ns, 실패 0 |

## 주소맵

| IP | base | 크기 |
|---|---|---|
| `conv_engine_0` | `0x4000_0000` | 64K |
| `conv0_engine_0` | `0x4001_0000` | 64K |
| `maxpool_engine_0` | `0x4002_0000` | 64K |

`sw/arty_cls_address_map.h` 참조. 세 엔진의 레지스터 **오프셋이 전부 다릅니다**
(`img_h` 가 0x4c/0x40/0x28) — 하나에서 다른 하나로 오프셋을 옮기지 마십시오.
m_axi 7개는 전부 `0x0000_0000` 의 512MB DDR 창을 보므로 포인터 레지스터에
물리 주소를 그대로 씁니다.

## ⚠️ 이 비트스트림으로 무엇을 바꿀 수 있나

**재합성 없이 교체 가능:** conv weights / bias / requant multiplier·shift /
leaky enable / 형상 레지스터(한계 내).

**비트스트림 고정:** `MAX_IMG_W`(conv 96, conv0 98), `MAX_IN_CH`=64, `MAX_OUT_CH`=64,
`PE_OC`=32, `TR`=4, `OC_GROUP_TILES`=2, LeakyReLU `13/128`, conv0 의 `3/16/3`.

즉 **학습이 끝나면 weights 만 바꿔 넣으면 됩니다.**

### ⚠️ `stride` — 레지스터는 있지만 엔진마다 다릅니다

| 엔진 | 레지스터 | 실제 지원 | 잘못 쓰면 |
|---|---|---|---|
| `conv_engine` | `REG_STRIDE` 0x74 **쓰기 가능** | **1 만** | **조용히 틀립니다.** 소스는 `assert(stride==1)` 로 막지만 그 assert 는 **csim 에서만** 돕니다. RTL 에는 분기 하드웨어가 없고(`(void)stride`), 보드에서 2 를 쓰면 에러 없이 stride=1 연산을 하고 출력 형상만 어긋납니다 |
| `conv0_engine` | — | 3x3 / stride 1 하드와이어 | — |
| `maxpool_engine` | 있음 | **1 과 2 둘 다** (런타임 분기 실제 존재) | stride=2 + padding 조합만 금지(assert) |

**이 표가 "stride=1" 한 줄보다 중요합니다** — 이전 판 문서는 셋을 뭉뚱그려
`stride=1` 이라고만 적어, ① conv 에 쓰기 가능한 레지스터가 있다는 사실과
② pool 은 2 를 실제로 지원한다는 사실을 둘 다 가렸습니다.

## 재현

```bash
cd hls/arty_96_classifier
bash run.sh check                 # 게이트 (툴 불필요)
bash run.sh package tr8|conv0|pool
bash run.sh bd                    # PS7 프리셋 검증
bash run.sh build                 # 합성+구현+XSA
cd <저장소 루트> && python3 python/build_arty_deliverable.py hls/arty_96_classifier
```

빌드 스크립트는 자체 완결입니다 — Digilent 보드 파일 설치가 필요 없습니다
(PS7 프리셋을 `system/arty_ps7_preset_z7_20.tcl` 에 명시 적용).
