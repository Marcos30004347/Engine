#include "../../virtualshadowmap/wgsl/vsm-common.wgsl"

struct DirectionalShadowUniforms {
    pageTableResolution : u32,
    activeLayers : u32,
    cascadeCount : u32,
    activeLights : u32,
    enabled : u32,
    physicalPageSize : u32,
    reverseZ : u32,
    fallbackCascadeOffset : u32,
    firstCascadeWorldExtent : f32,
    directionalShadowBias : f32,
    directionalMaxShadowBias : f32,
    pcfRadiusTexels : f32,
    directionalNormalBiasTexels : f32,
    shadowFilterTaps : u32,
    _padding0 : vec2<u32>,
};

struct DirectionalShadowCascadeState {
    pageOffset : vec2<i32>,
    pageShift : vec2<i32>,
    _padding : vec4<u32>,
};

struct DirectionalShadowCascadeMatrix {
    view : mat4x4<f32>,
    proj : mat4x4<f32>,
    viewProj : mat4x4<f32>,
    worldExtent : f32,
    pageWorldSize : f32,
    lightIndex : u32,
    cascadeIndex : u32,
    pageTableResolution : u32,
    lightType : u32,
    faceIndex : u32,
    mipLevel : u32,
};

struct DirectionalShadowCameraState {
    inverseView : mat4x4<f32>,
    inverseProjection : mat4x4<f32>,
    inverseViewProjection : mat4x4<f32>,
    viewProjection : mat4x4<f32>,
    cameraPosition : vec4<f32>,
    reverseZ : u32,
    _padding : vec3<u32>,
};

fn directional_shadow_layer_page_table_resolution(layer : u32) -> u32 {
    return max(directionalShadowCascadeMatrices[layer].pageTableResolution, 1u);
}

fn directional_shadow_texel_world_size(layer : u32) -> f32 {
    return directionalShadowCascadeMatrices[layer].pageWorldSize / f32(max(directionalShadowUniforms.physicalPageSize, 1u));
}

fn directional_shadow_bias_scale_for_layer(layer : u32) -> f32 {
    let lightIndex = directionalShadowCascadeMatrices[layer].lightIndex;
    let baseLayer = directionalLights[lightIndex].metadata.y;
    if (baseLayer >= directionalShadowUniforms.activeLayers) {
        return 1.0;
    }

    let baseTexelWorldSize = max(directional_shadow_texel_world_size(baseLayer), 1.0e-6);
    let currentTexelWorldSize = max(directional_shadow_texel_world_size(layer), 1.0e-6);
    return max(currentTexelWorldSize / baseTexelWorldSize, 1.0);
}

fn directional_shadow_select_cascade_level(worldPosition : vec3<f32>) -> u32 {
    return vsm_select_preferred_clipmap_level(
        worldPosition,
        directionalShadowCameraState.cameraPosition.xyz,
        directionalShadowUniforms.firstCascadeWorldExtent,
        max(directionalShadowUniforms.cascadeCount, 1u));
}

fn directional_shadow_cascade_contains_world_position(worldPosition : vec3<f32>, layer : u32) -> bool {
    return vsm_cascade_contains_world_position(worldPosition, directionalShadowCascadeMatrices[layer].viewProj);
}

fn directional_shadow_find_covering_cascade_level(lightIndex : u32, worldPosition : vec3<f32>) -> i32 {
    let baseLayer = directionalLights[lightIndex].metadata.y;
    if (baseLayer >= directionalShadowUniforms.activeLayers) {
        return -1;
    }

    let preferredCascade = directional_shadow_select_cascade_level(worldPosition);
    let availableCascades = min(directionalShadowUniforms.cascadeCount, directionalShadowUniforms.activeLayers - baseLayer);
    var cascade = preferredCascade;
    loop {
        if (cascade >= availableCascades) {
            break;
        }

        let layer = baseLayer + cascade;
        if (directional_shadow_cascade_contains_world_position(worldPosition, layer)) {
            return i32(layer);
        }

        cascade = cascade + 1u;
    }

    return -1;
}

fn directional_shadow_resolve_fallback_layer(layer : u32) -> u32 {
    if (directionalShadowUniforms.fallbackCascadeOffset == 0u) {
        return 0xffffffffu;
    }

    let lightIndex = directionalShadowCascadeMatrices[layer].lightIndex;
    let fallbackCascade = directionalShadowCascadeMatrices[layer].cascadeIndex + directionalShadowUniforms.fallbackCascadeOffset;
    if (fallbackCascade >= directionalShadowUniforms.cascadeCount) {
        return 0xffffffffu;
    }

    let fallbackLayer = directionalLights[lightIndex].metadata.y + fallbackCascade;
    if (fallbackLayer >= directionalShadowUniforms.activeLayers) {
        return 0xffffffffu;
    }
    return fallbackLayer;
}

fn directional_shadow_try_project_world_to_virtual_texel(
    worldPosition : vec3<f32>,
    layer : u32,
    receiverDepthOut : ptr<function, f32>,
    virtualTexelOut : ptr<function, vec2<f32>>
) -> bool {
    let ndc = vsm_project_world_to_cascade_ndc(worldPosition, directionalShadowCascadeMatrices[layer].viewProj);
    if (!vsm_cascade_ndc_is_covered(ndc)) {
        return false;
    }

    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let virtualResolution = f32(max(directional_shadow_layer_page_table_resolution(layer) * directionalShadowUniforms.physicalPageSize, 1u));
    (*receiverDepthOut) = ndc.z;
    (*virtualTexelOut) = uv * vec2<f32>(virtualResolution, virtualResolution);
    return true;
}

fn directional_shadow_clamp_virtual_texel(layer : u32, virtualTexel : vec2<f32>) -> vec2<f32> {
    let virtualResolution = f32(max(directional_shadow_layer_page_table_resolution(layer) * directionalShadowUniforms.physicalPageSize, 1u));
    let inset = 0.5;
    return clamp(
        virtualTexel,
        vec2<f32>(inset, inset),
        vec2<f32>(virtualResolution - inset, virtualResolution - inset));
}

fn directional_shadow_try_resolve_shadow_texel(
    layer : u32,
    virtualTexel : vec2<f32>,
    storedDepthOut : ptr<function, f32>
) -> bool {
    let layerPageTableResolution = directional_shadow_layer_page_table_resolution(layer);
    let virtualResolution = vec2<f32>(f32(max(layerPageTableResolution * directionalShadowUniforms.physicalPageSize, 1u)));
    if (virtualTexel.x < 0.0 || virtualTexel.y < 0.0 ||
        virtualTexel.x >= virtualResolution.x || virtualTexel.y >= virtualResolution.y) {
        return false;
    }

    let uv = clamp(virtualTexel / virtualResolution, vec2<f32>(0.0), vec2<f32>(0.99999994));
    let virtualSample = vsm_virtual_uv_to_page_sample(uv, layerPageTableResolution);
    let wrappedPageI = virtual_page_coords_to_wrapped_coords(
        vec2<i32>(i32(virtualSample.pageCoords.x), i32(virtualSample.pageCoords.y)),
        directionalShadowCascadeStates[layer].pageOffset,
        i32(layerPageTableResolution));
    if (wrappedPageI.x < 0 || wrappedPageI.y < 0) {
        return false;
    }

    let wrappedPage = vec2<u32>(u32(wrappedPageI.x), u32(wrappedPageI.y));
    let vptEntry = directionalShadowVirtualPageTable[
        vsm_page_table_index(layer, wrappedPage, directionalShadowUniforms.pageTableResolution)];
    if (!get_is_allocated(vptEntry) || get_is_dirty(vptEntry)) {
        return false;
    }

    let contractedLocalUv = vsm_contract_page_local_uv_for_filtering(virtualSample.pageLocalUV, directionalShadowUniforms.physicalPageSize);
    let atlasPixel = vsm_physical_page_and_local_uv_to_atlas_pixel(
        vec2<u32>(get_page_x(vptEntry), get_page_y(vptEntry)),
        contractedLocalUv,
        directionalShadowUniforms.physicalPageSize);
    let atlasResolution = vec2<i32>(max(textureDimensions(directionalShadowAtlasTexture), vec2<u32>(1u, 1u)));
    let atlasTexel = clamp(vec2<i32>(floor(atlasPixel)), vec2<i32>(0, 0), atlasResolution - vec2<i32>(1, 1));
    (*storedDepthOut) = vsm_load_shadow_depth(directionalShadowAtlasTexture, atlasTexel);
    return true;
}

fn directional_shadow_compute_normal_bias_depth(
    worldPosition : vec3<f32>,
    layer : u32,
    receiverDepth : f32,
    receiverNormal : vec3<f32>
) -> f32 {
    let normalBiasTexels = max(directionalShadowUniforms.directionalNormalBiasTexels, 0.0);
    if (normalBiasTexels <= 1.0e-4) {
        return 0.0;
    }

    let lightIndex = directionalShadowCascadeMatrices[layer].lightIndex;
    let toLight = light_surface_direction(lightIndex, worldPosition);
    let cosAngle = clamp(1.0 - dot(toLight, receiverNormal), 0.0, 1.0);
    let normalBiasWorld = normalBiasTexels * cosAngle * directional_shadow_texel_world_size(layer);
    if (normalBiasWorld <= 1.0e-6) {
        return 0.0;
    }

    var plusReceiverDepth = 0.0;
    var plusVirtualTexel = vec2<f32>(0.0);
    let plusValid = directional_shadow_try_project_world_to_virtual_texel(
        worldPosition + receiverNormal * normalBiasWorld,
        layer,
        &plusReceiverDepth,
        &plusVirtualTexel);

    var minusReceiverDepth = 0.0;
    var minusVirtualTexel = vec2<f32>(0.0);
    let minusValid = directional_shadow_try_project_world_to_virtual_texel(
        worldPosition - receiverNormal * normalBiasWorld,
        layer,
        &minusReceiverDepth,
        &minusVirtualTexel);

    if (plusValid && (!minusValid || select(plusReceiverDepth < minusReceiverDepth, plusReceiverDepth > minusReceiverDepth, directionalShadowUniforms.reverseZ != 0u))) {
        return abs(plusReceiverDepth - receiverDepth);
    }
    if (minusValid) {
        return abs(minusReceiverDepth - receiverDepth);
    }

    return 0.0;
}

fn directional_shadow_compare(layer : u32, virtualTexel : vec2<f32>, receiverDepth : f32, bias : f32, fallbackVisibility : f32) -> f32 {
    var storedDepth = 0.0;
    if (!directional_shadow_try_resolve_shadow_texel(layer, virtualTexel, &storedDepth)) {
        return fallbackVisibility;
    }
    return vsm_shadow_compare(storedDepth, receiverDepth, bias, directionalShadowUniforms.reverseZ != 0u);
}

fn directional_shadow_evaluate_pcf(layer : u32, virtualTexel : vec2<f32>, receiverDepth : f32, bias : f32) -> f32 {
    let centerVirtualTexel = directional_shadow_clamp_virtual_texel(layer, virtualTexel);
    let centerVisibility = directional_shadow_compare(layer, centerVirtualTexel, receiverDepth, bias, 1.0);
    let tapCount = max(directionalShadowUniforms.shadowFilterTaps, 1u);
    let filterRadius = max(directionalShadowUniforms.pcfRadiusTexels, 0.0);
    if (tapCount <= 1u || filterRadius <= 1.0e-4) {
        return centerVisibility;
    }

    let gridSize = select(4u, select(3u, 2u, tapCount <= 4u), tapCount <= 9u);
    let stride = (2.0 * filterRadius) / f32(max(gridSize - 1u, 1u));
    let start = -filterRadius;
    var total = 0.0;
    var count = 0u;
    for (var y = 0u; y < gridSize; y++) {
        for (var x = 0u; x < gridSize; x++) {
            let offset = vec2<f32>(start + stride * f32(x), start + stride * f32(y));
            let sampleVirtualTexel = directional_shadow_clamp_virtual_texel(layer, centerVirtualTexel + offset);
            total += directional_shadow_compare(layer, sampleVirtualTexel, receiverDepth, bias, centerVisibility);
            count += 1u;
        }
    }

    return total / f32(max(count, 1u));
}

fn evaluate_directional_shadow_visibility(lightIndex : u32, worldPosition : vec3<f32>, surfaceNormal : vec3<f32>) -> f32 {
    if (directionalShadowUniforms.enabled == 0u ||
        lightIndex >= directionalShadowUniforms.activeLights ||
        light_type(lightIndex) != LIGHT_TYPE_DIRECTIONAL) {
        return 1.0;
    }

    let preferredLayer = directional_shadow_find_covering_cascade_level(lightIndex, worldPosition);
    if (preferredLayer < 0) {
        return 1.0;
    }

    var resolvedLayer = u32(preferredLayer);
    var receiverDepth = 0.0;
    var virtualTexel = vec2<f32>(0.0);
    if (!directional_shadow_try_project_world_to_virtual_texel(worldPosition, resolvedLayer, &receiverDepth, &virtualTexel)) {
        return 1.0;
    }

    var storedDepth = 0.0;
    if (!directional_shadow_try_resolve_shadow_texel(resolvedLayer, directional_shadow_clamp_virtual_texel(resolvedLayer, virtualTexel), &storedDepth)) {
        let fallbackLayer = directional_shadow_resolve_fallback_layer(resolvedLayer);
        if (fallbackLayer == 0xffffffffu) {
            return 1.0;
        }

        var fallbackReceiverDepth = 0.0;
        var fallbackVirtualTexel = vec2<f32>(0.0);
        if (!directional_shadow_try_project_world_to_virtual_texel(worldPosition, fallbackLayer, &fallbackReceiverDepth, &fallbackVirtualTexel)) {
            return 1.0;
        }
        if (!directional_shadow_try_resolve_shadow_texel(fallbackLayer, directional_shadow_clamp_virtual_texel(fallbackLayer, fallbackVirtualTexel), &storedDepth)) {
            return 1.0;
        }

        resolvedLayer = fallbackLayer;
        receiverDepth = fallbackReceiverDepth;
        virtualTexel = fallbackVirtualTexel;
    }

    let biasScale = directional_shadow_bias_scale_for_layer(resolvedLayer);
    var bias = max(directionalShadowUniforms.directionalShadowBias, 0.0) * biasScale;
    bias = max(bias, directional_shadow_compute_normal_bias_depth(worldPosition, resolvedLayer, receiverDepth, surfaceNormal));
    let maxBias = max(directionalShadowUniforms.directionalMaxShadowBias, 0.0) * biasScale;
    if (maxBias > 0.0) {
        bias = min(bias, maxBias);
    }

    return directional_shadow_evaluate_pcf(resolvedLayer, virtualTexel, receiverDepth, max(bias, 0.0));
}
