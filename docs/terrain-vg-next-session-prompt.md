# Next session prompt — terrain virtual geometry, remaining phases

Paste the section below into a fresh session. Everything after it is reference
material that session can read on demand.

---

## The prompt

Continue the terrain virtual-geometry work in `/Users/marcosvinicius/workspace/Engine`.

Read `docs/terrain-vg-unified-hierarchy-plan.md` first. Phases 8d, 8e, 8f, 8g,
9 and 10 are **closed**. The work is on branch `terrain-vg-phase10`. `main` is a
different line — do not merge onto it without asking.

**Where this stands.** The virtual-geometry route no longer touches the brick
path to draw or to drive its cut: no brick raster, no brick culler, no brick
feedback, `visibleBricks=0`. The spatial cut is built from the same
projected-voxel metric the LOD profile contract judges, so at 5 km the profile
is now *exact* — 510 resident chunks, zero chunks over the pixel budget, worst
0.00 px, settled at frame 229. Pages are read from the scratch file by the
scene itself, retired chunks revive from it instead of re-meshing, and the
builder's back half is a named seam with byte-identical parallel fan-outs.

**What is left, in order:**

1. **The GPU brick arenas.** The CPU brick hierarchy can never leave: it is the
   input geometry the VG mesher meshes from
   (`buildSparseVoxelOctreeVirtualGeometry(chunks.bricks, ...)`). What can go is
   the *GPU* arenas and the CPU mirrors kept for upload, and the blocker is that
   the terrain **publication rides on the upload pass**, which is registered
   only when the payload is — with it gone every publication is refused with
   "a terrain GPU publication is already pending" and the cut freezes at 6
   chunks. Teaching the publication to complete with no brick ranges in its
   batch is the task. `--terrain-vg-no-brick-payload` reproduces the refusals.
   The geometry field cache is already gated for this (capacity zero means "no
   geometry fields this run"); it will need re-sourcing, which the user has said
   the lighting rewrite will cover.
2. **The lighting rewrite** is coming and will touch the same publication
   pipeline; `brickVoxelGeometryFieldCache` is the input to re-source, and
   everything `buildSparseVoxelGeometryFieldCache` reads is available from the
   chunk set and the material manager except the brick occupancy it walks.
3. **Optional, measured inert:** the projected-voxel spatial demand feed
   (`--terrain-vg-no-spatial-demand`) names no chunk the rendered-error band has
   not already claimed. It is kept because it is correct by construction rather
   than by coincidence; delete it if it ever costs anything.

Leave phase 7b–7e alone unless asked — design, not staged work.
`FileEncodingTests` crashes (bus error, varying page of its 66,000-page suzanne
fixture) and crashes at the baseline too; it is not in the gate list and it is
not yours.

**Run discipline is mandatory** (three machine reboots came from ignoring it).
Recreate `safe_run.sh` in the session scratchpad from
`docs/terrain-vg-unified-hierarchy-plan.md` and the `sample-run-discipline`
memory — it is session-scoped and will be gone. Build only with
`taskpolicy -c background nice -n 19 cmake --build build-release --target <one target> -j 4`,
never concurrently with a run. This machine is 16 GiB unified; ~512 MiB is the
working VG pool ceiling.

**Gates that must stay green:**

```
ctest -R "SparseVoxelTerrainVGSettleContract|SparseVoxelTerrainVGLodProfileContract|SparseVoxelTerrainVGScratchFile|VirtualGeometryPackerDeterminism|SparseVoxelTerrainVGSlotInstall|SparseVoxelOctreeVirtualGeometryBuild|SparseVoxelTerrainVirtualGeometryPhase14SceneRuntimeInstall|SparseVoxelTerrainVirtualGeometryPhase17ComputeRasterContract|SparseVoxelTerrainVirtualGeometryRuntimeRefinement"
```

Reference run:

```
--world-map world-data --terrain-render-distance-km 5 --resolution 1220x720 \
--lod-threshold 1.5 --frames 300 --disable-vsync --test-camera-walk \
--terrain-vg-memory-mb 512 --test-frame-signatures --test-capture-start-frame 0 \
--terrain-vg-synchronous-streaming
```

Always pass `--lod-threshold 1.5` on `--frames` runs or you get the historical
64 px test cut. Numbers to hold: **510** resident chunks, LOD profile
`violations=0 worstPx=0.00 overBudget=0`, 219,600 covered pixels, 0 install
failures, 0 root repairs, `visibleBricks=0`.

A/B levers added across these phases: `--test-camera-out-and-back` (with
`--test-camera-travel-m`), `--terrain-vg-no-scratch-retain`,
`--terrain-vg-brick-feedback`, `--terrain-vg-no-brick-payload`,
`--terrain-vg-no-lod-coupled-cut`, `--terrain-vg-no-spatial-demand`, and
`VG_PACK_THREADS=1` for the builder fan-outs.

Do not run `TriangleExample`, `TriangleReverseZ` or `VirtualGeometryRenderer` —
windowed samples with no termination gate, deregistered from ctest.

## Reference: metrics that lie, and what to read instead

This cost several rounds of work in the previous session. Read this before
trusting any measurement.

- **`interiorCoverageGapPixels` understates missing terrain by two orders of
  magnitude.** It counts only pixels enclosed by terrain *above and below in the
  same screen column*, so terrain replaced by sky does not register: a
  configuration reading 12.98 was drawing 98,824 terrain pixels against 219,600.
  **Read `terrainPixels` beside it.** The settle contract now asserts the final
  frame keeps 80% of the run's peak.
- **`visibleBricks` is a cull statistic, not a draw statistic.** Terrain is
  virtual-geometry only: under `route=virtual-geometry` the brick resolver is
  registered `feedbackOnly`, so `BrickPhase*.Reset/.Cull/.DrawArgs` are
  registered and **no draw pass at all**. A rise in `visibleBricks` never means
  the brick raster covered anything.
- **`bareAfterRetire` used to be inverted** and is now correct; it reads 0.
- **A coarse cut does not show up in the coverage counters at all.** The
  streaming-leaf marks keep the frontier drawing, so terrain that is two LOD
  levels too coarse still reports 219,600 covered pixels and 0 holes. Read the
  **LOD profile line** (`violations`, `worstPx`, `worstLevels`) and the resident
  chunk count; that is how phase 8e's removal was caught.
- **`--test-frame-signatures` is no longer a reliable A/B lever on this
  configuration.** Three runs each: phase 10 off gives 59351/59351/59348 drawn
  clusters and one hash; on gives 70206/70938/70938 and two hashes. Residency
  and the LOD profile are deterministic; the drawn set and final image are not.
  This is unexplained. Use the settle line, the LOD profile and the gap/terrain
  pixel counters for A/B, not the hash.
- **Believe the gap metrics over the counters.** Two "can the scene take this
  chunk" gates were written and both deleted for never firing once in 300
  frames.

## Reference: invariants now asserted, do not regress them

- A page holds whole groups; page dependencies admit an in-order install; and
  **every prefix of the install order is a valid cut** — 0 holes, 0 overlaps at
  every budget (`testEveryInstallPrefixIsAValidCut`).
- A group's coarse cluster's `self` and each child's `parent` are the *same*
  bound, sphere and error (`testGroupBoundsAreSharedByChildrenAndParent`).
- Every cluster is consumed by at most one group.
- The cross-chunk clamp: no parent error above `parentChunkError`, every finest
  cluster owns `[leafError, parentChunkError)` exactly, at the parent chunk's
  own error this chunk selects nothing, just under it it still draws.
- A cluster mask applied after install survives the uninstall replay only via
  `persistIntoPageUpdates` → `RuntimePageStatus::externalUpdates`. Folding it
  into `installUpdates` instead breaks a second contract: both the install path
  and the replay read `installUpdates.empty()` to choose a node's *default*
  mask.
- Terrain page installs are ancestor-closed: a page whose declared dependencies
  are not installed is refused, and a chunk installs the page *prefix* up to and
  including its root page (`rootPageIndex` is not always 0).
- The cut's radial bands: `giantRefinable` scaled against the drawn set, and
  `tiny` (drawn far finer than the distance needs) under 20% of drawn.

## Reference: the one open design question

**One surface per pixel.** The default is additive — a streaming leaf's self
error is forced to zero, so the finest resident representation draws at every
budget and coverage is total. `SVR_TERRAIN_VG_YIELD_MODE=1` makes the cut pick
one surface, removes most of the overlap (giant clusters 2,744 → 730, peak
triangle 117 px → 39) and costs 219,600 → 98,824 covered pixels.

Inside an asset this is already solved and always was: a page holds whole
groups, declares its dependencies, and `streamingBitsetForGroup` marks a cluster
a streaming leaf exactly while any DAG child sits on a page that is not
resident. The narrow failure is a **leaf chunk with no resident parent**: its
band is `[v, 2v)`, and when the budget wants coarser than `2v` nothing covers
it. That is chunk-local, not a cross-chunk protocol — keep a leaf's band open
upward. Measured as roots-infinite it traded away refinement demand (LOD0 chunks
40 → 20, worstPx 2.89 → 3.32, root repairs 20 → 276), so the demand metric has
to be decoupled from the band first.

## Reference: diagnostics

- `SVR_TRACE_TERRAIN=1` — cache stage timers, job phases, commit block, demand
  line (`vgDemand=a/b`: `b` is the candidate count; `a` zero while `b` is at the
  cap means the queue is full of things that cannot be acted on).
- `SVR_TRACE_TERRAIN_HOLES=1` — per-frame hole telemetry.
- `SVR_LOG_CPU_SECTIONS=N`, `RG_DUMP_ORDER=1`, `SVR_WAIT_IDLE_EACH_FRAME=1`.
- A/B knobs with recorded measurements: `SVR_TERRAIN_VG_YIELD_MODE`,
  `SVR_VG_PARENT_CHUNK_ERROR_SCALE`, `SVR_VG_PARENT_CHUNK_ERROR_ROOTS`,
  `SVR_VG_VANISHING_CONSUME`, `SVR_VG_PAGE_KIB`, `SVR_TERRAIN_VG_NODE_BOUNDARY`.

## Reference: known-red, not yours

- `SparseVoxelTerrainVirtualGeometryGBufferSmoke` fails in a large back-to-back
  GPU sweep and passes alone — device degradation, re-run alone before believing
  it.
- The generated route (`--terrain-generation gpu-v2`) device-timeouts on this
  machine. Its new page-priority feedback is verified to build, to be internally
  consistent and to satisfy its contract — **not to run**. It has no
  settle/profile gate of its own, and its failure mode is a cut that silently
  stops refining; that gate is worth writing before trusting it.
