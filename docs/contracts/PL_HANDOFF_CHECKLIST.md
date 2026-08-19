# PL → PS 인계 기준

PL 인계본은 **이 문서의 항목을 모두 만족해야 접수된다.** 미달이면 PS 쪽에서
받지 않고 반려한다. 개별 요청을 기다리지 말고 보내기 전에 자체 확인한다.

기준을 만족하는 실물 예시가 `arty/handoffs/eb/arty96_pl_handoff/` 에 있다.
형식이 애매하면 그쪽을 그대로 따라 하면 된다.

---

## 1. XSA — 보드 설정 (자동 검사 대상)

Arty Z7-20 의 물리 사양이다. **PL 설계와 무관하며 협의 대상이 아니다.**

| 파라미터 | 값 |
| --- | --- |
| `PCW_CRYSTAL_PERIPHERAL_FREQMHZ` | `50` |
| `PCW_UIPARAM_DDR_PARTNO` | `MT41J256M16 RE-125` |
| `PCW_UIPARAM_DDR_BUS_WIDTH` | `16 Bit` |
| `PCW_UIPARAM_DDR_DRAM_WIDTH` | `16 Bits` |
| `PCW_UIPARAM_DDR_DEVICE_CAPACITY` | `4096 MBits` |
| `PCW_UIPARAM_DDR_ROW_ADDR_COUNT` | `15` |
| `PCW_UART0_PERIPHERAL_ENABLE` | `1` |
| `PCW_UART0_UART0_IO` | `MIO 14 .. 15` |
| `PCW_SD0_PERIPHERAL_ENABLE` | `1` |
| `PCW_SD0_SD0_IO` | `MIO 40 .. 45` |

정본은 `arty/pl_eb/system/arty_ps7_preset_z7_20.tcl` 이다. 단 이 파일을 통째로
실행하면 안 된다 — 인스턴스 이름이 `ps7_0` 으로 박혀 있고 `USE_S_AXI_HP0` 을 끈다.
**보드 값만** 가져가고 AXI 포트·FCLK 는 각자 설계 값을 유지한다.

> 왜 이 항목이 생겼나 (2026-08-19): 크리스탈이 `33.333333` 인 XSA 가 왔다.
> 세 PLL 분주비가 전부 그 기준이라 실제 보드에서 CPU 가 2GHz 로 설정됐고
> (정격 667MHz), FSBL 이 PLL 단계에서 죽어 UART 가 한 글자도 안 나왔다.
> 합성·구현·타이밍은 전부 통과한다 — 보드에 올려야만 드러난다.

## 2. XSA 와 bitstream 을 함께 재생성

PS 설정만 고쳤어도 **XSA 를 다시 export 해야 한다.** 부팅을 좌우하는 것은
XSA 안의 `ps7_init` 이다. 블록 디자인만 고치고 기존 XSA 를 주면 아무것도 안 바뀐다.

## 3. `weights/` — 실제 학습 가중치로 검증한 결과

**합성 가중치나 난수로 통과한 cosim 은 인계 근거가 되지 않는다.**

포함할 것:

- 학습팀 INT8 익스포트 원본 (`w_conv*.bin`, `b_conv*.bin`, `fc_*.bin`, `manifest.json`)
- 골든 입력 (`input_roi_96x96x3_int8.bin`, `input_prepad_98x98x3_int8.bin`)
- `SOURCE.sha256` — 원본 파일별 해시
- `PROVENANCE.md` — 아래 내용을 담는다
  - 원본 아카이브 이름과 sha256
  - **레이어별 일치율** (conv0/1/2 의 out·pool 각각)
  - GAP→FC→argmax 결과 (logits 정수 일치 여부, 최종 class_id)
  - 이 인계본의 requant multiplier/shift 와 활성화 방식

> 왜 이 항목이 생겼나: EB 는 난수 가중치(seed=42)로 cosim PASS 상태였다가,
> 실제 익스포트를 넣으니 pool2 일치율이 **20~23%** 로 나왔다.
> **파일 크기·dtype·argmax 는 그때도 통과했다** — 형식 검사로는 못 잡는다.

## 4. 산술 규칙 명시

활성화·시프트·클램프를 문서에 적는다. 두 PL 변종이 이 축에서 다르고,
가중치는 각자의 산술에 맞춰 양자화돼 있어 **교환하면 조용히 틀린다.**

| | DB | EB |
| --- | --- | --- |
| 활성화 | ReLU (clamp 하한 0) | LeakyReLU 13/128, requant 이전 |
| 시프트 | `scaled >> shift`, 라운딩 없음 | `round_shift` (반올림 항) |
| 출력 범위 | `[0, 127]` | `[-128, 127]` |

## 5. `CHECKSUMS.sha256`

인계본 전체에 대해 만든다. 받는 쪽이 `sha256sum -c CHECKSUMS.sha256` 으로
검사한다.

## 6. `reports/`

`utilization_synth`, `utilization_impl`, `timing_impl` 을 포함한다.
타이밍 위반이 있으면 숨기지 말고 `HANDOFF.md` 에 적는다.

---

## 접수 절차

```bash
# 1) 무결성
cd <인계본> && sha256sum -c CHECKSUMS.sha256

# 2) XSA 보드 설정 (§1 자동 검사)
arty/tools/check_xsa.sh <XSA 경로>
```

둘 다 통과해야 PetaLinux 재빌드로 넘어간다.
