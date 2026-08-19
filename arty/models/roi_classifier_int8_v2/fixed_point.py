"""Fixed-point / requant utilities shared by quantize_export.py and
golden_int8.py. Mirrors the CURRENT arty/pl HLS implementation exactly:

  - requant: acc(INT32) -> acc * multiplier(INT32), then a PLAIN arithmetic
    right shift by `shift` (64-bit intermediate to avoid overflow), with NO
    rounding term, then ReLU, then clamp to INT8 [0, 127].

History / why this matters: an earlier revision of this file rounded
half-away-from-zero ((abs(prod) + 2^(shift-1)) >> shift). The HLS does
`scaled >> shift` with no rounding at all, so the rounding version was
removed — a golden model that rounds where the hardware truncates is not
bit-exact, which defeats the entire purpose of the golden vectors.

Note on the arithmetic: `>>` on a negative int64 in both C++ and numpy floors
toward negative infinity, so truncation and floor agree there. Post-ReLU that
distinction is moot anyway (anything <= 0 clamps to 0); the behavioral
difference vs the old rounding version is on POSITIVE values, where
truncation biases results down by up to 1 LSB (mean ~0.5 LSB).

`round_half_away_from_zero` is still used, but only for OFFLINE export-time
math (quantizing weights/bias, deriving the multiplier) — that is float->int
conversion done once on the host, not the per-inference datapath the PL runs.
"""
from __future__ import annotations

import numpy as np


def round_half_away_from_zero(x: np.ndarray) -> np.ndarray:
    """Offline (export-time) float->int rounding. NOT part of the PL datapath."""
    return np.where(x >= 0, np.floor(x + 0.5), np.ceil(x - 0.5))


def requant_shift_i64(acc: np.ndarray, multiplier: int, shift: int) -> np.ndarray:
    """acc: int64 array. Returns (acc * multiplier) >> shift with a plain
    arithmetic right shift and NO rounding, matching the HLS
    (`scaled >> shift`). Still int64; caller applies ReLU/clamp/cast.
    """
    acc = acc.astype(np.int64)
    prod = acc * np.int64(multiplier)
    if shift == 0:
        return prod
    return prod >> np.int64(shift)


def derive_multiplier_shift(real_multiplier: float, max_shift: int = 62) -> tuple[int, int]:
    """Given a small positive real number M = input_scale*weight_scale/output_scale,
    find (multiplier: int32, shift: uint8) such that M ~= multiplier / 2**shift,
    maximizing precision by normalizing the mantissa into [2**30, 2**31) before
    rounding to the nearest integer (standard requant-multiplier technique).
    """
    if real_multiplier <= 0:
        return 0, 0
    shift = 0
    m = real_multiplier
    while m < 2**30 and shift < max_shift:
        m *= 2.0
        shift += 1
    multiplier = int(round_half_away_from_zero(np.array(m)))
    # guard against the (rare) rounding pushing us to 2**31 (int32 overflow)
    if multiplier >= 2**31:
        multiplier //= 2
        shift -= 1
    return multiplier, shift


def quantize_symmetric_int8(x: np.ndarray, signed: bool = True) -> tuple[np.ndarray, float]:
    """Per-tensor symmetric quantization, zero_point=0. Returns (int8 array, scale)
    such that x ~= int8_array * scale. signed=False clamps to [0,127] (for
    post-ReLU activations); signed=True clamps to [-127,127] (weights)."""
    abs_max = float(np.max(np.abs(x))) if x.size else 0.0
    if abs_max == 0.0:
        scale = 1.0
    else:
        scale = abs_max / 127.0
    q = round_half_away_from_zero(x / scale)
    lo = -127 if signed else 0
    q = np.clip(q, lo, 127).astype(np.int8)
    return q, scale
