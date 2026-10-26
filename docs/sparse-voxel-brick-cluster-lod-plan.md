# Generated voxel pages as virtual geometry

This plan replaces the older "bricks as clusters" stage log. The new target is
simpler:

```text
terrain page requested
  -> payload exists in the transient page store? load it
  -> otherwise generate it on CPU
  -> write/cache the page payload
  -> install it to the GPU with the same rules as virtual geometry
```

CPU generation becomes a page source. Once a page payload exists, the renderer
should not care whether it came from an authored file or from the heightmap
generator.

## Goal

Generated terrain should use the same architectural split as virtual geometry:

| Tier | Contents | Residency |
|---|---|---|
| Directory | page address, parent/children, bounds, error, dependencies, state, patch-list offsets, payload key | always resident |
| Payload | page-local brick nodes, occupancy, materials, palette, roots, Tree64 data/source, measured error | RAM / LZ4 tmp file / regenerated |
| Source | world heightmap bundle and generator settings | always available |

The directory is the equivalent of a virtual-geometry file header. The payload
is the equivalent of streamed page data. The fact that payloads are generated
instead of loaded from a permanent asset is an implementation detail.

## Current state to preserve

Do not rebuild these pieces from scratch:

- `SparseVoxelBrickNodeGPU` already represents a cluster-like brick descriptor:
  payload offsets, child range, level, occupied bounds, coarse occupancy,
  `selfError`, and `parentError`.
- `SparseVoxelBrick.cpp` already measures monotonic brick error during
  OR-reduction.
- `PlanetVoxelChunkRecord::measuredErrorMetres` already stores page-level error.
- `TransientSparseVoxelPageStore` already supplies RAM/LZ4/tmp-directory storage
  with drop-oldest semantics, and generated-world transient serialization is
  already tested.
- `WorldVoxelPageDirectory` already names generated-world pages, parents,
  children, bounds, and cache/residency state, but today it is derived from the
  published cut rather than authoritative.
- `SparseVoxelPageResidency` and `SparseVoxelPhysicalPageRewriteGPU` already
  prove the install model for the older sparse-voxel file-streaming path:
  dependencies first, payload upload, then child-range rewrites.
- Incremental CPU spatial publication already avoids the original coarse-then-
  jump behavior. Keep it as the compatibility path until the generated page
  stream can replace it.

## The main correction

The previous plan treated cross-page brick parents and measured brick error as
the path to deleting the CPU quadtree. That was incomplete.

Generated terrain currently sets `kSparseVoxelBrickForceLeafRaster` on spatial
pages because OR-reduced brick parents are conservative. Conservative occupancy
is correct for Tree64 and shadows, but rasterizing it as terrain can invent
visible slabs and contour noise. Therefore removing `forceLeafRaster` is not a
plumbing cleanup. It requires a raster-safe coarse terrain representation.

This plan separates the two efforts:

1. First, make generated pages stream and install like virtual geometry while
   preserving current image behavior.
2. Later, design a raster-safe coarse representation and use it to retire
   `forceLeafRaster` and the CPU spatial LOD policy.

## Target runtime shape

### Build-time page

Generation may use whatever structure is convenient:

- parent pointers
- canonical grid addresses
- full brick hierarchy
- measured brick/page error
- patch-list derivation data
- validation/debug metadata

This data is build-time only unless the runtime explicitly needs it.

### Streamed page payload

The serialized/generated page payload must be local and relocatable:

```cpp
struct TerrainVirtualPagePayload
{
  WorldVoxelChunkAddress address;
  uint32_t internalLOD;
  uint64_t payloadRevision;

  std::vector<TerrainRuntimeBrickNode> nodes;
  std::vector<SparseVoxelBrickOccupancyGPU> occupancy;
  std::vector<uint32_t> childIndices;
  std::vector<uint32_t> rootIndices;
  std::vector<uint8_t> materialIndices;
  std::vector<uint32_t> microSurfaceData;
  std::vector<SparseVoxelBrickMaterialGPU> palette;
  std::vector<uint16_t> paletteDescriptorIds;

  SparseVoxelTerrainTree64PagePayload tree64;
  float measuredErrorMetres;
};
```

The payload must not contain physical GPU arena offsets. Those are assigned only
during install.

### Runtime brick node

Move toward a runtime node that is traversed top-down and does not need a
parent pointer:

```cpp
struct TerrainRuntimeBrickNode
{
  float minimumVoxelSize[4];
  uint32_t packedOccupiedBounds;
  uint32_t coarseOccupancyLow;
  uint32_t coarseOccupancyHigh;
  uint32_t occupancyIndex;
  uint32_t materialBase;
  uint32_t firstChild;
  uint32_t childCount;
  uint32_t levelAndFlags;
  uint32_t occupiedCount;
  uint32_t paletteBase;
  uint32_t pageSlotOrRegion;
  float selfError;
};
```

`parentIndex` exists today because the flat cull scans every node and asks "was
my parent coarse enough?" A virtual-geometry traversal starts at roots and
decides whether to descend. The parent is implicit in the traversal stack, so
runtime nodes do not need parent pointers after page generation and validation.

## Authoritative directory

Promote `WorldVoxelPageDirectory` from a derived diagnostic/policy structure to
the authoritative terrain page table.

Each entry should eventually hold:

```cpp
struct TerrainPageDirectoryEntry
{
  WorldVoxelChunkAddress address;
  uint32_t parentPage;
  uint32_t firstChildPage;
  uint32_t childCount;

  double minimumXMetres;
  double minimumZMetres;
  double edgeMetres;
  double minimumYMetres;
  double maximumYMetres;

  float geometricErrorMetres;
  float measuredErrorMetres; // zero/NaN means unknown

  TerrainPageState state; // Absent, Generating, Cached, Installing, Resident
  uint32_t generation;
  uint64_t payloadRevision;
  TransientSparseVoxelPageKey payloadKey;

  uint32_t installPatchOffset;
  uint32_t installPatchCount;
  uint32_t uninstallPatchOffset;
  uint32_t uninstallPatchCount;
};
```

Rules:

- Directory entries can exist with no payload.
- Parent/child order is known without loading payloads.
- The directory may cover the active roots, all active ancestors, and the
  refinement frontier. It does not need to materialize the whole planet
  quadtree.
- Payload eviction changes state to `Absent` or `Cached`; it must not delete the
  directory entry.
- The initial split policy may still be geometric. Measured error can refine
  prioritization once known, but correctness cannot depend on a page that has
  not been generated yet.

## Page lifecycle

```text
Absent
  -> request
  -> restore from transient store -> Cached
  -> or generate on CPU -> write transient payload -> Cached
  -> install dependencies first -> Installing
  -> upload payload and apply patches -> Resident
  -> uninstall children first -> Cached or Absent
  -> drop payload when cache budget needs space -> Absent
```

Generation is a loader backend. It should produce the same payload bytes for the
same key every time.

## Install model

A generated terrain install should look like the existing sparse voxel page
install path, not like the current full `PlanetVoxelChunkSet` publication.

Install transaction:

1. Ensure parent page is resident, unless this is a root page.
2. Restore or generate the child payload.
3. Allocate physical GPU ranges for brick nodes, child indices, occupancy,
   material indices, palette, and Tree64/dynamic data.
4. Rebase page-local offsets into the physical ranges.
5. Upload payload bytes.
6. Apply the page install patch list.
7. Publish the directory state and page generation.

Uninstall transaction:

1. Uninstall resident children first.
2. Apply the inverse patch list, making the parent a streaming leaf again.
3. Retire physical ranges through the existing deferred reclamation path.
4. Leave the payload cached if budget allows; otherwise drop it.

The install patch list should express only deltas. Do not rebuild or replay the
whole world hierarchy.

## Patch lists

Patch lists connect payloads to the resident hierarchy. They are generated from
page topology and validated before the payload is cached.

Minimum patch operations:

```cpp
enum class TerrainPagePatchKind : uint8_t
{
  SetChildRange,
  ClearChildRange,
  SetStreamingLeaf,
  ClearStreamingLeaf,
  SetPageInstalled,
  ClearPageInstalled,
};

struct TerrainPagePatch
{
  TerrainPagePatchKind kind;
  uint32_t nodeIndex;
  uint32_t firstChild;
  uint32_t childCount;
  uint32_t flags;
};
```

The concrete encoding can be more compact, but the semantics must remain
explicit:

- installing a child enables its root/child range from the parent page;
- uninstalling a child disables that range before the child payload is freed;
- streaming leaves request children when the traversal wants more detail;
- the enabled set is closed under dependencies.

## Traversal culling

Replace the flat full-array brick cut with a virtual-geometry-style traversal.

Current flat model:

```text
for every node:
  if self is fine enough and parent was not fine enough:
    emit node
```

Target traversal model:

```text
seed resident roots
while queue not empty:
  node = pop()
  if culled:
    continue

  wantsChildren = projectedError(node) > threshold
  canDescend = node.childrenEnabled && node.childrenResident

  if wantsChildren && canDescend:
    push children
  else:
    emit node
    if wantsChildren && node.isStreamingLeaf:
      request child page
```

Consequences:

- Runtime parent pointers are unnecessary.
- Nodes below culled/accepted ancestors are not touched.
- The same traversal boundary emits draw work and streaming feedback.
- The current per-node LOD history buffer can shrink or move to emitted leaves.

The flat cull is cheap today, but other passes walk the same large arrays. The
traversal is valuable because it retires whole-hierarchy reset/history work and
scales with the reached hierarchy, not because the existing cull dispatch alone
is expensive.

## Keeping current terrain images stable

Do not remove `kSparseVoxelBrickForceLeafRaster` during the streaming refactor.

Generated terrain should first stream like virtual geometry while preserving
the current raster contract:

- spatial terrain pages still emit authored leaves where required;
- OR-reduced conservative parents remain available for Tree64/shadows;
- raster-safe coarse terrain is a later feature;
- output hashes / exact-output readbacks should match the current converged
  terrain path.

This lets the architecture become sane before image quality changes.

## Tree64 and TLAS parity

Raster bricks and Tree64 data represent the same page and must publish in the
same transaction.

Install should allocate/upload:

- raster brick nodes and side arrays;
- Tree64 dynamic model/source data;
- Tree64 material cells when present;
- dynamic instance placement;
- TLAS/instance indirection updates.

Acceptance:

- raster and shadows never observe different page generations;
- evicting a terrain page removes both raster and Tree64 ownership in dependency
  order;
- Tree64 reuse is keyed by the payload/geometry revision, not by physical arena
  placement.

## Determinism requirements

The transient store is safe only if regeneration is byte-identical. Keep and
extend the existing determinism tests.

Required checks:

- generate page P twice and compare serialized payload bytes;
- generate with worker counts 1, 2, 4, and 8 and compare bytes;
- generate neighbouring pages before P and confirm P is unchanged;
- serialize -> transient store -> restore -> serialize and compare bytes;
- every generator input that changes output changes the payload key;
- residency budgets, view priority, and physical arena offsets do not change the
  payload key.

No raw struct padding may participate in payload identity. Serialize named
fields, or name and zero every padding byte covered by hashes.

## Valid-cut requirements

Port the virtual-geometry residency invariant to generated voxel pages.

The tests should assert after every install and uninstall:

- a resident child page implies a resident parent page;
- a streaming leaf is enabled;
- the selected cut is a partition of the covered terrain footprint;
- no parent and descendant are selected together;
- no finest-grid cell is left uncovered by the selected cut;
- installs run parent-before-child;
- uninstalls run child-before-parent;
- stale generation/page-slot feedback cannot refine a reused slot.

This is the central correctness test. Do not skip it.

## Implementation phases

### Phase 1: Terrain virtual page payload

Define the relocatable payload type and serializer.

Status: implemented as the compatibility boundary. Generated-world transient
serialization now writes a relocatable terrain payload, and Tree64 has a
matching virtual-page sidecar payload that can be restored and rebound to a
fresh terrain slot/generation during install.

Work:

- separate payload data from physical arena fields in `PlanetVoxelChunkSet`;
- serialize named fields only;
- include brick payload, roots, material data, Tree64 payload/source, measured
  page error, and revision;
- keep existing `PlanetVoxelChunkSet` publication as a compatibility adapter.

Acceptance:

- serialization round trip is byte-stable;
- restored payload produces the same brick and Tree64 revisions as direct
  generation.

### Phase 2: Page-source abstraction

Make generated terrain look like an asynchronous page loader.

Status: implemented as a compatibility page source inside the generated-world
cache update. The current renderer still publishes `PlanetVoxelChunkSet`, but
page acquisition now follows the virtual-geometry order: resident/staged page,
RAM payload cache, transient page store, then CPU generation. Fresh generated
pages are serialized into the transient store immediately, not only when later
evicted from the RAM cache.

Work:

- done: introduce a `WorldVoxelTerrainPageSource` compatibility service;
- done: lookup order: resident/staged -> RAM front-cache -> transient store ->
  CPU generation;
- done: generation completion writes the transient payload immediately;
- done: payload cache acts as a front-cache over the same transient key space;
- partial: directory entries are opportunistically marked `Cached` when the
  current derived directory already names the page. Authoritative directory
  state remains Phase 3.

Acceptance:

- normal streaming can restore generated pages from tmp storage;
- CPU regeneration happens only when no cached payload exists.

### Phase 3: Authoritative directory

Move page identity and page-source state into the directory.

Status: implemented as the compatibility authoritative directory. The
directory is built before page acquisition from the requested cover, not rebuilt
only from the already-published cut at the end of the transaction. Entries are
stable enough to name root/frontier pages before payloads exist, carry their
deterministic transient payload keys, and preserve per-page cache/residency
state across topology rebuilds. The renderer still commits pages through the
current staged `PlanetVoxelChunkSet` publication path; patch-list scheduling is
Phase 4.

Work:

- done: directory entries own parent, children, bounds, geometric error,
  measured error, payload LOD, payload key, payload revision, state, resident
  slot, resident LOD, and residency generation;
- done: directory entries are created before payloads and assigned deterministic
  transient keys from bundle identity, generator settings, page address, and
  internal LOD;
- done: topology rebuilds preserve page-source and residency state by address;
- done: the page source marks directory entries `Generating` before CPU work
  and `Cached` after RAM/transient restore or fresh transient write;
- done: staged pages annotate as `Installing`, committed pages annotate as
  `Resident`, payload-cache pages annotate as `Cached`, and unknown pages stay
  `Absent`;
- done: the generated-world update reports directory entry/covered/frontier and
  absent counts from this authoritative directory;
- compatibility: current cache annotation remains the bridge until generated
  pages install by per-page patches instead of whole-cut publication.

Acceptance:

- cold start can request root/frontier pages with no resident payloads;
- directory state survives a rebuild when the same page address remains
  nameable;
- generated/restored pages become cache-visible in the directory before they
  are resident;
- parent-before-child topology is present in the directory. Producing actual
  install/uninstall transactions from it remains Phase 4.

### Phase 4: Patch-list generation and CPU oracle

Generate install/uninstall deltas for terrain pages.

Status: implemented as a CPU-only directory/oracle layer. Directory entries now
own install/uninstall patch ranges, and the oracle can prepare and commit
per-page install/uninstall batches, reject stale topology batches, derive the
currently selected streaming cut, and validate the parent/child residency
invariants. These patches are still logical terrain-page deltas; translating
them to physical GPU brick/Tree64 ranges remains Phase 5.

Work:

- done: derive parent-child patch lists from directory topology;
- done: store per-entry install and uninstall patch ranges in the directory;
- done: prepare/commit install and uninstall batches with stale patch-generation
  rejection;
- done: validate parent residency, child-range ownership, selected streaming
  leaves, and no parent/descendant overlap in the selected cut;
- done: install planning is parent-before-child and uninstall planning is
  child-before-parent;
- done: parent pointers remain build/topology-only; runtime patch validation
  uses directory links and page indices.

Acceptance:

- installing child pages mutates only the affected parent/child page state;
- incomplete child groups stay hidden behind the parent streaming leaf;
- completing a child group moves the selected cut to those children;
- uninstall refuses a parent while resident children exist, and child uninstalls
  restore the parent streaming leaf;
- valid-cut tests pass in CPU-only form.

### Phase 5: GPU install path

Upload generated pages with per-page transactions.

Status: implemented as the compatibility physical-upload bridge. Generated
pages now allocate stable physical brick ranges before publication, staged
spatial pages are validated and rollback-safe arena owners, and each new
allocation emits a `PlanetVoxelTerrainPageUpload` packet carrying final arena
ranges, byte counts, identity, and the Phase-4 install patches. The sample
renderer consumes hidden staged-page payload uploads while keeping the old
visible-record publication path for compatibility; traversal-native page table
publication remains Phase 6.

Work:

- done: allocate physical brick ranges per generated/staged page;
- done: treat staged spatial pages as first-class physical arena owners in
  validation and rollback;
- done: rebase local brick payload offsets into final arena ranges before a
  page becomes visible;
- done: emit per-page physical upload packets with payload/publication byte
  counts and logical install patches;
- done: upload hidden staged page payload buffers through the existing terrain
  upload pass;
- done: retire old/stale physical ranges through deferred reclamation;
- compatibility: published-page visibility and root rewrites still use the
  existing full `PlanetVoxelChunkSet`/changed-record bridge until traversal
  owns the virtual hierarchy state directly;
- done in Phase 7: Tree64/TLAS physical page ranges participate in the same
  guarded transaction.

Acceptance:

- hidden child payloads can upload without publishing a partial ownership group;
- upload bytes per transaction scale with changed pages, not resident world
  size, for payload buffers;
- rollback preserves the previous valid cut and staged physical owners on
  failure.

### Phase 6: Traversal culling

Replace the flat brick scan for the virtualized terrain path.

Status: implemented as a feature-flagged generated-terrain traversal path.
The brick cull pass can now bind root and child-index buffers, dispatch from
resident roots, descend through child ranges with a bounded local stack, and
emit selected leaves through the same frustum/HZB/draw-list path as the old
flat scan. Generated terrain enables this traversal flag while ordinary brick
scenes keep the flat comparison path. Terrain feedback now records observations
from forced authored leaves as well, so streaming-page feedback comes from the
selected terminal pages rather than from a separate flat-only assumption.

Work:

- done: upload and bind root/child-index buffers for the brick terrain path;
- done: seed traversal from resident roots;
- done: descend through installed child ranges;
- done: emit draw records at accepted leaves through the existing cull output;
- done: emit/pack feedback observations from traversal-selected streaming
  leaves;
- done: generated-terrain traversal no longer needs `parentIndex` to decide
  the cut, while the flat comparison path still uses it;
- done: keep the old flat cull as a comparison path until output and
  performance are understood.

Acceptance:

- selected leaves match the current path where `forceLeafRaster` keeps authored
  terrain leaves;
- page feedback comes from traversal streaming leaves;
- remaining compatibility: LOD history commit still scans the node arena until
  the traversal path owns a compact page-state/history table.

### Phase 7: Tree64/TLAS same-transaction install

Make lighting geometry follow page residency.

Status: implemented as an explicit generated-terrain transaction guard on top
of the Phase 5/6 upload path. Tree64 physical pages are still uploaded through
the compatibility sample-side uploader, but publication now requires the raster
page state table, Tree64 descriptor table, dynamic Tree64 instances, pending
physical pages, and BVH4 TLAS instance-slot indirection to agree on the same
terrain slot/generation before the CPU commits the accepted GPU swap.

Work:

- done: build or restore Tree64 payload with the page;
- done: install raster and Tree64 ranges together;
- done: update dynamic instance/TLAS mapping under the same generation;
- done: evict both together.

Acceptance:

- raster, Tree64, and TLAS agree on page generation every frame;
- reused Tree64 models dominate under camera motion when payload geometry
  revision is unchanged.

### Phase 8: Retire compatibility publication

Generated-world terrain now has an opt-in feedback-driven streaming path. After
the bootstrap/root cut exists, the next spatial cut is derived from resident
pages plus accepted GPU split/merge feedback instead of recomputing the full
CPU metric cut every update. The sample enables this for generated-world
terrain, while tests and debug callers can leave the flag disabled.

Work:

- done: added `feedbackDrivenPageStreaming` as the feature flag;
- done: fixed request coalescing so generated-world feedback compares against
  combined spatial page LOD, matching the shader feedback contract;
- done: in feedback mode, no-feedback updates keep the resident cut unchanged
  and stop reporting metric-only pending detail;
- done: valid feedback split requests replace a resident spatial page with
  finer child pages through the existing page upload/install path;
- retained compatibility: `stagedSpatialEntries` still provides the atomic
  parent/child ownership barrier, and the compact root list still requires a
  compatibility `PlanetVoxelChunkSet` publication until roots are page-state
  driven.

Acceptance:

- resident page count changes through feedback-driven page installs;
- ordinary camera-stable refinement no longer needs the CPU metric cut to keep
  requesting deeper pages;
- GPU memory and upload budgets continue to be enforced by the page residency
  manager;
- remaining: retire the compact-root compatibility publisher after renderer
  roots can be sourced directly from page directory state.

### Phase 9: Raster-safe coarse terrain

Generated-world terrain can now opt into raster-safe coarse brick parents.
The publication path clears the old authored-leaf force bit only for generated
spatial pages when `rasterSafeCoarseTerrain` is enabled. The raster cull then
uses the existing measured heightfield `selfError` projected-pixel test to pick
coarse parents only where the parent is visually safe, while the conservative
OR hierarchy remains available to Tree64/shadows and to the force-leaf fallback.

Options to evaluate:

- retained fallback: authored leaf raster via the compatibility force bit;
- implemented: allow coarse raster only through the measured-error brick LOD
  test, so cliffs and noisy heightfield regions still refine;
- implemented: generated-world sample enables this together with
  feedback-driven page streaming, letting shader feedback request spatial page
  splits;
- still available to evaluate later: a separate raster-only proxy if captures
  show parent OR occupancy is still too conservative in marginal cases.

Acceptance:

- generated spatial pages can publish without `forceLeafRaster`;
- compatibility spatial terrain still publishes with the force bit when the
  feature flag is disabled;
- coarse raster selection is bounded by measured parent error, not by page LOD
  alone;
- remaining capture work: compare flat-region brick counts and image stability
  before deleting the fallback bit entirely.

## Things not to port from virtual geometry

- Do not port mesh cluster group/resplit machinery. Voxels have regular-grid
  cell ownership, not shared mesh vertices.
- Do not port virtual geometry's full hierarchy replay shortcut. Per-page
  deltas are required at voxel node counts.
- Do not make runtime nodes carry parent pointers just to mimic the current
  flat cull. Traversal removes the need.

## Near-term checklist

1. Done: add the relocatable terrain page payload type.
2. Done: move generated-world transient serialization from
   `PlanetVoxelChunkSet` toward that payload type.
3. Done: add a relocatable Tree64 virtual-page sidecar payload and adapter.
4. Done: add a compatibility generated-terrain page source.
5. Done: write fresh CPU-generated pages into the transient store immediately.
6. Done: promote directory state to authoritative page state.
7. Done: assign deterministic payload keys before payloads exist and preserve
   directory state across topology rebuilds.
8. Done: implement CPU patch-list generation and valid-cut tests.
9. Done: install one generated child page by delta while keeping current
   rendering.
10. Done: add traversal behind a feature flag.
11. Move Tree64/TLAS install into the same transaction.
12. Retire full generated-world publication.
13. Revisit coarse raster terrain.
