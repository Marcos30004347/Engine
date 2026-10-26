from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import torch

from .model import NeuralBilateralGridDenoiser


def main() -> None:
    parser = argparse.ArgumentParser(description="Export SSR weights for engine integration")
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("--output", type=Path, default=Path("ssr_weights.npz"))
    parser.add_argument("--onnx", type=Path)
    parser.add_argument("--height", type=int, default=256)
    parser.add_argument("--width", type=int, default=256)
    args = parser.parse_args()

    checkpoint = torch.load(args.checkpoint, map_location="cpu", weights_only=False)
    config = checkpoint.get("config", {})
    model = NeuralBilateralGridDenoiser(
        hidden_channels=int(config.get("hidden_channels", 20)),
    )
    model.load_state_dict(checkpoint["model"])
    model.eval()

    arrays = {name: tensor.detach().cpu().numpy().astype(np.float16) for name, tensor in model.state_dict().items()}
    np.savez(args.output, **arrays)
    manifest = {
        "format": "engine.ssr.weights.v1",
        "model": "MaskedNeuralBilateralGrid",
        "input_channels": model.input_channels,
        "spatial_strides": list(model.spatial_strides),
        "range_bins": list(model.range_bins),
        "weights": [{"name": name, "shape": list(value.shape), "dtype": "float16"} for name, value in arrays.items()],
    }
    args.output.with_suffix(".json").write_text(json.dumps(manifest, indent=2) + "\n")

    if args.onnx is not None:
        inputs = (
            torch.zeros(1, 3, args.height, args.width),
            torch.zeros(1, 1, args.height, args.width),
            torch.zeros(1, 3, args.height, args.width),
            torch.ones(1, 1, args.height, args.width),
            torch.ones(1, 3, args.height, args.width),
            torch.zeros(1, 1, args.height, args.width),
        )
        torch.onnx.export(
            model, inputs, args.onnx,
            input_names=("sparse_radiance", "sample_mask", "normal", "linear_depth", "albedo", "roughness"),
            output_names=("reconstructed_radiance",),
            dynamic_axes={name: {2: "height", 3: "width"} for name in (
                "sparse_radiance", "sample_mask", "normal", "linear_depth", "albedo", "roughness", "reconstructed_radiance"
            )},
            opset_version=18,
        )


if __name__ == "__main__":
    main()
