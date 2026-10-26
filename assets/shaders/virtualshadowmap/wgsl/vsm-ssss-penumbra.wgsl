#include "vsm-common.wgsl"
#include "vsm-ssss-common.wgsl"

const LIGHT_TYPE_DIRECTIONAL : u32 = 0u;
const LIGHT_TYPE_SPOT : u32 = 1u;
const LIGHT_TYPE_POINT : u32 = 2u;

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
    _padding: vec3<u32>,
}

struct DirectionalLight {
    position: vec4<f32>,
    direction: vec4<f32>,
    color: vec4<f32>,
    params: vec4<f32>,
    metadata: vec4<u32>,
}

struct ReceiverState {
    valid: u32,
    resolvedLayer: u32,
    receiverDepth: f32,
    virtualTexel: vec2<f32>,
    bias: f32,
    centerVisibility: f32,
    centerOccluderDelta: f32,
}

struct PenumbraSample {
    visibility: f32,
    penumbra: f32,
}

@group(0) @binding(0) var<uniform> uniforms: ScreenSpaceSoftShadowUniforms;
@group(0) @binding(1) var depthTexture: texture_depth_2d;
@group(0) @binding(2) var normalTexture: texture_2d<f32>;
@group(0) @binding(3) var<storage, read> softShadowLights: array<SoftShadowLight>;
@group(0) @binding(4) var hardShadowTexture: texture_storage_2d_array<r16float, write>;
@group(0) @binding(5) var penumbraTexture: texture_storage_2d_array<r16float, write>;
@group(0) @binding(6) var translucencyTexture: texture_storage_2d_array<rgba16float, write>;
@group(0) @binding(7) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(8) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(9) var<storage, read> cameraState: CameraState;
@group(0) @binding(10) var<storage, read> directionalLights: array<DirectionalLight>;
@group(0) @binding(11) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(12) var shadowAtlasTexture: texture_depth_2d;

fn light_type(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.x;
}

fn light_first_layer(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.y;
}

fn light_mip_count(light_index: u32) -> u32 {
    return max(directionalLights[light_index].metadata.w, 1u);
}

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

fn reconstruct_world_position(pixel_coord: vec2<u32>, depth: f32) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

fn reconstruct_view_position(pixel_coord: vec2<u32>, depth: f32) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_view_position_from_uv_depth(uv, depth, cameraState.inverseProjection);
}

fn cascade_contains_world_position(world_position: vec3<f32>, layer: u32) -> bool {
    return vsm_cascade_contains_world_position(world_position, cascadeMatrices[layer].viewProj);
}

fn select_cascade_level(camera_distance: f32) -> u32 {
    return vsm_select_rotationally_invariant_clipmap_level(
        camera_distance,
        uniforms.firstCascadeWorldExtent,
        uniforms.cascadeCount);
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
    let effective_range = select(range, range * 1.15, kind == LIGHT_TYPE_POINT);
    if (dot(to_receiver, to_receiver) > effective_range * effective_range) {
        return false;
    }

    if (kind == LIGHT_TYPE_POINT) {
        return true;
    }

    let len_sq = dot(to_receiver, to_receiver);
    if (len_sq <= 1e-8) {
        return true;
    }

    let light_direction = normalize(directionalLights[light_index].direction.xyz);
    let outer_angle = clamp(directionalLights[light_index].params.y, 0.0, 3.14159265);
    return dot(normalize(to_receiver), light_direction) >= cos(outer_angle * 0.5);
}

fn find_covering_cascade_level_for_light(world_position: vec3<f32>, light_index: u32, preferred_cascade: u32) -> i32 {
    let base_layer = light_first_layer(light_index);
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
    let light_index = cascadeMatrices[layer].lightIndex;
    let kind = light_type(light_index);
    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        if (uniforms.fallbackCascadeOffset == 0u) {
            return 0xFFFFFFFFu;
        }
        let fallback_cascade = cascadeMatrices[layer].cascadeIndex + uniforms.fallbackCascadeOffset;
        if (fallback_cascade >= uniforms.cascadeCount) {
            return 0xFFFFFFFFu;
        }
        let fallback_layer = light_first_layer(light_index) + fallback_cascade;
        return select(0xFFFFFFFFu, fallback_layer, fallback_layer < uniforms.activeLayers);
    }

    let fallback_mip = cascadeMatrices[layer].mipLevel + 1u;
    if (fallback_mip >= light_mip_count(light_index)) {
        return 0xFFFFFFFFu;
    }

    var fallback_layer = light_first_layer(light_index) + fallback_mip;
    if (kind == LIGHT_TYPE_POINT) {
        fallback_layer = point_light_layer(light_index, cascadeMatrices[layer].faceIndex, fallback_mip);
    }
    return select(0xFFFFFFFFu, fallback_layer, fallback_layer < uniforms.activeLayers);
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
    let virtual_resolution = f32(max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u));
    (*receiver_depth_out) = ndc.z;
    (*virtual_texel_out) = uv * vec2<f32>(virtual_resolution, virtual_resolution);
    return true;
}

fn try_resolve_shadow_texel(
    layer: u32,
    virtual_texel: vec2<f32>,
    stored_depth_out: ptr<function, f32>
) -> bool {
    let layer_page_table_resolution_u = layer_page_table_resolution(layer);
    let virtual_resolution_u = max(layer_page_table_resolution_u * uniforms.physicalPageSize, 1u);
    let virtual_resolution = vec2<f32>(f32(virtual_resolution_u), f32(virtual_resolution_u));
    if (virtual_texel.x < 0.0 || virtual_texel.y < 0.0 ||
        virtual_texel.x >= virtual_resolution.x || virtual_texel.y >= virtual_resolution.y) {
        return false;
    }

    let uv = clamp(virtual_texel / virtual_resolution, vec2<f32>(0.0, 0.0), vec2<f32>(0.99999994, 0.99999994));
    let virtual_sample = vsm_virtual_uv_to_page_sample(uv, layer_page_table_resolution_u);
    let wrapped_page_i = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(i32(virtual_sample.pageCoords.x), i32(virtual_sample.pageCoords.y)),
        cascadeStates[layer].pageOffset,
        i32(layer_page_table_resolution_u));
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
    let atlas_resolution = vec2<i32>(max(textureDimensions(shadowAtlasTexture), vec2<u32>(1u, 1u)));
    let atlas_texel = clamp(vec2<i32>(floor(atlas_pixel)), vec2<i32>(0, 0), atlas_resolution - vec2<i32>(1, 1));
    (*stored_depth_out) = vsm_load_shadow_depth(shadowAtlasTexture, atlas_texel);
    return true;
}

fn shadow_bias_for_light_type(kind: u32) -> f32 {
    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        return uniforms.directionalShadowBias;
    }
    if (kind == LIGHT_TYPE_SPOT) {
        return uniforms.spotShadowBias;
    }
    return uniforms.pointShadowBias;
}

fn shadow_texel_world_size(layer: u32) -> f32 {
    return cascadeMatrices[layer].pageWorldSize / f32(max(uniforms.physicalPageSize, 1u));
}

fn shadow_bias_scale_for_layer(layer: u32) -> f32 {
    let light_index = cascadeMatrices[layer].lightIndex;
    let kind = light_type(light_index);
    var base_layer = light_first_layer(light_index);
    if (kind == LIGHT_TYPE_POINT) {
        base_layer = point_light_layer(light_index, cascadeMatrices[layer].faceIndex, 0u);
    }
    if (base_layer >= uniforms.activeLayers) {
        return 1.0;
    }

    let base_texel_world_size = max(shadow_texel_world_size(base_layer), 1e-6);
    let current_texel_world_size = max(shadow_texel_world_size(layer), 1e-6);
    return max(current_texel_world_size / base_texel_world_size, 1.0);
}

fn clamp_bias_to_preserve_resolved_occluder(
    stored_depth: f32,
    receiver_depth: f32,
    bias: f32
) -> f32 {
    let occluder_delta = vsm_shadow_occluder_delta(stored_depth, receiver_depth, uniforms.reverseZ != 0u);
    let clamp_threshold = max(bias * 0.25, 1e-4);
    if (occluder_delta <= clamp_threshold) {
        return bias;
    }

    return min(bias, max(occluder_delta * 0.99, 0.0));
}

fn resolve_receiver_state_for_light(
    pixel_coord: vec2<u32>,
    depth: f32,
    world_position: vec3<f32>,
    camera_distance: f32,
    light_index: u32
) -> ReceiverState {
    var state = ReceiverState(0u, 0u, 0.0, vec2<f32>(0.0, 0.0), 0.0, 1.0, 1e20);
    if (uniforms.activeLayers == 0u || light_index >= uniforms.activeLights || !light_affects_world_position(light_index, world_position)) {
        return state;
    }

    let kind = light_type(light_index);
    var layer = 0u;
    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        let cascade_level = find_covering_cascade_level_for_light(world_position, light_index, select_cascade_level(camera_distance));
        if (cascade_level < 0) {
            return state;
        }
        layer = u32(cascade_level);
    } else if (kind == LIGHT_TYPE_POINT) {
        let primary_face = point_light_primary_face(directionalLights[light_index].position.xyz, world_position);
        let base_layer = point_light_layer(light_index, primary_face, 0u);
        var uv_min = vec2<f32>(0.0);
        var uv_max = vec2<f32>(0.0);
        if (!vsm_select_perspective_layer_from_base_layer(
                pixel_coord,
                depth,
                base_layer,
                light_mip_count(light_index),
                uniforms.physicalPageSize,
                uniforms.localLightMipTransitionScale,
                uniforms.localLightMipBias,
                uniforms.localLightPageWorldScale,
                &layer,
                &uv_min,
                &uv_max)) {
            return state;
        }
    } else {
        let base_layer = light_first_layer(light_index);
        var uv_min = vec2<f32>(0.0);
        var uv_max = vec2<f32>(0.0);
        if (!vsm_select_perspective_layer_from_base_layer(
                pixel_coord,
                depth,
                base_layer,
                light_mip_count(light_index),
                uniforms.physicalPageSize,
                uniforms.localLightMipTransitionScale,
                uniforms.localLightMipBias,
                uniforms.localLightPageWorldScale,
                &layer,
                &uv_min,
                &uv_max)) {
            return state;
        }
    }

    var receiver_depth = 0.0;
    var virtual_texel = vec2<f32>(0.0);
    if (!try_project_world_to_shadow_virtual_texel(world_position, layer, &receiver_depth, &virtual_texel)) {
        return state;
    }

    var stored_depth = 0.0;
    var resolved_layer = layer;
    var has_texel = try_resolve_shadow_texel(layer, virtual_texel, &stored_depth);
    if (!has_texel) {
        let fallback_layer = resolve_fallback_layer(layer);
        if (fallback_layer != 0xFFFFFFFFu) {
            var fallback_depth = 0.0;
            var fallback_texel = vec2<f32>(0.0);
            if (try_project_world_to_shadow_virtual_texel(world_position, fallback_layer, &fallback_depth, &fallback_texel) &&
                try_resolve_shadow_texel(fallback_layer, fallback_texel, &stored_depth)) {
                receiver_depth = fallback_depth;
                virtual_texel = fallback_texel;
                resolved_layer = fallback_layer;
                has_texel = true;
            }
        }
    }
    if (!has_texel) {
        return state;
    }

    state.valid = 1u;
    state.resolvedLayer = resolved_layer;
    state.receiverDepth = receiver_depth;
    state.virtualTexel = virtual_texel;
    let scaled_bias = max(shadow_bias_for_light_type(kind), 0.0) * shadow_bias_scale_for_layer(resolved_layer);
    state.bias = clamp_bias_to_preserve_resolved_occluder(stored_depth, receiver_depth, scaled_bias);
    state.centerVisibility = vsm_shadow_compare(stored_depth, receiver_depth, state.bias, uniforms.reverseZ != 0u);
    state.centerOccluderDelta = vsm_shadow_occluder_delta(stored_depth, receiver_depth, uniforms.reverseZ != 0u);
    return state;
}

fn evaluate_hard_shadow_and_penumbra(receiver: ReceiverState, light_radius: f32, penumbra_scale: f32) -> PenumbraSample {
    let center_visibility = receiver.centerVisibility;
    let center_occluder_delta = receiver.centerOccluderDelta;
    let contact_delta_threshold = min(max(receiver.bias * 1.25, 2e-5), 0.01);
    if (center_visibility >= 0.5 || light_radius <= 0.0 || penumbra_scale <= 0.0) {
        return PenumbraSample(center_visibility, 0.0);
    }

    let taps = max(uniforms.blockerSearchTaps, 1u);
    let half_taps = i32(taps / 2u);
    let search_radius = max(1.0, light_radius * 4.0);

    var blocker_delta_sum = center_occluder_delta;
    var blocker_count = 1u;
    for (var y = -half_taps; y <= half_taps; y = y + 1) {
        for (var x = -half_taps; x <= half_taps; x = x + 1) {
            if (x == 0 && y == 0) {
                continue;
            }
            let denom = f32(max(half_taps, 1));
            let offset = vec2<f32>(f32(x), f32(y)) * (search_radius / denom);
            var stored_depth = 0.0;
            if (try_resolve_shadow_texel(receiver.resolvedLayer, receiver.virtualTexel + offset, &stored_depth)) {
                let blocker_delta = vsm_shadow_occluder_delta(stored_depth, receiver.receiverDepth, uniforms.reverseZ != 0u);
                let visibility = vsm_shadow_compare(stored_depth, receiver.receiverDepth, receiver.bias, uniforms.reverseZ != 0u);
                if (visibility < 0.5) {
                    blocker_delta_sum = blocker_delta_sum + blocker_delta;
                    blocker_count = blocker_count + 1u;
                }
            }
        }
    }

    var penumbra = 0.0;
    if (blocker_count > 0u) {
        let average_delta = blocker_delta_sum / f32(blocker_count);
        var effective_delta = average_delta;
        if (center_visibility < 0.5) {
            effective_delta = min(average_delta, center_occluder_delta);
        }
        penumbra = ssss_saturate(effective_delta * max(light_radius, 0.0) * max(penumbra_scale, 0.0) * 128.0);
    }
    if (center_visibility < 0.5 && center_occluder_delta <= contact_delta_threshold) {
        penumbra = 0.0;
    }
    return PenumbraSample(center_visibility, penumbra);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = uniforms.inputViewport;
    if (!ssss_in_bounds(global_id.xy, dims) || global_id.z >= max(uniforms.layerCount, 1u)) {
        return;
    }

    let pixel = vec2<i32>(global_id.xy);
    let layer = i32(global_id.z);
    let input_pixel_i = pixel;
    let input_pixel = vec2<u32>(input_pixel_i);
    let depth = textureLoad(depthTexture, input_pixel_i, 0);

    var visibility = 1.0;
    var penumbra = 0.0;
    if (depth > 0.0 && uniforms.activeLights > 0u) {
        let world_position = reconstruct_world_position(input_pixel, depth);
        let camera_distance = length(reconstruct_view_position(input_pixel, depth));
        for (var light_index = 0u; light_index < uniforms.activeLights; light_index = light_index + 1u) {
            let soft_light = softShadowLights[light_index];
            if (soft_light.layer != global_id.z || (soft_light.flags & 1u) != 0u) {
                continue;
            }

            let receiver = resolve_receiver_state_for_light(input_pixel, depth, world_position, camera_distance, light_index);
            if (receiver.valid == 0u) {
                continue;
            }

            let light_radius = max(soft_light.lightRadiusWorld, uniforms.defaultLightRadiusWorld);
            let penumbra_scale = max(soft_light.penumbraScale, uniforms.penumbraScale);
            let sample = evaluate_hard_shadow_and_penumbra(receiver, light_radius, penumbra_scale);
            visibility = min(visibility, sample.visibility);
            penumbra = max(penumbra, sample.penumbra);
        }
    }

    textureStore(hardShadowTexture, pixel, layer, vec4<f32>(visibility, 0.0, 0.0, 1.0));
    textureStore(penumbraTexture, pixel, layer, vec4<f32>(penumbra, 0.0, 0.0, 1.0));
    textureStore(translucencyTexture, pixel, layer, vec4<f32>(1.0, 1.0, 1.0, 1.0));
}
