"""Exhaust a small geometry arena, travel out/back, and require continued publication."""
import argparse
import pathlib
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('sample', type=pathlib.Path)
parser.add_argument('world', type=pathlib.Path)
parser.add_argument('--validate-log', type=pathlib.Path)
args = parser.parse_args()
if args.validate_log:
    log = args.validate_log.read_text()
else:
    sample, world = args.sample.resolve(), args.world.resolve()
    command = [str(sample), '--world-map', str(world), '--terrain-render-distance-km', '5',
               '--terrain-vg-memory-mb', '128', '--resolution', '640x360', '--render-scale', '1',
               '--disable-frame-generation', '--disable-metalfx-upscaler',
               '--disable-metalfx-denoiser', '--no-taa', '--disable-vsync', '--frames', '1800',
               '--test-static-scene', '--test-camera-out-and-back', '--test-camera-forward',
               '--test-camera-travel-start-frame', '700', '--test-camera-travel-frames', '900',
               '--test-camera-travel-m', '256', '--test-frame-signatures',
               '--test-capture-start-frame', '1796']
    log_path = sample.parent / 'terrain-capacity-recovery.log'
    with log_path.open('w') as output:
        run = subprocess.run(command, cwd=sample.parent, stdout=output,
                             stderr=subprocess.STDOUT, text=True, timeout=240)
    log = log_path.read_text()
    assert run.returncode == 0, log[-12000:]
assert 'Terrain generated-VG reservation failed:' in log, 'capacity failure was not exercised'
assert 'Terrain GPU publication rolled back: generated-VG geometry arena capacity exhausted' in log
recovery = list(re.finditer(r'Terrain GPU headroom recovery: frame=(\d+) leaves=(\d+) -> (\d+)', log))
assert recovery and all(int(m[2]) - int(m[3]) == 3 for m in recovery), 'no complete sibling recovery'
assert re.search(r'full-GPU metadata committed: generation=\d+', log[recovery[0].end():]), 'recovery never published'
# The measured run must complete multiple transactions after the first failure.
first_failure = log.index('Terrain generated-VG reservation failed:')
commits = re.findall(r'full-GPU metadata committed: generation=(\d+)', log[first_failure:])
assert len(commits) >= 3, 'streaming stopped after recovery'
limit = re.search(r'geometry admission limit: pages=(\d+)', log)
assert limit, 'failed geometry budget was not propagated to admission'
resident = [int(n) for n in re.findall(r'metadata committed: generation=\d+ resident=(\d+)', log[recovery[0].end():])]
assert resident and min(resident) >= int(limit[1]) // 2, 'recovery collapsed the detailed cut to roots'
assert log.count('Terrain generated-VG reservation failed:') <= 4, 'allocation failure retry loop'
assert sum(int(m[1]) < 700 for m in recovery) <= 8, 'stationary refine/coarsen churn'
for bad in ('memory budget exceeded', 'BLAS preparation rejected', 'rollback directory failed'):
    assert bad not in log, bad
native = re.findall(r'native visibility: frame=(179[6-9]) instances=(\d+) rays=(\d+) invalid=(\d+)', log)
assert len(native) == 4 and all(int(i) > 0 and int(r) > 0 and int(b) == 0 for _, i, r, b in native), native
coverage = re.findall(r'frame signature: frame=(179[6-9]) coveredPixels=(\d+) holes=(\d+) enclosed=(\d+)', log)
assert len(coverage) == 4 and all(int(c) > 1000 and int(h) == 0 and int(e) == 0 for _, c, h, e in coverage), coverage
print('PASS: exhausted arena recovers, out/back camera keeps publishing, final native terrain has no holes or invalid rays')
