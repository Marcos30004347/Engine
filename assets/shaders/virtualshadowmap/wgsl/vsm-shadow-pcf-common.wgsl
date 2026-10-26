#include "vsm-common.wgsl"

struct ShadowPcfUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    cascadeCount: u32,
    activeLights: u32,
    enabled: u32,
    physicalPageSize: u32,
    reverseZ: u32,
    fallbackCascadeOffset: u32,
    firstCascadeWorldExtent: f32,
    directionalShadowBias: f32,
    spotShadowBias: f32,
    pointShadowBias: f32,
    _paddingBias0: f32,
    directionalSlopeScaleBias: f32,
    spotSlopeScaleBias: f32,
    pointSlopeScaleBias: f32,
    _paddingBias1: f32,
    directionalMaxShadowBias: f32,
    spotMaxShadowBias: f32,
    pointMaxShadowBias: f32,
    _paddingBias2: f32,
    pcfRadiusTexels: f32,
    directionalNormalBiasTexels: f32,
    spotNormalBiasTexels: f32,
    pointNormalBiasTexels: f32,
    _paddingBias3: f32,
    contactShadowDistance: f32,
    contactShadowThickness: f32,
    ambientShadowColor: vec3<f32>,
    contactShadowIntensity: f32,
    shadowFilterTaps: u32,
    contactShadowSamples: u32,
    screenSpaceShadowEnabled: u32,
    contactShadowStartBias: f32,
    localLightMipTransitionScale: f32,
    localLightMipBias: f32,
    normalLightingEnabled: u32,
    normalLightingThreshold: f32,
    normalLightingExponent: f32,
    localLightPageWorldScale: f32,
    fallbackPcfRadiusTexels: f32,
    fallbackShadowFilterTaps: u32,
    softShadowMethod: u32,
    _paddingFallbackFilter: u32,
    pcssBlockerSearchTaps: u32,
    pcssLightRadiusTexels: f32,
    pcssMaxPenumbraTexels: f32,
    pcssPenumbraScale: f32,
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
    _padding: vec3<u32>,
}

struct DirectionalLight {
    position: vec4<f32>,
    direction: vec4<f32>,
    color: vec4<f32>,
    params: vec4<f32>,
    metadata: vec4<u32>,
}

struct SoftShadowLight {
    layer: u32,
    flags: u32,
    lightRadiusWorld: f32,
    penumbraScale: f32,
}

const LIGHT_TYPE_DIRECTIONAL : u32 = 0u;
const LIGHT_TYPE_SPOT : u32 = 1u;
const LIGHT_TYPE_POINT : u32 = 2u;
const SOFT_SHADOW_METHOD_PCSS : u32 = 2u;
const SOFT_SHADOW_METHOD_LAYERED_SSSS : u32 = 3u;

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

fn light_color_rgb(light_index: u32) -> vec3<f32> {
    return max(directionalLights[light_index].color.rgb, vec3<f32>(0.0, 0.0, 0.0));
}

fn light_intensity(light_index: u32) -> f32 {
    return max(directionalLights[light_index].color.a, 0.0);
}

fn light_radiance(light_index: u32) -> vec3<f32> {
    return light_color_rgb(light_index) * light_intensity(light_index);
}

fn point_light_distance_falloff(light_index: u32, world_position: vec3<f32>) -> f32 {
    let range = max(directionalLights[light_index].params.x, 1e-4);
    let outer_range = range * 1.15;
    let light_position = directionalLights[light_index].position.xyz;
    let distance_to_light = distance(world_position, light_position);
    let normalized_distance = clamp(distance_to_light / outer_range, 0.0, 1.0);
    let edge_fade = 1.0 - smoothstep(0.0, 1.0, normalized_distance);
    return edge_fade * edge_fade;
}

fn light_distance_falloff(light_index: u32, world_position: vec3<f32>) -> f32 {
    if (light_type(light_index) == LIGHT_TYPE_POINT) {
        return point_light_distance_falloff(light_index, world_position);
    }
    return 1.0;
}

fn shadow_radiance_for_light(light_index: u32, light_radiance_value: vec3<f32>) -> vec3<f32> {
    let _light_index = light_index;
    let _light_radiance_value = light_radiance_value;
    return max(uniforms.ambientShadowColor, vec3<f32>(0.0, 0.0, 0.0));
}

fn shadow_bias_for_light_type(light_kind: u32) -> f32 {
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        return uniforms.directionalShadowBias;
    }
    if (light_kind == LIGHT_TYPE_SPOT) {
        return uniforms.spotShadowBias;
    }
    return uniforms.pointShadowBias;
}

fn slope_scale_bias_for_light_type(light_kind: u32) -> f32 {
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        return uniforms.directionalSlopeScaleBias;
    }
    if (light_kind == LIGHT_TYPE_SPOT) {
        return uniforms.spotSlopeScaleBias;
    }
    return uniforms.pointSlopeScaleBias;
}

fn max_shadow_bias_for_light_type(light_kind: u32) -> f32 {
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        return uniforms.directionalMaxShadowBias;
    }
    if (light_kind == LIGHT_TYPE_SPOT) {
        return uniforms.spotMaxShadowBias;
    }
    return uniforms.pointMaxShadowBias;
}

fn normal_bias_texels_for_light_type(light_kind: u32) -> f32 {
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        return uniforms.directionalNormalBiasTexels;
    }
    if (light_kind == LIGHT_TYPE_SPOT) {
        return uniforms.spotNormalBiasTexels;
    }
    return uniforms.pointNormalBiasTexels;
}

struct ReceiverState {
    valid: u32,
    resolvedLayer: u32,
    receiverDepth: f32,
    effectiveBias: f32,
    virtualTexel: vec2<f32>,
    receiverNormal: vec3<f32>,
    hasReceiverNormal: u32,
    usedFallback: u32,
}

@group(0) @binding(0) var<uniform> uniforms: ShadowPcfUniforms;
@group(0) @binding(1) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(2) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(3) var<storage, read> cameraState: CameraState;
@group(0) @binding(4) var<storage, read> directionalLights: array<DirectionalLight>;
@group(0) @binding(5) var depthTexture: texture_depth_2d;
@group(0) @binding(6) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(7) var shadowAtlasTexture: texture_depth_2d;
@group(0) @binding(8) var screenSpaceShadowTexture: texture_2d_array<f32>;
@group(0) @binding(9) var normalTexture: texture_2d<f32>;
@group(0) @binding(10) var outputTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(11) var<storage, read> softShadowLights: array<SoftShadowLight>;
@group(0) @binding(12) var screenSpaceSoftShadowTexture: texture_2d_array<f32>;

fn reconstruct_world_position(pixel_coord: vec2<u32>) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

fn try_load_gbuffer_normal(
    pixel_coord: vec2<u32>,
    normal_out: ptr<function, vec3<f32>>
) -> bool {
    let encoded_normal = textureLoad(normalTexture, vec2<i32>(pixel_coord), 0).xyz;
    let encoded_length_sq = dot(encoded_normal, encoded_normal);
    if (encoded_length_sq <= 1e-8) {
        return false;
    }

    (*normal_out) = encoded_normal / sqrt(encoded_length_sq);
    return true;
}

fn reconstruct_view_position(pixel_coord: vec2<u32>, depth: f32) -> vec3<f32> {
    let dims = textureDimensions(depthTexture);
    let uv = vsm_texel_center_uv(pixel_coord, dims);
    return vsm_reconstruct_view_position_from_uv_depth(uv, depth, cameraState.inverseProjection);
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
    (*world_position_out) = vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
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

fn shadow_bias_scale_for_layer(layer: u32) -> f32 {
    let light_index = cascadeMatrices[layer].lightIndex;
    let light_kind = light_type(light_index);
    var base_layer = light_first_layer(light_index);
    if (light_kind == LIGHT_TYPE_POINT) {
        base_layer = point_light_layer(light_index, cascadeMatrices[layer].faceIndex, 0u);
    }
    if (base_layer >= uniforms.activeLayers) {
        return 1.0;
    }

    let base_texel_world_size = max(shadow_texel_world_size(base_layer), 1e-6);
    let current_texel_world_size = max(shadow_texel_world_size(layer), 1e-6);
    return max(current_texel_world_size / base_texel_world_size, 1.0);
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

fn try_resolve_receiver_normal(
    pixel_coord: vec2<u32>,
    layer: u32,
    world_position: vec3<f32>,
    normal_out: ptr<function, vec3<f32>>
) -> bool {
    if (try_load_gbuffer_normal(pixel_coord, normal_out)) {
        return true;
    }

    return try_reconstruct_receiver_normal(vec2<i32>(pixel_coord), layer, world_position, normal_out);
}

/// @brief Selects the preferred cascade level for a receiver using camera-space distance.
fn select_cascade_level(camera_distance: f32) -> u32 {
    return vsm_select_rotationally_invariant_clipmap_level(
        camera_distance,
        uniforms.firstCascadeWorldExtent,
        uniforms.cascadeCount);
}

fn cascade_contains_world_position(world_position: vec3<f32>, layer: u32) -> bool {
    return vsm_cascade_contains_world_position(world_position, cascadeMatrices[layer].viewProj);
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
    let effective_range = select(range, range * 1.15, kind == LIGHT_TYPE_POINT);
    if (dot(to_receiver, to_receiver) > effective_range * effective_range) {
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

/// @brief Finds the finest covering cascade for one light, starting from the rotationally invariant preferred level.
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

fn clamp_virtual_texel_for_bilinear_filter(layer: u32, virtual_texel: vec2<f32>) -> vec2<f32> {
    let virtual_resolution = f32(max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u));
    let inset = 0.5;
    return clamp(
        virtual_texel,
        vec2<f32>(inset, inset),
        vec2<f32>(virtual_resolution - inset, virtual_resolution - inset));
}

fn try_resolve_shadow_texel(
    layer: u32,
    virtual_texel: vec2<f32>,
    atlas_pixel_out: ptr<function, vec2<f32>>,
    stored_depth_out: ptr<function, f32>,
    atlas_texel_out: ptr<function, vec2<i32>>
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
    (*atlas_pixel_out) = atlas_pixel;
    let atlas_resolution = vec2<i32>(max(textureDimensions(shadowAtlasTexture), vec2<u32>(1u, 1u)));
    (*atlas_texel_out) = clamp(vec2<i32>(floor(atlas_pixel)), vec2<i32>(0, 0), atlas_resolution - vec2<i32>(1, 1));
    (*stored_depth_out) = vsm_load_shadow_depth(shadowAtlasTexture, *atlas_texel_out);
    return true;
}

fn estimate_neighbor_shadow_slope(
    neighbor_pixel_coord: vec2<i32>,
    layer: u32,
    world_position: vec3<f32>,
    receiver_depth: f32,
    virtual_texel: vec2<f32>
) -> f32 {
    var neighbor_world_position = vec3<f32>(0.0, 0.0, 0.0);
    if (!try_reconstruct_world_position(neighbor_pixel_coord, &neighbor_world_position)) {
        return 0.0;
    }
    if (!receiver_neighbor_is_compatible(neighbor_world_position, world_position, layer)) {
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
    world_position: vec3<f32>,
    receiver_depth: f32,
    resolved_layer: u32,
    virtual_texel: vec2<f32>
) -> f32 {
    let pixel = vec2<i32>(pixel_coord);
    var slope = 0.0;
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(1, 0), resolved_layer, world_position, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(-1, 0), resolved_layer, world_position, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(0, 1), resolved_layer, world_position, receiver_depth, virtual_texel));
    slope = max(slope, estimate_neighbor_shadow_slope(pixel + vec2<i32>(0, -1), resolved_layer, world_position, receiver_depth, virtual_texel));

    // `slope` is already measured in shadow-depth units per shadow texel, so the
    // scale factor should stay in texel space instead of being damped by world size.
    // The constant bias, however, should grow with the clipmap texel size so
    // coarser cascades keep roughly the same world-space separation.
    let clipmap_bias_scale = shadow_bias_scale_for_layer(resolved_layer);
    let light_kind = light_type(cascadeMatrices[resolved_layer].lightIndex);
    let base_shadow_bias = max(shadow_bias_for_light_type(light_kind), 0.0);
    let slope_scale_bias = max(slope_scale_bias_for_light_type(light_kind), 0.0);
    let max_shadow_bias = max(max_shadow_bias_for_light_type(light_kind), 0.0);
    var bias = base_shadow_bias * clipmap_bias_scale + slope * slope_scale_bias;
    if (max_shadow_bias > 0.0) {
        bias = min(bias, max_shadow_bias * clipmap_bias_scale);
    }
    return max(bias, 0.0);
}

fn compute_normal_bias_depth(
    world_position: vec3<f32>,
    layer: u32,
    receiver_depth: f32,
    receiver_normal: vec3<f32>,
    has_receiver_normal: bool
) -> f32 {
    let light_index = cascadeMatrices[layer].lightIndex;
    if (light_index >= uniforms.activeLights) {
        return 0.0;
    }
    let light_kind = light_type(light_index);
    let normal_bias_texels = max(normal_bias_texels_for_light_type(light_kind), 0.0);
    if (normal_bias_texels <= 1e-4 || !has_receiver_normal) {
        return 0.0;
    }
    let to_light = normalize(select(
        directionalLights[light_index].position.xyz - world_position,
        -directionalLights[light_index].direction.xyz,
        light_kind == LIGHT_TYPE_DIRECTIONAL));
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

fn light_surface_direction(light_index: u32, world_position: vec3<f32>) -> vec3<f32> {
    let light_kind = light_type(light_index);
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        return normalize(-directionalLights[light_index].direction.xyz);
    }

    let to_light = directionalLights[light_index].position.xyz - world_position;
    let to_light_len_sq = dot(to_light, to_light);
    if (to_light_len_sq <= 1e-8) {
        return vec3<f32>(0.0, 1.0, 0.0);
    }

    return to_light / sqrt(to_light_len_sq);
}

fn evaluate_normal_lighting_weight(
    light_index: u32,
    world_position: vec3<f32>,
    surface_normal: vec3<f32>,
    has_surface_normal: bool
) -> f32 {
    if (uniforms.normalLightingEnabled == 0u || !has_surface_normal || light_index >= uniforms.activeLights) {
        return 1.0;
    }

    let lambert = clamp(dot(surface_normal, light_surface_direction(light_index, world_position)), 0.0, 1.0);
    let threshold = clamp(uniforms.normalLightingThreshold, 0.0, 0.9999);
    let remapped = clamp((lambert - threshold) / max(1.0 - threshold, 1e-4), 0.0, 1.0);
    let exponent = max(uniforms.normalLightingExponent, 1e-4);
    return pow(remapped, exponent);
}

fn clamp_bias_to_preserve_resolved_occluder(
    stored_depth: f32,
    receiver_depth: f32,
    bias: f32
) -> f32 {
    let occluder_delta = vsm_shadow_occluder_delta(stored_depth, receiver_depth, uniforms.reverseZ != 0u);
    // Leave tiny separations alone so bias can still absorb precision noise on self-shadowed receivers.
    let clamp_threshold = max(bias * 0.25, 1e-4);
    if (occluder_delta <= clamp_threshold) {
        return bias;
    }

    return min(bias, max(occluder_delta * 0.99, 0.0));
}

fn resolve_receiver_state_for_light(
    pixel_coord: vec2<u32>,
    world_position: vec3<f32>,
    camera_distance: f32,
    light_index: u32
) -> ReceiverState {
    var state = ReceiverState(0u, 0u, 0.0, 0.0, vec2<f32>(0.0, 0.0), vec3<f32>(0.0, 0.0, 0.0), 0u, 0u);
    if (uniforms.enabled == 0u || uniforms.activeLayers == 0u || light_index >= uniforms.activeLights) {
        return state;
    }
    if (!light_affects_world_position(light_index, world_position)) {
        return state;
    }
    let light_kind = light_type(light_index);
    var layer = 0u;
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    if (light_kind != LIGHT_TYPE_DIRECTIONAL && depth <= 0.0) {
        return state;
    }
    if (light_kind == LIGHT_TYPE_DIRECTIONAL) {
        let preferred_cascade = select_cascade_level(camera_distance);
        let cascade_level = find_covering_cascade_level_for_light(world_position, light_index, preferred_cascade);
        if (cascade_level < 0) {
            return state;
        }
        layer = u32(cascade_level);
    } else if (light_kind == LIGHT_TYPE_POINT) {
        let primary_face = point_light_primary_face(directionalLights[light_index].position.xyz, world_position);
        let base_layer = point_light_layer(light_index, primary_face, 0u);
        var base_uv_min = vec2<f32>(0.0, 0.0);
        var base_uv_max = vec2<f32>(0.0, 0.0);
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
                &base_uv_min,
                &base_uv_max)) {
            return state;
        }
    } else {
        let base_layer = light_first_layer(light_index);
        var uv_min = vec2<f32>(0.0, 0.0);
        var uv_max = vec2<f32>(0.0, 0.0);
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
    var virtual_texel = vec2<f32>(0.0, 0.0);
    if (!try_project_world_to_shadow_virtual_texel(world_position, layer, &receiver_depth, &virtual_texel)) {
        return state;
    }

    var resolved_layer = layer;
    var atlas_pixel = vec2<f32>(0.0, 0.0);
    var stored_depth = 0.0;
    var atlas_texel = vec2<i32>(0, 0);
    var used_fallback = 0u;
    var has_resolved_shadow_texel = try_resolve_shadow_texel(layer, virtual_texel, &atlas_pixel, &stored_depth, &atlas_texel);
    if (!has_resolved_shadow_texel) {
        let fallback_layer = resolve_fallback_layer(layer);
        if (fallback_layer != 0xFFFFFFFFu) {
            var fallback_receiver_depth = 0.0;
            var fallback_virtual_texel = vec2<f32>(0.0, 0.0);
            if (try_project_world_to_shadow_virtual_texel(world_position, fallback_layer, &fallback_receiver_depth, &fallback_virtual_texel)) {
                let fallback_resolved = try_resolve_shadow_texel(
                    fallback_layer,
                    fallback_virtual_texel,
                    &atlas_pixel,
                    &stored_depth,
                    &atlas_texel);
                if (fallback_resolved) {
                    receiver_depth = fallback_receiver_depth;
                    virtual_texel = fallback_virtual_texel;
                    resolved_layer = fallback_layer;
                    used_fallback = 1u;
                    has_resolved_shadow_texel = true;
                }
            }
        }
    }
    if (!has_resolved_shadow_texel) {
        return state;
    }

    let base_bias = compute_effective_shadow_bias(pixel_coord, world_position, receiver_depth, resolved_layer, virtual_texel);
    var receiver_normal = vec3<f32>(0.0, 0.0, 0.0);
    let has_receiver_normal = try_resolve_receiver_normal(pixel_coord, resolved_layer, world_position, &receiver_normal);
    let normal_bias = compute_normal_bias_depth(world_position, resolved_layer, receiver_depth, receiver_normal, has_receiver_normal);
    // Constant/slope bias and projected normal bias are both trying to solve the
    // same self-shadowing problem. Summing them tends to over-separate the shadow.
    var effective_bias = max(base_bias, normal_bias);
    let max_shadow_bias = max_shadow_bias_for_light_type(light_kind);
    if (max_shadow_bias > 0.0) {
        effective_bias = min(effective_bias, max_shadow_bias * shadow_bias_scale_for_layer(resolved_layer));
    }
    // When the resolved center texel already contains a distinct blocker, don't
    // let a view-dependent bias erase that blocker and flip the sample to lit.
    effective_bias = clamp_bias_to_preserve_resolved_occluder(stored_depth, receiver_depth, effective_bias);
    state.valid = 1u;
    state.resolvedLayer = resolved_layer;
    state.receiverDepth = receiver_depth;
    state.effectiveBias = effective_bias;
    state.virtualTexel = virtual_texel;
    state.receiverNormal = receiver_normal;
    state.hasReceiverNormal = select(0u, 1u, has_receiver_normal);
    state.usedFallback = used_fallback;
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
    bias: f32,
    fallback_visibility: f32
) -> f32 {
    let safe_virtual_texel = clamp_virtual_texel_for_bilinear_filter(layer, virtual_texel);
    var atlas_pixel = vec2<f32>(0.0, 0.0);
    var stored_depth = 0.0;
    var atlas_texel = vec2<i32>(0, 0);
    if (!try_resolve_shadow_texel(layer, safe_virtual_texel, &atlas_pixel, &stored_depth, &atlas_texel)) {
        return fallback_visibility;
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
    let center_visibility = sample_shadow_test_current_texel(
        receiver.resolvedLayer,
        receiver.virtualTexel,
        receiver.receiverDepth,
        receiver.effectiveBias);
    if (grid_size <= 1u || filter_radius <= 1e-4) {
        return center_visibility;
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
                receiver.effectiveBias,
                center_visibility);
            count = count + 1u;
            x = x + 1u;
        }

        y = y + 1u;
    }

    return total / f32(max(count, 1u));
}

fn evaluate_pcf_tap_count(receiver: ReceiverState, tap_count: u32, filter_radius: f32) -> f32 {
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
        case 25u: {
            return evaluate_pcf_grid(receiver, 5u, filter_radius);
        }
        case 36u: {
            return evaluate_pcf_grid(receiver, 6u, filter_radius);
        }
        case 49u: {
            return evaluate_pcf_grid(receiver, 7u, filter_radius);
        }
        case 64u: {
            return evaluate_pcf_grid(receiver, 8u, filter_radius);
        }
        default: {
            return evaluate_pcf_grid(receiver, 2u, filter_radius);
        }
    }
}

fn evaluate_pcf(receiver: ReceiverState) -> f32 {
    let use_fallback_filter = receiver.usedFallback != 0u;
    let tap_count = max(select(uniforms.shadowFilterTaps, uniforms.fallbackShadowFilterTaps, use_fallback_filter), 1u);
    let filter_radius = max(select(uniforms.pcfRadiusTexels, uniforms.fallbackPcfRadiusTexels, use_fallback_filter), 0.0);
    return evaluate_pcf_tap_count(receiver, tap_count, filter_radius);
}

fn pcss_disk_offset(index: u32, sample_count: u32) -> vec2<f32> {
    let count = max(sample_count, 1u);
    let radius = sqrt((f32(index) + 0.5) / f32(count));
    let angle = f32(index) * 2.39996322973;
    return vec2<f32>(cos(angle), sin(angle)) * radius;
}

fn evaluate_pcss_poisson_filter(
    receiver: ReceiverState,
    pixel_coord: vec2<u32>,
    tap_count: u32,
    filter_radius: f32,
    center_visibility: f32
) -> f32 {
    if (tap_count <= 1u || filter_radius <= 1e-4) {
        return center_visibility;
    }

    let sample_count = min(max(tap_count, 16u), 64u);
    _ = pixel_coord;
    var total = center_visibility * 1.5;
    var weight_sum = 1.5;
    for (var sample_index = 0u; sample_index < sample_count; sample_index = sample_index + 1u) {
        let disk_offset = pcss_disk_offset(sample_index, sample_count);
        let offset = disk_offset * filter_radius;
        let radius_weight = clamp(1.0 - dot(disk_offset, disk_offset) * 0.35, 0.35, 1.0);
        total = total + sample_shadow_test_bilinear(
            receiver.resolvedLayer,
            receiver.virtualTexel + offset,
            receiver.receiverDepth,
            receiver.effectiveBias,
            center_visibility) * radius_weight;
        weight_sum = weight_sum + radius_weight;
    }

    return total / max(weight_sum, 1e-4);
}

fn evaluate_pcss(receiver: ReceiverState, pixel_coord: vec2<u32>) -> f32 {
    let center_visibility = sample_shadow_test_current_texel(
        receiver.resolvedLayer,
        receiver.virtualTexel,
        receiver.receiverDepth,
        receiver.effectiveBias);

    let blocker_taps = max(uniforms.pcssBlockerSearchTaps, 1u);
    let half_taps = i32(blocker_taps / 2u);
    let cascade_texel_scale = max(shadow_bias_scale_for_layer(receiver.resolvedLayer), 1.0);
    let light_radius_texels = max(uniforms.pcssLightRadiusTexels, 0.0) / cascade_texel_scale;
    let search_radius = light_radius_texels;
    if (half_taps <= 0 || search_radius <= 1e-4) {
        return center_visibility;
    }

    var blocker_delta_sum = 0.0;
    var blocker_depth_sum = 0.0;
    var blocker_count = 0u;
    var center_occluder_delta = 1e20;
    var center_atlas_pixel = vec2<f32>(0.0, 0.0);
    var center_stored_depth = 0.0;
    var center_atlas_texel = vec2<i32>(0, 0);
    if (center_visibility < 0.5 &&
        try_resolve_shadow_texel(receiver.resolvedLayer, receiver.virtualTexel, &center_atlas_pixel, &center_stored_depth, &center_atlas_texel)) {
        _ = center_atlas_pixel;
        _ = center_atlas_texel;
        center_occluder_delta = vsm_shadow_occluder_delta(center_stored_depth, receiver.receiverDepth, uniforms.reverseZ != 0u);
        blocker_delta_sum = blocker_delta_sum + center_occluder_delta;
        blocker_depth_sum = blocker_depth_sum + center_stored_depth;
        blocker_count = blocker_count + 1u;
    }

    let blocker_sample_count = min(blocker_taps * blocker_taps, 64u);
    _ = pixel_coord;
    for (var sample_index = 0u; sample_index < blocker_sample_count; sample_index = sample_index + 1u) {
        let offset = pcss_disk_offset(sample_index, blocker_sample_count) * search_radius;
        var atlas_pixel = vec2<f32>(0.0, 0.0);
        var stored_depth = 0.0;
        var atlas_texel = vec2<i32>(0, 0);
        if (try_resolve_shadow_texel(receiver.resolvedLayer, receiver.virtualTexel + offset, &atlas_pixel, &stored_depth, &atlas_texel)) {
            _ = atlas_pixel;
            _ = atlas_texel;
            let visibility = vsm_shadow_compare(stored_depth, receiver.receiverDepth, receiver.effectiveBias, uniforms.reverseZ != 0u);
            if (visibility < 0.5) {
                blocker_delta_sum = blocker_delta_sum + vsm_shadow_occluder_delta(stored_depth, receiver.receiverDepth, uniforms.reverseZ != 0u);
                blocker_depth_sum = blocker_depth_sum + stored_depth;
                blocker_count = blocker_count + 1u;
            }
        }
    }

    if (blocker_count == 0u) {
        return center_visibility;
    }

    let average_blocker_delta = blocker_delta_sum / f32(blocker_count);
    let average_blocker_depth = blocker_depth_sum / f32(blocker_count);
    let relative_blocker_delta = average_blocker_delta / max(abs(average_blocker_depth), 1e-4);
    let contact_delta_threshold = min(max(receiver.effectiveBias * 1.5, 2e-5), 0.01);
    if (center_visibility < 0.5 && center_occluder_delta <= contact_delta_threshold) {
        return center_visibility;
    }
    let max_penumbra_texels = max(uniforms.pcssMaxPenumbraTexels, 0.0) / cascade_texel_scale;
    let penumbra_radius = clamp(
        relative_blocker_delta * light_radius_texels * max(uniforms.pcssPenumbraScale, 0.0) * 128.0,
        0.0,
        max_penumbra_texels);
    if (penumbra_radius <= 1e-4) {
        return center_visibility;
    }

    let use_fallback_filter = receiver.usedFallback != 0u;
    let tap_count = max(select(uniforms.shadowFilterTaps, uniforms.fallbackShadowFilterTaps, use_fallback_filter), 1u);
    return evaluate_pcss_poisson_filter(receiver, pixel_coord, tap_count, penumbra_radius, center_visibility);
}

fn evaluate_contact_shadow(
    pixel_coord: vec2<u32>,
    world_position: vec3<f32>,
    receiver: ReceiverState,
    light_index: u32
) -> f32 {
    let sample_count = uniforms.contactShadowSamples;
    let intensity = clamp(uniforms.contactShadowIntensity, 0.0, 1.0);
    if (sample_count == 0u || intensity <= 1e-4 || light_index >= uniforms.activeLights || receiver.valid == 0u) {
        return 1.0;
    }
    if (light_type(light_index) != LIGHT_TYPE_DIRECTIONAL) {
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

    let has_receiver_normal = receiver.hasReceiverNormal != 0u;
    let normal_offset = select(vec3<f32>(0.0), receiver.receiverNormal * thickness, has_receiver_normal);
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
    light_index: u32
) -> f32 {
    if (uniforms.screenSpaceShadowEnabled == 0u || light_index >= uniforms.activeLights) {
        return 1.0;
    }
    if (light_type(light_index) != LIGHT_TYPE_DIRECTIONAL) {
        return 1.0;
    }

    let shadow_dims_i = vec2<i32>(textureDimensions(screenSpaceShadowTexture).xy);
    let shadow_dims_f = vec2<f32>(shadow_dims_i);
    let full_dims_f = vec2<f32>(textureDimensions(depthTexture));
    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / full_dims_f;
    let texel = uv * shadow_dims_f - vec2<f32>(0.5, 0.5);
    let base = clamp(vec2<i32>(floor(texel)), vec2<i32>(0), shadow_dims_i - vec2<i32>(1, 1));
    let next = clamp(base + vec2<i32>(1, 1), vec2<i32>(0), shadow_dims_i - vec2<i32>(1, 1));
    let frac_uv = fract(texel);

    let s00 = textureLoad(screenSpaceShadowTexture, base, i32(light_index), 0).r;
    let s10 = textureLoad(screenSpaceShadowTexture, vec2<i32>(next.x, base.y), i32(light_index), 0).r;
    let s01 = textureLoad(screenSpaceShadowTexture, vec2<i32>(base.x, next.y), i32(light_index), 0).r;
    let s11 = textureLoad(screenSpaceShadowTexture, next, i32(light_index), 0).r;

    let sx0 = mix(s00, s10, frac_uv.x);
    let sx1 = mix(s01, s11, frac_uv.x);
    return clamp(mix(sx0, sx1, frac_uv.y), 0.0, 1.0);
}

fn ssss_shadow_texel_to_depth_pixel(shadow_pixel: vec2<i32>, shadow_dims_i: vec2<i32>) -> vec2<i32> {
    let full_dims = vec2<f32>(textureDimensions(depthTexture));
    let uv = (vec2<f32>(shadow_pixel) + vec2<f32>(0.5, 0.5)) / max(vec2<f32>(shadow_dims_i), vec2<f32>(1.0, 1.0));
    let depth_pixel = clamp(
        uv * full_dims,
        vec2<f32>(0.0, 0.0),
        max(full_dims - vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 0.0)));
    return vec2<i32>(depth_pixel);
}

fn ssss_shadow_upsample_sample_is_compatible(
    pixel_coord: vec2<u32>,
    center_depth: f32,
    center_view_position: vec3<f32>,
    center_normal: vec3<f32>,
    has_center_normal: bool,
    shadow_pixel: vec2<i32>,
    shadow_dims_i: vec2<i32>
) -> bool {
    if (center_depth <= 0.0) {
        return false;
    }

    let depth_pixel_i = ssss_shadow_texel_to_depth_pixel(shadow_pixel, shadow_dims_i);
    if (all(depth_pixel_i == vec2<i32>(pixel_coord))) {
        return true;
    }

    let full_dims_i = vec2<i32>(textureDimensions(depthTexture));
    if (depth_pixel_i.x < 0 || depth_pixel_i.y < 0 || depth_pixel_i.x >= full_dims_i.x || depth_pixel_i.y >= full_dims_i.y) {
        return false;
    }

    let sample_depth = textureLoad(depthTexture, depth_pixel_i, 0);
    if (sample_depth <= 0.0) {
        return false;
    }

    let sample_pixel = vec2<u32>(depth_pixel_i);
    let sample_view_position = reconstruct_view_position(sample_pixel, sample_depth);
    let center_distance = max(length(center_view_position), 1e-3);
    if (length(sample_view_position - center_view_position) > max(0.025 * center_distance, 0.05)) {
        return false;
    }

    if (has_center_normal) {
        var sample_normal = vec3<f32>(0.0, 0.0, 1.0);
        if (try_load_gbuffer_normal(sample_pixel, &sample_normal) && dot(center_normal, sample_normal) < 0.35) {
            return false;
        }

        let plane_delta = abs(dot(sample_view_position - center_view_position, center_normal));
        if (plane_delta > max(0.01 * center_distance, 0.02)) {
            return false;
        }
    }

    return true;
}

fn ssss_accumulate_depth_compatible_upsample(
    pixel_coord: vec2<u32>,
    center_depth: f32,
    center_view_position: vec3<f32>,
    center_normal: vec3<f32>,
    has_center_normal: bool,
    shadow_pixel: vec2<i32>,
    shadow_dims_i: vec2<i32>,
    visibility: f32,
    weight: f32,
    sum: ptr<function, f32>,
    weight_sum: ptr<function, f32>
) {
    if (weight <= 0.0) {
        return;
    }
    if (!ssss_shadow_upsample_sample_is_compatible(
            pixel_coord,
            center_depth,
            center_view_position,
            center_normal,
            has_center_normal,
            shadow_pixel,
            shadow_dims_i)) {
        return;
    }

    (*sum) = (*sum) + visibility * weight;
    (*weight_sum) = (*weight_sum) + weight;
}

fn evaluate_layered_screen_space_soft_shadow(
    pixel_coord: vec2<u32>,
    light_index: u32
) -> f32 {
    if (uniforms.softShadowMethod != SOFT_SHADOW_METHOD_LAYERED_SSSS || light_index >= uniforms.activeLights) {
        return 1.0;
    }

    let soft_light = softShadowLights[light_index];
    if ((soft_light.flags & 1u) != 0u) {
        return 1.0;
    }

    let shadow_dims_i = vec2<i32>(textureDimensions(screenSpaceSoftShadowTexture).xy);
    let layer_count = textureNumLayers(screenSpaceSoftShadowTexture);
    if (shadow_dims_i.x <= 0 || shadow_dims_i.y <= 0 || soft_light.layer >= layer_count) {
        return 1.0;
    }

    let shadow_dims_f = vec2<f32>(shadow_dims_i);
    let full_dims_f = vec2<f32>(textureDimensions(depthTexture));
    let uv = (vec2<f32>(pixel_coord) + vec2<f32>(0.5, 0.5)) / full_dims_f;
    let texel = uv * shadow_dims_f - vec2<f32>(0.5, 0.5);
    let base = clamp(vec2<i32>(floor(texel)), vec2<i32>(0), shadow_dims_i - vec2<i32>(1, 1));
    let next = clamp(base + vec2<i32>(1, 1), vec2<i32>(0), shadow_dims_i - vec2<i32>(1, 1));
    let frac_uv = fract(texel);
    let layer = i32(soft_light.layer);

    let center_depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let center_view_position = reconstruct_view_position(pixel_coord, center_depth);
    var center_normal = vec3<f32>(0.0, 0.0, 1.0);
    let has_center_normal = try_load_gbuffer_normal(pixel_coord, &center_normal);

    let s00 = textureLoad(screenSpaceSoftShadowTexture, base, layer, 0).r;
    let s10 = textureLoad(screenSpaceSoftShadowTexture, vec2<i32>(next.x, base.y), layer, 0).r;
    let s01 = textureLoad(screenSpaceSoftShadowTexture, vec2<i32>(base.x, next.y), layer, 0).r;
    let s11 = textureLoad(screenSpaceSoftShadowTexture, next, layer, 0).r;

    let w00 = (1.0 - frac_uv.x) * (1.0 - frac_uv.y);
    let w10 = frac_uv.x * (1.0 - frac_uv.y);
    let w01 = (1.0 - frac_uv.x) * frac_uv.y;
    let w11 = frac_uv.x * frac_uv.y;

    var sum = 0.0;
    var weight_sum = 0.0;
    ssss_accumulate_depth_compatible_upsample(
        pixel_coord,
        center_depth,
        center_view_position,
        center_normal,
        has_center_normal,
        base,
        shadow_dims_i,
        s00,
        w00,
        &sum,
        &weight_sum);
    ssss_accumulate_depth_compatible_upsample(
        pixel_coord,
        center_depth,
        center_view_position,
        center_normal,
        has_center_normal,
        vec2<i32>(next.x, base.y),
        shadow_dims_i,
        s10,
        w10,
        &sum,
        &weight_sum);
    ssss_accumulate_depth_compatible_upsample(
        pixel_coord,
        center_depth,
        center_view_position,
        center_normal,
        has_center_normal,
        vec2<i32>(base.x, next.y),
        shadow_dims_i,
        s01,
        w01,
        &sum,
        &weight_sum);
    ssss_accumulate_depth_compatible_upsample(
        pixel_coord,
        center_depth,
        center_view_position,
        center_normal,
        has_center_normal,
        next,
        shadow_dims_i,
        s11,
        w11,
        &sum,
        &weight_sum);

    if (weight_sum > 1e-5) {
        return clamp(sum / weight_sum, 0.0, 1.0);
    }

    let sx0 = mix(s00, s10, frac_uv.x);
    let sx1 = mix(s01, s11, frac_uv.x);
    return clamp(mix(sx0, sx1, frac_uv.y), 0.0, 1.0);
}
