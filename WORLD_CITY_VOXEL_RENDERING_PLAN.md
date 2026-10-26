# Production plan: instanced voxel cities at 10 cm

## Target

Each generated building is an assembly of reusable voxel model assets. A building stores model IDs, integer placements, quarter-turn rotations, material bindings, and semantic owners. Many houses reuse wall bays, openings, roof units, structural members, stairs, and props. Terrain, streets, unique roof junctions, and foundations stream separately. Raster and lighting consume the same assembly; no city-wide mesh or voxel volume is baked as the production representation.

The finest authored voxel is approximately 0.10 m. This is a **resolution contract**, not a request to allocate a 10 cm dense grid across a city. Store only occupied or surface cells in bounded parts, share assets across instances, and keep only the visible working set resident.

## Evidence in the current code

- `CellHouse` retains `CellGrid`, `CellFrame`, footprints, floors, party edges, and stairs (`CellDwellingPlanner.hpp`). `realiseCellDwelling` currently keeps only `DwellingPlan::cellMetres` from that frame. Preserve integer cell/edge identity and the full frame in a new assembly output.
- `buildStructure` produces typed wall segments, paired skins, real aperture polygons/reveals, materials and room ownership (`CellStructure.hpp`). `buildSkeleton` adds local `Piece` frames and contacts (`CellSkeleton.hpp`). Those stages are the natural part-authoring seam. `CityModelPlan::mesh` is a flattened inspection view.
- `Structure::instances`, `FacadeInstance`, and `facadeAsset` already share triangle assets. They currently scale normalized assets separately by width, height and depth, then flatten them into `Solid`. Voxel assets need dimensioned variants and preserved instance records; nonuniform scaling after voxelization changes pitch, thickness and seam locations.
- `SparseVoxelScene`/`SparseVoxelSceneManager` support shared BLAS plus transformed instances. File-backed virtual geometry has `instantiateObjectInstance`, but generated `registerRuntimeObject` creates one instance, and `instantiateObjectInstance` searches only file-backed objects. This must change before generated city models can be shared extensively.
- `VirtualGeometryInstanceGPUData` is currently **176 bytes per instance**, including current and previous 4x4 matrices. One million parts would require about 168 MiB for that buffer alone, before hierarchy, pages, materials, tracing or staging. A production city cannot use this record per 10 cm part or clone a hierarchy per placement.
- The current VG G-buffer obtains material ID from `instance.materialIndex`; both hardware and software paths need an instance appearance lookup per visible voxel. The brick hierarchy already separates occupancy from occupied-cell palette indices, but its material stream belongs to the brick hierarchy, not a reusable geometry asset with independently instanced appearances. Optional `microSurfaceData` costs eight bytes per occupied voxel and should be used only when measured features require it.
- VG's software visibility payload stores cluster and triangle but omits the instance. `virtualgeometry-visibility-resolve.wgsl` reconstructs the instance from the cluster's owning object, which is only correct for one placement per object. The hardware path already emits the instance index. A shared city model requires exact winning instance identity through software raster, merge, resolve, motion vectors, picking and TAA; six spare payload bits cannot represent city-scale placements.
- `VoxelOctreeVirtualGeometryBuilder` currently reads cell material indices and calls `sparseVoxelBrickGreedyQuads(..., true, ...)`. Face merging therefore depends on material layout. To share geometry across different appearances, build meshes from occupancy alone and recover the visible cell from local hit coordinates, or retain a compact per-face cell mapping. A merged quad cannot carry one material ID if its cells may vary by instance.

## Authoring and asset contracts

Add `DwellingVoxelAssembly` alongside `Solid`, emitted while `CellHouse`, `DwellingPlan`, `Structure`, and `Skeleton` are available:

```text
GridSpec:         cell pitch, integer origin, U/V orientation, vertical pitch,
                  10 cm subdivision policy, quantization version
PartKey:          part kind, integer dimensions, aperture/roof/profile variant,
                  connector mask, geometry-policy version
PartSource:       local occupancy/surface recipe, stable voxel addresses, connectors
AppearanceSource: per-occupied-voxel color and material identities, optional
                  procedural seed and sparse overrides, independent content hash
PartInstance:     PartKey, AppearanceKey, integer x/y/z, quarter turn,
                  building/room/piece/segment owner IDs
BuildingAssembly: building ID, site/world frame, part span, foundation/ground
                  ownership, revision and content hash
```

Catalogue candidates: plain and opening wall bays, corners/ends/junctions, floor and ceiling cells, roof straight/ridge/eave/hip/gable units, plinths, stairs/landings, structural timber, façade joinery, and furniture. A door/window bay has its aperture cut in the part recipe before voxelization. Color, weathering, age and material choices select an independent appearance asset without duplicating geometry. Irregular towers, roof intersections, party-line repairs and terrain contact can use parameterized or unique parts, but every unique part remains a model that may be reused by identical keys. The assembly builder reports exact geometry reuse, appearance reuse and why a part is unique.

Emit from the cell plan for lattice identity and from structure/skeleton for cut geometry, contacts and semantic material choices. Enumerate every `Skeleton::Piece` and `FacadeInstance`; unsupported forms use a deterministic unique-part compiler rather than disappearing. Other dwelling planners need the same assembly API or an explicit unique-part adapter until migrated.

## Geometry and appearance separation

The renderer needs three independently indexed layers:

```text
GeometryAsset  -> canonical 10 cm occupancy/surface, stable voxel address map,
                  material-independent VG pages/LODs and trace BLAS
AppearanceAsset -> exact color + material identity for each occupied voxel,
                   compressed in blocks, independently hashed and paged
PartInstance   -> GeometryAsset ID, AppearanceAsset ID, integer placement,
                  building frame/owner and optional dynamic state
```

Assign every occupied voxel a stable address at compile time, such as `(brick ID, rank of set bit in occupancy mask)`. The geometry asset owns that mapping; the appearance asset stores values in that order. All faces of a voxel read the same record by default; explicitly support a face override stream for roof edges, decals or cut surfaces without multiplying the common case. A color record and material ID are independent: material ID selects the shared shader/BRDF profile; the voxel color is its own albedo/tint input. This preserves a distinct color and material for **every voxel of every placement** while permitting any number of placements to share the same geometry and, when equal, the same appearance.

Compiler output should first establish occupancy and connectors, then construct or select an appearance stream from dwelling material choices, paint/weathering rules and stable per-instance seeds. A generated procedural appearance must resolve deterministically to the same exact per-voxel values for raster, ray tracing and cache rebuilds. Use content hashes to share identical streams. A shared base appearance plus sparse per-instance overrides is useful when only a few cells differ; dense unique data gets its own compressed appearance asset. No implicit 256-color limit or lossy palette quantization: choose an exact codec per block (1/2-byte palette indices, run/dictionary coding, sparse deltas or raw color+material records), with a raw fallback when the palette does not fit.

For axis-aligned voxel surfaces, decode the visible source cell from the hit's **model-local** position, face normal and canonical lattice using a documented half-open boundary rule; this lets material-independent greedy quads span differently colored cells. For arbitrary or sloped generated faces, carry a compact source-cell/face address with the primitive, or split at cell boundaries. Validate the lookup against a one-voxel-per-face reference on edges, rotations and LODs. Coarse LOD appearance needs a deterministic per-appearance reduction or a fine-cell lookup; geometry LOD must never silently substitute another placement's color.

The VG visibility contract becomes `(instance, geometry cluster, triangle)` for the winning pixel in both hardware and software paths. Redesign the software winner representation and tie break so depth and identity belong to the same candidate under concurrent writes; merely adding a separate `atomicMax(instance)` after depth would mix two surfaces. Then resolve the local voxel address and appearance value into G-buffer material ID **and voxel color**, and pass the latter through the material/lighting path. The material pass currently dispatches by material ID and applies a shared `baseColorFactor`; use shared material profiles with per-pixel voxel color so instance color variation does not create one material program per voxel. Ray hits must use the same `(instance, local cell)` resolution, and shadows/alpha rules must observe the same material identity.

## Grid and no-crack rules

1. One house uses one **integer voxel lattice** in its own U/V frame. For a 1.2 m cell, 12 subdivisions give exactly 10 cm; for 2.2 m, 22 do. For other pitches, choose an integer subdivision with a documented effective pitch near 10 cm. Quantize storey levels, wall thickness, sill/head, jetty, roof/eave interfaces, and landings **once** while authoring the assembly. Do not independently round each part's triangle mesh.
2. Place parts by integer voxel translations and 90-degree rotations only. Apply the house's street orientation as one outer world transform to the whole assembly. Never nonuniformly scale an already voxelized part. If a dimension is not a known variant, create a keyed variant at the canonical pitch.
3. Every join has an integer `Connector`: plane, footprint occupancy mask, normal, material boundary role and face owner. Placed neighbours have equal and opposite connector masks. Exactly one part owns each exposed face. Open ends use an end/cap/junction variant. Roof/wall, floor/wall, dormer/roof, stair/landing and façade/opening joins are checked like wall/wall joins.
4. Compile each distinct part once. Its occupancy is represented on the canonical lattice; connector variants decide which boundary faces are emitted. For a high-entropy join, generate one shared junction asset. A visibility seam is never repaired afterward by nudging meshes or adding a float epsilon.
5. Every LOD preserves the connector mask. Keep full-resolution boundary bands or provide verified stitch variants. Joined parts and their fallback caps publish and retire as one assembly transaction so partial streaming cannot reveal holes. Check seams while the camera moves and pages are evicted.

The lattice is per building because street axes and cell pitches vary. Row houses require a **shared party-line contract** from the lot planner: one quantized plane and extent, one wall owner, and a jointly authored junction/closed boundary. `partyEdges`/`partyGaps` currently push walls after planning; resolve that offset before part placement. At foundations, match the terrain's canonical sampled heights and own the join surface explicitly.

## Memory architecture

The basic arithmetic sets the scale: a 10 m × 10 m × 8 m house contains 800,000 possible 10 cm cells. At one byte per cell that is about 0.76 MiB *per house*, and whole-city dense grids or duplicate per-house voxel models multiply this rapidly. A 64³ cell volume is 262,144 cells; at 10 cm it covers only 6.4 m per side. Neither is an appropriate permanent city allocation unit.

- **Catalogue:** hash immutable geometry by `PartKey` and retain one compressed source plus one set of raster/trace pages per distinct model. Separate geometric identity from palette/weathering/placement. De-duplicate equal part payloads even when authored by different houses. Keep a bounded unique-part ratio; report it rather than assuming reuse.
- **Voxel storage:** use sparse occupied 8³ bricks or equivalent; compress empty spans and occupancy masks in geometry assets, then encode occupied-cell appearance in separate, independently streamed blocks. Avoid a full `uint64_t` micro-surface word per city voxel unless a measured material feature requires it. Generate temporary dense scratch only for one bounded part/tile, then release it before the next build. Stream output instead of keeping source triangles, dense scratch, brick hierarchy, encoded VG and decoded VG resident together.
- **Part granularity:** do not make each 10 cm voxel an instance. Instance structural modules such as one bay, one roof segment or a grouped timber assembly. Conversely, avoid unique full-house models. Measure the sweet spot by asset bytes + instance bytes + culling cost. Batch identical small decorative elements into a house-local compound model when their per-element instance overhead dominates their geometry.
- **Instance format:** store a compact, chunk-relative integer placement, quarter turn, geometry asset index and appearance asset index. Keep stable owner IDs in a side table or compressed ranges, not in every hot GPU record. Static city parts do not need two 4x4 matrices; reconstruct transforms from chunk/building frames and discrete placement, with a compact previous state only for moved/rebuilt assemblies. Target and verify a 16–32 byte hot placement record; the exact format follows profiling and shader ABI review.
- **VG and trace sharing:** one hierarchy/page table/encoded page set and one BLAS per model, many placements. Do not clone per-instance VG hierarchies or build per-instance BLAS. Two-level culling rejects spatial cell/building before per-part work. Instance payloads stream by city cell; asset pages use a separate LRU with pinned roots and reference counts. Raster and trace residency publish together.
- **Materials:** geometry and appearance pages have independent residency and reference counts. Appearance block descriptors select an exact codec; one block maps occupied-cell ordinal to its color/material pair. Material profiles, textures and procedural data stay in shared tables/atlases. At distance, an appearance-specific coarse stream may reduce colors with an explicit error metric; close LOD remains exact and interface occupancy remains exact.
- **Budget policy:** give CPU scratch, persistent compressed catalogue, GPU encoded/decoded VG, ray BLAS, materials, and instance tables independent hard budgets. Refuse or defer a build before crossing a budget, and keep a drawable coarse fallback. Record peak and steady-state bytes, cache hit/reuse ratio, unique model count, placements, visible placements, page faults, build time and over-budget events for a full city.

The current 176-byte VG record implies ~168 MiB for one million placements (MiB units); 24-byte compact placements would be ~23 MiB before side tables. Appearance memory has an unavoidable information cost: one million truly independent occupied voxels with arbitrary RGBA8 plus a 32-bit material ID require at least 8,000,000 bytes (~7.63 MiB) for that placement before compression or metadata. A one-byte palette stream is only exact when its palette represents all pairs in its block. These are scale examples, not accepted budgets. Measure geometry reuse, appearance reuse, compressed bytes per occupied voxel, page faults and decode cost independently on a real city before setting limits.

## Materials and lighting

Part appearances carry per-voxel color and shared material-profile identity (stone, timber, plaster, roof, glass, metal, earth and so on). Resolve `(instance appearance, model-local voxel address[, face])` consistently into G-buffer color, material ID, normal, roughness, metallic, opacity and emissive data, and use the same identity for ray hits. `CityModelPlan::surfaceMaterials`, ground weights and the texture manifest define comparison inputs; they are not a sufficient renderer ABI by themselves. Glass, grilles and foliage require a deliberate opacity/thin-surface rule so 10 cm conservative coverage does not turn them opaque.

## Runtime composition with terrain

```text
World/site/city generation
  -> CellHouse + DwellingPlan + Structure + Skeleton
  -> DwellingVoxelAssembly, integer connectors and voxel appearance recipes
  -> content-addressed geometry and appearance catalogues
  -> compressed 10 cm geometry -> shared VG pages + shared trace BLAS
  -> compressed per-voxel appearances -> independently resident material pages
  -> spatial building/part instance tables and TLAS
  -> terrain/city G-buffer, shadows and lighting
```

City spatial cells stream **instance lists**, not baked voxel chunks. Terrain uses the city's street-graded `CityReliefPlan::openGround`/`accessGround`; pads, paving, retaining pieces and foundations have explicit city ownership. Place the selected site through `WorldSettlementSite::origin`, then subtract the terrain cache's immutable elevation origin for renderer Y. Check border continuity at every terrain LOD. A terrain or relief change invalidates foundations and local instance placement, not the common wall/roof catalogue.

The generated VG scene needs a shared-model/many-instance API, compact static instance path, exact software winner identity, per-voxel appearance lookup and atomically published raster/trace residency. Its current one-instance runtime object is a blocker for production instancing. The trace backend must share one BLAS per distinct model and place it many times in its TLAS; shadow and primary visibility must agree about the same instances and materials.

## Implementation sequence and acceptance gates

1. **Assembly parity:** add `DwellingVoxelAssembly` to the cell dwelling path and preserve `Solid`. Verify every skeleton piece and façade instance appears, with matching bounds, openings, room paths, roof silhouette and per-voxel material coverage. Measure distinct geometry keys, appearance keys and placements.
2. **Grid compiler:** implement common wall, corner, door, window, slab, roof and stair parts at ~10 cm. Validate all connector masks, rotations, minimum thickness, deterministic hashes and zero missing boundary faces. Add the unique-part path for every remaining form before claiming complete-house support.
3. **Renderer instancing:** register one generated VG model with many compact placements and independent appearance bindings; share pages/hierarchy and trace BLAS. Redesign software visibility to preserve the winning instance and verify parity with hardware raster, motion vectors, picking and TAA under overlapping instances. Validate material and shadow parity, culling, eviction/reload and memory with a synthetic million-placement stress fixture.
4. **Exact voxel appearance:** compile two appearances for the same wall/roof geometry with deliberately different color and material for adjacent individual voxels. Render both placements simultaneously, including a merged quad, and compare G-buffer, lit color, ray hits and alpha/shadows to a reference. Exercise palette overflow, raw fallback, sparse overrides and page eviction.
5. **One complete house on world terrain:** use the actual site frame and relief; verify close-range seams, every LOD transition, shadows in both directions, walkable openings, per-voxel material variation and bounded memory.
6. **One generated city:** stream buildings and non-building assets. Test party walls, mixed orientations and grid pitches, complex roof joins, foundation/terrain joins, camera travel and churn. Publish a memory report for full build peak and steady rendering, then set budgets from measured target hardware.
7. **Release gate:** no missing/duplicate connector faces; deterministic geometry, appearance and instance hashes; exact close-range per-voxel colors/materials and raster/trace parity; bounded build and resident memory; versioned cache recovery; no visual gaps under streaming; representative captures for houses, arches, stairs, roofs, towers and adjacent buildings.
