# Virtual Geometry

## Scope

This document describes the current virtual geometry pipeline as implemented in the engine today:

- authoring and file creation
- on-disk encoding
- runtime page streaming
- render passes
- the main shader kernels used by each pass

The material shading stage is covered in more detail in `docs/material-pass.md`.

## High-Level Model

The system treats a mesh as a hierarchy of page-streamed meshlet data instead of one monolithic vertex/index buffer.

- Offline/editor code builds clusters, hierarchy nodes, pages, page dependencies, and install/uninstall rewrite lists.
- The `.virtualgeometry` file stores that authored hierarchy plus compressed page payloads.
- Runtime code streams only the pages needed for the current camera.
- Rendering is split into culling, depth, Hi-Z generation, final draw, and material shading.

At runtime, the GPU never scans arbitrary raw mesh files. It consumes:

- instance data
- hierarchy nodes
- scene page table entries
- resident page payload bytes
- per-page streaming priority buffers

## Authoring And File Creation

## Input And Build Stages

Virtual geometry content is authored in `src/editor/virtualgeometry`.

The build flow is:

1. Source meshes are imported by editor/CLI tooling.
2. Triangles are partitioned into meshlet-like clusters.
3. Each cluster gets:
   - local vertex data
   - local indices
   - LOD error bounds
   - cone data for backface-style rejection
   - mesh-part / skinning metadata
4. Clusters are organized into a hierarchy.
5. Hierarchy leaves and cluster groups are packed into streamable pages.
6. Page dependency lists and hierarchy flag rewrite lists are emitted.
7. `VirtualGeometryFile::write()` serializes the final authored result to disk.

The main responsibilities are split like this:

- `VirtualGeometryBuilder` builds clusters and merged build data.
- `VirtualGeometryHierarchyBuilder` finalizes page layout and hierarchy rewrites.
- `VirtualGeometryFile::write()` writes the final `.virtualgeometry` file.

## Why Install/Uninstall Rewrite Lists Exist

Each page can affect more than raw mesh payload residency.

- Installing a page can enable hierarchy nodes or cluster flags that were previously masked out.
- Uninstalling a page can disable nodes or restore parent-only traversal.

Those rewrite lists are precomputed offline and stored in the file so runtime page installs do not need to rebuild hierarchy state from scratch.

## File Layout

The file uses `VMESH` metadata and stores fixed tables plus variable page payloads.

The serialized layout is:

1. metadata/header
2. hierarchy table
3. shape table
4. material table
5. skeleton + mesh part table
6. page table placeholder
7. page dependency lists
8. page install update lists
9. page uninstall update lists
10. page payload data
11. final metadata rewrite
12. final page table rewrite

Important metadata fields include:

- hierarchy counts and offsets
- page counts and offsets
- shape/material/skeleton table offsets
- page dependency/install/uninstall offsets
- page data offset
- quantization factor
- unit scale
- max page size

## Page Payload Encoding

Each page is flattened into a `PageBuffer`, which stores:

- a page header
- per-group meshlet offsets
- a meshlet descriptor table
- packed position stream
- packed normal stream
- UV stream
- index stream
- optional bone-weight stream
- page dependency ids

### Meshlet Descriptor Contents

For each meshlet, the descriptor stores enough information for the GPU to decode vertices directly from the raw page payload:

- offsets and sizes for position, normal, UV, index, and bone-weight data
- vertex count and triangle count
- quantized position span
- meshlet-space minimum position
- self LOD bounds
- parent LOD bounds
- cone data
- packed local group / cluster indices

### Geometry Compression

The authored encoding is intentionally GPU-friendly rather than maximally compact.

- Positions are quantized and bit-packed.
- The quantization scale is derived from `quantization_factor * unit_scale`.
- Normals are octahedrally encoded and packed with `pack2x16Snorm`.
- UVs are stored as raw floats.
- Indices are stored as `uint8` meshlet-local indices.
- Bone weights are stored as packed weight/index pairs.

### Page Compression Modes

Each page payload can then be written as:

- `MESHLET_RAW`
- `MESHLET_MINIZ`
- `MESHLET_LZ4`

The file stores, per page:

- compression mode
- uncompressed size
- compressed payload bytes

If page padding is enabled at build time, pages are first padded to a common max size before optional compression.

## Runtime Streaming Model

At runtime, `VirtualGeometryScene` and `VirtualGeometryStreamingManager` manage residency.

Each object gets:

- a scene-wide page table range
- a hierarchy upload
- a per-object dependency graph
- a streaming manager that owns pending, ready, and installed pages

Each page table entry tracks:

- page buffer offset
- page size
- cluster offset/count
- install bit
- permanent priority slot

### Streaming Lifecycle

1. The GPU culling pass accumulates per-page priorities.
2. The streaming selection pass reduces that signal into install and evict candidates.
3. The CPU streaming manager requests page loads from the file.
4. Loaded pages are decoded and installed into the scene page buffer.
5. Install/uninstall rewrite lists update hierarchy visibility masks.
6. The page table is updated so later frames can render the newly resident page.

The runtime respects:

- parent-before-child dependencies
- memory pressure
- install budgets
- safe eviction rules for non-root pages

## Render Pass Pipeline

`VirtualGeometryRendererPass` currently registers passes in this order:

1. prepass culling
2. depth prepass draw
3. depth pyramid / Hi-Z generation
4. final culling
5. streaming page selection
6. hardware draw
7. material pass

## 1. Prepass Culling

Purpose:

- traverse the hierarchy before the depth prepass
- find coarse visible clusters
- avoid updating streaming priorities in this phase

Outputs:

- visible hardware cluster list
- indirect dispatch / draw arguments
- culling counters

Main shader module:

- `assets/shaders/virtualgeometry/wgsl/culling_multipass.wgsl`

Main kernels used by the pass:

- `initSync`
- `setupRootNodes`
- `prepareIndirectDispatch`
- `processHierarchyNodes`
- `prepareClusterDispatch`
- `processClusters`
- `prepareHWDrawIndirectArgs` when indirect-count draw is unavailable

Kernel overview:

- `setupRootNodes` seeds traversal from instance roots.
- `processHierarchyNodes` walks the page-streamed hierarchy, applies LOD tests, frustum tests, and optional Hi-Z occlusion, then either descends or emits cluster work.
- `processClusters` evaluates final cluster visibility, projected size, cone rejection, and hardware/software routing, then appends visible cluster records and draw counts.

## 2. Depth Prepass

Purpose:

- draw visible clusters into the scene depth buffer before final culling
- generate a fresh depth buffer for Hi-Z construction

Inputs:

- page table
- resident page payload buffer
- visible cluster list from prepass culling
- instance and mesh-part transform buffers

Main shader module:

- `assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl`

Main kernel used by the pass:

- `vs_main`

Kernel overview:

- Decodes meshlet vertices directly from the resident page payload.
- Reconstructs positions, normals, UVs, and optional skinning data.
- Transforms vertices to clip space and emits triangle geometry for depth-only rendering.

## 3. Depth Pyramid / Hi-Z

Purpose:

- build a hierarchical depth texture used for later occlusion decisions

Main shader modules:

- reduction to power-of-two: `hzb-depth-reduce-pot`
- chunked SPD pyramid generation:
  - `depth_pyramid_spd_chunked_source_depth`
  - `depth_pyramid_spd_chunked`

High-level behavior:

- Optionally reduces the input depth to a power-of-two-friendly layout.
- Generates multiple mip levels of a conservative depth pyramid.
- Produces the Hi-Z texture consumed by final culling.

This pass is not virtual-geometry-specific in its math, but it is a critical dependency for the final culling stage.

## 4. Final Culling

Purpose:

- repeat hierarchy traversal using the freshly built Hi-Z
- update page streaming priorities
- build the final visible-cluster list for the main draw

This pass uses the same culling shader module as prepass culling, but with streaming priority updates enabled.

Additional effect in this phase:

- visible or needed-but-missing pages contribute demand into the streaming priority buffer
- that demand later feeds page install and eviction selection

## 5. Streaming Page Selection

Purpose:

- reduce the full page-priority array into a small install list and a small eviction list

Main shader module:

- `assets/shaders/virtualgeometry/wgsl/streaming-page-selection-cs.wgsl`

Main kernel used by the pass:

- `reduceStreamingCandidates`

Kernel overview:

- Reads either raw page priorities or intermediate scratch candidate buffers.
- Runs a reduction that keeps the top `N` install candidates or the lowest-priority eviction candidates.
- Writes fixed-size install and evict buffers used by the CPU streaming manager.

The current selection count is `VirtualGeometryScene::STREAMING_PAGE_SELECTION_COUNT`, which is `64`.

## 6. Hardware Draw

Purpose:

- render the final visible clusters after culling
- write geometry/material identification buffers instead of directly performing full shading

Outputs:

- packed geometry ids low/high
- material id
- material UV
- depth
- color target attachment, for later material shading

Main shader modules:

- vertex: `assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-vs.wgsl`
- fragment: `assets/shaders/virtualgeometry/wgsl/virtualgeometry-meshlet-fs.wgsl`

Main kernels used by the pass:

- `vs_main`
- `fs_main`

Kernel overview:

- `vs_main` decodes and transforms meshlet geometry from resident page data.
- `fs_main` does not perform material shading. It packs instance/page/group/cluster/triangle identifiers into render targets and writes `materialId` plus `materialUV` for the later material pass.

## 7. Debug Info Pass

Purpose:

- visualize internal virtual-geometry ids and debugging overlays

This pass is auxiliary and does not change residency or visibility decisions.

## 8. Material Pass

Purpose:

- shade the geometry written by the hardware draw pass using the virtual texture system

This pass is documented separately in `docs/material-pass.md`.

## Shader Summary

## `culling_multipass.wgsl`

Primary role:

- hierarchy traversal, culling, cluster emission, and streaming demand generation

Main kernels:

- `setupRootNodes`
- `processHierarchyNodes`
- `processClusters`

## `virtualgeometry-meshlet-vs.wgsl`

Primary role:

- GPU decode of compressed meshlet payloads into draw-time vertex attributes

Main kernel:

- `vs_main`

## `virtualgeometry-meshlet-fs.wgsl`

Primary role:

- write geometric identity and material lookup data into G-buffer-like targets

Main kernel:

- `fs_main`

## `streaming-page-selection-cs.wgsl`

Primary role:

- reduce many page priorities into a compact install/evict list

Main kernel:

- `reduceStreamingCandidates`

## Current Limitations And Notes

- The hardware draw stage produces geometry/material lookup buffers, not final lighting.
- Final shading currently depends on the separate material pass.
- Runtime page streaming is asynchronous and can lag a frame or more behind visibility demand.
- Install/uninstall rewrite tables are part of the file format and are required for correct hierarchy state transitions.
