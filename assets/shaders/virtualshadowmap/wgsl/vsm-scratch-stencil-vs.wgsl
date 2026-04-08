#include "vsm-common.wgsl"

struct StencilPageUniforms {
    pageTableResolution: u32,
    physicalPageSize: u32,
    currentLayer: u32,
    scratchResolution: u32,
    depthValue: f32,
    _padding0: vec3<f32>,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms: StencilPageUniforms;
@group(0) @binding(1) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(2) var<storage, read> dirtyPageList: array<u32>;

struct VSOut {
    @builtin(position) clipPos: vec4<f32>,
}

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

/// @brief Vertex shader that generates a scratch-space quad for each dirty page to write into the stencil buffer.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VSOut {
    var out: VSOut;
    out.clipPos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);

    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    let packedPage = dirtyPageList[uniforms.currentLayer * pagesPerLayer + instance_index];
    let wrappedPage = unpack_layered_coords(packedPage).xy;
    let virtualPage = wrapped_page_coords_to_virtual_coords(
        wrappedPage,
        cascadeStates[uniforms.currentLayer].pageOffset,
        i32(uniforms.pageTableResolution));

    let pageOrigin = vec2<f32>(virtualPage * uniforms.physicalPageSize);
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 0.0), vec2<f32>(0.0, 1.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(1.0, 0.0), vec2<f32>(1.0, 1.0));
    let scratchPos = pageOrigin + corners[vertex_index] * f32(uniforms.physicalPageSize);
    let scratchUv = scratchPos / f32(max(uniforms.scratchResolution, 1u));
    out.clipPos = vec4<f32>(scratchUv * 2.0 - vec2<f32>(1.0, 1.0), uniforms.depthValue, 1.0);
    return out;
}
