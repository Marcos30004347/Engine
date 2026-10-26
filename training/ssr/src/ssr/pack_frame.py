from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np


def _rgba32f(path: Path, width: int, height: int) -> np.ndarray:
    expected = width * height * 4
    byte_count = path.stat().st_size
    offset = 0
    if byte_count == expected * 4 + 8:
        header = np.fromfile(path, dtype=np.uint32, count=2)
        if tuple(int(value) for value in header) != (width, height):
            raise ValueError(f"{path} header is {header[0]}x{header[1]}, expected {width}x{height}")
        offset = 8
    pixels = np.fromfile(path, dtype=np.float32, offset=offset)
    if pixels.size != expected:
        raise ValueError(
            f"{path} contains {pixels.size} floats; expected {expected} for {width}x{height} RGBA32F"
        )
    return pixels.reshape(height, width, 4)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Pack renderer --dump-frame RGBA32F layers into one SSR training frame"
    )
    parser.add_argument("--width", type=int, required=True)
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--sparse", type=Path, required=True, help="debug mode 18: RGB radiance, A exact sample mask")
    parser.add_argument("--reference", type=Path, required=True, help="full-rate debug mode 14")
    parser.add_argument("--normal", type=Path, required=True, help="debug mode 1, encoded from [-1,1] to [0,1]")
    parser.add_argument("--depth", type=Path, required=True, help="debug mode 8")
    parser.add_argument("--depth-scale", type=float, default=0.1, help="linear-depth debug multiplier")
    parser.add_argument("--albedo", type=Path, help="debug mode 0")
    parser.add_argument("--roughness", type=Path, help="debug mode 3")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    sparse = _rgba32f(args.sparse, args.width, args.height)
    reference = _rgba32f(args.reference, args.width, args.height)
    normal = _rgba32f(args.normal, args.width, args.height)
    depth = _rgba32f(args.depth, args.width, args.height)
    payload: dict[str, np.ndarray] = {
        "sparse_radiance": sparse[..., :3],
        "sample_mask": sparse[..., 3:4],
        "reference_radiance": reference[..., :3],
        "normal": normal[..., :3] * 2.0 - 1.0,
        "linear_depth": depth[..., :1] / max(args.depth_scale, 1.0e-8),
    }
    if args.albedo:
        payload["albedo"] = _rgba32f(args.albedo, args.width, args.height)[..., :3]
    if args.roughness:
        payload["roughness"] = _rgba32f(args.roughness, args.width, args.height)[..., :1]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output, **payload)
    print(f"wrote {args.output} ({args.width}x{args.height})")
