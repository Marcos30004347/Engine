#include "vsm-common.wgsl"

const COUNTER_CAPPED_REQUESTS: u32 = 0u;
const COUNTER_FALLBACK_REQUESTS: u32 = 1u;
const HPB_WORKGROUP_SIZE_X: u32 = 8u;
const HPB_WORKGROUP_SIZE_Y: u32 = 8u;
const HPB_WORKGROUP_SIZE: u32 = HPB_WORKGROUP_SIZE_X * HPB_WORKGROUP_SIZE_Y;
const HPB_MAX_PAGE_TABLE_RESOLUTION: u32 = 32u;
const HPB_MAX_TEXELS: u32 = HPB_MAX_PAGE_TABLE_RESOLUTION * HPB_MAX_PAGE_TABLE_RESOLUTION;
const HPB_MAX_MIP_LEVELS: u32 = 6u;

struct DrawUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    hpbMipCount: u32,
    requestCapacity: u32,
    futureRequestCapacity: u32,
    _padding0: vec3<u32>,
}

@group(0) @binding(0) var<uniform> uniforms: DrawUniforms;
@group(0) @binding(1) var<storage, read> allocatorCounters: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> allocationRequests: array<u32>;
@group(0) @binding(3) var<storage, read> futureAllocationRequests: array<u32>;
@group(0) @binding(4) var hpbMip0: texture_storage_2d_array<r32float, write>;
@group(0) @binding(5) var hpbMip1: texture_storage_2d_array<r32float, write>;
@group(0) @binding(6) var hpbMip2: texture_storage_2d_array<r32float, write>;
@group(0) @binding(7) var hpbMip3: texture_storage_2d_array<r32float, write>;
@group(0) @binding(8) var hpbMip4: texture_storage_2d_array<r32float, write>;
@group(0) @binding(9) var hpbMip5: texture_storage_2d_array<r32float, write>;

var<workgroup> hpbPing: array<u32, HPB_MAX_TEXELS>;
var<workgroup> hpbPong: array<u32, HPB_MAX_TEXELS>;

/// @brief Checks whether a virtual page is scheduled to be drawn this frame by scanning the allocation request lists.
/// @param layer The cascade layer index of the page.
/// @param page_coord The 2D page coordinate to look up.
/// @param request_count Number of regular allocation requests this frame.
/// @param fallback_request_count Number of fallback allocation requests this frame.
/// @returns 1 if the page appears in either request list, 0 otherwise.
fn page_is_scheduled_this_frame(layer: u32, page_coord: vec2<u32>, request_count: u32, fallback_request_count: u32) -> u32 {
    for (var request_index = 0u; request_index < request_count; request_index++) {
        let request = unpack_layered_coords(allocationRequests[request_index]);
        if (request.z == layer && all(request.xy == page_coord)) {
            return 1u;
        }
    }

    for (var request_index = 0u; request_index < fallback_request_count; request_index++) {
        let request = unpack_layered_coords(futureAllocationRequests[request_index]);
        if (request.z == layer && all(request.xy == page_coord)) {
            return 1u;
        }
    }

    return 0u;
}

/// @brief Writes a single value into a specified mip level of the hierarchical page bitmask (HPB) texture array.
/// @param level The HPB mip level to write to (0 = finest).
/// @param coord The 2D texel coordinate within the mip level.
/// @param layer The array layer (cascade index) to write to.
/// @param value The value to store (0 = no dirty pages, 1 = has dirty pages).
fn store_hpb_mip(level: u32, coord: vec2<i32>, layer: i32, value: u32) {
    let texel = vec4<f32>(f32(value), 0.0, 0.0, 1.0);
    switch (level) {
        case 0u: { textureStore(hpbMip0, coord, layer, texel); }
        case 1u: { textureStore(hpbMip1, coord, layer, texel); }
        case 2u: { textureStore(hpbMip2, coord, layer, texel); }
        case 3u: { textureStore(hpbMip3, coord, layer, texel); }
        case 4u: { textureStore(hpbMip4, coord, layer, texel); }
        case 5u: { textureStore(hpbMip5, coord, layer, texel); }
        default: {}
    }
}

/// @brief Reads from the appropriate ping-pong workgroup buffer for the given HPB mip level.
/// @param level The HPB mip level being processed.
/// @param index The flat index within the workgroup buffer.
/// @returns The stored value at that index.
fn read_hpb_shared(level: u32, index: u32) -> u32 {
    if ((level & 1u) == 0u) {
        return hpbPing[index];
    }
    return hpbPong[index];
}

/// @brief Writes to the appropriate ping-pong workgroup buffer for the given HPB mip level.
/// @param level The HPB mip level being processed.
/// @param index The flat index within the workgroup buffer.
/// @param value The value to write.
fn write_hpb_shared(level: u32, index: u32, value: u32) {
    if ((level & 1u) == 0u) {
        hpbPong[index] = value;
        return;
    }
    hpbPing[index] = value;
}

/// @brief Builds all mip levels of the hierarchical page bitmask (HPB) for all active cascade layers in a single pass.
@compute @workgroup_size(HPB_WORKGROUP_SIZE_X, HPB_WORKGROUP_SIZE_Y, 1)
fn build_hpb_all_main(
    @builtin(local_invocation_index) local_index: u32,
    @builtin(workgroup_id) workgroup_id: vec3<u32>
) {
    let layer = workgroup_id.z;
    if (layer >= uniforms.activeLayers ||
        uniforms.pageTableResolution > HPB_MAX_PAGE_TABLE_RESOLUTION ||
        uniforms.hpbMipCount > HPB_MAX_MIP_LEVELS) {
        return;
    }

    let base_resolution = uniforms.pageTableResolution;
    let base_texel_count = base_resolution * base_resolution;
    let request_count = min(atomicLoad(&allocatorCounters[COUNTER_CAPPED_REQUESTS]), uniforms.requestCapacity);
    let fallback_request_count = min(atomicLoad(&allocatorCounters[COUNTER_FALLBACK_REQUESTS]), uniforms.futureRequestCapacity);

    for (var flat_index = local_index; flat_index < base_texel_count; flat_index += HPB_WORKGROUP_SIZE) {
        let page_coord = vec2<u32>(flat_index % base_resolution, flat_index / base_resolution);
        let isScheduled = page_is_scheduled_this_frame(layer, page_coord, request_count, fallback_request_count);
        hpbPing[flat_index] = isScheduled;
        store_hpb_mip(0u, vec2<i32>(i32(page_coord.x), i32(page_coord.y)), i32(layer), isScheduled);
    }

    workgroupBarrier();

    var src_resolution = base_resolution;
    for (var mip_level = 1u; mip_level < uniforms.hpbMipCount; mip_level++) {
        let dst_resolution = max(1u, src_resolution >> 1u);
        let dst_texel_count = dst_resolution * dst_resolution;

        for (var flat_index = local_index; flat_index < dst_texel_count; flat_index += HPB_WORKGROUP_SIZE) {
            let dst_coord = vec2<u32>(flat_index % dst_resolution, flat_index / dst_resolution);
            let src_coord = dst_coord * 2u;
            var anyDirty = 0u;
            for (var oy = 0u; oy < 2u; oy++) {
                for (var ox = 0u; ox < 2u; ox++) {
                    let sample_coord = src_coord + vec2<u32>(ox, oy);
                    if (sample_coord.x >= src_resolution || sample_coord.y >= src_resolution) {
                        continue;
                    }
                    let sample_index = sample_coord.y * src_resolution + sample_coord.x;
                    anyDirty = max(anyDirty, read_hpb_shared(mip_level - 1u, sample_index));
                }
            }

            write_hpb_shared(mip_level - 1u, flat_index, anyDirty);
            store_hpb_mip(mip_level, vec2<i32>(i32(dst_coord.x), i32(dst_coord.y)), i32(layer), anyDirty);
        }

        workgroupBarrier();
        src_resolution = dst_resolution;
    }
}
