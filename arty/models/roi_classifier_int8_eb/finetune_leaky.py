#!/usr/bin/env python3
"""Fine-tune the FP32 ROI classifier from ReLU to leaky-ReLU(13/128) so it
matches the arty_96_classifier engine variant's activation contract.

Why this exists: there are two PL prototypes in parallel, each needing its own
weight set. The first INT8 export targeted the z7_classifier_64_hls variant,
whose activation is plain ReLU. This variant (hls/arty_96_classifier/) has no
ReLU path at all — conv0/conv1 use leaky 13/128, conv2 is linear, and outputs
saturate to [-128,127] rather than [0,127]. Weights trained under ReLU can't
be dropped into that datapath, so we fine-tune briefly under the matching
activation and re-quantize. The ReLU export stays as-is for the other variant.

Starts from the existing yolo_transfer ReLU checkpoint rather than training
from scratch — the conv filters are mostly reusable, only the negative-side
behavior changes.

Reads roi_classifier_fp32/ read-only (imports the model + dataset); writes
only into this directory's own output path.

Usage:
    yolo_env/bin/python roi_classifier_int8_export_arty96/finetune_leaky.py --epochs 3
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
from torch.utils.data import DataLoader

sys.path.insert(0, "/mnt/d/fpga_project/roi_classifier_fp32")
from classes import CLASSES  # noqa: E402
from model import ACTIVATION_PRESETS, RoiClassifier  # noqa: E402
from runtime_dataset import RoiManifestDataset  # noqa: E402

DATASET = Path("/home/user/fpga_roi_classifier_data/dataset")
SOURCE_CKPT = Path("/home/user/fpga_roi_classifier_data/runs/yolo_transfer_r96/best.pt")
OUT_DIR = Path("/home/user/fpga_roi_classifier_data/runs/leaky_finetune_r96")

# arty_96 variant: conv0/conv1 leaky 13/128, conv2 linear. This is exactly
# roi_classifier_fp32's "legacy_leaky" preset — the activation spec from the
# original request doc, which matches this PL prototype.
ARTY96_ACTIVATIONS = ACTIVATION_PRESETS["legacy_leaky"]  # ("leaky","leaky","none")


@torch.no_grad()
def evaluate(model, loader, device):
    model.eval()
    n_classes = len(CLASSES)
    confusion = np.zeros((n_classes, n_classes), dtype=np.int64)
    correct = total = 0
    for images, labels in loader:
        images, labels = images.to(device), labels.to(device)
        preds = model(images).argmax(dim=1)
        correct += (preds == labels).sum().item()
        total += labels.numel()
        for t, p in zip(labels.cpu().numpy(), preds.cpu().numpy()):
            confusion[t, p] += 1
    return (correct / total if total else 0.0), confusion


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--epochs", type=int, default=3)
    ap.add_argument("--lr", type=float, default=3e-4, help="lower than initial training — this is a fine-tune")
    ap.add_argument("--batch-size", type=int, default=128)
    ap.add_argument("--num-workers", type=int, default=4)
    ap.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    ap.add_argument("--out", type=Path, default=OUT_DIR)
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)

    model = RoiClassifier(num_classes=len(CLASSES), activations=ARTY96_ACTIVATIONS)
    state = torch.load(SOURCE_CKPT, map_location="cpu")
    model.load_state_dict(state)  # architecture is identical; only nn.LeakyReLU vs nn.ReLU differ (no params)
    model = model.to(args.device)
    print(f"loaded {SOURCE_CKPT} and switched activations to {ARTY96_ACTIVATIONS}")

    ds_kw = dict(roi_size=96, margin=0.15, augment_prob=0.5)
    train_ds = RoiManifestDataset(DATASET / "train_manifest.csv", "train", **ds_kw)
    val_ds = RoiManifestDataset(DATASET / "val_manifest.csv", "val", **ds_kw)
    test_ds = RoiManifestDataset(DATASET / "test_manifest.csv", "test", **ds_kw)
    train_loader = DataLoader(train_ds, batch_size=args.batch_size, shuffle=True, num_workers=args.num_workers)
    val_loader = DataLoader(val_ds, batch_size=args.batch_size, shuffle=False, num_workers=args.num_workers)
    test_loader = DataLoader(test_ds, batch_size=args.batch_size, shuffle=False, num_workers=args.num_workers)

    base_acc, _ = evaluate(model, val_loader, args.device)
    print(f"val_acc BEFORE fine-tune (ReLU weights running under leaky): {base_acc:.4f}")

    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    criterion = nn.CrossEntropyLoss()

    best_val = base_acc
    torch.save(model.state_dict(), args.out / "best.pt")
    history = [{"epoch": 0, "val_acc": base_acc, "note": "before fine-tune"}]

    for epoch in range(1, args.epochs + 1):
        model.train()
        running = 0.0
        for images, labels in train_loader:
            images, labels = images.to(args.device), labels.to(args.device)
            optimizer.zero_grad()
            loss = criterion(model(images), labels)
            loss.backward()
            optimizer.step()
            running += loss.item() * images.size(0)
        train_loss = running / max(len(train_ds), 1)
        val_acc, _ = evaluate(model, val_loader, args.device)
        history.append({"epoch": epoch, "train_loss": train_loss, "val_acc": val_acc})
        print(f"epoch {epoch}  train_loss={train_loss:.4f}  val_acc={val_acc:.4f}")
        torch.save(model.state_dict(), args.out / "last.pt")
        if val_acc > best_val:
            best_val = val_acc
            torch.save(model.state_dict(), args.out / "best.pt")

    model.load_state_dict(torch.load(args.out / "best.pt", map_location=args.device))
    test_acc, confusion = evaluate(model, test_loader, args.device)
    result = {
        "activations": list(ARTY96_ACTIVATIONS),
        "source_checkpoint": str(SOURCE_CKPT),
        "val_acc_before_finetune": base_acc,
        "best_val_acc": best_val,
        "test_acc": test_acc,
        "confusion_matrix": confusion.tolist(),
        "classes": CLASSES,
        "history": history,
    }
    (args.out / "finetune_result.json").write_text(json.dumps(result, indent=2))
    print(f"best_val_acc={best_val:.4f}  test_acc={test_acc:.4f}")
    print(f"wrote {args.out / 'finetune_result.json'}")


if __name__ == "__main__":
    main()
