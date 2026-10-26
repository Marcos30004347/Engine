#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGILightGridUpdateUniforms {
    gridResolution : vec3<u32>,
    cascadeCount : u32,
    maxItemsPerCell : u32,
    lightCount : u32,
    enabled : u32,
    frameIndex : u32,
    gridCenterBaseExtent : vec4<f32>,
    cascadeScalePadding : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGILightGridUpdateUniforms;
@group(0) @binding(1) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(2) var<storage, read_write> gridCells : array<WorldGILightGridCell>;
@group(0) @binding(3) var<storage, read_write> gridItemIds : array<u32>;

fn light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn sphere_overlaps_aabb(center : vec3<f32>, radius : f32, aabbMin : vec3<f32>, aabbMax : vec3<f32>) -> bool {
    let closest = clamp(center, aabbMin, aabbMax);
    let delta = closest - center;
    return dot(delta, delta) <= radius * radius;
}

fn aabb_overlaps_aabb(aMin : vec3<f32>, aMax : vec3<f32>, bMin : vec3<f32>, bMax : vec3<f32>) -> bool {
    return all(aMax >= bMin) && all(aMin <= bMax);
}

fn light_overlaps_cell(light : WorldGILight, cellMin : vec3<f32>, cellMax : vec3<f32>) -> bool {
    if (!light_is_enabled(light)) {
        return false;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        return true;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT) {
        return sphere_overlaps_aabb(light.positionRange.xyz, max(light.positionRange.w, 0.0), cellMin, cellMax);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        return sphere_overlaps_aabb(light.positionRange.xyz, max(light.positionRange.w, 0.0) + max(light.shapeParams.x, 0.0), cellMin, cellMax);
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let basis = worldgi_planar_light_basis(light);
        let halfWidth = max(light.shapeParams.y, 0.0);
        let halfHeight = max(light.shapeParams.z, 0.0);
        let range = max(light.positionRange.w, 0.0);
        let extent = abs(basis.tangent) * halfWidth + abs(basis.bitangent) * halfHeight + vec3<f32>(range);
        return aabb_overlaps_aabb(light.positionRange.xyz - extent, light.positionRange.xyz + extent, cellMin, cellMax);
    }

    return false;
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let resolution = max(uniforms.gridResolution.x, 1u);
    let cascadeCount = max(uniforms.cascadeCount, 1u);
    let cellsPerCascade = resolution * resolution * resolution;
    let totalCells = cellsPerCascade * cascadeCount;
    let cellIndex = gid.x;
    if (cellIndex >= totalCells || cellIndex >= arrayLength(&gridCells)) {
        return;
    }

    var count = 0u;
    var overflow = 0u;
    let maxItems = max(uniforms.maxItemsPerCell, 1u);
    let itemBase = cellIndex * maxItems;

    if (uniforms.enabled != 0u) {
        let cascadeIndex = cellIndex / cellsPerCascade;
        let localIndex = cellIndex - cascadeIndex * cellsPerCascade;
        let cellZ = localIndex / (resolution * resolution);
        let cellY = (localIndex - cellZ * resolution * resolution) / resolution;
        let cellX = localIndex - cellZ * resolution * resolution - cellY * resolution;

        let extent = max(uniforms.gridCenterBaseExtent.w, 1.0e-3) *
            pow(max(uniforms.cascadeScalePadding.x, 1.0), f32(cascadeIndex));
        let cellSize = extent / f32(resolution);
        let cascadeMin = uniforms.gridCenterBaseExtent.xyz - vec3<f32>(extent * 0.5);
        let cellMin = cascadeMin + vec3<f32>(f32(cellX), f32(cellY), f32(cellZ)) * cellSize;
        let cellMax = cellMin + vec3<f32>(cellSize);
        let cappedLightCount = min(uniforms.lightCount, arrayLength(&lights));

        for (var lightIndex = 0u; lightIndex < cappedLightCount; lightIndex = lightIndex + 1u) {
            let light = lights[lightIndex];
            if (!light_overlaps_cell(light, cellMin, cellMax)) {
                continue;
            }

            if (count < maxItems && itemBase + count < arrayLength(&gridItemIds)) {
                gridItemIds[itemBase + count] = lightIndex;
                count = count + 1u;
            } else {
                overflow = overflow + 1u;
            }
        }
    }

    gridCells[cellIndex].itemCount = vec4<u32>(count, overflow, cellIndex / cellsPerCascade, 0u);
}
