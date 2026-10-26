# GPU terrain V2 Phase 2: bounded work infrastructure

**Status:** complete  
**Date:** 2026-08-27

## Outcome

Phase 2 supplies the reusable asynchronous infrastructure required by every
later terrain generation stage. It does not yet sample maps or emit terrain.
Its job is to make those later dispatches deterministic, fence safe, and
strictly bounded before they are connected to runtime streaming.

The implementation is split into four pieces:

- `TerrainGPUV2Scan` provides the CPU oracle and a GPU three-level exclusive
  scan for as many as 1,048,576 elements. Binary flags can be compacted in
  stable input order, and the final pass emits indirect dispatch arguments.
- `TerrainGPUV2ParameterRing` retains an immutable 128-byte job record for
  every submitted dispatch until its completion fence passes. The default 256
  lanes accommodate a 128-chunk quantum across normal frame latency.
- `TerrainGPUV2FencedArena` provides power-of-two scratch and hidden-output
  size classes with buddy coalescing. Cancellation and failure defer reuse;
  neither makes a live GPU address immediately allocatable.
- `TerrainGPUV2Scheduler` orders jobs by starvation, priority class,
  projected screen error, distance, canonical key, and stable sequence. It
  splits every stage into bounded workgroup quanta and learns a
  stage/shape/device cost from timestamp results.

## Scan contract

The GPU chain is nine ordered passes:

```text
reset
  -> scan elements
  -> scan level-0 block sums
  -> scan level-1 block sums
  -> add level-1 prefixes
  -> add level-0 prefixes
  -> validate binary flags
  -> finalize status + indirect args
  -> emit compacted indices
```

The graph is compiled once. Each pass owns a private uniform lane which is
updated without re-recording the graph, while dispatch dimensions cover the
declared capacity and shaders mask the inactive tail. This avoids a mutable
shared-uniform race between scan levels. Later consumers use the compacted
count's indirect arguments.

Overflow is fail-closed. Arithmetic overflow, a non-binary flag, or a compact
count larger than output capacity latches `overflow`, leaves indirect X at
zero, and prevents every compact-output write. CPU oracle helpers commit scan
or compact output only after validation for the same reason.

## Scheduling and budgets

`TerrainGPUV2Budget` has independent limits; admission must satisfy all of
them rather than treating a chunk count as a proxy for cost:

- adaptive target generation time and an absolute time ceiling;
- maximum time for one dispatch;
- chunks advanced and new chunks admitted per frame;
- total workgroups;
- virtual-atlas upload bytes;
- scratch and hidden-output reservations per frame;
- total scratch and hidden-output arena capacities;
- immutable parameter lanes; and
- publications per frame.

Foreground load scales the generation target down between 85% and 95% of the
frame target and disables compute generation above that range. Timestamp
underprediction becomes time debt on later frames. The learned key includes
stage, chunk shape, and device profile, so a cheap small-chunk measurement
cannot silently authorize a large shape on another device.

The first scheduler round reserves a configurable fraction for background,
near-frustum, or starved work. A final fair round recovers unused reservation
without crossing any hard limit. The default cap is 128 chunks, not a promise
to generate 128: the smallest remaining budget determines the actual subset.

Atlas transfers are scheduled separately and a chunk cannot enter compute
until its pages are confirmed resident. A zero-byte nonresident request is
rejected rather than remaining permanently stuck.

## Lifetime and publication rules

Cancellation is asynchronous. An active dispatch finishes, its immutable
parameter lane retires at the completion fence, and scratch/output leases are
then queued for deferred reclamation. There is no renderer drain or GPU-idle
operation in this API.

Outputs remain hidden through `ReadyToPublish`. Submission changes the job to
`SubmittedForPublication`; completion rechecks world-bundle revision, terrain
slot generation, and cancellation before accepting ownership. A stale result
is cancelled and its storage is fence-retired even if the publication command
itself reported success.

## Qualification

`SparseVoxelTerrainV2Phase2Tests` covers:

- CPU scan sizes 0, 1, 255/256/257, 65,535/65,536/65,537, and 1,048,576;
- randomized stable compaction and pre-write overflow behavior;
- buddy size classes, coalescing, and fence-deferred reuse;
- immutable parameter lanes across fences;
- projected-error priority and the absolute chunk cap;
- a 128-job queue constrained simultaneously by time, workgroups, scratch,
  output, parameter lanes, and atlas transfer bytes;
- second-round budget recovery and delayed timestamp convergence;
- cancellation and stale publication rejection; and
- zero renderer drains and zero wait-idle calls.

`SparseVoxelTerrainV2Phase2GPUTests` runs the actual Vulkan scan chain at all
three hierarchy boundaries and its maximum capacity, compares every active
prefix against the CPU oracle, checks randomized compact order and indirect
arguments, and poisons compact output to prove overflow occurs before writes.

Phase 3 can now build the virtual-atlas footprint and parallel canonical
column sampling on these bounded primitives.
