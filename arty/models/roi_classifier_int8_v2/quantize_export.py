#!/usr/bin/env python3
"""INT8 export for the Arty Z7-20 ROI classifier — 96 HLS PL contract
(arty/pl/hls/HW/classifier_engine.*):

  conv0: 3->16, 3x3, ReLU, maxpool     (weight OIHW, no transpose)
  conv1: 16->32, 3x3, ReLU, maxpool    (weight WPACK = OIHW.transpose(0,2,3,1))
  conv2: 32->64, 3x3, ReLU, maxpool    (weight WPACK, same)
  PL output: 12x12x64 signed INT8 NHWC
  bias: INT32. requant: acc*multiplier >> shift, ReLU, INT8 clamp.
  GAP+FC+argmax run on PS.

Reads (read-only) the trained checkpoint and model architecture from
roi_classifier_fp32/ — imported, not duplicated, because getting the
architecture even slightly wrong here would silently produce a wrong
quantized model. Never writes anything under roi_classifier_fp32/.

Pipeline: load FP32 checkpoint -> fuse BatchNorm into each conv's weight/bias
-> calibrate activation ranges on real validation crops -> quantize weights
(symmetric per-tensor, zero-point 0) -> derive per-conv requant
multiplier/shift -> quantize FC -> write all .bin files + manifest.

Usage:
    yolo_env/bin/python roi_classifier_int8_export/quantize_export.py
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import torch

sys.path.insert(0, "/mnt/d/fpga_project/roi_classifier_fp32")
from classes import CLASSES  # noqa: E402
from model import RoiClassifier  # noqa: E402
from runtime_dataset import RoiManifestDataset  # noqa: E402

from fixed_point import derive_multiplier_shift, quantize_symmetric_int8, round_half_away_from_zero

CHECKPOINT = Path("/home/user/fpga_roi_classifier_data/runs/yolo_transfer_r96/best.pt")
VAL_MANIFEST = Path("/home/user/fpga_roi_classifier_data/dataset/val_manifest.csv")
OUT_DIR = Path("/mnt/d/fpga_project/roi_classifier_int8_export/export")
N_CALIBRATION = 512
GAP_SIZE = 12 * 12  # PL output spatial size at 96x96 input: 96/8=12 per side


def fuse_bn(conv_weight: torch.Tensor, bn) -> tuple[np.ndarray, np.ndarray]:
    """Conv(bias=False) + BatchNorm2d -> equivalent Conv weight + bias.
    W' = W * gamma / sqrt(var+eps) (per out-channel)
    b' = beta - gamma * mean / sqrt(var+eps)
    """
    gamma = bn.weight.detach().numpy()
    beta = bn.bias.detach().numpy()
    mean = bn.running_mean.detach().numpy()
    var = bn.running_var.detach().numpy()
    eps = bn.eps
    std = np.sqrt(var + eps)
    w = conv_weight.detach().numpy()  # (oc, ic, k, k)
    w_fused = w * (gamma / std)[:, None, None, None]
    b_fused = beta - gamma * mean / std
    return w_fused.astype(np.float32), b_fused.astype(np.float32)


def load_fused_params(checkpoint: Path) -> dict:
    model = RoiClassifier(num_classes=len(CLASSES), activations=("relu", "relu", "relu"))
    model.load_state_dict(torch.load(checkpoint, map_location="cpu"))
    model.eval()

    params = {}
    for name, block in [("conv0", model.conv0), ("conv1", model.conv1), ("conv2", model.conv2)]:
        w, b = fuse_bn(block.conv.weight, block.bn)
        params[name] = {"weight": w, "bias": b}
    params["fc"] = {
        "weight": model.fc.weight.detach().numpy().astype(np.float32),  # (N, 64)
        "bias": model.fc.bias.detach().numpy().astype(np.float32),  # (N,)
    }
    return params


def fused_forward_numpy(params: dict, x: np.ndarray) -> dict:
    """FP32 forward pass using the FUSED (BN-folded) weights, pure numpy via
    torch's conv2d (float, not quantized) — used only for calibration
    (collecting activation ranges), not for the final golden model."""
    import torch.nn.functional as F

    t = torch.from_numpy(x)
    activations = {}
    cur = t
    for name in ("conv0", "conv1", "conv2"):
        w = torch.from_numpy(params[name]["weight"])
        b = torch.from_numpy(params[name]["bias"])
        cur = F.conv2d(cur, w, b, stride=1, padding=1)
        cur = F.relu(cur)
        activations[f"{name}_out"] = cur.numpy().copy()
        cur = F.max_pool2d(cur, 2, 2)
        activations[f"{name}_pool"] = cur.numpy().copy()
    gap = cur.mean(dim=(2, 3))
    logits = F.linear(gap, torch.from_numpy(params["fc"]["weight"]), torch.from_numpy(params["fc"]["bias"]))
    activations["gap"] = gap.numpy().copy()
    activations["logits"] = logits.numpy().copy()
    return activations


def calibrate(params: dict, n: int) -> dict:
    """Runs n real validation crops through the fused FP32 model, tracking
    abs-max at each activation stage. Input scale is NOT calibrated — it's
    fixed by the manifest's input quantization convention (pixel/255 -> INT8
    via scale=1/127), so conv0's input activation range is exactly [0,127]."""
    ds = RoiManifestDataset(str(VAL_MANIFEST), "val", roi_size=96)
    idx = np.linspace(0, len(ds) - 1, num=min(n, len(ds)), dtype=int)

    abs_max = {"conv0_out": 0.0, "conv1_out": 0.0, "conv2_out": 0.0}
    for i in idx:
        tensor, _label = ds[int(i)]
        x = tensor.unsqueeze(0).numpy()
        acts = fused_forward_numpy(params, x)
        for k in abs_max:
            abs_max[k] = max(abs_max[k], float(np.max(np.abs(acts[k]))))
    return abs_max


def quantize_conv(name: str, w_fp32: np.ndarray, b_fp32: np.ndarray, input_scale: float, output_scale: float) -> dict:
    w_int8, w_scale = quantize_symmetric_int8(w_fp32, signed=True)
    bias_scale = input_scale * w_scale
    b_int32 = round_half_away_from_zero(b_fp32 / bias_scale).astype(np.int64)
    real_multiplier = (input_scale * w_scale) / output_scale
    multiplier, shift = derive_multiplier_shift(real_multiplier)

    # ROUNDING COMPENSATION (see README "requant rounding"):
    # The HLS requant is a bare `(acc * multiplier) >> shift` with no rounding
    # term, which truncates toward -inf and costs ~0.5 LSB per layer. Measured
    # end-to-end that was a 20pp accuracy loss (94.3 FP32 -> 90.3 rounded ->
    # 69.7 truncated). Adding a rounding term `2^(shift-1)` to the PRODUCT is
    # algebraically identical to adding `2^(shift-1)/multiplier` to the
    # ACCUMULATOR — and bias lands in the accumulator before requant, so we can
    # fold it into b_int32 here at export time. The HLS datapath stays byte-for-
    # byte unchanged and still produces bit-exact matches against our golden.
    rounding_compensation = int(round((2 ** (shift - 1)) / multiplier))
    b_int32 = (b_int32 + rounding_compensation).astype(np.int32)

    return {
        "weight_int8": w_int8, "weight_scale": w_scale,
        "bias_int32": b_int32, "bias_scale": bias_scale,
        "rounding_compensation": rounding_compensation,
        "input_scale": input_scale, "output_scale": output_scale,
        "requant_multiplier": multiplier, "requant_shift": shift,
    }


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    params = load_fused_params(CHECKPOINT)

    print(f"calibrating on {N_CALIBRATION} real val crops...")
    abs_max = calibrate(params, N_CALIBRATION)
    print("activation abs-max:", abs_max)

    INPUT_SCALE = 1.0 / 127.0  # fixed by the pixel/255 -> INT8[0,127] convention (manifest.json)

    conv0 = quantize_conv("conv0", params["conv0"]["weight"], params["conv0"]["bias"],
                           input_scale=INPUT_SCALE, output_scale=abs_max["conv0_out"] / 127.0)
    conv1 = quantize_conv("conv1", params["conv1"]["weight"], params["conv1"]["bias"],
                           input_scale=conv0["output_scale"], output_scale=abs_max["conv1_out"] / 127.0)
    conv2 = quantize_conv("conv2", params["conv2"]["weight"], params["conv2"]["bias"],
                           input_scale=conv1["output_scale"], output_scale=abs_max["conv2_out"] / 127.0)

    # WPACK for conv1/conv2: OIHW -> [oc][ky][kx][ic], done HERE (exporter),
    # PS copies the bytes straight to DDR with no transpose of its own.
    conv0_wire = conv0["weight_int8"]  # OIHW, no transpose
    conv1_wire = np.transpose(conv1["weight_int8"], (0, 2, 3, 1)).copy()  # WPACK
    conv2_wire = np.transpose(conv2["weight_int8"], (0, 2, 3, 1)).copy()  # WPACK

    (OUT_DIR / "w_conv0.bin").write_bytes(conv0_wire.tobytes())
    (OUT_DIR / "w_conv1.bin").write_bytes(conv1_wire.tobytes())
    (OUT_DIR / "w_conv2.bin").write_bytes(conv2_wire.tobytes())
    (OUT_DIR / "b_conv0.bin").write_bytes(conv0["bias_int32"].tobytes())
    (OUT_DIR / "b_conv1.bin").write_bytes(conv1["bias_int32"].tobytes())
    (OUT_DIR / "b_conv2.bin").write_bytes(conv2["bias_int32"].tobytes())

    # GAP: PL outputs 12x12x64 INT8; PS sums (NOT averages) the 144 spatial
    # positions per channel into an INT32 accumulator. The mathematical 1/144
    # is NOT applied on PS as a division — it's folded into the FC weight's
    # effective scale below, same pattern as the original 64x64 contract (just
    # 144 instead of 64, since PL output here is 12x12 not 8x8).
    gap_output_scale = conv2["output_scale"] / GAP_SIZE

    fc_w_fp32 = params["fc"]["weight"]  # (N, 64)
    fc_b_fp32 = params["fc"]["bias"]
    fc_w_int8, fc_w_scale = quantize_symmetric_int8(fc_w_fp32, signed=True)
    fc_bias_scale = gap_output_scale * fc_w_scale
    fc_b_int32 = round_half_away_from_zero(fc_b_fp32 / fc_bias_scale).astype(np.int32)
    logits_scale = fc_bias_scale  # real_logit = int32_logit * logits_scale; NOT needed for argmax itself

    (OUT_DIR / "fc_weight.bin").write_bytes(fc_w_int8.tobytes())  # INT8 [N][64]
    (OUT_DIR / "fc_bias.bin").write_bytes(fc_b_int32.tobytes())  # INT32 [N]

    manifest = {
        "pl_contract_ref": "arty/pl/hls/HW/classifier_engine.*",
        "checkpoint": str(CHECKPOINT),
        "classes": CLASSES,
        "input": {
            "logical_roi": "96x96x3 RGB UINT8",
            "ps_preprocess": "symmetric INT8, zero_point=0, then 1px zero border -> 98x98x3 signed INT8 NHWC to PL",
            "quant_formula": "q_int8 = clamp(round(pixel_u8 * 127 / 255), 0, 127)",
            "input_scale": INPUT_SCALE,
        },
        "layers": {
            "conv0": {
                "weight_file": "w_conv0.bin", "weight_layout": "OIHW [16][3][3][3], no transpose",
                "bias_file": "b_conv0.bin", "bias_dtype": "INT32[16]",
                "weight_scale": conv0["weight_scale"], "input_scale": INPUT_SCALE, "output_scale": conv0["output_scale"],
                "requant_multiplier": conv0["requant_multiplier"], "requant_shift": conv0["requant_shift"],
                "bias_includes_rounding_compensation": conv0["rounding_compensation"],
            },
            "conv1": {
                "weight_file": "w_conv1.bin", "weight_layout": "WPACK [32][3][3][16] = OIHW.transpose(0,2,3,1)",
                "bias_file": "b_conv1.bin", "bias_dtype": "INT32[32]",
                "weight_scale": conv1["weight_scale"], "input_scale": conv1["input_scale"], "output_scale": conv1["output_scale"],
                "requant_multiplier": conv1["requant_multiplier"], "requant_shift": conv1["requant_shift"],
                "bias_includes_rounding_compensation": conv1["rounding_compensation"],
            },
            "conv2": {
                "weight_file": "w_conv2.bin", "weight_layout": "WPACK [64][3][3][32] = OIHW.transpose(0,2,3,1)",
                "bias_file": "b_conv2.bin", "bias_dtype": "INT32[64]",
                "weight_scale": conv2["weight_scale"], "input_scale": conv2["input_scale"], "output_scale": conv2["output_scale"],
                "requant_multiplier": conv2["requant_multiplier"], "requant_shift": conv2["requant_shift"],
                "bias_includes_rounding_compensation": conv2["rounding_compensation"],
                "pl_final_output": "12x12x64 signed INT8 NHWC",
            },
        },
        "requant_formula": "out = clamp((acc_int64 * multiplier) >> shift, 0, 127)  [ReLU folded into the 0 lower clamp]",
        "rounding": "NONE — plain arithmetic right shift, no rounding term, matching the arty/pl HLS (`scaled >> shift`). The multiply must use a 64-bit intermediate.",
        "gap": {
            "mode": "sum (NOT mean) over the 12x12=144 spatial positions per channel, INT32 accumulator",
            "divisor": GAP_SIZE,
            "divisor_folded_into_fc": True,
            "note": "PS does NOT divide by 144 anywhere — the 1/144 is baked into fc_bias_scale / logits_scale below. Do not apply it twice.",
        },
        "fc": {
            "weight_file": "fc_weight.bin", "weight_layout": "INT8 [N][64], N=len(classes)=6",
            "bias_file": "fc_bias.bin", "bias_dtype": "INT32[6]",
            "weight_scale": fc_w_scale,
            "gap_output_scale": gap_output_scale,
            "logits_scale": logits_scale,
            "note": "argmax(logits_int32) directly gives the correct class — logits_scale is only needed if a real-valued confidence/softmax is wanted, not for classification itself (a single shared scale across all N outputs preserves argmax ordering).",
        },
    }
    (OUT_DIR / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(f"wrote export files to {OUT_DIR}")
    print(json.dumps({k: v for k, v in manifest["layers"].items()}, indent=2))


if __name__ == "__main__":
    main()
