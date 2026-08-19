# PL → PS 인계 — Arty Z7-20 ROI 분류기 (96×96)

**이 폴더가 인계물 전부입니다.** 저장소 없이 이것만 있어도 됩니다.
**이 문서는 자동 생성됩니다** — 아래 숫자는 impl 리포트와 cosim 로그에서 읽은
값이고 손으로 쓴 것이 없습니다.

---

## 0. 먼저 읽을 것 — 이 인계본의 한계

| | |
|---|---|
| ✅ **실제 학습 가중치가 들어 있습니다** | `weights/` (2026-08-19 학습팀 INT8 익스포트). golden 은 그 가중치와 **실제 샘플 이미지**로 생성됐고, 우리 C·numpy 두 참조와 학습팀 파이썬이 **6단계 비트 일치**합니다 (`weights/PROVENANCE.md`) |
| ⚠️ **보드 실측이 없습니다** | 아래 FPS 는 **PL 사이클만**입니다. PS(GAP/FC/softmax)·Ethernet·ROI 당 커널 6회 시작의 s_axilite 왕복·실제 DDR 대역폭이 **안 들어 있습니다** |
| ⚠️ **u8 입력 scale 미정** | 입력 전처리 수식이 정해지면 golden 을 다시 뽑아야 합니다 |

**즉 이것은 "정확도 0 이지만 배선과 계약이 검증된 가속기"입니다.**

---

## 1. 성능 (전부 cosim 실측 @100.000 MHz)

| op | 사이클 | 출처 |
|---|---|---|
| conv0  3->16 @98x98 pre-pad | 150,894 | cosim PASS (`conv0_cosim_x.log`) |
| pool0  96x96x16 | 50,096 | cosim PASS (`pool_cosim_0.log`) |
| conv1  16->32 @48x48 | 333,768 | cosim PASS (`tr8_cosim_1.log`) |
| pool1  48x48x32 | 25,112 | cosim PASS (`pool_cosim_1.log`) |
| conv2  32->64 @24x24 | 205,188 | cosim PASS (`tr8_cosim_2.log`) |
| pool2  24x24x64 | 12,620 | cosim PASS (`pool_cosim_2.log`) |
| **ROI 1개** | **777,678** | 위 합계 |

**7.78 ms · 129 ROI/s · 프레임당 ROI 10개 기준 12.86 FPS**
(5개면 25.7 · 8개면 16.1 · 15개면 8.6)

## 2. 구현 (Vivado impl 실측)

| | 값 |
|---|---|
| 파트 | `xc7z020clg400-1` · FCLK0 100.000 MHz |
| WNS | **+0.197 ns**, 실패 엔드포인트 **0 / 169318** |
| LUT | 42100 (53200%) |
| Slice | **13076 (13300%)** |
| BRAM | 50 (140%) |
| DSP | **198 (220%)** |

---

## 2-b. 가중치 — ⚠️ **레이아웃이 다른 두 벌이 같은 크기입니다**

| 폴더 | 레이아웃 | 용도 |
|---|---|---|
| **`weights/w_conv*.bin`** | conv0 **OIHW** · conv1/2 **WPACK** `[oc][ky][kx][ic]` | **PL 엔진에 DMA 할 것은 이쪽** |
| `golden/w_conv*.bin` | 전부 **OIHW** | 참조 모델 검증용. **DMA 하지 마십시오** |

`w_conv1` 은 양쪽 다 4,608 B, `w_conv2` 는 양쪽 다 18,432 B 입니다.
**크기·dtype 검사로는 바꿔 쓴 것을 못 잡습니다.** conv0 은 두 폴더가 동일합니다
(원래 전치하지 않는 레이어).

- 바이어스: `weights/b_conv*.bin` INT32
- **FC (PS 꼬리):** `weights/fc_weight.bin` INT8 `INT8 [N][64], N=len(classes)=6`,
  `weights/fc_bias.bin` INT32[6]
- 클래스 6종, **순서 고정**: `background, car, person, sign_warning, sign_prohibition, sign_mandatory`
- 정확도: INT8 실측 정확도 **89.7%** (FP32 94.3%, 둘 사이 일치율 92.7%, n=300)
- 출처와 검증 근거: `weights/PROVENANCE.md`

---

## 3. PS 가 해야 할 일

1. **XSA 를 Vitis 플랫폼으로 임포트** — `bitstream/arty96_classifier_v1.xsa` (비트스트림 포함)
2. **드라이버를 프로젝트에 복사** — `sw/` 전부. `sw/*` 가 `../HW/classifier_net.h` 를
   include 하므로 **이 폴더 구조(`sw/` 와 `HW/` 가 형제)를 유지**하십시오
3. **op 를 순서대로 실행** — 순서·형상은 `golden/manifest.json` 의 `layers` 에
   기계가 읽을 수 있게 들어 있습니다
4. **각 단계 출력을 `golden/out_*.bin` 과 대조** — 어느 단계에서 어긋났는지 바로 나옵니다
5. **PS 꼬리 구현** — GAP(합) → FC → softmax. 규약은 `PL_CONTRACT_DELTA.md`

### ⚠️ 반드시 지켜야 할 것 셋

- **GAP 나눗수는 144 입니다** (`12×12`). `manifest.json` 의
  `ps_tail.gap_divisor` 에 값으로 있습니다. **64×64 판 문서에는 64 로 적혀 있는데
  그건 다른 판입니다.** `gap_mode: sum` + `gap_divisor_folded_into_fc: true` 이므로
  **나눗수를 GAP 과 FC scale 양쪽에 적용하지 마십시오** — 그러면 argmax 는 맞고
  confidence 만 틀려서 **기능 시험을 통과합니다.**
- **세 엔진의 레지스터 오프셋이 서로 다릅니다** (`img_h` 가 0x4c / 0x40 / 0x28).
  한 엔진의 오프셋을 다른 엔진에 옮기지 마십시오. `sw/arty_cls_address_map.h` 가 정본입니다.
- **`conv_engine` 의 `REG_STRIDE`(0x74)는 쓰기 가능하지만 1 만 동작합니다.**
  RTL 에 분기 하드웨어가 없어 2 를 쓰면 **에러 없이 틀린 결과**가 나옵니다.
  `maxpool_engine` 은 stride 2 를 실제로 지원합니다 (표는 `README.md`).

---

## 3-b. 첫 점등 순서 (보드를 처음 켤 때)

**PL 쪽은 보드가 없어 여기까지만 검증했습니다** — cosim(이상적 AXI 슬레이브)과
Vivado 타이밍 마감. 아래 순서는 **PS 가 보드에서 처음 돌릴 때** 실패 지점을
좁히기 위한 것입니다. 한 단계씩, 앞 단계가 통과한 뒤에만 다음으로 가십시오.

| # | 무엇을 | 통과 기준 | 실패하면 |
|---|---|---|---|
| 1 | 비트스트림 로드 후 **s_axilite 읽기** — 각 엔진의 `REG_CTRL`(0x00) 을 읽는다 | 0 또는 4(ap_idle) 가 읽힌다 | 0xFFFFFFFF 나 버스 행 → 주소·클럭·비트스트림 문제. `sw/arty_cls_address_map.h` 의 베이스 확인 |
| 2 | **레지스터 왕복** — `REG_IMG_W` 에 96 을 쓰고 되읽는다 | 96 이 돌아온다 | 오프셋이 엔진마다 다르다(`img_h` = 0x4c/0x40/0x28). 한 엔진 값을 다른 엔진에 쓰지 마십시오 |
| 3 | **maxpool 단독** 실행 (`pool2`, 가장 작다: 24×24×64 → 12×12×64) | `golden/out_pl_final_12x12x64_int8.bin` 과 바이트 일치 | 캐시. PL 이 읽기 전에 **flush**, PL 이 쓴 뒤 **invalidate**. 물리 주소를 썼는지도 확인 |
| 4 | **conv0 단독** — 입력 `weights/input_prepad_98x98x3_int8.bin` | `golden/out_conv0_96x96x16_int8.bin` 과 일치 | 가중치 레이아웃. conv0 은 **OIHW**(전치 없음) |
| 5 | **conv1 단독** | `golden/out_conv1_48x48x32_int8.bin` 과 일치 | conv1/2 는 **WPACK**. `weights/w_conv1.bin` 을 쓰십시오. `golden/w_conv1_*` 는 OIHW 라 **크기만 같고 값이 다릅니다** |
| 6 | **체인 6단계 전부** (`sw/classifier_run.c` 순서) | 최종이 `golden/out_pl_final_*` 과 일치 | 중간 단계 golden 이 있으니 어디서 갈렸는지 바로 나옵니다 |
| 7 | **PS 꼬리** — GAP(합) → FC → argmax | `weights/vendor_golden/golden_vector.json` 의 `logits_int32` 와 정수 일치, `class_id=2` | 나눗수 144 를 **두 번 적용**하지 않았는지. argmax 는 맞고 confidence 만 틀리는 방식으로 숨습니다 |

### 그다음 — PL 이 못 잰 것을 PS 가 재 주십시오

아래는 **보드가 있어야만 알 수 있고**, PL 쪽 숫자에는 안 들어 있습니다.

| 재야 할 것 | 왜 | 비교 기준 |
|---|---|---|
| **op 별 실제 소요 시간** | cosim 은 이상적 AXI 슬레이브라 DDR 경합을 모델링하지 않습니다 | `golden/manifest.json` 의 `layers[].cycles_measured` (@100 MHz). 실측이 이보다 크면 그 차이가 **DDR 경합** 몫입니다 |
| **s_axilite 왕복 비용** | ROI 당 커널 6회 시작 = 프레임당 ROI 10개면 **60회** | PL 사이클에 안 들어 있습니다. 이게 크면 커널 시작을 묶는 설계 변경이 필요합니다 |
| ROI 당 전체 시간 | 위 둘의 합 | PL 만: **7.78 ms**. 여기서 얼마나 늘어나는지가 실제 여유입니다 |
| 전력·발열 | 미측정 | — |

**측정값을 PL 로 회신해 주시면** 그 숫자로 다음 최적화 대상을 정할 수 있습니다.
지금은 PL 사이클 기준 conv1 이 42.9%, conv2 26.4%, conv0 19.4%, pool 11.3% 입니다.

---

## 4. PL 이 PS 에 요청하는 것 (선택)

**conv0 pre-pad 버퍼의 행 스트라이드를 294 → 296 바이트로 패딩**해 주시면
프레임이 **약 4%** 빨라집니다 (12.86 → 약 13.4 FPS, **추정치**).

- 이유: 행 길이 `98×3 = 294` 이 4의 배수가 아니라 PL 이 **바이트 단위로만**
  읽습니다. 차분법 3점 + 스로틀 실측에서 conv0 는 **AXI 트랜잭션 바운드**로 확인됐습니다.
- 비용: 버퍼 28,812 → 29,008 B, 행 끝 패딩 바이트는 값 무관
- **거절하셔도 됩니다.** 요구(5~7 FPS)는 이미 충족이고, 이건 여유를 버는 것입니다.
- 상세: 저장소 `doc/04_team/2026-08-19_ps-request-conv0-row-stride.md`

---

## 5. 무결성 확인

```bash
cd arty96_pl_handoff && sha256sum -c CHECKSUMS.sha256
```

**하나라도 FAILED 가 나오면 전달 과정에서 손상된 것이니 다시 받으십시오.**

## 6. 이 폴더를 다시 만들려면

```bash
python3 python/build_arty_deliverable.py hls/arty_96_classifier --out arty96_pl_handoff
```

재생성은 폴더를 **통째로 교체**합니다. 이 폴더 안에 손으로 파일을 추가하지 마십시오 —
다음 재생성에서 사라집니다.
