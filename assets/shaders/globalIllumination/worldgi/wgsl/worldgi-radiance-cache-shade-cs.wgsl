#include "globalIllumination/worldgi/wgsl/worldgi-radiance-cache.wgsl"

struct WorldGIRadianceCacheShadeUniforms {
    activeCapacity : u32,
    frameIndex : u32,
    enabled : u32,
    lightGridEnabled : u32,
    lightGridResolution : vec3<u32>,
    lightGridCascadeCount : u32,
    lightGridMaxItemsPerCell : u32,
    lightCount : u32,
    cacheCapacity : u32,
    lightBVHEnabled : u32,
    lightGridCenterBaseExtent : vec4<f32>,
    lightGridCascadeScalePadding : vec4<f32>,
    clampHysteresisPadding : vec4<f32>,
};

const INV_PI : f32 = 0.31830988618;

@group(0) @binding(0) var<uniform> uniforms : WorldGIRadianceCacheShadeUniforms;
@group(0) @binding(1) var<storage, read_write> cacheEntries : array<GIWorldRadianceCacheEntry>;
@group(0) @binding(2) var<storage, read> activeList : array<GIWorldRadianceCacheActiveItem>;
@group(0) @binding(3) var<storage, read_write> counters : GIWorldRadianceCacheCounters;
@group(0) @binding(4) var<storage, read> traceSurfaces : array<GITraceSurface>;
@group(0) @binding(5) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(6) var<storage, read> lightGridCells : array<WorldGILightGridCell>;
@group(0) @binding(7) var<storage, read> lightGridItemIds : array<u32>;
@group(0) @binding(8) var<storage, read> lightBVHNodes : array<WorldGILightBVHNode>;
@group(0) @binding(9) var<storage, read> lightBVHInfo : WorldGILightBVHInfo;

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn surface_is_valid(surface : GITraceSurface) -> bool {
    return (surface.sourceFlags.y & GI_TRACE_SURFACE_FLAG_VALID) != 0u &&
        surface.sourceFlags.x != GI_TRACE_SURFACE_SOURCE_INVALID &&
        surface.geometryNormalConfidence.w > 0.0 &&
        surface.worldPositionHitT.w >= 0.0;
}

fn light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn shade_light_at_hit(light : WorldGILight, position : vec3<f32>, normal : vec3<f32>, baseColor : vec3<f32>) -> vec3<f32> {
    if (!light_is_enabled(light)) {
        return vec3<f32>(0.0);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let lightDirection = safe_normalize(-light.directionType.xyz, vec3<f32>(0.0, 1.0, 0.0));
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0);
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT && light.positionRange.w > 0.0) {
        let toLight = light.positionRange.xyz - position;
        let distanceSq = max(dot(toLight, toLight), 1.0e-4);
        let distance = sqrt(distanceSq);
        let lightDirection = toLight / distance;
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - distance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / distanceSq;
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE && light.positionRange.w > 0.0) {
        let toCenter = light.positionRange.xyz - position;
        let centerDistance = max(length(toCenter), 1.0e-4);
        let lightDirection = toCenter / centerDistance;
        let surfaceDistance = max(centerDistance - max(light.shapeParams.x, 0.0), 1.0e-4);
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - surfaceDistance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / max(surfaceDistance * surfaceDistance, 1.0e-4);
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * INV_PI);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR && light.positionRange.w > 0.0) {
        let lightPosition = worldgi_planar_light_closest_point(light, position);
        let toLight = lightPosition - position;
        let distanceSq = max(dot(toLight, toLight), 1.0e-4);
        let distance = sqrt(distanceSq);
        let lightDirection = toLight / distance;
        let basis = worldgi_planar_light_basis(light);
        let emissionTerm = max(dot(basis.normal, -lightDirection), 0.0);
        let range = max(light.positionRange.w, 1.0e-4);
        let rangeFade = clamp(1.0 - distance / range, 0.0, 1.0);
        let attenuation = rangeFade * rangeFade / distanceSq;
        let nDotL = max(dot(normal, lightDirection), 0.0);
        let lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0) * attenuation;
        return baseColor * lightRadiance * (nDotL * emissionTerm * INV_PI);
    }

    return vec3<f32>(0.0);
}

fn light_grid_cell_for_position(position : vec3<f32>) -> u32 {
    let resolution = max(uniforms.lightGridResolution.x, 1u);
    let cascadeCount = max(uniforms.lightGridCascadeCount, 1u);
    let cellsPerCascade = resolution * resolution * resolution;

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex = cascadeIndex + 1u) {
        let extent = max(uniforms.lightGridCenterBaseExtent.w, 1.0e-3) *
            pow(max(uniforms.lightGridCascadeScalePadding.x, 1.0), f32(cascadeIndex));
        let cascadeMin = uniforms.lightGridCenterBaseExtent.xyz - vec3<f32>(extent * 0.5);
        let local = position - cascadeMin;
        if (all(local >= vec3<f32>(0.0)) && all(local < vec3<f32>(extent))) {
            let cellSize = extent / f32(resolution);
            let coord = floor(local / cellSize);
            let maxCoord = f32(resolution - 1u);
            let cellX = u32(clamp(coord.x, 0.0, maxCoord));
            let cellY = u32(clamp(coord.y, 0.0, maxCoord));
            let cellZ = u32(clamp(coord.z, 0.0, maxCoord));
            return cascadeIndex * cellsPerCascade + cellZ * resolution * resolution + cellY * resolution + cellX;
        }
    }

    return 0xffffffffu;
}

fn directional_lighting_fallback(position : vec3<f32>, normal : vec3<f32>, baseColor : vec3<f32>) -> vec3<f32> {
    var radiance = vec3<f32>(0.0);
    let cappedLightCount = min(uniforms.lightCount, arrayLength(&lights));
    for (var lightIndex = 0u; lightIndex < cappedLightCount; lightIndex = lightIndex + 1u) {
        let light = lights[lightIndex];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
            radiance += shade_light_at_hit(light, position, normal, baseColor);
        }
    }
    return radiance;
}

fn position_in_light_bvh_node(position : vec3<f32>, node : WorldGILightBVHNode) -> bool {
    return all(position >= node.boundsMinNodeType.xyz) && all(position <= node.boundsMaxImportance.xyz);
}

fn light_bvh_node_type(node : WorldGILightBVHNode) -> u32 {
    return u32(node.boundsMinNodeType.w + 0.5);
}

fn light_bvh_lighting_at_hit(position : vec3<f32>, normal : vec3<f32>, baseColor : vec3<f32>) -> vec3<f32> {
    if (uniforms.lightBVHEnabled == 0u || lightBVHInfo.nodeCount == 0u || lightBVHInfo.rootNodeIndex == WORLD_GI_LIGHT_BVH_INVALID_NODE) {
        return vec3<f32>(0.0);
    }

    var radiance = vec3<f32>(0.0);
    var stack : array<u32, 32>;
    var stackCount = 0u;
    stack[stackCount] = lightBVHInfo.rootNodeIndex;
    stackCount = stackCount + 1u;

    while (stackCount > 0u) {
        stackCount = stackCount - 1u;
        let nodeIndex = stack[stackCount];
        if (nodeIndex >= min(lightBVHInfo.nodeCount, arrayLength(&lightBVHNodes))) {
            continue;
        }

        let node = lightBVHNodes[nodeIndex];
        let nodeType = light_bvh_node_type(node);
        if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_INVALID) {
            continue;
        }
        if (nodeType != WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE && !position_in_light_bvh_node(position, node)) {
            continue;
        }

        if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_LEAF || nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE) {
            let lightIndex = node.childOrLightCount.z;
            if (lightIndex < min(uniforms.lightCount, arrayLength(&lights))) {
                radiance += shade_light_at_hit(lights[lightIndex], position, normal, baseColor);
            }
            continue;
        }

        if (node.childOrLightCount.x != WORLD_GI_LIGHT_BVH_INVALID_NODE && stackCount < 32u) {
            stack[stackCount] = node.childOrLightCount.x;
            stackCount = stackCount + 1u;
        }
        if (node.childOrLightCount.y != WORLD_GI_LIGHT_BVH_INVALID_NODE && stackCount < 32u) {
            stack[stackCount] = node.childOrLightCount.y;
            stackCount = stackCount + 1u;
        }
    }

    return radiance;
}

fn direct_lighting_at_hit(surface : GITraceSurface) -> vec3<f32> {
    let normal = safe_normalize(surface.shadingNormalRoughness.xyz, surface.geometryNormalConfidence.xyz);
    let baseColor = clamp(surface.baseColorOpacity.rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let position = surface.worldPositionHitT.xyz;
    var radiance = light_bvh_lighting_at_hit(position, normal, baseColor);

    if (uniforms.lightBVHEnabled == 0u && uniforms.lightGridEnabled != 0u) {
        let cellIndex = light_grid_cell_for_position(position);
        if (cellIndex != 0xffffffffu && cellIndex < arrayLength(&lightGridCells)) {
            let cell = lightGridCells[cellIndex];
            let itemCount = min(cell.itemCount.x, uniforms.lightGridMaxItemsPerCell);
            let itemBase = cellIndex * max(uniforms.lightGridMaxItemsPerCell, 1u);
            for (var itemIndex = 0u; itemIndex < itemCount; itemIndex = itemIndex + 1u) {
                let itemAddress = itemBase + itemIndex;
                if (itemAddress >= arrayLength(&lightGridItemIds)) {
                    break;
                }
                let lightIndex = lightGridItemIds[itemAddress];
                if (lightIndex < min(uniforms.lightCount, arrayLength(&lights))) {
                    radiance += shade_light_at_hit(lights[lightIndex], position, normal, baseColor);
                }
            }
        } else {
            radiance += directional_lighting_fallback(position, normal, baseColor);
        }
    }

    return min(radiance, vec3<f32>(max(uniforms.clampHysteresisPadding.x, 0.0)));
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let activeIndex = gid.x;
    if (uniforms.enabled == 0u) {
        return;
    }

    let activeCount = min(atomicLoad(&counters.activeCount), min(uniforms.activeCapacity, arrayLength(&activeList)));
    if (activeIndex >= activeCount) {
        return;
    }

    let activeItem = activeList[activeIndex].entrySurfaceProducerFlags;
    let entryIndex = activeItem.x;
    let surfaceIndex = activeItem.y;
    if (entryIndex >= min(uniforms.cacheCapacity, arrayLength(&cacheEntries)) || surfaceIndex >= arrayLength(&traceSurfaces)) {
        return;
    }

    let surface = traceSurfaces[surfaceIndex];
    if (!surface_is_valid(surface)) {
        return;
    }

    let directDiffuse = direct_lighting_at_hit(surface);
    let hysteresis = clamp(uniforms.clampHysteresisPadding.y, 0.0, 0.99);
    let previousDiffuse = cacheEntries[entryIndex].directDiffuseRadiance.rgb;
    let confidence = clamp(surface.geometryNormalConfidence.w, 0.0, 1.0);
    cacheEntries[entryIndex].worldPositionCellSize = vec4<f32>(surface.worldPositionHitT.xyz, cacheEntries[entryIndex].worldPositionCellSize.w);
    cacheEntries[entryIndex].normalConfidence = surface.geometryNormalConfidence;
    cacheEntries[entryIndex].baseColorRoughness = vec4<f32>(surface.baseColorOpacity.rgb, surface.shadingNormalRoughness.w);
    cacheEntries[entryIndex].emissiveRadiance = vec4<f32>(max(surface.emissiveRadiance.rgb, vec3<f32>(0.0)), confidence);
    cacheEntries[entryIndex].directDiffuseRadiance = vec4<f32>(mix(directDiffuse, previousDiffuse, hysteresis), confidence);
    cacheEntries[entryIndex].metadata.x = uniforms.frameIndex;
    cacheEntries[entryIndex].metadata.w = surfaceIndex;
}
