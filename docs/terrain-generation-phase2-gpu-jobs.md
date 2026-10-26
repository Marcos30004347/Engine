# GPU terrain generation Phase 2 job framework

Phase 2 establishes the bounded execution and lifetime framework used by the
later raster and Tree64 GPU builders. It does not generate production terrain
hierarchies and cannot publish a page unless a caller both requests a
publication schedule and explicitly accepts that publication.

The existing CPU `SparseVoxelTree64PageScheduler` remains unchanged and is
still the production fallback and publication path.

## Engine-owned control plane

`TerrainGPUBuildScheduler` owns:

- the full stage machine from map readiness through validation;
- immutable bundle, chunk, LOD, generator, slot, and terrain-generation keys;
- fixed-capacity scratch allocations with fence-deferred reclamation;
- bounded column uploads, page starts, dispatches, publications, scratch, and
  resident jobs;
- deterministic priority recomputation;
- per-stage exponential-mean, recent-percentile, and decaying high-water
  timing estimates;
- render-pressure throttling, predictive admission, and GPU time debt;
- cancellation, stale-generation rejection, failure results, and diagnostics.

The scheduler issues bounded `TerrainGPUDispatch` records. A renderer or test
executor completes those records with a completion fence, optional delayed
timestamp, and fail-closed status. It does not infer completion from frame
age. Completed timestamp records use a fixed diagnostic history; a backend
that permanently loses timer readback increments `droppedTimingSamples`
instead of growing scheduler memory without bound.

One job advances at most one GPU quantum per scheduling call. Current synthetic
quanta model the future production work:

| Stage | Quantum |
| --- | ---: |
| Column sampling | 16 rows |
| Boundary finalization | one page |
| Fine brick count/emission | 256 candidates |
| Compaction | one scan level |
| Brick reduction/emission | one hierarchy level |
| Tree64 construction | one level |
| Sidecars | 16 rows |
| Validation | one page |

When a measured quantum is larger than the target or admission ceiling, the
scheduler reduces the next work-unit range. Time debt consumes future frame
tokens and decays without creating positive spare-time credit, so a single bad
estimate cannot cause either a burst or permanent starvation.

## Scratch lifetime

`TerrainGPUFixedScratchAllocator` uses deterministic first-fit allocation and
coalescing free ranges. A handle can be submitted for release exactly once.
The allocation remains unavailable until its recorded GPU fence completes.

Queued cancellation releases at the latest completed fence. In-flight
cancellation marks the job and waits for that dispatch's completion fence.
Stale in-flight work can finish physically, but it cannot advance to validation
or publication. Duplicate and malformed releases are counted as allocator
errors rather than silently accepted.

## Fixed render-graph boundary

`TerrainGPUBuildSyntheticChain` registers ten `TerrainGPUBuild.Phase2.StageN`
passes and all buffers once at initialization. Per-frame `update()` writes only
fixed-size work records and indirect dispatch counts. Inactive stages have a
zero dispatch count.

The synthetic WGSL kernel hashes the generation key and bounded work range into
a diagnostic result. No synthetic result buffer is connected to raster pages,
Tree64 pages, terrain generations, the publication CAS, or lighting. This is a
deliberate safety boundary until Phases 3 through 5 provide hierarchy builders,
validation, and atomic publication.

## Default limits

The default interactive budget is:

| Limit | Default |
| --- | ---: |
| Target GPU build time | 750 us/frame |
| Admission ceiling | 1250 us/frame |
| Dispatches | 8/frame |
| Pages started | 2/frame |
| Pages published | 4/frame |
| Column upload | 4 MiB/frame |
| Scratch | 64 MiB |
| Resident jobs | 8 |

Foreground GPU load below 85% leaves the terrain target unchanged. From 85%
to 95% it scales linearly to zero; at or above 95%, GPU build admission is
zero. Column upload and already validated publication retain their independent
limits.

## Validation

Build the scheduler suite and synthetic shader, then run the Phase 2 test:

```sh
cmake --build build \
  --target SparseVoxelTerrainPhase2Tests \
           SparseVoxelTerrainPhase2GPUTests terrain_gpu_build_synthetic -j 8
ctest --test-dir build \
  -R 'SparseVoxelTerrainPhase2(Scheduler|SyntheticGPU)' --output-on-failure
```

The suite independently exercises target and ceiling admission, adaptive
quanta, dispatch/page/publication/upload/scratch/resident limits, priority and
stable ties, delayed timings, time debt, zero budget, permanent render
pressure, stale queued and in-flight work, every cancellation ownership state,
failure injection, deferred fence reclamation, and randomized lifecycle churn.
The GPU smoke test creates the complete fixed graph, runs simultaneous sampling
and validation stage records through indirect dispatches, and verifies their
diagnostic results by readback.

The close-range light-field flicker and brick discontinuities reported before
Phase 2 are outside this phase: live terrain output and lighting behavior are
unchanged. Those visual gates become actionable when GPU raster pages can be
built and atomically published in Phases 3 and 5.
