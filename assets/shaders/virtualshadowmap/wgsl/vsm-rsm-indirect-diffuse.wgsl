#include "vsm-common.wgsl"

struct RsmIndirectDiffuseUniforms {
    pageTableResolution: u32,
    activeLayers: u32,
    cascadeCount: u32,
    activeLights: u32,
    enabled: u32,
    physicalPageSize: u32,
    physicalAtlasResolution: u32,
    rsmAtlasResolution: u32,
    width: u32,
    height: u32,
    sampleCount: u32,
    frameIndex: u32,
    debugMode: u32,
    fallbackCascadeOffset: u32,
    _padding0u: vec2<u32>,
    firstCascadeWorldExtent: f32,
    sampleRadiusWorld: f32,
    intensity: f32,
    normalBias: f32,
    minDistance: f32,
    maxDistance: f32,
    localLightMipTransitionScale: f32,
    localLightMipBias: f32,
    localLightPageWorldScale: f32,
    fireflyClamp: f32,
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
    valid: bool,
    layer: u32,
    virtualTexel: vec2<f32>,
    normal: vec3<f32>,
    worldPosition: vec3<f32>,
}

struct RsmSample {
    valid: bool,
    texel: vec2<i32>,
    atlasTexel: vec2<f32>,
}

struct RsmSampleOffset {
    offset: vec2<f32>,
    weight: f32,
}

const INVALID_LAYER: u32 = 0xFFFFFFFFu;
const PI: f32 = 3.14159265;
const LIGHT_TYPE_DIRECTIONAL : u32 = 0u;
const LIGHT_TYPE_SPOT : u32 = 1u;
const LIGHT_TYPE_POINT : u32 = 2u;

@group(0) @binding(0) var<uniform> uniforms: RsmIndirectDiffuseUniforms;
@group(0) @binding(1) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(2) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(3) var<storage, read> cameraState: CameraState;
@group(0) @binding(4) var<storage, read> directionalLights: array<DirectionalLight>;
@group(0) @binding(5) var depthTexture: texture_depth_2d;
@group(0) @binding(6) var normalTexture: texture_2d<f32>;
@group(0) @binding(7) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(8) var rsmFluxTexture: texture_2d<f32>;
@group(0) @binding(9) var rsmNormalTexture: texture_2d<f32>;
@group(0) @binding(10) var rsmPositionTexture: texture_2d<f32>;
@group(0) @binding(11) var outputTexture: texture_storage_2d<rgba16float, write>;

fn light_first_layer(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.y;
}

fn light_type(light_index: u32) -> u32 {
    return directionalLights[light_index].metadata.x;
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
    let outer_angle = clamp(directionalLights[light_index].params.y, 0.0, PI);
    let cos_outer = cos(outer_angle * 0.5);
    return dot(normalize(to_receiver), light_direction) >= cos_outer;
}

fn reconstruct_world_position(pixel_coord: vec2<u32>) -> vec3<f32> {
    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    let uv = vsm_texel_center_uv(pixel_coord, textureDimensions(depthTexture));
    return vsm_reconstruct_world_position_from_uv_depth(uv, depth, cameraState.inverseViewProjection);
}

fn try_load_normal(pixel_coord: vec2<u32>, normal_out: ptr<function, vec3<f32>>) -> bool {
    let normal = textureLoad(normalTexture, vec2<i32>(pixel_coord), 0).xyz;
    let len_sq = dot(normal, normal);
    if (len_sq <= 1.0e-8) {
        return false;
    }
    (*normal_out) = normal * inverseSqrt(len_sq);
    return true;
}

fn select_cascade_level(world_position: vec3<f32>) -> u32 {
    return vsm_select_preferred_clipmap_level(
        world_position,
        cameraState.cameraPosition.xyz,
        uniforms.firstCascadeWorldExtent,
        uniforms.cascadeCount);
}

fn select_directional_receiver_layer(world_position: vec3<f32>, light_index: u32) -> i32 {
    if (uniforms.activeLights == 0u || uniforms.activeLayers == 0u || uniforms.cascadeCount == 0u) {
        return -1;
    }

    let base_layer = light_first_layer(light_index);
    if (base_layer >= uniforms.activeLayers) {
        return -1;
    }

    let available_cascades = min(min(uniforms.cascadeCount, light_layer_count(light_index)), uniforms.activeLayers - base_layer);
    var cascade = select_cascade_level(world_position);
    loop {
        if (cascade >= available_cascades) {
            break;
        }
        let layer = base_layer + cascade;
        if (vsm_cascade_contains_world_position(world_position, cascadeMatrices[layer].viewProj)) {
            return i32(layer);
        }
        cascade = cascade + 1u;
    }

    return -1;
}

fn resolve_fallback_layer(layer: u32) -> u32 {
    let light_index = cascadeMatrices[layer].lightIndex;
    let kind = light_type(light_index);
    var fallback_layer = INVALID_LAYER;

    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        if (uniforms.fallbackCascadeOffset == 0u) {
            return INVALID_LAYER;
        }

        let fallback_cascade = cascadeMatrices[layer].cascadeIndex + uniforms.fallbackCascadeOffset;
        if (fallback_cascade >= uniforms.cascadeCount) {
            return INVALID_LAYER;
        }
        fallback_layer = light_first_layer(light_index) + fallback_cascade;
    } else {
        let fallback_mip = cascadeMatrices[layer].mipLevel + 1u;
        if (fallback_mip >= light_mip_count(light_index)) {
            return INVALID_LAYER;
        }

        fallback_layer = select(
            light_first_layer(light_index) + fallback_mip,
            point_light_layer(light_index, cascadeMatrices[layer].faceIndex, fallback_mip),
            kind == LIGHT_TYPE_POINT);
    }

    if (fallback_layer >= uniforms.activeLayers) {
        return INVALID_LAYER;
    }

    return fallback_layer;
}

fn try_project_world_to_virtual_texel(world_position: vec3<f32>, layer: u32, virtual_texel_out: ptr<function, vec2<f32>>) -> bool {
    let ndc = vsm_project_world_to_cascade_ndc(world_position, cascadeMatrices[layer].viewProj);
    if (!vsm_cascade_ndc_is_covered(ndc)) {
        return false;
    }

    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let virtual_resolution = f32(max(layer_page_table_resolution(layer) * uniforms.physicalPageSize, 1u));
    (*virtual_texel_out) = uv * vec2<f32>(virtual_resolution, virtual_resolution);
    return true;
}

fn select_receiver_layer_for_light(pixel_coord: vec2<u32>, depth: f32, world_position: vec3<f32>, light_index: u32) -> i32 {
    if (light_index >= uniforms.activeLights || !light_affects_world_position(light_index, world_position)) {
        return -1;
    }

    let kind = light_type(light_index);
    if (kind == LIGHT_TYPE_DIRECTIONAL) {
        return select_directional_receiver_layer(world_position, light_index);
    }

    var base_layer = light_first_layer(light_index);
    if (kind == LIGHT_TYPE_POINT) {
        let face = point_light_primary_face(directionalLights[light_index].position.xyz, world_position);
        base_layer = point_light_layer(light_index, face, 0u);
    }

    var layer = 0u;
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
        return -1;
    }

    return i32(layer);
}

fn resolve_receiver(pixel_coord: vec2<u32>, light_index: u32) -> ReceiverState {
    var state: ReceiverState;
    state.valid = false;
    state.layer = 0u;
    state.virtualTexel = vec2<f32>(0.0, 0.0);
    state.normal = vec3<f32>(0.0, 1.0, 0.0);
    state.worldPosition = vec3<f32>(0.0, 0.0, 0.0);

    let depth = textureLoad(depthTexture, vec2<i32>(pixel_coord), 0);
    if (depth <= 0.0) {
        return state;
    }

    var normal = vec3<f32>(0.0, 0.0, 0.0);
    if (!try_load_normal(pixel_coord, &normal)) {
        return state;
    }

    let world_position = reconstruct_world_position(pixel_coord);
    let layer_i = select_receiver_layer_for_light(pixel_coord, depth, world_position, light_index);
    if (layer_i < 0) {
        return state;
    }

    var virtual_texel = vec2<f32>(0.0, 0.0);
    if (!try_project_world_to_virtual_texel(world_position + normal * uniforms.normalBias, u32(layer_i), &virtual_texel)) {
        return state;
    }

    let preferred_layer = u32(layer_i);
    if (!try_resolve_rsm_texel(preferred_layer, virtual_texel).valid) {
        let fallback_layer = resolve_fallback_layer(preferred_layer);
        if (fallback_layer == INVALID_LAYER ||
            !try_project_world_to_virtual_texel(world_position + normal * uniforms.normalBias, fallback_layer, &virtual_texel) ||
            !try_resolve_rsm_texel(fallback_layer, virtual_texel).valid) {
            return state;
        }
        state.layer = fallback_layer;
    } else {
        state.layer = preferred_layer;
    }

    state.valid = true;
    state.virtualTexel = virtual_texel;
    state.normal = normal;
    state.worldPosition = world_position;
    return state;
}

fn try_resolve_rsm_texel(layer: u32, virtual_texel: vec2<f32>) -> RsmSample {
    var sample: RsmSample;
    sample.valid = false;
    sample.texel = vec2<i32>(0, 0);
    sample.atlasTexel = vec2<f32>(0.0, 0.0);

    let layer_resolution = layer_page_table_resolution(layer);
    let virtual_resolution_u = max(layer_resolution * uniforms.physicalPageSize, 1u);
    let virtual_resolution = vec2<f32>(f32(virtual_resolution_u), f32(virtual_resolution_u));
    if (virtual_texel.x < 0.0 || virtual_texel.y < 0.0 ||
        virtual_texel.x >= virtual_resolution.x || virtual_texel.y >= virtual_resolution.y) {
        return sample;
    }

    let uv = clamp(virtual_texel / virtual_resolution, vec2<f32>(0.0, 0.0), vec2<f32>(0.99999994, 0.99999994));
    let virtual_sample = vsm_virtual_uv_to_page_sample(uv, layer_resolution);
    let wrapped_page_i = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(i32(virtual_sample.pageCoords.x), i32(virtual_sample.pageCoords.y)),
        cascadeStates[layer].pageOffset,
        i32(layer_resolution));
    if (wrapped_page_i.x < 0 || wrapped_page_i.y < 0) {
        return sample;
    }

    let wrapped_page = vec2<u32>(u32(wrapped_page_i.x), u32(wrapped_page_i.y));
    let entry = virtualPageTable[vsm_page_table_index(layer, wrapped_page, uniforms.pageTableResolution)];
    if (!get_is_allocated(entry)) {
        return sample;
    }

    let local_uv = vsm_contract_page_local_uv_for_filtering(virtual_sample.pageLocalUV, uniforms.physicalPageSize);
    let atlas_pixel = vsm_physical_page_and_local_uv_to_atlas_pixel(
        vec2<u32>(get_page_x(entry), get_page_y(entry)),
        local_uv,
        uniforms.physicalPageSize);
    let scale = f32(max(uniforms.rsmAtlasResolution, 1u)) / f32(max(uniforms.physicalAtlasResolution, 1u));
    let rsm_dims = vec2<i32>(max(textureDimensions(rsmFluxTexture), vec2<u32>(1u, 1u)));
    sample.atlasTexel = atlas_pixel * scale;
    sample.texel = clamp(vec2<i32>(floor(sample.atlasTexel)), vec2<i32>(0, 0), rsm_dims - vec2<i32>(1, 1));
    sample.valid = true;
    return sample;
}

fn rsm_payload_texel_is_valid(texel: vec2<i32>) -> bool {
    return textureLoad(rsmPositionTexture, texel, 0).a > 0.0;
}

fn try_resolve_rsm_payload_texel(layer: u32, virtual_texel: vec2<f32>) -> RsmSample {
    var sample = try_resolve_rsm_texel(layer, virtual_texel);
    if (sample.valid && !rsm_payload_texel_is_valid(sample.texel)) {
        sample.valid = false;
    }
    return sample;
}

fn oct_decode(encoded: vec2<f32>) -> vec3<f32> {
    var f = encoded * 2.0 - vec2<f32>(1.0, 1.0);
    var n = vec3<f32>(f.x, f.y, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) {
        let old_x = n.x;
        n.x = (1.0 - abs(n.y)) * select(-1.0, 1.0, old_x >= 0.0);
        n.y = (1.0 - abs(old_x)) * select(-1.0, 1.0, n.y >= 0.0);
    }
    return normalize(n);
}

fn hash_u32(value: u32) -> u32 {
    var x = value;
    x = (x ^ (x >> 16u)) * 2246822519u;
    x = (x ^ (x >> 13u)) * 3266489917u;
    return x ^ (x >> 16u);
}

fn hash_sequence_sample(pixel: vec2<u32>, light_index: u32, sample_index: u32, frame_index: u32, salt: u32) -> f32 {
    var seed = hash_u32(light_index * 0x27D4EB2Du + sample_index);
    seed = seed ^ hash_u32(pixel.x * 0x9E3779B9u);
    seed = seed ^ hash_u32(pixel.y * 0x85EBCA6Bu);
    seed = seed ^ hash_u32(frame_index * 0xC2B2AE35u);
    seed = seed ^ hash_u32(salt * 0x165667B1u);
    return f32(hash_u32(seed) & 0x00FFFFFFu) / f32(0x01000000u);
}

fn sample_offset(pixel: vec2<u32>, light_index: u32, sample_index: u32, sample_count: u32, radius_texels: f32) -> RsmSampleOffset {
    let count = max(sample_count, 1u);
    let frame_phase = uniforms.frameIndex & 63u;
    let sequence_index = (sample_index + frame_phase) % count;
    let radius_jitter = hash_sequence_sample(pixel, light_index, sequence_index, uniforms.frameIndex, 17u);
    let angle_jitter = hash_sequence_sample(pixel, light_index, sequence_index, uniforms.frameIndex, 53u);
    let radial = clamp((f32(sequence_index) + radius_jitter) / f32(count), 0.0, 0.999999);
    let radius = radial * radius_texels;
    let angle = fract(angle_jitter + f32(sequence_index) * 0.61803398875 + f32(frame_phase) * 0.38196601125) * 2.0 * PI;
    var sample: RsmSampleOffset;
    sample.offset = vec2<f32>(cos(angle), sin(angle)) * radius;
    sample.weight = max(radial * radial, 1.0e-4);
    return sample;
}

fn choose_stochastic_rsm_payload_texel(sample: RsmSample, light_index: u32, sample_index: u32) -> RsmSample {
    if (!sample.valid) {
        return sample;
    }

    let rsm_dims = vec2<i32>(max(textureDimensions(rsmPositionTexture), vec2<u32>(1u, 1u)));
    let coord = clamp(sample.atlasTexel, vec2<f32>(0.0, 0.0), vec2<f32>(rsm_dims - vec2<i32>(1, 1)));
    let base = vec2<i32>(floor(coord));
    let frac = fract(coord);

    var seed = hash_u32(u32(base.x) * 0x8DA6B343u);
    seed = seed ^ hash_u32(u32(base.y) * 0xD8163841u);
    seed = seed ^ hash_u32(light_index * 0xCB1AB31Fu + sample_index);
    seed = seed ^ hash_u32(uniforms.frameIndex * 0xC2B2AE35u);
    seed = seed ^ hash_u32(91u) ^ hash_u32(0x165667B1u);
    let r = f32(hash_u32(seed) & 0x00FFFFFFu) / f32(0x01000000u);
    let w00 = (1.0 - frac.x) * (1.0 - frac.y);
    let w10 = frac.x * (1.0 - frac.y);
    let w01 = (1.0 - frac.x) * frac.y;
    var chosen_offset = vec2<i32>(1, 1);
    if (r < w00) {
        chosen_offset = vec2<i32>(0, 0);
    } else if (r < w00 + w10) {
        chosen_offset = vec2<i32>(1, 0);
    } else if (r < w00 + w10 + w01) {
        chosen_offset = vec2<i32>(0, 1);
    }

    var resolved = sample;
    resolved.texel = clamp(base + chosen_offset, vec2<i32>(0, 0), rsm_dims - vec2<i32>(1, 1));
    resolved.atlasTexel = vec2<f32>(resolved.texel);
    if (rsm_payload_texel_is_valid(resolved.texel)) {
        return resolved;
    }
    resolved.valid = false;
    return resolved;
}

fn clamp_luminance(value: vec3<f32>) -> vec3<f32> {
    let max_luminance = max(uniforms.fireflyClamp, 1.0e-4);
    let luminance = max(dot(value, vec3<f32>(0.2126, 0.7152, 0.0722)), 0.0);
    let scale = min(1.0, max_luminance / max(luminance, 1.0e-4));
    return value * scale;
}

fn evaluate_rsm_sample(receiver: ReceiverState, sample_texel: vec2<i32>) -> vec3<f32> {
    // The RSM flux atlas stores reflected radiant flux. Each texel is gathered as
    // a cosine-emitting pixel light, matching the Dachsbacher RSM estimator.
    let reflected_flux = max(textureLoad(rsmFluxTexture, sample_texel, 0).rgb, vec3<f32>(0.0, 0.0, 0.0));
    let emitter_normal = oct_decode(textureLoad(rsmNormalTexture, sample_texel, 0).rg);
    let emitter_position_sample = textureLoad(rsmPositionTexture, sample_texel, 0);
    if (emitter_position_sample.a <= 0.0 || dot(reflected_flux, vec3<f32>(1.0, 1.0, 1.0)) <= 1.0e-6) {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let emitter_position = emitter_position_sample.xyz;
    let emitter_to_receiver = receiver.worldPosition - emitter_position;
    let distance_sq = dot(emitter_to_receiver, emitter_to_receiver);
    let min_distance_sq = uniforms.minDistance * uniforms.minDistance;
    let max_distance_sq = uniforms.maxDistance * uniforms.maxDistance;
    if (distance_sq < min_distance_sq || distance_sq > max_distance_sq) {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let emitter_cos = max(dot(emitter_normal, emitter_to_receiver), 0.0);
    let receiver_cos = max(dot(receiver.normal, -emitter_to_receiver), 0.0);
    let geometric_term = emitter_cos * receiver_cos / max(distance_sq * distance_sq, 1.0e-4);
    return reflected_flux * geometric_term;
}

fn store_center_debug(pixel: vec2<u32>, receiver: ReceiverState) -> bool {
    let rsm_sample = try_resolve_rsm_payload_texel(receiver.layer, receiver.virtualTexel);
    if (uniforms.debugMode == 4u) {
        let color = select(vec3<f32>(1.0, 0.85, 0.0), vec3<f32>(0.0, 1.0, 0.0), rsm_sample.valid);
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(color, 1.0));
        return true;
    }

    if (!rsm_sample.valid) {
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return true;
    }

    if (uniforms.debugMode == 1u) {
        let flux = max(textureLoad(rsmFluxTexture, rsm_sample.texel, 0).rgb, vec3<f32>(0.0, 0.0, 0.0));
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(flux, 1.0));
        return true;
    }

    if (uniforms.debugMode == 2u) {
        let normal = oct_decode(textureLoad(rsmNormalTexture, rsm_sample.texel, 0).rg);
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(normal * 0.5 + vec3<f32>(0.5), 1.0));
        return true;
    }

    if (uniforms.debugMode == 3u) {
        let position = textureLoad(rsmPositionTexture, rsm_sample.texel, 0);
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(position.xyz, position.a));
        return true;
    }

    return false;
}

fn find_debug_receiver(pixel: vec2<u32>) -> ReceiverState {
    var empty: ReceiverState;
    empty.valid = false;
    empty.layer = 0u;
    empty.virtualTexel = vec2<f32>(0.0, 0.0);
    empty.normal = vec3<f32>(0.0, 1.0, 0.0);
    empty.worldPosition = vec3<f32>(0.0, 0.0, 0.0);

    let light_count = min(uniforms.activeLights, arrayLength(&directionalLights));
    for (var light_index = 0u; light_index < light_count; light_index = light_index + 1u) {
        let receiver = resolve_receiver(pixel, light_index);
        if (receiver.valid) {
            return receiver;
        }
    }
    return empty;
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let pixel = global_id.xy;
    if (pixel.x >= uniforms.width || pixel.y >= uniforms.height) {
        return;
    }

    if (uniforms.enabled == 0u) {
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    if (uniforms.debugMode != 0u) {
        let receiver = find_debug_receiver(pixel);
        if (!receiver.valid) {
            let color = select(vec3<f32>(0.0, 0.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), uniforms.debugMode == 4u);
            textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(color, 1.0));
            return;
        }

        if (store_center_debug(pixel, receiver)) {
            return;
        }
    }

    let sample_count = min(max(uniforms.sampleCount, 1u), 64u);
    var sum = vec3<f32>(0.0, 0.0, 0.0);
    var valid_count = 0u;
    var attempted_count = 0u;
    let light_count = min(uniforms.activeLights, arrayLength(&directionalLights));
    for (var light_index = 0u; light_index < light_count; light_index = light_index + 1u) {
        let receiver = resolve_receiver(pixel, light_index);
        if (!receiver.valid) {
            continue;
        }

        let virtual_texel_world_size = cascadeMatrices[receiver.layer].pageWorldSize / f32(max(uniforms.physicalPageSize, 1u));
        let sample_radius_world = max(uniforms.sampleRadiusWorld, virtual_texel_world_size);
        let sample_radius_texels = sample_radius_world / max(virtual_texel_world_size, 1.0e-6);
        var light_sum = vec3<f32>(0.0, 0.0, 0.0);
        var light_weight_sum = 0.0;
        var attempted_weight_sum = 0.0;
        var light_valid_count = 0u;
        attempted_count = attempted_count + sample_count;
        for (var i = 0u; i < 64u; i = i + 1u) {
            if (i >= sample_count) {
                break;
            }

            let sample = sample_offset(pixel, light_index, i, sample_count, sample_radius_texels);
            attempted_weight_sum = attempted_weight_sum + sample.weight;
            let rsm_sample = choose_stochastic_rsm_payload_texel(
                try_resolve_rsm_texel(receiver.layer, receiver.virtualTexel + sample.offset),
                light_index,
                i);
            if (!rsm_sample.valid) {
                continue;
            }
            light_sum = light_sum + evaluate_rsm_sample(receiver, rsm_sample.texel) * sample.weight;
            light_weight_sum = light_weight_sum + sample.weight;
            light_valid_count = light_valid_count + 1u;
        }

        if (light_valid_count > 0u) {
            let sample_disk_texel_count = PI * sample_radius_texels * sample_radius_texels;
            let valid_weight_ratio = clamp(light_weight_sum / max(attempted_weight_sum, 1.0e-4), 0.0, 1.0);
            let sparse_sample_damping = smoothstep(0.05, 0.35, valid_weight_ratio);
            sum = sum + light_sum * (sample_disk_texel_count / max(attempted_weight_sum, 1.0e-4)) * sparse_sample_damping;
            valid_count = valid_count + light_valid_count;
        }
    }

    let valid_ratio = select(0.0, f32(valid_count) / f32(attempted_count), attempted_count > 0u);
    if (uniforms.debugMode == 5u) {
        textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(vec3<f32>(valid_ratio), 1.0));
        return;
    }
    textureStore(outputTexture, vec2<i32>(pixel), vec4<f32>(clamp_luminance(sum * uniforms.intensity), valid_ratio));
}
