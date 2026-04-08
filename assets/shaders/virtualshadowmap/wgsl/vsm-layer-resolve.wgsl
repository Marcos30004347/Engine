#include "vsm-common.wgsl"

const PAGE_TILE_SIZE: u32 = 4u;
const TILE_WORKGROUP_SIZE_X: u32 = 8u;
const TILE_WORKGROUP_SIZE_Y: u32 = 8u;

struct ResolveUniforms {
    pageTableResolution: u32,
    physicalPageSize: u32,
    currentLayer: u32,
    activeLayers: u32,
    enableDirtyPageStencil: u32,
    _padding0: vec3<u32>,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms: ResolveUniforms;
@group(0) @binding(1) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(2) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(3) var<storage, read> dirtyPageCounts: array<u32>;
@group(0) @binding(4) var<storage, read> dirtyPageList: array<u32>;
@group(0) @binding(5) var<storage, read_write> resolveDispatchArgs: array<u32>;
@group(0) @binding(6) var<storage, read_write> pageStencilDrawIndirectArgs: array<u32>;
@group(0) @binding(7) var<storage, read_write> pageClearDrawIndirectArgs: array<u32>;
@group(0) @binding(8) var scratchTexture: texture_depth_2d;
@group(0) @binding(9) var atlasTexture: texture_storage_2d<r32float, write>;
@group(0) @binding(10) var<storage, read_write> pageOpDispatchArgs: array<u32>;

/// @brief Converts wrapped (toroidal) page coordinates back to linear virtual page coordinates.
/// @param wrapped_page The wrapped 2D page coordinate within the physical page table.
/// @param cascade_offset The toroidal offset of the cascade origin in page space.
/// @param page_table_resolution The resolution (in pages) of the page table.
/// @returns The linear virtual page coordinates.
fn wrapped_page_coords_to_virtual_coords(wrapped_page: vec2<u32>, cascade_offset: vec2<i32>, page_table_resolution: i32) -> vec2<u32> {
    let virtual_x = ((i32(wrapped_page.x) - cascade_offset.x) % page_table_resolution + page_table_resolution) % page_table_resolution;
    let virtual_y = ((i32(wrapped_page.y) - cascade_offset.y) % page_table_resolution + page_table_resolution) % page_table_resolution;
    return vec2<u32>(u32(virtual_x), u32(virtual_y));
}

/// @brief Computes the number of compute tiles needed to cover one axis of a physical page.
/// @param physical_page_size The size of a physical page in texels.
/// @returns The number of PAGE_TILE_SIZE tiles required to span the page.
fn tiles_per_axis(physical_page_size: u32) -> u32 {
    return (physical_page_size + PAGE_TILE_SIZE - 1u) / PAGE_TILE_SIZE;
}

/// @brief Fills the indirect dispatch and draw argument buffers for each active layer based on the dirty page count.
@compute @workgroup_size(64, 1, 1)
fn prepare_dispatch(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let layer = global_id.x;
    if (layer >= uniforms.activeLayers) {
        return;
    }

    let dirtyPageCount = dirtyPageCounts[layer];
    let dispatchIndex = layer * 3u;
    resolveDispatchArgs[dispatchIndex + 0u] = (dirtyPageCount + 63u) / 64u;
    resolveDispatchArgs[dispatchIndex + 1u] = 1u;
    resolveDispatchArgs[dispatchIndex + 2u] = 1u;

    let tilesPerPageAxis = tiles_per_axis(uniforms.physicalPageSize);
    pageOpDispatchArgs[dispatchIndex + 0u] = (tilesPerPageAxis + TILE_WORKGROUP_SIZE_X - 1u) / TILE_WORKGROUP_SIZE_X;
    pageOpDispatchArgs[dispatchIndex + 1u] = (tilesPerPageAxis + TILE_WORKGROUP_SIZE_Y - 1u) / TILE_WORKGROUP_SIZE_Y;
    pageOpDispatchArgs[dispatchIndex + 2u] = dirtyPageCount;

    let drawIndex = layer * 4u;
    pageClearDrawIndirectArgs[drawIndex + 0u] = 6u;
    pageClearDrawIndirectArgs[drawIndex + 1u] = dirtyPageCount;
    pageClearDrawIndirectArgs[drawIndex + 2u] = 0u;
    pageClearDrawIndirectArgs[drawIndex + 3u] = 0u;

    pageStencilDrawIndirectArgs[drawIndex + 0u] = 6u;
    pageStencilDrawIndirectArgs[drawIndex + 1u] = select(0u, dirtyPageCount, uniforms.enableDirtyPageStencil != 0u);
    pageStencilDrawIndirectArgs[drawIndex + 2u] = 0u;
    pageStencilDrawIndirectArgs[drawIndex + 3u] = 0u;
}

/// @brief Copies rendered shadow depth tiles from the scratch texture into the shadow atlas at the correct physical page locations.
@compute @workgroup_size(TILE_WORKGROUP_SIZE_X, TILE_WORKGROUP_SIZE_Y, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dirtyPageCount = dirtyPageCounts[uniforms.currentLayer];
    let tilesPerPageAxis = tiles_per_axis(uniforms.physicalPageSize);
    let pageIndex = global_id.z;
    if (pageIndex >= dirtyPageCount || global_id.x >= tilesPerPageAxis || global_id.y >= tilesPerPageAxis) {
        return;
    }

    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    let packed_page = dirtyPageList[uniforms.currentLayer * pagesPerLayer + pageIndex];
    let wrapped_page = unpack_layered_coords(packed_page).xy;
    let vpt_entry = virtualPageTable[vsm_page_table_index(uniforms.currentLayer, wrapped_page, uniforms.pageTableResolution)];
    if (!get_is_allocated(vpt_entry)) {
        return;
    }

    let virtual_page = wrapped_page_coords_to_virtual_coords(
        wrapped_page,
        cascadeStates[uniforms.currentLayer].pageOffset,
        i32(uniforms.pageTableResolution));
    let src_origin = vec2<i32>(virtual_page * uniforms.physicalPageSize);
    let dst_origin = vec2<i32>(vec2<u32>(get_page_x(vpt_entry), get_page_y(vpt_entry)) * uniforms.physicalPageSize);
    let tileBase = global_id.xy * PAGE_TILE_SIZE;
    for (var y = 0u; y < PAGE_TILE_SIZE; y++) {
        for (var x = 0u; x < PAGE_TILE_SIZE; x++) {
            let localCoord = tileBase + vec2<u32>(x, y);
            if (localCoord.x >= uniforms.physicalPageSize || localCoord.y >= uniforms.physicalPageSize) {
                continue;
            }

            let src_texel = src_origin + vec2<i32>(localCoord);
            let dst_texel = dst_origin + vec2<i32>(localCoord);
            let depth = textureLoad(scratchTexture, src_texel, 0);
            textureStore(atlasTexture, dst_texel, vec4<f32>(depth, 0.0, 0.0, 1.0));
        }
    }
}
