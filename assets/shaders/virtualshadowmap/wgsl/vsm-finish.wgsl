#include "vsm-common.wgsl"

const WORKGROUP_SIZE: u32 = 64u;
const COUNTER_SHADOW_DRAW_OVERFLOW: u32 = 4u;

struct FinishUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    currentLayer: u32,
    _padding0: u32,
}

@group(0) @binding(0) var<uniform> uniforms: FinishUniforms;
@group(0) @binding(1) var<storage, read> drawCounters: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> virtualPageTable: array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> dirtyPageCounts: array<u32>;
@group(0) @binding(4) var<storage, read> dirtyPageList: array<u32>;

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param pageCoord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vsm_vpt_index(layer: u32, pageCoord: vec2<u32>) -> u32 {
    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pagesPerLayer + pageCoord.y * uniforms.pageTableResolution + pageCoord.x;
}

/// @brief Clears the dirty bit on an allocated virtual page, marking it as fully rendered.
/// @param page_index The flat index of the virtual page to clear.
fn clear_page_dirty(page_index: u32) {
    let page_entry = atomicLoad(&virtualPageTable[page_index]);
    if (!get_is_allocated(page_entry) || !get_is_dirty(page_entry)) {
        return;
    }

    atomicStore(&virtualPageTable[page_index], set_dirty(page_entry, false));
}

/// @brief Clears the dirty flag on all pages that were successfully drawn this frame.
@compute @workgroup_size(WORKGROUP_SIZE)
fn finish_drawn_pages_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (atomicLoad(&drawCounters[COUNTER_SHADOW_DRAW_OVERFLOW]) != 0u) {
        return;
    }

    let dirty_page_count = dirtyPageCounts[uniforms.currentLayer];
    if (global_id.x < dirty_page_count) {
        let pages_per_layer = uniforms.pageTableResolution * uniforms.pageTableResolution;
        let packed_page = dirtyPageList[uniforms.currentLayer * pages_per_layer + global_id.x];
        let request = unpack_layered_coords(packed_page);
        clear_page_dirty(vsm_vpt_index(uniforms.currentLayer, request.xy));
    }
}
