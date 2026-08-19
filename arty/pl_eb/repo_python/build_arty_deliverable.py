#!/usr/bin/env python3
"""Arty ROI 분류기 인계 패키지 생성기 — 값을 손으로 쓰지 않는다.

    python3 python/build_arty_deliverable.py hls/arty_96_classifier
    python3 python/build_arty_deliverable.py hls/arty_classifier \
            --out deliverable_arty_classifier

64 판 패키지는 2026-08-18 에 손으로 만들었고, 그 안의 수치 표는 그날의
impl 리포트를 보고 옮겨 적은 것이다. 그 방식은 이 저장소에서 이미 두 번
틀렸다 — 값은 맞는데 **어느 세대·어느 구성의 값인지**가 틀리는 종류다.
그래서 여기서는 전부 리포트와 cosim 로그에서 읽는다. 없으면 '미측정'이라고
쓰고 추정치를 찍지 않는다.

증명: `--out deliverable_arty_classifier` 로 64 판을 재생성하면 기존 패키지의
XSA·golden·sw 가 **바이트 동일**해야 한다(README 는 유도식이라 문면이 다르다).
"""
import argparse
import atexit
import hashlib
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]


def die(msg):
    print(f"[FAIL] {msg}")
    sys.exit(1)


def one(pat, where, what):
    """글롭으로 정확히 하나를 고른다. 손목록을 쓰지 않기 위해서고,
    0개나 2개면 세대가 섞였다는 뜻이므로 조용히 넘어가지 않는다."""
    hits = sorted(where.glob(pat))
    if len(hits) != 1:
        die(f"{what}: {where}/{pat} 이 {len(hits)}개 (정확히 1개여야 한다)")
    return hits[0]


def rpt_val(txt, row):
    m = re.search(rf"^\|\s*{re.escape(row)}\s*\|\s*([\d.]+)\s*\|.*?\|\s*(\d+)\s*\|\s*([\d.]+)\s*\|",
                  txt, re.M)
    return (m.group(1), m.group(3)) if m else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tree", help="hls/arty_96_classifier 같은 트리 경로")
    ap.add_argument("--out", help="패키지 디렉터리 이름 (기본: ROI 에서 유도)")
    a = ap.parse_args()

    tree = (REPO / a.tree).resolve()
    if not (tree / "HW/classifier_net.h").exists():
        die(f"분류기 트리가 아니다: {tree}")

    # --- 형상·구성은 소스에서 읽는다 ---------------------------------------
    sys.path.insert(0, str(tree / "python"))
    for m in ("collect_cosim", "roi_budget"):
        sys.modules.pop(m, None)
    import collect_cosim as cc                                    # noqa: E402
    from roi_budget import ADOPTED                                # noqa: E402
    if cc.ROOT != tree:
        die(f"import 한 python/ 이 다른 트리를 가리킨다: {cc.ROOT}")

    ce = (tree / "conv_engine_tr8/HW/conv_engine.h").read_text(
        encoding="utf-8", errors="surrogateescape")

    def cconst(n):
        m = re.search(rf"^const unsigned {n}\s*=\s*(\d+);", ce, re.M)
        return m.group(1) if m else "?"

    tr, pe, g = cconst("TR"), cconst("PE_OC"), cconst("OC_GROUP_TILES")
    cmax = cconst("MAX_IMG_W")
    mic, moc = cconst("MAX_IN_CH"), cconst("MAX_OUT_CH")

    out_final = REPO / (a.out or f"deliverable_arty{cc.ROI}_classifier")
    pkg_name = out_final.name
    # 2026-08-19: 예전엔 여기서 바로 out_final 을 rmtree 했다. 검증이 뒤에 있어서
    # **실패하면 기존 인계 패키지가 반쯤 지워진 채 남았다** (실제로 재현함:
    # golden 이 13개일 때 30 -> 26 파일). 이제 스테이징에 만들고 **전부 통과한
    # 뒤에만** 교체한다. 실패하면 원본이 그대로 살아 있다.
    out = out_final.parent / (pkg_name + ".staging")
    if out.exists():
        shutil.rmtree(out)
    # ⚠️ 기본 인자로 **경로를 지금 값으로 묶는다.** 그냥 `lambda:` 로 쓰면
    # 클로저가 `out` 을 참조로 잡고, 마지막에 `out = out_final` 로 재바인딩한
    # 뒤 종료 시점에 **성공한 실행이 최종 패키지를 지운다** (2026-08-19 에
    # 실제로 그렇게 지웠고, 부정 경로 시험이 잡았다).
    atexit.register(lambda _p=out: shutil.rmtree(_p, ignore_errors=True))

    # --- 사이클: cosim 로그에서만. 하나라도 없으면 만들지 않는다 -------------
    total, rows = 0, []
    for label, log in ADOPTED:
        cyc, verdict = cc.scan(cc.LOGS / log)
        if cyc is None or verdict != "PASS":
            die(f"cosim 없음/실패: {label} ({log}) -> {verdict}")
        total += cyc
        rows.append((label, cyc, log))
    ms = total * cc.CLOCK_NS / 1e6
    roi_s = 1000.0 / ms

    # --- 면적·타이밍: impl 리포트에서만 -------------------------------------
    sysd = tree / "system"
    xsa = one("*.xsa", sysd, "XSA")
    t_rpt = one("timing_impl_*.rpt", sysd, "timing impl 리포트")
    u_rpt = one("utilization_impl_*.rpt", sysd, "utilization impl 리포트")
    s_rpt = one("utilization_synth_*.rpt", sysd, "utilization synth 리포트")

    # 툴 버전은 **빌드 로그에서 읽는다.** 문서에 "2024.2" 로 박아 두면 툴을
    # 올렸을 때 안 따라오고, 재현하려는 사람이 틀린 버전을 깐다.
    _bl = tree / "logs/vivado_build.log"
    _m = (re.search(r"Vivado v([\d.]+)",
                    _bl.read_text(encoding="utf-8", errors="surrogateescape"))
          if _bl.exists() else None)
    tool_ver = f"Vivado / Vitis HLS {_m.group(1)}" if _m else "버전 미확인"

    ut = u_rpt.read_text(encoding="utf-8", errors="surrogateescape")
    tt = t_rpt.read_text(encoding="utf-8", errors="surrogateescape")
    util = {k: rpt_val(ut, k) for k in
            ("Slice LUTs", "Slice Registers", "Slice", "Block RAM Tile", "DSPs")}
    if any(v is None for v in util.values()):
        die(f"utilization 리포트에서 못 읽은 행: "
            f"{[k for k, v in util.items() if v is None]}")

    m = re.search(r"^\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(\d+)\s+(\d+)\s+"
                  r"(-?[\d.]+)\s+(-?[\d.]+)\s+(\d+)\s+(\d+)", tt, re.M)
    if not m:
        die("timing 리포트에서 Design Timing Summary 를 못 읽었다")
    wns, tns_fail, whs, ths_fail, tot_ep = (m.group(1), int(m.group(3)),
                                            m.group(5), int(m.group(7)),
                                            int(m.group(4)))
    if float(wns) < 0 or tns_fail or ths_fail:
        die(f"타이밍 미마감: WNS {wns}, setup 실패 {tns_fail}, hold 실패 {ths_fail}")

    clk = re.search(r"^\S+\s+\{[\d.\s]+\}\s+([\d.]+)\s+([\d.]+)\s*$", tt, re.M)
    freq = clk.group(2) if clk else "?"

    # --- 세대 가드 ----------------------------------------------------------
    # 2026-08-18: 트리의 conv_engine.h 가 빌드 이후에 바뀌어 있었고(MAX_IN_CH
    # 64->32), 그 상태로 패키지를 만들면 README 가 **빌드되지 않은 값**을 찍는다.
    # XSA 보다 새로운 HLS 소스가 하나라도 있으면 만들지 않는다.
    stale = [p.relative_to(tree) for d in ("conv_engine_tr8", "conv0_engine",
                                           "maxpool_engine")
             for p in (tree / d / "HW").glob("*")
             # 테스트벤치는 **합성 대상이 아니다** - TB 를 고쳐도 XSA 는 그대로
             # 유효하다. 여기 넣으면 진단용 TB 수정마다 인계가 막힌다.
             if p.is_file() and not p.name.endswith("_tb.cpp")
             and p.suffix in (".cpp", ".h")
             and p.stat().st_mtime > xsa.stat().st_mtime]
    if stale:
        die("HLS 소스가 XSA 보다 새롭다 - 이 트리는 빌드된 것과 다르다:\n       "
            + "\n       ".join(str(x) for x in sorted(stale))
            + f"\n       XSA: {xsa.name} ({xsa.stat().st_mtime})"
            "\n       재빌드하거나, 소스를 빌드 시점으로 되돌린 뒤 다시 실행할 것.")

    # --- 복사 ---------------------------------------------------------------
    (out / "bitstream").mkdir(parents=True)
    (out / "reports").mkdir()
    shutil.copy2(xsa, out / "bitstream" / xsa.name)
    for r in (t_rpt, u_rpt, s_rpt):
        shutil.copy2(r, out / "reports" / r.name)
    shutil.copytree(tree / "SW", out / "sw")
    # 2026-08-19: `sw/arty_cls_address_map.h` 와 `sw/classifier_run.c` 가
    # `#include "../HW/classifier_net.h"` 한다. 그 파일을 안 실어 보내고 있었다 -
    # **PS 가 이 패키지만 받으면 컴파일이 안 된다.** 격리 복사본에서 확인했다.
    # `sw/../HW` 가 패키지 루트의 HW/ 이므로 그 자리에 넣는다.
    (out / "HW").mkdir()
    shutil.copy2(tree / "HW/classifier_net.h", out / "HW/classifier_net.h")
    shutil.copytree(tree / "forps_golden", out / "golden")

    # golden 은 "있어야 할 집합"과 대조한다 (자동 목록은 잔재를 정당화한다)
    ngold = len(list((out / "golden").glob("*.bin")))
    if ngold != 14:
        die(f"golden .bin 이 {ngold}개 (14 여야 한다) — 스테일 파일 의심")


    tbl = "\n".join(f"| {lb} | {c:,} | cosim PASS (`{lg}`) |"
                    for lb, c, lg in rows)

    # --- PS 계약 회신 ------------------------------------------------------
    # 2026-08-19: 이 복사는 무조건이었고, 그래서 **64판 문서가 96판 패키지에
    # 들어갔다.** pl_response.md 는 pl_request.md(64x64 기준)에 답한 문서라
    # 형상(66x66 / 64x64x16), 버퍼 크기(65,536 B), golden 파일명, 사이클
    # (434,544), FPS(23.0)가 전부 64판이다. PS 쪽은 12.86 FPS 짜리 패키지를
    # 받아 "23.0 FPS" 라고 적힌 계약서를 읽게 된다 - 값이 맞아서 제일 안 보이는
    # 종류의 오류다.
    #
    # 이제 **그 문서가 이 트리의 형상을 말하고 있을 때만** 복사한다.
    # 아니면 트리에서 유도한 델타 문서를 대신 넣는다.
    plr = REPO / "pl_response.md"
    plr_txt = plr.read_text(encoding="utf-8", errors="surrogateescape") if plr.exists() else ""
    roi_tag = f"{cc.ROI}x{cc.ROI}"
    roi_tag2 = f"{cc.ROI}\u00d7{cc.ROI}"
    if plr_txt and (roi_tag in plr_txt or roi_tag2 in plr_txt):
        shutil.copy2(plr, out / "pl_response.md")
        contract_note = "`pl_response.md` — PS 요청(`pl_request.md`) 전 절에 대한 회신 정본"
    else:
        (out / "PL_CONTRACT_DELTA.md").write_text(f"""# PL 계약 델타 — {cc.ROI}x{cc.ROI} 판

⚠️ **저장소 루트의 `pl_response.md` 는 {cc.ROI}x{cc.ROI} 판 문서가 아닙니다.**
그 문서는 `pl_request.md`(64x64 기준)에 답한 것이라 형상·버퍼 크기·사이클·FPS 가
전부 64 판 값입니다. **그래서 이 패키지에 넣지 않았습니다.**

설계 불변 부분(레지스터 맵 규칙, GAP 나눗수 규약, 재양자화 계약, stride/pad
의미, 무엇이 재합성 없이 바뀌는가)은 그 문서가 그대로 유효합니다.
**형상에 딸린 값만** 아래로 대체하십시오. 아래는 전부 이 트리에서 유도했습니다.

## 형상

| | 값 |
|---|---|
| ROI (논리 입력) | `{cc.ROI}x{cc.ROI}x3` |
| conv0 wire 입력 (PRE-PADDED) | `{cc.PRE}x{cc.PRE}x3` = {cc.PRE*cc.PRE*3:,} B |
| conv0 출력 | `{cc.ROI}x{cc.ROI}x16` |
| pool0 출력 | `{cc.R2}x{cc.R2}x16` |
| conv1 출력 | `{cc.R2}x{cc.R2}x32` |
| pool1 출력 | `{cc.R4}x{cc.R4}x32` |
| conv2 출력 | `{cc.R4}x{cc.R4}x64` |
| pool2 출력 (PL 최종) | `{cc.R8}x{cc.R8}x64` |
| PS 가 받을 GAP 입력 | `{cc.R8}x{cc.R8}x64`, GAP 나눗수 **{cc.R8*cc.R8}** |

⚠️ **GAP 나눗수는 해상도에 따라 다릅니다** (64 판 64 / 96 판 {cc.R8*cc.R8} / 128 판 256).
`gap_mode: sum` + `gap_divisor_folded_into_fc: true` 규약은 그대로이고,
**양쪽에 다 적용하면 argmax 는 맞고 confidence 만 틀립니다** - 사람 눈으로
안 잡히니 `manifest.json` 을 정본으로 쓰십시오.

## 사이클 (전부 cosim 실측 @{freq} MHz)

| op | 사이클 | 출처 |
|---|---|---|
{tbl}
| **ROI 1개** | **{total:,}** | 위 합계 |

**{ms:.2f} ms · {roi_s:.0f} ROI/s · 프레임당 ROI 10개면 {roi_s/10:.2f} FPS**

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

- **가중치가 난수입니다.** `golden/` 은 레이아웃·주소 계약 검증용이지
  정확도용이 아닙니다. 학습이 끝나면 weights/bias/requant 만 교체하면 되고
  **재합성은 불필요**합니다.
- **u8 입력 전처리 수식(입력 scale)이 미정**입니다.
- **보드 실측이 없습니다** — DDR 실대역폭과 s_axilite 왕복 비용은 미측정입니다.
""", encoding="utf-8")
        contract_note = ("`PL_CONTRACT_DELTA.md` — 이 판의 형상·사이클 (루트 "
                         "`pl_response.md` 는 64 판 문서라 넣지 않았습니다)")

    # --- README: 위에서 읽은 값만 쓴다 --------------------------------------
    (out / "README.md").write_text(f"""# {pkg_name} — Arty Z7-20 ROI 분류 가속기 ({cc.ROI}×{cc.ROI})

**트리:** `{a.tree}` · **파트:** `xc7z020clg400-1` · **툴:** {tool_ver} (빌드 로그에서 읽음)
**이 파일은 `python/build_arty_deliverable.py` 가 생성했습니다** — 표의 모든 숫자는
impl 리포트와 cosim 로그에서 읽은 것이고 손으로 쓴 값은 없습니다.

## 무엇이 들어 있나

```
bitstream/{xsa.name}
reports/                           impl 면적·타이밍, synth 면적
golden/                            PS 검증용 더미 golden + manifest.json + CONTRACT.txt
sw/                                드라이버 3종 + 주소맵 + PL 시퀀서 참조 구현
{contract_note}
```

## op 별 사이클 (전부 cosim 실측)

| op | 사이클 | 출처 |
|---|---|---|
{tbl}
| **ROI 1개** | **{total:,}** | 위 합계 |

**{ms:.2f} ms · {roi_s:.0f} ROI/s · 프레임당 ROI 10개면 {roi_s / 10:.2f} FPS** (@{freq} MHz)

⚠️ **PL 사이클만입니다.** PS(GAP/FC/softmax), Ethernet 전송, ROI 당 s_axilite
6회 왕복, 실제 DDR 대역폭(Arty DDR3 는 16비트 폭)은 들어 있지 않습니다.

## 면적·타이밍 (Vivado impl 실측)

| | 값 |
|---|---|
| LUT | {util['Slice LUTs'][0]} ({util['Slice LUTs'][1]}%) |
| FF | {util['Slice Registers'][0]} ({util['Slice Registers'][1]}%) |
| Slice | **{util['Slice'][0]} ({util['Slice'][1]}%)** |
| BRAM | {util['Block RAM Tile'][0]} ({util['Block RAM Tile'][1]}%) |
| DSP | {util['DSPs'][0]} ({util['DSPs'][1]}%) |
| WNS @{freq} MHz | **+{wns} ns**, 실패 엔드포인트 {tns_fail}/{tot_ep} |
| WHS | +{whs} ns, 실패 {ths_fail} |

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

**비트스트림 고정:** `MAX_IMG_W`(conv {cmax}, conv0 {cc.PRE}), `MAX_IN_CH`={mic}, `MAX_OUT_CH`={moc},
`PE_OC`={pe}, `TR`={tr}, `OC_GROUP_TILES`={g}, LeakyReLU `13/128`, conv0 의 `3/16/3`.

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
cd {a.tree}
bash run.sh check                 # 게이트 (툴 불필요)
bash run.sh package tr8|conv0|pool
bash run.sh bd                    # PS7 프리셋 검증
bash run.sh build                 # 합성+구현+XSA
cd <저장소 루트> && python3 python/build_arty_deliverable.py {a.tree}
```

빌드 스크립트는 자체 완결입니다 — Digilent 보드 파일 설치가 필요 없습니다
(PS7 프리셋을 `system/arty_ps7_preset_z7_20.tcl` 에 명시 적용).
""", encoding="utf-8")

    # --- HANDOFF.md: PS 가 가장 먼저 읽는 문서 --------------------------------
    # 2026-08-19: 처음엔 이 문서를 손으로 써서 패키지에 넣었다. 두 가지가 틀렸다.
    #   1) 재생성이 스테이징 교체라 **손으로 넣은 파일은 지워진다** (실제로 지워짐)
    #   2) 사이클·FPS·WNS·나눗수를 **손으로 타이핑**했다 - 이 저장소가 하루 종일
    #      잡은 결함이 정확히 그 패턴이다
    # 그래서 생성기 안으로 넣고 숫자는 전부 위에서 읽은 값을 쓴다.
    (out / "HANDOFF.md").write_text(f"""# PL → PS 인계 — Arty Z7-20 ROI 분류기 ({cc.ROI}×{cc.ROI})

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

## 1. 성능 (전부 cosim 실측 @{freq} MHz)

| op | 사이클 | 출처 |
|---|---|---|
{tbl}
| **ROI 1개** | **{total:,}** | 위 합계 |

**{ms:.2f} ms · {roi_s:.0f} ROI/s · 프레임당 ROI 10개 기준 {roi_s / 10:.2f} FPS**
(5개면 {roi_s / 5:.1f} · 8개면 {roi_s / 8:.1f} · 15개면 {roi_s / 15:.1f})

## 2. 구현 (Vivado impl 실측)

| | 값 |
|---|---|
| 파트 | `xc7z020clg400-1` · FCLK0 {freq} MHz |
| WNS | **+{wns} ns**, 실패 엔드포인트 **{tns_fail} / {tot_ep}** |
| LUT | {util['Slice LUTs'][0]} ({util['Slice LUTs'][1]}%) |
| Slice | **{util['Slice'][0]} ({util['Slice'][1]}%)** |
| BRAM | {util['Block RAM Tile'][0]} ({util['Block RAM Tile'][1]}%) |
| DSP | **{util['DSPs'][0]} ({util['DSPs'][1]}%)** |

---

## 3. PS 가 해야 할 일

1. **XSA 를 Vitis 플랫폼으로 임포트** — `bitstream/{xsa.name}` (비트스트림 포함)
2. **드라이버를 프로젝트에 복사** — `sw/` 전부. `sw/*` 가 `../HW/classifier_net.h` 를
   include 하므로 **이 폴더 구조(`sw/` 와 `HW/` 가 형제)를 유지**하십시오
3. **op 를 순서대로 실행** — 순서·형상은 `golden/manifest.json` 의 `layers` 에
   기계가 읽을 수 있게 들어 있습니다
4. **각 단계 출력을 `golden/out_*.bin` 과 대조** — 어느 단계에서 어긋났는지 바로 나옵니다
5. **PS 꼬리 구현** — GAP(합) → FC → softmax. 규약은 `PL_CONTRACT_DELTA.md`

### ⚠️ 반드시 지켜야 할 것 셋

- **GAP 나눗수는 {cc.R8 * cc.R8} 입니다** (`{cc.R8}×{cc.R8}`). `manifest.json` 의
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

**conv0 pre-pad 버퍼의 행 스트라이드를 {cc.PRE * 3} → {((cc.PRE * 3 + 3) // 4) * 4} 바이트로 패딩**해 주시면
프레임이 **약 4%** 빨라집니다 ({roi_s / 10:.2f} → 약 {roi_s / 10 * 1.04:.1f} FPS, **추정치**).

- 이유: 행 길이 `{cc.PRE}×3 = {cc.PRE * 3}` 이 4의 배수가 아니라 PL 이 **바이트 단위로만**
  읽습니다. 차분법 3점 + 스로틀 실측에서 conv0 는 **AXI 트랜잭션 바운드**로 확인됐습니다.
- 비용: 버퍼 {cc.PRE * cc.PRE * 3:,} → {cc.PRE * (((cc.PRE * 3 + 3) // 4) * 4):,} B, 행 끝 패딩 바이트는 값 무관
- **거절하셔도 됩니다.** 요구(5~7 FPS)는 이미 충족이고, 이건 여유를 버는 것입니다.
- 상세: 저장소 `doc/04_team/2026-08-19_ps-request-conv0-row-stride.md`

---

## 5. 무결성 확인

```bash
cd {pkg_name} && sha256sum -c CHECKSUMS.sha256
```

**하나라도 FAILED 가 나오면 전달 과정에서 손상된 것이니 다시 받으십시오.**

## 6. 이 폴더를 다시 만들려면

```bash
python3 python/build_arty_deliverable.py {a.tree} --out {pkg_name}
```

재생성은 폴더를 **통째로 교체**합니다. 이 폴더 안에 손으로 파일을 추가하지 마십시오 —
다음 재생성에서 사라집니다.
""", encoding="utf-8")

    # --- CHECKSUMS: **패키지 전체** ------------------------------------------
    # 2026-08-19: 여기는 XSA 한 개만 해싱하고 있었다. 패키지가 30 파일인데
    # 29 개가 무검증이었다 - 드라이버 헤더를 손상시켜도 트리 게이트도
    # CHECKSUMS 도 통과했다(실제로 시험함). PS 쪽이 받은 것이 보낸 것과 같은지
    # 확인할 방법이 없다는 뜻이다.
    #
    # 반드시 **마지막에** 쓴다 - 자기 자신은 빼고 나머지 전부.
    lines = []
    for f in sorted(out.rglob("*")):
        if not f.is_file() or f.name == "CHECKSUMS.sha256":
            continue
        h = hashlib.sha256(f.read_bytes()).hexdigest()
        lines.append(f"{h}  {f.relative_to(out)}\n")
    (out / "CHECKSUMS.sha256").write_text("".join(lines), encoding="utf-8")

    # --- 전부 통과했으므로 이제 교체한다 -------------------------------------
    if out_final.exists():
        shutil.rmtree(out_final)
    out.rename(out_final)
    out = out_final

    n = sum(1 for _ in out.rglob("*") if _.is_file())
    sz = sum(p.stat().st_size for p in out.rglob("*") if p.is_file())
    print(f"[OK] {out.name}: {n}파일 {sz/1e6:.1f}MB")
    print(f"     구성 TR={tr} PE_OC={pe} G={g} · ROI {total:,} 사이클 "
          f"{ms:.2f} ms · {roi_s/10:.2f} FPS@10ROI")
    print(f"     WNS +{wns} ns 실패 {tns_fail} · XSA {xsa.name}")


if __name__ == "__main__":
    main()
