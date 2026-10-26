# Terrain GPU V2 Phase 8: Runtime Integration and Adaptive Budgeting

Phase 8 adds the renderer-independent runtime control plane that turns terrain
coverage requests into bounded Phase 2 scheduler work. It keeps generated
columns, occupancy, Tree64, and publication payloads GPU-owned: runtime state
contains keys, priorities, leases, fences, timestamps, and diagnostics only.
It never copies generated geometry or volume data to a CPU cache.

The shipping renderer switch remains Phase 10. Until the Phase 9 visual gates
pass, this runtime is the `terrain-gpu-v2` qualification path and the known
correct CPU column/build path is its fallback. The old monolithic GPU page
builder is not a V2 fallback.

## Request and priority model

`TerrainGPUV2Runtime` accepts either one bounded 3D chunk or a region. A region
is decomposed across X, Y, and Z into independently keyed jobs with distinct
terrain slots. This makes vertical terrain stacks explicit and prevents one
large horizontal page from becoming the scheduling unit.

Every frame, live jobs are re-scored from the camera and viewport:

```text
projected error = geometric error * pixels per radian / distance to chunk AABB
```

Priority class is evaluated before error, then distance. Visible missing
coverage therefore outranks visible refinement, near-frustum prefetch, and
off-screen background work. Within a class, larger projected error wins.
Re-scoring happens before each frame schedule, so camera motion changes queue
order without rebuilding or reading back payloads.

## Bounded asynchronous submission

`beginFrame()` supplies the Phase 2 scheduler with foreground frame time,
completed fence, device profile, frame target, and publication permission. The
scheduler may advance many jobs, but it cannot exceed any of these independent
limits:

- adaptive generation-time target and hard ceiling;
- maximum chunks and newly admitted chunks per frame;
- maximum workgroups per frame;
- scratch and output reservation bytes;
- atlas upload bytes;
- parameter-ring lanes; and
- publication count.

The returned schedule contains atlas uploads, stage dispatches, and
generation-checked publications. The renderer records these into its normal
command graph and reports completion fences later. Phase 8 introduces no queue
idle, frame drain, or synchronous result read. A delayed timestamp can arrive
after dispatch completion; its immutable dispatch record remains alive until
the timing sample is consumed.

The default scheduler admits at most 128 chunks, but 128 is only a safety cap.
GPU-time, workgroups, arenas, atlas transfer, and parameter lanes commonly
produce a smaller actual count. This is intentional.

## Timing controller

On devices with timestamp queries, per-dispatch measurements update the stage
cost model and per-frame totals. The runtime records a rolling 256-frame
window, reports generation p99, and opens the session circuit breaker after a
configured run of hard-budget violations.

Without timestamps the runtime selects a conservative profile:

- 0.5 ms target;
- 1.0 ms hard ceiling; and
- at most 8 chunks per frame.

Predicted dispatch cost is used to retire timing records in this mode, so lack
of timestamp support does not accumulate unbounded timing debt.

## Device qualification and fallback

GPU mode requires compute support, a qualified device profile, at least 256
compute invocations per workgroup, at least eight storage buffers per stage,
a storage-buffer limit large enough for the output arena, and device-local
memory for residency plus scheduler scratch and output arenas.

The session switches to compact CPU rebuild requests for:

- explicit CPU mode;
- missing compute or insufficient device limits;
- an unqualified device profile;
- device loss;
- repeated atlas, allocation, dispatch, validation, or publication failures;
  or
- sustained hard-budget violations.

Fallback requests carry the exact world revision, signed 3D chunk coordinate,
LOD, generator revision, terrain slot, expected generation, and shape. They do
not carry generated payload. Active GPU jobs are cancelled, their leases retire
behind fences, and stale late publications cannot become committed ownership.

## Telemetry and qualification gate

`TerrainGPUV2RuntimeTelemetry` and `overlayText()` expose:

- active mode and persistent fallback reason;
- jobs by every V2 stage plus aggregate queue state;
- selected chunk shape and maximum vertical stack;
- submitted dispatches, workgroups, predicted time, measured p99, and budget
  remaining;
- atlas ready/waiting tiles, faults, pins, upload bytes, and retries;
- accepted/rejected publications;
- committed, hidden, retired, scratch, and total residency;
- nearest visible error, distance, queue rank, and LOD0 refinement latency;
- CPU fallback request count; and
- renderer-drain and wait-idle counters.

Qualification is deliberately stricter than successful generation. It also
requires generation p99 below the hard ceiling, p95 interactive frame cost no
more than 110% of the reference, zero synchronization drains, and all eight
camera-motion / temporal-upscaling / lighting combinations. Raw raster must be
stable first; lighting stability is evaluated only for lighting-enabled
captures.

## Phase 8 tests

`SparseVoxelTerrainV2Phase8Tests` verifies:

- near visible work wins a one-chunk budget over far and off-screen work;
- a 4 x 3 x 2 region becomes 24 independent 3D jobs;
- caps of 1, 8, and 128 publish identical final chunk keys while changing only
  throughput and latency;
- multi-job submission advances 16 fitting chunks in one frame;
- a three-workgroup budget advances three jobs even with a 128-chunk cap;
- immediate, delayed, and no-timestamp accounting remain asynchronous;
- generation p99 and LOD0 refinement latency satisfy their limits;
- the complete capture/performance qualification matrix is enforced;
- missing compute, device loss, and repeated build errors produce compact CPU
  rebuild requests; and
- static configuration rejects queue drains and generated-payload read paths in
  the runtime source.

Phase 9 now owns close-ground visual qualification: canonical face identity,
raw G-buffer stability, crack/overlap detection, stationary shadow stability,
upscaler isolation, and the 1 cm camera case.
