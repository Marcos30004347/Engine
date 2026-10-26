# GPU terrain V2 Phase 5: parallel Tree64

**Status:** complete  
**Date:** 2026-08-27

## Outcome

Phase 5 consumes Phase 4's compact 8x8x8 occupancy leaves and constructs an
exact renderer-native Tree64 page entirely on the GPU. The CPU plans immutable
level extents, buffer offsets, identities, and capacity reservations. It does
not receive generated occupancy, masks, nodes, or descriptors back from the
runtime pipeline.

The build is:

```text
clear dense candidate masks
  -> accumulate terminal 4x4x4 masks from Phase 4 words
  -> reduce upper masks, finest to root
  -> flag live candidates (plus the empty root)
  -> one global exclusive scan
  -> validate node capacity
  -> compact masks into Tree64 nodes
  -> link upper levels, root to finest
  -> validate live nodes in parallel
  -> finalize the descriptor in O(1)
```

No stage walks a page from one invocation. Terminal accumulation handles at
most 32 source voxels, mask reduction and linking handle at most 64 children,
and structural validation handles one live candidate with at most 64 children.
The only single-invocation operations are reset, capacity finalization, and
descriptor finalization; none scales with payload size.

## Morton layout and one global scan

Tree64 requires the live children of an internal node to occupy a contiguous
node run. Phase 5 obtains that property without a serial relocation pass:

- candidate coordinates use base-4 Morton order, so a parent's 64 candidates
  are one contiguous range;
- level ranges are placed root-to-finest, although the semantic level number
  remains finest-to-root; and
- a stable scan over all level ranges emits the root at node zero, followed by
  successively finer live levels.

For a parent at level `L`, its child base is the exclusive-scan offset at the
first candidate of its 64-candidate Morton group. Empty candidates contribute
zero, so the populated children are contiguous in population-mask order. Link
dispatches run from the root downward and write the existing packed
`childBase << 1` Tree64 ABI. Terminal nodes set bit zero.

An empty chunk still emits one empty root. This preserves a valid descriptor
and represents zero occupancy without a special traversal path.

## Exact terminal occupancy

One invocation consumes one of a Phase 4 brick's sixteen occupancy words.
Every set bit is converted from the brick's signed canonical origin into a
chunk-local integer voxel. It atomically ORs exactly one bit in exactly one
terminal 4x4x4 mask. Padded cells outside a non-cubic chunk remain empty.

This path reads neither materials nor microgeometry. It does not derive a
surface, offset a hit, inflate a voxel, or create overlapping faces. The sum
of all terminal-mask popcounts must equal Phase 4's occupied voxel count before
the page can become ready.

## Parallel validation and failure atomicity

Each live candidate validates its compact destination, population mask,
terminal flag, child base, child range, and non-empty referenced children.
Atomic counters aggregate validation bits, visited nodes, occupied terminal
voxels, and maximum child count. The O(1) finalizer compares the aggregate
against Phase 4 and writes the 64-byte
`SparseVoxelTerrainTree64DescriptorGPU` only on success.

There is no runtime semantic or full-payload hash. Determinism is established
by the stable scan and differential tests, not by rereading every generated
word from a serial shader.

Stale Phase 4 generations, invalid canonical addresses, and capacity overflow
change only the result record. Node emission begins only after the scan total
fits the reserved inactive range. Consequently a failed rebuild cannot modify
the previously completed inactive payload prefix, and Phase 7 will keep it
unreachable until joint raster/Tree64 publication.

## Memory and scheduling budget

The planner rejects unsupported shapes and candidate reservations before GPU
allocation. `terrainGPUV2Tree64Memory` reports three independent categories:

- two 32-bit mask words per dense candidate;
- flags, offsets, and two scan-summary tiers; and
- maximum packed Tree64 nodes plus the result/descriptor record.

For model side `S`, terminal candidates are `(S / 4)^3`; each upper level is
the previous count divided by 64. The scheduler now charges:

- mask clear + sixteen Phase 4 words per dense brick + every upper candidate;
- every candidate for scan;
- every upper candidate for conservative link admission; and
- every candidate for parallel validation.

Link and validation shaders skip empty candidates, so executed useful work
tracks live hierarchy complexity while admission remains conservative. The
three scan tiers cover up to 1,048,576 candidates. Workgroup variants with 64,
128, and 256 invocations are compiled for device qualification.

## Qualification

`SparseVoxelTerrainV2Phase5Tests` covers:

- frozen 192-byte config and 128-byte result ABIs;
- exact mask/scratch/output accounting and preflight rejection;
- all-cell semantic differential against Phase 4 occupancy;
- randomized 64-cubed conservative occupancy;
- a valid empty root;
- dense DDA versus Tree64 boundary rays;
- deterministic compaction; and
- stale-generation and node-overflow rejection.

`SparseVoxelTerrainV2Phase5GPUTests` uses a 4,161-candidate hierarchy so the
scan crosses multiple blocks. It byte-compares GPU nodes and descriptors with
the CPU oracle for all three workgroup variants and verifies stale/overflow
payload preservation. CMake shader analysis rejects the old 512-cell serial
loop signatures and payload hashing.

Phase 5 creates hidden output only. Phase 7 owns atomic raster/Tree64
publication and eviction, and Phase 8 wires the V2 chain into the runtime
scheduler as the default qualified terrain path.

