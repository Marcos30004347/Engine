# City virtual geometry hardware ray tracing plan

## Decision

Use the existing virtual geometry hierarchy as the single geometric source for raster and hardware rays. The ray tracing system selects a *ray cut* of geometry groups from that hierarchy, streams only the selected groups, builds or reuses acceleration structures for shared geometry, and builds a TLAS of city placements. Exact voxel color and material remain in per-instance appearance storage. The ray cut must cover all directions relevant to shadows, reflections and indirect light; camera-frustum visibility alone is insufficient.

The model is informed by NVIDIA's [RTX Mega Geometry Cluster LOD architecture](https://github.com/NVIDIA-RTX/RTXMG/blob/main/docs/ClusterLOD.md): offline cluster groups, a GPU-selected LOD cut, on-demand residency, shared/cached BLAS, and a bounded acceleration-structure pool. The NVIDIA [SDK](https://github.com/NVIDIA-RTX/RTXMG) uses `VK_NV_cluster_acceleration_structure` on Vulkan and NVAPI on DX12. The current macOS renderer uses [Metal primitive and instance acceleration structures](https://developer.apple.com/documentation/metal/ray-tracing-with-acceleration-structures), so its first production implementation must use bounded grouped triangle BLAS rather than assuming CLAS exists on Metal. A Vulkan CLAS backend can be added behind the same cut and shading contract when the device exposes the extension.

## Implemented checkpoint (October 2026)

- The offline `.cvscene` packager partitions each exact LOD0 voxel surface into trace groups of at most 8,192 triangles. Each group is a separately checked page and retains exact source voxel face addresses. The release audit compares the combined group faces with the source geometry and rejects duplicates or gaps.
- The file runtime exposes resident trace groups. The Metal scene loads them on demand, caps new BLAS inputs and builds per update, shares each group BLAS across differently colored placements, publishes a compact TLAS cut, resolves hits through the placement appearance, and evicts unused group BLAS inputs. Pending or over-budget cuts report incomplete coverage.
- `CityVoxelStaticScene::loadSceneIndex` reconstructs placement transforms and spatial cells from the file index, including quarter turns and millimetre city origin. A hardware test starts from that reconstructed scene rather than compiler-owned placement arrays.
- The open-window diagnostic dwelling packages 16 placements, 11 geometries, 16 appearances and 32 trace group pages. Indexed lattice vertices reduced trace storage from 8.58 MB to 5.05 MB. A Metal test verifies two trace groups, BLAS reuse, per-instance color and material, and eviction.
- `CityVoxelHardwareRayRender` reads only the `.cvscene`, traces camera and sun-shadow rays with the grouped Metal scene, and writes a diagnostic PPM. A 640×360 interior view resolved 222,732 hardware surface hits and used 32 shared BLAS groups across 37 TLAS group instances.

## Current code and problems to solve

- `CityVoxelHardwareRayTraceScene` follows the file runtime's spatial-cell publication, but its trace groups are still a separate LOD0 partition. They are not selected from the virtual geometry DAG, and all groups of a detailed asset must be resident before that spatial cell publishes.
- The compact city instance record still encodes quarter turns only. Native dwelling `CellFrame` may rotate freely, so the file index, static scene, raster placement matrices, and Metal TLAS transforms must share an indexed arbitrary-orientation frame before real dwellings can be placed without snapping or baking each owner into unique world-space geometry. The existing `frameId` field is a possible index, but it has no persisted frame table or runtime consumer yet.
- `VirtualGeometryMetalRayTraceBackend` already caches immutable geometry, supports build budgets, input-read synchronization, deferred eviction, GPU-written instance counts, and closest-hit/any-hit queries. `VirtualGeometryRayTraceCutPass` selects a ray cut for generated terrain. The city and general VG paths should consume this backend through a common cut record rather than duplicating its resource lifecycle.
- `.cvscene` still wraps an entire `.vg` file in one page per geometry. Trace pages are independently addressable but are not yet keyed to VG cluster groups or LOD nodes.
- The hardware scene is exercised through Metal graph tests and a path-only diagnostic renderer with simple albedo and sun shading. The interactive sample does not yet load `.cvscene` into its raster/PBR lighting frame graph, and the trace cut must still be selected with conservative secondary-ray reach before a full-city lighting release.
- Indexed lattice vertices reduced one diagnostic 10 cm dwelling's trace stream from 8.58 MB to 5.05 MB. This is still an exact per-voxel-face mesh and needs a coarse hierarchy plus group LOD selection for a full city of unique assets.
- Phase 5's OR-reduced coarse voxel geometry and cross-owner seams are not yet watertight under independent LOD choices. Coarse groups cannot enter a shipping ray cut until boundary ownership and stitch tests pass.
- Material records contain only profile IDs and hashes. The scene file needs complete profile parameters and referenced texture payloads to reproduce a path-only render.
- The current dwelling OBJ bridge now assigns intersecting 10 cm cells to door/window frames and removes them from the shell before compiling independent placements. This is an offline ownership rule for the diagnostic dwelling. The native `DwellingVoxelAssembly` still needs the equivalent occupancy compiler and a cross-part seam audit before the rule can cover every world-generated house.
- `CityVoxelCityAssembler` now constructs compact owner cells and part placements directly from previously compiled geometry/appearance IDs, with checked 10 cm bounds, owner identity and quarter-turn placement. `CityVoxelInstancePackTool` reads archetype references plus owner locations and repackages existing checked geometry, appearance, trace and VG pages verbatim; repeated houses do not rerun voxelization or triangulation. A 1,000-owner release-audited test uses two shared assets for 2,000 parts. A placement stress file at 1,115 native house locations contains 17,840 placements but only 11 shared geometries and 16 appearances; Metal builds 32 shared BLAS groups for 41,255 TLAS group instances. This stress file uses one diagnostic dwelling archetype and omits native terrain, so it is not the full-city release scene.
- The instance-only repack copies existing checked asset pages verbatim and writes new placement/fallback records. The 1,115-owner stress repack took about one second and 26.9 MB peak resident memory; its hardware-ray PPM is byte-identical to the earlier rebuild path. `DwellingVoxelAssembly` exposes per-part occupancy priority and disables glass for the opaque ray scene. `CityVoxelDwellingCellSampler` now samples typed wall volumes, detailed apertures, concave source polygons, and separate facade atlas instances on the native 10 cm lattice; `CityVoxelDwellingCompiler` consumes those cells through ownership and shared geometry/appearance compilation. `CityVoxelTerrainTileSampler` produces bounded, deterministic heightfield tiles with neighbour-halo skirts. The first real dwelling revealed a fully covered 17-cell soffit, a sub-10 cm fixed timber, two portal skins collapsed into the same cells, and 6.5 cm door relief hidden in its leaf. Fixed structural parts with no retained cells are counted and culled; paired portal skins use one deterministic canonical skin per structural segment and count a fully hidden partner. Sub-voxel wooden `WallBlock` relief paints overlapping door-leaf cells in the appearance stream; any protruding cells remain geometry. Other movable facade parts and furniture still fail if fully covered. `WorldSettlementPreview --voxel-native-export 1` packages the first validated native dwelling for hardware-ray inspection and audits a bounded sample of actual city dwellings and one terrain tile.
- The selected native full-city export at `build/renders/cell-landforms/final-castle-city-12m/city-model-c10.json` has 1,288 buildings and 3,881,529 mesh triangles. Its building footprint envelope alone spans roughly 534 m by 617 m, or 32.95 million top-surface cells at 10 cm. Packaging that envelope as a single LOD0 terrain mesh would exceed the current trace residency budget before adding houses. Full-city capture therefore depends on tiled terrain pages and a coarse ray cut, not just a larger instance cap.

## Shared contracts

1. **One authored cell address.** Every raster cluster triangle and ray primitive resolves `(geometry ID, source cell, face)` or an explicitly authored coarse-appearance address. A greedy triangle may span differently colored voxels, so ray shading must use the model-local hit position and face normal with `CityVoxelSurfaceAddress` to recover the exact 10 cm source cell; a single primitive-to-cell ID is valid only for triangles bounded to one cell. The placement maps this to an appearance ID, stable owner ID, and material profile. There is no material baked into shared BLAS geometry.
2. **One visibility epoch.** Raster page table, appearance table, ray cut, BLAS handles and TLAS are published together after all referenced uploads/builds complete. A stale hit never indexes a reclaimed appearance allocation. Retired epochs stay alive for frames in flight.
3. **One seam cut.** Connected building parts, party walls, foundations, terrain tile borders and civil structures use a common boundary constraint. A group may refine only when the connected boundary group has a compatible level or a validated stitch. Full-resolution border strips are the initial safe policy.
4. **Conservative secondary visibility.** The ray cut includes off-screen casters and reflection candidates. Budget pressure coarsens to resident roots and flags invalid coverage; it does not silently omit geometry.
5. **Measured budgets.** Track encoded VG, decoded triangles, appearance words, BLAS/CLAS, TLAS, scratch, upload ring, and retired epochs separately. Build/transfer caps are per frame. Pool overflow is visible in counters and quality state.

## Ordered implementation

### A. Repackage the offline geometry

- Split `.vg` into a small hierarchy/root index plus independently checked cluster-group page ranges. Store source-cell/face tables and connector masks alongside each group; keep appearance pages independently content-addressed. Split the trace triangle stream by the same groups, or derive trace input from resident decoded VG pages. Do not retain a whole-model trace duplicate on disk unless a measured compression or startup benefit justifies it.
- Generate a conservative root ray mesh for each shared geometry and one fallback per spatial cell. The root may be lower fidelity, but must enclose or occlude the intended solid consistently and carry a valid appearance mapping. Export complete material profiles and textures in the same file.
- Gate: arbitrary page read and dependency closure in a clean process; deterministic hashes; bounded page sizes; no whole-asset read requirement.

### B. Select a ray cut from the VG DAG

- Extend the existing VG cut logic with ray error, light/view reach, and connector constraints. Emit group IDs keyed by geometry revision and placement cut class. Share one selected cut among identical instances when their projected/ray error and boundary state permit it.
- Pin roots, request missing groups, and keep the previous valid cut during streaming. Track both desired and published cuts. Do not promote a group until geometry, appearance address data and its acceleration structure are ready.
- Gate: cut closure and seam validation for every adjacent pair; camera teleport converges without missing shadow casters; stable owner and voxel address through LOD changes.

### C. Build acceleration structures with platform adapters

- **Metal baseline:** assemble resident groups into a bounded BLAS for each shared geometry and compatible cut class; cache by `(geometry hash, cut hash, revision)` and share it across placements. Keep the number of cut classes bounded so group selection does not multiply TLAS instances by the group count. Evaluate Metal's multi-level instancing for very large model assemblies after the single-level grouped baseline is measured. Build the TLAS from compact placements. Reuse the current `VirtualGeometryMetalRayTraceBackend` budget, completion and delayed-eviction machinery. Never build a full LOD0 BLAS for the entire city at startup.
- **Vulkan NVIDIA path:** when `VK_NV_cluster_acceleration_structure` is available, build CLAS for resident VG clusters, assemble group BLAS, and use the same cut/appearance/identity tables. Detect support at runtime. This is an optimization path; correctness and file format do not depend on it.
- Cap new builds and build bytes per frame. Preserve previous BLAS/TLAS epochs until GPU completion. Record cache hits, shared instances, build time, scratch peak, resident and retired bytes.
- Gate: a million repeated placements must not imply a million BLASes; eviction/reload and camera cuts stay inside budgets, with stable TLAS hit identity.

### D. Shade ray hits exactly

- Map `(TLAS instance, BLAS group, primitive)` to source voxel address. Fetch RGBA and material profile from the placement appearance; then evaluate the same PBR profile and texture bindings as raster. Define alpha/cutout separately from opaque traversal; a conservative opaque proxy must never masquerade as colored glass.
- Implement closest-hit for reflections and indirect light, any-hit/opacity for shadows, and motion/stable IDs for temporal reuse. Compare raster G-buffer hits against hardware ray hits at sampled points, including differently colored instances of one geometry and connected seams.
- Gate: owner, material profile, color, normal, opacity and hit distance agree at LOD0 within defined geometric tolerance; coarse error is bounded by the selected ray cut.

### E. Full-city release and measurement

- Build a selected city file from every authored owner, including terrain, roads, water, castles, towers, bridges, interiors and props. Feed the authored owner manifest to `CityVoxelReleaseAudit`; missing or unexpected owners, missing fallback/trace/VG data, corrupt pages or unmapped material profiles fail the build.
- Run clean-process startup, scripted street/roof/interior/far-view cameras, teleport/evict/reload, frame captures, seam and hit parity tests. Record p95/p99 frame time, startup time, CPU/GPU peak and each residency pool. Release only when the scene path alone reproduces the city with lighting and materials.

## Migration checkpoints

1. One instanced model: file-backed raster and ray cut share an exact appearance table.
2. One dwelling and terrain tile: connected full-resolution boundary, shadows and reflections agree.
3. One district: group streaming, mixed material instances, teleports and BLAS sharing under caps.
4. Selected full city: owner audit and clean-process render/trace gates pass.
