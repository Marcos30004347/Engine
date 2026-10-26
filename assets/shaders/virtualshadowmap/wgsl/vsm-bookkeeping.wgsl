#include "vsm-common.wgsl"

const COUNTER_CAPPED_REQUESTS : u32 = 0u;
const COUNTER_FALLBACK_REQUESTS : u32 = 1u;
const COUNTER_FREE_PAGE_COUNT : u32 = 2u;

struct BookkeepingUniforms {
    pageTableResolution: u32,
    physicalPageTableResolution: u32,
    activeLayers: u32,
    maskWordsPerLayer: u32,
    requestCapacity: u32,
    futureRequestCapacity: u32,
    cascadeCount: u32,
    activeLights: u32,
    debugOutputEnabled: u32,
    fallbackCascadeOffset: u32,
    physicalPageSize: u32,
    shadowFilterTaps: u32,
    projectedLightIndex: i32,
    pcfRadiusTexels: f32,
    localLightMipTransitionScale: f32,
    localLightMipBias: f32,
    localLightPageWorldScale: f32,
    firstCascadeWorldExtent: f32,
    _padding3: vec3<f32>,
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
    viewProjection: mat4x4<f32>,
    cameraPosition: vec4<f32>,
    reverseZ: u32,
    forceVisiblePagesDirtyLight: u32,
    _padding: vec2<u32>,
}

struct Light {
    position: vec4<f32>,
    direction: vec4<f32>,
    color: vec4<f32>,
    params: vec4<f32>,
    metadata: vec4<u32>,
}

const LIGHT_TYPE_DIRECTIONAL : u32 = 0u;
const LIGHT_TYPE_SPOT : u32 = 1u;
const LIGHT_TYPE_POINT : u32 = 2u;

@group(0) @binding(0) var<uniform> uniforms: BookkeepingUniforms;
@group(0) @binding(1) var<storage, read_write> invalidationMasks: array<atomic<u32>>;
@group(0) @binding(2) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(3) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(4) var<storage, read> cameraState: CameraState;
@group(0) @binding(5) var depthTexture: texture_depth_2d;
@group(0) @binding(6) var<storage, read_write> virtualPageTable: array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> physicalPageTable: array<u32>;
@group(0) @binding(8) var<storage, read_write> allocatorCounters: array<atomic<u32>>;
@group(0) @binding(9) var<storage, read_write> allocationRequests: array<u32>;
@group(0) @binding(10) var<storage, read_write> futureAllocationRequests: array<u32>;
@group(0) @binding(11) var<storage, read_write> unallocatedPhysicalPages: array<u32>;
@group(0) @binding(12) var outputTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(13) var<storage, read_write> virtualPageState: array<atomic<u32>>;
@group(0) @binding(14) var<storage, read> directionalLights: array<Light>;

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param page_coord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vpt_index(layer: u32, page_coord: vec2<u32>) -> u32 {
    let pages_per_layer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pages_per_layer + page_coord.y * uniforms.pageTableResolution + page_coord.x;
}

/// @brief Computes the flat physical page table index for the given physical page coordinate.
/// @param page_coord The 2D physical page coordinate within the atlas.
/// @returns The flat array index into the physical page table.
fn ppt_index(page_coord: vec2<u32>) -> u32 {
    return page_coord.y * uniforms.physicalPageTableResolution + page_coord.x;
}

/// @brief Computes the flat index into the invalidation mask buffer for a given layer and page row.
/// @param layer The cascade layer index.
/// @param row The page row (Y coordinate) within the layer.
/// @returns The flat array index into the invalidation masks buffer.
fn mask_word_index(layer: u32, row: u32) -> u32 {
    return layer * uniforms.maskWordsPerLayer + row;
}

/// @brief Atomically loads a virtual page table entry.
/// @param index The flat index into the virtual page table.
/// @returns The current packed entry value.
fn load_vpt_entry(index: u32) -> u32 {
    return atomicLoad(&virtualPageTable[index]);
}

/// @brief Atomically stores a packed entry into the virtual page table.
/// @param index The flat index into the virtual page table.
/// @param value The packed entry value to store.
fn store_vpt_entry(index: u32, value: u32) {
    atomicStore(&virtualPageTable[index], value);
}

/// @brief Atomically loads the page state for a given virtual page index.
/// @param index The flat index into the virtual page state buffer.
/// @returns The current packed page state value.
fn load_page_state(index: u32) -> u32 {
    return atomicLoad(&virtualPageState[index]);
}

/// @brief Atomically stores a packed page state for a given virtual page index.
/// @param index The flat index into the virtual page state buffer.
/// @param value The packed page state value to store.
fn store_page_state(index: u32, value: u32) {
    atomicStore(&virtualPageState[index], value);
}

/// @brief Checks whether a virtual page has been dynamically invalidated this frame.
/// @param layer The cascade layer containing the page.
/// @param page_coord The 2D page coordinate within the layer.
/// @returns True if the page's invalidation bit is set in the mask buffer.
fn page_is_dynamically_invalidated(layer: u32, page_coord: vec2<u32>) -> bool {
    let row_mask = atomicLoad(&invalidationMasks[mask_word_index(layer, page_coord.y)]);
    return extract_page_bit_from_mask(page_coord, row_mask);
}

/// @brief Clears the per-frame dynamic invalidation bit for a virtual page.
/// @param layer The cascade layer containing the page.
/// @param page_coord The 2D page coordinate within the layer.
fn clear_dynamic_invalidation(layer: u32, page_coord: vec2<u32>) {
    let bit = 1u << page_coord.x;
    let row_index = mask_word_index(layer, page_coord.y);
    atomicAnd(&invalidationMasks[row_index], ~bit);
}

/// @brief Reconstructs the world-space position of a pixel from the depth texture using the camera inverse view-projection.
/// @param pixel_coord The integer pixel coordinate in the depth texture.
/// @returns The reconstructed world-space position.
fn reconstruct_world_position(pixel_coord: vec2<u32>) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

/// @brief Returns whether a virtual page entry represents a fully resident, non-dirty page.
/// @param entry The packed virtual page table entry.
/// @returns True if the page is allocated and not dirty.
fn page_is_valid(entry: u32) -> bool {
    return get_is_allocated(entry) && !get_is_dirty(entry);
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

fn light_type(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.x;
}

fn light_first_layer(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.y;
}

fn light_layer_count(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.z;
}

fn light_mip_count(light_index: u32) -> u32 {
    return max(directionalLights[light_index].metadata.w, 1u);
}

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

#include "vsm-local-light-selection.wgsl"

fn point_light_layer(light_index: u32, face_index: u32, mip_level: u32) -> u32 {
    return light_first_layer(light_index) + face_index * light_mip_count(light_index) + mip_level;
}

fn point_light_primary_face(light_position: vec3<f32>, world_position: vec3<f32>) -> u32 {
    let light_vector = world_position - light_position;
    let abs_light_vector = abs(light_vector);
    if (abs_light_vector.x >= abs_light_vector.y && abs_light_vector.x >= abs_light_vector.z) {
        return select(1u, 0u, light_vector.x >= 0.0);
    }
    if (abs_light_vector.y >= abs_light_vector.z) {
        return select(3u, 2u, light_vector.y >= 0.0);
    }
    return select(5u, 4u, light_vector.z >= 0.0);
}

fn light_affects_world_position(light_index: u32, world_position: vec3<f32>) -> bool {
    if (light_index >= uniforms.activeLights) {
        return false;
    }

    let kind = light_type(light_index);
    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        return true;
    }

    let light_position = directionalLights[light_index].position.xyz;
    let to_receiver = world_position - light_position;
    let range = max(directionalLights[light_index].params.x, 0.0);
    if (dot(to_receiver, to_receiver) > range * range) {
        return false;
    }

    if (kind == LIGHT_TYPE_POINT) {
        return true;
    }

    let to_receiver_len_sq = dot(to_receiver, to_receiver);
    if (to_receiver_len_sq <= 1e-8) {
        return true;
    }

    let light_direction = normalize(directionalLights[light_index].direction.xyz);
    let outer_angle = clamp(directionalLights[light_index].params.y, 0.0, 3.14159265);
    let cos_outer = cos(outer_angle * 0.5);
    return dot(normalize(to_receiver), light_direction) >= cos_outer;
}

fn pixel_footprint_overlaps_local_light_influence(
    light_index: u32,
    pixel_coord: vec2<u32>,
    depth: f32,
    center_world_position: vec3<f32>
) -> bool {
    if (light_affects_world_position(light_index, center_world_position)) {
        return true;
    }

    let depth_dims = textureDimensions(depthTexture);
    let corner_offsets = array<vec2<u32>, 4>(
        vec2<u32>(0u, 0u),
        vec2<u32>(1u, 0u),
        vec2<u32>(0u, 1u),
        vec2<u32>(1u, 1u));

    for (var i = 0u; i < 4u; i++) {
        let corner = min(pixel_coord + corner_offsets[i], depth_dims);
        let corner_world_position = vsm_reconstruct_world_position_from_pixel_corner(corner, depth, depth_dims);
        if (light_affects_world_position(light_index, corner_world_position)) {
            return true;
        }
    }

    return false;
}

fn uv_rect_overlaps_circle(
    uv_min: vec2<f32>,
    uv_max: vec2<f32>,
    circle_center: vec2<f32>,
    circle_radius: f32
) -> bool {
    let closest = clamp(circle_center, uv_min, uv_max);
    let delta = closest - circle_center;
    return dot(delta, delta) <= circle_radius * circle_radius;
}

fn perspective_page_overlaps_local_light_influence(
    light_index: u32,
    layer: u32,
    page_coords: vec2<u32>
) -> bool {
    if (light_type(light_index) != LIGHT_TYPE_SPOT) {
        return true;
    }

    let page_resolution = f32(layer_page_table_resolution(layer));
    let uv_min = vec2<f32>(page_coords) / page_resolution;
    let uv_max = vec2<f32>(page_coords + vec2<u32>(1u, 1u)) / page_resolution;
    return uv_rect_overlaps_circle(uv_min, uv_max, vec2<f32>(0.5, 0.5), 0.5);
}

fn find_covering_directional_layer(world_position: vec3<f32>, light_index: u32, preferred_cascade: u32) -> i32 {
    let first_layer = light_first_layer(light_index);
    let layer_count = min(light_layer_count(light_index), uniforms.cascadeCount);
    var cascade = preferred_cascade;
    loop {
        if (cascade >= layer_count) {
            break;
        }

        let layer = first_layer + cascade;
        if (layer < uniforms.activeLayers && cascade_contains_world_position(world_position, layer)) {
            return i32(layer);
        }

        cascade = cascade + 1u;
    }

    return -1;
}

fn project_pixel_footprint_to_perspective_layer(
    pixel_coord: vec2<u32>,
    depth: f32,
    layer: u32,
    center_uv_out: ptr<function, vec2<f32>>,
    center_wrapped_page_out: ptr<function, vec2<u32>>,
    center_page_local_uv_out: ptr<function, vec2<f32>>,
    page_min_out: ptr<function, vec2<u32>>,
    page_max_out: ptr<function, vec2<u32>>,
    uv_min_out: ptr<function, vec2<f32>>,
    uv_max_out: ptr<function, vec2<f32>>,
    center_ndc_out: ptr<function, vec3<f32>>
) -> bool {
    let depth_dims = textureDimensions(depthTexture);
    var center_uv = vec2<f32>(0.0, 0.0);
    if (!vsm_project_world_to_light_uv(
            vsm_reconstruct_world_position_from_uv_depth(vsm_texel_center_uv(pixel_coord, depth_dims), depth, cameraState.inverseViewProjection),
            layer,
            &center_uv,
            center_ndc_out)) {
        return false;
    }

    let page_table_resolution = layer_page_table_resolution(layer);
    let center_page_sample = vsm_virtual_uv_to_page_sample(center_uv, page_table_resolution);
    (*center_uv_out) = center_uv;
    (*center_wrapped_page_out) = center_page_sample.pageCoords;
    (*center_page_local_uv_out) = center_page_sample.pageLocalUV;

    var uv_min = vec2<f32>(0.0, 0.0);
    var uv_max = vec2<f32>(0.0, 0.0);
    if (!vsm_project_pixel_footprint_uv_bounds(pixel_coord, depth, layer, &uv_min, &uv_max)) {
        return false;
    }

    (*uv_min_out) = uv_min;
    (*uv_max_out) = uv_max;

    let page_res_f = f32(page_table_resolution);
    let clamped_uv_min = clamp(uv_min, vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0));
    let clamped_uv_max = clamp(uv_max, vec2<f32>(0.0, 0.0), vec2<f32>(1.0, 1.0));
    (*page_min_out) = min(
        vec2<u32>(floor(clamped_uv_min * page_res_f)),
        vec2<u32>(page_table_resolution - 1u, page_table_resolution - 1u));
    (*page_max_out) = min(
        vec2<u32>(max(ceil(clamped_uv_max * page_res_f), vec2<f32>(1.0, 1.0)) - vec2<f32>(1.0, 1.0)),
        vec2<u32>(page_table_resolution - 1u, page_table_resolution - 1u));
    return true;
}

fn footprint_touches_face_edge(uv_min: vec2<f32>, uv_max: vec2<f32>, border: f32) -> bool {
    return uv_min.x <= border || uv_min.y <= border || uv_max.x >= (1.0 - border) || uv_max.y >= (1.0 - border);
}

/// @brief Attempts to enqueue a page allocation request into the fallback (future) request list.
/// @param virtual_coords The packed (x, y, layer) virtual page coordinates to request.
/// @returns True if the request was successfully enqueued within capacity.
fn enqueue_fallback_request(virtual_coords: vec3<u32>) -> bool {
    let packed_request = pack_layered_coords(virtual_coords.x, virtual_coords.y, virtual_coords.z);
    let request_index = atomicAdd(&allocatorCounters[COUNTER_FALLBACK_REQUESTS], 1u);
    if (request_index < uniforms.futureRequestCapacity) {
        futureAllocationRequests[request_index] = packed_request;
        return true;
    }
    return false;
}

/// @brief Attempts to enqueue a page allocation request into the regular (capped) request list.
/// @param virtual_coords The packed (x, y, layer) virtual page coordinates to request.
/// @returns True if the request was successfully enqueued within capacity.
fn enqueue_regular_request(virtual_coords: vec3<u32>) -> bool {
    let packed_request = pack_layered_coords(virtual_coords.x, virtual_coords.y, virtual_coords.z);
    let request_index = atomicAdd(&allocatorCounters[COUNTER_CAPPED_REQUESTS], 1u);
    if (request_index < uniforms.requestCapacity) {
        allocationRequests[request_index] = packed_request;
        return true;
    }
    return false;
}

/// @brief Returns the maximum number of physical pages that can be tracked in the free page queue.
/// @returns The total capacity of the free physical page queue.
fn free_page_queue_capacity() -> u32 {
    return uniforms.physicalPageTableResolution * uniforms.physicalPageTableResolution;
}

/// @brief Pushes a physical page back onto the free page queue for future reuse.
/// @param physical_coords The 2D physical page coordinates to return to the free list.
fn push_free_physical_page(physical_coords: vec2<u32>) {
    let capacity = free_page_queue_capacity();
    if (capacity == 0u) {
        return;
    }

    loop {
        let free_page_count = atomicLoad(&allocatorCounters[COUNTER_FREE_PAGE_COUNT]);
        if (free_page_count >= capacity) {
            return;
        }

        let claim = atomicCompareExchangeWeak(
            &allocatorCounters[COUNTER_FREE_PAGE_COUNT],
            free_page_count,
            free_page_count + 1u);
        if (claim.exchanged) {
            unallocatedPhysicalPages[free_page_count] = pack_physical_coords(physical_coords.x, physical_coords.y);
            return;
        }
    }
}

/// @brief Pops a free physical page from the free page queue using a lock-free CAS loop.
/// @returns The packed physical page coordinates, or 0xFFFFFFFF if the queue is empty.
fn pop_free_physical_page() -> u32 {
    var claimed_page = 0xFFFFFFFFu;
    loop {
        let free_page_count = atomicLoad(&allocatorCounters[COUNTER_FREE_PAGE_COUNT]);
        if (free_page_count == 0u) {
            break;
        }

        let claim = atomicCompareExchangeWeak(
            &allocatorCounters[COUNTER_FREE_PAGE_COUNT],
            free_page_count,
            free_page_count - 1u);
        if (claim.exchanged) {
            claimed_page = unallocatedPhysicalPages[free_page_count - 1u];
            break;
        }
    }

    return claimed_page;
}

/// @brief Atomically ensures the page state has the requested visibility and fallback flags set, using a CAS retry loop.
/// @param virtual_index The flat index of the virtual page in the state buffer.
/// @param make_visible If true, sets the visible flag.
/// @param make_fallback If true, sets the fallback flag.
/// @returns The resolved page state after all flags are applied.
fn ensure_page_state(virtual_index: u32, make_visible: bool, make_fallback: bool) -> u32 {
    var resolved_entry = load_page_state(virtual_index);
    loop {
        var updated_entry = resolved_entry;
        if (make_visible) {
            updated_entry = set_page_state_visible(updated_entry, true);
        }
        if (make_fallback) {
            updated_entry = set_page_state_fallback(updated_entry, true);
        }

        if (updated_entry == resolved_entry) {
            break;
        }

        let update_result = atomicCompareExchangeWeak(&virtualPageState[virtual_index], resolved_entry, updated_entry);
        if (update_result.exchanged) {
            resolved_entry = updated_entry;
            break;
        }

        resolved_entry = update_result.old_value;
    }

    return resolved_entry;
}

/// @brief Atomically ensures the dirty flag is set on a virtual page table entry, using a CAS retry loop.
/// @param virtual_index The flat index of the virtual page in the page table.
/// @returns The resolved virtual page table entry with the dirty flag set.
fn ensure_vpt_dirty(virtual_index: u32) -> u32 {
    var resolved_entry = load_vpt_entry(virtual_index);
    loop {
        let updated_entry = set_dirty(resolved_entry, true);
        if (updated_entry == resolved_entry) {
            break;
        }

        let update_result = atomicCompareExchangeWeak(&virtualPageTable[virtual_index], resolved_entry, updated_entry);
        if (update_result.exchanged) {
            resolved_entry = updated_entry;
            break;
        }

        resolved_entry = update_result.old_value;
    }

    return resolved_entry;
}

fn force_visible_pages_dirty(layer: u32) -> bool {
    return cameraState.forceVisiblePagesDirtyLight == 0xffffffffu ||
        (cameraState.forceVisiblePagesDirtyLight != 0u &&
         cascadeMatrices[layer].lightIndex + 1u == cameraState.forceVisiblePagesDirtyLight);
}

fn ensure_visible_page_dirty(layer: u32, virtual_index: u32) {
    if (force_visible_pages_dirty(layer)) {
        _ = ensure_vpt_dirty(virtual_index);
    }
}

/// @brief Resolves the fallback cascade layer index for a given primary layer, if one is configured.
/// @param layer The primary cascade layer index.
/// @returns The fallback layer index, or 0xFFFFFFFF if no valid fallback exists.
fn resolve_fallback_layer(layer: u32) -> u32 {
    let light_index = cascadeMatrices[layer].lightIndex;
    let light_kind = light_type(light_index);
    var fallback_layer = 0xFFFFFFFFu;

    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        if (uniforms.fallbackCascadeOffset == 0u) {
            return 0xFFFFFFFFu;
        }

        let current_cascade = cascadeMatrices[layer].cascadeIndex;
        let fallback_cascade = current_cascade + uniforms.fallbackCascadeOffset;
        if (fallback_cascade >= uniforms.cascadeCount) {
            return 0xFFFFFFFFu;
        }

        fallback_layer = light_first_layer(light_index) + fallback_cascade;
    } else {
        let mip_count = light_mip_count(light_index);
        let current_mip = cascadeMatrices[layer].mipLevel;
        let fallback_mip = current_mip + 1u;
        if (fallback_mip >= mip_count) {
            return 0xFFFFFFFFu;
        }

        if (light_kind == LIGHT_TYPE_POINT) {
            fallback_layer = point_light_layer(light_index, cascadeMatrices[layer].faceIndex, fallback_mip);
        } else {
            fallback_layer = light_first_layer(light_index) + fallback_mip;
        }
    }

    if (fallback_layer >= uniforms.activeLayers) {
        return 0xFFFFFFFFu;
    }

    return fallback_layer;
}

/// @brief Requests allocation of the fallback cascade page covering a world-space position.
/// @param world_position The world-space position whose fallback page should be requested.
/// @param source_layer The primary cascade layer that triggered the fallback request.
fn request_fallback_page(world_position: vec3<f32>, source_layer: u32) {
    let fallback_layer = resolve_fallback_layer(source_layer);
    if (fallback_layer == 0xFFFFFFFFu) {
        return;
    }

    var wrapped_page_coords = vec2<u32>(0u, 0u);
    var page_local_uv = vec2<f32>(0.0, 0.0);
    var ndc = vec3<f32>(0.0, 0.0, 0.0);
    if (!vsm_try_project_world_to_wrapped_page_sample(
            world_position,
            cascadeMatrices[fallback_layer].viewProj,
            cascadeStates[fallback_layer].pageOffset,
            layer_page_table_resolution(fallback_layer),
            &wrapped_page_coords,
            &page_local_uv,
            &ndc)) {
        return;
    }

    let fallback_index = vpt_index(fallback_layer, wrapped_page_coords);
    let fallback_entry = load_vpt_entry(fallback_index);
    let needs_fallback_draw = !page_is_valid(fallback_entry) || force_visible_pages_dirty(fallback_layer);
    if (needs_fallback_draw) {
        _ = ensure_vpt_dirty(fallback_index);
    }
    _ = ensure_page_state(fallback_index, true, needs_fallback_draw);
}

fn offset_wrapped_page_coords(page_coords: vec2<u32>, delta: vec2<i32>, page_table_resolution_u: u32) -> vec2<u32> {
    let page_table_resolution = i32(page_table_resolution_u);
    let wrapped_x = ((i32(page_coords.x) + delta.x) % page_table_resolution + page_table_resolution) % page_table_resolution;
    let wrapped_y = ((i32(page_coords.y) + delta.y) % page_table_resolution + page_table_resolution) % page_table_resolution;
    return vec2<u32>(u32(wrapped_x), u32(wrapped_y));
}

fn ensure_visible_footprint_pages(
    layer: u32,
    wrapped_page_coords: vec2<u32>,
    page_local_uv: vec2<f32>
) -> u32 {
    let center_virtual_index = vpt_index(layer, wrapped_page_coords);
    var center_page_state = ensure_page_state(center_virtual_index, true, false);
    ensure_visible_page_dirty(layer, center_virtual_index);
    let page_table_resolution = layer_page_table_resolution(layer);

    if (uniforms.shadowFilterTaps <= 1u || uniforms.pcfRadiusTexels <= 1e-4) {
        return center_page_state;
    }

    let page_size = f32(max(uniforms.physicalPageSize, 1u));
    let local_texel = page_local_uv * page_size;
    let filter_radius = max(uniforms.pcfRadiusTexels, 0.0);
    let min_page_delta = vec2<i32>(
        i32(floor((local_texel.x - filter_radius) / page_size)),
        i32(floor((local_texel.y - filter_radius) / page_size)));
    let max_page_delta = vec2<i32>(
        i32(floor((local_texel.x + filter_radius) / page_size)),
        i32(floor((local_texel.y + filter_radius) / page_size)));

    var page_delta_y = min_page_delta.y;
    loop {
        if (page_delta_y > max_page_delta.y) {
            break;
        }

        var page_delta_x = min_page_delta.x;
        loop {
            if (page_delta_x > max_page_delta.x) {
                break;
            }

            if (page_delta_x != 0 || page_delta_y != 0) {
                let wrapped_neighbor = offset_wrapped_page_coords(
                    wrapped_page_coords,
                    vec2<i32>(page_delta_x, page_delta_y),
                    page_table_resolution);
                let neighbor_virtual_index = vpt_index(layer, wrapped_neighbor);
                _ = ensure_page_state(neighbor_virtual_index, true, false);
                ensure_visible_page_dirty(layer, neighbor_virtual_index);
            }

            page_delta_x = page_delta_x + 1;
        }

        page_delta_y = page_delta_y + 1;
    }

    return center_page_state;
}

fn ensure_visible_page_rect(
    light_index: u32,
    layer: u32,
    page_min: vec2<u32>,
    page_max: vec2<u32>,
    center_page_coords: vec2<u32>
) -> u32 {
    var center_page_state = 0u;
    for (var page_y = page_min.y; page_y <= page_max.y; page_y++) {
        for (var page_x = page_min.x; page_x <= page_max.x; page_x++) {
            let page_coords = vec2<u32>(page_x, page_y);
            if (!perspective_page_overlaps_local_light_influence(light_index, layer, page_coords)) {
                continue;
            }
            let page_index = vpt_index(layer, page_coords);
            let page_state = ensure_page_state(page_index, true, false);
            ensure_visible_page_dirty(layer, page_index);
            if (all(page_coords == center_page_coords)) {
                center_page_state = page_state;
            }
        }
    }
    return center_page_state;
}

/// @brief Computes a pseudo-random float in [0, 1) from a float seed.
/// @param value The input seed value.
/// @returns A pseudo-random float in [0, 1).
fn hash11(value: f32) -> f32 {
    return fract(sin(value * 91.3458 + 17.123) * 41358.5453123);
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

/// @brief Generates a visually distinct color for a virtual page based on its ID for debug visualization.
/// @param page_id The (x, y, layer) virtual page identifier.
/// @returns A stable pseudo-random RGB color for the given page.
fn page_id_color(page_id: vec3<u32>) -> vec3<f32> {
    let seed =
        hash_u32(page_id.x * 73856093u) ^
        hash_u32(page_id.y * 19349663u) ^
        hash_u32(page_id.z * 83492791u);
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

/// @brief Produces a debug visualization color for a virtual page based on its allocation state and visibility.
/// @param entry The packed virtual page table entry.
/// @param page_state The packed page state word.
/// @param layer The cascade layer index.
/// @param wrapped_page The wrapped 2D page coordinate.
/// @param page_local_uv The fractional UV within the page, used for edge highlighting.
/// @returns An RGBA debug color for the page.
fn shade_assigned_page(entry: u32, page_state: u32, layer: u32, wrapped_page: vec2<u32>, page_local_uv: vec2<f32>) -> vec4<f32> {
    let seed = pack_layered_coords(wrapped_page.x, wrapped_page.y, layer);
    let hashed_page_color = page_id_color(vec3<u32>(wrapped_page, layer));
    let random_intensity = 0.45 + hash11(f32(seed)) * 0.35;

    var brightness = 0.08;
    if (get_is_allocated(entry)) {
        brightness = 0.42 + 0.18 * random_intensity;
    }
    if (get_page_state_is_visible(page_state)) {
        brightness = 0.82 + 0.14 * random_intensity;
    }

    var color = hashed_page_color * brightness + vec3<f32>(0.01, 0.01, 0.012);
    color = mix(color, vec3<f32>(1.0, 0.98, 0.92), page_edge_factor(page_local_uv) * 0.7);
    if (get_is_dirty(entry)) {
        color = mix(color, vec3<f32>(1.0, 0.42, 0.18), 0.45);
    }

    return vec4<f32>(clamp(color, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}

// Structurally mirrors the design doc's free_wrapped_pages helper.
/// @brief Frees the physical page backing a virtual page if it falls in the wrap-around or dynamically invalidated region.
/// @param layer The cascade layer containing the page.
/// @param page_coords The linear (unwrapped) 2D page coordinates.
/// @param cascade_shift The integer shift applied to the cascade origin this frame.
/// @param cascade_offset The current toroidal offset of the cascade in page space.
fn free_wrapped_pages(layer: u32, page_coords: vec2<u32>, cascade_shift: vec2<i32>, cascade_offset: vec2<i32>) {
    let page_table_resolution = i32(layer_page_table_resolution(layer));
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(page_coords), cascade_offset, page_table_resolution);
    if (wrapped_page_coords.x < 0 || wrapped_page_coords.y < 0) {
        return;
    }

    let should_clear_wrap = should_clear_wrap_region(vec2<i32>(page_coords), cascade_shift, page_table_resolution);
    let should_clear_dynamic = page_is_dynamically_invalidated(layer, page_coords);
    if (!should_clear_wrap && !should_clear_dynamic) {
        return;
    }

    // Consume one-frame invalidation bits as the bookkeeping pass visits pages.
    if (should_clear_dynamic) {
        clear_dynamic_invalidation(layer, page_coords);
    }

    let virtual_index = vpt_index(layer, vec2<u32>(wrapped_page_coords));
    let page_entry = load_vpt_entry(virtual_index);
    if (should_clear_dynamic && !should_clear_wrap) {
        if (get_is_allocated(page_entry)) {
            store_vpt_entry(virtual_index, set_dirty(page_entry, true));
        }
        return;
    }

    if (get_is_allocated(page_entry)) {
        let physical_coords = vec2<u32>(get_page_x(page_entry), get_page_y(page_entry));
        let physical_index = ppt_index(physical_coords);
        physicalPageTable[physical_index] = 0u;
        push_free_physical_page(physical_coords);
    }

    store_vpt_entry(virtual_index, 0u);
}

/// @brief Resets the allocation request counters to zero at the start of each frame.
@compute @workgroup_size(1, 1, 1)
fn init_allocator_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.x != 0u || global_id.y != 0u || global_id.z != 0u) {
        return;
    }

    atomicStore(&allocatorCounters[COUNTER_CAPPED_REQUESTS], 0u);
    atomicStore(&allocatorCounters[COUNTER_FALLBACK_REQUESTS], 0u);
}

/// @brief Resets page state and frees pages invalidated by cascade scrolling or dynamic changes.
@compute @workgroup_size(8, 8, 1)
fn bookkeeping_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.z >= uniforms.activeLayers) {
        return;
    }

    let page_table_resolution = layer_page_table_resolution(global_id.z);
    if (global_id.x >= page_table_resolution || global_id.y >= page_table_resolution) {
        return;
    }

    let state = cascadeStates[global_id.z];
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(global_id.xy), state.pageOffset, i32(page_table_resolution));
    if (wrapped_page_coords.x < 0 || wrapped_page_coords.y < 0) {
        return;
    }

    let virtual_index = vpt_index(global_id.z, vec2<u32>(wrapped_page_coords));
    store_page_state(virtual_index, 0u);

    free_wrapped_pages(global_id.z, global_id.xy, state.pageShift, state.pageOffset);
}

/// @brief Analyzes the depth buffer to identify which virtual shadow pages are visible and marks them accordingly.
@compute @workgroup_size(8, 8, 1)
fn analyze_visible_pages_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let depth_dims = textureDimensions(depthTexture);
    let output_dims = textureDimensions(outputTexture);
    if (global_id.x >= depth_dims.x || global_id.y >= depth_dims.y ||
        global_id.x >= output_dims.x || global_id.y >= output_dims.y) {
        return;
    }

    let output_pixel = vec2<i32>(global_id.xy);
    let empty_color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    let write_debug_output = uniforms.debugOutputEnabled != 0u;

    let depth = textureLoad(depthTexture, output_pixel, 0);
    if (depth <= 0.0) {
        if (write_debug_output) {
            textureStore(outputTexture, output_pixel, empty_color);
        }
        return;
    }

    let uv = vsm_texel_center_uv(global_id.xy, depth_dims);
    let camera_distance = length(vsm_reconstruct_view_position_from_uv_depth(uv, depth, cameraState.inverseProjection));
    let world_position = vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
    let preferred_cascade = select_cascade_level(camera_distance);
    let selected_light = u32(clamp(uniforms.projectedLightIndex, 0, max(i32(uniforms.activeLights), 1) - 1));

    var debug_color = empty_color;
    var wrote_debug = false;

    for (var light_index = 0u; light_index < uniforms.activeLights; light_index++) {
        let light_kind = light_type(light_index);
        if (light_kind != LIGHT_TYPE_DIRECTIONAL &&
            !pixel_footprint_overlaps_local_light_influence(light_index, global_id.xy, depth, world_position)) {
            continue;
        }
        if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
            let cascade_level = find_covering_directional_layer(world_position, light_index, preferred_cascade);
            if (cascade_level < 0) {
                continue;
            }

            let layer = u32(cascade_level);
            var wrapped_page_coords = vec2<u32>(0u, 0u);
            var page_local_uv = vec2<f32>(0.0, 0.0);
            var ndc = vec3<f32>(0.0, 0.0, 0.0);
            if (!vsm_try_project_world_to_wrapped_page_sample(
                    world_position,
                    cascadeMatrices[layer].viewProj,
                    cascadeStates[layer].pageOffset,
                    layer_page_table_resolution(layer),
                    &wrapped_page_coords,
                    &page_local_uv,
                    &ndc)) {
                continue;
            }

            let virtual_index = vpt_index(layer, wrapped_page_coords);
            let page_state = ensure_visible_footprint_pages(layer, wrapped_page_coords, page_local_uv);
            let page_entry = load_vpt_entry(virtual_index);

            if (!page_is_valid(page_entry)) {
                request_fallback_page(world_position, layer);
            }

            if (write_debug_output && light_index == selected_light && !wrote_debug) {
                debug_color = shade_assigned_page(page_entry, page_state, layer, wrapped_page_coords, page_local_uv);
                wrote_debug = true;
            }
            continue;
        }

        if (light_kind == LIGHT_TYPE_POINT) {
            let primary_face = point_light_primary_face(directionalLights[light_index].position.xyz, world_position);
            let base_layer = point_light_layer(light_index, primary_face, 0u);
            var base_uv_min = vec2<f32>(0.0, 0.0);
            var base_uv_max = vec2<f32>(0.0, 0.0);
            var selected_layer = 0u;
            if (!vsm_select_perspective_layer_from_base_layer(
                    global_id.xy,
                    depth,
                    base_layer,
                    light_mip_count(light_index),
                    uniforms.physicalPageSize,
                    uniforms.localLightMipTransitionScale,
                    uniforms.localLightMipBias,
                    uniforms.localLightPageWorldScale,
                    &selected_layer,
                    &base_uv_min,
                    &base_uv_max)) {
                continue;
            }

            let base_page_table_resolution = layer_page_table_resolution(base_layer);
            var center_uv = vec2<f32>(0.0, 0.0);
            var center_page_coords = vec2<u32>(0u, 0u);
            var center_page_local_uv = vec2<f32>(0.0, 0.0);
            var page_min = vec2<u32>(0u, 0u);
            var page_max = vec2<u32>(0u, 0u);
            var uv_min = vec2<f32>(0.0, 0.0);
            var uv_max = vec2<f32>(0.0, 0.0);
            var ndc = vec3<f32>(0.0, 0.0, 0.0);
            if (!project_pixel_footprint_to_perspective_layer(
                    global_id.xy,
                    depth,
                    selected_layer,
                    &center_uv,
                    &center_page_coords,
                    &center_page_local_uv,
                    &page_min,
                    &page_max,
                    &uv_min,
                    &uv_max,
                    &ndc)) {
                continue;
            }

        let virtual_index = vpt_index(selected_layer, center_page_coords);
        let page_state = ensure_visible_page_rect(light_index, selected_layer, page_min, page_max, center_page_coords);
        let page_entry = load_vpt_entry(virtual_index);
        if (!page_is_valid(page_entry)) {
            request_fallback_page(world_position, selected_layer);
        }

            if (write_debug_output && light_index == selected_light && !wrote_debug) {
                debug_color = shade_assigned_page(page_entry, page_state, selected_layer, center_page_coords, center_page_local_uv);
                wrote_debug = true;
            }

            let seam_border = max(2.0 / f32(base_page_table_resolution), 0.02);
            if (footprint_touches_face_edge(base_uv_min, base_uv_max, seam_border)) {
                for (var face_index = 0u; face_index < 6u; face_index++) {
                    if (face_index == primary_face) {
                        continue;
                    }

                    let neighbor_base_layer = point_light_layer(light_index, face_index, 0u);
                    var neighbor_selected_layer = 0u;
                    var neighbor_base_uv_min = vec2<f32>(0.0, 0.0);
                    var neighbor_base_uv_max = vec2<f32>(0.0, 0.0);
                    if (!vsm_select_perspective_layer_from_base_layer(
                            global_id.xy,
                            depth,
                            neighbor_base_layer,
                            light_mip_count(light_index),
                            uniforms.physicalPageSize,
                            uniforms.localLightMipTransitionScale,
                            uniforms.localLightMipBias,
                            uniforms.localLightPageWorldScale,
                            &neighbor_selected_layer,
                            &neighbor_base_uv_min,
                            &neighbor_base_uv_max)) {
                        continue;
                    }

                    var neighbor_center_uv = vec2<f32>(0.0, 0.0);
                    var neighbor_center_page_coords = vec2<u32>(0u, 0u);
                    var neighbor_center_page_local_uv = vec2<f32>(0.0, 0.0);
                    var neighbor_page_min = vec2<u32>(0u, 0u);
                    var neighbor_page_max = vec2<u32>(0u, 0u);
                    var neighbor_uv_min = vec2<f32>(0.0, 0.0);
                    var neighbor_uv_max = vec2<f32>(0.0, 0.0);
                    var neighbor_ndc = vec3<f32>(0.0, 0.0, 0.0);
                    if (!project_pixel_footprint_to_perspective_layer(
                            global_id.xy,
                            depth,
                            neighbor_selected_layer,
                            &neighbor_center_uv,
                            &neighbor_center_page_coords,
                            &neighbor_center_page_local_uv,
                            &neighbor_page_min,
                            &neighbor_page_max,
                            &neighbor_uv_min,
                            &neighbor_uv_max,
                            &neighbor_ndc)) {
                        continue;
                    }

                    _ = ensure_visible_page_rect(light_index, neighbor_selected_layer, neighbor_page_min, neighbor_page_max, neighbor_center_page_coords);
                }
            }
            continue;
        }

        let base_layer = light_first_layer(light_index);
        var base_uv_min = vec2<f32>(0.0, 0.0);
        var base_uv_max = vec2<f32>(0.0, 0.0);
        var layer = 0u;
        if (!vsm_select_perspective_layer_from_base_layer(
                global_id.xy,
                depth,
                base_layer,
                light_mip_count(light_index),
                uniforms.physicalPageSize,
                uniforms.localLightMipTransitionScale,
                uniforms.localLightMipBias,
                uniforms.localLightPageWorldScale,
                &layer,
                &base_uv_min,
                &base_uv_max)) {
            continue;
        }

        var center_uv = vec2<f32>(0.0, 0.0);
        var center_page_coords = vec2<u32>(0u, 0u);
        var center_page_local_uv = vec2<f32>(0.0, 0.0);
        var page_min = vec2<u32>(0u, 0u);
        var page_max = vec2<u32>(0u, 0u);
        var uv_min = vec2<f32>(0.0, 0.0);
        var uv_max = vec2<f32>(0.0, 0.0);
        var ndc = vec3<f32>(0.0, 0.0, 0.0);
        if (!project_pixel_footprint_to_perspective_layer(
                global_id.xy,
                depth,
                layer,
                &center_uv,
                &center_page_coords,
                &center_page_local_uv,
                &page_min,
                &page_max,
                &uv_min,
                &uv_max,
                &ndc)) {
            continue;
        }

        let virtual_index = vpt_index(layer, center_page_coords);
        let page_state = ensure_visible_page_rect(light_index, layer, page_min, page_max, center_page_coords);
        let page_entry = load_vpt_entry(virtual_index);
        if (!page_is_valid(page_entry)) {
            request_fallback_page(world_position, layer);
        }

        if (write_debug_output && light_index == selected_light && !wrote_debug) {
            debug_color = shade_assigned_page(page_entry, page_state, layer, center_page_coords, center_page_local_uv);
            wrote_debug = true;
        }
    }

    if (write_debug_output) {
        textureStore(outputTexture, output_pixel, debug_color);
    }
}

/// @brief Emits allocation requests for visible dirty pages into the regular or fallback request queues.
@compute @workgroup_size(8, 8, 1)
fn emit_page_requests_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.z >= uniforms.activeLayers) {
        return;
    }

    let page_table_resolution = layer_page_table_resolution(global_id.z);
    if (global_id.x >= page_table_resolution || global_id.y >= page_table_resolution) {
        return;
    }

    let state = cascadeStates[global_id.z];
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(global_id.xy), state.pageOffset, i32(page_table_resolution));
    if (wrapped_page_coords.x < 0 || wrapped_page_coords.y < 0) {
        return;
    }

    let virtual_coords = vec2<u32>(wrapped_page_coords);
    let virtual_index = vpt_index(global_id.z, virtual_coords);
    let page_entry = load_vpt_entry(virtual_index);
    let page_state = load_page_state(virtual_index);
    if (!get_page_state_is_visible(page_state) || (get_is_allocated(page_entry) && !get_is_dirty(page_entry))) {
        return;
    }

    var queued = false;
    if (get_page_state_is_fallback(page_state)) {
        queued = enqueue_fallback_request(vec3<u32>(virtual_coords, global_id.z));
    } else {
        queued = enqueue_regular_request(vec3<u32>(virtual_coords, global_id.z));
    }

    if (!queued) {
        _ = ensure_vpt_dirty(virtual_index);
    }
}

/// @brief Allocates physical pages for all pending regular and fallback page requests by popping from the free page queue.
@compute @workgroup_size(64, 1, 1)
fn allocate_pages_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let regular_request_count = min(atomicLoad(&allocatorCounters[COUNTER_CAPPED_REQUESTS]), uniforms.requestCapacity);
    let fallback_request_count = min(atomicLoad(&allocatorCounters[COUNTER_FALLBACK_REQUESTS]), uniforms.futureRequestCapacity);
    let total_request_count = regular_request_count + fallback_request_count;
    if (global_id.x >= total_request_count) {
        return;
    }

    let is_fallback_request = global_id.x >= regular_request_count;
    var request = vec3<u32>(0u, 0u, 0u);
    if (is_fallback_request) {
        let request_index = global_id.x - regular_request_count;
        request = unpack_layered_coords(futureAllocationRequests[request_index]);
    } else {
        request = unpack_layered_coords(allocationRequests[global_id.x]);
    }
    let new_virtual_index = vpt_index(request.z, request.xy);
    let existing_entry = load_vpt_entry(new_virtual_index);
    let page_state = load_page_state(new_virtual_index);
    if (get_is_allocated(existing_entry) || !get_page_state_is_visible(page_state)) {
        return;
    }

    let chosen_physical_packed = pop_free_physical_page();
    if (chosen_physical_packed == 0xFFFFFFFFu) {
        return;
    }

    let chosen_physical = unpack_physical_coords(chosen_physical_packed);
    let physical_index = ppt_index(chosen_physical);

    // Preserve the current frame's visibility when materializing a requested page
    // so debug/state views do not immediately "lose" it after allocation.
    // Newly allocated pages still need one draw/resolve pass before they become cache-valid.
    store_vpt_entry(new_virtual_index, pack_vpt_entry(true, false, true, false, chosen_physical.x, chosen_physical.y));
    physicalPageTable[physical_index] = pack_ppt_entry(false, true, request.x, request.y, request.z);
}
