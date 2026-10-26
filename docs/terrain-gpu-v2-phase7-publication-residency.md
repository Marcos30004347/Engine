# GPU terrain V2 Phase 7: atomic publication and 3D residency

**Status:** complete  
**Date:** 2026-08-27

## Outcome

Phase 7 adds the transaction and residency boundary required to keep generated
terrain GPU-owned. A generated 3D chunk first receives a hidden resident lease.
It is not addressable by rendering until every member of its ownership group
passes payload, generation, revision, occupancy, capacity, and exact half-open
coverage validation.

Publication is a bounded two-dispatch GPU transaction:

```text
validate requests and complete 3D ownership partitions in parallel
  -> compare/exchange the expected even group epoch to an odd writer epoch
  -> write the complete replacement owner range
  -> publish generation, revision, range, and the next even epoch
```

The group epoch is a seqlock. Render consumers may use an owner range only when
the epoch is even and unchanged around their metadata read. A rejected,
interrupted, or stale request never changes the prior owner range. Requests in
one batch must use distinct ownership groups and disjoint destination ranges,
so publication contains no cross-request write race.

Each payload is explicitly occupancy-only and no-microgeometry. The normalized
payload record requires matching raster and Tree64 occupied-voxel counts and
matching terrain generation and geometry revision. LOD replacement coverage is
three-dimensional: up to eight independently generated chunks must be contained,
pairwise disjoint, and exactly fill the prior group's half-open box. This permits
vertical chunk stacks as well as horizontal terrain grids.

## GPU ownership and retirement

The residency manager owns active, hidden, and fence-retired payload leases.
The CPU stores only compact keys, coverage, lease metadata, revisions, priority
state, and rebuild requests; `cpuPayloadBytes` is permanently zero. Eviction
removes an entire complete ownership group and queues its chunk keys for GPU
regeneration. Payload storage is returned to the arena only after the last GPU
reader fence completes.

Resident accounting includes raster nodes, occupancy, materials, topology,
Tree64 nodes, virtual-map atlas pages, generation scratch, and publication
metadata. It counts allocation-class fragmentation and retains retired bytes in
the total until reclamation. The default profile is:

- hard cap: 2048 MiB;
- high watermark: 1740 MiB;
- emergency watermark: 1945 MiB; and
- maximum active plus hidden chunks: 65,536.

Incoming allocation size is included when selecting trim candidates. The oldest
unrequired and unpinned hidden output or complete ownership group retires first.
Chunk-count pressure uses the same policy even below the byte watermark. No
allocation or external atlas/scratch update may make accounted storage exceed
the hard cap.

## Qualification

`SparseVoxelTerrainV2Phase7Tests` covers frozen CPU/GPU ABIs, hidden visibility,
coarse-to-eight-child replacement, stale and interrupted preservation, complete
group eviction, fence reclamation, regeneration without CPU geometry, every
resident category, incoming-allocation watermark trimming, the resident chunk
cap, transactional external accounting, and the 2048 MiB hard limit.

`SparseVoxelTerrainV2Phase7GPUTests` runs the validate/commit shader and
byte-compares its result, group state, and full owner table with the CPU oracle
for coarse publication, an interrupted eight-child build, successful 3D refine,
and a stale CAS. The build also compiles the publication shader as part of
`SparseVoxelRasterShaders`.

Runtime scheduling and renderer consumption of this V2 publication table are
Phase 8. Phase 7 establishes the GPU transaction and storage lifetime contract;
it does not yet switch the shipping terrain streamer to this table.
