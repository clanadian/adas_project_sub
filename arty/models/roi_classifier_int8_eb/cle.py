"""Cross-Layer Equalization (CLE) for per-tensor INT8 quantization.

Problem this solves: the PL mandates symmetric PER-TENSOR weight scales (no
per-channel). Measured on the leaky fine-tuned checkpoint, conv0's per-output-
channel weight magnitudes span 24x (0.69 .. 16.68). A single tensor-wide scale
is set by the largest channel, so the smallest channel gets ~5 of 127 levels
and is effectively destroyed. Isolating this showed weight quantization alone
costing 93.5% -> 75.5%, far more than activation quantization or calibration.

Fix: for a positively-homogeneous activation f (f(k*x) == k*f(x) for k > 0),
scaling output channel i of layer L by 1/s_i and input channel i of layer L+1
by s_i leaves the network's function EXACTLY unchanged while letting us
equalize the per-channel ranges. Both activations here qualify:
    leaky ReLU:  leaky(k*x) = k*leaky(x)   for k > 0
    identity  :  trivially homogeneous
MaxPool also commutes with positive scaling (max(k*x) = k*max(x)), and GAP is
linear, so the whole conv->act->pool->conv chain is safe.

Reference: Nagel et al., "Data-Free Quantization Through Weight Equalization
and Bias Correction" (ICCV 2019), section 3.

This is an EXPORT-SIDE transform only. The hardware sees the same file
layouts, the same dtypes, the same op sequence — only the numbers differ.
"""
from __future__ import annotations

import numpy as np


def _out_channel_ranges(w: np.ndarray) -> np.ndarray:
    """w: (out, in, kh, kw) -> per-output-channel max |w|."""
    return np.abs(w).reshape(w.shape[0], -1).max(axis=1)


def _in_channel_ranges(w: np.ndarray) -> np.ndarray:
    """w: (out, in, kh, kw) -> per-INPUT-channel max |w|."""
    return np.abs(w).transpose(1, 0, 2, 3).reshape(w.shape[1], -1).max(axis=1)


def equalize_pair(
    w1: np.ndarray, b1: np.ndarray, w2: np.ndarray, eps: float = 1e-8
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Equalize output channels of layer1 against input channels of layer2.

    Returns (w1_new, b1_new, w2_new, scales). The composed function is
    unchanged: layer1's channel i is divided by s_i, layer2's matching input
    channel is multiplied by s_i.

    s_i = sqrt(r1_i / r2_i), the choice that lands BOTH resulting ranges on
    the geometric mean sqrt(r1_i * r2_i):
        r1_new = r1 / s = r1 / sqrt(r1/r2) = sqrt(r1 * r2)   ✓
        r2_new = r2 * s = r2 * sqrt(r1/r2) = sqrt(r1 * r2)   ✓
    (Dividing by r1 instead of r2 here inverts the correction and blows the
    spread up instead of collapsing it — verified the hard way.)
    """
    r1 = _out_channel_ranges(w1)
    r2 = _in_channel_ranges(w2)

    # Channels that are entirely zero on either side carry no signal; leave
    # them at scale 1 instead of dividing by ~0 and manufacturing huge values.
    valid = (r1 > eps) & (r2 > eps)
    s = np.ones_like(r1)
    s[valid] = np.sqrt(r1[valid] / r2[valid])

    w1_new = w1 / s[:, None, None, None]
    b1_new = b1 / s
    w2_new = w2 * s[None, :, None, None]
    return w1_new.astype(np.float32), b1_new.astype(np.float32), w2_new.astype(np.float32), s


def equalize_conv_to_fc(
    w_conv: np.ndarray, b_conv: np.ndarray, w_fc: np.ndarray, eps: float = 1e-8
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    """Same idea for the last conv feeding GAP->FC. GAP is a positive-weighted
    linear op, so it commutes with per-channel positive scaling; the FC's
    matching input column absorbs the inverse. w_fc: (n_classes, channels)."""
    r1 = _out_channel_ranges(w_conv)
    r2 = np.abs(w_fc).max(axis=0)  # per input-channel (column) range

    valid = (r1 > eps) & (r2 > eps)
    s = np.ones_like(r1)
    s[valid] = np.sqrt(r1[valid] / r2[valid])

    w_conv_new = w_conv / s[:, None, None, None]
    b_conv_new = b_conv / s
    w_fc_new = w_fc * s[None, :]
    return w_conv_new.astype(np.float32), b_conv_new.astype(np.float32), w_fc_new.astype(np.float32), s


def apply_cle(params: dict, iterations: int = 3) -> dict:
    """Run CLE over conv0->conv1->conv2->fc. Iterating helps because
    equalizing one pair perturbs the next pair's ranges; it converges quickly.

    `params` is the fused {layer: {"weight","bias"}} dict from
    quantize_export.load_fused_params(). Returns a NEW dict; the input is not
    mutated.
    """
    p = {k: {kk: vv.copy() for kk, vv in v.items()} for k, v in params.items()}

    for _ in range(iterations):
        p["conv0"]["weight"], p["conv0"]["bias"], p["conv1"]["weight"], _ = equalize_pair(
            p["conv0"]["weight"], p["conv0"]["bias"], p["conv1"]["weight"]
        )
        p["conv1"]["weight"], p["conv1"]["bias"], p["conv2"]["weight"], _ = equalize_pair(
            p["conv1"]["weight"], p["conv1"]["bias"], p["conv2"]["weight"]
        )
        p["conv2"]["weight"], p["conv2"]["bias"], p["fc"]["weight"], _ = equalize_conv_to_fc(
            p["conv2"]["weight"], p["conv2"]["bias"], p["fc"]["weight"]
        )
    return p


def channel_spread(w: np.ndarray) -> float:
    """max/min of per-output-channel |w| ranges — the number CLE is reducing."""
    r = _out_channel_ranges(w)
    r = r[r > 0]
    return float(r.max() / r.min()) if r.size else 1.0
