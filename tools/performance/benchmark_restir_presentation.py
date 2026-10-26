#!/usr/bin/env python3
"""Measure real-frame pacing with the supported-Apple presentation defaults.

This is application throughput, not Metal encoder instrumentation. Attribute
GPU pass costs separately using Xcode Metal System Trace. Generated frames are
not counted as rendered frames or used to double the reported FPS.
"""
import argparse
import json
from pathlib import Path
import subprocess
from benchmark_restir_native import distribution


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample", type=Path)
    parser.add_argument("world", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--crt-preset", choices=("consumer90s", "trinitron", "arcade", "pvm"))
    parser.add_argument("--warmup", type=int, default=2400)
    parser.add_argument("--frames", type=int, default=600)
    args = parser.parse_args()
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    command = [str(args.sample.resolve()), "--world-map", str(args.world.resolve()),
               "--terrain-render-distance-km", "5", "--resolution", "1200x720",
               "--disable-vsync", "--benchmark-lighting", str(output),
               "--benchmark-sequence", "static", "--benchmark-warmup", str(args.warmup),
               "--benchmark-frames", str(args.frames)]
    if args.crt_preset:
        command += ["--crt-preset", args.crt_preset]
    with output.with_suffix(".log").open("w") as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
    report = json.loads(output.read_text())
    config, frames = report["configuration"], report["frames"]
    assert config["resolution"] == [1200, 720]
    assert config["internal_resolution"] == [600, 360]
    assert config["light_field_resolution"] == [450, 270]
    assert config["restir_backend"] == "metal"
    assert config["denoiser"] == "metalfx-temporal-denoised"
    assert config["frame_generation_enabled"] and config["upscaling_enabled"]
    assert config["crt_enabled"] == bool(args.crt_preset)
    assert config["svgf_atrous_iterations"] == 0
    times = [frame["cpu_total_ms"] for frame in frames]
    timing = distribution(times)
    resident = [frame["terrain_streaming"]["resident_page_count"] for frame in frames]
    feedback = sum(bool(frame.get("terrain_feedback", {}).get("readback_submitted")) for frame in frames)
    assert feedback >= .95 * len(frames), "Terrain feedback stopped during measurement"
    summary = {
        "command": command,
        "measurement": "Application real-frame duration with native interpolation enabled; generated frames excluded from FPS",
        "configuration": config,
        "frames": len(frames),
        "real_fps": 1000 / timing["mean_ms"],
        "frame_time": timing,
        "stable_45fps_p95": timing["p95_ms"] <= 1000 / 45,
        "frames_over_50ms": sum(value > 50 for value in times),
        "resident_pages_min_max": [min(resident), max(resident)],
        "terrain_feedback_frames": feedback,
        "warm_blas_builds": config["hardware_warm_blas_builds"],
    }
    output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()
