from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import tempfile

import numpy as np

from .dataset import blue_noise_block_mask
from .pack_frame import _rgba32f


def _capture(
    renderer: Path, renderer_args: list[str], temporary: Path, name: str,
    mode: int, frame_count: int, width: int, height: int,
    lighting_args: list[str] | None = None,
) -> np.ndarray:
    prefix = temporary / name
    command = [
        str(renderer), *renderer_args,
        "--frames", str(frame_count), "--resolution", f"{width}x{height}",
        "--debug-mode", str(mode), "--dump-frame", str(prefix), "--no-taa",
        "--disable-svgf", *(lighting_args or []),
    ]
    print("capture:", " ".join(command))
    subprocess.run(command, check=True)
    outputs = sorted(temporary.glob(f"{name}.*.rgba32f"))
    if not outputs:
        raise RuntimeError(f"renderer produced no frame for {name}")
    return _rgba32f(outputs[-1], width, height)


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Drive the renderer and generate train/validation NBG frames"
    )
    parser.add_argument("--renderer", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--resolution", default="960x540")
    parser.add_argument("--capture-frames", default="64,96,128", help="independent deterministic trajectory snapshots")
    parser.add_argument("--windows", default="2,4,8,16")
    parser.add_argument("--validation-fraction", type=float, default=0.2)
    parser.add_argument("--depth-scale", type=float, default=0.1)
    parser.add_argument("renderer_args", nargs=argparse.REMAINDER, help="arguments after -- are forwarded to the renderer")
    args = parser.parse_args()
    width, height = (int(value) for value in args.resolution.lower().split("x", 1))
    capture_frames = [int(value) for value in args.capture_frames.split(",")]
    windows = [int(value) for value in args.windows.split(",")]
    if any(window not in (2, 4, 8, 16) for window in windows):
        raise ValueError("windows must be selected from 2,4,8,16")
    forwarded = list(args.renderer_args)
    if forwarded and forwarded[0] == "--":
        forwarded.pop(0)
    validation_count = max(1, round(len(capture_frames) * args.validation_fraction)) if len(capture_frames) > 1 else 0
    split_at = len(capture_frames) - validation_count

    with tempfile.TemporaryDirectory(prefix="engine-nbg-capture-") as temporary_name:
        temporary = Path(temporary_name)
        for capture_index, frame_count in enumerate(capture_frames):
            split = "validation" if capture_index >= split_at else "train"
            # Debug 18 is the raw current-frame demodulated lighting contract.
            reference = _capture(
                args.renderer, forwarded, temporary,
                f"reference_{capture_index}", 18, frame_count, width, height,
                ["--ray-resolution-divisor", "1", "--rays-per-pixel", "8", "--restir-candidates-per-pixel", "16"],
            )
            normal = _capture(args.renderer, forwarded, temporary, f"normal_{capture_index}", 1, frame_count, width, height)
            depth = _capture(args.renderer, forwarded, temporary, f"depth_{capture_index}", 8, frame_count, width, height)
            albedo = _capture(args.renderer, forwarded, temporary, f"albedo_{capture_index}", 0, frame_count, width, height)
            roughness = _capture(args.renderer, forwarded, temporary, f"roughness_{capture_index}", 3, frame_count, width, height)
            reference_rgb = reference[..., :3]
            base = {
                "reference_radiance": reference_rgb,
                "normal": normal[..., :3] * 2.0 - 1.0,
                "linear_depth": depth[..., :1] / max(args.depth_scale, 1.0e-8),
                "albedo": albedo[..., :3],
                "roughness": roughness[..., :1],
            }
            for window in windows:
                mask = blue_noise_block_mask(
                    height, width, window,
                    seed=frame_count * 17 + window,
                ).numpy().transpose(1, 2, 0)
                output = args.output / split / f"frame_{capture_index:04d}_n{window}.npz"
                output.parent.mkdir(parents=True, exist_ok=True)
                np.savez_compressed(
                    output, **base,
                    sparse_radiance=reference_rgb * mask,
                    sample_mask=mask,
                    sampling_window=np.asarray(window, dtype=np.uint32),
                    capture_frame=np.asarray(frame_count, dtype=np.uint32),
                )
                print("wrote", output)


if __name__ == "__main__":
    main()
