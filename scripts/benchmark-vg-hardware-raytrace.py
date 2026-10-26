#!/usr/bin/env python3
"""Run interleaved native RT microbenchmarks and retain raw samples and provenance.

This is a benchmark driver, not a renderer/frame-path operation. Its subprocesses
and diagnostic waits intentionally run outside the measured native GPU interval.
"""
import argparse
import hashlib
import json
from pathlib import Path
import statistics
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--warmup", type=int, default=120)
    args = parser.parse_args()
    if args.repeats < 1 or args.frames < 1 or args.warmup < 0:
        parser.error("repeats/frames must be positive and warmup nonnegative")
    executable = args.executable.resolve(strict=True)
    args.output.mkdir(parents=True, exist_ok=True)
    root = Path(__file__).resolve().parents[1]
    def git(*command):
        return subprocess.run(["git", *command], cwd=root, text=True,
                              capture_output=True, check=True).stdout.strip()
    sources = [root / "assets/shaders/virtualgeometry/metal/virtualgeometry-raytrace.metal",
               root / "src/runtime/virtualgeometry/raytracing/metal/VirtualGeometryMetalRayTraceBackend.mm",
               root / "tests/virtualgeometry/VirtualGeometryHardwareRayTraceBenchmark.mm"]
    def fingerprint():
        return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest() for p in sources}
    source_hashes = fingerprint()
    binary_hash = hashlib.sha256(executable.read_bytes()).hexdigest()
    commit = git("rev-parse", "HEAD")
    dirty = bool(git("status", "--porcelain"))
    cases = [
        ("trace-1k-1m", "trace", 1000, 1000000, 1),
        ("tlas-1k", "tlas", 1000, 1000000, 1),
        ("full-1k-1m", "full", 1000, 1000000, 1),
        ("full-10k-1m", "full", 10000, 1000000, 1000),
        ("blas-1k", "blas", 1000, 256, 1000),
    ]
    reports = {}
    for repeat in range(args.repeats):
        for name, suite, instances, rays, clusters in (cases if repeat % 2 == 0 else reversed(cases)):
            output = args.output / f"{name}-run{repeat + 1}.json"
            # Cold builds are expensive; keep explicit separate sample counts.
            frames = min(args.frames, 20) if suite == "blas" else args.frames
            warmup = 0 if suite == "blas" else args.warmup
            command = [str(executable), "--backend", "metal", "--suite", suite,
                       "--instances", str(instances), "--rays", str(rays),
                       "--clusters", str(clusters), "--warmup", str(warmup),
                       "--frames", str(frames), "--output", str(output.resolve())]
            result = subprocess.run(command, capture_output=True, text=True, check=False)
            if result.returncode == 77:
                raise SystemExit("Hardware benchmark skipped: " + result.stdout.strip())
            if result.returncode != 0:
                raise SystemExit(f"{name} failed: {result.stderr}\n{result.stdout}")
            report = json.loads(output.read_text())
            if not report.get("quality_verified") or report.get("build_type") != "release":
                raise SystemExit(f"{name}: performance reports require release and verified visibility")
            reports.setdefault(name, []).append(report)
            print(f"{name} run {repeat+1}: p50={report['gpu_ms_p50']:.3f} ms "
                  f"p95={report['gpu_ms_p95']:.3f} ms", flush=True)
    if source_hashes != fingerprint() or binary_hash != hashlib.sha256(executable.read_bytes()).hexdigest():
        raise SystemExit("Benchmark source or binary changed during measurement; rerun from a stable build")
    summary = {"schema_version": 1, "commit": commit,
               "working_tree_dirty": dirty,
               "binary_sha256": binary_hash,
               "source_sha256": source_hashes,
               "measurement": "serialized native GPU microbenchmarks; no renderer integration timing",
               "cases": {}}
    for name, runs in reports.items():
        summary["cases"][name] = {
            "runs": len(runs), "device": runs[0]["device"], "os": runs[0]["os"],
            "median_run_p50_ms": statistics.median(r["gpu_ms_p50"] for r in runs),
            "median_run_p95_ms": statistics.median(r["gpu_ms_p95"] for r in runs),
            "worst_run_p99_ms": max(r["gpu_ms_p99"] for r in runs),
            "run_p50_min_ms": min(r["gpu_ms_p50"] for r in runs),
            "run_p50_max_ms": max(r["gpu_ms_p50"] for r in runs),
            "blas_bytes": runs[0]["blas_bytes"], "tlas_bytes": runs[0]["tlas_bytes"],
            "scratch_bytes": runs[0]["scratch_bytes"],
        }
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
