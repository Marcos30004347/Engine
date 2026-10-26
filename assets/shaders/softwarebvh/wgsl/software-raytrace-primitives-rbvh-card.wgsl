struct RBVHCardPrimitive {
    origin: vec3<f32>,
    flags: u32,
    axisU: vec3<f32>,
    halfExtentU: f32,
    axisV: vec3<f32>,
    halfExtentV: f32,
    axisN: vec3<f32>,
    depthMin: f32,
    boundsMin: vec3<f32>,
    depthMax: f32,
    boundsMax: vec3<f32>,
    samplingError: f32,
    atlasX: u32,
    atlasY: u32,
    atlasWidth: u32,
    atlasHeight: u32,
    sourceNodeIndex: u32,
    sourcePrimitiveOffset: u32,
    sourcePrimitiveCount: u32,
    padding0: u32,
};

struct BLASMetadataCommon {
    handleId: u32,
    cwbvhNodeOffset: u32,
    cwbvhNodeCount: u32,
    leafRemapOffset: u32,
};

struct RBVHCardBLASMetadata {
    handleId: u32,
    cwbvhNodeOffset: u32,
    cwbvhNodeCount: u32,
    leafRemapOffset: u32,
    primitiveRemapOffset: u32,
    primitiveCount: u32,
    rbvhCardOffset: u32,
    rbvhCardCount: u32,
    rbvhDepthAtlasOffset: u32,
    rbvhAtlasSize: u32,
    rbvhAtlasTexelCount: u32,
    rbvhDepthMipOffset: u32,
    rbvhDepthMipLevelCount: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
};

@group(0) @binding(4) var<storage, read> blasMetadata: array<RBVHCardBLASMetadata>;
@group(0) @binding(5) var<storage, read> blasPrimitiveRemap: array<u32>;
@group(0) @binding(6) var<storage, read> rbvhCards: array<RBVHCardPrimitive>;
@group(0) @binding(7) var rbvhDepthAtlas: texture_2d<f32>;
@group(0) @binding(8) var<storage, read> rbvhDepthMipAtlas: array<u32>;
@group(0) @binding(15) var rbvhDepthAtlasSampler: sampler;

struct RBVHAtlasTexel {
    valid: f32,
    depth: f32,
};

fn rbvh_card_atlas_texel_coord(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, sampleX: u32, sampleY: u32) -> vec2<i32> {
    let textureWidth = max(uniforms.rbvhDepthAtlasTextureWidth, 1u);
    let textureHeight = max(uniforms.rbvhDepthAtlasTextureHeight, 1u);
    let tileBaseX = metadata.rbvhDepthAtlasOffset % textureWidth;
    let tileBaseY = metadata.rbvhDepthAtlasOffset / textureWidth;
    let globalX = card.atlasX + sampleX;
    let globalY = card.atlasY + sampleY;
    let atlasX = tileBaseX + globalX;
    let atlasY = tileBaseY + globalY;
    if atlasX >= textureWidth || atlasY >= textureHeight {
        return vec2<i32>(-1, -1);
    }

    return vec2<i32>(i32(atlasX), i32(atlasY));
}

fn load_blas_metadata_common(blasHandle: u32) -> BLASMetadataCommon {
    let metadata = blasMetadata[blasHandle];
    return BLASMetadataCommon(metadata.handleId, metadata.cwbvhNodeOffset, metadata.cwbvhNodeCount, metadata.leafRemapOffset);
}

fn load_rbvh_card_blas_metadata(blasHandle: u32) -> RBVHCardBLASMetadata {
    return blasMetadata[blasHandle];
}

fn load_trace_primitive_metadata(blasHandle: u32) -> RBVHCardBLASMetadata {
    return load_rbvh_card_blas_metadata(blasHandle);
}

fn sample_rbvh_card_texel(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, sampleX: u32, sampleY: u32) -> RBVHAtlasTexel {
    if sampleX >= card.atlasWidth || sampleY >= card.atlasHeight {
        return RBVHAtlasTexel(0.0, 0.0);
    }

    let atlasCoord = rbvh_card_atlas_texel_coord(metadata, card, sampleX, sampleY);
    if atlasCoord.x < 0 || atlasCoord.y < 0 {
        return RBVHAtlasTexel(0.0, 0.0);
    }

    let texel = textureLoad(rbvhDepthAtlas, atlasCoord, 0);
    if texel.y < 0.999 {
        return RBVHAtlasTexel(0.0, 0.0);
    }

    return RBVHAtlasTexel(1.0, card.depthMin + texel.x * (card.depthMax - card.depthMin));
}

fn rbvh_card_texel_to_atlas_uv(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, texelCoord: vec2<f32>) -> vec2<f32> {
    let textureSize = vec2<f32>(
        f32(max(uniforms.rbvhDepthAtlasTextureWidth, 1u)),
        f32(max(uniforms.rbvhDepthAtlasTextureHeight, 1u)));
    let textureWidth = max(uniforms.rbvhDepthAtlasTextureWidth, 1u);
    let tileBaseX = metadata.rbvhDepthAtlasOffset % textureWidth;
    let tileBaseY = metadata.rbvhDepthAtlasOffset / textureWidth;
    let atlasTexel = vec2<f32>(f32(tileBaseX + card.atlasX), f32(tileBaseY + card.atlasY)) + texelCoord;
    return (atlasTexel + vec2<f32>(0.5)) / textureSize;
}

fn sample_rbvh_card_depth(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, texelCoord: vec2<f32>) -> vec2<f32> {
    if card.atlasWidth == 0u || card.atlasHeight == 0u {
        return vec2<f32>(0.0, 0.0);
    }

    let clampedTexelCoord = clamp(texelCoord, vec2<f32>(0.5), vec2<f32>(f32(card.atlasWidth) - 0.5, f32(card.atlasHeight) - 0.5));
    let atlasUv = rbvh_card_texel_to_atlas_uv(metadata, card, clampedTexelCoord - vec2<f32>(0.5));
    let texel = textureSampleLevel(rbvhDepthAtlas, rbvhDepthAtlasSampler, atlasUv, 0.0);
    if texel.y < 0.999 {
        return vec2<f32>(0.0, 0.0);
    }

    return vec2<f32>(1.0, card.depthMin + texel.x * (card.depthMax - card.depthMin));
}

fn sample_rbvh_card_difference(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, point: vec3<f32>) -> vec2<f32> {
    if card.atlasWidth == 0u || card.atlasHeight == 0u || metadata.rbvhAtlasSize == 0u || metadata.rbvhAtlasTexelCount == 0u {
        return vec2<f32>(0.0, 0.0);
    }

    let local = point - card.origin;
    let u = dot(local, card.axisU);
    let v = dot(local, card.axisV);
    if abs(u) > card.halfExtentU || abs(v) > card.halfExtentV {
        return vec2<f32>(0.0, 0.0);
    }

    let uv = vec2<f32>(u / card.halfExtentU * 0.5 + 0.5, v / card.halfExtentV * 0.5 + 0.5);
    let surface = sample_rbvh_card_depth(
        metadata,
        card,
        uv * vec2<f32>(f32(card.atlasWidth), f32(card.atlasHeight)));
    if surface.x <= 0.5 {
        return vec2<f32>(0.0, 0.0);
    }

    let pointDepth = dot(point, card.axisN);
    return vec2<f32>(1.0, pointDepth - surface.y);
}

fn rbvh_card_texel_coord(card: RBVHCardPrimitive, point: vec3<f32>) -> vec2<f32> {
    let local = point - card.origin;
    let u = dot(local, card.axisU);
    let v = dot(local, card.axisV);
    return vec2<f32>(
        (u / card.halfExtentU * 0.5 + 0.5) * f32(card.atlasWidth),
        (v / card.halfExtentV * 0.5 + 0.5) * f32(card.atlasHeight));
}

fn rbvh_depth_mip_level_offset(atlasSize: u32, mipLevel: u32) -> u32 {
    var offset = 0u;
    var levelSize = max(atlasSize >> 1u, 1u);
    for (var level = 1u; level < mipLevel; level += 1u) {
        offset += levelSize * levelSize;
        levelSize = max(levelSize >> 1u, 1u);
    }
    return offset;
}

fn rbvh_card_root_mip_level(cardDimension: u32, mipLevelCount: u32) -> u32 {
    if cardDimension <= 1u {
        return 0u;
    }

    return min(mipLevelCount, 32u - countLeadingZeros(cardDimension - 1u));
}

fn rbvh_card_packed_root_mip_level(card: RBVHCardPrimitive, metadata: RBVHCardBLASMetadata) -> u32 {
    return min(min(card.padding0 & 31u, metadata.rbvhDepthMipLevelCount), RBVH_CARD_DDA_MAX_MIP_LEVEL);
}

fn rbvh_card_packed_root_mip_offset(card: RBVHCardPrimitive) -> u32 {
    return card.padding0 >> 5u;
}

fn rbvh_card_mip_level_offset(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, mipLevel: u32, rootMipLevel: u32) -> u32 {
    if mipLevel == rootMipLevel {
        return rbvh_card_packed_root_mip_offset(card);
    }
    return rbvh_depth_mip_level_offset(metadata.rbvhAtlasSize, mipLevel);
}

fn sample_rbvh_depth_minmax_mip(metadata: RBVHCardBLASMetadata, card: RBVHCardPrimitive, mipLevel: u32, mipLevelOffset: u32, localX: u32, localY: u32) -> vec3<f32> {
    if mipLevel == 0u {
        let texel = sample_rbvh_card_texel(metadata, card, localX, localY);
        return vec3<f32>(texel.valid, texel.depth, texel.depth);
    }

    if mipLevel > metadata.rbvhDepthMipLevelCount || metadata.rbvhDepthMipLevelCount == 0u {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let blockSize = 1u << mipLevel;
    let levelSize = max(metadata.rbvhAtlasSize >> mipLevel, 1u);
    let localMinX = localX * blockSize;
    let localMinY = localY * blockSize;
    if localMinX >= card.atlasWidth || localMinY >= card.atlasHeight {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let globalMinX = card.atlasX + localMinX;
    let globalMinY = card.atlasY + localMinY;
    let globalMaxX = min(min(card.atlasX + card.atlasWidth, card.atlasX + (localX + 1u) * blockSize), metadata.rbvhAtlasSize);
    let globalMaxY = min(min(card.atlasY + card.atlasHeight, card.atlasY + (localY + 1u) * blockSize), metadata.rbvhAtlasSize);
    if globalMinX >= globalMaxX || globalMinY >= globalMaxY {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let mipMinX = min(globalMinX >> mipLevel, levelSize - 1u);
    let mipMinY = min(globalMinY >> mipLevel, levelSize - 1u);
    let mipMaxX = min((globalMaxX - 1u) >> mipLevel, levelSize - 1u);
    let mipMaxY = min((globalMaxY - 1u) >> mipLevel, levelSize - 1u);

    var encodedMin = 0xffffu;
    var encodedMax = 0u;
    var valid = false;
    var mipY = mipMinY;
    loop {
        var mipX = mipMinX;
        loop {
            let packed = rbvhDepthMipAtlas[metadata.rbvhDepthMipOffset + mipLevelOffset + mipY * levelSize + mipX];
            let candidateMin = packed & 0xffffu;
            let candidateMax = packed >> 16u;
            if candidateMin <= candidateMax {
                encodedMin = min(encodedMin, candidateMin);
                encodedMax = max(encodedMax, candidateMax);
                valid = true;
            }

            if mipX >= mipMaxX {
                break;
            }
            mipX += 1u;
        }

        if mipY >= mipMaxY {
            break;
        }
        mipY += 1u;
    }

    if !valid {
        return vec3<f32>(0.0, 0.0, 0.0);
    }

    let depthMin = card.depthMin + (f32(encodedMin) / 65535.0) * (card.depthMax - card.depthMin);
    let depthMax = card.depthMin + (f32(encodedMax) / 65535.0) * (card.depthMax - card.depthMin);
    return vec3<f32>(1.0, depthMin, depthMax);
}

fn rbvh_depth_ranges_overlap(rayMin: f32, rayMax: f32, surfaceMin: f32, surfaceMax: f32, epsilon: f32) -> bool {
    return max(rayMin, surfaceMin) <= min(rayMax, surfaceMax) + epsilon;
}

fn rbvh_clip_segment_to_texel_rect(
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    rectMin: vec2<f32>,
    rectMax: vec2<f32>,
    sBegin: f32,
    sEnd: f32) -> vec2<f32> {
    var clippedBegin = sBegin;
    var clippedEnd = sEnd;

    if abs(texelDelta.x) <= 1.0e-8 {
        if startTexel.x < rectMin.x || startTexel.x > rectMax.x {
            return vec2<f32>(1.0, -1.0);
        }
    } else {
        let sx0 = (rectMin.x - startTexel.x) / texelDelta.x;
        let sx1 = (rectMax.x - startTexel.x) / texelDelta.x;
        clippedBegin = max(clippedBegin, min(sx0, sx1));
        clippedEnd = min(clippedEnd, max(sx0, sx1));
    }

    if abs(texelDelta.y) <= 1.0e-8 {
        if startTexel.y < rectMin.y || startTexel.y > rectMax.y {
            return vec2<f32>(1.0, -1.0);
        }
    } else {
        let sy0 = (rectMin.y - startTexel.y) / texelDelta.y;
        let sy1 = (rectMax.y - startTexel.y) / texelDelta.y;
        clippedBegin = max(clippedBegin, min(sy0, sy1));
        clippedEnd = min(clippedEnd, max(sy0, sy1));
    }

    return vec2<f32>(clippedBegin, clippedEnd);
}

fn rbvh_card_depth_difference_at_s(
    metadata: RBVHCardBLASMetadata,
    card: RBVHCardPrimitive,
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    startDepth: f32,
    depthDelta: f32,
    s: f32) -> vec2<f32> {
    let surface = sample_rbvh_card_depth(metadata, card, startTexel + texelDelta * s);
    if surface.x <= 0.0 {
        return vec2<f32>(0.0, 0.0);
    }
    return vec2<f32>(1.0, startDepth + depthDelta * s - surface.y);
}

fn trace_rbvh_card_leaf(
    metadata: RBVHCardBLASMetadata,
    card: RBVHCardPrimitive,
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    startDepth: f32,
    depthDelta: f32,
    sBegin: f32,
    sEnd: f32,
    texelX: u32,
    texelY: u32,
    tBegin: f32,
    tEnd: f32,
    epsilon: f32) -> f32 {
    if sBegin > sEnd {
        return -1.0;
    }

    let clipped = rbvh_clip_segment_to_texel_rect(
        startTexel,
        texelDelta,
        vec2<f32>(f32(texelX), f32(texelY)),
        vec2<f32>(f32(texelX + 1u), f32(texelY + 1u)),
        sBegin,
        sEnd);
    if clipped.x > clipped.y {
        return -1.0;
    }

    var bestS = clipped.x;
    var bestAbs = 1.0e20;
    var bracketLoS = clipped.x;
    var bracketHiS = clipped.y;
    var bracketLoDiff = 0.0;
    var bracketHiDiff = 0.0;
    var hasBracket = false;

    let searchSteps = 4u;
    var previousS = clipped.x;
    var previous = rbvh_card_depth_difference_at_s(metadata, card, startTexel, texelDelta, startDepth, depthDelta, previousS);
    if previous.x > 0.5 {
        bestAbs = abs(previous.y);
        bestS = previousS;
    }

    for (var search = 1u; search <= searchSteps; search += 1u) {
        let sampleS = mix(clipped.x, clipped.y, f32(search) / f32(searchSteps));
        let sample = rbvh_card_depth_difference_at_s(metadata, card, startTexel, texelDelta, startDepth, depthDelta, sampleS);
        if sample.x > 0.5 {
            let sampleAbs = abs(sample.y);
            if sampleAbs < bestAbs {
                bestAbs = sampleAbs;
                bestS = sampleS;
            }
            if previous.x > 0.5 && ((previous.y <= 0.0 && sample.y >= 0.0) || (previous.y >= 0.0 && sample.y <= 0.0)) {
                bracketLoS = previousS;
                bracketHiS = sampleS;
                bracketLoDiff = previous.y;
                bracketHiDiff = sample.y;
                hasBracket = true;
                break;
            }
            previousS = sampleS;
            previous = sample;
        }
    }

    if bestAbs <= epsilon {
        return mix(tBegin, tEnd, bestS);
    }
    if !hasBracket {
        if bestAbs <= epsilon * 2.0 {
            return mix(tBegin, tEnd, bestS);
        }
        return -1.0;
    }

    var loS = bracketLoS;
    var hiS = bracketHiS;
    var loDiff = bracketLoDiff;
    var hiDiff = bracketHiDiff;
    for (var refine = 0u; refine < 5u; refine += 1u) {
        let denom = hiDiff - loDiff;
        let secantT = select(0.5, clamp(-loDiff / denom, 0.05, 0.95), abs(denom) > 1.0e-8);
        let sampleS = mix(loS, hiS, secantT);
        let sample = rbvh_card_depth_difference_at_s(metadata, card, startTexel, texelDelta, startDepth, depthDelta, sampleS);
        if sample.x <= 0.5 {
            break;
        }

        let sampleAbs = abs(sample.y);
        if sampleAbs < bestAbs {
            bestAbs = sampleAbs;
            bestS = sampleS;
        }
        if sampleAbs <= epsilon {
            return mix(tBegin, tEnd, sampleS);
        }

        if (loDiff <= 0.0 && sample.y >= 0.0) || (loDiff >= 0.0 && sample.y <= 0.0) {
            hiS = sampleS;
            hiDiff = sample.y;
        } else {
            loS = sampleS;
            loDiff = sample.y;
        }
    }

    if bestAbs <= epsilon * 2.0 {
        return mix(tBegin, tEnd, bestS);
    }
    return -1.0;
}

fn rbvh_card_cell_at_s(card: RBVHCardPrimitive, startTexel: vec2<f32>, texelDelta: vec2<f32>, s: f32) -> vec2<u32> {
    let coord = startTexel + texelDelta * s;
    return vec2<u32>(
        min(card.atlasWidth - 1u, u32(clamp(floor(coord.x), 0.0, f32(card.atlasWidth - 1u)))),
        min(card.atlasHeight - 1u, u32(clamp(floor(coord.y), 0.0, f32(card.atlasHeight - 1u)))));
}

fn rbvh_card_dda_next_s(
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    cell: vec2<u32>,
    currentS: f32,
    endS: f32) -> f32 {
    var nextS = endS;
    if texelDelta.x > 1.0e-8 {
        nextS = min(nextS, (f32(cell.x + 1u) - startTexel.x) / texelDelta.x);
    } else if texelDelta.x < -1.0e-8 {
        nextS = min(nextS, (f32(cell.x) - startTexel.x) / texelDelta.x);
    }

    if texelDelta.y > 1.0e-8 {
        nextS = min(nextS, (f32(cell.y + 1u) - startTexel.y) / texelDelta.y);
    } else if texelDelta.y < -1.0e-8 {
        nextS = min(nextS, (f32(cell.y) - startTexel.y) / texelDelta.y);
    }

    return clamp(nextS, currentS, endS);
}

fn rbvh_card_dda_advance_s(currentS: f32, nextS: f32, endS: f32) -> f32 {
    let stepEpsilon = max((endS - currentS) * 1.0e-5, 1.0e-6);
    return min(max(nextS, currentS) + stepEpsilon, endS);
}

fn rbvh_trace_minmax_quadtree_block(
    metadata: RBVHCardBLASMetadata,
    card: RBVHCardPrimitive,
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    startDepth: f32,
    depthDelta: f32,
    currentS: f32,
    sEnd: f32,
    rootMipLevel: u32,
    epsilon: f32) -> vec2<f32> {
    let cell = rbvh_card_cell_at_s(card, startTexel, texelDelta, currentS);
    var mipLevel = rootMipLevel;
    loop {
        let blockSize = 1u << mipLevel;
        let blockX = cell.x >> mipLevel;
        let blockY = cell.y >> mipLevel;
        let blockMin = vec2<f32>(f32(blockX * blockSize), f32(blockY * blockSize));
        let blockMax = vec2<f32>(
            min(f32((blockX + 1u) * blockSize), f32(card.atlasWidth)),
            min(f32((blockY + 1u) * blockSize), f32(card.atlasHeight)));
        let clippedBlock = rbvh_clip_segment_to_texel_rect(startTexel, texelDelta, blockMin, blockMax, currentS, sEnd);
        if clippedBlock.x > clippedBlock.y {
            return vec2<f32>(0.0, currentS);
        }

        let mipLevelOffset = rbvh_card_mip_level_offset(metadata, card, mipLevel, rootMipLevel);
        let minMaxDepth = sample_rbvh_depth_minmax_mip(metadata, card, mipLevel, mipLevelOffset, blockX, blockY);
        let depth0 = startDepth + depthDelta * clippedBlock.x;
        let depth1 = startDepth + depthDelta * clippedBlock.y;
        if minMaxDepth.x <= 0.5 || !rbvh_depth_ranges_overlap(min(depth0, depth1), max(depth0, depth1), minMaxDepth.y, minMaxDepth.z, epsilon) {
            return vec2<f32>(0.0, clippedBlock.y);
        }

        if mipLevel == 0u {
            let hitT = trace_rbvh_card_leaf(
                metadata,
                card,
                startTexel,
                texelDelta,
                startDepth,
                depthDelta,
                clippedBlock.x,
                clippedBlock.y,
                blockX,
                blockY,
                0.0,
                1.0,
                epsilon);
            if hitT >= 0.0 {
                return vec2<f32>(1.0, hitT);
            }
            return vec2<f32>(0.0, clippedBlock.y);
        }

        mipLevel -= 1u;
    }
    return vec2<f32>(0.0, currentS);
}

fn trace_rbvh_card_dda(
    metadata: RBVHCardBLASMetadata,
    card: RBVHCardPrimitive,
    startTexel: vec2<f32>,
    texelDelta: vec2<f32>,
    startDepth: f32,
    depthDelta: f32,
    sBegin: f32,
    sEnd: f32,
    tBegin: f32,
    tEnd: f32,
    epsilon: f32) -> f32 {
    var currentS = sBegin;
    let metadataRootMipLevel = rbvh_card_packed_root_mip_level(card, metadata);
    let computedRootMipLevel = min(RBVH_CARD_DDA_MAX_MIP_LEVEL, rbvh_card_root_mip_level(max(card.atlasWidth, card.atlasHeight), metadata.rbvhDepthMipLevelCount));
    let rootMipLevel = select(computedRootMipLevel, metadataRootMipLevel, metadataRootMipLevel > 0u || card.padding0 != 0u);
    for (var step = 0u; step < RBVH_CARD_DDA_MAX_STEPS; step += 1u) {
        if currentS > sEnd {
            break;
        }

        let blockResult = rbvh_trace_minmax_quadtree_block(
            metadata,
            card,
            startTexel,
            texelDelta,
            startDepth,
            depthDelta,
            currentS,
            sEnd,
            rootMipLevel,
            epsilon);
        if blockResult.x > 0.5 {
            return mix(tBegin, tEnd, blockResult.y);
        }

        let nextS = blockResult.y;
        if nextS >= sEnd {
            break;
        }
        currentS = rbvh_card_dda_advance_s(currentS, nextS, sEnd);
    }

    return -1.0;
}

fn trace_rbvh_card_local(metadata: RBVHCardBLASMetadata, localCardIndex: u32, localOrigin: vec3<f32>, localDirection: vec3<f32>, localInvDir: vec3<f32>, maxRayT: f32) -> f32 {
    if localCardIndex >= metadata.rbvhCardCount {
        return -1.0;
    }

    let card = rbvhCards[metadata.rbvhCardOffset + localCardIndex];
    let interval = intersect_aabb_interval_invdir(localOrigin, localInvDir, card.boundsMin, card.boundsMax);
    var t = max(interval.x, uniforms.tMin);
    let tEnd = min(interval.y, min(uniforms.tMax, maxRayT));
    if t > tEnd {
        return -1.0;
    }

    let segmentStart = localOrigin + localDirection * t;
    let segmentEnd = localOrigin + localDirection * tEnd;
    let segmentDelta = segmentEnd - segmentStart;
    let startTexel = rbvh_card_texel_coord(card, segmentStart);
    let endTexel = rbvh_card_texel_coord(card, segmentEnd);
    let epsilon = max(card.samplingError * 1.5, 1.0e-4);

    if card.atlasWidth == 0u || card.atlasHeight == 0u {
        return -1.0;
    }

    let texelDelta = endTexel - startTexel;
    let startDepth = dot(segmentStart, card.axisN);
    let depthDelta = dot(segmentDelta, card.axisN);
    let clippedCardSegment = rbvh_clip_segment_to_texel_rect(
        startTexel,
        texelDelta,
        vec2<f32>(0.0, 0.0),
        vec2<f32>(f32(card.atlasWidth), f32(card.atlasHeight)),
        0.0,
        1.0);
    if clippedCardSegment.x > clippedCardSegment.y {
        return -1.0;
    }
    let clippedDepth0 = startDepth + depthDelta * clippedCardSegment.x;
    let clippedDepth1 = startDepth + depthDelta * clippedCardSegment.y;
    if !rbvh_depth_ranges_overlap(min(clippedDepth0, clippedDepth1), max(clippedDepth0, clippedDepth1), card.depthMin, card.depthMax, epsilon) {
        return -1.0;
    }

    return trace_rbvh_card_dda(
        metadata,
        card,
        startTexel,
        texelDelta,
        startDepth,
        depthDelta,
        clippedCardSegment.x,
        clippedCardSegment.y,
        t,
        tEnd,
        epsilon);
}

fn rbvh_card_aabb_hit_local(metadata: RBVHCardBLASMetadata, localCardIndex: u32, localOrigin: vec3<f32>, localInvDir: vec3<f32>, maxRayT: f32) -> bool {
    if localCardIndex >= metadata.rbvhCardCount {
        return false;
    }

    let card = rbvhCards[metadata.rbvhCardOffset + localCardIndex];
    let interval = intersect_aabb_interval_invdir(localOrigin, localInvDir, card.boundsMin, card.boundsMax);
    let t = max(interval.x, uniforms.tMin);
    let tEnd = min(interval.y, min(uniforms.tMax, maxRayT));
    return t <= tEnd;
}

fn trace_blas_primitive_leaf_local(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    instanceIndex: u32,
    metadata: BLASMetadataCommon,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    state: ptr<function, HitState>) {
    trace_blas_primitive_leaf_local_cached(
        rayOrigin,
        rayDirection,
        instance,
        instanceIndex,
        metadata,
        load_trace_primitive_metadata(instance.blasHandle),
        localPrimitiveIndex,
        localOrigin,
        localDirection,
        safe_inverse_direction(localDirection),
        state);
}

fn trace_blas_primitive_leaf_local_cached(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    instanceIndex: u32,
    metadata: BLASMetadataCommon,
    rbvhMetadata: RBVHCardBLASMetadata,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    localInvDir: vec3<f32>,
    state: ptr<function, HitState>) {
    (*state).iterationCount += 1u;
    if localPrimitiveIndex >= rbvhMetadata.primitiveCount {
        return;
    }

    let originalPrimitiveIndex = blasPrimitiveRemap[rbvhMetadata.primitiveRemapOffset + localPrimitiveIndex];
#ifndef SWRT_DISABLE_OVERLAY
    if rbvh_card_aabb_hit_local(rbvhMetadata, originalPrimitiveIndex, localOrigin, localInvDir, (*state).bestT) {
        mark_debug_rbvh_card_aabb(state);
    }
#endif

    let hitTLocal = trace_rbvh_card_local(rbvhMetadata, originalPrimitiveIndex, localOrigin, localDirection, localInvDir, (*state).bestT);
    if hitTLocal < 0.0 {
        return;
    }

    let worldT = hitTLocal;
    if worldT < uniforms.tMin || worldT > uniforms.tMax || worldT >= (*state).bestT {
        return;
    }

    update_best_hit_local(state, worldT, vec2<f32>(0.0), originalPrimitiveIndex, instanceIndex);
}

fn trace_blas_primitive_leaf_any_hit(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    metadata: BLASMetadataCommon,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    maxRayT: f32) -> bool {
    return trace_blas_primitive_leaf_any_hit_cached(
        rayOrigin,
        rayDirection,
        instance,
        metadata,
        load_trace_primitive_metadata(instance.blasHandle),
        localPrimitiveIndex,
        localOrigin,
        localDirection,
        safe_inverse_direction(localDirection),
        maxRayT);
}

fn trace_blas_primitive_leaf_any_hit_cached(
    rayOrigin: vec3<f32>,
    rayDirection: vec3<f32>,
    instance: InstanceGPU,
    metadata: BLASMetadataCommon,
    rbvhMetadata: RBVHCardBLASMetadata,
    localPrimitiveIndex: u32,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    localInvDir: vec3<f32>,
    maxRayT: f32) -> bool {
    if localPrimitiveIndex >= rbvhMetadata.primitiveCount {
        return false;
    }

    let originalPrimitiveIndex = blasPrimitiveRemap[rbvhMetadata.primitiveRemapOffset + localPrimitiveIndex];
    let hitTLocal = trace_rbvh_card_local(rbvhMetadata, originalPrimitiveIndex, localOrigin, localDirection, localInvDir, maxRayT);
    if hitTLocal < 0.0 {
        return false;
    }

    let worldT = hitTLocal;
    return worldT >= uniforms.tMin && worldT <= maxRayT;
}
