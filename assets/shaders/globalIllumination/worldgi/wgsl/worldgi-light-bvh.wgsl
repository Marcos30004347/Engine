#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

fn worldgi_light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn worldgi_light_can_affect_position(light : WorldGILight, position : vec3<f32>) -> bool {
    if (!worldgi_light_is_enabled(light)) {
        return false;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        return true;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT) {
        let toLight = light.positionRange.xyz - position;
        return dot(toLight, toLight) <= light.positionRange.w * light.positionRange.w;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        let toLight = light.positionRange.xyz - position;
        let influenceRadius = max(light.positionRange.w, 0.0) + max(light.shapeParams.x, 0.0);
        return dot(toLight, toLight) <= influenceRadius * influenceRadius;
    }

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let closestPoint = worldgi_planar_light_closest_point(light, position);
        let delta = position - closestPoint;
        return dot(delta, delta) <= light.positionRange.w * light.positionRange.w;
    }

    return false;
}

fn worldgi_position_in_light_bvh_node(position : vec3<f32>, node : WorldGILightBVHNode) -> bool {
    return all(position >= node.boundsMinNodeType.xyz) && all(position <= node.boundsMaxImportance.xyz);
}

fn worldgi_light_bvh_node_type(node : WorldGILightBVHNode) -> u32 {
    return u32(node.boundsMinNodeType.w + 0.5);
}

fn worldgi_query_directional_light(
    position : vec3<f32>,
    bvhInfo : WorldGILightBVHInfo,
    lights : ptr<storage, array<WorldGILight>, read>) -> u32 {
    for (var lightIndex = 0u; lightIndex < min(bvhInfo.lightCount, arrayLength(lights)); lightIndex++) {
        let light = (*lights)[lightIndex];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL &&
            worldgi_light_can_affect_position(light, position)) {
            return lightIndex;
        }
    }
    return 0xffffffffu;
}

fn worldgi_query_light_bvh_first(
    position : vec3<f32>,
    bvhInfo : WorldGILightBVHInfo,
    bvhNodes : ptr<storage, array<WorldGILightBVHNode>, read>,
    lights : ptr<storage, array<WorldGILight>, read>) -> u32 {
    if (bvhInfo.nodeCount == 0u || bvhInfo.rootNodeIndex == WORLD_GI_LIGHT_BVH_INVALID_NODE) {
        return worldgi_query_directional_light(position, bvhInfo, lights);
    }

    var stack : array<u32, 32>;
    var stackCount = 0u;
    stack[stackCount] = bvhInfo.rootNodeIndex;
    stackCount = stackCount + 1u;

    while (stackCount > 0u) {
        stackCount = stackCount - 1u;
        let nodeIndex = stack[stackCount];
        if (nodeIndex >= min(bvhInfo.nodeCount, arrayLength(bvhNodes))) {
            continue;
        }

        let node = (*bvhNodes)[nodeIndex];
        let nodeType = worldgi_light_bvh_node_type(node);
        if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_INVALID) {
            continue;
        }

        if (nodeType != WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE && !worldgi_position_in_light_bvh_node(position, node)) {
            continue;
        }

        if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_LEAF || nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE) {
            let lightIndex = node.childOrLightCount.z;
            if (lightIndex < min(bvhInfo.lightCount, arrayLength(lights)) &&
                worldgi_light_can_affect_position((*lights)[lightIndex], position)) {
                return lightIndex;
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

    // Directional lights are deliberately outside the finite BVH.
    return worldgi_query_directional_light(position, bvhInfo, lights);
}
