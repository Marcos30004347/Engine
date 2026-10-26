# Terrain generation Phase 7: GPU columns

Phase 7 moves map evaluation and canonical shell extraction behind the GPU map
atlas. `terrain_gpu_column_build.wgsl` samples committed atlas pages, evaluates
water, river incision, authored erosion/uplift conditioning, deterministic
detail, infrastructure smoothing, terraces, biome and surface material, then
emits the Phase 1 column ABI and its boundary sidecars.

Generator revision 14 uses one world vertical datum supplied in the build
configuration. It intentionally replaces the old per-page minimum-derived
origin: page residency, dispatch slicing, workgroup order, and LOD may no longer
renumber the voxel Y coordinate of a fixed world height.

The five resumable stages are reset, atlas sampling, top/boundary extraction,
column/wall extraction, and validation. Missing atlas pages and vertical
overflow fail closed. The terminal result contains a semantic hash and occupied
voxel count.

Two output views are emitted without a CPU column upload:

- canonical 16-byte columns for the Tree64 builder;
- interleaved 32-byte column/wall records for the raster builder.

Both Phase 3 and Phase 4 chains accept these external GPU buffers. Before
Tree64 execution, copy the four-byte occupied count from
`kTerrainGPUColumnOccupiedVoxelCountOffset` in the Phase 7 result into
`kTerrainGPUTree64OccupiedVoxelCountConfigOffset` in its configuration. This
keeps Phase 5's raster/Tree64 publication comparison exact.

The CPU oracle mirrors quantized ownership and shell/wall tie-breaking from
decoded atlas samples. It qualifies deterministic hashes, cliffs, shared-edge
fixed-point continuity, malformed inputs, and a 4,500 metre summit. Shader
compilation plus `spirv-val` is the non-device qualification gate; runtime GPU
differential tests require a healthy Vulkan device.
