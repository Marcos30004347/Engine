# Game-scale world generation and light-field stability plan

**Status:** Parts 0 through 4 are implemented and verified. Part A's terrain
commit mitigation is also implemented; the remaining work is A.4's
active-commit rotation verdict.
**Date:** 2026-08-26.

Two complaints, one plan, because they turn out to be coupled:

1. The rendered terrain is boring — no landmarks on the horizon, weak biome
   transitions.
2. Light artifacts appear when the camera is ~2 m above ground and moving
   forward.

The coupling: the current world is so flat that terrain LOD transitions barely
move the surface, which hides a history-invalidation defect in the light field.
Making the world interesting makes that defect **worse** — measured below.

---

## Part A — Terrain page commit: the crash, and the light blink it causes

**Priority: first.** This is one event with two symptoms. A page commit both
aborts the run under fast movement and is what the light blink is timed to.

### A.0 What is measured

Walking 2 km in 900 frames aborts:

```
Terrain GPU upload commit failed:
  Generated-world runtime visible root count diverges from compact roots:
  runtime=240 compact=244 rootIndices=4224 pages=898
  resident=60 installed=113 streamingLeaf=60 visible=60
```

(The counts in that message were added while diagnosing this; the shipped
message said only "diverges from compact roots".)

Reading it: every page contributes exactly 4 brick roots, so `runtime=240` is
60 visible pages and `compact=244` is 61. **The two derivations of "which pages
are visible" are exactly one page apart.** And `installed=113` against
`resident=60` says the two flags have different lifetimes — 53 pages hold an
installed payload while no longer being resident.

### A.1 Why they can disagree

`publishTerrainRuntimePageTable` (`SparseVoxelRendererSample.cpp:12735`) checks
two independently produced answers against each other:

* `buildWorldVoxelTerrainRuntimeVisibleBrickRoots` walks the runtime page table
  and takes every page with `resident && payloadInstalled && streamingLeaf`.
* `chunks.bricks.rootCount` is a **running total**, not a computed set.
  `writeTranslatedHierarchySlot` does `destination.rootCount += ...` as a side
  effect. The visible publish loop (`PlanetVoxelChunk.cpp:8540`) lets that
  accumulate to pack a compact prefix; the resident publish loop
  (`PlanetVoxelChunk.cpp:8666`) saves `rootCount` before the call and restores
  it after, precisely to suppress the same side effect.

So one number is a set membership test over page flags and the other is an
accumulator that two loops deliberately treat differently. Nothing keeps them
in step across an eviction that lands between the two loops, and a single
page's worth of drift is what the failure reports.

### A.1b Root cause, found 2026-08-26 — **not what A.1 assumed**

A.1 guessed a race between eviction and the two publish loops. The actual
mechanism is narrower and does not involve eviction at all.

`refreshWorldVoxelPageRuntimeFlags` (`PlanetVoxelChunk.cpp:4110`) is the
authority on `streamingLeaf`, not the patch list — it **recomputes** it for
every entry:

```cpp
entry.streamingLeaf = installed && !worldVoxelPageChildrenResident(...)
```

So a parent page stops being a cut leaf the moment **all of its children become
resident** — which can happen inside the very commit that publishes the parent.
Its roots were already packed into the compact prefix by then, so the prefix
counts a page the runtime table (correctly) no longer calls visible. That is the
one-page skew, and it is why `streamingLeaf=60` equals `visible=60` while
`compact=244` implies 61.

**The two derivations are taken at different points in the pipeline.** The
prefix is packed during the publish; the directory is refreshed *after* it. Two
fixes were tried and both failed on that ordering:

* Making `SetStreamingLeaf` unconditional (removing its `worldVoxelPageInstalled`
  guard) is **inert** — the patch path is not what sets the flag; the refresh
  overwrites it. `streamingLeaf` stayed at 60. Reverted.
* Filtering the publish loop by `Resident && streamingLeaf` **over-corrects** —
  at publish time the directory has not been refreshed, so the predicate is
  false for every entry and the prefix comes out empty (`compact=0` against
  `runtime=16`). Reverted.

**The skew accumulates.** Measured one page at first occurrence and three a few
commits later; it does not resolve on its own, because nothing ever removes a
page from the prefix once it has stopped being a leaf.

### A.2 The fix — **implemented 2026-08-26**

**A.2.1 Derive the visible root set once.** `rootCount` should not be a
side-effect accumulator that one caller relies on and another undoes. Compute
the compact visible prefix from the same page-flag predicate the runtime table
uses, in one pass, and have `writeTranslatedHierarchySlot` take the root base as
an explicit argument without touching `rootCount`. The equality check then
becomes an assertion about one derivation rather than a reconciliation of two.

Implemented: every progressive page is first written to its physical root
range. After `annotateWorldVoxelPageDirectory` refreshes the runtime flags,
`compactWorldVoxelTerrainRuntimeVisibleBrickRoots` selects resident installed
streaming leaves once, gathers their physical roots in stable physical-range
order, writes the compact prefix, sets `rootCount`, and patches the visible
runtime and record ranges to that prefix. `writeTranslatedHierarchySlot` no
longer mutates `rootCount`; its legacy callers now account for roots explicitly.

**A.2.2 Give `payloadInstalled` and `resident` one lifetime, or stop conflating
them.** 113 installed against 60 resident is either a leak in the flag or a
deliberate two-phase state that the visibility predicate reads as if it were one
phase. Decide which, and make eviction clear both, or make the predicate
explicit about the intermediate state.

Decision: the different lifetimes are deliberate. `Installing` owns an uploaded
physical payload before it joins the resident cut. The shared
`worldVoxelRuntimePageVisible` predicate now makes the contract explicit: a page
must be resident, payload-installed, and a streaming leaf. Regression coverage
asserts that an installing page carries the payload flag but neither resident
nor streaming-leaf, so it cannot enter the compact set.

**A.2.3 A consistency failure must not abort the process — implemented.**
`publishTerrainRuntimePageTable` now counts consecutive skews, logs
`Terrain runtime page table skipped (n/8)` with the full counts, keeps the
previous table, and escalates to the old hard failure only after 8 in a row.

Before A.2.1's structural fix, the safety net measured as follows: at a
realistic walking pace (300 m over 600 frames, with rotation and 36 commits) the
run **completed**, with 2-3 non-consecutive skips that the next commit resolved.
At 2 km over 900 frames — a 133 m/s camera, far beyond gameplay — it survived 14
commits instead of 3 before hitting the limit.

The escalation is deliberate rather than a missing `if`: while the prefix keeps
a page the table no longer calls visible, that page is drawn on top of its own
children, and silently rendering double coverage forever is worse than stopping.
Note also that a run without the net sometimes shows zero skews at the same
settings — the streaming workers make commit timing nondeterministic, so this
needs several runs to judge, per `metric-noise-floor-before-verdict`.

**A.2.3b (original wording)** This is a race
between streaming and publication; the correct response is to skip this frame's
publish and retry on the next commit, keeping the previous good table. Losing a
whole session to a transient one-page skew is the wrong trade. Keep the check —
it caught a real bug — but demote it to a logged skip with a counter, and fail
hard only if it persists across some number of consecutive commits.

**A.2.4 Keep the self-describing message.** The counts are what turned this from
"streaming is broken" into "exactly one page, and the flags have different
lifetimes" in a single run.

### A.3 Regression coverage — **implemented and passing**

`SparseVoxelWorldCutAdvanceTests` and `SparseVoxelWorldPageDirectoryTests`
already exist and pass; neither advances the cut fast enough to produce the
skew. Add a case that advances the camera far enough per step to force eviction
and installation in the same commit, and assert the two derivations agree.
`--test-camera-travel-m` makes this reproducible from the sample as well.

The directory test now exercises the exact skew boundary deterministically:
parent fallback, partial child install, the fourth-child commit that removes the
parent from the streaming-leaf set, and child uninstall back to the parent. It
compacts from the refreshed runtime table and checks the expected parent/child
root sets. Both `SparseVoxelWorldPageDirectoryTests` and
`SparseVoxelWorldCutAdvanceTests` pass.

The sample stress run also completes at 2 km over 900 frames (640x360, 4 km
render distance). It crosses three generated-world GPU commits, the point where
the original run aborted, with no visible-root skew warning or skipped runtime
table publication.

### A.4 The light must not blink when a page commits — **cause confirmed, fix only partial**

**A.4.0 Reproduced.** Added `--test-camera-yaw-rate DEG`, which composes
continuous yaw onto the ground-forward camera, so rotation and streaming happen
together for the first time. 600 frames, 300 m of travel, 0.5 deg/frame, 36
commits:

| | pop mean | pop max | n frames |
| --- | ---: | ---: | ---: |
| commit frames (+0..3) | **0.2833 %** | **18.59 %** | 108 |
| every other frame | 0.0076 % | 0.251 % | 372 |

37x in the mean, 74x in the worst frame, and **every** spike above 0.5 % lands
on a commit. The report is exactly right: the blink is the commit.

**A.4.1 Attributed.** Counters were added for every way a tap can be rejected
(slots 14-16, logged with `Sparse voxel light field truncation:`):

| | pop | lostAll | normal | instance | **depth** | emptyRecord |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| commit (+0..3) | 0.278 % | 0.728 % | 0.168 % | 0.000 % | **0.618 %** | 0.002 % |
| other | 0.007 % | 0.018 % | 0.033 % | 0.000 % | 0.015 % | 0.002 % |

Depth carries it: 40x more rejections on a commit frame, four times the normal
term. Region-visibility truncation, anti-lag truncation, the surprise decay and
`inexactId` all measure **0.000 %** — the candidates listed in A.4.3 below are
all ruled out. It is the reprojection depth veto and nothing else.

**A.4.2 Not a tuning problem.** Raising `terrainHistoryDepthThreshold` from 0.20
to 0.50 recovered only 17 % of the loss and did not move the worst frame at all.
The surface genuinely moves on a commit: the rebuild quantizes it on a different
grid, and at the coarse spatial LODs actually resident that is tens of metres,
which near-grazing is an enormous view-depth change.

**A.4.3 Implemented: settle instead of reset.** `LIGHT_FIELD_COMMIT_TAP_WEIGHT`
(0.35) — on a commit frame, a terrain-to-terrain tap that fails the depth veto is
accepted at low weight instead of being rejected. The low weight caps confidence
through the Part 2.4 footprint mechanism, so the pixel re-converges over several
frames rather than stepping. Newly exposed faces are not covered: their normal
changed and the orientation test still rejects them. Plumbed as
`Settings::terrainCommitted`, set for a few frames after each GPU commit.

**Result: the worst-frame spike falls 18.59 % -> 16.30 %, reproducibly, and the
mean does not move.** Two identical runs give 0.2733 % and 0.3190 %, so the
noise floor on this metric under streaming is about 17 % and the mean difference
is inside it.

**A.4.4 Why it is only partial, and what to do instead.** Only 4.4 % of the
rejected taps are recovered by the relaxed gate. The pixels that still pop are
ones where the surface moved so far that *no* tap in the 2x2 footprint is
geometrically plausible — there is no history to keep, valid or otherwise.

That is not fixable in the light field. The fix is upstream, and it is the item
already open as Part 2.5: **refine pages in place**. `refined=0 coarsened=0` on
every commit means the surface is re-quantized wholesale each time; if a refined
page kept its instance and quantized on a grid consistent with its parent, the
depth delta would be small and the existing gate would pass without any commit
special case. A.2's rework of the visible-root derivation is a prerequisite for
that, which is why Part A is sequenced first.

### A.4.5 Original candidate list (all now ruled out by measurement)

Design intent, from the report: a page commit changes which nodes carry the
surface, but **the surface and its incoming light are essentially unchanged**.
Newly exposed faces are the only genuinely new information. So a commit must
never reset lighting — the old history should be kept and allowed to settle over
a few frames.

This is the regime none of the measurements so far covered. Part 2 fixed
*reprojection* validity, and Part 2.6 showed that under pure rotation the three
truncation paths are all at 0.000 % — but those rotation runs were taken on
terrain that had already finished streaming. **Rotation plus an active commit is
untested**, and it is exactly what is reported.

**A.4.1 Reproduce the actual combination first.** No existing harness rotates
*while* pages commit: `--test-camera-walk` oscillates inside already-resident
terrain, and `--test-camera-ground-forward` holds orientation. Add a camera that
translates into unstreamed terrain *and* yaws continuously, then read the
existing counters — reprojection loss, region-visibility truncation, anti-lag
truncation, pop — against per-frame commit events. Do not design the fix before
this run says which counter moves; two hypotheses have already been falsified
this way (the sub-pixel ray offset, and region/anti-lag truncation under
rotation).

**A.4.2 Instrument the commit boundary. — DONE** Part 3.4's joined telemetry
logs `built/refined/coarsened/evicted`, `attempted/lost`, and both localized pop
metrics on one frame-accurate line. CPU churn is snapshotted into the pending
frame that owns the GPU readback, so delayed completion cannot pair a pop with
the wrong terrain generation.

**A.4.3 Candidate causes, in the order the evidence should be checked.**
  * The commit changes `instanceIndex` / `regionMappingId` for surviving
    geometry. Identity is already demoted to corroboration for the *tap weight*
    (Part 2), but `light_field_identity` still folds the instance into the
    identity word, and the region-stability tiles are keyed on it.
  * `combineSceneRayGeneration` folds a content generation into
    `settings.reachability.x`. If a commit bumps it, every cached reachability
    observation is discarded at once — a global reset on a local change.
  * The Tree64 shadow structure is rebuilt with the page, so shadow rays can
    genuinely change answer for a frame or two. That is real, and the response
    should be to let it settle, not to reset.

**A.4.4 The rule for whatever is found.** A commit is evidence that geometry was
*re-expressed*, not that lighting changed. The response should be a confidence
reduction bounded well above zero — the same cap introduced in Part 2.4 — so the
estimate re-converges over several frames with no visible step. Reserve outright
invalidation for pixels whose depth or normal actually moved, which is the
newly-exposed-face case and is already handled.

---

## Part 0 — Measured findings — **DONE**

Reproduced 2026-08-26 from the checked-in `world-data` bundle and a freshly
generated reference bundle. The field measurements are now repeatable without
the original analysis session:

```sh
scripts/game-scale-part0-audit.py map world-data /tmp/world-game
scripts/game-scale-part0-audit.py log /tmp/world-data-100m.log
```

The first command decodes the finest paged fields directly, strips their
gutters, and evaluates every complete land-centred window. The second aggregates
the `Sparse voxel light field history` and terrain-streaming generation lines
from one or more renderer logs. The 128 km fixture is generated with the command
in 1.1. That command reproduces all structural counts in 0.5, but currently
returns status 2 after a valid export because the requested `coast` shape is
classified as `island`; scripts should treat the verified `exported` line as
success until that classifier mismatch is resolved.

### 0.1 The world is 1800 km across and the view is 16 km

`world-data/manifest.json` reports `extentMetres = 1800000` at a 1024² export:
**1758 m per elevation texel**. A 16 km render distance spans about **9 texels**
of bilinear heightmap.

Relief actually present in a 16 km window, over every land-centred window in the
map (measured by decoding the `base_elevation` pages directly):

| window | current 1800 km world | 128 km world, same generator |
| --- | ---: | ---: |
| 16 km, p10 | 24 m | 587 m |
| 16 km, **median** | **75 m** | **988 m** |
| 16 km, p90 | 266 m | 1502 m |
| 5 km, median | 16 m | 331 m |
| metres/texel | 1758 | 62.5 |

75 m across 16 km is a 0.5 % grade. There is no mountain on the horizon because
there is no mountain within 16 km *anywhere in the world*. The map is
geographically correct; a correct 1800 km continent looks like Kansas when you
stand on it.

### 0.2 Sub-texel detail cannot fill the gap

`proceduralWorldMapTerrainDetail` (`PlanetVoxelChunk.cpp:394`) is the only
structure between map texels. Its longest wavelength is 2400 m at ±24 m ×
`terrainWeight`, doubled at the default `--terrain-detail-strength 2.0`. That is
a ±48 m bump every 2.4 km — a 2 % grade. Not a landmark.

`terrainWeight = 0.35 + 0.35·roughness + 0.30·clamp(|relief|/600)`. Measured
over the current export: median authored relief is **30 m**, p90 is **256 m**,
and only 0.5 % of texels exceed the 600 m saturation point. So the weight sits
at ≈0.44 essentially everywhere. The map distinguishes a 30 m cell from a 550 m
cell and the voxelizer turns that into a **1.15× difference in noise
amplitude**. The signal is real and well-populated; the transfer function
flattens it.

### 0.3 The design the request asks for already exists

Poisson sites → Voronoi cells → landmark type per cell is implemented.
`MapNode::type` is already `Mountain / Hill / Plateau / Valley / Plain /
Desert` (`MapGraph.hpp:66`); ranges are laid on orogenic plate seams
(`--ranges`, `--range-strength`, `--range-width`); and `rescaleForExtent()`
(`WorldGeneratorSample.cpp:237`) already rescales **every** length in the
pipeline — range width, coastal plain, landform provinces, climate reach,
region target area — linearly with extent.

No new system is needed for the layout. The brief is what is wrong.

### 0.4 Two export fields reach the voxelizer and are then discarded

- **`biome_mix_weights` / `biome_mix_ids`.** The export ships a 4-way sparse
  biome mixture built specifically for blending. `PlanetVoxelChunk.cpp:2983`
  calls `biome.dominantBiome()` and throws the rest away. Every biome boundary
  is therefore a hard step quantized to the map-texel grid — 1758 m today. This
  is the "bad transition boundaries" complaint, and the data to fix it is
  already sampled at the call site.
- **`landform_id`.** Exported, has **zero consumers** anywhere in
  `src/voxel/`. The map computes Mountain/Hill/Plateau/Valley per cell and the
  terrain builder never reads it.

### 0.5 Scale sweep

Generated with `WorldGeneratorSample`, `--levels 2`, seed 7:

| extent | spacing | relief budget | 16 km relief (p50) | rivers | lakes | biomes |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1800 km (current) | 12 km | 3000 m | 75 m | many | many | — |
| 24 km | 0.8 km | 900 m | degenerate¹ | 0 | 2 | — |
| 48 km | 1.2 km | 1200 m | 857 m | 0 | 0 | 8 |
| 96 km | 2.0 km | 2000 m | 967 m | 2 mouths | 0 | 13 |
| **128 km** | **0.7 km** | **2200 m** | **988 m** | **15 mouths** | **34 cells** | **14** |
| 160 km | 3.0 km | 2500 m | 946 m | 1 mouth | 0 | — |

¹ A 24 km world is *smaller than the render distance*: the whole map is visible
from spawn and the relief statistic is constant.

Two parameters matter more than they look:

- **`--spacing` drives rivers and biome variety, not `--extent`.** At 128 km:
  spacing 2.0 km → 0 rivers, 13 biomes; spacing 0.7 km → 15 river mouths,
  34 lake cells, 14 biomes, 26 waterfalls. Drainage depth is set by how much of
  the network the cell size resolves. (Consistent with
  `drainage-dials-that-do-nothing`.)
- **`--levels 2`, not the default 3.** The coarsest tier is
  `spacing × 4^(levels-1)`; at 3 levels a small world produces fewer than three
  coarse sites and `buildHierarchy` fails outright.

**On the 4-hour traverse figure.** 4 h × 6 km/h = 24 km, which the sweep shows
is degenerate. Straight-line walking speed is the wrong model: terrain,
detours and elevation mean a 128 km map is roughly a 4-hour crossing in
practice. A `--traverse-hours` dial should apply a detour factor of ≈5.

### 0.6 The light artifact: reproduced and attributed

**The existing harness could not reproduce it.** `--test-camera-ground-forward`
travelled `min(pageEdge × 0.35, 12.0)` metres over the entire run — **4.48 m**
at the default 12.8 m page. It never leaves its starting page, so it cannot
observe a page handover or a spatial-LOD change. Added
`--test-camera-travel-m M` to set the distance explicitly. (Same failure mode as
`svraster-dolly-camera-harness`: the static cameras cannot see motion
artifacts.)

**Aggregate metrics are blind to it.** Frame-to-frame mean luminance, TSR
`instability`, `edgeEnergy` and light-field `zeroTap` all move by less than
their noise floor between a 4.5 m and a 100 m walk. A localized pop of a few
thousand pixels does not survive a whole-screen average.

**Added history-cause counters** to `sparse_voxel_light_field.wgsl` (slots 3–7
of the existing `reconstructionStatistics` buffer, logged as
`Sparse voxel light field history:` under `--test-frame-signatures`), mirroring
the reconstruction counters from `light-field-sparse-reconstruction`. Per-frame,
world-data, 4 km render distance, 2 m eye height, 240 frames:

| travel | attempted/frame | lost all history | faceRejects | hashRejects | lodOnlyTaps |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 4.5 m | 370 102 | 0.220 % mean, **13.5 % max** | 0.70 % | 0.018 % | **62.5 %** |
| 20 m | 370 151 | 0.186 % mean, **13.7 % max** | 0.62 % | 0.011 % | **65.3 %** |
| 100 m | 371 237 | 0.182 % mean, **9.5 % max** | 0.84 % | 0.032 % | **57.7 %** |

Findings, in order of importance:

1. **`lodOnlyTaps` is 56–65 % of every history tap.** The exact temporal id
   does not match, so the tap is accepted at `LIGHT_FIELD_LOD_TAP_WEIGHT = 0.5`.
   On terrain, identity mismatch is not an occasional event — it is the normal
   case. The light field runs permanently at half history confidence.
2. **`lost all history` spikes to 9.5–17 % of the screen in a single frame.**
   That is the pop. Mean is 0.2 %.
3. **Spikes are not camera-motion driven.** They land on almost identical
   frames (17–18, 43–46, 66–84) regardless of whether the camera walks 4.5 m or
   100 m. They track *terrain page commits*.
4. **Face class is a minor cause today** (0.6–1.0 % of taps) and the light hash
   is negligible (0.02 %). The normal-flip hypothesis is largely wrong at this
   scale — see 0.8 for why it will not stay that way.

### 0.7 The trigger is terrain page churn, and pages never refine in place

`--test-terrain-streaming`, steady state, camera walking slowly:

```
generation=11 resident=237 reused=233 built=1  refined=0 coarsened=0 evicted=2
generation=12 resident=231 reused=229 built=2  refined=0 coarsened=0 evicted=8
generation=13 resident=231 reused=205 built=32 refined=0 coarsened=0 evicted=0 restored=14
generation=14 resident=228 reused=227 built=1  refined=0 coarsened=0 evicted=4
```

Residency oscillates 220↔237 and never settles; commits arrive about every
14 frames, matching the spike spacing. **`refined=0` and `coarsened=0` on every
generation**: the streamer never refines a page in place — every LOD change is
an evict-and-rebuild that produces a fresh instance and a fresh temporal id
under the same screen pixels.

The history test has no tolerance sized for that:

- `light_field_tap_identity_weight` reaches its depth-margin path **only when
  the instance differs** (`!sameInstance`). A same-instance rebuild at a new
  LOD falls straight to the 0.5 weight — that is the 56–65 %.
- The margin itself, `terrainHistoryDepthThreshold`, defaults to **0.01** — a
  1 % relative view depth. At 2 m eye height the visible ground runs from 2 m to
  several hundred metres; a rebuild that moves the surface by half a coarse
  voxel (25–100 m at the spatial LODs actually resident) against a 200 m view
  depth is a 12–50 % relative change. 0.01 rejects it outright.
- `sameFace` is a **hard binary reject with no tolerance at all** — a 6-way
  quantization of a continuous normal, evaluated with `>` on magnitudes.

Also worth noting from the same logs: at 16 km render distance the resident set
is `resident=4/512`, `levels=1`, spatial LOD 11 — the whole 52 km footprint is
four pages at an effective 204.8 m voxel. That is a separate streaming question
from this plan, but it is why the near field looks coarse.

### 0.8 The two problems are coupled

Same repro, same 240 frames, only the map changed:

| map | relief in resident footprint | faceRejects | lost all history (mean) |
| --- | ---: | ---: | ---: |
| 1800 km world-data | 267 m | 0.84 % | 0.182 % |
| 128 km game-scale | **1565 m** | **1.52 %** | **0.294 %** |

Fixing the map makes history rejection **81 % worse on face class and 61 %
worse overall**, because steep terrain puts normals near the dominant-axis tie
where the face class flickers. Part 1 must not ship without Part 2.

---

## Part 1 — Regenerate at game scale — **DONE**

Implemented and verified 2026-08-26. The sample's defaults are now the 128 km
reference brief below. A default headless run reproduces 19 175 cells,
6 585 km² land, 15 river mouths, 34 lake cells, and 26 waterfalls in 0.63 s.
Deterministic world-map generation semantics advanced to revision 9 so cached
pages from the old biome/detail path cannot be restored under the new rules.

**1.1 Adopt a game-scale brief — DONE.** Reference configuration:

```
WorldGeneratorSample --headless --export world-game --export-resolution 2048 \
  --extent 128 --spacing 0.7 --levels 2 --relief 2200 \
  --continent-span 105 --continents 1 --tags coast,highland \
  --land 0.75 --ranges 3 --seed 7
```

0.6 s to generate, 11 s to export. 19 175 cells, 6 585 km² land, 14 biomes,
15 river mouths, 34 lake cells, 26 waterfalls.

**1.2 Add a `--traverse-hours` dial — DONE.**
`extent = hours × speed × detourFactor`
with `speed` default 6 km/h and `detourFactor` default 5; derive
`spacing = extent / 180` so cell count stays in the range that produces rivers.
Keep `--extent`/`--spacing` as overrides. Guard `levels` so
`spacing × 4^(levels-1)` still yields ≥ 3 coarse sites, and emit a clear error
rather than the current bare `partition failed`.

Implemented `--traverse-speed` and `--traverse-detour` alongside the hours dial.
A 4-hour default traversal derives 120 km and 0.667 km spacing; explicit extent
and spacing win regardless of argument order. Invalid hierarchy depth now names
the extent, spacing, level count, and resulting coarse sites per axis.

**1.3 Consume `biome_mix_weights` at voxelization — DONE.** Replace
`biome.dominantBiome()` at `PlanetVoxelChunk.cpp:2983` with a weighted blend
over the returned mixture. The material path is a compact palette, so blend the
*surface material selection* stochastically per column against the mixture
weights using a position-hashed threshold — this keeps the palette compact
(no new material per voxel) while dissolving the texel-grid step into a
dithered transition band. Verify against `VoxelMaterialTests`.

`MapExportBiomeSample::selectBiome` owns normalized CDF selection, and the voxel
path supplies a deterministic hash of the canonical finest world column. The
material remains one existing palette entry per column. In a 25/75 fixture the
first contributor stays between 22 % and 28 % over 16 384 columns, and every
column regenerates identically.

**1.4 Consume `landform_id` — DONE.** Drive detail *character*, not just amplitude:
ridged noise on `Mountain`, terraced steps on `Plateau`, smoothed on `Plain`,
channel-biased on `Valley`. Read it in `sampleWorldMapSurface` alongside
`relief_roughness`.

Older fixtures without the optional field retain neutral hill detail; schema-v4
exports take the authored nearest-filtered landform path.

**1.5 Widen the `relief_roughness` transfer function — DONE.** Drive detail amplitude
proportionally to authored relief instead of through the compressed
`0.35 + 0.30·clamp(relief/600)` weight. Remove the 600 m saturation; scale the
noise stack's amplitude by `relief` directly with a floor, so a 30 m cell and a
550 m cell differ by ~18× rather than 1.15×.

The full spectrum now shares `max(|relief|, 20 m) / 600 m`, modulated only by a
bounded roughness factor. The test-measured 30 m → 550 m RMS response is 18.33×.

**1.6 Add a mid-scale detail band — DONE.** Even at 62 m/texel there is a gap between
the map texel and the 2400 m longest procedural wavelength. Add octaves in the
100 m – 1500 m band, gated by `landform_id` from 1.4.

The stack now adds 1500 m and 1050 m bands ahead of the existing 720 m, 420 m,
220 m, 48 m, and 14 m bands. `VoxelMaterialTests`, `WorldMapTests`,
`SparseVoxelTerrainPageDeterminismTests`, and the GPU
`SparseVoxelWorldTerrainRasterRegression` all pass.

---

## Part 2 — Make light-field history survive LOD changes — **DONE**

Implemented and measured 2026-08-26. Results, `--test-camera-ground-forward
--test-camera-height 2 --test-camera-travel-m 100 --frames 240`, two identical
runs per configuration (noise floor better than 1%):

| metric | world-data | | 128 km game map | |
| --- | ---: | ---: | ---: | ---: |
| | before | after | before | after |
| total history loss, mean | 0.182 % | **0.046 %** | 0.293 % | **0.106 %** |
| total history loss, max | 9.53 % | **2.40 %** | 13.83 % | 11.97 % |
| normal/face rejections | 0.53 % | **0.05 %** | 0.82 % | **0.23 %** |
| **pop, steady state mean** | 0.0403 % | **0.0003 %** | 0.0933 % | **0.0000 %** |
| **pop, steady state max** | 0.241 % | **0.006 %** | 0.394 % | **0.000 %** |

**In steady state the artifact is gone**: on the game map no settled pixel pops
at all after frame 120, and on world-data the rate falls by a factor of 130. The
spikes that remain are confined to terrain streaming warm-up (frames 17–62),
where pages arrive for the first time and there is no prior geometry — a true
disocclusion, not a history failure.

Neutral where it should be: mean luminance is unchanged to four decimals on
world-data, `edgeEnergy` moves 0.09826 → 0.09793, and on a voxelized mesh
scene (`--test-obj-preview`, dolly camera) the before/after numbers are
identical to three decimals — exact identity dominates there, so the change is
inert. Frame time is within run-to-run noise (7.81 s vs 7.79 s over 300 frames).

What shipped:

**2.1 Continuous normal instead of a quantized face class.** Bindings 33/34 carry
the previous frame's geometry normal as `RGBA16Float`, xyz plus a validity
channel. `sameFace`'s hard reject became a `dot` ramp against a dedicated floor
(`historyNormalThreshold`, `thresholds.w`).

Two calibration traps, both found by measurement:
  * Reusing the spatial gate's `thresholds.x` (0.85, a 32° cone) is *stricter*
    than the six-way face class it replaces — on gentle terrain the dominant
    axis almost never flips. It raised rejections instead of removing them. The
    floor is its own setting, defaulting to **0.0**: 90°, which still rejects
    the opposite-face shell case the face class was introduced for.
  * The validity channel is load-bearing. An octahedral pair cannot distinguish
    "never written" from a legitimate normal — zero decodes to +Z — so every
    consumer that does not wire the ring rejected all history. That is exactly
    what `SparseVoxelSceneRayGPUValidationTests` caught.

**2.2/2.3 Depth margin on the inexact-identity path, corrected for grazing.**
`terrainHistoryDepthThreshold` 0.01 → **0.20**, divided by
`|dot(normal, viewDirection)|` (clamped at 0.15). A displacement d along the
normal moves view depth by d/cosθ, and at 2 m eye height most visible ground is
near-grazing, which is why a fixed relative threshold was tens of times too
tight where LOD transitions happen.

Applying the margin to *every* path, including exact identity, was a first
attempt and a regression: total history loss went 0.182 % → 0.801 % because the
tolerance then also has to absorb ordinary sub-texel reprojection parallax.
Exact identity — same instance and same cell — means the same voxel and needs no
position tolerance. Identity now only ever *raises* confidence; it can no longer
veto a tap the geometry accepts.

**2.4 Settle instead of reset.** `light_field_reproject_history` returns a
confidence from the agreeing fraction of the bilinear footprint, which **caps**
the sample count rather than scaling it. Scaling was the first attempt and
compounds: a footprint straddling any discontinuity agrees only partially every
frame, so a stable pixel decayed to a permanently low count and converged
terrain went noisy — the same shape as `svraster-history-ring-inplace`. A cap
does not accumulate and climbs straight back.

### 2.6 Follow-up: the residual under camera *rotation* is not history at all

Measured 2026-08-26 after the above, because the artifact was reported as still
present. `--test-camera-walk` (translation **plus yaw**) is a regime none of the
earlier runs covered — every harness used so far holds orientation fixed.

| camera | pop, before | pop, after | improvement |
| --- | ---: | ---: | ---: |
| ground-forward, 4 km | 0.1187 % | 0.0396 % | **66 %** |
| walk + yaw, 4 km | 0.1391 % | 0.0933 % | 33 % |
| walk + yaw, 16 km | 0.1949 % | 0.1339 % | 31 % |

Under rotation `faceRejects` is **identical** before and after (0.15 % / 0.21 %)
and so is `lodTaps` (74.4 % / 74.2 %) — the Part 2 fix is not engaging there. Two
counters added for the other ways history can be discarded (slots 10 and 11,
logged as `Sparse voxel light field truncation:`):

| path | ground-forward | walk + yaw |
| --- | ---: | ---: |
| reprojection loss | 0.095 % | 0.133 % |
| region-visibility truncation | **0.000 %** | **0.000 %** |
| anti-lag truncation | **0.000 %** | **0.000 %** |

**All three are near zero under rotation.** History is being reprojected and
kept; the pixels that pop have valid history. So the residual instability under
rotation is *not* invalidation — it is the estimate itself moving, in regions
whose sample count is low because rotation has just revealed them.

Two dial sweeps confirm it and, notably, point in **opposite directions** in the
two regimes:

| dial | ground-forward pop | walk + yaw pop |
| --- | ---: | ---: |
| light-field-quality low | 0.0355 % | 0.1469 % |
| light-field-quality high (full raster) | 0.1123 % | 0.0922 % |
| rays-per-pixel 1 → 4 | — | 0.1480 % → 0.1187 % |

Forward motion is dominated by per-pixel variance, so spatial sharing (a coarser
light field, one ray serving nine pixels) *helps*. Rotation is dominated by
newly-revealed regions needing to converge, so more samples help. There is no
single setting that is best for both — the same shape as
`fsr-shading-change-scale-tension`.

**Falsified: the sub-pixel ray offset is not the cause.** `scene_ray_source_pixel`
does move the measurement around a 3x3 raster block every frame
(`offset = vec2(phase, phase >> 1) % scale`), so the hypothesis was well formed.
But removing that motion entirely — `--light-field-quality high`, where scale is
1 and every pixel traces its own ray at its own position — makes forward-motion
pop **three times worse** (0.0355 % → 0.1123 %). The moving offset is a spatial
average, not a source of error.

Also found: **`--ray-resolution-divisor` is inert.** All three values produce a
320x180 light field. The ray grid comes from `lightFieldFraction`, driven by
`--light-field-quality` / `--lighting-scale`. Same shape as the dead
`--lighting-scale` in `svraster-frame-is-submit-bound`.

And a real crash, not a light-field issue: walking 2 km in 900 frames aborts with
`Terrain GPU upload commit failed: Generated-world runtime visible root count
diverges from compact roots`. Fast movement outruns the streaming budget.

**2.5 Refine pages in place — DONE.** Spatial children are uploaded while hidden
and an ownership group advances only when every replacement page is ready, so
the published parent remains the fallback until one atomic parent-to-children
cut rewrite. Packed terrain already uses implicit raster instance 0; its exact
temporal ID now hashes the hit's canonical finest-grid X/Z world column instead
of the physical brick cell. Parent and child pages therefore describe the same
ground receiver with the same identity even when page address, allocation,
vertical quantization, or microcell representation changes. Ordinary object
voxels retain their existing full 3D brick/microcell identity.

Spatial parent-to-child and children-to-parent publications now contribute to
`refinedChunkCount` and `coarsenedChunkCount`; previously those counters covered
only fixed-footprint internal-LOD replacements, which is why every spatial
commit misleadingly printed `refined=0`. The feedback split/merge fixture pins
both directions. Verified with all brick-draw WGSL variants,
`SparseVoxelTerrainPageDeterminismTests`,
`SparseVoxelMicroRasterR2DirectEquivalence`,
`SparseVoxelWorldTerrainRasterRegression`,
`SparseVoxelWorldTerrainLODStability`, and
`SparseVoxelWorldTerrainStreamingRoundTrip`.

### Part 2 as originally planned

Guiding rule, already the convention elsewhere in this renderer
(`probe_matches_surface`, `can_reuse`): **history validation must never be an
identity test on voxel data.** It must be a tolerance test on surface
position and orientation.

**2.1 Replace the binary face class with a continuous normal term.** Bind the
previous frame's geometry normal (or pack it into the spare bits already
carried in `historyIdTexture`) and replace
`if (!sameFace) { return 0.0; }` with a `dot(n, nPrev)` ramp that falls to zero
at a configurable angle. This removes the only remaining hard reject on a
continuous quantity, and 0.8 shows it is the term that degrades most as the
world gets steeper.

**2.2 Apply the depth margin on every path, not only on instance mismatch.**
Move the relative-view-depth test out of the `!sameInstance` branch so a
same-instance LOD rebuild is validated by depth as well. This is what turns
the 56–65 % `lodOnlyTaps` into full-weight taps when the surface genuinely has
not moved.

**2.3 Size the margin to the voxel, not to a constant.** `0.01` relative is
arbitrary. The correct tolerance is the LOD change's own quantization: the
resident spatial LOD's voxel size projected to view depth. Derive it per pixel
from `finestVoxelSizeMetres << spatialLOD` rather than from a scalar setting,
and keep `terrainHistoryDepthThreshold` as a multiplier on that.

**2.4 Decay instead of resetting.** Where a tap fails today, it contributes
nothing. Replace the `historyCount <= 0 → disoccluded → full reset` path with a
confidence decay: retain history at a reduced sample count and let it settle
over several frames. The accumulator already has `maximumRetainedSamples`
(`antiLag.z`) for exactly this shape of problem during a lighting change —
extend it to geometry changes. A genuine disocclusion still resets, because its
depth delta is large and its normal is uncorrelated; a LOD rebuild does not.

**2.5 Refine pages in place. — DONE** Hidden child uploads and atomic ownership
group advancement preserve the old parent until its complete replacement is
ready. Packed terrain's implicit instance remains 0 and its temporal ID is now
derived from the canonical finest-grid world column rather than a brick-local
cell index. Spatial cut advances are also classified as refinement/coarsening in
the commit telemetry.

---

## Part 3 — Instrumentation — DONE

**3.1 `--test-camera-travel-m M`** — done. Sets ground-forward travel
explicitly instead of the page-derived 4.48 m cap.

**3.2 History-cause counters** — done. Slots 3–7 of
`reconstructionStatistics`, logged as `Sparse voxel light field history:
attempted / lost / faceRejects / hashRejects / lodTaps`. The diagnostic buffer
now binds and clears 24 words; the 2-word fallback keeps the shipping path
registering (verified: default run with no `--test-frame-signatures` is clean).

**3.3 Localized pop metric** — done, and it took two attempts.

First version gated on the *validated* history count, which is zero at exactly
the pixels that pop; it read 0.0000 % on both the broken and the fixed build.
The shipped version reads the previous frame's radiance through an
**unvalidated** bilinear fetch and counts pixels that moved more than half a
stop while their view depth agreed to 10 % — brightness changed, geometry did
not. Logged as `Sparse voxel light field pop: settled=N popped=M`.

**3.4 Variance pop metric joined to page churn — done.** Slots 20–21 count
settled pixels whose depth agrees to 10 %, whose previous/current normal dot
product is at least 0.95, and whose luminance moved by more than three standard
deviations of their own previous-frame moments. Slots 22–23 retain the commit
window subset.

`Sparse voxel light field churn` logs the exact pending frame's terrain
generation and `built/refined/coarsened/evicted` counts beside `attempted/lost`,
the half-stop metric, and both variance counts. The integration regression ran
120 streaming frames, captured 6 commit frames with page churn, and exercised
the variance classifier on 110 frames; it also proves commit-only GPU counters
are zero outside the CPU commit snapshot and equal the full variance counters
inside it.

---

## Part 4 — Verification — DONE

Automated by `SparseVoxelLightFieldStabilityVerification`. The test regenerates
the exact 128 km reference fixture, then runs two identical 240-frame, 100 m
ground-forward dollies on both it and checked-in `world-data`. Only frames
120–239 are aggregated, after initial page publication has settled.

Rates below are percentages. Normal, depth, and inexact-identity rates use the
four bilinear history candidates as their denominator; the old logs divided
tap counts by attempted pixels, so `lodTaps` could exceed 100 % and was not a
percentage despite being presented as one.

| map/run | zero tap | history loss | normal reject | depth reject | inexact identity | half-stop pop | three-sigma pop |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| world-data A | 0.0000 | 0.0355 | 0.7812 | 0.7323 | 88.5443 | 0.0263 | **0.0000** |
| world-data B | 0.0000 | 0.0355 | 0.7812 | 0.7323 | 88.5443 | 0.0263 | **0.0000** |
| 128 km A | 0.0534 | 0.0003 | 0.8161 | 0.4039 | 96.7066 | 0.0525 | **0.0000** |
| 128 km B | 0.0536 | 0.0003 | 0.8161 | 0.4039 | 96.7066 | 0.0524 | **0.0000** |

The measured duplicate-run spread is 0 for every world-data metric and at most
0.0002 percentage points on the game map, far below the 0.2 percentage-point
noise ceiling. The regression enforces that ceiling plus: zero-tap <= 1 %,
complete history loss <= 0.5 %, normal/depth rejection <= 1.2 % of candidate
taps, half-stop pops <= 0.2 %, and three-sigma pops <= 0.1 %.

The originally proposed `lodOnlyTaps -> 0` gate is superseded. It counts any
accepted bilinear neighbour whose exact finest-column identity differs, not a
history rejection; with 10 cm canonical terrain columns, adjacent screen taps
are expected to differ. Geometry validation deliberately retains those taps at
reduced confidence. The direct consequences are gated instead: depth and normal
rejection, complete footprint loss, and both localized pop metrics. The
individual implementation ablations remain recorded in Part 2; the automated
test freezes the combined shipping result on both terrain scales.

---

## Sequencing

Part 2 before Part 1 in *merge* order, even though Part 1 is the visible win:
0.8 shows the game-scale map makes the artifact measurably worse, and shipping
the map first means shipping a regression. Part 3.3 before either, because
neither has a verdict without it.
