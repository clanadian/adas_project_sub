#!/usr/bin/env python3
"""Export the deployed Arty ROI classifier as FP32 ONNX graphs.

TensorRT applies FP16 precision when it builds the engine.  The shipped INT8
weights are dequantized with export/manifest.json; simply casting the integer
files to float would create a different network.
"""

import argparse
import json
from pathlib import Path

import numpy as np
import torch
from torch import nn


class RoiClassifier(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.conv0 = nn.Conv2d(3, 16, 3, padding=1)
        self.conv1 = nn.Conv2d(16, 32, 3, padding=1)
        self.conv2 = nn.Conv2d(32, 64, 3, padding=1)
        self.pool = nn.MaxPool2d(2, 2)
        self.fc = nn.Linear(64, 6)

    def features(self, x: torch.Tensor) -> torch.Tensor:
        x = self.pool(torch.relu(self.conv0(x)))
        x = self.pool(torch.relu(self.conv1(x)))
        return self.pool(torch.relu(self.conv2(x)))

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = self.features(x)
        # PL/PS uses an integer sum, with 1/144 folded into the FC scale.
        # Its real-valued equivalent is global average pooling.
        x = torch.mean(x, dim=(2, 3))
        return self.fc(x)


class FeatureOnly(nn.Module):
    def __init__(self, model: RoiClassifier) -> None:
        super().__init__()
        self.model = model

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return self.model.features(x)


def read_array(path: Path, dtype: np.dtype, shape: tuple[int, ...]) -> np.ndarray:
    values = np.fromfile(path, dtype=dtype)
    expected = int(np.prod(shape))
    if values.size != expected:
        raise ValueError(f"{path}: expected {expected} values, got {values.size}")
    return values.reshape(shape)


def copy_parameter(parameter: torch.Tensor, values: np.ndarray) -> None:
    with torch.no_grad():
        parameter.copy_(torch.from_numpy(np.ascontiguousarray(values)).float())


def load_model(model_dir: Path, manifest: dict) -> RoiClassifier:
    model = RoiClassifier()
    layers = manifest["layers"]

    # conv0 is already OIHW. conv1/2 are WPACK [O][H][W][I].
    layouts = {
        "conv0": ((16, 3, 3, 3), False),
        "conv1": ((32, 3, 3, 16), True),
        "conv2": ((64, 3, 3, 32), True),
    }
    modules = {"conv0": model.conv0, "conv1": model.conv1, "conv2": model.conv2}

    for name, (shape, is_wpack) in layouts.items():
        spec = layers[name]
        weight_q = read_array(model_dir / spec["weight_file"], np.int8, shape)
        if is_wpack:
            weight_q = weight_q.transpose(0, 3, 1, 2)
        weight = weight_q.astype(np.float32) * float(spec["weight_scale"])

        bias_q = read_array(
            model_dir / spec["bias_file"], np.int32, (shape[0],)
        )
        # The shipped bias includes the exporter's integer rounding compensation.
        # Keeping it reproduces the deployed parameter set as closely as a
        # continuous FP graph can; intermediate INT8 requantization is not copied.
        bias_scale = float(spec["input_scale"]) * float(spec["weight_scale"])
        bias = bias_q.astype(np.float32) * bias_scale

        copy_parameter(modules[name].weight, weight)
        copy_parameter(modules[name].bias, bias)

    fc_spec = manifest["fc"]
    class_count = len(manifest["classes"])
    if class_count != model.fc.out_features:
        raise ValueError(f"expected 6 classes, manifest has {class_count}")
    fc_weight_q = read_array(
        model_dir / fc_spec["weight_file"], np.int8, (class_count, 64)
    )
    fc_bias_q = read_array(
        model_dir / fc_spec["bias_file"], np.int32, (class_count,)
    )
    copy_parameter(
        model.fc.weight,
        fc_weight_q.astype(np.float32) * float(fc_spec["weight_scale"]),
    )
    copy_parameter(
        model.fc.bias,
        fc_bias_q.astype(np.float32) * float(fc_spec["logits_scale"]),
    )
    return model.eval()


def export_graph(
    model: nn.Module, output_path: Path, output_name: str, opset: int
) -> None:
    dummy = torch.zeros((1, 3, 96, 96), dtype=torch.float32)
    torch.onnx.export(
        model,
        dummy,
        str(output_path),
        input_names=["images"],
        output_names=[output_name],
        opset_version=opset,
        do_constant_folding=True,
        dynamic_axes=None,
    )


def sanity_check(model: RoiClassifier, model_dir: Path, manifest: dict) -> None:
    input_path = model_dir / "golden_input_98x98x3_int8.npy"
    vector_path = model_dir / "golden_vector.json"
    if not input_path.exists() or not vector_path.exists():
        print("sanity: skipped (golden files missing)")
        return

    padded = np.load(input_path)
    if padded.shape != (98, 98, 3):
        raise ValueError(f"unexpected golden input shape: {padded.shape}")
    roi_q = padded[1:97, 1:97, :].astype(np.float32)
    roi_real = roi_q * float(manifest["input"]["input_scale"])
    tensor = torch.from_numpy(roi_real.transpose(2, 0, 1)[None, ...])
    with torch.no_grad():
        logits = model(tensor)
    actual = int(torch.argmax(logits, dim=1).item())
    expected = int(json.loads(vector_path.read_text())["class_id"])
    result = "MATCH" if actual == expected else "DIFF (allowed: FP graph has no INT8 requant)"
    print(f"sanity: expected class={expected}, FP graph class={actual}: {result}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("model_dir", type=Path)
    parser.add_argument("output_dir", type=Path)
    parser.add_argument("--opset", type=int, default=13)
    args = parser.parse_args()

    manifest_path = args.model_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text())
    args.output_dir.mkdir(parents=True, exist_ok=True)

    model = load_model(args.model_dir, manifest)
    sanity_check(model, args.model_dir, manifest)
    export_graph(
        FeatureOnly(model).eval(),
        args.output_dir / "classifier_features.onnx",
        "features",
        args.opset,
    )
    export_graph(
        model,
        args.output_dir / "classifier_full.onnx",
        "logits",
        args.opset,
    )

    metadata = {
        "source_manifest": str(manifest_path),
        "input": "1x3x96x96 FP32; TensorRT builds FP16",
        "features_output": "1x64x12x12",
        "full_output": f"1x{len(manifest['classes'])}",
        "scope": "latency comparison; not bit-exact with intermediate INT8 requant",
    }
    (args.output_dir / "export_metadata.json").write_text(
        json.dumps(metadata, indent=2) + "\n"
    )
    print(f"wrote {args.output_dir / 'classifier_features.onnx'}")
    print(f"wrote {args.output_dir / 'classifier_full.onnx'}")


if __name__ == "__main__":
    main()
