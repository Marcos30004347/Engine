const VPT_DIRTY_BIT : u32 = 1u << 0u;
const VPT_ALLOCATED_BIT : u32 = 1u << 2u;
const VPT_PAGE_X_SHIFT : u32 = 8u;
const VPT_PAGE_Y_SHIFT : u32 = 16u;

const VSM_PAGE_STATE_VISIBLE_BIT : u32 = 1u << 0u;
const VSM_PAGE_STATE_FALLBACK_BIT : u32 = 1u << 1u;

const PPT_VISIBLE_BIT : u32 = 1u << 0u;
const PPT_ALLOCATED_BIT : u32 = 1u << 1u;
const PPT_VPT_X_SHIFT : u32 = 8u;
const PPT_VPT_Y_SHIFT : u32 = 16u;
const PPT_LAYER_SHIFT : u32 = 24u;

struct VSMVirtualPageSample {
    pageCoords: vec2<u32>,
    pageLocalUV: vec2<f32>,
}

/// @brief Computes the location-given rotationally invariant preferred clipmap level from camera distance.
/// @details Our clipmaps are square in light space, so the largest rotation-invariant support region
///     is the inscribed circle. We therefore convert the level-0 square side length into a support
///     radius before applying the logarithmic distance rule, which keeps the chosen level radial
///     instead of letting square coverage tests dominate the result.
/// @param distance_to_camera World-space distance from the evaluated point to the camera.
/// @param level0_cascade_side_length World-space side length of the level-0 clipmap.
/// @param cascade_count Total number of clipmap cascade levels.
/// @returns The preferred clipmap level, clamped to the available cascade range.
fn vsm_select_rotationally_invariant_clipmap_level(
    distance_to_camera: f32,
    level0_cascade_side_length: f32,
    cascade_count: u32
) -> u32 {
    let level0_support_radius = max(level0_cascade_side_length * 0.5, 1e-4);
    let normalized_distance = max(distance_to_camera / level0_support_radius, 1e-6);
    let unclamped_level = max(i32(floor(log2(normalized_distance))), 0);
    return min(u32(unclamped_level), max(cascade_count, 1u) - 1u);
}

/// @brief Selects the preferred clipmap cascade level for a world position using the rotationally invariant distance rule.
/// @param world_position World-space position of the point being evaluated.
/// @param camera_position World-space position of the camera.
/// @param first_cascade_world_extent World-space side length covered by the first (finest) cascade.
/// @param cascade_count Total number of clipmap cascade levels.
/// @returns The preferred clipmap level for the world position.
fn vsm_select_preferred_clipmap_level(
    world_position: vec3<f32>,
    camera_position: vec3<f32>,
    first_cascade_world_extent: f32,
    cascade_count: u32
) -> u32 {
    let distance_to_camera = distance(world_position, camera_position);
    return vsm_select_rotationally_invariant_clipmap_level(
        distance_to_camera,
        first_cascade_world_extent,
        cascade_count);
}

/// @brief Computes the center UV for a texel coordinate within a 2D image.
/// @param pixel_coord The integer texel coordinate.
/// @param dimensions The image dimensions in texels.
/// @returns The normalized UV at the texel center.
fn vsm_texel_center_uv(pixel_coord: vec2<u32>, dimensions: vec2<u32>) -> vec2<f32> {
    return (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dimensions);
}

/// @brief Converts UV and depth into clip-space NDC coordinates.
/// @param uv The normalized texture coordinate.
/// @param depth The sampled depth value in the camera depth buffer.
/// @returns The homogeneous NDC position.
fn vsm_ndc_from_uv_depth(uv: vec2<f32>, depth: f32) -> vec4<f32> {
    return vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
}

/// @brief Projects a world-space position into a cascade's NDC space using its view-projection matrix.
/// @param world_position World-space position to project.
/// @param view_proj The cascade's combined view-projection matrix.
/// @returns The NDC coordinates (x, y, z) of the projected position.
fn vsm_project_world_to_cascade_ndc(world_position: vec3<f32>, view_proj: mat4x4<f32>) -> vec3<f32> {
    let clip = view_proj * vec4<f32>(world_position, 1.0);
    let inv_w = select(1.0, 1.0 / clip.w, abs(clip.w) > 1e-6);
    return clip.xyz * inv_w;
}

/// @brief Safely applies a perspective divide to a homogeneous position.
/// @param homogeneous_position The homogeneous coordinate to divide by `w`.
/// @returns The corresponding 3D position after perspective divide.
fn vsm_perspective_divide(homogeneous_position: vec4<f32>) -> vec3<f32> {
    let inv_w = select(1.0, 1.0 / homogeneous_position.w, abs(homogeneous_position.w) > 1e-6);
    return homogeneous_position.xyz * inv_w;
}

/// @brief Reconstructs camera-space position from screen UV and depth.
/// @param uv The normalized screen UV of the sample.
/// @param depth The sampled camera depth.
/// @param inverse_projection The inverse projection matrix of the camera.
/// @returns The reconstructed camera-space position.
fn vsm_reconstruct_view_position_from_uv_depth(
    uv: vec2<f32>,
    depth: f32,
    inverse_projection: mat4x4<f32>
) -> vec3<f32> {
    let view = inverse_projection * vsm_ndc_from_uv_depth(uv, depth);
    return vsm_perspective_divide(view);
}

/// @brief Reconstructs world-space position from screen UV and depth.
/// @param uv The normalized screen UV of the sample.
/// @param depth The sampled camera depth.
/// @param inverse_view_projection The inverse view-projection matrix of the camera.
/// @returns The reconstructed world-space position.
fn vsm_reconstruct_world_position_from_uv_depth(
    uv: vec2<f32>,
    depth: f32,
    inverse_view_projection: mat4x4<f32>
) -> vec3<f32> {
    let world = inverse_view_projection * vsm_ndc_from_uv_depth(uv, depth);
    return vsm_perspective_divide(world);
}

/// @brief Tests whether an NDC coordinate falls within the canonical unit frustum of a cascade.
/// @param ndc The NDC coordinate to test.
/// @returns True if the NDC position is inside the cascade frustum.
fn vsm_cascade_ndc_is_covered(ndc: vec3<f32>) -> bool {
    return ndc.x >= -1.0 && ndc.x <= 1.0 &&
           ndc.y >= -1.0 && ndc.y <= 1.0 &&
           ndc.z >= 0.0 && ndc.z <= 1.0;
}

/// @brief Tests whether a world-space position is contained within a cascade's frustum.
/// @param world_position World-space position to test.
/// @param view_proj The cascade's combined view-projection matrix.
/// @returns True if the world position projects inside the cascade frustum.
fn vsm_cascade_contains_world_position(world_position: vec3<f32>, view_proj: mat4x4<f32>) -> bool {
    return vsm_cascade_ndc_is_covered(vsm_project_world_to_cascade_ndc(world_position, view_proj));
}

/// @brief Returns whether the dirty bit is set in a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @returns True if the dirty bit is set.
fn get_is_dirty(entry: u32) -> bool {
    return (entry & VPT_DIRTY_BIT) != 0u;
}

/// @brief Returns whether the allocated bit is set in a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @returns True if the allocated bit is set.
fn get_is_allocated(entry: u32) -> bool {
    return (entry & VPT_ALLOCATED_BIT) != 0u;
}

/// @brief Extracts the physical page X coordinate from a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @returns The physical page X index.
fn get_page_x(entry: u32) -> u32 {
    return (entry >> VPT_PAGE_X_SHIFT) & 0xFFu;
}

/// @brief Extracts the physical page Y coordinate from a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @returns The physical page Y index.
fn get_page_y(entry: u32) -> u32 {
    return (entry >> VPT_PAGE_Y_SHIFT) & 0xFFu;
}

/// @brief Sets or clears the dirty bit in a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @param value True to set the dirty bit, false to clear it.
/// @returns The updated entry.
fn set_dirty(entry: u32, value: bool) -> u32 {
    return select(entry & ~VPT_DIRTY_BIT, entry | VPT_DIRTY_BIT, value);
}

/// @brief Sets or clears the allocated bit in a virtual page table entry.
/// @param entry The packed virtual page table entry.
/// @param value True to set the allocated bit, false to clear it.
/// @returns The updated entry.
fn set_allocated(entry: u32, value: bool) -> u32 {
    return select(entry & ~VPT_ALLOCATED_BIT, entry | VPT_ALLOCATED_BIT, value);
}

/// @brief Returns whether the visible bit is set in a page state entry.
/// @param entry The packed page state entry.
/// @returns True if the visible bit is set.
fn get_page_state_is_visible(entry: u32) -> bool {
    return (entry & VSM_PAGE_STATE_VISIBLE_BIT) != 0u;
}

/// @brief Returns whether the fallback bit is set in a page state entry.
/// @param entry The packed page state entry.
/// @returns True if the fallback bit is set.
fn get_page_state_is_fallback(entry: u32) -> bool {
    return (entry & VSM_PAGE_STATE_FALLBACK_BIT) != 0u;
}

/// @brief Sets or clears the visible bit in a page state entry.
/// @param entry The packed page state entry.
/// @param value True to set the visible bit, false to clear it.
/// @returns The updated entry.
fn set_page_state_visible(entry: u32, value: bool) -> u32 {
    return select(entry & ~VSM_PAGE_STATE_VISIBLE_BIT, entry | VSM_PAGE_STATE_VISIBLE_BIT, value);
}

/// @brief Sets or clears the fallback bit in a page state entry.
/// @param entry The packed page state entry.
/// @param value True to set the fallback bit, false to clear it.
/// @returns The updated entry.
fn set_page_state_fallback(entry: u32, value: bool) -> u32 {
    return select(entry & ~VSM_PAGE_STATE_FALLBACK_BIT, entry | VSM_PAGE_STATE_FALLBACK_BIT, value);
}

/// @brief Packs visible and fallback flags into a page state word.
/// @param visible Whether the page is currently visible.
/// @param fallback Whether the page is serving as a fallback.
/// @returns The packed page state word.
fn pack_page_state(visible: bool, fallback: bool) -> u32 {
    var entry = 0u;
    entry = select(entry, entry | VSM_PAGE_STATE_VISIBLE_BIT, visible);
    entry = select(entry, entry | VSM_PAGE_STATE_FALLBACK_BIT, fallback);
    return entry;
}

/// @brief Packs all fields into a virtual page table entry word.
/// @param dirty Whether the page is dirty.
/// @param visible Whether the page is visible (unused in current encoding but kept for parity).
/// @param allocated Whether the page is allocated.
/// @param fallback Whether the page is a fallback (unused in current encoding but kept for parity).
/// @param page_x Physical page X coordinate.
/// @param page_y Physical page Y coordinate.
/// @returns The packed virtual page table entry.
fn pack_vpt_entry(dirty: bool, visible: bool, allocated: bool, fallback: bool, page_x: u32, page_y: u32) -> u32 {
    var entry = 0u;
    entry = select(entry, entry | VPT_DIRTY_BIT, dirty);
    entry = select(entry, entry | VPT_ALLOCATED_BIT, allocated);
    entry = entry | (page_x << VPT_PAGE_X_SHIFT);
    entry = entry | (page_y << VPT_PAGE_Y_SHIFT);
    if (visible || fallback) {
    }
    return entry;
}

/// @brief Sets or clears the visible bit in a physical page table entry.
/// @param entry The packed physical page table entry.
/// @param value True to set the visible bit, false to clear it.
/// @returns The updated entry.
fn set_ppt_visible(entry: u32, value: bool) -> u32 {
    return select(entry & ~PPT_VISIBLE_BIT, entry | PPT_VISIBLE_BIT, value);
}

/// @brief Returns whether the allocated bit is set in a physical page table entry.
/// @param entry The packed physical page table entry.
/// @returns True if the allocated bit is set.
fn get_ppt_is_allocated(entry: u32) -> bool {
    return (entry & PPT_ALLOCATED_BIT) != 0u;
}

/// @brief Extracts the virtual page X coordinate from a physical page table entry.
/// @param entry The packed physical page table entry.
/// @returns The virtual page X coordinate.
fn get_ppt_vpt_x(entry: u32) -> u32 {
    return (entry >> PPT_VPT_X_SHIFT) & 0xFFu;
}

/// @brief Extracts the virtual page Y coordinate from a physical page table entry.
/// @param entry The packed physical page table entry.
/// @returns The virtual page Y coordinate.
fn get_ppt_vpt_y(entry: u32) -> u32 {
    return (entry >> PPT_VPT_Y_SHIFT) & 0xFFu;
}

/// @brief Extracts the cascade layer index from a physical page table entry.
/// @param entry The packed physical page table entry.
/// @returns The cascade layer index.
fn get_ppt_layer(entry: u32) -> u32 {
    return (entry >> PPT_LAYER_SHIFT) & 0xFFu;
}

/// @brief Sets or clears the allocated bit in a physical page table entry.
/// @param entry The packed physical page table entry.
/// @param value True to set the allocated bit, false to clear it.
/// @returns The updated entry.
fn set_ppt_allocated(entry: u32, value: bool) -> u32 {
    return select(entry & ~PPT_ALLOCATED_BIT, entry | PPT_ALLOCATED_BIT, value);
}

/// @brief Packs all fields into a physical page table entry word.
/// @param visible Whether the physical page is visible.
/// @param allocated Whether the physical page is allocated.
/// @param vpt_x The virtual page X coordinate this physical page is mapped to.
/// @param vpt_y The virtual page Y coordinate this physical page is mapped to.
/// @param layer The cascade layer this physical page belongs to.
/// @returns The packed physical page table entry.
fn pack_ppt_entry(visible: bool, allocated: bool, vpt_x: u32, vpt_y: u32, layer: u32) -> u32 {
    var entry = 0u;
    entry = select(entry, entry | PPT_VISIBLE_BIT, visible);
    entry = select(entry, entry | PPT_ALLOCATED_BIT, allocated);
    entry = entry | (vpt_x << PPT_VPT_X_SHIFT);
    entry = entry | (vpt_y << PPT_VPT_Y_SHIFT);
    entry = entry | (layer << PPT_LAYER_SHIFT);
    return entry;
}

/// @brief Packs X, Y, and layer coordinates into a single 32-bit word.
/// @param x The X coordinate (8-bit).
/// @param y The Y coordinate (8-bit).
/// @param layer The layer index (8-bit).
/// @returns The packed coordinate word.
fn pack_layered_coords(x: u32, y: u32, layer: u32) -> u32 {
    return (x & 0xFFu) | ((y & 0xFFu) << 8u) | ((layer & 0xFFu) << 16u);
}

/// @brief Unpacks a layered coordinate word into X, Y, and layer components.
/// @param value The packed coordinate word.
/// @returns A vec3 with components (x, y, layer).
fn unpack_layered_coords(value: u32) -> vec3<u32> {
    return vec3<u32>(value & 0xFFu, (value >> 8u) & 0xFFu, (value >> 16u) & 0xFFu);
}

/// @brief Packs X and Y physical atlas coordinates into a single 32-bit word.
/// @param x The X coordinate (16-bit).
/// @param y The Y coordinate (16-bit).
/// @returns The packed coordinate word.
fn pack_physical_coords(x: u32, y: u32) -> u32 {
    return (x & 0xFFFFu) | ((y & 0xFFFFu) << 16u);
}

/// @brief Unpacks a physical atlas coordinate word into X and Y components.
/// @param value The packed coordinate word.
/// @returns A vec2 with components (x, y).
fn unpack_physical_coords(value: u32) -> vec2<u32> {
    return vec2<u32>(value & 0xFFFFu, (value >> 16u) & 0xFFFFu);
}

/// @brief Determines whether a page at given coordinates falls inside the region that must be cleared due to cascade scrolling.
/// @param page_coords The integer page coordinates to test.
/// @param cascade_shift The integer shift applied to the cascade this frame.
/// @param page_table_resolution The resolution (in pages) of the page table.
/// @returns True if the page is in the wrap region that must be invalidated.
fn should_clear_wrap_region(page_coords: vec2<i32>, cascade_shift: vec2<i32>, page_table_resolution: i32) -> bool {
    if (abs(cascade_shift.x) >= page_table_resolution || abs(cascade_shift.y) >= page_table_resolution) {
        return true;
    }

    let should_clear_wrap_x =
        (cascade_shift.x > 0 && page_coords.x >= page_table_resolution - cascade_shift.x) ||
        (cascade_shift.x < 0 && page_coords.x < -cascade_shift.x);

    let should_clear_wrap_y =
        (cascade_shift.y > 0 && page_coords.y >= page_table_resolution - cascade_shift.y) ||
        (cascade_shift.y < 0 && page_coords.y < -cascade_shift.y);

    return should_clear_wrap_x || should_clear_wrap_y;
}

/// @brief Extracts the bit corresponding to the given page X coordinate from a row bitmask.
/// @param page_coords The page coordinates; only the X component is used as the bit index.
/// @param bitmask The 32-bit row bitmask to test.
/// @returns True if the bit for that page column is set.
fn extract_page_bit_from_mask(page_coords: vec2<u32>, bitmask: u32) -> bool {
    return ((bitmask >> page_coords.x) & 1u) != 0u;
}

/// @brief Applies a toroidal (wrapped) offset to virtual page coordinates.
/// @param page_coords The linear virtual page coordinates.
/// @param cascade_offset The toroidal offset of the cascade origin in page space.
/// @param page_table_resolution The resolution (in pages) of the page table.
/// @returns The wrapped physical page coordinates, or (-1, -1) if out of bounds.
fn virtual_page_coords_to_wrapped_coords(page_coords: vec2<i32>, cascade_offset: vec2<i32>, page_table_resolution: i32) -> vec2<i32> {
    if (page_coords.x < 0 || page_coords.y < 0 || page_coords.x >= page_table_resolution || page_coords.y >= page_table_resolution) {
        return vec2<i32>(-1, -1);
    }

    let offset_page_coords = page_coords + cascade_offset;
    let wrapped_x = ((offset_page_coords.x % page_table_resolution) + page_table_resolution) % page_table_resolution;
    let wrapped_y = ((offset_page_coords.y % page_table_resolution) + page_table_resolution) % page_table_resolution;
    return vec2<i32>(wrapped_x, wrapped_y);
}

/// @brief Computes the flat buffer index for a virtual page given its layer and page coordinates.
/// @param layer The cascade layer index.
/// @param page_coords The 2D page coordinates within the layer.
/// @param page_table_resolution The resolution (in pages) of the page table per layer.
/// @returns The flat array index for the specified virtual page.
fn vsm_page_table_index(layer: u32, page_coords: vec2<u32>, page_table_resolution: u32) -> u32 {
    let pages_per_layer = page_table_resolution * page_table_resolution;
    return layer * pages_per_layer + page_coords.y * page_table_resolution + page_coords.x;
}

/// @brief Converts a virtual UV coordinate to a page grid sample including the in-page fractional UV.
/// @param uv The virtual texture UV coordinate in [0, 1].
/// @param page_table_resolution The resolution (in pages) of the page table.
/// @returns A VSMVirtualPageSample containing the integer page coordinates and the fractional UV within that page.
fn vsm_virtual_uv_to_page_sample(uv: vec2<f32>, page_table_resolution: u32) -> VSMVirtualPageSample {
    let clamped_uv = clamp(uv, vec2<f32>(0.0, 0.0), vec2<f32>(0.99999994, 0.99999994));
    let page_space = clamped_uv * f32(max(page_table_resolution, 1u));
    return VSMVirtualPageSample(
        vec2<u32>(floor(page_space)),
        fract(page_space));
}

/// @brief Projects a world position into the wrapped page table and returns page coords and local UV.
/// @param world_position The world-space position to project.
/// @param view_proj The cascade's combined view-projection matrix.
/// @param page_offset The toroidal page offset of the cascade.
/// @param page_table_resolution The resolution (in pages) of the page table.
/// @param wrapped_page_out Output: the resulting wrapped page coordinates.
/// @param page_local_uv_out Output: the fractional UV within the resolved page.
/// @param ndc_out Output: the NDC coordinate of the projected position.
/// @returns True if the projection succeeded and the position is covered by the cascade.
fn vsm_try_project_world_to_wrapped_page_sample(
    world_position: vec3<f32>,
    view_proj: mat4x4<f32>,
    page_offset: vec2<i32>,
    page_table_resolution: u32,
    wrapped_page_out: ptr<function, vec2<u32>>,
    page_local_uv_out: ptr<function, vec2<f32>>,
    ndc_out: ptr<function, vec3<f32>>
) -> bool {
    let ndc = vsm_project_world_to_cascade_ndc(world_position, view_proj);
    (*ndc_out) = ndc;
    if (!vsm_cascade_ndc_is_covered(ndc)) {
        return false;
    }

    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let virtual_sample = vsm_virtual_uv_to_page_sample(uv, page_table_resolution);
    let wrapped_page = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(i32(virtual_sample.pageCoords.x), i32(virtual_sample.pageCoords.y)),
        page_offset,
        i32(page_table_resolution));
    if (wrapped_page.x < 0 || wrapped_page.y < 0) {
        return false;
    }

    (*wrapped_page_out) = vec2<u32>(u32(wrapped_page.x), u32(wrapped_page.y));
    (*page_local_uv_out) = virtual_sample.pageLocalUV;
    return true;
}

/// @brief Contracts a page-local UV inward by one texel to avoid sampling across page boundaries during filtering.
/// @param page_local_uv The fractional UV within a physical page in [0, 1].
/// @param physical_page_size The size of a physical page in texels.
/// @returns The inset UV safe for bilinear filtering within the page.
fn vsm_contract_page_local_uv_for_filtering(page_local_uv: vec2<f32>, physical_page_size: u32) -> vec2<f32> {
    let inset = 1.0 / f32(max(physical_page_size, 1u));
    return clamp(page_local_uv, vec2<f32>(inset, inset), vec2<f32>(1.0 - inset, 1.0 - inset));
}

/// @brief Converts a physical page coordinate and in-page UV to an atlas pixel position.
/// @param physical_page_coords The 2D coordinates of the physical page within the atlas.
/// @param page_local_uv The fractional UV within the physical page.
/// @param physical_page_size The size of a physical page in texels.
/// @returns The continuous pixel position within the shadow atlas texture.
fn vsm_physical_page_and_local_uv_to_atlas_pixel(physical_page_coords: vec2<u32>, page_local_uv: vec2<f32>, physical_page_size: u32) -> vec2<f32> {
    let page_size = f32(max(physical_page_size, 1u));
    let page_origin = vec2<f32>(physical_page_coords) * page_size;
    return page_origin + page_local_uv * page_size - vec2<f32>(0.5, 0.5);
}

/// @brief Compares a shadow map depth sample against a receiver depth with an optional bias.
/// @param shadow_depth The depth value stored in the shadow atlas.
/// @param receiver_depth The projected depth of the shadow receiver.
/// @param bias The depth bias to apply to reduce self-shadowing.
/// @param reverse_z True if the depth buffer uses reversed Z (1 = near, 0 = far).
/// @returns 1.0 if the receiver is lit, 0.0 if it is in shadow.
fn vsm_shadow_compare(shadow_depth: f32, receiver_depth: f32, bias: f32, reverse_z: bool) -> f32 {
    let lit_forward = receiver_depth - bias <= shadow_depth;
    let lit_reverse = receiver_depth + bias >= shadow_depth;
    return select(0.0, 1.0, select(lit_forward, lit_reverse, reverse_z));
}

/// @brief Measures how far the receiver lies behind the resolved shadow depth along the light direction.
/// @param shadow_depth The depth value stored in the shadow atlas.
/// @param receiver_depth The projected depth of the shadow receiver.
/// @param reverse_z True if the depth buffer uses reversed Z (1 = near, 0 = far).
/// @returns A positive occluder separation when the receiver is behind the stored depth, otherwise 0.
fn vsm_shadow_occluder_delta(shadow_depth: f32, receiver_depth: f32, reverse_z: bool) -> f32 {
    let forward_delta = receiver_depth - shadow_depth;
    let reverse_delta = shadow_depth - receiver_depth;
    return max(select(forward_delta, reverse_delta, reverse_z), 0.0);
}

/// @brief Loads a single depth value from the shadow atlas at the given texel.
/// @param shadow_atlas The shadow atlas texture.
/// @param atlas_texel The integer texel coordinate within the shadow atlas.
/// @returns The shadow depth value at that texel.
fn vsm_load_shadow_depth(shadow_atlas: texture_depth_2d, atlas_texel: vec2<i32>) -> f32 {
    return textureLoad(shadow_atlas, atlas_texel, 0);
}

/// @brief Performs a single-tap shadow test at the given atlas texel.
/// @param shadow_atlas The shadow atlas texture.
/// @param atlas_texel The integer texel coordinate within the shadow atlas.
/// @param receiver_depth The projected depth of the shadow receiver.
/// @param bias The depth bias to apply.
/// @param reverse_z True if the depth buffer uses reversed Z.
/// @returns 1.0 if the receiver is lit, 0.0 if in shadow.
fn vsm_sample_shadow_texture_1tap(
    shadow_atlas: texture_depth_2d,
    atlas_texel: vec2<i32>,
    receiver_depth: f32,
    bias: f32,
    reverse_z: bool
) -> f32 {
    let shadow_depth = vsm_load_shadow_depth(shadow_atlas, atlas_texel);
    return vsm_shadow_compare(shadow_depth, receiver_depth, bias, reverse_z);
}

/// @brief Performs a 4-tap bilinear-filtered PCF shadow test over a 2x2 texel footprint.
/// @param shadow_atlas The shadow atlas texture.
/// @param atlas_pixel The continuous pixel position within the shadow atlas.
/// @param atlas_resolution The resolution of the shadow atlas texture.
/// @param receiver_depth The projected depth of the shadow receiver.
/// @param bias The depth bias to apply.
/// @param reverse_z True if the depth buffer uses reversed Z.
/// @returns A bilinearly interpolated shadow visibility in [0, 1].
fn vsm_sample_shadow_texture_bilinear_pcf_4tap(
    shadow_atlas: texture_depth_2d,
    atlas_pixel: vec2<f32>,
    atlas_resolution: vec2<u32>,
    receiver_depth: f32,
    bias: f32,
    reverse_z: bool
) -> f32 {
    let safe_resolution = vec2<i32>(max(atlas_resolution, vec2<u32>(1u, 1u)));
    let base = vec2<i32>(floor(atlas_pixel));
    let frac = fract(atlas_pixel);

    let p00 = clamp(base, vec2<i32>(0, 0), safe_resolution - vec2<i32>(1, 1));
    let p10 = clamp(base + vec2<i32>(1, 0), vec2<i32>(0, 0), safe_resolution - vec2<i32>(1, 1));
    let p01 = clamp(base + vec2<i32>(0, 1), vec2<i32>(0, 0), safe_resolution - vec2<i32>(1, 1));
    let p11 = clamp(base + vec2<i32>(1, 1), vec2<i32>(0, 0), safe_resolution - vec2<i32>(1, 1));

    let s00 = vsm_sample_shadow_texture_1tap(shadow_atlas, p00, receiver_depth, bias, reverse_z);
    let s10 = vsm_sample_shadow_texture_1tap(shadow_atlas, p10, receiver_depth, bias, reverse_z);
    let s01 = vsm_sample_shadow_texture_1tap(shadow_atlas, p01, receiver_depth, bias, reverse_z);
    let s11 = vsm_sample_shadow_texture_1tap(shadow_atlas, p11, receiver_depth, bias, reverse_z);

    let sx0 = mix(s00, s10, frac.x);
    let sx1 = mix(s01, s11, frac.x);
    return mix(sx0, sx1, frac.y);
}

/// @brief Performs a cheap 4-tap PCF shadow sample equivalent to the bilinear variant.
/// @param shadow_atlas The shadow atlas texture.
/// @param atlas_pixel The continuous pixel position within the shadow atlas.
/// @param atlas_resolution The resolution of the shadow atlas texture.
/// @param receiver_depth The projected depth of the shadow receiver.
/// @param bias The depth bias to apply.
/// @param reverse_z True if the depth buffer uses reversed Z.
/// @returns A bilinearly interpolated shadow visibility in [0, 1].
fn vsm_sample_shadow_texture_cheap_4tap(
    shadow_atlas: texture_depth_2d,
    atlas_pixel: vec2<f32>,
    atlas_resolution: vec2<u32>,
    receiver_depth: f32,
    bias: f32,
    reverse_z: bool
) -> f32 {
    return vsm_sample_shadow_texture_bilinear_pcf_4tap(
        shadow_atlas,
        atlas_pixel,
        atlas_resolution,
        receiver_depth,
        bias,
        reverse_z);
}
