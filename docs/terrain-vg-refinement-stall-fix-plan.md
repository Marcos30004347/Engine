# Terrain VG refinement stall: diagnosis and fix plan

Investigated 2026-09-08/09 at `8f02fd6e`. The investigation below records the
original failure and subsequent implementation. The main geometry defect was
translation-dependent reconstruction of octree parent links; the builder now
uses the authored topology. Further fixes cover closed block faces, complete
root residency, dependency references, radial source demand, capacity backoff,
and fallback retirement. Validation results are recorded below.

## Reproduction and findings

User command:

```sh
./build-release/tests/SparseVoxelRendererSample \
  --world-map world-data --terrain-render-distance-km 5 --resolution 1220x720
```

This selects CPU terrain generation, encoded VG pages, asynchronous streaming,
and the normal spawn camera. The interactive raster threshold is 1.5 px.

**There are multiple contributing defects, rather than a generally broken page
installer.** Coarse ancestor chunks remain drawable after finer chunks arrive;
the generation planner repeatedly rejects refinement batches at its slot limit;
and the default decoded pool develops eviction/repair churn that prevents some
pending pages from installing. Increasing the page-install budget alone cannot
resolve these problems.

### Measurements

Used the existing executable, serial runs, a 6 GiB process memory guard, and a
180-second external watchdog. A three-frame smoke passed first. The bounded
default reproduction added `--frames 300 --lod-threshold 1.5 --test-static-scene
--memory-budget-mb 6144`. The explicit threshold prevents `--frames` from
selecting the historical 64 px test threshold; `--test-static-scene` prevents
the bounded-run instance animation. No scripted camera walk or synchronous
streaming was enabled.

The sample logged an internal raster resolution of 610x360 for the requested
1220x720 output; the culler's pixel-error measurements refer to that internal
resolution.

A separate 300-frame diagnostic used `--terrain-vg-memory-mb 512` to give decoded
geometry more room. That run did not set `--test-static-scene`, so its precise
draw counts are not a strict single-variable A/B with the default run. Both
completed successfully and exhibited the same coarse-cluster failure.

| Observation | Default decoded budget | 512 MiB decoded diagnostic |
|---|---:|---:|
| Resident terrain chunks at end | 486 | 489 |
| LOD0 chunks | 32 | 32 |
| Installed VG pages, frame 240 | 1,283 | 1,753 |
| Evictions by end | 436 | 0 |
| Page-prefix repairs by end (`rootRepairs`) | 138 | 0 |
| Reported page-install failures | 0 | 0 |
| Oversized streaming-leaf clusters at end | 3,675 | 3,707 |
| Maximum drawn triangle edge | 165.68 px | 165.68 px |
| Rejected generation transactions | 51 | 49 |
| LOD-profile violations | 0 | 0 |

At default capacity, frame 240 used **7,108,764 / 7,456,536 decoded indices**
(95.34%), while encoded pages occupied only 177 / 512 MiB. The bottleneck was
not the encoded-page buffer. At frame 299 the demand log still reported
`pagesPending=6 pageInstalls=0`. The scene successfully served 64 page requests
from scratch-file locations across the run, with zero source-read failures.

Raw logs are in `/tmp/terrain-vg-diagnosis/{smoke,default,baseline,yield}.log`;
these are session artifacts, not committed fixtures. `baseline.log` is the
512 MiB diagnostic described above.

## 1. Coarse fallback clusters never relinquish their ground

Relevant code:

- `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp:18736`,
  `republishTerrainVGSharedParent`, particularly the default `yieldMode=0`
  around line 18835.
- `src/editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.cpp:731` marks
  the finest clusters of a chunk as streaming leaves.
- `assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl:1351` substitutes
  zero for a streaming leaf's self error.

The default parent republish computes `yieldChildMask=0`, even when child chunks
are installed. Its cross-chunk rewrites therefore retain the coarse chunk's
finest streaming-leaf bits. With effective self error zero, those clusters pass
the self-error test regardless of how oversized they are. They can coexist
with the finer representations; ordinary depth testing does not guarantee
that finer terrain wins, because the two resolutions have different surfaces.

The 512 MiB diagnostic reported `yieldBits=0/574288`, zero oversized *refinable*
clusters, and 3,707 oversized *streaming-leaf* clusters. This identifies a
fallback/LOD ownership defect, not a generic failure of the self-error test.

Demand from these retained ancestors is largely unactionable. At the final
frame, 326 of 364 demand candidates had no current terrain chunk record.
`recordOfSlot` searches the current leaf records, while the VG scene retains
ancestor geometry. The demand loop skips those ancestors. More page reads or
larger candidate arrays cannot make their streaming marks relinquish coverage.

**Do not ship the existing yield switch as the fix.** An exploratory run with
`SVR_TERRAIN_VG_YIELD_MODE=1` and the same 512 MiB diagnostic settings reduced
covered pixels to 109,281 at frame 188, versus 219,600 in the baseline's settled
view. It then exited through the 6 GiB memory guard at 6.02 GiB. This was not a
completed or validated fix run; the large coverage change needs pixel-level
inspection, rather than assuming the removed coverage was all valid terrain.

There is also a concrete boundary inconsistency to resolve: the cache builder
assigns finite `parentChunkError = 2 * voxelSize` to shared chunks regardless of
whether a drawable parent exists (`TerrainVirtualGeometryPageCache.cpp:495`).
Runtime parent changes update the chunk-root hierarchy gate
(`terrainVGSharedParentLeafError` / `republishTerrainVGSharedChildRoots`), but
do not equivalently rewrite every encoded cluster's parent-error band. Simply
clearing fallback bits is therefore insufficient to establish a complete cut.

## 2. Refinement admission ignores both final and transient capacity

`src/voxel/svraster/PlanetVoxelChunk.cpp:9281` builds a feedback-driven desired
cut by applying all accepted requests. Splitting a quadtree leaf adds three
net leaves. The budget-fitting code at line 9430 is in the other branch and
does not run for this feedback-driven cut.

Two distinct failures were reproduced:

1. With 489 resident chunks, eight requested splits produce
   `489 + 8 * 3 = 513`, exceeding the 512 limit. The whole transaction fails at
   line 9569. The same demand is retried indefinitely. The error incorrectly
   claims that this cut was already coarsened as far as possible.
2. With 486 resident chunks, eight splits produce a legal final cut of 510,
   but require 32 new child slots while all 486 old owners remain protected:
   `486 + 32 = 518`. The allocator around line 9874 protects existing and staged
   owners, assigns invalid slots to the excess children, and generation fails
   at line 10025: `Generated-world spatial leaf cut exhausted its slots`.

The default route also disables both available merge feeds:
`gTerrainVGCoarsenEnabled=false` at sample line 391, and brick feedback merge
votes are disabled for the VG resolver at line 25488. There is no normal
feedback-driven exchange of distant detail for new visible detail at capacity.

## 3. Pool pressure produces stalled installs without install failures

`terrainVGDecodedBudgetMiB()` defaults to 256 MiB on this 16 GiB machine.
`installTerrainVGRequestedPages` at sample line 18554 stops before attempting an
install when any pool exceeds 95% and its eviction attempt returns zero.
Consequently `installFailures=0` is not evidence of available capacity or of
serviced demand. Reinstalling page prefixes after eviction also consumes the
same budget as new detail.

The default run's 436 evictions, 138 repairs, saturated index arena, and pending
pages demonstrate pressure and churn. The larger decoded diagnostic eliminates
that churn but leaves the 165.68 px coarse geometry and planner refusals intact.
Pool size is a contributor, not the complete diagnosis.

## 4. Existing diagnostics can miss the visible failure

The LOD profile at sample line 37783 examines only pages touched by the culler,
maps them to current leaf records, and skips retained ancestors without records.
It measures the record's nominal voxel size, not the actual selected cluster's
LOD. Both completed runs report zero violations despite thousands of oversized
streaming leaves. Near-field checks can also become vacuous when the touched
set contains no near-field chunks.

`tests/voxel/SparseVoxelTerrainVGCutErrorContract.cmake:51` parses
`${CONTRACT_LINE}` without ever extracting or assigning it from `${LOG}`. Fix
the parser and require a match before trusting this gate. This investigation
did not run its 1,200-frame GPU scenario.

The logged `vgRequests` is `vgFeedbackRequests`, incremented for the generated
route; it is not the size of the CPU route's `vgRequests` page-request vector.
Its persistent zero is not proof that the culler requested no VG pages.

## Implementation sequence

### A. Establish a regression that measures the actual failure

1. Repair the cut-contract parser: extract the last contract line, reject
   missing/malformed fields, then evaluate it.
2. Add a bounded default-camera 5 km regression with the interactive threshold,
   asynchronous streaming, and no test animation. Keep the scripted walk as a
   separate scenario.
3. Report drawn clusters by owner address, spatial LOD, actual raw error, and
   fallback reason: missing intra-chunk page, missing child chunk, retained
   ancestor with ready replacement, or terminal LOD0. Include ancestors in
   quality checks. Large greedy triangles alone are not a geometric-error bug.
4. Separate raw page requests, installs, generation demand, admitted groups,
   completed publications, deferred work, and rejection reasons. Report unmet
   demand age; repeating a request is not progress.

### B. Make generation admission capacity-aware

1. Plan complete replacement groups in demand-priority order before mutating
   the desired cut. Account for final leaf count **and** peak coexistence of
   published owners, staged children, and resources awaiting safe retirement.
2. Admit the largest useful set of whole groups that fits; retain the rest as
   deferred requests. A rejected eighth split must not prevent the first five
   or six feasible groups from completing. Do not reuse a live parent's slot
   before publication and the required GPU lifetime have completed.
3. Reserve enough transition capacity for at least one replacement group, or
   provide an explicitly bounded staging pool. Apply the same rule to merges.
4. At steady capacity, schedule eligible, low-priority sibling-group merges
   before higher-value refinement, with hysteresis and minimum residency.
   Keep valid coverage throughout. Do not globally coarsen the camera's near
   field just to fit an unfiltered request batch.
5. Test 489/512 final-cut overflow and 486/512 transient overflow directly,
   plus staged siblings, retries, stale generations, and merge/refine cycles.

### C. Establish a real cross-chunk replacement contract

1. Represent the relation between a parent's clusters and their replacing
   child groups explicitly. A child being registered or having its root page
   installed is insufficient: require a drawable replacement cut over every
   relevant region, including completed decode/publication.
2. Use shared transition bounds and error values on both sides of each LOD
   boundary. A scalar `2 * voxelSize` on unrelated spheres does not by itself
   guarantee complementary screen-space tests. When no coarser representation
   covers a chunk, keep its coarse band open in both traversal and cluster
   selection, not just its root stub.
3. Clear the parent's fallback streaming bits only when this contract holds;
   retain the parent for budgets that legitimately choose coarser detail.
   Restore fallback eligibility before evicting a required child. Persist
   these rewrites through the scene's external-update/replay mechanism.
4. Test every install/eviction prefix across two or more chunk levels, including
   partial siblings, root-only pages, parentless chunks, and asymmetric terrain
   heights. Require complete ownership with neither missing replacement regions
   nor simultaneous coarse/fine ownership at the same chosen error band.
5. Only enable yielding by default after these invariants and image checks pass.

### D. Keep page demand and residency useful under pressure

1. Route ancestor demand to its missing replacement pages or current descendant
   owners. Handle missing pages before rejecting an owner as LOD0 or absent
   from the leaf-record array; an LOD0 chunk can still lack finer asset pages.
2. Keep demand based on raw geometric error and required replacement coverage,
   independent of the zero self error used to draw a fallback. Do not make
   successful discovery depend solely on already-rendered fine geometry.
3. Audit hierarchy visibility gates with a targeted occlusion-on/off test:
   page priority is written after hierarchy occlusion at shader line 1119.
   Determine whether a coarse fallback's depth suppresses requests for its own
   replacement. This is a remaining hypothesis, not a measured root cause here;
   retain occlusion for normal drawing.
4. Budget encoded bytes, clusters, decoded vertices/indices, and deferred
   releases independently. Schedule requested pages by benefit and dependency
   closure; retain requests across deferred-release waits. Distinguish
   dependency-preserving eviction from repeatedly evicting and repairing the
   same useful prefix.
5. Reclaim redundant ancestor resources once the replacement contract permits
   it. Avoid solving retention of all coarse roots by unbounded pool growth.

## Acceptance criteria

- The user's normal launch refines after startup and after free-camera movement.
- Valid high-priority replacement groups continue publishing near capacity;
  neither reproduced batch is rejected repeatedly with unchanged state.
- A ready fine replacement removes oversized coarse fallback selection where
  the pixel budget demands it. Unmet demand has an explicit actionable or
  capacity-deferred state, rather than disappearing into `noRecord`.
- Page installs remain dependency-closed; memory-pressure tests converge without
  persistent repair/re-eviction loops, stale page generations, or partial cuts.
- Judge visible geometry and terrain coverage together, including sky silhouettes
  and depth. A lower giant count or a zero nominal LOD-profile score alone is
  insufficient.
- Run focused planner tests and existing slot-install, scratch-file, packer,
  octree, phase-14 runtime-install, and runtime-refinement tests, then bounded
  default-camera, look-away/return, and constrained-memory GPU checks serially.
  Respect the memory guard; build one target at background QoS with at most four
  jobs, separately from GPU runs.


## Implemented correction

The intended cut uses 10 cm spatial LOD0 terrain near the camera. Each chunk
retains its complete internal VG hierarchy, so the culler can select coarser
pages with distance. A spatial ancestor supplies temporary coverage until the
replacement child's root-page dependency prefix is installed.

- Admit complete ownership groups against peak occupied slots, including staged
  payloads. Preserve already balanced cuts when capacity defers a neighbouring split.
- Remove the finite cross-chunk parent-error clamp. Independent chunk assets
  cannot share an upper LOD band without a real cross-chunk simplification DAG.
- Disable parent clusters whose XZ footprint is covered by drawable children.
  Reconstruct masks after page installation or eviction. Stop traversal of
  completely replaced ancestors so their missing pages cannot consume the LRU.
- Patch traversal gates in the live scene hierarchy, preserving the cluster
  offsets and residency bits written by page installation.
- Serve missing asset pages before filtering demand by current spatial owner or
  LOD0. Count prefix repairs and both install feeds against one frame budget.
- Project streaming demand using the drawn cluster's own error sphere. Compute
  CPU height distance in the camera's shared elevation frame.
- Set the automatic decoded arena to 768 MiB on machines with more than 8 and
  at most 16 GiB RAM; retain the smaller/larger machine tiers and explicit
  budget override. Closed-block geometry exceeds the index allocation of the 512 MiB tier.
  Distance LOD remains GPU page selection within each complete chunk hierarchy;
  camera movement continues to update the spatial coverage cut.

Regression coverage includes final and transient slot pressure, staged payloads,
balancing dependencies, parent-mask replacement/restoration, and live hierarchy
patch preservation. The bounded default-camera GPU regression also requires
visible 10 cm detail and bounded rendered fallback error, not just installed
chunk counts.

The earlier all-screen coverage baseline was misleading: image inspection
showed that oversized coarse faces occluded the sky and distant landscape.
Removing those faces legitimately reduces terrain pixel coverage. Pixel counts
must be interpreted together with the rendered surface and geometric error.


### Validation before closed-block meshing

- Release build and regenerated culling shaders succeed.
- World-cut admission and chunk-slot rewrite tests pass.
- The scene runtime-install GPU test passes, including the new check that
  disabling/restoring traversal preserves all live page-install fields.
- A 300-frame 5 km run with ancestor replacement and explicit 512 MiB
  arena reached 32 spatial LOD0 chunks, with visible 10 cm terrain, no near-field
  coarse chunks, no LOD-profile violations, no page-install failures, no
  evictions or prefix repairs, and no rejected capacity transactions. It
  installed 1,885 pages by frame 240. Maximum streaming-leaf error was 1.84 px;
  maximum triangle edge was 30.90 px (greedy flat faces can have large edges
  while meeting the geometric-error budget). Three spatial refinement requests
  remained deferred at the 510/512 occupied-slot plateau.
- This result predates closed-block meshing; the current default is 768 MiB,
  as described below.

The optional experimental spatial merge feed is left at its existing default
(off). An A/B run exposed a separate empty-feedback CPU fallback that can undo
recent refinement while that feed is active. GPU page LOD selection with
camera distance does not require enabling this experimental feed.


### Closed bricks, radial source refinement, and shared positions

The follow-up crack report exposed a separate meshing contract violation:
faces were removed against neighbouring bricks and across the boundaries of
independently selectable blocks inside a brick. Adjacent blocks can be selected
at different octree levels, so those faces cannot depend on the neighbour's LOD.
The builder now closes all six sides of every selectable block, while still
removing interior faces between occupied cells within that block. Greedy merging
respects both block boundaries and material boundaries. Groups without a coarse
replacement retain their roots, matching the normal VG DAG convention.

Shared vertices now come from the canonical integer world lattice using one
double-precision evaluation. Reconstructing vertices by adding a rounded brick
minimum to a rounded cell offset gave adjacent bricks different shared planes.
Regression coverage includes independently meshed neighbouring bricks at different
LODs with deliberately perturbed float minima.

Spatial source demand filters by projected voxel size and visits resident chunks
from the camera outward, independently of the viewing direction. It no longer depends on which pages the culler touched or
waits for the entire current asset to become resident. GPU culling continues to
select geometry pages within the generated octree DAG. The automatic near-field
full-asset install heuristic has been removed: installation follows requested
page dependencies, with full-asset installation reserved for the explicit debug
switch. The first closed-block/radial experiment with the old eager install
heuristic saturated the decoded pool and reached the 6 GiB process guard; it is
not a successful validation run.

The planner now distinguishes deferred GPU requests from unfinished CPU bootstrap
or camera-motion work. An idle feedback frame cannot use a GPU backlog as a
reason to replace the refined cut with the coarser CPU target. World-cut and
page-determinism regression suites pass, including bootstrap, camera movement,
forced render-graph reconstruction, bounded publication, and idle feedback.


### Dependency-reference corruption on page installation

The runtime retained dependency lists on primed and uninstalled pages so they
could be read again. `installRuntimeObjectPage` unconditionally decremented the
old list before claiming the new list, even when the page was not installed and
owned no references. In a three-page DAG with pages 1 and 2 both depending on
page 0, priming/installing page 2 stole page 1's reference. Evicting page 2 then
left page 0 evictable while page 1 still drew. The same bug recurred on reinstall.

The new runtime regression fails five assertions before the correction, including
the actual attempt to uninstall a still-required root. Installation now releases
old references only when replacing an installed page. Primed and evicted pages
claim their references without decrementing another resident page's protection.
This is a shared VG runtime correction, not a terrain-only repair policy.

A diagnostic 768 MiB run of the closed-block cut (before this reference fix) held
2,217 pages without eviction or prefix repair and settled page activity at frame
181. It used a 5.84 GiB process footprint. At 512 MiB the same source cut repeatedly
evicted/repaired pages and used 5.92 GiB; reducing churn matters more than merely
minimizing the reserved decoded pool. The current automatic tier is 768 MiB for
machines with more than 8 and at most 16 GiB RAM.

### Incomplete root-page coverage

After fixing residency and radial ordering, the 5 km run reached 168 LOD0 chunks
and settled without eviction or repair, but retained 247 enclosed uncovered
pixels. Zero `interiorHoles` in the terrain-image heuristic did not prove an
intact surface; the exact-output flood-fill detected the remaining enclosed gap.

The shared packer selected the shape's root page by the largest self error.
An octree DAG can have independent roots at different levels, spread across
several pages. The selected page's prefix could omit a later root. The terrain
installer then declared the child drawable and disabled the parent before every
part of the child's initial cut existed. A two-root, two-page regression with
the coarsest root packed first reproduces this directly.

The packer now selects the last page containing an independent root, so its
initial dependency prefix covers the complete root cut. This applies to the
normal VG packer as well as octree geometry. The capacity regression checks
exact enclosed coverage, in addition to near-field LOD and rendered error.
The closed-block run temporarily reached 6.23 GiB during generation, so its
bounded test uses a 7 GiB guard, below this machine's normal 45%-of-RAM budget;
the settled footprint was 5.85 GiB.

### Translation-dependent DAG topology

A stronger geometry regression exposed the remaining builder defect: translating
an otherwise identical brick hierarchy by whole finest-grid cells changed its
DAG edges and number of roots. The builder inferred parents and child octants by
floor-dividing global canonical coordinates at each level. Streamed page origins,
particularly elevation anchors, do not necessarily align to every coarser brick
extent. The inferred octant could therefore differ from the actual octree octant;
a coarse block then replaced fine blocks from the wrong volume. Bounds-union and
error-monotonicity checks could still pass while the rendered cut developed gaps
and incorrect slopes.

The builder now follows each brick's authored `parentIndex`, restricted to the
chunk span, and identifies the child octant from its integer offset relative to
that parent. It checks levels, scale, octant alignment, and uniqueness. No global
coordinate hash is used to reconstruct the DAG. The translated-hierarchy
regression fails before the change and compares exact source-block DAG edges and
root counts afterwards. The solid-column experiment was not retained: correcting
the authored topology preserves the existing terrain source representation.

The generated-world sample also restores the terrain settings' normal four-cell
closure below exposed sides. Its previous zero-depth override was clamped to
one cell by the page builder. With independent spatial sampling, two closed
boundary columns can otherwise meet only at an edge, leaving a diagonal opening.
The topology-correct 300-frame diagnostic with four-cell closure reports no giant
clusters and zero enclosed uncovered pixels, without page eviction or repair.

### Further fixes from the reported no-op loop and wall screenshot

The capacity limiter correctly retained the covered cut, but the sample still
interpreted deferred work as immediately runnable. A transaction with zero
admitted groups and zero generated chunks is now suspended for that published
generation. Camera, cut settings, or a new publication re-enable it; ordinary VG
page requests continue independently. The capacity contract limits empty jobs
relative to successful publications.

The shared object also retained hidden, fully replaced ancestors in the same
512-slice pool used by current leaf chunks. Ancestor directory entries now keep
their spatial links while releasing geometry once a complete descendant cut is
drawable. Leaving terrain outside the new cut no longer waits for a replacement
that will never exist. Recursive descendant coverage remains valid after the
intermediate asset is retired. The complete initial root-page prefix is protected
from LRU eviction, including independent roots that are not each other's page
dependencies.

The sample's procedural terracing strength default was 1.0, with four-metre
steps. This deliberately produces ledges beyond ordinary voxel quantization.
The sample now defaults to zero terrace strength; explicit terrace settings
remain supported. Generated-world rendering defaults to a deterministic Lambert
term plus ambient fill using the G-buffer normal and albedo. Tree64 light tracing
and temporal lighting passes are disabled on this route. Source Tree64 payload
construction remains part of the existing publication representation for now.

Culling requests the first missing page it visits and returns before visiting
children, including page-backed interior nodes. Page-less BVH nodes continue to
traverse normally. A GPU regression injects a missing interior page, checks that
it is the only requested page and no children draw, then restores residency and
checks that the original visible cut returns.

### Validation refinements

The 600-frame, 24-metre out-and-back ground-camera run reached visible 10 cm
terrain with zero enclosed gaps, but exposed a remaining transition-capacity
failure: 511 source chunks competed with their temporary fallbacks for 512 VG
hierarchy slices. The default minimum is now 768 slices, allowing the 512-owner
source cut plus its four-child ancestor tree and a transition margin. Obsolete
fallbacks are also retired on meshing completion each frame, including after the
last source publication. Actual source records, rather than retained fallback
bounds, identify the current owners.

The source LOD profile test now gives asynchronous generation 1200 frames. Its
300-frame uninstrumented run could terminate before the near field arrived.
At a completely full 512-owner source budget, it allows only the configured
4 px source bound, and still requires zero near-field coarse chunks, zero radial
inversions, zero install/slice failures, and zero oversized rendered clusters.
It does not claim that finite source capacity can satisfy 1.5 px everywhere.
The settled 1200-frame diagnostic had 196 LOD0 chunks, nearCoarse=0, inversions=0,
maxTriPx=4.59, and no oversized clusters; three nominal chunk-box estimates were
between one and 1.4 levels above the VG target while source capacity was full.

The tiny-triangle fraction is retained as a diagnostic rather than a rejection
criterion. Closed block sides and grazing views legitimately produce short
triangles; VG selection is based on geometric deviation, not triangle length.
The coverage, near-field and cut-error assertions remain. Generated-world
Lambert shading preserves the configured raster resolution, so disabling traced
lighting does not silently quadruple pixel demand and source refinement work.

## Final validation (2026-09-09)

Release sample and affected unit-test targets build successfully. `git diff
--check` passes. Existing compiler initializer-order warnings remain.

The final bounded reproduction uses the user's command plus `--frames 360
--lod-threshold 1.5 --test-static-scene --memory-budget-mb 7168` and final-frame
readbacks. It exits successfully with the normal configured 610x360 internal
raster for 1220x720 output:

| Metric | Final result |
|---|---:|
| LOD0 source chunks | 108 |
| LOD0 chunks in camera quadrants | 26 / 24 / 30 / 28 |
| Last VG refinement/install activity | frame 194 |
| Maximum drawn triangle edge | 5.07 px |
| Oversized clusters | 0 |
| Missing-page requests at end | 0 |
| Install failures / missing slices / root repairs / evictions | 0 / 0 / 0 / 0 |
| Final coverage holes / enclosed uncovered pixels | 0 / 0 |
| Settled process footprint | 5.24 GiB |

This diagnostic took 17.36 seconds including startup and readbacks; it is not
an interactive frame-rate benchmark. Logs and the captured image are under
`/tmp/terrain-vg-fix/final-launch.*`.

The final capacity regression reaches 168 LOD0 chunks, prepares all four camera
quadrants, and issues exactly one empty capacity-blocked job instead of looping.
The final 24-metre out-and-back movement regression reaches 220 LOD0 chunks;
visible terrain within 8 metres uses 10 cm source voxels, with zero install
failures, missing slices, bare replacements, root repairs, evictions, coverage
holes or enclosed gaps. Its graph contains no light-tracing passes.

Passing checks during this implementation:

- Eight CPU suites: world-cut advancement, page determinism, octree VG build,
  runtime refinement, octree page rewrites, slot installation, scratch storage,
  and packer determinism. The final root-dependency change was rechecked by the
  octree and packer suites.
- GPU runtime lifecycle, terrain Phase14 runtime installation, and the GPU
  first-missing-page traversal regression. The latter checks processed-node
  counts, so an empty visible cut cannot make it pass without stopping traversal.
- The 1200-frame cut-error contract.
- Final capacity, movement, settling and LOD-profile contracts (all four pass
  on the final executable; `/tmp/terrain-vg-fix/verified-gpu-contracts.log`).

The last coverage correction publishes ancestor masks after retiring replaced
assets, so a coarsening chain cannot leave its higher fallback hidden until the
next frame. Residency fingerprints include page identities rather than just a
page count, since dependency-driven installation can produce noncontiguous cuts.

Limits: the finite source-owner budget can retain distant coarse fallbacks;
this does not guarantee 1.5 px nominal source error everywhere. Close 10 cm
terrain and radial preparation are checked explicitly. Source Tree64 payload
construction still belongs to the existing publication pipeline, although no
Tree64 lighting rays run in the default generated-world renderer.
