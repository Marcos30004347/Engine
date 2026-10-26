const GPUSignedDistanceFields_TraceMaxCascades : u32 = 8u;
const GPUSignedDistanceFields_BottomAabbTreeWordOffset : u32 = 0u;
const GPUSignedDistanceFields_MidAabbTreeWordOffset : u32 = 16u * 16u * 16u;
const GPUSignedDistanceFields_RootAabbTreeWordOffset : u32 =
    GPUSignedDistanceFields_MidAabbTreeWordOffset + GPUSignedDistanceFields_BuildTreeMidNodeCount * 6u;

struct TraceParameters {
    maxSteps : u32,
    atlasBricksPerAxis : u32,
    cascadeCount : u32,
    cascadeOverride : i32,
    hitThresholdScale : f32,
    emptySpaceStepScale : f32,
}

struct AabbHit {
    hit : bool,
    tMin : f32,
    tMax : f32,
}

struct TraceHit {
    hit : bool,
    hasCascade : bool,
    hasBrick : bool,
    hasUninitialized : bool,
    hasValidBrickId : bool,
    hasRootAabbData : bool,
    hasTreeRoot : bool,
    hasMidAabb : bool,
    hasBottomAabb : bool,
    cascadeIndex : u32,
    brickId : u32,
    stepCount : u32,
    t : f32,
    firstBrickDistance : f32,
    worldPosition : vec3<f32>,
    uvw : vec3<f32>,
    cellWorldSize : f32,
}

struct CornerHit {
    coord : vec3<u32>,
    hitMax : f32,
}

fn load_combined_brick_map(cascadeIndex: u32, flatIndex: u32) -> u32 {
    switch (cascadeIndex) {
        case 0u: { return combinedBrickMap0[flatIndex]; }
        case 1u: { return combinedBrickMap1[flatIndex]; }
        case 2u: { return combinedBrickMap2[flatIndex]; }
        case 3u: { return combinedBrickMap3[flatIndex]; }
        case 4u: { return combinedBrickMap4[flatIndex]; }
        case 5u: { return combinedBrickMap5[flatIndex]; }
        case 6u: { return combinedBrickMap6[flatIndex]; }
        case 7u: { return combinedBrickMap7[flatIndex]; }
        default: { return GPUSignedDistanceFields_InvalidId; }
    }
}

fn load_combined_aabb_tree(cascadeIndex: u32, wordIndex: u32) -> u32 {
    switch (cascadeIndex) {
        case 0u: { return combinedAabbTree0[wordIndex]; }
        case 1u: { return combinedAabbTree1[wordIndex]; }
        case 2u: { return combinedAabbTree2[wordIndex]; }
        case 3u: { return combinedAabbTree3[wordIndex]; }
        case 4u: { return combinedAabbTree4[wordIndex]; }
        case 5u: { return combinedAabbTree5[wordIndex]; }
        case 6u: { return combinedAabbTree6[wordIndex]; }
        case 7u: { return combinedAabbTree7[wordIndex]; }
        default: { return 0u; }
    }
}

fn point_inside_cascade(cascade: CascadeInfoGPU, worldPosition: vec3<f32>) -> bool {
    return all(worldPosition >= cascade.gridMin) && all(worldPosition <= cascade.gridMax);
}

fn unpack_brick_aabb_min(packedAabb: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsUnflatten3D(packedAabb & 0x1ffu, GPUSignedDistanceFields_BrickResolution);
}

fn unpack_brick_aabb_max(packedAabb: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsUnflatten3D((packedAabb >> 9u) & 0x1ffu, GPUSignedDistanceFields_BrickResolution);
}

fn load_tree_u32x3(cascadeIndex: u32, wordOffset: u32) -> vec3<u32> {
    return vec3<u32>(
        load_combined_aabb_tree(cascadeIndex, wordOffset + 0u),
        load_combined_aabb_tree(cascadeIndex, wordOffset + 1u),
        load_combined_aabb_tree(cascadeIndex, wordOffset + 2u));
}

fn tree_aabb_is_empty_u32(aabbMin: vec3<u32>, aabbMax: vec3<u32>) -> bool {
    return aabbMin.x == aabbMax.x &&
        aabbMin.y == aabbMax.y &&
        aabbMin.z == aabbMax.z;
}

fn tree_local_coord_to_world(cascade: CascadeInfoGPU, localCoord: vec3<u32>) -> vec3<f32> {
    return cascade.gridMin + vec3<f32>(localCoord) * (cascade.voxelSize / 8.0);
}

fn intersect_aabb(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, aabbMin: vec3<f32>, aabbMax: vec3<f32>) -> AabbHit {
    var result = AabbHit(false, 0.0, 0.0);
    var tMin = -1.0e30;
    var tMax = 1.0e30;

    if (abs(rayDirection.x) < 1.0e-8) {
        if (rayOrigin.x < aabbMin.x || rayOrigin.x > aabbMax.x) {
            return result;
        }
    } else {
        let invDir = 1.0 / rayDirection.x;
        let t0 = (aabbMin.x - rayOrigin.x) * invDir;
        let t1 = (aabbMax.x - rayOrigin.x) * invDir;
        tMin = max(tMin, min(t0, t1));
        tMax = min(tMax, max(t0, t1));
    }

    if (abs(rayDirection.y) < 1.0e-8) {
        if (rayOrigin.y < aabbMin.y || rayOrigin.y > aabbMax.y) {
            return result;
        }
    } else {
        let invDir = 1.0 / rayDirection.y;
        let t0 = (aabbMin.y - rayOrigin.y) * invDir;
        let t1 = (aabbMax.y - rayOrigin.y) * invDir;
        tMin = max(tMin, min(t0, t1));
        tMax = min(tMax, max(t0, t1));
    }

    if (abs(rayDirection.z) < 1.0e-8) {
        if (rayOrigin.z < aabbMin.z || rayOrigin.z > aabbMax.z) {
            return result;
        }
    } else {
        let invDir = 1.0 / rayDirection.z;
        let t0 = (aabbMin.z - rayOrigin.z) * invDir;
        let t1 = (aabbMax.z - rayOrigin.z) * invDir;
        tMin = max(tMin, min(t0, t1));
        tMax = min(tMax, max(t0, t1));
    }

    result.hit = tMax >= max(tMin, 0.0);
    result.tMin = tMin;
    result.tMax = tMax;
    return result;
}

fn get_intersect_corner(cascade: CascadeInfoGPU, cornerSign: vec3<f32>, rayCursor: vec3<f32>, rayIDirection: vec3<f32>, eps: f32, voxelK: f32) -> CornerHit {
    let cellSize = cascade.voxelSize * voxelK;
    let relativeCascadeOrigin = cascade.gridMin - rayCursor;
    let coordF = max(vec3<f32>(0.0), -relativeCascadeOrigin / cellSize);
    let coord = vec3<u32>(coordF);
    let nodeMax = relativeCascadeOrigin + (vec3<f32>(coord) + cornerSign) * cellSize;
    let tbot = rayIDirection * nodeMax;
    return CornerHit(coord, min(tbot.x, min(tbot.y, tbot.z)) + eps);
}

fn sample_brixel_distance(brickId: u32, uvw: vec3<f32>, atlasBricksPerAxis: u32) -> f32 {
    let clampedUvw = clamp(uvw, vec3<f32>(0.0), vec3<f32>(1.0));
    let atlasDim = f32(atlasBricksPerAxis * GPUSignedDistanceFields_BrickResolution);
    let brickOffsetF = vec3<f32>(GPUSignedDistanceFieldsAtlasBrickCoord(brickId, atlasBricksPerAxis)) * f32(GPUSignedDistanceFields_BrickResolution);
    let uvwMin = (brickOffsetF + vec3<f32>(0.5)) / atlasDim;
    let uvwMax = (brickOffsetF + vec3<f32>(f32(GPUSignedDistanceFields_BrickResolution) - 0.5)) / atlasDim;
    return textureSampleLevel(sdfAtlasTexture, sdfAtlasSampler, mix(uvwMin, uvwMax, clampedUvw), 0.0).x;
}

fn trace_hit_epsilon_normalized(traceParams: TraceParameters) -> f32 {
    return max(traceParams.hitThresholdScale, 0.01) / f32(GPUSignedDistanceFields_BrickResolution - 1u);
}

fn trace_min_step_normalized() -> f32 {
    return 0.125 / f32(GPUSignedDistanceFields_BrickResolution - 1u);
}

fn sampling_empty_distance_world(cascade: CascadeInfoGPU, traceParams: TraceParameters) -> f32 {
    let brickCellCount = f32(GPUSignedDistanceFields_BrickResolution - 1u);
    let minEmptyDistance = cascade.voxelSize * (0.25 / brickCellCount);
    return max(cascade.voxelSize * max(traceParams.emptySpaceStepScale, 0.125), minEmptyDistance);
}

fn choose_normal_sampling_cascade(preferredCascadeIndex: u32, worldPosition: vec3<f32>, traceParams: TraceParameters) -> u32 {
    let cascadeCount = min(traceParams.cascadeCount, GPUSignedDistanceFields_TraceMaxCascades);
    if (traceParams.cascadeOverride >= 0 && traceParams.cascadeOverride < i32(cascadeCount)) {
        let overrideIndex = u32(traceParams.cascadeOverride);
        let overrideCascade = cascadeInfos[overrideIndex];
        if (overrideCascade.isEnabled != 0u && overrideCascade.isInitialized != 0u && point_inside_cascade(overrideCascade, worldPosition)) {
            return overrideIndex;
        }
    }

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex++) {
        let cascade = cascadeInfos[cascadeIndex];
        if (cascade.isEnabled == 0u || cascade.isInitialized == 0u) {
            continue;
        }
        if (point_inside_cascade(cascade, worldPosition)) {
            return cascadeIndex;
        }
    }

    return min(preferredCascadeIndex, max(cascadeCount, 1u) - 1u);
}

fn sample_world_distance(preferredCascadeIndex: u32, worldPosition: vec3<f32>, traceParams: TraceParameters) -> f32 {
    let cascadeIndex = choose_normal_sampling_cascade(preferredCascadeIndex, worldPosition, traceParams);
    let cascade = cascadeInfos[cascadeIndex];
    if (!point_inside_cascade(cascade, worldPosition)) {
        return sampling_empty_distance_world(cascade, traceParams);
    }

    let maxVoxelCoord = f32(GPUSignedDistanceFields_CascadeResolution - 1u) - 1.0e-4;
    let voxelPosition = clamp(
        (worldPosition - cascade.gridMin) * cascade.iVoxelSize,
        vec3<f32>(0.0),
        vec3<f32>(maxVoxelCoord));
    let voxelCoord = vec3<u32>(voxelPosition);
    let wrappedCoord = GPUSignedDistanceFieldsWrapCoords(cascade, voxelCoord);
    let brickId = load_combined_brick_map(
        cascadeIndex,
        GPUSignedDistanceFieldsFlatten3D(wrappedCoord, GPUSignedDistanceFields_CascadeResolution));
    if (!GPUSignedDistanceFieldsIsBrickValid(brickId)) {
        return sampling_empty_distance_world(cascade, traceParams);
    }

    let localUVW = clamp(fract(voxelPosition), vec3<f32>(0.0), vec3<f32>(1.0));
    return sample_brixel_distance(brickId, localUVW, traceParams.atlasBricksPerAxis) * cascade.voxelSize;
}

fn orient_normal_against_ray(normal: vec3<f32>, rayDirection: vec3<f32>) -> vec3<f32> {
    return select(normal, -normal, dot(normal, rayDirection) > 0.0);
}

fn estimate_hit_normal(hit: TraceHit, rayDirection: vec3<f32>, traceParams: TraceParameters) -> vec3<f32> {
    if (!GPUSignedDistanceFieldsIsBrickValid(hit.brickId)) {
        return orient_normal_against_ray(vec3<f32>(0.0, 0.0, 1.0), rayDirection);
    }

    let localEps = 1.0 / f32(GPUSignedDistanceFields_BrickResolution - 1u);
    let approachBias = 1.25 / f32(GPUSignedDistanceFields_BrickResolution - 1u);
    let localUvw = clamp(hit.uvw - rayDirection * approachBias, vec3<f32>(0.0), vec3<f32>(1.0));
    let lx0 = sample_brixel_distance(hit.brickId, localUvw - vec3<f32>(localEps, 0.0, 0.0), traceParams.atlasBricksPerAxis);
    let lx1 = sample_brixel_distance(hit.brickId, localUvw + vec3<f32>(localEps, 0.0, 0.0), traceParams.atlasBricksPerAxis);
    let ly0 = sample_brixel_distance(hit.brickId, localUvw - vec3<f32>(0.0, localEps, 0.0), traceParams.atlasBricksPerAxis);
    let ly1 = sample_brixel_distance(hit.brickId, localUvw + vec3<f32>(0.0, localEps, 0.0), traceParams.atlasBricksPerAxis);
    let lz0 = sample_brixel_distance(hit.brickId, localUvw - vec3<f32>(0.0, 0.0, localEps), traceParams.atlasBricksPerAxis);
    let lz1 = sample_brixel_distance(hit.brickId, localUvw + vec3<f32>(0.0, 0.0, localEps), traceParams.atlasBricksPerAxis);
    let localGrad = vec3<f32>(lx1 - lx0, ly1 - ly0, lz1 - lz0);
    if (dot(localGrad, localGrad) > 1.0e-10) {
        return orient_normal_against_ray(normalize(localGrad), rayDirection);
    }

    let eps = max(hit.cellWorldSize * 0.75, 1.0e-4);
    let biasedWorldPosition = hit.worldPosition - rayDirection * max(hit.cellWorldSize * 1.25, 1.0e-4);
    let k0 = vec3<f32>(1.0, -1.0, -1.0);
    let k1 = vec3<f32>(-1.0, -1.0, 1.0);
    let k2 = vec3<f32>(-1.0, 1.0, -1.0);
    let k3 = vec3<f32>(1.0, 1.0, 1.0);
    let s0 = sample_world_distance(hit.cascadeIndex, biasedWorldPosition + k0 * eps, traceParams);
    let s1 = sample_world_distance(hit.cascadeIndex, biasedWorldPosition + k1 * eps, traceParams);
    let s2 = sample_world_distance(hit.cascadeIndex, biasedWorldPosition + k2 * eps, traceParams);
    let s3 = sample_world_distance(hit.cascadeIndex, biasedWorldPosition + k3 * eps, traceParams);
    let grad = k0 * s0 + k1 * s1 + k2 * s2 + k3 * s3;
    if (dot(grad, grad) <= 1.0e-10) {
        return -rayDirection;
    }
    return orient_normal_against_ray(normalize(grad), rayDirection);
}

fn trace_brick_at(
    baseHit: TraceHit,
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    rayT: f32,
    rayCursor: vec3<f32>,
    cascadeIndex: u32,
    cascade: CascadeInfoGPU,
    voxelCoord: vec3<u32>,
    brickId: u32,
    maxDistance: f32,
    traceParams: TraceParameters) -> TraceHit {
    var hit = baseHit;
    let voxelMin = vec3<f32>(voxelCoord) * cascade.voxelSize + cascade.gridMin;
    let packedAabb = contextBrickAabb[brickId];
    let brickAabbMin = voxelMin - vec3<f32>(cascade.voxelSize / (2.0 * 7.0)) + vec3<f32>(unpack_brick_aabb_min(packedAabb)) * cascade.voxelSize / 7.0;
    let brickAabbMax = voxelMin - vec3<f32>(cascade.voxelSize / (2.0 * 7.0)) + vec3<f32>(unpack_brick_aabb_max(packedAabb) + vec3<u32>(1u)) * cascade.voxelSize / 7.0;
    let brickAabbHit = intersect_aabb(rayCursor, rayDirection, brickAabbMin, brickAabbMax);
    if (!brickAabbHit.hit) {
        return hit;
    }

    hit.hasBrick = true;
    hit.cascadeIndex = cascadeIndex;
    hit.brickId = brickId;
    hit.cellWorldSize = cascade.voxelSize / f32(GPUSignedDistanceFields_BrickResolution - 1u);

    var uvw = (rayCursor + max(brickAabbHit.tMin, 0.0) * rayDirection - voxelMin) * cascade.iVoxelSize;
    var totalDist = 0.0;
    let hitEps = trace_hit_epsilon_normalized(traceParams);
    let minStep = trace_min_step_normalized();

    for (var innerStep = 0u; innerStep < 8u; innerStep++) {
        hit.stepCount += 1u;
        let sdfDistance = sample_brixel_distance(brickId, uvw, traceParams.atlasBricksPerAxis);
        if (hit.firstBrickDistance < 0.0) {
            hit.firstBrickDistance = sdfDistance * cascade.voxelSize;
        }
        if (sdfDistance <= hitEps) {
            hit.hit = true;
            hit.t = rayT + max(brickAabbHit.tMin, 0.0) + totalDist * cascade.voxelSize;
            if (hit.t > maxDistance) {
                return hit;
            }
            hit.worldPosition = rayOrigin + rayDirection * hit.t;
            hit.uvw = uvw;
            return hit;
        }

        let dist = max(sdfDistance - hitEps * 0.5, minStep);
        uvw += rayDirection * dist;
        totalDist += dist;
        if (any(abs(uvw - vec3<f32>(0.5)) > vec3<f32>(0.501))) {
            break;
        }
    }

    return hit;
}

fn trace_brixelizer_raw(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, minDistance: f32, maxDistance: f32, traceParams: TraceParameters) -> TraceHit {
    var hit = TraceHit(
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        false,
        GPUSignedDistanceFields_InvalidId,
        GPUSignedDistanceFields_InvalidId,
        0u,
        0.0,
        -1.0,
        rayOrigin,
        vec3<f32>(0.0),
        0.01);
    let traversalStepLimit = max(traceParams.maxSteps, 32u);
    let rayIDirection = vec3<f32>(1.0) / rayDirection;
    let cornerSign = select(vec3<f32>(0.0), vec3<f32>(1.0), rayDirection > vec3<f32>(0.0));

    let cascadeCount = min(traceParams.cascadeCount, GPUSignedDistanceFields_TraceMaxCascades);
    var beginCascade = 0i;
    var endCascade = i32(cascadeCount);
    if (traceParams.cascadeOverride >= 0 && traceParams.cascadeOverride < i32(cascadeCount)) {
        beginCascade = traceParams.cascadeOverride;
        endCascade = traceParams.cascadeOverride + 1;
    }

    var rayT = max(minDistance, 0.0);
    for (var cascadeIndex = beginCascade; cascadeIndex < endCascade; cascadeIndex++) {
        let cascadeIndexU = u32(cascadeIndex);
        let cascade = cascadeInfos[cascadeIndexU];
        let origRayT = rayT;
        var localIterCount = 0u;
        let eps = cascade.voxelSize / 1024.0;
        var rayCursor = rayOrigin + rayDirection * rayT;

        if (cascade.isEnabled == 0u) {
            continue;
        }

        let originInsideCascade = point_inside_cascade(cascade, rayOrigin);
        let gridEntryHit = intersect_aabb(rayOrigin, rayDirection, cascade.gridMin, cascade.gridMax);
        if (!gridEntryHit.hit) {
            continue;
        }

        rayT = select(
            max(rayT, max(gridEntryHit.tMin, 0.0)),
            0.0,
            originInsideCascade);
        rayCursor = rayOrigin + rayDirection * rayT;
        let cursorInsideCascade =
            all(rayCursor >= cascade.gridMin - vec3<f32>(eps)) &&
            all(rayCursor <= cascade.gridMax + vec3<f32>(eps));
        if (!cursorInsideCascade) {
            rayT = min(rayT + eps, gridEntryHit.tMax);
            rayCursor = rayOrigin + rayDirection * rayT;
        }
        let adjustedCursorInsideCascade =
            all(rayCursor >= cascade.gridMin - vec3<f32>(eps)) &&
            all(rayCursor <= cascade.gridMax + vec3<f32>(eps));
        if (originInsideCascade && !adjustedCursorInsideCascade) {
            rayT = 0.0;
            rayCursor = rayOrigin;
        }
        let finalCursorInsideCascade =
            all(rayCursor >= cascade.gridMin - vec3<f32>(eps)) &&
            all(rayCursor <= cascade.gridMax + vec3<f32>(eps));
        if (rayT >= min(gridEntryHit.tMax, maxDistance) || !finalCursorInsideCascade) {
            continue;
        }

        hit.hasCascade = true;
        hit.cascadeIndex = cascadeIndexU;
        hit.cellWorldSize = cascade.voxelSize / f32(GPUSignedDistanceFields_BrickResolution - 1u);

        let cascadeAabbMinLocal = load_tree_u32x3(cascadeIndexU, GPUSignedDistanceFields_RootAabbTreeWordOffset + 0u);
        let cascadeAabbMaxLocal = load_tree_u32x3(cascadeIndexU, GPUSignedDistanceFields_RootAabbTreeWordOffset + 3u);
        if (tree_aabb_is_empty_u32(cascadeAabbMinLocal, cascadeAabbMaxLocal)) {
            continue;
        }
        hit.hasRootAabbData = true;
        let cascadeAabbMin = tree_local_coord_to_world(cascade, cascadeAabbMinLocal);
        let cascadeAabbMax = tree_local_coord_to_world(cascade, cascadeAabbMaxLocal);
        let cascadeTreeHit = intersect_aabb(rayOrigin, rayDirection, cascadeAabbMin, cascadeAabbMax);
        if (!cascadeTreeHit.hit) {
            continue;
        }

        hit.hasTreeRoot = true;
        rayT = max(rayT, max(cascadeTreeHit.tMin, 0.0));
        if (rayT >= min(cascadeTreeHit.tMax, maxDistance)) {
            continue;
        }

        var topLevelMax = 0.0;
        var stampSize = 16.0;
        var level = 0u;
        let cascadeHitMax = min(cascadeTreeHit.tMax, maxDistance);

        while (rayT < cascadeHitMax) {
            hit.stepCount += 1u;
            localIterCount += 1u;
            if (localIterCount > traversalStepLimit) {
                break;
            }

            rayCursor = rayOrigin + rayDirection * rayT;
            let stampCorner = get_intersect_corner(cascade, cornerSign, rayCursor, rayIDirection, eps, stampSize);
            let stampHitMax = rayT + stampCorner.hitMax;
            let stampDimension = select(4u, 16u, level != 0u);
            let stampCoord = min(stampCorner.coord, vec3<u32>(stampDimension - 1u));
            let stampIndex = GPUSignedDistanceFieldsFlatten3D(stampCoord, stampDimension);

            var stampAabbMin = vec3<f32>(0.0);
            var stampAabbMax = vec3<f32>(0.0);
            if (level == 0u) {
                topLevelMax = stampHitMax;
                let wordOffset = GPUSignedDistanceFields_MidAabbTreeWordOffset + (2u * stampIndex + 0u) * 3u;
                let stampAabbMinLocal = load_tree_u32x3(cascadeIndexU, wordOffset + 0u);
                let stampAabbMaxLocal = load_tree_u32x3(cascadeIndexU, wordOffset + 3u);
                stampAabbMin = tree_local_coord_to_world(cascade, stampAabbMinLocal);
                stampAabbMax = tree_local_coord_to_world(cascade, stampAabbMaxLocal);
            } else {
                let bottomNode = load_combined_aabb_tree(cascadeIndexU, stampIndex);
                if (bottomNode != GPUSignedDistanceFields_InvalidBottomAabbNode) {
                    let bottomAabbMinU = GPUSignedDistanceFieldsUnflatten3D(bottomNode & 0x7fffu, 32u);
                    let bottomAabbMaxU = GPUSignedDistanceFieldsUnflatten3D((bottomNode >> 16u) & 0x7fffu, 32u);
                    let bottomStampWorldCoord = vec3<f32>(stampCoord) * cascade.voxelSize * 4.0 + cascade.gridMin;
                    stampAabbMin = bottomStampWorldCoord + vec3<f32>(bottomAabbMinU) * cascade.voxelSize / 8.0;
                    stampAabbMax = bottomStampWorldCoord + vec3<f32>(bottomAabbMaxU + vec3<u32>(1u)) * cascade.voxelSize / 8.0;
                }
            }

            let stampAabbHit = intersect_aabb(rayOrigin, rayDirection, stampAabbMin, stampAabbMax);
            if (all(stampAabbMin == stampAabbMax) || !stampAabbHit.hit) {
                rayT = stampHitMax;
                if (level != 0u && rayT > topLevelMax) {
                    level = 0u;
                    stampSize = 16.0;
                }
                continue;
            }

            if (level == 0u) {
                hit.hasMidAabb = true;
                level = 1u;
                stampSize = 4.0;
                continue;
            }

            hit.hasBottomAabb = true;
            let stampAabbHitMax = min(stampAabbHit.tMax, maxDistance);
            while (rayT < stampAabbHitMax) {
                hit.stepCount += 1u;
                localIterCount += 1u;
                if (localIterCount > traversalStepLimit) {
                    break;
                }

                rayCursor = rayOrigin + rayDirection * rayT;
                let voxelCorner = get_intersect_corner(cascade, cornerSign, rayCursor, rayIDirection, eps, 1.0);
                let voxelCoord = min(voxelCorner.coord, vec3<u32>(GPUSignedDistanceFields_CascadeResolution - 1u));
                var voxelHitMax = voxelCorner.hitMax;
                if (voxelHitMax < eps) {
                    rayT = rayT + voxelHitMax;
                    break;
                }
                voxelHitMax += rayT;

                let flatIndex = GPUSignedDistanceFieldsFlatten3D(
                    GPUSignedDistanceFieldsWrapCoords(cascade, voxelCoord),
                    GPUSignedDistanceFields_CascadeResolution);
                let brickId = load_combined_brick_map(cascadeIndexU, flatIndex);
                if (brickId == GPUSignedDistanceFields_UninitializedId) {
                    hit.hasUninitialized = true;
                    localIterCount = traversalStepLimit + 1u;
                    break;
                }

                if (GPUSignedDistanceFieldsIsBrickValid(brickId)) {
                    hit.hasValidBrickId = true;
                    hit = trace_brick_at(hit, rayOrigin, rayDirection, rayT, rayCursor, cascadeIndexU, cascade, voxelCoord, brickId, maxDistance, traceParams);
                    if (hit.hit) {
                        return hit;
                    }
                }

                rayT = voxelHitMax;
            }

            if (rayT > topLevelMax) {
                level = 0u;
                stampSize = 16.0;
            }
            if (localIterCount > traversalStepLimit) {
                break;
            }
            rayT = stampHitMax;
        }

        if (localIterCount > traversalStepLimit) {
            rayT = max(origRayT, rayT - cascade.voxelSize);
            continue;
        }

        let gridHit = intersect_aabb(rayOrigin, rayDirection, cascade.gridMin, cascade.gridMax);
        if (gridHit.hit) {
            rayT = max(origRayT, gridHit.tMax - cascade.voxelSize);
        }
    }

    return hit;
}
