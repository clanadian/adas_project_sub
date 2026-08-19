#!/usr/bin/env python3
"""후보 구성 비교 — DSP 예산과 ROI 사이클을 같이 본다.

이 파일이 존재하는 이유: "면적이 남으면 PE_OC 를 올린다" 처럼 **막는 자원을
틀리게 짚는** 실수를 이미 한 번 했다. 여기서는 DSP·LUT·BRAM 을 전부 들고
다니면서, 사이클 이득과 자원 비용을 한 화면에서 본다.

숫자의 출처를 행마다 표시한다. 'M' = 실측, 'P' = 예측/외삽.
    python3 python/config_compare.py
"""
import sys
from pathlib import Path

# ============================================================================
# ⚠️ 2026-08-19: 이 도구는 **측정 전 예측기**이고 ROI 가 64 로 박혀 있다.
#
# 이 트리(96x96)에서 그냥 돌리면 "ROI 64x64 ... 28.1 FPS" 를 찍는다. 이 트리의
# 실측은 **777,678 사이클 / 7.78 ms / 12.86 FPS** 다. 두 숫자는 세 배 넘게
# 다르고, 표에 'ROI 64x64' 라고 적혀 있어도 스크롤하다 보면 놓친다.
#
# 이 파일은 `run.sh check` 가 부르지 않는다(참조 0). 남겨 두는 이유는 상단의
# csynth 면적 표가 여전히 유효하고 - 그건 리포트에서 직접 읽는다 - "막는 자원을
# 틀리게 짚는" 실수를 다시 하지 않기 위한 기록이기 때문이다.
#
# **사이클/FPS 를 알고 싶으면 `python3 python/roi_budget.py` 를 쓸 것.**
# 그쪽은 cosim 로그에서 읽고, 값이 없으면 "인용하지 말 것" 이라고 찍는다.
# ============================================================================

import re

ROOT = Path(__file__).resolve().parents[1]

# ---------------------------------------------------------------------------
# 실측 (M) — 출처를 각각 명시
# ---------------------------------------------------------------------------
# 위치당 사이클. doc/01_status/2026-08-15_zybo-frame-measured.md, tr16 구성,
# xc7z020 @100MHz, 22-op cosim 스윕.
CYC_TR16 = {(3, 16): 93.3, (16, 32): 155.7, (32, 64): 361.7}

# TR 8 -> 16 이 벌어준 비율(같은 문서, 같은 스윕). 역수가 TR=8 의 벌점이다.
# in_ch 가 작을수록 TR 의 효과가 작다는 것이 이 표의 요지.
TR16_GAIN = {(16, 32): 5_738_309 / 6_477_197,   # 0.886  -> TR8 은 x1.129
             (32, 64): 3_333_245 / 4_073_753}   # 0.818  -> TR8 은 x1.222

# conv0_engine 위치당 사이클. ⚠️ KV260 세대에서 **512 폭** 레이어 0 을 재서
# 나온 값이다(project_conv0_engine_measured_3x_not_26x). 64x64 에서 같은 값이
# 나온다는 보장이 없다 - 고정 오버헤드가 위치 4,096 개에 덜 amortize 된다.
# 그래서 'P' 로 표시한다. bash run.sh cosim conv0 이 이 값을 교체한다.
CONV0_CYC_PER_POS = 13.1

POOL_ALLOWANCE = 3000     # P: 형상이 작아 conv 대비 무시 수준
CLOCK_HZ = 100e6
DSP_AVAIL, LUT_AVAIL, BRAM36_AVAIL = 220, 53200, 140


def read_csynth(path):
    """csynth 리포트에서 (BRAM18, DSP, FF, LUT) 를 뽑는다. 없으면 None."""
    p = Path(path)
    if not p.exists():
        return None
    txt = p.read_text(encoding="utf-8", errors="surrogateescape")
    m = re.search(r"^\|Total\s+\|\s*(\S+)\|\s*(\S+)\|\s*(\S+)\|\s*(\S+)\|", txt, re.M)
    if not m:
        return None
    def num(s):
        s = s.strip()
        return int(s) if s.isdigit() else 0
    return dict(bram18=num(m.group(1)), dsp=num(m.group(2)),
                ff=num(m.group(3)), lut=num(m.group(4)))


ENGINES = {
    "conv(TR=16)": "conv_engine/conv_engine_prj/solution1/syn/report/conv_engine_csynth.rpt",
    "conv(TR=8)":  "conv_engine_tr8/conv_engine_prj/solution1/syn/report/conv_engine_csynth.rpt",
    "conv0":       "conv0_engine/conv0_engine_prj/solution1/syn/report/conv0_engine_csynth.rpt",
    "maxpool":     "maxpool_engine/maxpool_engine_prj/solution1/syn/report/maxpool_engine_csynth.rpt",
}


def conv_cycles(roi, use_conv0, tr8):
    """conv0/conv1/conv2 의 사이클. (dict, 모두_실측인가)"""
    out, measured = {}, True
    # conv0: 3->16 @ roi x roi
    pos0 = roi * roi
    if use_conv0:
        out["conv0"] = (pos0 * CONV0_CYC_PER_POS, "P")   # 다른 형상에서 외삽
        measured = False
    else:
        out["conv0"] = (pos0 * CYC_TR16[(3, 16)], "M")
    # conv1: 16->32 @ roi/2
    pos1 = (roi // 2) ** 2
    c1 = pos1 * CYC_TR16[(16, 32)]
    if tr8:
        c1 /= TR16_GAIN[(16, 32)]
        out["conv1"] = (c1, "M+M")     # 실측 cyc/pos x 실측 TR 비율
    else:
        out["conv1"] = (c1, "M")
    # conv2: 32->64 @ roi/4
    pos2 = (roi // 4) ** 2
    c2 = pos2 * CYC_TR16[(32, 64)]
    if tr8:
        c2 /= TR16_GAIN[(32, 64)]
        out["conv2"] = (c2, "M+M")
    else:
        out["conv2"] = (c2, "M")
    return out, measured


def main():
    print("\u26a0\ufe0f  이 도구는 **측정 전 예측기**이고 ROI 가 64 로 박혀 있다.")
    print("   아래 사이클/FPS 표는 이 트리의 값이 아니다 - 실측은")
    print("   `python3 python/roi_budget.py` (cosim 로그에서 읽는다).")
    print()
    print("=" * 78)
    print("엔진별 csynth 실측 (없으면 '-' = 아직 안 돌림)")
    print("=" * 78)
    area = {}
    print(f"{'엔진':<14}{'BRAM18':>8}{'DSP':>7}{'FF':>9}{'LUT':>9}")
    for name, rel in ENGINES.items():
        a = read_csynth(ROOT / rel)
        area[name] = a
        if a:
            print(f"{name:<14}{a['bram18']:>8}{a['dsp']:>7}{a['ff']:>9}{a['lut']:>9}")
        else:
            print(f"{name:<14}{'-':>8}{'-':>7}{'-':>9}{'-':>9}")

    print()
    print("=" * 78)
    print("구성 후보 (ROI 64x64, 10 ROI/frame 기준)")
    print("=" * 78)
    hdr = f"{'구성':<26}{'DSP':>7}{'판정':>7}{'ROI 사이클':>13}{'ms':>8}{'FPS':>7}  출처"
    print(hdr); print("-" * 78)

    for label, engs, use_conv0, tr8 in [
        ("conv(TR=16) + pool",           ["conv(TR=16)", "maxpool"],          False, False),
        ("conv(TR=8) + pool",            ["conv(TR=8)", "maxpool"],           False, True),
        ("conv(TR=16) + conv0 + pool",   ["conv(TR=16)", "conv0", "maxpool"], True,  False),
        ("conv(TR=8) + conv0 + pool",    ["conv(TR=8)", "conv0", "maxpool"],  True,  True),
    ]:
        parts = [area[e] for e in engs]
        if any(p is None for p in parts):
            print(f"{label:<26}{'?':>7}{'?':>7}{'(csynth 미완)':>13}")
            continue
        dsp = sum(p["dsp"] for p in parts)
        verdict = "OK" if dsp <= DSP_AVAIL else f"+{dsp - DSP_AVAIL}"
        cyc, _ = conv_cycles(64, use_conv0, tr8)
        total = sum(v for v, _ in cyc.values()) + 3 * POOL_ALLOWANCE
        ms = total / CLOCK_HZ * 1e3
        src = " ".join(f"{k}:{s}" for k, (_, s) in cyc.items())
        print(f"{label:<26}{dsp:>7}{verdict:>7}{int(total):>13,}{ms:>8.2f}{1000/(ms*10):>7.1f}  {src}")

    print()
    print("출처 표기: M=실측, P=예측/외삽, M+M=실측 cyc/pos x 실측 TR 비율")
    print("⚠️ conv0 의 13.1 cyc/pos 는 **512 폭 레이어**에서 잰 값이다. 64x64 에서")
    print("   고정 오버헤드가 덜 amortize 되므로 실측은 더 나쁠 수 있다.")
    print("   `bash run.sh cosim conv0` 이 이 값을 교체한다.")
    print()
    print("⚠️ DSP 합은 csynth 합이다. Zybo 실장 기록상 DSP 는 csynth 가 정직했지만")
    print("   (v5_tr16 synth 184 vs conv csynth 156 + pool 10 + up 1 + route 14 = 181),")
    print("   LUT 는 1.4~1.5배 과대평가한다. LUT 판정은 실장으로만 할 것.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
