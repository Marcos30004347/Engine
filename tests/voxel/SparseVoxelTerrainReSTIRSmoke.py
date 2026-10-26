"""Default terrain lighting must trace valid native shadows on both frame slots."""
import argparse
import math
import pathlib
import re
import struct
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sample", type=pathlib.Path)
parser.add_argument("world", type=pathlib.Path)
parser.add_argument("--rays-per-pixel", type=int, choices=range(1, 9))
parser.add_argument("--terrain-gpu-build-lanes", type=int, choices=range(1, 101))
parser.add_argument("--capture-directory", type=pathlib.Path,
                    help="Keep captured frames for visual inspection")
options = parser.parse_args()
sample = options.sample.resolve()
world = options.world.resolve()
overrides = []
if options.rays_per_pixel is not None:
    overrides += ["--rays-per-pixel", str(options.rays_per_pixel)]
if options.terrain_gpu_build_lanes is not None:
    overrides += ["--terrain-gpu-build-lanes", str(options.terrain_gpu_build_lanes)]
with tempfile.TemporaryDirectory(prefix="terrain-restir-") as directory:
    capture_directory = (options.capture_directory.resolve()
                         if options.capture_directory else pathlib.Path(directory))
    capture_directory.mkdir(parents=True, exist_ok=True)
    for mode, extra in (("screen-first", []), ("world-only", ["--disable-screen-traces", "--test-camera-ground-forward", "--test-camera-travel-m", "4", "--test-camera-travel-start-frame", "150", "--test-camera-travel-frames", "30"])):
        prefix = capture_directory / mode
        command = [str(sample), "--world-map", str(world), "--terrain-render-distance-km", "1",
                   "--frames", "180", "--resolution", "640x360", "--test-static-scene",
                   "--disable-frame-generation", "--disable-vsync", "--test-capture-start-frame", "176",
                   "--dump-frame", str(prefix), *overrides, *extra]
        run = subprocess.run(command, cwd=sample.parent, text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, timeout=150)
        log = run.stdout
        (sample.parent / f"terrain-restir-{mode}.log").write_text(log)
        if "ReSTIR hardware tracing unavailable for this scene/backend" in log:
            print("SKIP: native terrain ray tracing unavailable")
            sys.exit(77)
        assert run.returncode == 0, log[-12000:]
        assert "ReSTIR hardware ray tracing: Metal terrain" in log
        assert "Terrain hardware visibility: software Tree64 pools and TLAS omitted" in log
        assert "Terrain Tree64 immutable page pools:" not in log
        assert "Registering pass svr.sample.Final.BrickPhase" not in log
        assert "Terrain VG uses ReSTIR ray-traced lighting." in log
        if "ReSTIR denoiser: MetalFX temporal denoised scaler" in log:
            assert "Registering pass svr.sample.SVGF." not in log
            assert "Registering pass svr.sample.LightingTemporalAccumulation" not in log
        assert "Failed to map" not in log and "BLAS preparation rejected" not in log
        assert "page scheduling rejected" not in log and "has no page slot" not in log
        native = re.findall(r"ReSTIR native visibility: frame=(\d+) instances=(\d+) rays=(\d+) invalid=(\d+)", log)
        assert {int(f) for f, *_ in native} >= {176, 177, 178, 179}, native
        assert all(int(i) > 0 and int(r) > 0 and int(bad) == 0 for _, i, r, bad in native), native
        stats = re.findall(r"Sparse voxel scene ray stats: frame=(\d+) rays=(\d+) screenHits=(\d+) screenMisses=(\d+) worldHits=(\d+) worldMisses=(\d+) escapedScene=(\d+) unresolved=(\d+)", log)
        assert len(stats) >= 4, stats
        reused = re.findall(r"temporalReservoirs=(\d+)", log)
        assert len(reused) >= 4 and all(int(count) > 0 for count in reused), reused
        assert all(int(h) > 0 and int(m) > 0 and int(e) == 0 and int(u) == 0
                   for _, _, _, _, h, m, e, u in stats), stats
        # The default has a fresh sky-occlusion lane for every scheduled
        # receiver, independent of the sun reservoir. Validate both visible
        # sky (ambient fill) and blocked sky (occlusion), on both frame slots.
        sky_stats = re.findall(r"skyHits=(\d+) skyRays=(\d+).*scheduled=(\d+)", log)
        assert len(sky_stats) >= 4, sky_stats
        assert all(0 < int(hits) < int(rays) for hits, rays, _ in sky_stats), sky_stats
        if options.rays_per_pixel != 1:
            assert all(int(rays) == int(scheduled) for _, rays, scheduled in sky_stats), sky_stats
            if mode == "screen-first":
                blocked_fraction = [int(hits) / int(rays) for hits, rays, _ in sky_stats]
                assert max(blocked_fraction) - min(blocked_fraction) < 0.10, (
                    "Stationary sky visibility changes with raster jitter", sky_stats)
        if mode == "world-only":
            assert all(int(s) == 0 for _, _, s, *_ in stats), stats
            # The moving VG path previously omitted previous camera matrices,
            # which made every motion vector zero and ghosted lighting history.
            # Signature hashes contain all pixels' two float32 motion channels.
            zero_motion = 1469598103934665603
            for _ in range(320 * 180 * 8):
                zero_motion = (zero_motion * 1099511628211) & ((1 << 64) - 1)
            motion_hashes = re.findall(r"motionVectorHash=([0-9a-f]+)", log)
            assert len(motion_hashes) >= 4 and all(int(h, 16) != zero_motion for h in motion_hashes), motion_hashes
        for frame in range(176, 180):
            data = pathlib.Path(f"{prefix}.presented.{frame}.rgba32f").read_bytes()
            w, h = struct.unpack_from("II", data)
            assert (w, h) == (640, 360) and len(data) == 8 + w*h*16
            pixels = struct.iter_unpack("ffff", data[8:])
            lit = 0
            for rgba in pixels:
                assert all(math.isfinite(value) for value in rgba), "nonfinite reconstructed image"
                lit += max(rgba[:3]) > 0.001
            assert lit > w*h*0.9, f"black reconstruction: {lit}/{w*h}"
        print(f"PASS: {mode}, native terrain shadows and finite reconstructed output on both frame slots")
