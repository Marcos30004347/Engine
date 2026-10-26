#include "vsm-common.wgsl"

struct DebugUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    cascadeCount: u32,
    enabled: u32,
    debugLayer: i32,
    _padding0: vec3<u32>,
    firstCascadeWorldExtent: f32,
    _padding1: vec3<f32>,
    physicalAtlasResolution: u32,
    physicalPageSize: u32,
    physicalPageTableResolution: u32,
    atlasDebugMode: u32,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

struct CascadeMatrix {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    worldExtent: f32,
    pageWorldSize: f32,
    lightIndex: u32,
    cascadeIndex: u32,
    pageTableResolution: u32,
    lightType: u32,
    faceIndex: u32,
    mipLevel: u32,
}

struct CameraState {
    inverseView: mat4x4<f32>,
    inverseProjection: mat4x4<f32>,
    inverseViewProjection: mat4x4<f32>,
    cameraPosition: vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: DebugUniforms;
@group(0) @binding(1) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(2) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(3) var<storage, read> cameraState: CameraState;
@group(0) @binding(4) var depthTexture: texture_depth_2d;
@group(0) @binding(5) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(6) var<storage, read> virtualPageState: array<u32>;
@group(0) @binding(7) var outputTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(8) var shadowAtlasTexture: texture_depth_2d;
@group(0) @binding(9) var<storage, read> physicalPageTable: array<u32>;

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param page_coord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vpt_index(layer: u32, page_coord: vec2<u32>) -> u32 {
    let pages_per_layer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pages_per_layer + page_coord.y * uniforms.pageTableResolution + page_coord.x;
}

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

/// @brief Computes a pseudo-random float in [0, 1) from a float seed.
/// @param value The input seed value.
/// @returns A pseudo-random float in [0, 1).
fn hash11(value: f32) -> f32 {
    return fract(sin(value * 91.3458 + 17.123) * 41358.5453123);
}

/// @brief Generates three independent pseudo-random floats in [0, 1) from an unsigned seed.
/// @param value The input unsigned seed value.
/// @returns A vec3 of pseudo-random floats in [0, 1).
fn hash31(value: u32) -> vec3<f32> {
    let x = hash11(f32(value) + 0.13);
    let y = hash11(f32(value) + 3.71);
    let z = hash11(f32(value) + 9.19);
    return vec3<f32>(x, y, z);
}

/// @brief Returns a fixed per-cascade debug color for cascade-level visualization.
/// @param layer The cascade layer index (cycles through 4 colors).
/// @returns An RGB color representing the cascade level.
fn cascade_color(layer: u32) -> vec3<f32> {
    switch (layer & 3u) {
        case 0u: { return vec3<f32>(0.16, 0.78, 0.96); }
        case 1u: { return vec3<f32>(0.94, 0.42, 0.19); }
        case 2u: { return vec3<f32>(0.46, 0.88, 0.30); }
        default: { return vec3<f32>(0.94, 0.82, 0.22); }
    }
}

/// @brief Reconstructs the world-space position of a pixel from the depth buffer using the inverse view-projection.
/// @param pixel_coord The integer pixel coordinate in the depth texture.
/// @returns The reconstructed world-space position.
fn reconstruct_world_position(pixel_coord: vec2<u32>) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

/// @brief Selects the preferred cascade level for a receiver distance using the rotationally invariant rule.
/// @param camera_distance The Euclidean distance from the camera to the receiver in camera space.
/// @returns The preferred clipmap cascade level index.
fn select_cascade_level(camera_distance: f32) -> u32 {
    return vsm_select_rotationally_invariant_clipmap_level(
        camera_distance,
        uniforms.firstCascadeWorldExtent,
        uniforms.cascadeCount);
}

/// @brief Tests whether a world-space position falls within the frustum of a given cascade layer.
/// @param world_position The world-space position to test.
/// @param layer The cascade layer index to test against.
/// @returns True if the position is covered by the cascade.
fn cascade_contains_world_position(world_position: vec3<f32>, layer: u32) -> bool {
    return vsm_cascade_contains_world_position(world_position, cascadeMatrices[layer].viewProj);
}

/// @brief Finds the finest active cascade level that covers a world-space position, starting from the rotationally invariant preferred level.
/// @param world_position The world-space position to look up.
/// @param preferred_layer The initial cascade level to start the search from.
/// @returns The index of the first covering cascade layer, or -1 if none covers the position.
fn find_covering_cascade_level(world_position: vec3<f32>, preferred_layer: u32) -> i32 {
    let max_layers = min(uniforms.cascadeCount, uniforms.activeLayers);
    var layer = preferred_layer;
    loop {
        if (layer >= max_layers) {
            break;
        }

        if (cascade_contains_world_position(world_position, layer)) {
            return i32(layer);
        }

        layer = layer + 1u;
    }

    return -1;
}

/// @brief Computes a pseudo-random unsigned 32-bit integer from an unsigned seed.
/// @param value The input seed value.
/// @returns A pseudo-random u32.
fn hash_u32(value: u32) -> u32 {
    var x = value;
    x = (x ^ 61u) ^ (x >> 16u);
    x = x * 9u;
    x = x ^ (x >> 4u);
    x = x * 0x27d4eb2du;
    x = x ^ (x >> 15u);
    return x;
}

/// @brief Generates a visually distinct color for a virtual page based on its signed ID for debug visualization.
/// @param page_id The signed (x, y, layer) virtual page identifier.
/// @returns A stable pseudo-random RGB color for the given page.
fn page_id_color(page_id: vec3<i32>) -> vec3<f32> {
    let seed =
        hash_u32(bitcast<u32>(page_id.x) * 73856093u) ^
        hash_u32(bitcast<u32>(page_id.y) * 19349663u) ^
        hash_u32(bitcast<u32>(page_id.z) * 83492791u);
    let color = vec3<f32>(
        f32(hash_u32(seed ^ 0x68bc21ebu) & 0x00ffffffu) / 16777215.0,
        f32(hash_u32(seed ^ 0x02e5be93u) & 0x00ffffffu) / 16777215.0,
        f32(hash_u32(seed ^ 0x967a889bu) & 0x00ffffffu) / 16777215.0);
    return mix(vec3<f32>(0.20, 0.20, 0.22), color, 0.8);
}

/// @brief Computes an edge highlight factor for a page based on the fractional UV position within it.
/// @param page_uv The fractional UV position within a page in [0, 1].
/// @returns A value near 1 at page edges and 0 in the interior.
fn page_edge_factor(page_uv: vec2<f32>) -> f32 {
    let edge_distance = min(min(page_uv.x, 1.0 - page_uv.x), min(page_uv.y, 1.0 - page_uv.y));
    return 1.0 - smoothstep(0.02, 0.08, edge_distance);
}

/// @brief Projects a world-space position into the wrapped page table for the given cascade layer.
/// @param world_position The world-space position to project.
/// @param layer The cascade layer index to project into.
/// @param wrapped_page_out Output: the resulting wrapped page coordinates.
/// @param page_local_uv_out Output: the fractional UV within the resolved page.
/// @returns True if the projection succeeded and the position is covered by the cascade.
fn try_resolve_wrapped_page_sample(world_position: vec3<f32>, layer: u32, wrapped_page_out: ptr<function, vec2<u32>>, page_local_uv_out: ptr<function, vec2<f32>>) -> bool {
    var ndc = vec3<f32>(0.0, 0.0, 0.0);
    return vsm_try_project_world_to_wrapped_page_sample(
        world_position,
        cascadeMatrices[layer].viewProj,
        cascadeStates[layer].pageOffset,
        layer_page_table_resolution(layer),
        wrapped_page_out,
        page_local_uv_out,
        &ndc);
}

/// @brief Produces a debug color for a virtual page table entry based on its allocation and visibility state.
/// @param entry The packed virtual page table entry.
/// @param page_state The packed page state word.
/// @param layer The cascade layer index used for cascade color coding.
/// @param local_page_uv The fractional UV within the page for edge highlighting.
/// @param intensity_seed A seed value used to vary the brightness per page.
/// @returns An RGBA debug color for the page.
fn shade_table_vpt_entry(entry: u32, page_state: u32, layer: u32, local_page_uv: vec2<f32>, intensity_seed: u32) -> vec4<f32> {
    let base_color = cascade_color(layer);
    let random_intensity = 0.45 + hash11(f32(intensity_seed)) * 0.35;
    var brightness = 0.025;
    if (get_is_allocated(entry)) {
        brightness = 0.42 + 0.18 * random_intensity;
    }
    if (get_page_state_is_visible(page_state)) {
        brightness = 0.82 + 0.14 * random_intensity;
    }

    var color = base_color * brightness + vec3<f32>(0.01, 0.01, 0.012);
    color = mix(color, vec3<f32>(1.0, 0.98, 0.92), page_edge_factor(local_page_uv) * 0.65);

    if (get_is_dirty(entry)) {
        color = mix(color, vec3<f32>(1.0, 0.42, 0.18), 0.55);
    }

    return vec4<f32>(color, 1.0);
}

fn grid_axis_count(item_count: u32, aspect: f32) -> u32 {
    if (item_count <= 1u) {
        return 1u;
    }
    return max(1u, u32(ceil(sqrt(f32(item_count) * max(aspect, 1e-4)))));
}

fn debug_layer_count() -> u32 {
    return select(uniforms.activeLayers, 0u, uniforms.enabled == 0u);
}

fn shade_table_layer_sample(layer: u32, local_uv: vec2<f32>) -> vec4<f32> {
    if (layer >= uniforms.activeLayers) {
        return vec4<f32>(0.01, 0.01, 0.012, 1.0);
    }

    let page_table_resolution = layer_page_table_resolution(layer);
    let page_space = clamp(local_uv, vec2<f32>(0.0), vec2<f32>(0.999999)) * f32(page_table_resolution);
    let page_coords = vec2<u32>(u32(floor(page_space.x)), u32(floor(page_space.y)));
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(page_coords),
        cascadeStates[layer].pageOffset,
        i32(page_table_resolution));
    if (wrapped_page_coords.x < 0 || wrapped_page_coords.y < 0) {
        return vec4<f32>(0.01, 0.01, 0.012, 1.0);
    }

    let wrapped_page_coords_u32 = vec2<u32>(wrapped_page_coords);
    let entry = virtualPageTable[vpt_index(layer, wrapped_page_coords_u32)];
    let page_state = virtualPageState[vpt_index(layer, wrapped_page_coords_u32)];
    let intensity_seed = entry ^ pack_layered_coords(page_coords.x, page_coords.y, layer);
    return shade_table_vpt_entry(entry, page_state, layer, fract(page_space), intensity_seed);
}

fn shade_atlas_depth(depth: f32, allocated: bool) -> vec3<f32> {
    let valid = select(0.0, 1.0, allocated && depth > 0.0);
    let visible_depth = pow(clamp(depth, 0.0, 1.0), 0.25);
    return mix(vec3<f32>(0.01, 0.01, 0.012), vec3<f32>(visible_depth), valid);
}

/// @brief Renders a per-pixel debug overlay coloring each screen pixel by the virtual shadow page it maps to.
@compute @workgroup_size(8, 8, 1)
fn pages_debug_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    var out_color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    if (uniforms.enabled == 0u || uniforms.activeLayers == 0u) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let depth = textureLoad(depthTexture, vec2<i32>(global_id.xy), 0);
    if (depth <= 0.0) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let uv = vsm_texel_center_uv(global_id.xy, dims);
    let camera_distance = length(vsm_reconstruct_view_position_from_uv_depth(uv, depth, cameraState.inverseProjection));
    let world_position = vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
    let preferred_layer = select_cascade_level(camera_distance);
    let cascade_level = find_covering_cascade_level(world_position, preferred_layer);
    if (cascade_level < 0) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }
    let layer = u32(cascade_level);

    var wrapped_page = vec2<u32>(0u, 0u);
    var page_local_uv = vec2<f32>(0.0, 0.0);
    if (!try_resolve_wrapped_page_sample(world_position, layer, &wrapped_page, &page_local_uv)) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let entry = virtualPageTable[vpt_index(layer, wrapped_page)];
    let page_state = virtualPageState[vpt_index(layer, wrapped_page)];
    if (!get_is_allocated(entry)) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), vec4<f32>(0.02, 0.02, 0.025, 1.0));
        return;
    }

    let page_id = vec3<i32>(i32(wrapped_page.x), i32(wrapped_page.y), i32(layer));
    var color = page_id_color(page_id);
    if (get_is_dirty(entry)) {
        color = mix(color, vec3<f32>(1.0, 0.42, 0.18), 0.6);
    } else if (get_page_state_is_visible(page_state)) {
        color = mix(color, vec3<f32>(1.0, 1.0, 1.0), 0.2);
    }
    out_color = vec4<f32>(color, 1.0);
    textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
}

/// @brief Renders a minimap-style debug overlay visualizing the virtual page table layout for one or all cascade layers.
@compute @workgroup_size(8, 8, 1)
fn table_debug_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    var out_color = vec4<f32>(0.01, 0.01, 0.012, 1.0);
    let layer_count = debug_layer_count();
    if (layer_count == 0u) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    if (uniforms.debugLayer >= 0) {
        let layer = u32(uniforms.debugLayer);
        if (layer >= layer_count) {
            textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
            return;
        }

        let local_uv = clamp(
            (vec2<f32>(global_id.xy) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims),
            vec2<f32>(0.0, 0.0),
            vec2<f32>(0.999999, 0.999999));
        out_color = shade_table_layer_sample(layer, local_uv);
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let uv = (vec2<f32>(global_id.xy) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims);
    let columns = grid_axis_count(layer_count, f32(dims.x) / f32(max(dims.y, 1u)));
    let rows = max(1u, (layer_count + columns - 1u) / columns);
    let tile = vec2<f32>(1.0 / f32(columns), 1.0 / f32(rows));
    let tile_coords = vec2<u32>(
        min(u32(floor(uv.x * f32(columns))), columns - 1u),
        min(u32(floor(uv.y * f32(rows))), rows - 1u));
    let layer = tile_coords.y * columns + tile_coords.x;
    if (layer < layer_count) {
        let tile_min = vec2<f32>(tile_coords) * tile;
        let local_uv = clamp((uv - tile_min) / tile, vec2<f32>(0.0), vec2<f32>(0.999999));
        out_color = shade_table_layer_sample(layer, local_uv);
        let tile_edge = min(min(local_uv.x, 1.0 - local_uv.x), min(local_uv.y, 1.0 - local_uv.y));
        let outlined_rgb = mix(vec3<f32>(0.88, 0.90, 0.92), out_color.rgb, smoothstep(0.0, 0.012, tile_edge));
        out_color = vec4<f32>(outlined_rgb, out_color.a);
    }

    textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
}

/// @brief Renders the physical VSM atlas depth. debugLayer selects one layer/face; -1 shows all allocated pages.
@compute @workgroup_size(8, 8, 1)
fn atlas_debug_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    var out_color = vec4<f32>(0.01, 0.01, 0.012, 1.0);
    if (uniforms.enabled == 0u || uniforms.physicalAtlasResolution == 0u || uniforms.physicalPageSize == 0u || uniforms.physicalPageTableResolution == 0u) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let uv = (vec2<f32>(global_id.xy) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims);
    let atlas_pixel = clamp(
        vec2<u32>(floor(uv * f32(uniforms.physicalAtlasResolution))),
        vec2<u32>(0u),
        vec2<u32>(uniforms.physicalAtlasResolution - 1u));
    let physical_page = atlas_pixel / uniforms.physicalPageSize;
    if (physical_page.x >= uniforms.physicalPageTableResolution || physical_page.y >= uniforms.physicalPageTableResolution) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let ppt_index = physical_page.y * uniforms.physicalPageTableResolution + physical_page.x;
    let ppt_entry = physicalPageTable[ppt_index];
    let allocated = get_ppt_is_allocated(ppt_entry);
    let layer = get_ppt_layer(ppt_entry);
    let layer_selected = uniforms.debugLayer < 0 || layer == u32(uniforms.debugLayer);
    if (!allocated || !layer_selected || layer >= uniforms.activeLayers) {
        textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
        return;
    }

    let depth = textureLoad(shadowAtlasTexture, vec2<i32>(atlas_pixel), 0);
    let page_local = vec2<f32>(atlas_pixel % vec2<u32>(uniforms.physicalPageSize)) / f32(uniforms.physicalPageSize);
    let edge = page_edge_factor(page_local);
    let layer_tint = cascade_color(layer);
    var color = shade_atlas_depth(depth, allocated);
    color = mix(color, color * (0.45 + 0.55 * layer_tint), 0.45);
    color = mix(color, vec3<f32>(1.0, 0.98, 0.90), edge * 0.75);
    if ((ppt_entry & PPT_VISIBLE_BIT) != 0u) {
        color = mix(color, layer_tint, 0.18);
    }
    out_color = vec4<f32>(color, 1.0);
    textureStore(outputTexture, vec2<i32>(global_id.xy), out_color);
}
