# GPU terrain V2 Phase 3: virtual atlas and parallel columns

**Status:** complete  
**Date:** 2026-08-27

## Outcome

Phase 3 connects terrain V2's bounded job infrastructure to the existing
world-map virtual page-table and physical-pool format. It produces immutable
canonical X/Z column tiles for later raster and Tree64 stages. It does not
publish terrain or copy generated columns to CPU memory.

The old page column builder used one workgroup whose 64 invocations looped
over the whole page, followed by a single-invocation validation/hash loop.
The V2 chain has no page-sized invocation loop:

```text
reset (1 invocation)
    -> height lattice (one invocation per sample)
    -> material lattice (one invocation per column)
    -> column reduction (one invocation per column)
    -> finalize status (1 invocation)
```

Three compiled workgroup variants are available: 8x8, 16x8, and 8x16. A
device-profile measurement sweep selects the fastest variant that respects
the device invocation limit. All variants produce byte-identical output.

## Canonical tile contract

A tile identity contains world-bundle revision, X/Z tile coordinates, clipmap
LOD, terrain slot/generation, core X/Z dimensions, and halo width. It excludes
chunk Y intentionally. Vertically stacked chunks therefore acquire the same
tile registry entry and sample the maps once.

X/Z positions are computed as signed canonical integers:

```text
canonical = tile minimum + (local - halo) * (1 << clipmap LOD)
```

Only after this operation is the position converted to normalized map
coordinates. Adjacent tiles and different LODs consequently use the same
integer before conversion whenever they share a sample. Heights are quantized
once into signed canonical height units. Per-column output contains the exact
minimum and maximum of its four height-lattice corners, material, and flags.
No page-local floating datum is introduced.

The default 64x64 core with a two-column halo stores 69x69 height samples and
68x68 column records. Shapes up to 256x256 and halos up to eight are validated
without changing the shader ABI.

## Atlas footprint and pinning

`TerrainGPUV2WorldMapAtlas` reuses `MapExportPageCache` and
`MapExportGpuPageLayout`. Planning expands the complete halo bounds and all
eight required terrain fields:

- refined elevation, with base elevation as the fallback field contract;
- water type and water level;
- river distance/discharge;
- substrate and soil depth; and
- the atomic biome ID/weight sampling group.

Every tile pin contributes its resolved pages to one union target. A page
needed by any pin cannot be evicted. Upload publications are limited by an
explicit byte argument, physical payloads are applied before the page table,
and only a matching commit revision can make a tile ready. Removing the final
pin makes detail pages evictable while required roots remain resident.

## Missing-page behavior

Although a tile is dispatched only after its complete footprint is committed,
the shader still treats residency as fallible. A failed required sample
atomically appends a compact record containing field, requested level, and
logical page X/Z. Fault capacity is bounded.

Height and material sampling may write scratch during a faulted attempt, but
the column pass checks the global fault/overflow latches and performs zero
column writes. Final status becomes `MissingPage`; the tile registry deduplicates
faults and moves the tile to `Faulted`. The CPU may add those logical pages to
the existing pin and retry after a newer atlas transaction. Prior visible
terrain remains untouched.

Only the compact status and fault list are eligible for runtime readback.
Generated height and column arrays remain GPU-owned; the atlas diagnostics
counter for generated-column readback is permanently zero.

## Surface fixture semantics

Phase 3 establishes deterministic map consumption needed by occupancy
generation:

- water columns use authored water level and water material;
- land height uses elevation with deterministic river incision;
- dominant sparse-biome material is overridden conservatively by exposed
  substrate or active river channels; and
- all required samples use one requested map level and a common pinned
  publication.

Phase 4 consumes these columns to emit exact 8x8x8 occupancy bricks. It does
not need to resample maps or reconstruct page-local coordinates.

## Qualification

`SparseVoxelTerrainV2Phase3Tests` verifies:

- field formats, signed coordinate bounds, footprint expansion, and halo;
- exact shared samples between adjacent tiles and across LODs;
- one tile shared by vertically stacked chunks;
- fault/retry state and final-consumer release;
- incremental atlas publications that never exceed their transfer budget;
- pin-safe eviction and atomic page-table commits;
- compact fault-driven page retries; and
- no generated-column CPU readback.

`SparseVoxelTerrainV2Phase3GPUTests` compares every active GPU height and
column byte against the CPU oracle, checks all three compiled 2D workgroups,
and removes a required page from the table to prove that faults are emitted
while previously generated columns remain byte-identical.
