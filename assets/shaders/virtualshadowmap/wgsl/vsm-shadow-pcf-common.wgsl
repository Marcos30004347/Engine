#include "vsm-common.wgsl"

struct ShadowPcfUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    cascadeCount: u32,
    activeDirectionalLights: u32,
    enabled: u32,
    physicalPageSize: u32,
    reverseZ: u32,
    fallbackCascadeOffset: u32,
    firstCascadeWorldExtent: f32,
    shadowBias: f32,
    slopeScaleBias: f32,
    maxShadowBias: f32,
    pcfRadiusTexels: f32,
    normalBiasTexels: f32,
    contactShadowDistance: f32,
    contactShadowThickness: f32,
    ambientShadowColor: vec3<f32>,
    contactShadowIntensity: f32,
    shadowFilterTaps: u32,
    contactShadowSamples: u32,
    screenSpaceShadowEnabled: u32,
    contactShadowStartBias: f32,
    _padding0: vec2<f32>,
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
    viewProjection: mat4x4<f32>,
    cameraPosition: vec4<f32>,
    reverseZ: u32,
    _padding: vec3<u32>,
}

struct DirectionalLight {
    direction: vec4<f32>,
    color: vec4<f32>,
}

struct ReceiverState {
    valid: u32,
    resolvedLayer: u32,
    receiverDepth: f32,
    effectiveBias: f32,
    virtualTexel: vec2<f32>,
}

@group(0) @binding(0) var<uniform> uniforms: ShadowPcfUniforms;
@group(0) @binding(1) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(2) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(3) var<storage, read> cameraState: CameraState;
@group(0) @binding(4) var<storage, read> directionalLights: array<DirectionalLight>;
@group(0) @binding(5) var depthTexture: texture_depth_2d;
@group(0) @binding(6) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(7) var shadowAtlasTexture: texture_2d<f32>;
@group(0) @binding(8) var screenSpaceShadowTexture: texture_2d<f32>;
@group(0) @binding(9) var outputTexture: texture_storage_2d<rgba16float, write>;

fn reconstruct_world_position(pixel_coord: vec2<u32>) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    let world = cameraState.inverseViewProjection * ndc;
    return world.xyz / max(world.w, 1e-6);
}

fn try_reconstruct_world_position(pixel_coord: vec2<i32>, world_position_out: ptr<function, vec3<f32>>) -> bool {
    let dims = vec2<i32>(textureDimensions(depthTexture));
    if (pixel_coord.x < 0 || pixel_coord.y < 0 || pixel_coord.x >= dims.x || pixel_coord.y >= dims.y) {
        return false;
    }

    let depth = textureLoad(depthTexture, pixel_coord, 0);
    if (depth <= 0.0) {
        return false;
    }

    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(dims);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depth, 1.0);
    let world = cameraState.inverseViewProjection * ndc;
    (*world_position_out) = world.xyz / max(world.w, 1e-6);
    return true;
}

fn project_world_to_camera_pixel(
    world_position: vec3<f32>,
    pixel_coord_out: ptr<function, vec2<i32>>
) -> bool {
    let clip = cameraState.viewProjection * vec4<f32>(world_position, 1.0);
    if (abs(clip.w) <= 1e-6) {
        return false;
    }

    let ndc = clip.xyz / clip.w;
    if (ndc.x < -1.0 || ndc.x > 1.0 || ndc.y < -1.0 || ndc.y > 1.0 || ndc.z < 0.0 || ndc.z > 1.0) {
        return false;
    }

    let dims = vec2<i32>(textureDimensions(depthTexture));
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    let pixel_f = clamp(uv * vec2<f32>(dims), vec2<f32>(0.0), vec2<f32>(dims) - vec2<f32>(1.0));
    (*pixel_coord_out) = vec2<i32>(pixel_f);
    return true;
}

fn interleaved_gradient_noise(pixel_coord: vec2<u32>) -> f32 {
    let seed = dot(vec2<f32>(pixel_coord), vec2<f32>(0.06711056, 0.00583715));
    return fract(52.9829189 * fract(seed));
}

fn evaluate_contact_shadow_candidate(
    candidate_pixel: vec2<i32>,
    ray_origin: vec3<f32>,
    ray_direction: vec3<f32>,
    ray_distance: f32,
    perpendicular_thickness: f32,
    segment_half_length: f32
) -> bool {
    var scene_world = vec3<f32>(0.0, 0.0, 0.0);
    if (!try_reconstruct_world_position(candidate_pixel, &scene_world)) {
        return false;
    }

    let receiver_to_scene = scene_world - ray_origin;
    let projected_distance = dot(receiver_to_scene, ray_direction);
    if (projected_distance <= 0.0) {
        return false;
    }

    let perpendicular_vector = receiver_to_scene - ray_direction * projected_distance;
    let perpendicular_distance = length(perpendicular_vector);
    if (perpendicular_distance > perpendicular_thickness) {
        return false;
    }

    let axial_distance = abs(projected_distance - ray_distance);
    return axial_distance <= segment_half_length;
}

fn shadow_texel_world_size(layer: u32) -> f32 {
    return cascadeMatrices[layer].pageWorldSize / f32(max(uniforms.physicalPageSize, 1u));
}

fn receiver_neighbor_is_compatible(
    neighbor_world_position: vec3<f32>,
    world_position: vec3<f32>,
    layer: u32
) -> bool {
    let texel_world_size = shadow_texel_world_size(layer);
    let max_distance = max(texel_world_size * 6.0, 1e-3);
    return distance(neighbor_world_position, world_position) <= max_distance;
}

fn try_reconstruct_receiver_normal(
    pixel_coord: vec2<i32>,
    layer: u32,
    world_position: vec3<f32>,
    normal_out: ptr<function, vec3<f32>>
) -> bool {
    var right_world = vec3<f32>(0.0, 0.0, 0.0);
    let has_right = try_reconstruct_world_position(pixel_coord + vec2<i32>(1, 0), &right_world) &&
        receiver_neighbor_is_compatible(right_world, world_position, layer);
    var left_world = vec3<f32>(0.0, 0.0, 0.0);
    let has_left = try_reconstruct_world_position(pixel_coord + vec2<i32>(-1, 0), &left_world) &&
        receiver_neighbor_is_compatible(left_world, world_position, layer);
    var down_world = vec3<f32>(0.0, 0.0, 0.0);
    let has_down = try_reconstruct_world_position(pixel_coord + vec2<i32>(0, 1), &down_world) &&
        receiver_neighbor_is_compatible(down_world, world_position, layer);
    var up_world = vec3<f32>(0.0, 0.0, 0.0);
    let has_up = try_reconstruct_world_position(pixel_coord + vec2<i32>(0, -1), &up_world) &&
        receiver_neighbor_is_compatible(up_world, world_position, layer);

    var tangent_x = vec3<f32>(0.0, 0.0, 0.0);
    if (has_right && has_left) {
        tangent_x = right_world - left_world;
    } else if (has_right) {
        tangent_x = right_world - world_position;
    } else if (has_left) {
        tangent_x = world_position - left_world;
    } else {
        return false;
    }

    var tangent_y = vec3<f32>(0.0, 0.0, 0.0);
    if (has_down && has_up) {
        tangent_y = down_world - up_world;
    } else if (has_down) {
        tangent_y = down_world - world_position;
    } else if (has_up) {
        tangent_y = world_position - up_world;
    } else {
        return false;
    }

    let normal = cross(tangent_x, tangent_y);
    let normal_length_sq = dot(normal, normal);
    if (normal_length_sq <= 1e-8) {
        return false;
    }

    var resolved_normal = normal / sqrt(normal_length_sq);
    let view_direction = cameraState.cameraPosition.xyz - world_position;
    if (dot(resolved_normal, view_direction) < 0.0) {
        resolved_normal = -resolved_normal;
    }
    (*normal_out) = resolved_normal;
    return true;
}

fn select_cascade_level(world_position: vec3<f32>) -> u32 {
    return vsm_select_preferred_clipmap_level(
        world_position,
        cameraState.cameraPosition.xyz,
        uniforms.firstCascadeWorldExtent,
        uniforms.cascadeCount);
}

fn cascade_contains_world_position(world_position: vec3<f32>, layer: u32) -> bool {
    return vsm_cascade_contains_world_position(world_position, cascadeMatrices[layer].viewProj);
}

fn find_covering_cascade_level_for_light(world_position: vec3<f32>, light_index: u32, preferred_cascade: u32) -> i32 {
    let base_layer = light_index * uniforms.cascadeCount;
    if (base_layer >= uniforms.activeLayers) {
        return -1;
    }

    let available_cascades = min(uniforms.cascadeCount, uniforms.activeLayers - base_layer);
    var cascade = preferred_cascade;
    loop {
        if (cascade >= available_cascades) {
            break;
        }

        let layer = base_layer + cascade;
        if (cascade_contains_world_position(world_position, layer)) {
            return i32(layer);
        }

        cascade = cascade + 1u;
    }

    return -1;
}

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

fn try_project_world_to_shadow_virtual_texel(
    world_position: vec3<f32>,
    layer: u32,
    receiver_depth_out: ptr<function, f32>,
    virtual_texel_out: ptr<function, vec2<f32>>
) -> bool {
    let ndc = vsm_project_world_to_cascade_ndc(world_position, cascadeMatrices[layer].viewProj);
    if (!vsm_cascade_ndc_is_covered(ndc)) {
        return false;
    }

    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let virtual_resolution = f32(max(uniforms.pageTableResolution * uniforms.physicalPageSize, 1u));
    (*receiver_depth_out) = ndc.z;
    (*virtual_texel_out) = uv * vec2<f32>(virtual_resolution, virtual_resolution);
    return true;
}

fn try_resolve_shadow_texel(
    layer: u32,
    virtual_texel: vec2<f32>,
    atlas_pixel_out: ptr<function, vec2<f32>>,
    stored_depth_out: ptr<function, f32>,
    atlas_texel_out: ptr<function, vec2<i32>>
) -> bool {
    let virtual_resolution_u = max(uniforms.pageTableResolution * uniforms.physicalPageSize, 1u);
    let virtual_resolution = vec2<f32>(f32(virtual_resolution_u), f32(virtual_resolution_u));
    if (virtual_texel.x < 0.0 || virtual_texel.y < 0.0 ||
        virtual_texel.x >= virtual_resolution.x || virtual_texel.y >= virtual_resolution.y) {
        return false;
    }

    let uv = clamp(virtual_texel / virtual_resolution, vec2<f32>(0.0, 0.0), vec2<f32>(0.99999994, 0.99999994));
    let virtual_sample = vsm_virtual_uv_to_page_sample(uv, uniforms.pageTableResolution);
    let wrapped_page_i = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(i32(virtual_sample.pageCoords.x), i32(virtual_sample.pageCoords.y)),
        cascadeStates[layer].pageOffset,
        i32(uniforms.pageTableResolution));
    if (wrapped_page_i.x < 0 || wrapped_page_i.y < 0) {
        return false;
    }

    let wrapped_page = vec2<u32>(u32(wrapped_page_i.x), u32(wrapped_page_i.y));
    let vpt_entry = virtualPageTable[vsm_page_table_index(layer, wrapped_page, uniforms.pageTableResolution)];
    if (!get_is_allocated(vpt_entry) || get_is_dirty(vpt_entry)) {
        return false;
    }

    let contracted_local_uv = vsm_contract_page_local_uv_for_filtering(virtual_sample.pageLocalUV, uniforms.physicalPageSize);
    let atlas_pixel = vsm_physical_page_and_local_uv_to_atlas_pixel(
        vec2<u32>(get_page_x(vpt_entry), get_page_y(vpt_entry)),
        contracted_local_uv,
        uniforms.physicalPageSize);
    (*atlas_pixel_out) = atlas_pixel;
    let atlas_resolution = vec2<i32>(max(textureDimensions(shadowAtlasTexture), vec2<u32>(1u, 1u)));
    (*atlas_texel_out) = clamp(vec2<i32>(floor(atlas_pixel)), vec2<i32>(0, 0), atlas_resolution - vec2<i32>(1, 1));
    (*stored_depth_out) = vsm_load_shadow_depth(shadowAtlasTexture, *atlas_texel_out);
    return true;
}

fn estimate_neighbor_shadow_slope(
    neighbor_pixel_coord: vec2<i32>,
    layer: u32,
    receiver_depth: f32,
    virtual_texel: vec2<f32>
) -> f32 {
    var neighbor_world_position = vec3<f32>(0.0, 0.0, 0.0);
    if (!try_reconstruct_world_position(neighbor_pixel_coord, &neighbor_world_position)) {
        return 0.0;
    }

    var neighbor_receiver_depth = 0.0;
    var neighbor_virtual_texel = vec2<f32>(0.0, 0.0);
    if (!try_project_world_to_shadow_virtual_texel(neighbor_world_position, layer, &neighbor_receiver_depth, &neighbor_virtual_texel)) {
        return 0.0;
    }

    let texel_delta = length(neighbor_virtual_texel - virtual_texel);
    if (texel_delta <= 1e-4) {
        return 0.0;
    }

    return abs(neighbor_receiver_depth - receiver_depth) / texel_delta;
}

fn compute_effective_shadow_bias(
    pixel_coord: vec2<u32>,
    receiver_depth: f32,
    resolved_layer: u32,
    virtual_texel: vec2<f32>
) -> f32 {
    let pixel = vec2<i32>(pixel_coord);
    var slope = 0.0;
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(1, 0), resolved_layer, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(-1, 0), resolved_layer, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(0, 1), resolved_layer, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(0, -1), resolved_layer, receiver_depth, virtual_texel));

    // `slope` is already measured in shadow-depth units per shadow texel, so the
    // scale factor should stay in texel space instead of being damped by world size.
    var bias = max(uniforms.shadowBias, 0.0) + slope * max(uniforms.slopeScaleBias, 0.0);
    if (uniforms.maxShadowBias > 0.0) {
        bias = min(bias, uniforms.maxShadowBias);
    }
    return max(bias, 0.0);
}

fn compute_normal_bias_depth(
    pixel_coord: vec2<u32>,
    world_position: vec3<f32>,
    layer: u32,
    receiver_depth: f32
) -> f32 {
    let normal_bias_texels = max(uniforms.normalBiasTexels, 0.0);
    if (normal_bias_texels <= 1e-4) {
        return 0.0;
    }

    var receiver_normal = vec3<f32>(0.0, 0.0, 0.0);
    if (!try_reconstruct_receiver_normal(vec2<i32>(pixel_coord), layer, world_position, &receiver_normal)) {
        return 0.0;
    }

    let light_index = cascadeMatrices[layer].lightIndex;
    if (light_index >= uniforms.activeDirectionalLights) {
        return 0.0;
    }

    let to_light = normalize(-directionalLights[light_index].direction.xyz);
    let cos_angle = clamp(1.0 - dot(to_light, receiver_normal), 0.0, 1.0);
    let normal_bias_world = normal_bias_texels * cos_angle * shadow_texel_world_size(layer);
    if (normal_bias_world <= 1e-6) {
        return 0.0;
    }

    var plus_receiver_depth = 0.0;
    var plus_virtual_texel = vec2<f32>(0.0, 0.0);
    let plus_valid = try_project_world_to_shadow_virtual_texel(
        world_position + receiver_normal * normal_bias_world,
        layer,
        &plus_receiver_depth,
        &plus_virtual_texel);

    var minus_receiver_depth = 0.0;
    var minus_virtual_texel = vec2<f32>(0.0, 0.0);
    let minus_valid = try_project_world_to_shadow_virtual_texel(
        world_position - receiver_normal * normal_bias_world,
        layer,
        &minus_receiver_depth,
        &minus_virtual_texel);

    if (plus_valid && (!minus_valid || select(plus_receiver_depth < minus_receiver_depth, plus_receiver_depth > minus_receiver_depth, uniforms.reverseZ != 0u))) {
        return abs(plus_receiver_depth - receiver_depth);
    }
    if (minus_valid) {
        return abs(minus_receiver_depth - receiver_depth);
    }

    return 0.0;
}

fn resolve_receiver_state_for_light(
    pixel_coord: vec2<u32>,
    world_position: vec3<f32>,
    light_index: u32
) -> ReceiverState {
    var state = ReceiverState(0u, 0u, 0.0, 0.0, vec2<f32>(0.0, 0.0));
    if (uniforms.enabled == 0u || uniforms.activeLayers == 0u || light_index >= uniforms.activeDirectionalLights) {
        return state;
    }

    let preferred_cascade = select_cascade_level(world_position);
    let cascade_level = find_covering_cascade_level_for_light(world_position, light_index, preferred_cascade);
    if (cascade_level < 0) {
        return state;
    }

    let layer = u32(cascade_level);
    var receiver_depth = 0.0;
    var virtual_texel = vec2<f32>(0.0, 0.0);
    if (!try_project_world_to_shadow_virtual_texel(world_position, layer, &receiver_depth, &virtual_texel)) {
        return state;
    }

    var resolved_layer = layer;
    var atlas_pixel = vec2<f32>(0.0, 0.0);
    var stored_depth = 0.0;
    var atlas_texel = vec2<i32>(0, 0);
    if (!try_resolve_shadow_texel(layer, virtual_texel, &atlas_pixel, &stored_depth, &atlas_texel)) {
        let fallback_layer = resolve_fallback_layer(layer);
        if (fallback_layer != 0xFFFFFFFFu) {
            var fallback_receiver_depth = 0.0;
            var fallback_virtual_texel = vec2<f32>(0.0, 0.0);
            if (try_project_world_to_shadow_virtual_texel(world_position, fallback_layer, &fallback_receiver_depth, &fallback_virtual_texel)) {
                receiver_depth = fallback_receiver_depth;
                virtual_texel = fallback_virtual_texel;
                resolved_layer = fallback_layer;
            }
        }
    }

    let base_bias = compute_effective_shadow_bias(pixel_coord, receiver_depth, resolved_layer, virtual_texel);
    let normal_bias = compute_normal_bias_depth(pixel_coord, world_position, resolved_layer, receiver_depth);
    // Constant/slope bias and projected normal bias are both trying to solve the
    // same self-shadowing problem. Summing them tends to over-separate the shadow.
    var effective_bias = max(base_bias, normal_bias);
    if (uniforms.maxShadowBias > 0.0) {
        effective_bias = min(effective_bias, uniforms.maxShadowBias);
    }
    state.valid = 1u;
    state.resolvedLayer = resolved_layer;
    state.receiverDepth = receiver_depth;
    state.effectiveBias = effective_bias;
    state.virtualTexel = virtual_texel;
    return state;
}

fn sample_shadow_test_current_texel(
    layer: u32,
    virtual_texel: vec2<f32>,
    receiver_depth: f32,
    bias: f32
) -> f32 {
    var atlas_pixel = vec2<f32>(0.0, 0.0);
    var stored_depth = 0.0;
    var atlas_texel = vec2<i32>(0, 0);
    if (!try_resolve_shadow_texel(layer, virtual_texel, &atlas_pixel, &stored_depth, &atlas_texel)) {
        return 1.0;
    }
    _ = atlas_pixel;
    _ = atlas_texel;
    return vsm_shadow_compare(stored_depth, receiver_depth, bias, uniforms.reverseZ != 0u);
}

fn sample_shadow_test_bilinear(
    layer: u32,
    virtual_texel: vec2<f32>,
    receiver_depth: f32,
    bias: f32
) -> f32 {
    var atlas_pixel = vec2<f32>(0.0, 0.0);
    var stored_depth = 0.0;
    var atlas_texel = vec2<i32>(0, 0);
    if (!try_resolve_shadow_texel(layer, virtual_texel, &atlas_pixel, &stored_depth, &atlas_texel)) {
        return 1.0;
    }
    _ = stored_depth;
    _ = atlas_texel;
    return vsm_sample_shadow_texture_bilinear_pcf_4tap(
        shadowAtlasTexture,
        atlas_pixel,
        textureDimensions(shadowAtlasTexture),
        receiver_depth,
        bias,
        uniforms.reverseZ != 0u);
}

fn evaluate_pcf_grid(receiver: ReceiverState, grid_size: u32, filter_radius: f32) -> f32 {
    if (grid_size <= 1u || filter_radius <= 1e-4) {
        return sample_shadow_test_current_texel(
            receiver.resolvedLayer,
            receiver.virtualTexel,
            receiver.receiverDepth,
            receiver.effectiveBias);
    }

    let stride = (2.0 * filter_radius) / f32(grid_size - 1u);
    let start = -filter_radius;
    var total = 0.0;
    var count = 0u;
    var y = 0u;
    loop {
        if (y >= grid_size) {
            break;
        }

        var x = 0u;
        loop {
            if (x >= grid_size) {
                break;
            }

            let offset = vec2<f32>(start + stride * f32(x), start + stride * f32(y));
            total = total + sample_shadow_test_bilinear(
                receiver.resolvedLayer,
                receiver.virtualTexel + offset,
                receiver.receiverDepth,
                receiver.effectiveBias);
            count = count + 1u;
            x = x + 1u;
        }

        y = y + 1u;
    }

    return total / f32(max(count, 1u));
}

fn evaluate_pcf(receiver: ReceiverState) -> f32 {
    let tap_count = max(uniforms.shadowFilterTaps, 1u);
    let filter_radius = max(uniforms.pcfRadiusTexels, 0.0);
    switch tap_count {
        case 1u: {
            return sample_shadow_test_current_texel(
                receiver.resolvedLayer,
                receiver.virtualTexel,
                receiver.receiverDepth,
                receiver.effectiveBias);
        }
        case 4u: {
            return evaluate_pcf_grid(receiver, 2u, filter_radius);
        }
        case 9u: {
            return evaluate_pcf_grid(receiver, 3u, filter_radius);
        }
        case 16u: {
            return evaluate_pcf_grid(receiver, 4u, filter_radius);
        }
        default: {
            return evaluate_pcf_grid(receiver, 2u, filter_radius);
        }
    }
}

fn evaluate_contact_shadow(
    pixel_coord: vec2<u32>,
    world_position: vec3<f32>,
    receiver: ReceiverState,
    light_index: u32
) -> f32 {
    let sample_count = uniforms.contactShadowSamples;
    let intensity = clamp(uniforms.contactShadowIntensity, 0.0, 1.0);
    if (sample_count == 0u || intensity <= 1e-4 || light_index >= uniforms.activeDirectionalLights || receiver.valid == 0u) {
        return 1.0;
    }

    // Contact shadows are only meant to recover very local detail near the camera.
    // Restricting them to the finest cascade keeps the effect focused and avoids
    // paying the full march cost for far-field pixels.
    if (cascadeMatrices[receiver.resolvedLayer].cascadeIndex != 0u) {
        return 1.0;
    }

    let texel_world_size = shadow_texel_world_size(receiver.resolvedLayer);
    let max_distance = max(uniforms.contactShadowDistance, 0.0) * texel_world_size;
    let thickness = max(max(uniforms.contactShadowThickness, 0.5) * texel_world_size, texel_world_size * 1.5);
    let start_bias = max(uniforms.contactShadowStartBias, 0.0) * texel_world_size;
    if (max_distance <= 1e-5) {
        return 1.0;
    }

    let light_direction = normalize(directionalLights[light_index].direction.xyz);
    let ray_direction = -light_direction;
    let step_size = max_distance / f32(sample_count);

    var receiver_normal = vec3<f32>(0.0, 0.0, 0.0);
    let has_receiver_normal = try_reconstruct_receiver_normal(vec2<i32>(pixel_coord), receiver.resolvedLayer, world_position, &receiver_normal);
    let normal_offset = select(vec3<f32>(0.0), receiver_normal * thickness, has_receiver_normal);
    let ray_origin = world_position + normal_offset + ray_direction * start_bias;
    var step_index = 0u;
    loop {
        if (step_index >= sample_count) {
            break;
        }

        let ray_distance = step_size * (f32(step_index) + 0.5);
        if (ray_distance >= max_distance) {
            break;
        }

        let sample_world = ray_origin + ray_direction * ray_distance;
        var sample_pixel = vec2<i32>(0, 0);
        if (!project_world_to_camera_pixel(sample_world, &sample_pixel)) {
            step_index = step_index + 1u;
            continue;
        }

        let segment_half_length = max(step_size * 0.75, thickness * 0.5);
        if (evaluate_contact_shadow_candidate(sample_pixel, ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(1, 0), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(-1, 0), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(0, 1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(0, -1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(1, 1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(1, -1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(-1, 1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length) ||
            evaluate_contact_shadow_candidate(sample_pixel + vec2<i32>(-1, -1), ray_origin, ray_direction, ray_distance, thickness, segment_half_length)) {
            return 1.0 - intensity;
        }

        step_index = step_index + 1u;
    }

    return 1.0;
}

fn evaluate_screen_space_shadow(
    pixel_coord: vec2<u32>,
    receiver: ReceiverState,
    light_index: u32
) -> f32 {
    if (uniforms.screenSpaceShadowEnabled == 0u || light_index != 0u || receiver.valid == 0u) {
        return 1.0;
    }

    // Screen-space shadows are only intended to recover near-field detail.
    if (cascadeMatrices[receiver.resolvedLayer].cascadeIndex != 0u) {
        return 1.0;
    }

    let dims = textureDimensions(screenSpaceShadowTexture);
    let sample_coord = clamp(vec2<i32>(pixel_coord), vec2<i32>(0), vec2<i32>(dims) - vec2<i32>(1));
    return clamp(textureLoad(screenSpaceShadowTexture, sample_coord, 0).r, 0.0, 1.0);
}
