"""Fixed-point / requant utilities for the arty_96_classifier engine variant.

There are currently TWO PL prototypes in parallel, each maintained by a
different person, and they need separate weight sets. This is a COPY of
roi_classifier_int8_export/fixed_point.py with the arithmetic changed to match
`hls/arty_96_classifier/`; the sibling directory targets
`team_arty_96/z7_classifier_64_hls/`. Neither is "the" official one — they are
two prototypes, and both exports are maintained side by side.

  this variant (arty_96)           | sibling variant (z7_64)
  ---------------------------------|-------------------------------------
  leaky 13/128 on conv0/conv1      | plain ReLU on all three convs
  conv2 linear (no activation)     | ReLU
  round_shift, ties away from zero | bare `>>`, truncating
  saturate to [-128, 127]          | clamp to [0, 127]

Reference: conv_engine.cpp:76 / conv0_engine.cpp:47 `apply_activation()`.

    static act_t apply_activation(accum_t acc, bool leaky_relu_enable,
                                  int32_t requant_multiplier, unsigned requant_shift) {
        accum_t post_leaky = acc;
        if (leaky_relu_enable && acc < 0)
            post_leaky = (accum_t)round_shift((ap_int<64>)acc * 13, 7);
        ap_int<64> scaled = (ap_int<64>)post_leaky * (ap_int<64>)requant_multiplier;
        return saturate(round_shift(scaled, requant_shift));
    }

Note the ordering: leaky is applied to the INT32 ACCUMULATOR, before requant.
Because leaky ReLU is positively homogeneous (leaky(k*x) == k*leaky(x) for
k > 0), doing it in the integer domain is equivalent to doing it in the real
domain, so FP32 calibration ranges carry over unchanged.
"""
from __future__ import annotations

import numpy as np

LEAKY_NUM = 13
LEAKY_SHIFT = 7  # 13/128 = 0.1015625


def round_half_away_from_zero(x: np.ndarray) -> np.ndarray:
    """Offline (export-time) float->int rounding. NOT part of the PL datapath."""
    return np.where(x >= 0, np.floor(x + 0.5), np.ceil(x - 0.5))


def round_shift(x: np.ndarray, s: int) -> np.ndarray:
    """HLS round_shift() — ties away from zero, sign-symmetric.

    NOT an arithmetic right shift: for negatives it rounds the magnitude then
    reapplies the sign, so e.g. -100 with an identity multiplier stays -100
    instead of flooring to -101.
    """
    x = np.asarray(x, dtype=np.int64)
    if s == 0:
        return x
    half = np.int64(1) << np.int64(s - 1)
    magnitude = (np.abs(x) + half) >> np.int64(s)
    return np.where(x >= 0, magnitude, -magnitude)


def apply_activation(acc: np.ndarray, leaky_enable: bool, multiplier: int, shift: int) -> np.ndarray:
    """Canonical datapath: optional leaky on the accumulator, requant with
    round_shift, saturate to signed INT8 [-128, 127].

    Unlike the ReLU engine there is NO zero lower bound — negative outputs are
    normal and expected, and the next layer's scale must be chosen assuming
    they occur. conv2 (leaky_enable=False) passes negatives through completely
    uncompressed into GAP.
    """
    acc = np.asarray(acc, dtype=np.int64)
    post = acc.copy()
    if leaky_enable:
        neg = post < 0
        post[neg] = round_shift(post[neg] * LEAKY_NUM, LEAKY_SHIFT)
    scaled = post * np.int64(multiplier)
    return np.clip(round_shift(scaled, shift), -128, 127).astype(np.int8)


def derive_multiplier_shift(real_multiplier: float, max_shift: int = 62) -> tuple[int, int]:
    """M ~= multiplier / 2**shift, with the mantissa normalized into
    [2**30, 2**31) before rounding so the int32 multiplier keeps full precision."""
    if real_multiplier <= 0:
        return 0, 0
    shift = 0
    m = real_multiplier
    while m < 2**30 and shift < max_shift:
        m *= 2.0
        shift += 1
    multiplier = int(round_half_away_from_zero(np.array(m)))
    if multiplier >= 2**31:
        multiplier //= 2
        shift -= 1
    return multiplier, shift


def quantize_symmetric_int8(x: np.ndarray, signed: bool = True) -> tuple[np.ndarray, float]:
    """Per-tensor symmetric quantization, zero_point=0, so x ~= q * scale."""
    abs_max = float(np.max(np.abs(x))) if x.size else 0.0
    scale = abs_max / 127.0 if abs_max else 1.0
    q = round_half_away_from_zero(x / scale)
    lo = -127 if signed else 0
    return np.clip(q, lo, 127).astype(np.int8), scale
