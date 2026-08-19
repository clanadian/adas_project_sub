#!/usr/bin/env python3
"""ROI classifier cycle ESTIMATOR — **superseded. Use roi_budget.py.**

⚠️ 2026-08-18: 이 파일이 내놓은 숫자는 **전부 실측으로 교체됐습니다.**
   실제 값은 `python/roi_budget.py` (cosim 로그를 직접 읽는 집계기)를 쓰십시오.

   이 파일을 지우지 않고 남기는 이유는 **얼마나 틀렸는지가 다음 추정의
   캘리브레이션이기 때문**입니다:

       항목                 이 파일의 예측   실측      오차
       conv1 (16->32)          180,003    186,492    +3.6%
       conv2 (32->64)          113,171    125,196   +10.6%
       pool x3 (허용치 3,000)    9,000     47,592   **+429%**
       conv0 (13.1 cyc/pos)     53,657     75,264   **+40%**
       ROI 합계                355,790    434,544   **+22%**

   두 가지가 크게 틀렸습니다:
     1. maxpool 을 "데이터 이동뿐이니 무시 가능"으로 보고 3,000 사이클
        허용치를 준 것. 실제로는 위치당 26~107 사이클입니다.
     2. conv0 의 13.1 cyc/pos 를 **512 폭 레이어**에서 가져다 64x64 에
        외삽한 것. 위치가 4,096 개뿐이라 고정 오버헤드가 덜 amortize 됩니다.
        (이 리스크는 아래 MEASURED_CYC_PER_POS 주석에 미리 적혀 있었는데도
        헤드라인 숫자로는 예측치를 썼습니다.)

   **외삽은 낙관 방향으로 틀립니다.** 이 저장소에는 62% 낙관 전례도 있습니다.

   새 형상 후보를 **비교**할 때는 여전히 쓸모가 있습니다(상대 순서는 맞았습니다).
   절대값을 인용할 때만 쓰지 마십시오.

--- 원래 설명 ---

ROI classifier cycle estimator - Arty Z7-20 @100 MHz.

Compares candidate architectures using MEASURED cycles-per-position from the
22-op cosim sweep, so the training side can pick shapes before training
instead of discovering the cost after.

Read the health warning in MEASURED_CYC_PER_POS before trusting a number.

Usage:
    python3 cycle_model.py                # baseline (64x64) + 32x32 variant
    python3 cycle_model.py --roi 32
"""
import argparse

# ---------------------------------------------------------------------------
# MEASURED. Source: doc/01_status/2026-08-15_zybo-frame-measured.md, tr16
# configuration (PE_OC=16, TR=16), xc7z020clg400-1 @100 MHz, cosim, bit-exact.
# Derived as (layer cycles / output positions) from that document's table.
#
# These were measured on YOLO-sized layers (512x288 down to 32x18). Applying
# them to 64x64 assumes per-position cost is independent of image size. That
# holds for the AXI-transaction-bound part of the cost but NOT for fixed
# per-kernel overhead (weight load, pipeline fill/drain), which amortises over
# fewer positions here. **Expect the real number to be worse than this model,
# not better.** This repo has overestimated FPS by 62% once already by
# extrapolating a model instead of measuring (3.0 predicted -> 1.85 measured).
# ---------------------------------------------------------------------------
MEASURED_CYC_PER_POS = {
    (3, 16):    93.3,
    (16, 32):  155.7,
    (32, 64):  361.7,
    (64, 128): 932.5,
}

# maxpool on these tiny shapes is far below conv; measured pool cost in the
# YOLO net was 1.89M cycles for six 512x288-class pools. Scaled to <=64x64
# it is order 1e3. Held as a flat allowance rather than pretended precision.
POOL_ALLOWANCE_CYCLES = 3000

CLOCK_HZ = 100e6


def cyc_per_pos(in_ch, out_ch):
    """Return (value, is_measured)."""
    key = (in_ch, out_ch)
    if key in MEASURED_CYC_PER_POS:
        return MEASURED_CYC_PER_POS[key], True
    # Crude fallback: cost tracks out_ch tiles x in_ch reduction passes.
    # PE_OC=16, TR=16. Anchored on the (16,32) point. Clearly labelled as a
    # guess because it misses the measured points by 20-30%.
    PE_OC, TR = 16, 16
    units = -(-out_ch // PE_OC) * max(1, -(-in_ch // TR))
    anchor_units = 2
    return MEASURED_CYC_PER_POS[(16, 32)] / anchor_units * units, False


def build_ops(roi, in_ch=3, chans=(16, 32, 64)):
    """conv/pool pairs, each pool halving the edge. stride-1 convs only."""
    ops, h, c = [], roi, in_ch
    for out_c in chans:
        ops.append(("conv", h, c, out_c))
        ops.append(("pool", h, out_c, out_c))
        h //= 2
        c = out_c
    return ops, h, c


def report(roi, chans=(16, 32, 64)):
    ops, final_hw, final_ch = build_ops(roi, 3, chans)
    total, any_guess = 0, False
    print(f"\n=== ROI {roi}x{roi}, channels {chans} ===")
    print(f"{'op':<8}{'shape':<18}{'positions':>10}{'cyc/pos':>10}{'cycles':>12}  src")
    for kind, h, ic, oc in ops:
        if kind == "conv":
            pos = h * h
            cpp, measured = cyc_per_pos(ic, oc)
            cyc = int(pos * cpp)
            any_guess |= not measured
            src = "measured" if measured else "GUESS"
            print(f"{'conv':<8}{f'{h}x{h} {ic}->{oc}':<18}{pos:>10}{cpp:>10.1f}{cyc:>12,}  {src}")
        else:
            cyc = POOL_ALLOWANCE_CYCLES
            print(f"{'pool':<8}{f'{h}x{h} {ic}ch':<18}{'-':>10}{'-':>10}{cyc:>12,}  allowance")
        total += cyc

    ms = total / CLOCK_HZ * 1e3
    print(f"{'':<8}{'TOTAL per ROI':<18}{'':>10}{'':>10}{total:>12,}   = {ms:.2f} ms")
    print(f"         PS tail: GAP over {final_hw}x{final_hw}x{final_ch}, FC {final_ch}x5, softmax")
    print(f"         ROI throughput: {1000/ms:.0f} ROI/s")
    print("         frame FPS if FPGA is the only bottleneck:")
    for n in (5, 10, 20):
        print(f"           {n:>2} ROI/frame -> {1000/(ms*n):5.1f} FPS")
    if any_guess:
        print("  !! contains GUESS rows - cosim before quoting")
    return total


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--roi", type=int, default=None)
    ap.add_argument("--chans", type=str, default="16,32,64")
    a = ap.parse_args()
    chans = tuple(int(x) for x in a.chans.split(","))
    if a.roi:
        report(a.roi, chans)
    else:
        report(64, chans)
        report(32, chans)
        print("\nAll figures are PREDICTIONS extrapolated from measured cyc/pos.")
        print("Replace with cosim of the real shapes before using them anywhere.")
