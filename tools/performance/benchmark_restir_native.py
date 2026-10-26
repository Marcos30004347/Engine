#!/usr/bin/env python3
"""Measure real ReSTIR frames at native resolution, without frame generation.

Use Xcode Metal System Trace/Time Profiler in a separate run to attribute costs.
These application frame durations measure throughput and pacing, not GPU passes.
The raster stays native; the light field uses the current default fraction.
The default disables temporal upscaling; MetalFX denoising at 1x is opt-in.
"""
import argparse
import collections
import json
import math
import os
import pathlib
import re
import statistics
import subprocess


def distribution(values):
    ordered = sorted(values)
    return {"mean_ms": statistics.mean(ordered),
            "median_ms": statistics.median(ordered),
            "p95_ms": ordered[math.ceil(len(ordered) * .95) - 1],
            "p99_ms": ordered[math.ceil(len(ordered) * .99) - 1],
            "max_ms": ordered[-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("sample", type=pathlib.Path)
    parser.add_argument("world", type=pathlib.Path)
    parser.add_argument("output", type=pathlib.Path)
    parser.add_argument("--warmup", type=int, default=2400)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--rays-per-pixel", type=int, choices=range(1, 9))
    parser.add_argument("--metalfx-denoiser", action="store_true",
                        help="Use MetalFX denoising at native 1x resolution; no frame generation")
    parser.add_argument("--summarize-only", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    command = [str(args.sample.resolve()), "--world-map", str(args.world.resolve()),
               "--terrain-render-distance-km", "5", "--resolution", "1200x720",
               "--render-scale", "1", "--disable-frame-generation",
               "--disable-metalfx-upscaler",
               "--disable-vsync", "--benchmark-lighting", str(output),
               "--benchmark-sequence", "static", "--benchmark-warmup", str(args.warmup),
               "--benchmark-frames", str(args.frames)]
    if args.metalfx_denoiser:
        command += ["--enable-metalfx-denoiser"]
    else:
        command += ["--disable-metalfx-denoiser", "--no-taa"]
    if args.rays_per_pixel is not None:
        command += ["--rays-per-pixel", str(args.rays_per_pixel)]
    if not args.summarize_only:
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.with_suffix(".log").open("w") as log:
            subprocess.run(command, cwd=args.sample.resolve().parent,
                           stdout=log, stderr=subprocess.STDOUT, check=True)
    report = json.loads(output.read_text())
    settings_log = output.with_suffix(".log").read_text()
    sampling = re.search(r"receiverStride=(\d+) raysPerPixel=(\d+)", settings_log)
    assert sampling, "Missing effective terrain ray settings"
    config = report["configuration"]
    assert config["resolution"] == config["internal_resolution"] == [1200, 720]
    assert config["frame_generation_enabled"] is False
    assert config["restir_backend"] == "metal"
    assert config["denoiser"] == (
        "metalfx-temporal-denoised" if args.metalfx_denoiser else "svgf-temporal-lighting")
    if args.metalfx_denoiser:
        assert config["svgf_atrous_iterations"] == 0, "SVGF must be bypassed with MetalFX"
    frames = report["frames"]
    assert frames
    timing = distribution([f["cpu_total_ms"] for f in frames])
    sections = collections.defaultdict(list)
    for frame in frames:
        for section in frame["cpu_sections"]:
            sections[section["name"]].append(section["milliseconds"])
    resident = [f["terrain_streaming"]["resident_page_count"] for f in frames]
    feedback_frames = sum(bool(f.get("terrain_feedback", {}).get("readback_submitted"))
                          for f in frames)
    summary = {
        "command": command,
        "metal_prefill_override": os.environ.get("ENGINE_MVK_PREFILL"),
        "measurement": "Application real-frame duration; GPU attribution requires Metal/Xcode trace",
        "frames": len(frames), "fps": 1000 / timing["mean_ms"],
        "denoiser": config["denoiser"],
        "light_field_resolution": config.get("light_field_resolution"),
        "rays_per_pixel": int(sampling[2]),
        "receiver_stride": int(sampling[1]),
        "frame_time": timing,
        "frames_over_45fps_budget": sum(f["cpu_total_ms"] > 1000 / 45 for f in frames),
        "frames_over_50ms": sum(f["cpu_total_ms"] > 50 for f in frames),
        "stable_45fps_p95": timing["p95_ms"] <= 1000 / 45,
        "resident_pages_min_max": [min(resident), max(resident)],
        "terrain_feedback_frames": feedback_frames,
        "terrain_feedback_fraction": feedback_frames / len(frames),
        "warm_blas_builds": config["hardware_warm_blas_builds"],
        "cpu_sections": {k: distribution(v) for k, v in sections.items()},
        "slowest_frames": sorted(frames, key=lambda f: f["cpu_total_ms"], reverse=True)[:5],
    }
    output.with_suffix(".summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    print(json.dumps({k: v for k, v in summary.items()
                      if k not in ("command", "cpu_sections", "slowest_frames")}, indent=2))
    # A generation change can discard a few stale feedback snapshots. A run
    # with paused streaming must never qualify as a renderer performance win.
    assert feedback_frames >= .95 * len(frames), "Terrain feedback stopped during measurement"


if __name__ == "__main__":
    main()
