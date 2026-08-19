# PL → PS 인계 — Arty Z7-20 ROI 분류기 (96×96)

**이 폴더가 인계물 전부입니다.** 저장소 없이 이것만 있어도 됩니다.
**이 문서는 자동 생성됩니다** — 아래 숫자는 impl 리포트와 cosim 로그에서 읽은
값이고 손으로 쓴 것이 없습니다.

---

## 0. 먼저 읽을 것 — 이 인계본의 한계

| | |
|---|---|
| ⚠️ **가중치가 난수입니다** | `golden/` 은 **레이아웃·주소 계약 검증용**이지 정확도용이 아닙니다. 학습이 끝나면 weights/bias/requant 만 교체하면 되고 **재합성은 불필요**합니다 |
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
