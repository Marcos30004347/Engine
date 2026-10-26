# Full-city voxel scene: offline compiler and runtime renderer plan

## Outcome and architectural decision

The deliverable is one versioned, seekable city scene file, provisionally `.cvscene`. An offline command takes a selected world settlement and produces this file. The renderer opens it, validates its index, loads a small root working set, and progressively streams the rest. It does **not** run dwelling generation, voxelization, greedy meshing, VG packing, or city-wide TLAS construction on startup.

Keep three distinct identities throughout the pipeline:

```text
GeometryAsset ID  -> 10 cm occupied lattice, stable voxel addresses,
                     material-independent surface/LOD/VG data and trace source
Appearance ID     -> exact per-occupied-voxel color and material identity
Instance ID       -> geometry ID + appearance ID + grid placement + stable owner
```

Many house parts share one geometry asset. Two placements may share an appearance or carry different appearance data. Terrain and irregular civil works can have unique geometry assets, but obey the same file, residency and material contracts. One file contains the manifest and all page payloads; the file may internally reuse existing VG and sparse-voxel encoders. Do not evolve `.vsvo` v1 in place: its node-attached materials and full matrix instances are valid for its current use, but do not express a separate per-placement appearance stream or the compact city placement contract.

The existing [production design](WORLD_CITY_VOXEL_RENDERING_PLAN.md) defines grid joins, exact appearance, and memory principles. This document turns them into implementation work and release gates.

## Current seams to use

| Concern | Existing implementation | Required change |
| --- | --- | --- |
| City source | `src/runtime/procedural/world/WorldCityModel.cpp`, `WorldCityModel.hpp`; `WorldSettlementPreview.cpp` runs `settlementGeneratorTasks` | Emit semantic voxel assemblies alongside `CityModelPlan`; use mesh as parity reference and fallback for unsupported forms during development. |
| Dwelling structure | Cell dwelling, `Structure`, `Skeleton`, `FacadeInstance` | Retain integer cell/edge IDs, part ownership, openings and contacts before flattening to `Solid`. |
| City surfaces | `CityModelPlan::surfaces`, road owner/segment arrays, relief `openGround`/`accessGround` | Bake graded terrain, paving, roads, foundations and water boundaries into city spatial tiles with explicit ownership. |
| Voxel scene file | `SparseVoxelSceneFile.hpp/.cpp`, `SparseVoxelSceneFileReader.cpp`, `SparseVoxelFileRuntime.cpp` | Reuse checksummed page IO and atomic write pattern; add a new city container schema with separate geometry, appearance and instance tables. |
| VG build and pages | `VoxelOctreeVirtualGeometryBuilder.cpp`, `VirtualGeometryFile`, terrain VG bridge | Build geometry from occupancy without material-dependent face merging; load offline encoded pages without reconstructing them. |
| VG scene instances | `VirtualGeometryScene::registerRuntimeObject` and file-backed `instantiateObjectInstance` | One generated/file-backed model registration, many compact static placements, one shared hierarchy/page allocation per model. |
| G-buffer and material | VG hardware/software raster and `material.wgsl` | Both raster paths preserve winning instance ID; resolve local voxel to per-instance color and material ID before lighting. |
| Ray scene | `SparseVoxelSceneManager`, Tree64/hardware trace paths | Share one trace geometry per asset, many TLAS instances, and resolve the same appearance address at a ray hit. |
| Sample entry | `samples/sparseVoxelRenderer/SparseVoxelRendererSample.cpp` supports `--open-vsvo` | Add `--open-city-voxel-scene path.cvscene`; route it through a reusable city scene runtime rather than embedding the loader in the sample. |

## Runtime architecture

```text
CityVoxelSceneFile
  header + validated chunk directory + spatial cell index
  geometry catalogue + VG page directory + optional portable trace pages
  appearance catalogue + compressed appearance page directory
  compact instance lists + owner/room/semantic side tables
  terrain/road/water tile records + material references + diagnostics
          |
          v
CityVoxelSceneRuntime
  range reader / IO queue / checksum / decompressor
  cell streamer -> instance lists and coarse root fallback
  geometry cache -> VG page residency and trace geometry residency
  appearance cache -> exact blocks / coarse appearance fallback
  instance manager -> compact GPU records and stable ID mapping
  spatial acceleration -> cell BVH / TLAS updates
          |
          v
existing VG visibility + G-buffer + voxel material/lighting + shadows/rays
```

All material and geometry lookups use a **stable instance ID**, not a transient dense-array offset, for externally visible identity. The GPU may map stable IDs to compact dense slots; picking, history, source IDs and diagnostics retain stable IDs. Static city instances reconstruct transforms from a cell origin, building frame, integer voxel translation and quarter turn. Only moved or replaced instances need previous-transform data.

### File contract

The `.cvscene` file is an index plus independently compressed, aligned page payloads. The header records magic, major/minor schema, endian marker, coordinate convention, city origin, nominal voxel pitch, generator/compiler versions, source/settings hashes, required feature bits, chunk directory offset/count and checksum. Required chunks are geometry catalogue, appearance catalogue, material references, spatial cells, instance lists, page directories and root fallback. Optional chunks include source IDs, room/building tables, diagnostics, camera bookmarks and portable trace acceleration. Use fixed-width little-endian records and checked 64-bit offsets/sizes; no raw C++ structs or pointers on disk.

Geometry records contain bounds, pitch/subdivision policy, occupancy address-map version, connector hash, VG root/page spans, trace root/page spans, and content hash. Appearance records identify their geometry address-map hash, exact block codec, palette/raw data spans, optional shared base/override chain, and content hash. An instance record contains stable ID, geometry ID, appearance ID, cell/building frame reference, integer placement, orientation, flags and optional semantic-table offset. File pages are addressable by `(kind, asset ID, page ID)` and carry dependency lists, uncompressed size and checksum. Keep the root index bounded; overflow/invalid references fail before GPU allocation.

Store the offline packed VG hierarchy and encoded page payloads directly. Reuse `VirtualGeometryFile` page coding where its ABI permits; put a city-level directory around it. Precompute portable trace occupancy/BVH nodes if profitable. Hardware BLAS handles are device-specific: build/cache those at runtime from the file's shared geometry pages, once per geometry asset, not per placement. File validation and version migration are explicit; unsupported major versions get a clear error. Write a temporary file, reopen/check all metadata and sampled payloads, then atomically rename it.

### Residency and failure behavior

Stream in dependency order: cell root and coarse terrain/building fallback, then geometry roots, appearance roots, fine VG pages and trace pages. Geometry and appearance have separate byte budgets and eviction queues; a visible placement pins its required roots while fine pages may churn. A fine geometry page is drawable only when its required appearance page or a valid appearance fallback is resident. Do not expose uninitialized material reads. If exact close-range appearance is unavailable, draw the explicit coarse fallback until the page arrives; record the event. A failed checksum or unsupported codec quarantines that asset/page and keeps a diagnostic fallback. Residency publication to raster and trace is frame-boundary transactional so shading, shadows and picking cannot see half-installed assets.

## Offline compiler pipeline

The CLI, provisionally `city_voxel_compile`, should accept a deterministic world seed/site selection or a saved generator snapshot, output path, build profile, and limits for CPU scratch, workers, source cells, geometry pages, appearance pages and file size. It should emit a machine-readable build report and fail with a named offending building/part if an input cannot be represented. A normal build uses bounded per-part/per-tile scratch and streams pages to a temporary file; it never retains all city triangles plus all voxel grids plus all encoded pages in memory.

```text
settlement snapshot
 -> semantic city assembly (buildings + props + civil surfaces + terrain)
 -> canonical 10 cm lattice and connector resolution
 -> geometry recipes + appearance recipes + instance placements
 -> content-addressed geometry/appearance catalogues
 -> sparse voxel occupancy + exact appearance blocks
 -> material-independent surface triangulation + LOD/stitches
 -> VG pages + portable trace pages
 -> spatial cells, instance lists, coarse fallback and file index
 -> validate, publish .cvscene and report
```

The dwelling assembly must be emitted during `realiseCellDwelling`/structure/skeleton construction, before the current mesh flattening loses part identity. Building walls, roofs, floors, stairs, openings, columns, windows and props become reusable model placements with integer lattice coordinates. Model dimensions and aperture patterns belong in the geometry key. Color, material, wear and per-voxel variation belong in the appearance key. Non-cell structures (castle, church, walls, towers) need adapters that emit the same contract; unsupported pieces compile as **named unique assets**, never silently disappear. For non-building surfaces, clip by city spatial tile with deterministic border ownership; voxelize at ~10 cm only where the feature needs it, and use a coarser multiresolution terrain representation elsewhere.

Derive surface triangles from occupancy without reading appearance. Axis-aligned greedy quads may cross voxel color boundaries because the fragment/ray hit will calculate the exact source cell in model space. Sloped or cut geometry carries a source-cell/face address or splits at cell boundaries. Offline LOD building must preserve connector masks and boundary strips; appearance-specific coarse reduction is stored separately. Compare triangles and occupied cells to the legacy `CityModelPlan` per owner, opening, room path and city tile.

## Ordered implementation steps

Each step produces a mergeable deliverable and a gate. The order keeps the first file-loadable city slice visible early while still arriving at the full architecture.

### 1. Freeze the scene ABI and budgets

**Implemented:** the [phase 1 index specification](CITY_VOXEL_SCENE_FORMAT.md), `CityVoxelSceneFormat.hpp/.cpp`, bounded reader/writer validation, a synthetic round-trip/corruption test, and the selected-city baseline in `tools/terrain/data/city-voxel-phase1-baseline.json`. This is the metadata/index ABI; page payloads and a renderable `.cvscene` file are step 9.

- Add a short schema specification and C++ reader/writer record definitions under `src/voxel/svraster` (or a dedicated `cityvoxel` module shared by editor and runtime). Specify coordinate units, lattice edge rules, stable IDs, hashes, page dependencies, compression codec IDs, format versions and fallback semantics.
- Add a common `CityVoxelGeometryAsset`, `CityVoxelAppearanceAsset`, `CityVoxelInstance` and `CityVoxelSpatialCell` intermediate representation. Ensure runtime headers do not depend on procedural generator types.
- Build a baseline fixture from one selected full city and record current building counts, triangle counts, material use and available target GPU/CPU budgets. Set explicit configurable caps after measurement, with conservative defaults until then.
- **Gate:** round-trip synthetic records; reject truncated, overflowed, cyclic, mismatched-hash and unsupported-version files without allocating large buffers.

### 2. Give the dwelling builder an assembly output

- Add `DwellingVoxelAssembly` beside the current `Solid`/mesh output, capturing cell frame, integer extents, `Skeleton::Piece` and `FacadeInstance` ownership, contact planes, apertures, semantic material choices and instance seeds.
- Move quantization to one place in authoring. Preserve each building's grid pitch and integer subdivisions yielding approximately 10 cm voxels. Use integer placements and quarter turns beneath one outer building transform; produce keyed dimensional variants instead of scaling voxelized parts.
- Implement adapters for every building family emitted by `CityModelPlan`, with explicit fallback as unique parts when a reusable recipe is unavailable.
- **Gate:** a whole-city assembly audit maps every generated building part to one asset placement; bounds/openings/room paths agree with the current model; no missing owners.

**Implemented:** `hierarchy/DwellingVoxelAssembly.hpp/.cpp` emits a source-indexed assembly for cell dwellings. It records the house grid and approximately 10 cm subdivision, integer part placement, local source face ownership, facade support, contact coordinates in both piece frames, opening and room-path identities, semantic appearance keys, per-part seeds, and source geometry bounds. Sub-voxel offsets and non-quarter-turn angles are included in baked geometry keys so compatible parts can still share assets. `CellDwellingGenerator` retains the assembly beside its skeleton. `CityModelSettings::voxelAssemblySink` receives each accepted lot and compound wing synchronously while the source skeleton is alive, avoiding city-wide retention of detailed polygons. `voxelUniqueMeshSink` receives named triangle spans for defence towers, gatehouses, compound towers, lot supports, and compound support details; the dungeon cutaway accepts the same sink for its room assets. `WorldSettlementPreview --voxel-audit-only 1` reports source coverage, candidate geometry sharing, exceptional placements, missing building IDs, and overlapping triangle ownership in `settlement.json` without exporting the large model mesh.

**Phase 2 audit:** The seed 41 city run with `--population 1600 --city-region 1 --relief 500 --terrace-cut-fill 6 --voxel-audit-only 1` produced 3,853 model buildings. All 3,853 had an assembly or named unique mesh part; there were no missing building IDs or overlapping triangle spans. It emitted 3,836 dwelling assemblies with 3,292,187 part placements, 2,748,304 owned structure faces, 782,082 facades, and 1,251,187 connectors. The candidate geometry keys numbered 845,249. A subsequent seed 17 smoke checked the newer building count, window count, base, footprint-bound, and triangle-span comparisons: 10/10 buildings covered, zero mismatches, zero duplicate IDs, zero overlapping spans. The broad `CellDwellingTests` suite still reports 22 failures in existing planner/structure invariants; the phase 2 assembly and dungeon tests pass. This is an authoring intermediate, not a voxelized render asset. The 2,873,065 off-grid placements and 2,276,276 non-quarter orientations in the full-city run make canonicalization and connector resolution the main memory risk for phases 3–4.

### 3. Solve connectors and terrain ownership before voxelization

- Define connector masks for walls, slabs, roofs, stairs, openings and prop sockets. Resolve party walls jointly, with one face owner, before independent building compilation. Resolve foundations against sampled relief and assign terrain/building ownership to the join.
- Partition roads, paving, retaining walls, water banks and terrain tiles with a canonical shared edge sample set. Record which tile owns boundary voxels/faces and which representation supplies the far-field fallback.
- **Gate:** exhaustive adjacent-part rotations and representative city joins have no missing or doubled boundary faces at full resolution; border samples match across terrain tiles.

**Phase 3 authoring contract:** `WorldCityVoxelBoundaries.hpp/.cpp` supplies a global 100 mm terrain lattice, 32 m half-open tile addresses, millimetre-quantized border heights, source-indexed tile feature ranges and explicit coarse fallbacks. Roads and paving come from `CitySurfaceStructure`; accepted building footprints, relief retaining platforms and waterway water/bank ribbons receive separate feature references. No dense terrain voxels or per-tile copies of the source polygons are retained. The city model streams foundation perimeter samples at no more than 100 mm spacing through `voxelFoundationSink`; terrain owns the common ground interface and the named unique foundation mesh owns volume above it. `voxelBoundarySink` receives the live city model, completion plan, relief plan and original terrain field synchronously, so an offline compiler can clip exact source geometry and sample both sides of a tile border using the same global integer address.

Party status now originates on `DwellingWall`, passes through structure faces and skeleton pieces, and reaches `DwellingVoxelPart::partyWall`. The city collects only compact strips of authored exterior party faces while each source skeleton is alive. After all lots have been accepted, it intersects strips from opposing buildings along the canonical lot line and vertical extent. Only those `partyContacts` may suppress a coincident face; a rejected or geometrically separated neighbour leaves the surviving wall visible. The lower building ID breaks a coincident geometry tie, while both source part IDs stay in the contact so the compiler can retain each visible side's material. Connector records now carry semantic masks for walls, slabs, roofs, stairs, openings and prop sockets, including dormer subtypes and facade sockets.

**Verification:** `CityVoxelBoundaryTests` covers owner order, rejected neighbours, overlapping versus separated wall strips, duplicate contact rejection, negative tile coordinates, every tile border sample across seven-by-seven tiles, foundation relief joins and connector masks at all four quarter turns. Its generated street fixture exercises two neighboring houses from the actual planner; that source emits twelve strips on one side of the first shared line and none on the other, so the resolver correctly leaves that authored wall visible. A separate paired-wall fixture verifies one owner where both sides really coincide. `DwellingVoxelAssemblyTests` and the focused `CellDwellingTests --party-access` path pass; the latter checks that a planned party wall reaches the voxel assembly. The seed 17, 900 m relief city smoke emitted 10 foundation joins with 2,536 perimeter samples, 2,545 spatial feature references and 4,505 valid terrain border corner samples; assembly/model coverage remained 10/10 with zero mismatches. That smoke has no row-house party seams. The full-resolution occupied-cell face and LOD seam gate remains to be run when phase 4 produces voxel occupancy and phase 5 produces final triangles; this phase records the shared addresses and ownership that those gates must enforce.

### 4. Compile reusable sparse voxel geometry and exact appearances

- Compile one bounded part at a time into sparse occupancy bricks. Assign stable occupied-voxel addresses, such as `(brick, occupancy rank)`, and preserve this mapping through cache rebuilds and LOD generation.
- Compile per-voxel RGBA/color and shared material-profile ID into a separate appearance stream. Select an **exact** block codec: small palette, wider palette, run/dictionary, base plus sparse overrides, or raw. No forced 8-bit palette and no implicit lossy quantization. Hash and deduplicate geometry and appearance independently.
- Add compiler reports for occupied voxels, geometry bytes, appearance bytes, distinct assets, placements, codec distribution and unique-part causes. Enforce scratch limits with backpressure.
- **Gate:** two placements sharing geometry but using different color/material at every tested cell decode exactly, including after disk round-trip; hashes and bytes are deterministic across worker counts.

**Phase 4 asset contract:** `CityVoxelAssetCompiler.hpp/.cpp` now accepts one bounded part of integer-lattice occupied cells at a time. It canonicalizes placement-independent coordinates, emits sorted 8³ occupancy bricks, and assigns each occupied cell a stable `(brick index, occupancy rank)` address. Geometry hashes include pitch, extent and occupancy; appearance hashes include the geometry address identity plus every exact 32-bit RGBA and 32-bit material-profile value in rank order. The registry deduplicates both streams separately using hash lookup and byte comparison, and reports unique bytes, placement reuse, occupied counts, codec distribution, source-supplied uniqueness causes and a conservative scratch bound. Input order does not affect any encoded bytes or asset IDs. The exact appearance encoder selects raw, 8-bit palette, 16-bit palette or dominant value plus sparse overrides by encoded size; no color or profile bits are reduced. A checked `.cva` cache pair can be written and reopened under a read budget; it is a phase-4 intermediate, not the final `.cvscene` page format.

**Phase 4 verification:** `CityVoxelAssetCompilerTests` checks two shifted placements with identical occupancy but different color and material at every occupied address, repeated-placement reuse, stable IDs/bytes after reversed input order, exact raw/palette8/palette16/sparse decoding, disk round-trip, corruption rejection and scratch-budget rejection. `CityVoxelSceneFormatTests` remains green. This phase produces an asset compiler from occupied source cells; the source-polygon-to-cell voxelization recipes and scene-wide connector clipping are the next integration work before full dwelling coverage can be claimed. Phase 5 consumes these stable addresses for material-independent triangulation and LOD topology.

### 5. Make offline triangulation independent of material

- Refactor `VoxelOctreeVirtualGeometryBuilder`/`SparseVoxelBrickSurfaceMesh` so face exposure and greedy merging depend on occupancy and connector ownership, not material equality. Encode source-cell recovery metadata only where model-local position and normal are insufficient.
- Build cluster hierarchy, occlusion/culling bounds, LODs and seam stitches offline. Preserve full-resolution boundary topology or proven stitch variants. Package encoded VG pages for range reads.
- **Gate:** one shared wall geometry renders adjacent different-colored voxels correctly even on one merged quad; LOD changes do not open building or terrain seams; VG pages reproduce the offline geometry hash.

**Phase 5 implementation:** The octree VG builder has a city geometry-only mode that reads no palette indices; `sparseVoxelBrickGreedyGeometryQuads` merges across material changes. The city mode culls faces against same-level neighboring bricks, so its LOD0 surface contains exactly the exposed voxel faces. The legacy terrain mode retains its material-aware partition while its material resolve is migrated. Face UV, normal and the linearized cluster's brick origin/scale recover the source cell after a merged-quad hit through `CityVoxelSurfaceAddress.hpp`. The offline `CityVoxelVirtualGeometryCompiler` converts a Phase 4 geometry asset into a bounded sparse brick octree, builds the existing VG cluster hierarchy and LOD DAG, encodes deterministic page payloads, and records per-page range offsets, sizes and checksums. An optional connector/party ownership callback suppresses only exposed source faces and contributes a separate face-ownership hash, so two instances can share occupancy while choosing distinct mesh variants when their interfaces differ. No appearance asset is read while building city geometry.

**Phase 5 verification and boundary:** `CityVoxelVirtualGeometryTests` checks two distinct exact appearance streams sharing one geometry and byte-identical VG pages across one versus four encode workers, all 378 exposed LOD0 faces of an 18×9×1 wall, a merged 8×8 face resolving each source cell's different color/material, encoded page decode and disk range reads, multiple LOD levels, ownership clipping and node-budget rejection. `SparseVoxelOctreeVirtualGeometryBuild` passes and covers closed selectable blocks and canonical fine/coarse shared planes. The separate legacy `SparseVoxelOctreePageStreaming` test currently fails two visible-face oracle assertions (8,764 closed-block faces on occluded sides); its material-aware builder path remains unchanged by the city mode, and its residency/no-hole checks pass. The cross-building/terrain LOD seam gate is still open: the current OR-reduced parent is conservative but can overlap a neighbor's finer surface, and no city-specific stitch variants or synchronized interface cut have been proven. Until that gate is closed, connected city assets must remain at the full-resolution cut at their interfaces. The final `.cvscene` page table and runtime material resolve are phases 9 and 7 respectively.

### 6. Refactor VG model registration and static instancing

- Split model registration from placement creation in `VirtualGeometryScene`. The model owns one hierarchy and one page source; placements reference it without cloning hierarchy slices. Support both file-backed city assets and generated/runtime assets through the same API.
- Add a compact static-city instance buffer with geometry ID, appearance ID, cell/building transform reference, integer placement and stable ID. Keep the existing matrix path for animated/dynamic content. Update culling, page requests and capacity accounting to traverse spatial cells/buildings first, then model placements.
- **Gate:** hundreds of thousands of placements of one model use one model hierarchy/page allocation; instance memory scales at the measured compact stride; hardware and software culling agree.

**Phase 6 implementation:** `VirtualGeometryScene` separates model registration from placement creation for runtime and file-backed models. File-backed placements share hierarchy slices and one streaming manager. `CityVoxelStaticScene` retains a 32-byte record per offline placement and a 16-byte building transform, uploads compact records once, and activates only selected spatial cells into the existing matrix instance buffer. Batching prevents a full instance upload after every placement change. The 250,000-placement lifecycle fixture activates 2,000 at a time, checks hierarchy/page reuse, compact GPU readback, stable IDs, cell swaps and capacity failure without changing the active set. The city GPU fixture renders two placements of the same model in hardware and software modes and reads back both identities and material slots; a large-scale GPU culling and streaming stress capture remains open.

### 7. Repair visibility identity and material resolution

- Define one winning-pixel identity `(instance, global cluster, triangle)` for both VG raster paths. Hardware already writes the instance to its low ID target. Redesign software depth/payload winner selection so the same candidate supplies depth and **all** identity fields under ties/concurrency; do not atomically update instance in a separate race-prone side buffer. Expand or replace the packed visibility key as needed.
- In G-buffer resolve, recover the model-local source voxel from the hit, fetch the instance's appearance, and write per-pixel material ID **and** voxel color. Change the material pass to apply this color with shared material profiles, textures and PBR factors; avoid one material program per unique voxel. Define alpha/thin-surface behavior for glass, foliage and grilles.
- Carry stable instance identity through picking, motion vectors, TAA history, screen-space ray queries and debug views.
- **Gate:** overlapping instances of one geometry with different appearances remain correct in both raster modes; tie/depth stress tests, TAA motion, picking and G-buffer readback agree.

**Phase 7 implementation:** Unified visibility now carries depth, global cluster/triangle payload and a dense instance index. The software path has ordered depth, payload and instance passes; the merge reads the hardware instance target and both software instance buffers. The city appearance atlas uploads sparse occupancy bricks, compressed per-voxel values, profile-to-material slots and per-resident-cluster source addresses. G-buffer resolve selects the voxel's material slot; the material pass multiplies its PBR color by exact RGBA. TAA stores the compact placement token across dense index changes, and the CPU picking adapter recovers the full stable placement ID. CPU tests cover visibility tie permutations, all four appearance codecs and two distinct appearances on one geometry; renderer shaders and executable compile.

**Current raster contract and open gates:** City attachment accepts LOD0 cluster sources only. Coarse OR-reduced surfaces need separately encoded appearance LODs and interface stitches before they can safely enter the material path. The deferred opaque visibility path rejects voxel alpha below 255 and material profile opacity below one; glass and translucent foliage need a separate transparent visibility/lighting pass. Cutout grilles should be represented by empty lattice cells. A city GPU capture now verifies two distinct appearance placements, an exact overlap tie in hardware and software modes, red/blue cells on one merged geometry through material-pass color readback, stable picking IDs from GPU winner indices, and camera motion vector parity. A full TAA history accumulation test under dense instance compaction and large-scale streaming remains open. The atlas must be installed before creating renderer pass binding groups, which currently capture its buffer handles.

### 8. Make ray tracing share geometry and appearance semantics

- Build or load one trace geometry per geometry asset. Build TLAS leaves from compact placements; keep stable IDs and model-local hit coordinates. Integrate with Tree64 and, where enabled, hardware tracing without per-instance BLAS duplication.
- Resolve the same voxel address and appearance block for primary ray hits, shadow opacity and reflected/indirect shading. Align normal and alpha rules with raster; avoid hard-coded scene material attached to geometry nodes.
- Publish geometry and appearance residency together at frame boundaries. Define a coarse, conservative trace fallback matching visible fallback coverage.
- **Gate:** raster and ray hit owner, color, material and opacity agree on sampled surfaces and part joins; shadows from two differently colored/materialized placements are distinct where material behavior requires it.

**Phase 8 hardware implementation:** `CityVoxelHardwareMesh` emits only exposed LOD0 faces, with compact primitive-to-cell addresses. `CityVoxelHardwareRayTraceScene` installs one Metal BLAS per unique geometry asset, emits one TLAS instance per static placement, and resolves closest-hit instance/primitive IDs through the same exact appearance lookup used by raster. It validates opaque appearances, uses the integer quarter-turn transform, and publishes missing geometry/appearance as invalid conservative ray instances. The Metal graph test traces two placements with different colors and material profiles through one BLAS, checks stable IDs and normals, and verifies one BLAS build. The earlier Tree64 CPU reference remains a test aid; no Tree64 renderer port is required for this route. A full raster G-buffer versus Metal hit image comparison, streamed BLAS eviction and transparent material behavior remain phase 10/12 gates.

### 9. Build the `.cvscene` writer and reader

- Add a compiler writer that streams page payloads, writes catalogue/spatial indices and checksums, validates by reopening, then atomically publishes one file. Reuse the existing `.vsvo` checked IO and `VirtualGeometryFile` page encoder where possible without coupling the new schema to their old instance/material layouts.
- Add `CityVoxelSceneFileReader` for bounded metadata reads and direct page range reads; add corruption, endian and codec rejection tests. Record source/settings hashes and required renderer feature bits.
- **Gate:** a clean process can open the file and fetch arbitrary geometry/appearance/trace pages; no generator or editor libraries are linked into the runtime reader.

**Phase 9 implementation:** `writeCityVoxelSceneFile` accepts a one-page-at-a-time producer, fills file ranges and FNV checksums, reopens every page for verification and atomically renames a unique temporary file. `CityVoxelSceneFileReader` preflights a bounded index, checks nonoverlapping page ranges, and serves independently checked direct page reads. `writeCityVoxelCompiledScene` builds the catalogue and spatial placement index from compiled assets and the static dwelling scene, validates material profiles and streams geometry, appearance and pretriangulated hardware trace pages. Source/settings hashes and required shared-geometry/trace feature bits remain in the index. The format test launches a separate reader process and fetches geometry, appearance, fallback and trace pages in arbitrary order; it also covers payload corruption, wrong endian, unsupported codec, page limits and failed-publish preservation. The compiled-scene test reopens a packaged scene and decodes its trace page. Renderer startup wiring and spatial streaming remain phases 10 and 11.

### 10. Implement spatial streaming and root fallbacks

- Add a reusable `CityVoxelSceneRuntime` above the existing page systems. Maintain separate budgets for metadata, instance tables, geometry encoded/decoded pages, appearances, trace data and staging. Read camera-visible cells and prioritized page requests; coalesce duplicate asset requests across instances. Pin shared roots; evict by reference count and last use.
- Precompute cell/building coarse proxies offline. Publish cell instance lists, geometry pages, appearance pages and trace handles transactionally. Support camera teleports and demand spikes without retaining the entire city in CPU or GPU memory.
- **Gate:** traverse a full city, teleport, evict and reload with bounded peak memory; no holes, invalid material fetches or stale instances; page-fault and fallback counts are reported.

### 11. Integrate the full city in the voxel renderer

- Add `--open-city-voxel-scene path.cvscene` to `SparseVoxelRendererSample.cpp` as a thin entry point to `CityVoxelSceneRuntime`. The opened scene supplies terrain, streets, walls, buildings, props and water/civil features from one file. Use the existing VG raster/G-buffer and voxel lighting graph; merge city and terrain depth/IDs consistently for shadows, reflections and picking.
- Keep the existing `--open-vsvo` and procedural terrain paths as independent controls during migration. Add camera bookmarks/owner debug overlays from the file, plus counters for visible buildings, part placements, distinct resident assets and memory by pool.
- **Gate:** launch a clean renderer process with only the scene path; first coarse frame appears without generating the world; a full city can be navigated with lighting and materials enabled.

**Current integration status (phase 10/11 work):** The `.cvscene` packager now embeds a checked virtual-geometry payload and an offline cell fallback proxy in addition to occupancy, appearance and hardware trace pages. `CityVoxelSceneRuntime` opens the file under separate page and instance budgets, coalesces shared-asset requests, resolves page dependencies, prioritizes fallback pages, publishes detailed cells only when all required geometry/appearance/VG/trace pages are resident, and evicts cold pages on a camera teleport. Resident assets can be reconstructed for the existing VG appearance uploader. Synthetic file-backed tests exercise two appearances on one geometry, a one-read-per-frame budget, fallback publication, demand spikes, eviction and reload. The embedded `.vg` file is opened by the normal VG reader in the package test.

The full phase 10/11 gate is **not yet met**. The fallback proxy is CPU metadata; the sample does not draw it. `SparseVoxelRendererSample` does not yet accept `--open-city-voxel-scene` or route the file into its VG G-buffer and hardware-lighting graph. The scene file's material catalogue has profile IDs and hashes but no complete PBR/texture payload, so a path-only launch cannot reproduce materials. The offline packager currently consumes precompiled voxel assets and placements; it does not yet compile the complete terrain, roads, water, civil features and every building owner into one selected city file. File-backed VG is currently wrapped as one bounded page per geometry, and the appearance atlas uploader still expects all asset tables at once. The phase-2 audited city of 3,292,187 placements occupies about 176 MiB at the current 56-byte runtime instance stride, close to the 192 MiB instance budget before any renderer-side compact placement copy. These must be made genuinely streamed or compacted before claiming full-city memory bounds or the clean-process render gate.

**Actual dwelling bake smoke:** `CityVoxelOBJPackTool` uses the material-aware OBJ voxelizer, expands sparse octree leaves with a bounded `CityVoxelSparseSceneBridge`, compiles separate geometry and exact appearance assets, packages `.cvscene`, audits every page, and opens it through `CityVoxelSceneRuntime`. Applied to a native generated timber-house mesh at 10 cm, it produced 61,249 occupied cells in one placement and checked five scene pages. This verifies one source-asset path, not the city build: the OBJ importer emits a flattened world-space model, and the tool does not consume `DwellingVoxelAssembly`, `CityVoxelBoundaryPlan`, terrain tiles, or the web city's compact instances. It also cannot supply the missing PBR/texture catalogue. The next production step is a per-part voxelizer called from the synchronous world-generator sinks, with shared asset keys and authored owner IDs, followed by spatial scene packing and the renderer path-only load.

The same real dwelling measured 45,476 resident encoded geometry bytes, 189,262 appearance bytes, **8,033,268 trace bytes**, 179,100 VG bytes and 40 fallback bytes; the `.cvscene` is 8.1 MiB. The current one-quad-per-exposed-voxel LOD0 trace mesh dominates the file. Do not scale this representation to the selected 2,312-building city. The ray cut must consume grouped VG triangles with hit-position-derived source voxel addresses and shared BLASes before a full-city bake is considered memory viable.

### 12. Close coverage gaps and validate production quality

- Compare every authored city owner with rendered/voxelized owners. Close unique structure paths for castles, churches, towers, bridges, walls, streets, interiors and detailed props; do not declare completion based on houses alone.
- Run an automated city fixture suite: building/room/part coverage, seam masks, voxel address/material round-trip, raster/trace agreement, hardware/software visibility parity, page corruption and eviction, deterministic rebuild, peak CPU/GPU memory, startup time and camera traversal. Capture representative street, rooftop, interior and far-view frames against `CityModelPlan` references.
- **Release gate:** 100% intended source-owner coverage or an explicit documented exclusion; no visible cracks or doubled faces at tested LODs; exact close-up per-voxel colors/materials; stable frame identity; no over-budget allocations; single-file clean-process load; repeatable build and cache recovery.

**Phase 12 audit work:** `writeCityVoxelCompiledScene` accepts an authored `placement ID -> owner ID` table, so the file can preserve source building ownership independently from stable placement identity. `WorldSettlementPreview --voxel-audit-only 1` now writes a categorized `modelBuildingOwners` manifest. `CityVoxelReleaseAuditTool scene.cvscene settlement.json report.json` compares the file against that manifest, checks every page checksum and its geometry/appearance/trace/VG/fallback payload, verifies material profile coverage and opaque values, and requires each fallback to enclose its detailed cell. The synthetic scene test covers shared geometry with two source owners and verifies that a missing/extra owner fails the release audit. The tool reports building coverage separately and fails the full release gate while authored civil/terrain features lack their own manifest. A complete civil/terrain owner manifest, complete city `.cvscene`, phase 10/11 renderer integration, and the seam/raster-versus-ray/image/memory gates above still need their city fixtures. The hardware-only, Mega Geometry-inspired continuation is in [CITY_VOXEL_MEGA_GEOMETRY_RAYTRACE_PLAN.md](CITY_VOXEL_MEGA_GEOMETRY_RAYTRACE_PLAN.md).

## Milestones that keep the work observable

1. **Format + one instanced part:** two appearances on one geometry, saved to `.cvscene`, loaded in a clean renderer process. This proves the file and shader ABI before the city compiler grows.
2. **One dwelling + terrain tile:** complete openings, roof and foundation, with correct joins, lighting, ray hits and streamed pages.
3. **One city district:** roads, terrain, adjacent houses, props, mixed pitch/orientation and party walls; measure reuse and memory.
4. **Full selected city:** all building families and civil features from one offline file, with bounded streaming and the release gates above.

The compiler and renderer should report memory separately for geometry, appearances, instances, VG decoded pages, trace data and staging. This is essential: geometry sharing cannot reduce the information in genuinely unique per-voxel colors/materials, and a full-city success claim must include both build peak and runtime peak memory.
