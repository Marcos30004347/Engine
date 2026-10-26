"""Move 256 m toward the default mountain view; reject stalled native terrain cuts."""
import argparse
import pathlib
import re
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("sample", type=pathlib.Path)
parser.add_argument("world", type=pathlib.Path)
parser.add_argument("--validate-log", type=pathlib.Path)
parser.add_argument("--native-resolution", action="store_true")
parser.add_argument("--dump-prefix", type=pathlib.Path)
args = parser.parse_args()
if args.validate_log:
    log = args.validate_log.read_text()
else:
    sample, world = args.sample.resolve(), args.world.resolve()
    command = [str(sample), "--world-map", str(world), "--terrain-render-distance-km", "5",
               "--resolution", "1220x720", "--frames", "1400", "--test-static-scene",
               "--test-camera-forward", "--test-camera-travel-m", "256",
               "--test-camera-travel-start-frame", "400", "--test-camera-travel-frames", "600",
               "--test-frame-signatures", "--test-capture-start-frame", "1396"]
    if args.native_resolution:
        command += ["--resolution", "1200x720", "--render-scale", "1",
                    "--disable-frame-generation", "--disable-metalfx-upscaler",
                    "--disable-metalfx-denoiser", "--no-taa", "--disable-vsync"]
    if args.dump_prefix:
        command += ["--dump-frame", str(args.dump_prefix.resolve())]
    log_path = sample.parent / "terrain-restir-movement.log"
    with log_path.open("w") as output:
        run = subprocess.run(command, cwd=sample.parent, text=True, stdout=output,
                             stderr=subprocess.STDOUT, timeout=240)
    log = log_path.read_text()
    if "ReSTIR hardware tracing unavailable for this scene/backend" in log:
        print("SKIP: native terrain ray tracing unavailable")
        sys.exit(77)
    assert run.returncode == 0, log[-12000:]
assert "ReSTIR hardware ray tracing: Metal terrain" in log
feedback = re.search(r'feedback scheduler: readbacks=(\d+)', log)
assert feedback and int(feedback[1]) >= 1330, 'terrain feedback stopped during the movement test'
for failure in ("page scheduling rejected", "has no page slot", "BLAS preparation rejected",
                "produced an incomplete G-buffer", "memory budget exceeded"):
    assert failure not in log, failure
# Empty-build cut switches must commit too; otherwise this exact event holds
# the in-flight CPU result forever and prevents further camera refinement.
for match in re.finditer(r"Terrain GPU stream staged: builds=0[^\n]*queued=0", log):
    remainder = log[match.end():]
    queued = re.search(r"metadata queued: sourceGeneration=(\d+)", remainder)
    assert queued, "missing zero-build ownership transaction"
    generation = int(queued[1]) + 1
    assert re.search(rf"metadata committed: generation={generation}\b", remainder), "stalled zero-build cut"
settle = re.search(r"terrain settle:.*lod0Chunks=(\d+).*installFailures=(\d+) noSlice=(\d+) bareAfterRetire=(\d+)", log)
assert settle and int(settle[1]) > 0 and all(int(x) == 0 for x in settle.groups()[1:]), settle
native = re.findall(r"native visibility: frame=(\d+) instances=(\d+) rays=(\d+) invalid=(\d+)", log)
assert {int(f) for f, *_ in native} >= {1396, 1397, 1398, 1399}, native
assert all(int(i) > 0 and int(r) > 0 and int(bad) == 0 for _, i, r, bad in native), native
coverage = re.findall(r"frame signature: frame=(\d+) coveredPixels=(\d+) holes=(\d+) enclosed=(\d+)", log)
assert len(coverage) >= 4 and all(int(c) > 1000 and int(h) == 0 and int(e) == 0 for _, c, h, e in coverage), coverage
print("PASS: mountain-view travel, continuing cut publication, finest pages, native visibility and no page-scale coverage gaps")
