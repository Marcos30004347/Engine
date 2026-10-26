#ifndef SPARSE_VOXEL_BRICK_MICRO_COMMON_WGSL
#define SPARSE_VOXEL_BRICK_MICRO_COMMON_WGSL

// Brick-side exact-microgeometry helpers shared by the hardware raster fragment
// and the tiled compute raster.
//
// These lived in sparse_voxel_brick_draw.wgsl while the fragment was the only
// caller. H1 gives the compute path the same exact-micro carving, and a hit that
// two paths must agree on to the bit cannot be reached through two copies of the
// arithmetic: the comment on brickTraceExactMicroHeightfield already said the
// point of factoring it out was to give both callers "the same floating-point
// expression tree". A duplicated copy silently drifts under any compiler
// difference between a fragment and a compute entry point.
//
// Depends on sparse_voxel_micro_geometry.wgsl (and therefore on the geometry IR
// includes ahead of it), so include it after those. Like them it declares no
// bindings - every input arrives as a parameter, which is exactly what lets the
// compute kernel call it without inheriting the fragment's binding layout.

fn brickSafeDirection(direction : vec3<f32>) -> vec3<f32> {
    let parallel = abs(direction) < vec3<f32>(1.0e-20);
    let parallelDirection = select(
        vec3<f32>(-1.0e-20), vec3<f32>(1.0e-20),
        direction >= vec3<f32>(0.0));
    return select(direction, parallelDirection, parallel);
}

fn brickSafeInverseDirection(direction : vec3<f32>) -> vec3<f32> {
    return vec3<f32>(1.0) / brickSafeDirection(direction);
}

struct SparseVoxelTerrainPatchHit {
    distance : f32,
    valid : bool,
}

fn brickTerrainPatchCorner(surfaceData : vec2<u32>, corner : u32) -> f32 {
    var quantized = 0u;
    if (corner == 0u) {
        quantized = surfaceData.x & 0x3fffu;
    } else if (corner == 1u) {
        quantized = (surfaceData.x >> 14u) & 0x3fffu;
    } else if (corner == 2u) {
        quantized = ((surfaceData.x >> 28u) |
            ((surfaceData.y & 0x03ffu) << 4u)) & 0x3fffu;
    } else {
        quantized = (surfaceData.y >> 10u) & 0x3fffu;
    }
    return f32(quantized) / 16383.0;
}

fn brickTerrainPatchIsEmpty(surfaceData : vec2<u32>) -> bool {
    return all(surfaceData == vec2<u32>(0u));
}

// Exact-micro prepared records retain their compact four-UNorm6 payload. Only
// descriptor-free analytic terrain consumes the full UNorm14 sidecar.
fn brickTerrainPatchLegacyData(surfaceData : vec2<u32>) -> u32 {
    var packed = 0u;
    for (var corner = 0u; corner < 4u; corner += 1u) {
        let quantized = u32(round(
            brickTerrainPatchCorner(surfaceData, corner) * 63.0));
        packed |= (quantized & 0x3fu) << (corner * 6u);
    }
    return packed;
}

fn brickTerrainPatchSignedHeight(
    localPosition : vec3<f32>, corners : vec4<f32>) -> f32 {
    let negativeZ = mix(corners.x, corners.y, localPosition.x);
    let positiveZ = mix(corners.z, corners.w, localPosition.x);
    return localPosition.y - mix(negativeZ, positiveZ, localPosition.z);
}

// Intersect the filled half-space below a bilinear terrain patch. Unlike the
// staircase microgeometry path this performs no subdivision or descriptor
// lookup: one quadratic is paid only for a partially covered terrain voxel.
// Shared corner samples make adjacent cells C0-continuous.
fn brickTraceTerrainSurfacePatch(
    brickMinimum : vec3<f32>, parentCell : vec3<u32>, voxelSize : f32,
    origin : vec3<f32>, direction : vec3<f32>,
    entryT : f32, exitT : f32,
    surfaceData : vec2<u32>) -> SparseVoxelTerrainPatchHit {
    if (brickTerrainPatchIsEmpty(surfaceData) || exitT < entryT) {
        return SparseVoxelTerrainPatchHit(exitT, false);
    }
    let parentMinimum =
        brickMinimum + vec3<f32>(parentCell) * voxelSize;
    // Solve relative to the cell entry point. With a 10 cm voxel and terrain
    // tens of kilometres from the floating origin, solving from localOrigin
    // forms values around 500,000 and then cancels them with direction * t.
    // The lost f32 bits turn valid intersections into a regular pattern of
    // misses. Entry-relative coefficients stay close to the unit cell.
    let localDirection = direction / voxelSize;
    let corners = vec4<f32>(
        brickTerrainPatchCorner(surfaceData, 0u),
        brickTerrainPatchCorner(surfaceData, 1u),
        brickTerrainPatchCorner(surfaceData, 2u),
        brickTerrainPatchCorner(surfaceData, 3u));
    let entryPosition = fma(
        direction, vec3<f32>(entryT), origin - parentMinimum) / voxelSize;
    let tolerance = 2.0e-4;
    if (brickTerrainPatchSignedHeight(entryPosition, corners) <= tolerance) {
        return SparseVoxelTerrainPatchHit(entryT, true);
    }

    let crossTerm = corners.x - corners.y - corners.z + corners.w;
    let xTerm = corners.y - corners.x;
    let zTerm = corners.z - corners.x;
    let a = -crossTerm * localDirection.x * localDirection.z;
    let b = localDirection.y - xTerm * localDirection.x -
        zTerm * localDirection.z - crossTerm *
        (entryPosition.x * localDirection.z +
         entryPosition.z * localDirection.x);
    let c = brickTerrainPatchSignedHeight(entryPosition, corners);

    var roots = vec2<f32>(1.0e30);
    var rootCount = 0u;
    if (abs(a) <= 1.0e-8) {
        if (abs(b) > 1.0e-8) {
            roots.x = -c / b;
            rootCount = 1u;
        }
    } else {
        let discriminant = b * b - 4.0 * a * c;
        let discriminantTolerance = 1.0e-6 *
            max(1.0, max(b * b, abs(4.0 * a * c)));
        if (discriminant >= -discriminantTolerance) {
            let rootSpan = sqrt(max(discriminant, 0.0));
            // The direct quadratic formula loses the near root when b and
            // sqrt(discriminant) almost cancel, which is common for shallow
            // terrain rays. Compute the first root through q and recover the
            // second from the product of roots instead.
            let signedSpan = select(-rootSpan, rootSpan, b >= 0.0);
            let q = -0.5 * (b + signedSpan);
            if (abs(q) > 1.0e-12) {
                roots = vec2<f32>(q / a, c / q);
            } else {
                let repeatedRoot = -b / (2.0 * a);
                roots = vec2<f32>(repeatedRoot);
            }
            if (roots.y < roots.x) {
                roots = roots.yx;
            }
            rootCount = 2u;
        }
    }

    for (var rootIndex = 0u; rootIndex < rootCount; rootIndex += 1u) {
        let relativeRoot = roots[rootIndex];
        if (relativeRoot < -tolerance ||
            relativeRoot > exitT - entryT + tolerance) {
            continue;
        }
        let position = entryPosition + localDirection * relativeRoot;
        if (position.x >= -tolerance && position.x <= 1.0 + tolerance &&
            position.y >= -tolerance && position.y <= 1.0 + tolerance &&
            position.z >= -tolerance && position.z <= 1.0 + tolerance) {
            return SparseVoxelTerrainPatchHit(
                clamp(entryT + relativeRoot, entryT, exitT), true);
        }
    }

    // If the interval ends inside the filled half-space, continuity guarantees
    // a crossing even when the closed-form roots were lost to f32 rounding at
    // a cell boundary. Bisect only this sign-bracketed failure path; rays that
    // remain above the patch pay no iterations.
    let intervalLength = max(exitT - entryT, 0.0);
    let exitPosition = entryPosition + localDirection * intervalLength;
    var bracketedInsideT = intervalLength;
    var bracketed =
        brickTerrainPatchSignedHeight(exitPosition, corners) <= tolerance;
    // An upward-opening quadratic can enter and leave above the patch while
    // dipping into the filled half-space between those endpoints. Its vertex
    // brackets the first crossing when the closed-form roots lose enough
    // precision to fail the unit-cell bounds check.
    if (!bracketed && a > 1.0e-8) {
        let vertexT = -b / (2.0 * a);
        if (vertexT > 0.0 && vertexT < intervalLength &&
            fma(a, vertexT * vertexT, fma(b, vertexT, c)) <= tolerance) {
            bracketedInsideT = vertexT;
            bracketed = true;
        }
    }
    if (bracketed) {
        var outsideT = 0.0;
        var insideT = bracketedInsideT;
        for (var iteration = 0u; iteration < 12u; iteration += 1u) {
            let middleT = 0.5 * (outsideT + insideT);
            let middlePosition =
                entryPosition + localDirection * middleT;
            if (brickTerrainPatchSignedHeight(
                    middlePosition, corners) <= tolerance) {
                insideT = middleT;
            } else {
                outsideT = middleT;
            }
        }
        return SparseVoxelTerrainPatchHit(entryT + insideT, true);
    }

    return SparseVoxelTerrainPatchHit(exitT, false);
}

// Ordinary voxel temporal identity must not depend on a physical residency
// slot. Hash the represented world-space brick and its LOD, so a reused slot
// cannot inherit the previous occupant's lighting. Generated height-field
// terrain needs continuity across LOD as well and uses the world-column helper
// below instead. Bit patterns keep negative coordinates deterministic.
//
// Note it takes no distance input. That is why the compute path's differing
// low-bit surfaceT does not disturb temporal identity.
fn brickSurfaceIdentity(
    brickMinimumVoxelSize : vec4<f32>, cellIndex : u32) -> u32 {
    var hash = bitcast<u32>(brickMinimumVoxelSize.x) * 0x9e3779b9u;
    hash = (hash ^ bitcast<u32>(brickMinimumVoxelSize.y)) * 0x85ebca6bu;
    hash = (hash ^ bitcast<u32>(brickMinimumVoxelSize.z)) * 0xc2b2ae35u;
    hash = (hash ^ bitcast<u32>(brickMinimumVoxelSize.w)) * 0x27d4eb2fu;
    hash = hash ^ (cellIndex * 0x165667b1u);
    return hash ^ (hash >> 16u);
}

// Generated terrain is a height field, so its stable receiver address is the
// finest-grid world column under the hit. Parent and child pages carry the same
// canonical grid even though their brick cells and physical allocations differ;
// deriving the column from the hit position therefore preserves temporal
// identity across an in-place page refinement. Y is intentionally omitted: LOD
// replacement changes the represented height while preserving the ground being
// refined.
fn brickTerrainSurfaceIdentity(
    brickMinimumVoxelSize : vec4<f32>,
    canonicalGridOriginScale : vec4<i32>,
    worldPosition : vec3<f32>) -> u32 {
    let canonicalScale = max(canonicalGridOriginScale.w, 1);
    let finestVoxelSize =
        brickMinimumVoxelSize.w / f32(canonicalScale);
    let localColumn = vec2<i32>(floor(
        (worldPosition.xz - brickMinimumVoxelSize.xz) /
            max(finestVoxelSize, 1.0e-8)));
    let canonicalColumn = canonicalGridOriginScale.xz + localColumn;
    var hash = bitcast<u32>(canonicalColumn.x) * 0x9e3779b9u;
    hash = (hash ^ bitcast<u32>(canonicalColumn.y)) * 0x85ebca6bu;
    return hash ^ (hash >> 16u);
}

fn brickMicroSurfaceIdentity(
    parentIdentity : u32, cell : vec3<u32>, face : u32,
    descriptorId : u32) -> u32 {
    var hash = parentIdentity ^ (cell.x * 0x8da6b343u);
    hash = (hash ^ (cell.y * 0xd8163841u)) * 0x85ebca6bu;
    hash = (hash ^ (cell.z * 0xcb1ab31fu)) * 0xc2b2ae35u;
    hash ^= face * 0x27d4eb2fu;
    hash ^= descriptorId * 0x165667b1u;
    return hash ^ (hash >> 16u);
}

fn brickMicroCanonicalParentKey(
    canonicalGridOriginScale : vec4<i32>,
    parentCell : vec3<i32>) -> vec3<i32> {
    return canonicalGridOriginScale.xyz +
        parentCell * canonicalGridOriginScale.w;
}

fn brickMicroPayload(
    descriptorId : u32, cell : vec3<u32>, carveFace : u32,
    subdivision : u32, heightfield : bool,
    terrainSlope : bool) -> u32 {
    var payloadCell = cell;
    if (heightfield) {
        let frame = sparseVoxelExactMicroFaceFrame(carveFace);
        payloadCell.x = select(
            cell[frame.y], subdivision - 1u - cell[frame.y],
            (frame.w & 1u) != 0u);
        payloadCell.y = select(
            cell[frame.z], subdivision - 1u - cell[frame.z],
            (frame.w & 2u) != 0u);
        payloadCell.z = select(
            cell[frame.x], subdivision - 1u - cell[frame.x],
            (carveFace & 1u) != 0u);
    }
    return (descriptorId & 0xffffu) |
        ((payloadCell.x & 0x0fu) << 16u) |
        ((payloadCell.y & 0x0fu) << 20u) |
        ((payloadCell.z & 0x0fu) << 24u) |
        SPARSE_VOXEL_MICRO_PAYLOAD_VALID |
        select(0u, SPARSE_VOXEL_MICRO_PAYLOAD_HEIGHTFIELD, heightfield) |
        select(0u, SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE, terrainSlope);
}

fn brickMicroFallbackPayload(
    descriptorId : u32, terrainSlope : bool) -> u32 {
    return (descriptorId & 0xffffu) |
        SPARSE_VOXEL_MICRO_PAYLOAD_VALID |
        SPARSE_VOXEL_MICRO_PAYLOAD_FALLBACK |
        select(0u, SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE, terrainSlope);
}

struct SparseVoxelBrickParentInterval {
    nearT : vec3<f32>,
    farT : vec3<f32>,
    entry : f32,
    exit : f32,
}

// Exact-micro tracing has one canonical parent interval in both the incumbent
// fine DDA and the R2 direct path. The legacy walk still selects the parent and
// owns all X/Y/Z ties; only the algebraically equivalent interval passed to the
// micro tracer is canonicalized here. This avoids a one-ULP drift caused by
// accumulated fine/coarse DDA boundary additions at oblique views.
fn brickExactMicroParentInterval(
    brickMinimum : vec3<f32>, parentCell : vec3<u32>, voxelSize : f32,
    origin : vec3<f32>, direction : vec3<f32>,
    nearDistance : f32) -> SparseVoxelBrickParentInterval {
    let parentMinimum = brickMinimum + vec3<f32>(parentCell) * voxelSize;
    let parentMaximum = brickMinimum +
        vec3<f32>(parentCell + vec3<u32>(1u)) * voxelSize;
    let inverseDirection = brickSafeInverseDirection(direction);
    let slabA = fma(
        parentMinimum - origin, inverseDirection, vec3<f32>(0.0));
    let slabB = fma(
        parentMaximum - origin, inverseDirection, vec3<f32>(0.0));
    let nearT = min(slabA, slabB);
    let farT = max(slabA, slabB);
    let entry = max(
        nearDistance, max(nearT.x, max(nearT.y, nearT.z)));
    return SparseVoxelBrickParentInterval(
        nearT, farT,
        entry,
        min(farT.x, min(farT.y, farT.z)));
}

// Keep the parent-local ray construction and exact heightfield call in one
// shared function. Besides reducing duplicated hot code, this gives the
// incumbent and direct shader variants the same floating-point expression
// tree for the returned world-ray distance.
fn brickTraceExactMicroHeightfield(
    descriptor : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32, parentKey : vec3<i32>,
    brickMinimum : vec3<f32>, parentCell : vec3<u32>, voxelSize : f32,
    origin : vec3<f32>, direction : vec3<f32>,
    nearDistance : f32, parentFace : u32,
    microContext : u32, microSurfaceData : u32,
    solidStaircase : bool,
    columnCache : vec4<u32>) -> SparseVoxelExactMicroHit {
    let parentMinimum = brickMinimum + vec3<f32>(parentCell) * voxelSize;
    let localOrigin = (origin - parentMinimum) / voxelSize;
    let localDirection = direction / voxelSize;
    let interval = brickExactMicroParentInterval(
        brickMinimum, parentCell, voxelSize, origin, direction,
        nearDistance);
    let subdivision = descriptor.geometry & 0x0fu;
    if (solidStaircase) {
        return traceSparseVoxelExactMicroSolidStaircaseInterior(
            descriptorId, subdivision, localOrigin, localDirection,
            interval.entry, interval.exit);
    }
    return traceSparseVoxelExactMicroOneFace(
        descriptor, descriptorId, geometryExecutionFlags, parentKey,
        localOrigin, localDirection, interval.entry, interval.exit,
        parentFace, microContext, microSurfaceData, 0u, columnCache);
}

#endif // SPARSE_VOXEL_BRICK_MICRO_COMMON_WGSL
