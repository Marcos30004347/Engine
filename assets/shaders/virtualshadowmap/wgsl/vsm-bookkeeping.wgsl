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
    debugOutputEnabled: u32,
    fallbackCascadeOffset: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
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
}

struct CameraState {
    inverseView: mat4x4<f32>,
    inverseProjection: mat4x4<f32>,
    inverseViewProjection: mat4x4<f32>,
    cameraPosition: vec4<f32>,
}

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
    let pixel = vec2<i32>(pixel_coord);
    let depth = textureLoad(depthTexture, pixel, 0);
    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    let world = cameraState.inverseViewProjection * ndc;
    return world.xyz / max(world.w, 1e-6);
}

/// @brief Returns whether a virtual page entry represents a fully resident, non-dirty page.
/// @param entry The packed virtual page table entry.
/// @returns True if the page is allocated and not dirty.
fn page_is_valid(entry: u32) -> bool {
    return get_is_allocated(entry) && !get_is_dirty(entry);
}

/// @brief Selects the preferred cascade level for a world-space position based on camera distance.
/// @param world_position The world-space position to evaluate.
/// @returns The preferred clipmap cascade level index.
fn select_cascade_level(world_position: vec3<f32>) -> u32 {
    return vsm_select_preferred_clipmap_level(
        world_position,
        cameraState.cameraPosition.xyz,
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

/// @brief Finds the finest active cascade level that covers a world-space position, starting from the preferred level.
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

/// @brief Resolves the fallback cascade layer index for a given primary layer, if one is configured.
/// @param layer The primary cascade layer index.
/// @returns The fallback layer index, or 0xFFFFFFFF if no valid fallback exists.
fn resolve_fallback_layer(layer: u32) -> u32 {
    if (uniforms.fallbackCascadeOffset == 0u) {
        return 0xFFFFFFFFu;
    }

    let current_cascade = cascadeMatrices[layer].cascadeIndex;
    let fallback_cascade = current_cascade + uniforms.fallbackCascadeOffset;
    if (fallback_cascade >= uniforms.cascadeCount) {
        return 0xFFFFFFFFu;
    }

    let fallback_layer = cascadeMatrices[layer].lightIndex * uniforms.cascadeCount + fallback_cascade;
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
            uniforms.pageTableResolution,
            &wrapped_page_coords,
            &page_local_uv,
            &ndc)) {
        return;
    }

    let fallback_index = vpt_index(fallback_layer, wrapped_page_coords);
    let fallback_entry = load_vpt_entry(fallback_index);
    let needs_fallback_draw = !page_is_valid(fallback_entry);
    if (needs_fallback_draw) {
        _ = ensure_vpt_dirty(fallback_index);
    }
    _ = ensure_page_state(fallback_index, true, needs_fallback_draw);
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
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(page_coords), cascade_offset, i32(uniforms.pageTableResolution));
    if (wrapped_page_coords.x < 0 || wrapped_page_coords.y < 0) {
        return;
    }

    let should_clear_wrap = should_clear_wrap_region(vec2<i32>(page_coords), cascade_shift, i32(uniforms.pageTableResolution));
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
    if (global_id.z >= uniforms.activeLayers ||
        global_id.x >= uniforms.pageTableResolution ||
        global_id.y >= uniforms.pageTableResolution) {
        return;
    }

    let state = cascadeStates[global_id.z];
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(global_id.xy), state.pageOffset, i32(uniforms.pageTableResolution));
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

    let world_position = reconstruct_world_position(global_id.xy);
    let preferred_layer = select_cascade_level(world_position);
    let cascade_level = find_covering_cascade_level(world_position, preferred_layer);
    if (cascade_level < 0) {
        if (write_debug_output) {
            textureStore(outputTexture, output_pixel, empty_color);
        }
        return;
    }
    let layer = u32(cascade_level);

    var wrapped_page_coords = vec2<u32>(0u, 0u);
    var page_local_uv = vec2<f32>(0.0, 0.0);
    var ndc = vec3<f32>(0.0, 0.0, 0.0);
    if (!vsm_try_project_world_to_wrapped_page_sample(
            world_position,
            cascadeMatrices[layer].viewProj,
            cascadeStates[layer].pageOffset,
            uniforms.pageTableResolution,
            &wrapped_page_coords,
            &page_local_uv,
            &ndc)) {
        if (write_debug_output) {
            textureStore(outputTexture, output_pixel, empty_color);
        }
        return;
    }

    let virtual_coords = wrapped_page_coords;
    let virtual_index = vpt_index(layer, virtual_coords);
    let page_entry = load_vpt_entry(virtual_index);
    let page_state = ensure_page_state(virtual_index, true, false);

    if (!page_is_valid(page_entry)) {
        request_fallback_page(world_position, layer);
    }

    if (write_debug_output) {
        textureStore(outputTexture, output_pixel, shade_assigned_page(page_entry, page_state, layer, virtual_coords, page_local_uv));
    }
}

/// @brief Emits allocation requests for visible dirty pages into the regular or fallback request queues.
@compute @workgroup_size(8, 8, 1)
fn emit_page_requests_main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    if (global_id.z >= uniforms.activeLayers ||
        global_id.x >= uniforms.pageTableResolution ||
        global_id.y >= uniforms.pageTableResolution) {
        return;
    }

    let state = cascadeStates[global_id.z];
    let wrapped_page_coords = virtual_page_coords_to_wrapped_coords(vec2<i32>(global_id.xy), state.pageOffset, i32(uniforms.pageTableResolution));
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
    store_vpt_entry(new_virtual_index, pack_vpt_entry(true, false, true, false, chosen_physical.x, chosen_physical.y));
    physicalPageTable[physical_index] = pack_ppt_entry(false, true, request.x, request.y, request.z);
}
