# Terrain generation and streaming budgets

The default CPU-generated world terrain uses larger bounded batches:

| Stage | Previous | New |
| --- | ---: | ---: |
| Source chunks generated per update, below 1440p output | 32 | 48 |
| Source chunks generated per update, 1440p output and above | 16 | 24 |
| Mesh jobs admitted per frame | 8 | 16 |
| Shared VG chunk installs per apply | 4 | 8 |
| Requested VG page installs per frame | 32 | 64 |
| Source upload ceiling, below 1440p | 64 MiB | 128 MiB |
| Source upload ceiling, 1440p and above | 32 MiB | 64 MiB |

The startup budget log reports generation, request, mesh, and install limits
together. Existing command-line overrides remain available. Worker concurrency
and resident memory budgets are unchanged; jobs and page reads remain
asynchronous. Encoded geometry and native atomic64 rasterization remain the
terrain defaults. GPU-v2 retains its separate atomic transaction limit.

## Selecting the limits

The measured-error request batch remains eight owners per job. A trial with
sixteen owners filled the 510-owner source cut with coarser pages before any
LOD-0 source pages arrived. That is faster completion of a worse cut, so it is
not the new default. Increasing the generation ceiling independently allows
bootstrap and camera-driven transactions to prepare more chunks while keeping
feedback-driven depth convergence bounded.

A 64-chunk generation trial with both doubled and quadrupled installation
budgets reproduced 114 enclosed gap pixels in the wide-view capacity fixture.
The larger limit is therefore not the default. The 48-chunk batch with doubled
mesh/install limits passed that unchanged test twice, with zero enclosed gaps,
10 cm near-field detail, 144 LOD-0 source chunks, and no install failures or
exhausted slices. Last activity was frame 293 and 277 respectively; asynchronous
scheduling changes this timing between runs.

Exploratory 600-frame comparisons at 5 km and 1220x720 output / 610x360 internal
rendering showed little end-to-end timing change from higher ceilings alone.
Generation work, upload/publication cycles, and successive hierarchy levels
still limit convergence. The upload ceiling was not saturated. These tests
selected higher safe throughput limits, not a claimed multiple of frame-rate
or convergence speed.

Trial logs are under `/tmp/terrain-vg-fix/throughput-*`. In particular,
`throughput-capacity-double.log` records the rejected 64-chunk trial, while
`throughput-capacity-balanced.log` and `throughput-capacity-balanced-repeat.log`
record the selected 48-chunk trial. The coverage assertions were not relaxed.

## Frontier prefetch

The culler retains the first error-rejected page when its projected parent
error is between half the render threshold and the threshold. It performs
visibility tests, publishes priority 1 with atomicMax, and returns without
queuing clusters or descending. Actual demand has priority at least 2.
Installed frontier pages receive the same retention touch; repeated speculative
visits cannot accumulate enough priority to outrank demand.

The terrain installer serves these speculative requests after required page
requests, using at most eight spare installs per frame. Prefetch stops above
90% page or cluster pool usage. It reuses the completed priority snapshot and
existing asynchronous page source; it adds no GPU readback or synchronization.
The visible cut's error threshold is unchanged.

This anticipates existing encoded asset pages. It does not extrapolate camera
velocity or speculatively split additional source chunks. The source budget is
already near its 512-slot cap in the movement fixture; speculative source
splits would compete with required 10 cm refinement. Camera-driven generation
continues to use radial demand and the corrected capacity priority ordering.

## Validation

Release renderer and culling integration test rebuilt successfully.

- CPU cut-advance and page-determinism tests: 2/2 passed.
- GPU frontier test: passed for resident and missing pages in and outside the
  prefetch band, with no cluster enqueues or descendant traversal.
- Terrain cut-error, capacity, movement, settle, and LOD-profile contracts:
  5/5 passed (217.15 seconds). Movement includes refinement while travelling.
- 5 km / 1220x720 output, default 610x360 internal raster, 256 m travel over
  600 frames after 400 warmup frames, then 400 settle frames: passed. Final
  near-field pages were 10 cm, all 219,600 ground-view pixels were LOD 0,
  holes/enclosed gaps were zero, and installation failures were zero.
  Last activity was frame 1055 (55 frames after travel ended), with 156 LOD-0
  source chunks and 509 resident chunks. Footprint was 5.35 GiB.

That moving run took 27.81 seconds. Earlier comparable runs took approximately
27–28 seconds, so no material end-to-end throughput gain is established.
The previous movement-fix run settled at frame 1078, but its final source cut
was different; the 23-frame difference is not a controlled prefetch speedup.

Logs: `/tmp/terrain-vg-fix/prefetch-cpu-tests.log`,
`prefetch-gpu-frontier.log`, `prefetch-terrain-tests.log`, and
`prefetch-moving-default-5km.log` in the same directory.
