# Virtual Texture System

## Scope

This document describes the current virtual texture system used by the virtual geometry material pass.

It covers:

- `.vmat` material file creation and layout
- CPU-side virtual texture state construction
- physical atlas residency
- feedback-driven page uploads
- current limitations in the renderer

## High-Level Model

The virtual texture system is currently CPU-managed.

- Materials are registered as prepared texture sets.
- The runtime builds a global virtual page table for all registered material textures.
- A physical atlas stores only resident virtual pages.
- The GPU requests missing pages through a feedback buffer.
- The CPU ranks requests, uploads pages, and updates residency state for later frames.

This is not a fully GPU-managed sparse-texture implementation. The GPU identifies demand, but page selection and upload policy live on the CPU.

## Material File Format

Material files use the `VMAT` magic and current version `2`.

Each `.vmat` file stores:

1. header
2. texture count
3. per-texture source path
4. per-texture sampling parameters
5. mip count
6. per-mip width, height, texel count, and texels

Per-texture sampling state currently includes:

- address mode U
- address mode V
- filter mode
- mip bias
- min mip
- max mip

The file stores fully prepared mip texels, not only source image references. The source path is preserved as metadata, but the runtime can load the material from the file alone.

## Material Creation

Materials are built from `VirtualTextureSystem::MaterialCreateInfo`.

The preparation flow is:

1. load source images
2. optionally flip vertically
3. convert pixels to packed RGBA8 `uint32`
4. build the full mip chain on the CPU
5. clamp/normalize sampling settings to the available mip range
6. write the prepared result to `.vmat`

The virtual texture CLI tool introduced in `docs/cli-tools.md` writes these `.vmat` files from image sources such as PNG and JPEG.

## Runtime State Construction

When materials are registered, the system eventually rebuilds a flattened runtime view.

The rebuild step generates:

- `atlasInfo_`
- `textureEntries_`
- `materialEntries_`
- `pageTableEntries_`
- `pageTableOwners_`
- `physicalPagePixels_`
- `physicalPages_`
- `freePhysicalPages_`
- dirty/residency tracking buffers

### `TextureEntryGPU`

Each registered texture exposes:

- per-mip dimensions
- page-table offsets
- page counts in X and Y
- mip count
- page size
- address/filter settings
- mip clamp settings

### `MaterialEntryGPU`

Each material exposes:

- up to `MAX_TEXTURES_PER_MATERIAL` virtual texture indices
- a texture count

### `PageTableEntryGPU`

Each virtual texture page table entry exposes:

- physical page id
- residency flags

## Atlas And Page Layout

The runtime atlas is a uniform grid of fixed-size pages.

Key settings:

- page size: default `128`
- physical pages per axis: default `32`
- max uploads per frame: default `32`

The physical atlas resolution is:

- `pageSize * physicalPagesPerAxis`

The current implementation enforces a maximum virtual texture size of `16384 x 16384`.

## Fallback Residency

The system installs fallback pages after rebuilding state.

The intent is:

- every texture has at least a coarse mip available
- missing fine pages can still sample from an ancestor mip
- the material pass can render immediately instead of failing hard on the first frame

These fallback pages are pinned so they are not chosen as ordinary eviction candidates.

## Feedback-Driven Uploads

The material pass writes GPU requests into a feedback buffer. Each request identifies:

- texture index
- mip level
- page X
- page Y

The CPU then calls `processFeedbackRequests()`.

### Request Processing Steps

1. Deduplicate identical requests and accumulate hit counts.
2. Validate requests against the registered texture set.
3. Walk from the requested mip toward coarser mips to find the nearest resident ancestor.
4. Aggregate all missing pages that matter for the current frame.
5. Score missing pages.
6. Upload pages until the frame budget is exhausted.

### Ranking Heuristics

Current scoring considers:

- total request hits
- repeated demand across consecutive frames
- distance to the nearest resident ancestor
- whether the immediate parent is already resident
- requested detail level

This favors:

- pages that many pixels want
- pages that remain important over multiple frames
- pages that extend already-resident branches

## Upload And Eviction Policy

When a requested page must become resident:

1. Try the free physical page list first.
2. If no free page exists, choose an eviction candidate.
3. Copy the page texels into the selected physical page.
4. Mark the page table entry resident.
5. Mark the physical page dirty for GPU upload/state sync.

The eviction policy currently prefers the oldest non-pinned resident page.

Resident ancestors touched during request processing are refreshed in the LRU sense so useful coarse pages stay alive.

## GPU Interface Used By The Material Pass

The material pass consumes:

- atlas info buffer
- material entries buffer
- texture entries buffer
- virtual page table buffer
- physical page pixels buffer

The fragment shader uses those buffers to:

- map material id to texture indices
- map texture UVs to virtual pages
- resolve virtual pages to physical pages
- request missing pages
- sample the physical atlas

## Material And Texture Limits

The system can represent multiple texture slots per material, but the current material shader only shades using texture slot `0`.

That means:

- the virtual texture system itself already supports richer material layouts
- the current renderer uses the first texture primarily as base color

## Shader Relationship

The virtual texture system itself is mostly CPU-side. Its GPU-facing behavior is exercised through the material pass shaders documented in `docs/material-pass.md`.

From the virtual texture point of view, the most important shader operation is:

- `virtualgeometry-material-fs.wgsl::fs_main`

High-level overview:

- derive the requested mip from screen-space derivatives
- enqueue feedback if the ideal page is missing
- walk to a resident ancestor if needed
- sample the resolved physical atlas page

## Current Characteristics

- Mip generation is CPU-side.
- Residency management is CPU-side.
- Feedback generation is GPU-side.
- Fallback pages guarantee that some mip is usually sampleable.
- Uploads are explicitly budgeted per frame.
