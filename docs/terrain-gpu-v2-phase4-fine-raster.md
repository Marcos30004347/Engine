# GPU terrain V2 Phase 4: occupancy-only fine raster

**Status:** complete  
**Date:** 2026-08-27

## Outcome

Phase 4 converts immutable Phase 3 column tiles into compact renderer-native
8x8x8 brick leaves. It emits only solid voxel occupancy, byte palette indices,
and `SparseVoxelBrickNodeGPU` records. It does not emit surface patches,
microgeometry, raster parents, child links, or CPU geometry.

The previous experimental raster allocated 512 global scratch words per node
and used single invocations that walked complete 512-cell bricks. The V2 fine
raster uses this bounded pipeline:

```text
reset
  -> classify 16 occupancy words per dense brick (32 cells/invocation)
  -> reduce 16 word summaries per brick
  -> paired node/material exclusive scan
  -> validate both capacities
  -> stable compact brick work list
  -> emit nodes and roots
  -> emit occupancy prefixes and packed material bytes
  -> finalize
```

There is no per-voxel scratch array. The only classification scratch is one
32-byte summary per occupancy word: bits, popcount, occupied bounds, and the
two 4x4x4 coarse masks. Node emission reduces sixteen summaries rather than
walking 512 cells. Material emission recomputes at most 32 column-derived
cells per invocation.

## Exact occupancy contract

Every fine voxel belongs to exactly one half-open chunk and one 8x8x8 brick.
The canonical voxel minimum is compared directly with the globally quantized
Phase 3 column height. No floating ray offset, surface displacement, bilinear
patch, or micro-face participates in occupancy.

Dense bricks are ordered X-major, then Y, then Z. The paired exclusive scan
produces both the stable compact node index and the material base. Every
occupancy cell stores the popcount of all preceding words, so the renderer's
material address remains:

```text
node.materialBase + word.prefix + popcount(bits before the hit bit)
```

Material bytes are written with byte-masked atomic compare/exchange. Each
logical byte has one owner, while adjacent invocations may safely populate
different bytes of the same 32-bit storage word. Dispatch order therefore
cannot alter the payload.

## Leaf-only renderer mode

Generated nodes have no parent or children, are roots of the unpublished
chunk payload, and set `kSparseVoxelBrickForceLeafRaster`. Their nonzero stable
`regionMappingId` selects the renderer's existing terrain occupancy-only path.
That path explicitly disables legacy terrain surface patches and material
microgeometry even if a shared palette entry advertises it.

The result descriptor always carries all three mode flags:

- leaf-only;
- occupancy-only; and
- no microgeometry.

`perNodeVoxelScratchWords` and `microSurfaceBytes` are permanently zero.

## Budget and failure behavior

`terrainGPUV2FineRasterMemory` reports exact scratch and maximum output bytes
before a scheduler admits a chunk. Classification and emission are budgeted as
sixteen word invocations per dense brick instead of one misleading brick item.
The output reservation includes nodes, occupancy records, root indices, packed
material words, and the result descriptor.

Both scan totals are validated before compact output writes. A node or material
overflow changes only the result status; the previous payload is untouched.
Stale/missing Phase 3 column generations behave the same way. Workgroup
variants with 64, 128, and 256 invocations are compiled, and device timing
measurements select the fastest qualified variant.

## Qualification

`SparseVoxelTerrainV2Phase4Tests` verifies:

- the frozen ABI and exact memory reservation;
- all sixteen occupancy-word positions and arbitrary bit patterns;
- material prefixes and addresses;
- empty, full, one-cell, shell, cliff, and boundary fixtures;
- leaf-only/no-micro descriptor validation;
- overflow and stale-column failure atomicity;
- exact adjacent canonical and world AABBs; and
- stable face/depth identity at 1 cm and under subpixel motion.

`SparseVoxelTerrainV2Phase4GPUTests` byte-compares GPU nodes, occupancies,
materials, and roots with the CPU oracle. It covers all three workgroup
variants, deterministic scan/compaction, stale input, output overflow, and
zero microgeometry allocation.

Phase 5 consumes this exact finest occupied set to construct Tree64. Phase 7
will publish the inactive raster and Tree64 outputs together; Phase 4 itself
does not make partial chunks visible.
