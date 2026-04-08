# Virtual Shadow System

## Scope

This document describes the current virtual shadow system implemented under `src/runtime/virtualshadowmap`.

It covers:

- the manager and data model
- page tables and invalidation
- pass order
- the main shader kernels used by each pass

This system is a virtual shadow map pipeline. In this document, `VSM` means virtual shadow map, not variance shadow map.

## High-Level Model

The shadow system virtualizes shadow map storage in the same spirit that the virtual texture system virtualizes texture storage.

- A virtual page table defines shadow pages in cascade space.
- A physical atlas stores only the currently allocated shadow pages.
- Cascades can scroll over the world while keeping wrapped page coordinates stable.
- Dirty and invalid pages are selectively redrawn.

The runtime is centered around `VirtualShadowMapManager`.

## Manager Responsibilities

`VirtualShadowMapManager` owns the persistent shadow state for the frame graph.

Core responsibilities:

- virtual page table allocation
- physical page table allocation
- per-page state tracking
- invalidation masks
- cascade state and matrices
- camera state
- shadow atlas texture
- hierarchical page bounds texture

Important settings include:

- virtual page table resolution
- physical atlas resolution
- physical page size
- cascade count
- allocation request capacities
- fallback cascade offset
- world extent for the first cascade
- cascade scaling factor
- page world scale
- directional light distance
- reverse-Z mode

## Core Data Structures

The manager maintains:

- a virtual page table buffer
- a physical page table buffer
- per-page state buffers
- invalidation mask buffers
- cascade-state and cascade-matrix buffers
- camera-state buffers
- allocator/request buffers
- a shadow atlas texture
- a hierarchical page bounds texture

Conceptually:

- the virtual page table says which shadow page is needed in cascade space
- the physical page table says where that page lives in the atlas
- the page-state and invalidation masks say whether it must be refreshed

## Cascade Model

Each directional light owns a number of cascades.

For each cascade, the manager computes:

- page offset
- page shift
- quantized center
- world extent
- page world size
- view / projection / view-projection matrices

This allows the shadow pages to behave like wrapped clipmaps:

- the virtual page grid follows the camera
- only newly exposed or invalidated pages need redrawing
- old physical pages can be reused

## Pass Graph

`VirtualShadowMapPass` currently registers passes in this order:

1. bookkeeping
2. draw
3. optional table debug
4. shadow-light resolve with PCF + contact shadows

## 1. Bookkeeping Pass

Purpose:

- initialize allocator state
- update visibility and dirty state in the virtual page table
- analyze the main scene depth buffer
- emit allocation requests
- assign physical pages

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-bookkeeping.wgsl`

Main kernels used by the pass:

- `init_allocator_main`
- `bookkeeping_main`
- `analyze_visible_pages_main`
- `emit_page_requests_main`
- `allocate_pages_main`

Kernel overview:

- `init_allocator_main` seeds the free physical page queue.
- `bookkeeping_main` updates virtual/physical page state and applies invalidation rules.
- `analyze_visible_pages_main` reconstructs world positions from the main depth texture and determines which virtual shadow pages are visible for the current camera.
- `emit_page_requests_main` queues regular and fallback page requests for dirty or missing shadow pages.
- `allocate_pages_main` consumes those requests and maps them onto physical atlas pages.

This pass is where visibility demand from the main camera turns into actual shadow page work.

## 2. Draw Pass

The draw pass is internally split into several sub-stages.

### 2.1 Hierarchical Page Bounds Build

Purpose:

- mark which shadow pages are scheduled for work this frame
- generate a coarse hierarchy over those page requests

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-draw.wgsl`

Main kernel used by the pass:

- `build_hpb_all_main`

Kernel overview:

- Reads current and fallback page requests.
- Builds the hierarchical page bounds texture mip chain.
- Lets later culling cheaply reject scene regions that do not overlap any dirty or requested shadow page.

### 2.2 Shadow Culling

Purpose:

- traverse the virtual geometry hierarchy per shadow layer
- determine which clusters affect dirty shadow pages
- build per-layer draw lists and counters

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-culling-multipass.wgsl`

Main kernels used by the pass:

- `init_sync`
- `setup_root_nodes`
- `prepare_indirect_dispatch`
- `process_hierarchy_nodes`
- `prepare_cluster_dispatch`
- `process_clusters`

Kernel overview:

- `process_hierarchy_nodes` traverses the virtual geometry hierarchy in light space and tests whether a node projects into any dirty wrapped page range.
- `process_clusters` resolves cluster-level page coverage and emits visible shadow draw records only for pages that need work.

This stage is the shadow analogue of the main virtual-geometry culling pass, but it culls against shadow-page demand instead of camera visibility.

### 2.3 Scratch Clear

Purpose:

- clear only the scratch clipmap pages that will be redrawn

Main shader module:

- vertex: `assets/shaders/virtualshadowmap/wgsl/vsm-scratch-stencil-vs.wgsl`

Main kernel used by the pass:

- `vs_main`

Kernel overview:

- Uses a depth-only draw over dirty page rectangles so only those scratch pages are reset.

### 2.4 Page Stencil Mark

Purpose:

- mark dirty wrapped pages in stencil so later raster work only touches those page rectangles

Main shader modules:

- vertex: `assets/shaders/virtualshadowmap/wgsl/vsm-scratch-stencil-vs.wgsl`

Main kernels used by the pass:

- `vs_main`

Kernel overview:

- The vertex stage generates wrapped page rectangles.
- Fixed-function stencil writes coverage for those page tiles.

### 2.5 Scratch Shadow Draw

Purpose:

- rasterize only the clusters that affect dirty shadow pages
- write shadow depth into the scratch clipmaps

Main shader modules:

- vertex: `assets/shaders/virtualshadowmap/wgsl/vsm-meshlet-vs.wgsl`

Main kernels used by the pass:

- `vs_main`

Kernel overview:

- `vs_main` decodes meshlet geometry from virtual geometry pages, projects it into the selected shadow cascade layer, and writes clip-space depth directly.

The draw can optionally be stencil-restricted to dirty pages, and native depth compare is selected to match normal or reverse-Z operation.

### 2.6 Resolve To Physical Atlas

Purpose:

- copy freshly drawn scratch pages into their physical atlas locations

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-layer-resolve.wgsl`

Main kernels used by the pass:

- `prepare_dispatch`
- `main`

Kernel overview:

- `prepare_dispatch` compacts dirty-page counts into dispatch arguments.
- `main` copies page-sized tiles from scratch clipmaps into the final physical shadow atlas.

### 2.7 Finish Pass

Purpose:

- clear the dirty state for pages that were successfully drawn and resolved

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-finish.wgsl`

Main kernel used by the pass:

- `finish_drawn_pages_main`

Kernel overview:

- Marks completed pages clean unless the frame overflowed and the page must remain pending.

## 3. Optional Table Debug Pass

Purpose:

- visualize page-table state for debugging

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-debug.wgsl`

Main kernels:

- `pages_debug_main`
- `table_debug_main`

This pass is optional and diagnostic only.

## 4. Shadow-Light Resolve Pass

Purpose:

- evaluate per-light shadow visibility for the main camera using the virtual shadow atlas
- accumulate a screen-space shadow-light texture that the material pass can sample directly
- add short-range contact shadows from the current frame depth buffer

Main shader module:

- `assets/shaders/virtualshadowmap/wgsl/vsm-shadow-mask-pcf.wgsl`

Main kernel used by the pass:

- `main`

Outputs:

- RGB stores the blended shadow-light color used by the material pass
- A stores the average visibility across contributing lights

Kernel overview:

- Reconstructs the receiver from the main depth texture.
- Selects the cascade.
- Resolves the corresponding virtual shadow page into the physical atlas.
- Computes hard visibility plus filtered PCF visibility.
- Writes a shadow mask texture for later shading.

## Shader Summary

## `vsm-bookkeeping.wgsl`

Primary role:

- page allocation and visibility-driven request generation

Main kernels:

- `bookkeeping_main`
- `analyze_visible_pages_main`
- `emit_page_requests_main`
- `allocate_pages_main`

## `vsm-draw.wgsl`

Primary role:

- build hierarchical page bounds for dirty/requested shadow pages

Main kernel:

- `build_hpb_all_main`

## `vsm-culling-multipass.wgsl`

Primary role:

- light-space hierarchy traversal and per-page shadow draw emission

Main kernels:

- `process_hierarchy_nodes`
- `process_clusters`

## `vsm-meshlet-vs.wgsl`

Primary role:

- decode resident meshlet geometry and write shadow depth

Main kernels:

- `vs_main`
- `fs_main`

## `vsm-layer-resolve.wgsl`

Primary role:

- copy scratch page tiles into the physical atlas

Main kernels:

- `prepare_dispatch`
- `main`

## `vsm-shadow-mask-pcf.wgsl`

Primary role:

- sample the virtual shadow atlas and produce the screen-space shadow mask

Main kernel:

- `main`

## Current Characteristics

- The system is selective: it redraws dirty pages rather than whole cascades.
- The main scene depth buffer drives page visibility analysis.
- Cascades behave like wrapped clipmaps and can scroll with camera motion.
- The hierarchical page bounds texture is the key bridge between requested pages and efficient light-space culling.
