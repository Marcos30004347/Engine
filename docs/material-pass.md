# Material Pass

## Scope

This document describes the current material pass used after virtual geometry has already produced:

- scene depth
- material ids
- material UVs
- a VSM shadow-light texture

This is the renderer's current shading path for virtual geometry.

## Role In The Frame

The material pass runs after:

1. virtual geometry culling
2. depth prepass
3. depth pyramid generation
4. final culling
5. hardware draw
6. virtual shadow map shadow-light resolve

The hardware draw stage does not fully shade geometry. It writes the data the material pass needs:

- `materialId`
- `materialUV`
- depth
- packed geometry ids for debugging and other consumers

The material pass then performs tile-based material classification and indirect shading draws.

## Main Inputs

Scene textures:

- scene depth texture
- material id texture
- material UV texture
- shadow-light texture

Virtual texture state:

- atlas info buffer
- material entries buffer
- texture entries buffer
- virtual page table buffer
- physical page buffer

Material-pass private resources:

- feedback buffer
- feedback readback buffer
- tile draw entries buffer
- tile draw counters buffer
- material draw indirect buffer
- material depth texture

## Frame Flow

The pass has two important CPU-side hooks plus one GPU-side render sequence.

## 1. `beginFrame()`

At frame start, the pass:

- syncs virtual texture state to GPU buffers
- clears the feedback header
- clears tile draw counters
- resets indirect draw arguments

This ensures the later compute and draw stages start from an empty worklist.

## 2. GPU Render Sequence

`recordCommandBuffer()` currently does the following:

1. Upload or refresh virtual texture state buffers.
2. Dispatch `prepareTileMaterials`.
3. Dispatch `prepareMaterialIndirectArgs`.
4. Render the material-depth pass.
5. Render the material-draw pass via indirect draw.
6. Copy the feedback buffer to a readback buffer.

## 3. `processFeedback()`

After GPU execution, the pass can:

- read back feedback requests
- pass them to `VirtualTextureSystem::processFeedbackRequests()`
- sync updated residency state if uploads occurred

That creates the feedback loop between rendered demand and later texture residency.

## Why The Pass Is Tile-Based

The current material path is designed to avoid doing a naive full-screen material resolve for every material.

Instead it:

1. partitions the screen into fixed-size tiles
2. determines which materials are present in each tile
3. emits one draw entry per `(tile, material)` pair
4. renders only those tile/material quads

This makes shading cost scale with the visible material distribution rather than blindly with every pixel/material combination.

## Stage Breakdown

## A. Tile Classification Compute

Purpose:

- scan depth + material ids per tile
- build a compact list of tile/material work items

Main shader module:

- `assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-tiles-cs.wgsl`

Main kernels used by the pass:

- `prepareTileMaterials`
- `prepareMaterialIndirectArgs`

Kernel overview:

- `prepareTileMaterials` scans the pixels in each screen tile, collects unique material ids seen in that tile, and appends `TileMaterialDrawEntry` records into a work buffer.
- `prepareMaterialIndirectArgs` converts the final draw-entry count into indirect draw arguments for the later graphics pass.

Outputs:

- tile/material draw entries
- draw counters
- indirect draw arguments

## B. Material Depth Pass

Purpose:

- generate a material-sorted depth mask so only the correct material draw wins at each pixel

Main shader modules:

- vertex: `assets/shaders/spirv/renderToQuadPass-vs.spirv`
- fragment: `assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-depth-fs.wgsl`

Main kernels used by the pass:

- `vs_main`
- `fs_main`

Kernel overview:

- `vs_main` is the generic fullscreen-quad vertex kernel.
- `fs_main` reads the scene `materialId` texture per pixel and writes a material-encoded depth value that later lets the indirect material draw pass use `DepthEqual` to shade only pixels owned by the selected material.

This step converts the scene's per-pixel material id into a depth test that the later indirect tile/material draws can reuse.

## C. Material Draw Pass

Purpose:

- shade the visible material pixels using the virtual texture system

Main shader modules:

- vertex: `assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-vs.wgsl`
- fragment: `assets/shaders/virtualgeometry/wgsl/virtualgeometry-material-fs.wgsl`

Main kernels used by the pass:

- `vs_main`
- `fs_main`

Kernel overview:

- `vs_main` again expands the `(tile, material)` entry into a tile quad.
- `fs_main` reconstructs the material UV, resolves the material's virtual texture pages, emits feedback for missing pages, and samples the physical atlas.

## Feedback Buffer Behavior

The fragment shader writes page requests into a feedback buffer with a bounded capacity.

The feedback header tracks:

- request count
- overflow count

If the shader wants a page that is not resident:

1. it computes the requested mip from derivatives
2. it queues a page request
3. it falls back to the nearest resident ancestor mip when possible

That means the frame can still render while also informing the CPU which pages should be uploaded next.

## Main Shader Behavior

## `virtualgeometry-material-tiles-cs.wgsl`

Primary role:

- tile classification and indirect draw setup

Main kernels:

- `prepareTileMaterials`
- `prepareMaterialIndirectArgs`

## `virtualgeometry-material-vs.wgsl`

Primary role:

- expand `(tile, material)` work entries into rasterizable quads

Main kernel:

- `vs_main`

## `virtualgeometry-material-depth-fs.wgsl`

Primary role:

- create the material ownership depth mask

Main kernel:

- `fs_main`

## `virtualgeometry-material-fs.wgsl`

Primary role:

- resolve virtual texture pages, emit feedback, and shade

Main kernel:

- `fs_main`

High-level steps inside `fs_main`:

1. Read the tile/material draw entry.
2. Recover the per-pixel UV from the `materialUV` texture.
3. Map the material id to texture indices through `materialEntries`.
4. Compute the requested mip from derivatives.
5. Emit a feedback request when the ideal page is missing.
6. Resolve to a resident page or ancestor page.
7. Sample the physical page texels from the atlas buffer.
8. Sample the shadow-light texture for the current pixel.
9. Output the shadowed color.

## Current Material Model

The current shader path is intentionally limited.

Today it effectively uses:

- texture slot `0` as the sampled material texture

What it does not yet provide in this pass:

- full multi-texture PBR evaluation
- normal-map / roughness / metallic composition
- lighting accumulation
- advanced material graph logic

In other words, this is currently a virtual-texture-backed material resolve pass, not a complete physically based shading pipeline.

## Current Characteristics

- shading work is bin-packed by tile and material
- missing virtual texture pages generate feedback instead of hard failure
- a private material-depth texture determines which tile/material quad owns each pixel
- residency changes are applied on later frames after CPU feedback processing
