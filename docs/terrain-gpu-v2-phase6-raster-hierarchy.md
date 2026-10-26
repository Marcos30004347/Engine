# GPU terrain V2 Phase 6: optional raster hierarchy

**Status:** complete as a qualified experiment; leaf-only remains default  
**Date:** 2026-08-31

## Outcome

Phase 6 can replace Phase 4's forced-leaf raster with a renderer-native brick
hierarchy without changing the exact fine occupancy. It consumes the hidden
Phase 4 node, occupancy, and packed material buffers directly on the GPU and
builds parents from LOD0 toward one or more exact roots. No generated payload
is copied to the CPU, no microgeometry is emitted, and stale input cannot
modify the previously completed hierarchy payload.

The optional chain is:

```text
copy compact leaves and establish dense LOD0 mapping
  -> for each parent level, finest to root:
       16-way occupancy/material classification
       -> candidate reduction
       -> vector node/material/child exclusive scan
       -> capacity reservation
       -> parallel node and occupancy/material emission
       -> octant-ordered child linking
  -> stable root emission
  -> parallel structural validation
  -> O(1) finalization
```

Parent classification uses sixteen invocations per candidate, each owning one
32-cell occupancy word. Those invocations also reduce occupied bounds and the
4x4x4 coarse mask. Node emission reads sixteen summaries; it never walks all
512 cells on one thread. Each occupied parent cell conservatively OR-reduces
its eight child samples and selects the dominant byte material, breaking ties
toward the lowest material index.

## Exact hierarchy and LOD contract

Level axes are halved independently. The planner stops before a parent voxel
could straddle the chunk's half-open maximum, so an odd or non-cubic chunk may
publish multiple roots instead of padded overlapping coverage. Every parent
stores explicit `firstChild`/`childCount`, and a side buffer stores child node
indices in octant order. Children store the matching `parentIndex` and an exact
copy of `parent.selfError`.

Parent error is at least the maximum child error and has a deterministic floor
derived from its voxel scale. It is no longer just "mixed occupancy means half a
voxel": parent classification now measures the world-space deviation hidden by
the representative against the terrain surface before voxelization.

The column stage writes a compact height-sample sidecar beside the generated
columns. The low eight bits preserve the column validity flags, while the upper
twenty-four bits store the sub-voxel residual between the integer
`heightUnits` shell and the continuous sampled height. The hierarchy builder
consumes that sidecar on CPU and GPU. For each occupied parent cell it samples a
deterministic 3x3 lattice over the represented XZ footprint, reconstructs the
continuous terrain height, and measures how far that surface lies below the
coarse representative's top. It also keeps the occupancy-derived side term:
the eight child samples that collapsed into the parent cell still record
vertical/silhouette variation across the four child XZ subcolumns, plus small
conservative penalties for partial occupancy and material divergence. The
stored value is:

```text
parent.selfError =
  max(max(child.selfError),
      continuousSurfaceDeviation,
      occupancySilhouetteDeviation,
      voxelFloor)
```

This keeps flat terrain near the floor while cliffs, steps, silhouettes, and
material-changing reductions retain larger error, matching the virtual
geometry rule in spirit: project measured geometric error, not node voxel size.
The continuous term removes the former voxelized-proxy caveat; the metric is
now based on the authored terrain height samples used to build the voxel shell,
with occupancy only acting as side/silhouette protection. The monotonic max
makes error monotonic from every leaf to its root. The renderer culler can
therefore admit exactly one node on a hierarchy path: when a parent is
acceptable its children reject themselves, and when it is not acceptable the
parent rejects itself. Generated hierarchy nodes clear the Phase 4 force-leaf
bit but retain the nonzero terrain region mapping, so the draw and material
paths remain occupancy-only and cannot select surface patch or microgeometry
data.

Independently requested coarse spatial chunks still build directly from their
own Phase 3 columns and Phase 4 leaves. They do not depend on a finer chunk,
fine payload residency, or readback. Phase 7 remains responsible for replacing
coarse and fine spatial owners atomically; this experiment does not expose a
partially built hierarchy.

## Memory and frame budget

The CPU planner reserves the worst case before dispatch:

- Phase 4 leaf capacity plus every dense parent candidate for nodes;
- up to 512 material bytes per dense parent candidate;
- up to eight child indices per dense parent candidate; and
- the complete coarsest dense level for roots.

Scratch includes sixteen eight-word summaries per maximum parent level,
four-lane node/material/child count and offset streams, two scan-summary tiers,
dense mappings, and per-level bases. `terrainGPUV2RasterHierarchyMemory`
reports scratch and output bytes separately and always reports zero
microgeometry.

The scheduler no longer treats a parent level as `leafBricks / 8` cheap items.
It charges all sixteen classifiers, reduction, scan/add passes, node emit,
sixteen word emitters, child link, scan-summary work, and level control work.
Consequently hierarchy jobs compete under the same GPU time, workgroup,
scratch, output, atlas-transfer, and chunks-per-frame limits as the required
leaf-only and Tree64 stages.

## Device qualification

Hierarchy is not automatically enabled. The device setting requires an
explicit experiment opt-in, a qualified device tier, valid leaf-only and
hierarchy captures, and the configured end-to-end p99 improvement (5% by
default). `compareTerrainGPUV2RasterHierarchyCaptures` records rendered-node
savings alongside build-p99 cost, end-to-end improvement, and resident-memory
delta. If the full pipeline does not improve p99, `LeafOnly` remains selected
and the hierarchy chain is not scheduled.

## Qualification

`SparseVoxelTerrainV2Phase6Tests` covers the frozen ABI, worst-case budget
rejection, deterministic conservative parent occupancy/materials, measured
representative error above the voxel floor, continuous pre-voxel height
residuals raising parent error beyond the occupancy-derived fallback, monotonic
errors and links, one node per cut path, no parent/child simultaneous coverage,
independent coarse-chunk generation, half-open root bounds, stale rejection,
capture accounting, and the p99 device gate.

`SparseVoxelTerrainV2Phase6GPUTests` byte-compares GPU nodes, occupancies,
materials, children, and roots with the CPU oracle for 64-, 128-, and
256-thread variants. It also verifies deterministic links and errors, zero
microgeometry, bounded pass counts, and stale-payload preservation. CMake
shader analysis rejects a reintroduced single-thread 512-cell emission loop or
microgeometry path.

Ground-level transitions inherit exact half-open node bounds, conservative
parent occupancy, monotonic cut selection, and the occupancy-only DDA contract.
The automated camera/capture matrix remains a Phase 9 release qualification;
until that device capture also clears the end-to-end p99 gate, the production
setting remains leaf-only.
