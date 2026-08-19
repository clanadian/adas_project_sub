#!/usr/bin/env python3
"""Pure-integer bit-exact golden model — the actual hardware arithmetic
(int8 x int8 -> int32 accumulate, +int32 bias, int64 requant multiply-shift,
ReLU, int8 clamp), not a float approximation. Used to (a) produce the
layer-by-layer golden input/output vectors for cross-checking the HLS/PL
implementation, and (b) sanity-check that quantization didn't wreck accuracy
by comparing golden argmax against the FP32 model on real validation crops.

Conv0 consumes the PS-pre-padded 98x98x3 INT8 input directly (no extra
padding here). Conv1/conv2 pad by 1 internally (they have a padding port,
per prior PL contract notes; conv0 doesn't, hence the external border).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import cv2
import numpy as np

sys.path.insert(0, "/mnt/d/fpga_project/roi_classifier_fp32")
from classes import CLASSES  # noqa: E402
from model import RoiClassifier  # noqa: E402
from roi_dataset import Box, make_roi  # noqa: E402
import torch  # noqa: E402

from fixed_point import requant_shift_i64, round_half_away_from_zero

EXPORT_DIR = Path("/mnt/d/fpga_project/roi_classifier_int8_export/export")


def load_export() -> dict:
    manifest = json.loads((EXPORT_DIR / "manifest.json").read_text())
    layers = {}
    for name, ky, kx in [("conv0", 3, 3), ("conv1", 3, 3), ("conv2", 3, 3)]:
        L = manifest["layers"][name]
        oc, ic = {"conv0": (16, 3), "conv1": (32, 16), "conv2": (64, 32)}[name]
        w_raw = np.frombuffer((EXPORT_DIR / L["weight_file"]).read_bytes(), dtype=np.int8)
        if name == "conv0":
            w = w_raw.reshape(oc, ic, ky, kx)  # OIHW
        else:
            w_wpack = w_raw.reshape(oc, ky, kx, ic)  # WPACK
            w = np.transpose(w_wpack, (0, 3, 1, 2)).copy()  # back to OIHW for numpy conv below
        b = np.frombuffer((EXPORT_DIR / L["bias_file"]).read_bytes(), dtype=np.int32)
        layers[name] = {"weight": w, "bias": b, "multiplier": L["requant_multiplier"], "shift": L["requant_shift"]}

    n_classes = len(CLASSES)
    fc_w = np.frombuffer((EXPORT_DIR / "fc_weight.bin").read_bytes(), dtype=np.int8).reshape(n_classes, 64)
    fc_b = np.frombuffer((EXPORT_DIR / "fc_bias.bin").read_bytes(), dtype=np.int32)
    return {"layers": layers, "fc_weight": fc_w, "fc_bias": fc_b, "manifest": manifest}


def int8_conv3x3(x_int8: np.ndarray, w_int8: np.ndarray, b_int32: np.ndarray, pad: int) -> np.ndarray:
    """x_int8: (H,W,Cin) int8 NHWC (no batch). w_int8: (Cout,Cin,3,3) OIHW.
    Returns int32 accumulator (H',W',Cout) — NOT yet requantized."""
    if pad:
        x_int8 = np.pad(x_int8, ((pad, pad), (pad, pad), (0, 0)), mode="constant", constant_values=0)
    H, W, Cin = x_int8.shape
    Cout = w_int8.shape[0]
    Ho, Wo = H - 2, W - 2  # valid 3x3, since padding (if any) already applied
    x32 = x_int8.astype(np.int64)
    w32 = w_int8.astype(np.int64)
    acc = np.zeros((Ho, Wo, Cout), dtype=np.int64)
    for ky in range(3):
        for kx in range(3):
            patch = x32[ky:ky + Ho, kx:kx + Wo, :]  # (Ho,Wo,Cin)
            wk = w32[:, :, ky, kx]  # (Cout,Cin)
            acc += patch @ wk.T
    acc += b_int32.astype(np.int64)[None, None, :]
    return acc


def requant_relu_clamp(acc_int64: np.ndarray, multiplier: int, shift: int) -> np.ndarray:
    out = requant_shift_i64(acc_int64, multiplier, shift)  # plain >> , no rounding (matches HLS)
    out = np.clip(out, 0, 127)  # ReLU (clip<0 to 0) folded into the lower clamp bound
    return out.astype(np.int8)


def maxpool2x2(x: np.ndarray) -> np.ndarray:
    H, W, C = x.shape
    x4 = x.reshape(H // 2, 2, W // 2, 2, C)
    return x4.max(axis=(1, 3))


def quantize_input_96(img_rgb_u8: np.ndarray) -> np.ndarray:
    """96x96x3 uint8 RGB -> 98x98x3 signed INT8 NHWC (PS pre-padding + symmetric quant)."""
    q = round_half_away_from_zero(img_rgb_u8.astype(np.float64) * 127.0 / 255.0)
    q = np.clip(q, 0, 127).astype(np.int8)
    return np.pad(q, ((1, 1), (1, 1), (0, 0)), mode="constant", constant_values=0)


def run_golden(export: dict, img_rgb_u8: np.ndarray) -> dict:
    trace = {}
    x = quantize_input_96(img_rgb_u8)
    trace["input_98x98x3_int8"] = x

    cur = x
    for name, pad in [("conv0", 0), ("conv1", 1), ("conv2", 1)]:
        L = export["layers"][name]
        acc = int8_conv3x3(cur, L["weight"], L["bias"], pad=pad)
        out = requant_relu_clamp(acc, L["multiplier"], L["shift"])
        trace[f"{name}_out"] = out
        pooled = maxpool2x2(out)
        trace[f"{name}_pool"] = pooled
        cur = pooled

    # cur is now 12x12x64 int8 == PL final output
    gap_sum = cur.astype(np.int64).sum(axis=(0, 1))  # INT32-range sum per channel, NOT divided
    trace["gap_sum_int32"] = gap_sum.astype(np.int32)

    fc_w = export["fc_weight"].astype(np.int64)  # (N,64)
    fc_b = export["fc_bias"].astype(np.int64)
    logits = gap_sum[None, :] @ fc_w.T + fc_b[None, :]
    logits = logits[0]
    trace["logits_int32"] = logits.astype(np.int32)
    trace["class_id"] = int(np.argmax(logits))
    trace["class_name"] = CLASSES[trace["class_id"]]
    return trace


def compare_against_fp32(export: dict, checkpoint: Path, manifest_csv: Path, n: int = 300) -> dict:
    """Argmax agreement between the INT8 golden model and the original FP32
    model, on real validation crops — the real sanity check that quantization
    didn't break the model."""
    import csv

    model = RoiClassifier(num_classes=len(CLASSES), activations=("relu", "relu", "relu"))
    model.load_state_dict(torch.load(checkpoint, map_location="cpu"))
    model.eval()

    rows = list(csv.DictReader(manifest_csv.open("r", encoding="utf-8")))
    step = max(1, len(rows) // n)
    rows = rows[::step][:n]

    agree = 0
    int8_correct = 0
    fp32_correct = 0
    total = 0
    for row in rows:
        img_bgr = cv2.imread(row["image_path"])
        if img_bgr is None:
            continue
        img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
        # image_path is the FULL source image, not a pre-cropped ROI — apply
        # the exact same canonical crop runtime_dataset.py uses for val/test
        # (margin 0.15 for positive boxes, 0.0 for background boxes which are
        # already exact squares; square expand, black pad, INTER_LINEAR resize).
        box = Box(float(row["x1"]), float(row["y1"]), float(row["x2"]), float(row["y2"]))
        margin = 0.0 if row["is_background"] == "1" else 0.15
        img_rgb = make_roi(img_rgb, box, margin=margin, out_size=96)
        true_class = row["class_name"]

        golden = run_golden(export, img_rgb)

        with torch.no_grad():
            t = torch.from_numpy(img_rgb.astype(np.float32) / 255.0).permute(2, 0, 1).unsqueeze(0)
            fp32_pred = int(model(t).argmax(dim=1).item())

        agree += int(golden["class_id"] == fp32_pred)
        int8_correct += int(golden["class_name"] == true_class)
        fp32_correct += int(CLASSES[fp32_pred] == true_class)
        total += 1

    return {
        "n": total,
        "int8_vs_fp32_agree": agree, "int8_vs_fp32_agreement_rate": agree / total if total else 0.0,
        "int8_vs_ground_truth_accuracy": int8_correct / total if total else 0.0,
        "fp32_vs_ground_truth_accuracy": fp32_correct / total if total else 0.0,
    }


def main() -> None:
    import csv

    export = load_export()

    val_manifest = Path("/home/user/fpga_roi_classifier_data/dataset/val_manifest.csv")
    with val_manifest.open("r", encoding="utf-8") as f:
        sample_row = next(r for r in csv.DictReader(f) if r["class_name"] == "person")

    sample_path = sample_row["image_path"]
    img_bgr = cv2.imread(sample_path)
    img_rgb = cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB)
    box = Box(float(sample_row["x1"]), float(sample_row["y1"]), float(sample_row["x2"]), float(sample_row["y2"]))
    img_rgb = make_roi(img_rgb, box, margin=0.15, out_size=96)

    golden = run_golden(export, img_rgb)
    print(f"golden class: {golden['class_name']} (id={golden['class_id']})")
    print("logits:", golden["logits_int32"])

    golden_out = {
        "sample_image": sample_path,
        "sample_box_xyxy": [box.x1, box.y1, box.x2, box.y2],
        "sample_true_class": sample_row["class_name"],
        "input_98x98x3_int8_shape": list(golden["input_98x98x3_int8"].shape),
        "conv0_out_shape": list(golden["conv0_out"].shape),
        "pool0_out_shape": list(golden["conv0_pool"].shape),
        "conv1_out_shape": list(golden["conv1_out"].shape),
        "pool1_out_shape": list(golden["conv1_pool"].shape),
        "conv2_out_shape": list(golden["conv2_out"].shape),
        "pool2_final_shape": list(golden["conv2_pool"].shape),
        "gap_sum_int32": golden["gap_sum_int32"].tolist(),
        "logits_int32": golden["logits_int32"].tolist(),
        "class_id": golden["class_id"],
        "class_name": golden["class_name"],
    }
    (EXPORT_DIR / "golden_vector.json").write_text(json.dumps(golden_out, indent=2))
    for key in ["input_98x98x3_int8", "conv0_out", "conv0_pool", "conv1_out", "conv1_pool", "conv2_out", "conv2_pool"]:
        np.save(EXPORT_DIR / f"golden_{key}.npy", golden[key])
    print(f"wrote golden vectors to {EXPORT_DIR}")

    checkpoint = Path(export["manifest"]["checkpoint"])
    val_manifest = Path("/home/user/fpga_roi_classifier_data/dataset/val_manifest.csv")
    print("comparing INT8 golden argmax vs FP32 model argmax on 300 real val crops...")
    result = compare_against_fp32(export, checkpoint, val_manifest, n=300)
    print(json.dumps(result, indent=2))
    (EXPORT_DIR / "int8_vs_fp32_agreement.json").write_text(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
