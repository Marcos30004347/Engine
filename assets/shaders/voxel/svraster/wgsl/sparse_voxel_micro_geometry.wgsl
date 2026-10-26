// Analytic intersection with the piecewise-constant 2-by-3 micro height field.
// Unlike a single parallax lookup, this detects the first crossed cell wall,
// making recessed horizontal faces and their vertical risers one connected
// screen-space surface. The routine is deterministic and contains no frame
// index, derivatives or texture sampling.
//
// The staircase-surface column depth and its compact cache live in
// sparse_voxel_micro_columns.wgsl so MicroPrepare can generate the cache
// without acquiring this file's geometry-IR bindings.
#include "sparse_voxel_micro_columns.wgsl"

fn sparseVoxelMicroHash(coordinate : vec3<i32>, face : u32) -> u32 {
    var value = bitcast<u32>(coordinate.x) * 0x8da6b343u;
    value ^= bitcast<u32>(coordinate.y) * 0xd8163841u;
    value ^= bitcast<u32>(coordinate.z) * 0xcb1ab31fu;
    value ^= face * 0x9e3779b9u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn sparseVoxelMicroHeight(
    cell : vec2<i32>, plane : i32, face : u32) -> f32 {
    let hash = sparseVoxelMicroHash(vec3<i32>(cell, plane), face);
    return f32(hash & 0xffffu) * (2.0 / 65535.0) - 1.0;
}

// Geometry, shading, and the band-limited colour fallback must use exactly the
// same handoff. Exact geometry remains fully active while one microcell covers
// at least one rendered pixel. Only sub-pixel cells transition to expected
// coverage, avoiding a distance-aligned LOD band.
const SPARSE_VOXEL_MICRO_FADE_START_PIXELS : f32 = 0.50;
const SPARSE_VOXEL_MICRO_FADE_END_PIXELS : f32 = 1.00;

fn sparseVoxelMicroDetailFade(projectedMicroPixels : f32) -> f32 {
    return smoothstep(
        SPARSE_VOXEL_MICRO_FADE_START_PIXELS,
        SPARSE_VOXEL_MICRO_FADE_END_PIXELS,
        projectedMicroPixels);
}

struct SparseVoxelMicroHit {
    position : vec3<f32>,
    normal : vec3<f32>,
    tangentU : vec3<f32>,
    tangentV : vec3<f32>,
    cell : vec2<i32>,
    plane : i32,
    face : u32,
    height : f32,
    cellRecess : f32,
    surfaceDepth : f32,
    detailFade : f32,
    isWall : bool,
    valid : bool,
}

fn sparseVoxelInactiveMicroHit(
    position : vec3<f32>, normal : vec3<f32>) -> SparseVoxelMicroHit {
    return SparseVoxelMicroHit(
        position, normal, vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 1.0, 0.0), vec2<i32>(0), 0, 0u,
        0.0, 0.0, 0.0, 0.0, false, false);
}

fn traceSparseVoxelMicroSurface(
    basePosition : vec3<f32>, rayDirection : vec3<f32>,
    faceNormal : vec3<f32>, surfaceT : f32, packedFace : u32,
    focalPrincipal : vec4<f32>, baseVoxelSize : f32,
    maximumRecess : f32) -> SparseVoxelMicroHit {
    if (baseVoxelSize <= 1.0e-6 || maximumRecess <= 1.0e-6) {
        return sparseVoxelInactiveMicroHit(basePosition, faceNormal);
    }

    let microWorldSize = baseVoxelSize / 3.0;
    let projectedMicroPixels = microWorldSize *
        min(abs(focalPrincipal.x), abs(focalPrincipal.y)) /
        max(surfaceT, 1.0e-4);
    // Some streamed/packed terrain paths preserve the face axis but not a
    // consistently camera-facing sign. Recess distance only needs the cosine
    // magnitude; clamping the signed value to zero made those faces lose their
    // detail as the camera crossed them at close range.
    let facing = abs(dot(rayDirection, faceNormal));
    // Distance is the only visibility fade. In particular, do not erase the
    // effect at close grazing angles: that was perceived as the detail turning
    // off when the camera approached a face.
    let detailFade = sparseVoxelMicroDetailFade(projectedMicroPixels);
    if (detailFade <= 0.0) {
        return sparseVoxelInactiveMicroHit(basePosition, faceNormal);
    }

    let face = packedFace & 7u;
    let axis = face & 3u;
    var tangentU = vec3<f32>(1.0, 0.0, 0.0);
    var tangentV = vec3<f32>(0.0, 1.0, 0.0);
    if (axis == 0u) {
        tangentU = vec3<f32>(0.0, 0.0, 1.0);
        tangentV = vec3<f32>(0.0, 1.0, 0.0);
    } else if (axis == 1u) {
        tangentU = vec3<f32>(1.0, 0.0, 0.0);
        tangentV = vec3<f32>(0.0, 0.0, 1.0);
    }

    let inside = basePosition - faceNormal * baseVoxelSize * 0.01;
    let solidCell = vec3<i32>(floor(inside / baseVoxelSize));
    let plane = solidCell[axis];
    let coordinate = vec2<f32>(
        dot(basePosition, tangentU) / baseVoxelSize * 2.0,
        dot(basePosition, tangentV) / baseVoxelSize * 3.0);
    let coordinateDirection = vec2<f32>(
        dot(rayDirection, tangentU) / baseVoxelSize * 2.0,
        dot(rayDirection, tangentV) / baseVoxelSize * 3.0);
    let inwardSpeed = max(facing, 1.0e-5);
    var cell = vec2<i32>(floor(coordinate));
    var fallbackDistance = 0.0;
    var fallbackDepth = 0.0;
    var fallbackNormal = faceNormal;
    var fallbackCell = cell;
    var fallbackHeight = sparseVoxelMicroHeight(cell, plane, face);
    var fallbackRecess = (fallbackHeight * 0.5 + 0.5) *
        maximumRecess * detailFade;

    // At a one-centimetre recess and the smallest supported facing angle this
    // normally crosses zero or one cell. Four exact DDA boundary tests retain
    // headroom without turning this fullscreen pass into a ray marcher.
    for (var iteration = 0u; iteration < 4u; iteration += 1u) {
        let height = sparseVoxelMicroHeight(cell, plane, face);
        let cellRecess = (height * 0.5 + 0.5) *
            maximumRecess * detailFade;
        let horizontalDistance = cellRecess / inwardSpeed;

        var boundaryU = 1.0e30;
        if (coordinateDirection.x > 1.0e-7) {
            boundaryU = (f32(cell.x + 1) - coordinate.x) /
                coordinateDirection.x;
        } else if (coordinateDirection.x < -1.0e-7) {
            boundaryU = (f32(cell.x) - coordinate.x) /
                coordinateDirection.x;
        }
        var boundaryV = 1.0e30;
        if (coordinateDirection.y > 1.0e-7) {
            boundaryV = (f32(cell.y + 1) - coordinate.y) /
                coordinateDirection.y;
        } else if (coordinateDirection.y < -1.0e-7) {
            boundaryV = (f32(cell.y) - coordinate.y) /
                coordinateDirection.y;
        }
        let boundaryDistance = min(boundaryU, boundaryV);

        if (horizontalDistance <= boundaryDistance + 1.0e-6) {
            return SparseVoxelMicroHit(
                basePosition + rayDirection * horizontalDistance,
                faceNormal, tangentU, tangentV, cell, plane, face, height,
                cellRecess, cellRecess, detailFade, false, true);
        }

        let crossedU = boundaryU <= boundaryV;
        var nextCell = cell;
        var wallNormal = vec3<f32>(0.0);
        if (crossedU) {
            let direction = select(-1, 1, coordinateDirection.x > 0.0);
            nextCell.x += direction;
            wallNormal = tangentU * -f32(direction);
        } else {
            let direction = select(-1, 1, coordinateDirection.y > 0.0);
            nextCell.y += direction;
            wallNormal = tangentV * -f32(direction);
        }

        let nextHeight = sparseVoxelMicroHeight(nextCell, plane, face);
        let nextRecess = (nextHeight * 0.5 + 0.5) *
            maximumRecess * detailFade;
        let boundaryDepth = boundaryDistance * inwardSpeed;
        fallbackDistance = boundaryDistance;
        fallbackDepth = boundaryDepth;
        fallbackNormal = wallNormal;
        fallbackCell = nextCell;
        fallbackHeight = nextHeight;
        fallbackRecess = nextRecess;
        // Crossing from a deeper cavity directly into the neighbouring solid
        // is an exact hit on its vertical riser.
        if (boundaryDepth + 1.0e-6 >= nextRecess) {
            return SparseVoxelMicroHit(
                basePosition + rayDirection * boundaryDistance,
                wallNormal, tangentU, tangentV, nextCell, plane, face,
                nextHeight, nextRecess, boundaryDepth, detailFade, true, true);
        }
        cell = nextCell;
    }

    // Extremely oblique rays can exceed the bounded DDA. Keep the last crossed
    // riser instead of disabling the effect or returning a detached horizontal
    // plate. This is conservative in depth and remains a connected surface.
    return SparseVoxelMicroHit(
        basePosition + rayDirection * fallbackDistance,
        fallbackNormal, tangentU, tangentV, fallbackCell, plane, face,
        fallbackHeight, fallbackRecess, fallbackDepth, detailFade, true, true);
}

// Material-driven exact carving. This is separate from the older analytic
// appearance function above: it changes visibility and walks discrete N x N
// columns inside one occupied parent voxel. Descriptor face order is
// -X,+X,-Y,+Y,-Z,+Z; returned faces use the renderer's axis/sign packing.
const SPARSE_VOXEL_EXACT_MICRO_CONSTANT : u32 = 1u;
const SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS : u32 = 2u;
const SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY : u32 = 3u;
const SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS : u32 = 4u;
const SPARSE_VOXEL_EXACT_MICRO_STAIRCASE : u32 = 5u;
const SPARSE_VOXEL_EXACT_MICRO_MAX_SUBDIVISION : u32 = 8u;
const SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR : u32 = 0u;
const SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE : u32 = 1u;
const SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X : u32 = 10u;
const SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_X : u32 = 11u;
const SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_Z : u32 = 12u;
const SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z : u32 = 13u;
const SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG : u32 = 0x80u;
const SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_MASK : u32 = 0x7fu;
const SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_QUANTIZATION : f32 = 126.0;
const SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG : u32 = 1u << 31u;
const SPARSE_VOXEL_MICRO_PAYLOAD_VALID : u32 = 1u << 28u;
const SPARSE_VOXEL_MICRO_PAYLOAD_HEIGHTFIELD : u32 = 1u << 29u;
const SPARSE_VOXEL_MICRO_PAYLOAD_FALLBACK : u32 = 1u << 30u;
const SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE : u32 = 1u << 31u;

fn sparseVoxelExactMicroIsTerrainStaircaseSurface(
    profile : u32, context : u32) -> bool {
    return profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE &&
        (context == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE ||
         (context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u);
}

// Program metadata is immutable for the duration of a draw, but exact micro
// traversal used to fetch this word again in every helper and every microcell
// step. Load it once at the parent candidate and carry the register value down
// the traversal instead. An absent/out-of-range program retains the legacy
// zero-flags behavior.
fn sparseVoxelExactMicroGeometryExecutionFlags(descriptorId : u32) -> u32 {
#ifdef VOXEL_GEOMETRY_IR_ENABLED
    if (descriptorId < arrayLength(&voxelGeometryPrograms)) {
        return voxelGeometryPrograms[descriptorId].executionFlags;
    }
#endif
    return 0u;
}

struct SparseVoxelExactMicroHit {
    distance : f32,
    cell : vec3<u32>,
    face : u32,
    descriptorId : u32,
    steps : u32,
    valid : bool,
}

fn sparseVoxelExactMicroMiss(
    distance : f32, descriptorId : u32, steps : u32) -> SparseVoxelExactMicroHit {
    return SparseVoxelExactMicroHit(
        distance, vec3<u32>(0u), 0u, descriptorId, steps, false);
}

fn sparseVoxelExactMicroMix(valueIn : u32) -> u32 {
    var value = valueIn;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn sparseVoxelExactMicroColumnHash(
    seed : u32, parentKey : vec3<i32>, face : u32,
    faceCell : vec2<u32>, profile : u32) -> u32 {
    var value = seed ^ 0x9e3779b9u;
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.x));
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.y));
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.z));
    value = sparseVoxelExactMicroMix(value ^ (face * 0x85ebca6bu));
    value = sparseVoxelExactMicroMix(value ^ (faceCell.x * 0xc2b2ae35u));
    value = sparseVoxelExactMicroMix(value ^ (faceCell.y * 0x27d4eb2fu));
    return sparseVoxelExactMicroMix(value ^ (profile * 0x165667b1u));
}

fn sparseVoxelExactMicroVolumeHash(
    seed : u32, parentKey : vec3<i32>, cell : vec3<u32>,
    subdivision : u32, profile : u32) -> u32 {
    // Perform the global-address arithmetic as u32 so negative parent keys and
    // wraparound have identical, defined hash inputs on CPU and GPU.
    let canonicalCell = vec3<u32>(
        bitcast<u32>(parentKey.x) * subdivision + cell.x,
        bitcast<u32>(parentKey.y) * subdivision + cell.y,
        bitcast<u32>(parentKey.z) * subdivision + cell.z);
    var value = seed ^ 0x517cc1b7u;
    value = sparseVoxelExactMicroMix(
        value ^ canonicalCell.x * 0x8da6b343u);
    value = sparseVoxelExactMicroMix(
        value ^ canonicalCell.y * 0xd8163841u);
    value = sparseVoxelExactMicroMix(
        value ^ canonicalCell.z * 0xcb1ab31fu);
    return sparseVoxelExactMicroMix(value ^ profile * 0x165667b1u);
}

fn sparseVoxelExactMicroSelector(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    parentKey : vec3<i32>, salt : u32) -> f32 {
    var value = sparseVoxelExactMicroMix(
        (material.geometry >> 24u) ^ descriptorId ^ salt);
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.x));
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.y));
    value = sparseVoxelExactMicroMix(value ^ bitcast<u32>(parentKey.z));
    return f32(value & 0x00ffffffu) / 16777215.0;
}

fn sparseVoxelExactMicroHandoffWeight(
    material : SparseVoxelMicroMaterial,
    projectedMicroPixels : f32) -> f32 {
    let encodedScale = (material.shading >> 24u) & 0xffu;
    let handoffScale = select(
        1.0, clamp(f32(encodedScale) / 128.0, 0.25, 1.0),
        encodedScale != 0u);
    return smoothstep(
        SPARSE_VOXEL_MICRO_FADE_START_PIXELS * handoffScale,
        SPARSE_VOXEL_MICRO_FADE_END_PIXELS * handoffScale,
        projectedMicroPixels);
}

// Expected surviving volume used only by the sub-pixel coverage handoff.
// Constant face cuts and volume density have exact closed forms. Independent
// hash columns use the product of each face's expected surviving fraction;
// this is deliberately band-limited and never replaces exact occupancy.
fn sparseVoxelExactMicroExpectedCoverage(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32,
    parentKey : vec3<i32>, context : u32,
    surfaceData : u32) -> f32 {
    let subdivision = material.geometry & 0x0fu;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    let authoredMaximumDepthFixed =
        (material.geometry >> 4u) & 0xffu;
    let maximumDepth = f32(select(
        authoredMaximumDepthFixed, subdivision * 16u,
        profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE)) / 16.0;
    if (subdivision == 0u) {
        return 1.0;
    }
#ifdef VOXEL_GEOMETRY_IR_ENABLED
    if ((geometryExecutionFlags & VOXEL_GEOMETRY_PROGRAM_ENABLED) != 0u) {
        if ((geometryExecutionFlags &
             VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u) {
            return voxelGeometryFieldCacheCoverage(
                parentKey, descriptorId, subdivision);
        }
        var face = 0u;
        for (var candidate = 0u; candidate < 6u; candidate += 1u) {
            if ((faceMask & (1u << candidate)) != 0u) {
                face = candidate;
                break;
            }
        }
        var total = 0.0;
        for (var v = 0u; v < 4u; v += 1u) {
            for (var u = 0u; u < 4u; u += 1u) {
                let sampleCell = vec2<u32>(
                    min(subdivision - 1u, u * subdivision / 4u),
                    min(subdivision - 1u, v * subdivision / 4u));
                var baseSurface = VOXEL_GEOMETRY_FIXED_ONE;
                if (face == 3u &&
                    (context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u) {
                    let localX = (f32(sampleCell.x) + 0.5) / f32(subdivision);
                    let localZ =
                        (f32(subdivision - 1u - sampleCell.y) + 0.5) /
                        f32(subdivision);
                    let negativeZ = mix(
                        sparseVoxelExactMicroSurfaceCorner(surfaceData, 0u),
                        sparseVoxelExactMicroSurfaceCorner(surfaceData, 1u),
                        localX);
                    let positiveZ = mix(
                        sparseVoxelExactMicroSurfaceCorner(surfaceData, 2u),
                        sparseVoxelExactMicroSurfaceCorner(surfaceData, 3u),
                        localX);
                    baseSurface = i32(round(
                        mix(negativeZ, positiveZ, localZ) * 65536.0));
                }
                let sample = voxelGeometryEvaluate(
                    descriptorId, parentKey, sampleCell, vec3<u32>(0u),
                    face, subdivision,
                    material.geometry >> 24u, context, baseSurface, 0u,
                    voxelGeometryDefaultMaterialFields(descriptorId));
                total += f32(sample.retainedHeight) / 65536.0 *
                    f32(sample.coverage) / 65536.0;
            }
        }
        return total / 16.0;
    }
#endif
    if (profile == SPARSE_VOXEL_EXACT_MICRO_CONSTANT) {
        var coverage = 1.0;
        for (var axis = 0u; axis < 3u; axis += 1u) {
            let negativeDepth = select(
                0.0, maximumDepth,
                (faceMask & (1u << (axis * 2u))) != 0u);
            let positiveDepth = select(
                0.0, maximumDepth,
                (faceMask & (1u << (axis * 2u + 1u))) != 0u);
            let surviving = f32(subdivision) - min(
                f32(subdivision), negativeDepth + positiveDepth);
            coverage *= surviving / f32(subdivision);
        }
        return coverage;
    }
    if (profile == SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY) {
        return 1.0 - maximumDepth / f32(subdivision);
    }
    if (profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE) {
        let contextClass = context;
        if (contextClass == SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR) {
            return 1.0;
        }
        if (contextClass == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE) {
            return 1.0;
        }
        let staircaseSpan = f32(subdivision - 1u);
        let wallSurvival = clamp(
            1.0 - staircaseSpan * 0.5 / f32(subdivision), 0.0, 1.0);
        if (contextClass >= 10u && contextClass <= 13u) {
            return wallSurvival;
        }
        if ((context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u) {
            // The integral of a bilinear patch over its unit square is the
            // mean of its corners. This is also the band-limited coverage used
            // after exact microcolumns become sub-pixel.
            return (sparseVoxelExactMicroSurfaceCorner(surfaceData, 0u) +
                    sparseVoxelExactMicroSurfaceCorner(surfaceData, 1u) +
                    sparseVoxelExactMicroSurfaceCorner(surfaceData, 2u) +
                    sparseVoxelExactMicroSurfaceCorner(surfaceData, 3u)) * 0.25;
        }
    }
    let faceSurvival = 1.0 -
        (maximumDepth * 0.5) / f32(subdivision);
    return pow(clamp(faceSurvival, 0.0, 1.0), f32(countOneBits(faceMask)));
}

fn sparseVoxelExactMicroFaceFrame(face : u32) -> vec4<u32> {
    // axis, U axis, V axis, inversion bits (U=1,V=2).
    switch face {
        case 0u: { return vec4<u32>(0u, 2u, 1u, 0u); }
        case 1u: { return vec4<u32>(0u, 2u, 1u, 1u); }
        case 2u: { return vec4<u32>(1u, 0u, 2u, 0u); }
        case 3u: { return vec4<u32>(1u, 0u, 2u, 2u); }
        case 4u: { return vec4<u32>(2u, 0u, 1u, 1u); }
        default: { return vec4<u32>(2u, 0u, 1u, 0u); }
    }
}

fn sparseVoxelExactMicroFloorDivide4(value : i32) -> i32 {
    let quotient = value / 4;
    return quotient - select(0, 1, value < 0 && (value % 4) != 0);
}

fn sparseVoxelExactMicroCanonicalFaceColumn(
    parentKey : vec3<i32>, subdivision : u32, face : u32,
    faceCell : vec2<u32>) -> vec3<i32> {
    let frame = sparseVoxelExactMicroFaceFrame(face);
    var coordinate = parentKey * i32(subdivision);
    let localU = select(
        faceCell.x, subdivision - 1u - faceCell.x,
        (frame.w & 1u) != 0u);
    let localV = select(
        faceCell.y, subdivision - 1u - faceCell.y,
        (frame.w & 2u) != 0u);
    coordinate[frame.y] += i32(localU);
    coordinate[frame.z] += i32(localV);
    coordinate[frame.x] += select(0, i32(subdivision), (face & 1u) != 0u);
    return coordinate;
}

fn sparseVoxelExactMicroCanonicalColumnHash(
    seed : u32, coordinate : vec3<i32>, face : u32,
    profile : u32, salt : u32) -> u32 {
    var value = seed ^ salt;
    value = sparseVoxelExactMicroMix(
        value ^ bitcast<u32>(coordinate.x) * 0x8da6b343u);
    value = sparseVoxelExactMicroMix(
        value ^ bitcast<u32>(coordinate.y) * 0xd8163841u);
    value = sparseVoxelExactMicroMix(
        value ^ bitcast<u32>(coordinate.z) * 0xcb1ab31fu);
    value = sparseVoxelExactMicroMix(value ^ face * 0x85ebca6bu);
    return sparseVoxelExactMicroMix(value ^ profile * 0x165667b1u);
}

fn sparseVoxelExactMicroClumpedColumnDepth(
    seed : u32, parentKey : vec3<i32>, subdivision : u32,
    maximumDepthFixed : u32, face : u32, faceCell : vec2<u32>,
    profile : u32) -> u32 {
    let canonical = sparseVoxelExactMicroCanonicalFaceColumn(
        parentKey, subdivision, face, faceCell);
    let cluster = vec3<i32>(
        sparseVoxelExactMicroFloorDivide4(canonical.x),
        sparseVoxelExactMicroFloorDivide4(canonical.y),
        sparseVoxelExactMicroFloorDivide4(canonical.z));
    let base = i32(sparseVoxelExactMicroCanonicalColumnHash(
        seed, cluster, face, profile, 0x68bc21ebu) %
        (maximumDepthFixed + 1u));
    let jitter = (i32(sparseVoxelExactMicroCanonicalColumnHash(
        seed, canonical, face, profile, 0x02e5be93u) % 3u) - 1) * 16;
    return u32(clamp(base + jitter, 0, i32(maximumDepthFixed)));
}

fn sparseVoxelExactMicroCellCoordinate(
    coordinateIn : f32, direction : f32, subdivision : u32) -> u32 {
    let ownedCoordinate = coordinateIn + select(
        select(0.0, -1.0e-6, direction < 0.0),
        1.0e-6, direction > 0.0);
    return min(
        u32(floor(clamp(ownedCoordinate, 0.0, 1.0) * f32(subdivision))),
        subdivision - 1u);
}

fn sparseVoxelExactMicroCellAt(
    origin : vec3<f32>, direction : vec3<f32>, distance : f32,
    subdivision : u32) -> vec3<u32> {
    let point = origin + direction * distance;
    return vec3<u32>(
        sparseVoxelExactMicroCellCoordinate(point.x, direction.x, subdivision),
        sparseVoxelExactMicroCellCoordinate(point.y, direction.y, subdivision),
        sparseVoxelExactMicroCellCoordinate(point.z, direction.z, subdivision));
}

fn sparseVoxelExactMicroCarveDepth(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32,
    parentKey : vec3<i32>,
    face : u32, faceCell : vec2<u32>, context : u32,
    surfaceData : u32, auxContext : u32,
    columnCache : vec4<u32>) -> f32 {
    let profile = (material.geometry >> 18u) & 0x07u;
    let authoredMaximumDepthFixed =
        (material.geometry >> 4u) & 0xffu;
    let subdivision = material.geometry & 0x0fu;
#ifdef VOXEL_GEOMETRY_IR_ENABLED
    if ((geometryExecutionFlags & VOXEL_GEOMETRY_PROGRAM_ENABLED) != 0u) {
        // Field-driven programs are converted to whole-cell occupancy before
        // traversal and therefore never evaluate a live carve column.
        if ((geometryExecutionFlags &
             VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u) {
            return 0.0;
        }
        var baseSurface = VOXEL_GEOMETRY_FIXED_ONE;
        if (face == 3u &&
            (context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u) {
            let localX = (f32(faceCell.x) + 0.5) / f32(subdivision);
            let localZ =
                (f32(subdivision - 1u - faceCell.y) + 0.5) /
                f32(subdivision);
            let negativeZ = mix(
                sparseVoxelExactMicroSurfaceCorner(surfaceData, 0u),
                sparseVoxelExactMicroSurfaceCorner(surfaceData, 1u), localX);
            let positiveZ = mix(
                sparseVoxelExactMicroSurfaceCorner(surfaceData, 2u),
                sparseVoxelExactMicroSurfaceCorner(surfaceData, 3u), localX);
            baseSurface = i32(round(
                mix(negativeZ, positiveZ, localZ) * 65536.0));
        }
        let evaluated = voxelGeometryEvaluate(
            descriptorId, parentKey, faceCell, vec3<u32>(0u),
            face, subdivision,
            material.geometry >> 24u, context, baseSurface, auxContext,
            voxelGeometryDefaultMaterialFields(descriptorId));
        return f32(evaluated.carveDepth) * f32(subdivision) / 65536.0;
    }
#endif
    let maximumDepthFixed = select(
        authoredMaximumDepthFixed, subdivision * 16u,
        profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE);
    var depthFixed = 0u;
    if (profile == SPARSE_VOXEL_EXACT_MICRO_CONSTANT) {
        depthFixed = maximumDepthFixed;
    } else if (profile == SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS) {
        let seed = material.geometry >> 24u;
        depthFixed = sparseVoxelExactMicroColumnHash(
            seed, parentKey, face, faceCell, profile) %
            (maximumDepthFixed + 1u);
    } else if (profile == SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS) {
        let seed = material.geometry >> 24u;
        depthFixed = sparseVoxelExactMicroClumpedColumnDepth(
            seed, parentKey, material.geometry & 0x0fu, maximumDepthFixed,
            face, faceCell, profile);
    } else if (profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE) {
        let contextClass = context;
        if (contextClass >= 10u && contextClass <= 13u) {
            var wallFace = 0u;
            if (contextClass == SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_X) {
                wallFace = 1u;
            } else if (contextClass == SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_Z) {
                wallFace = 4u;
            } else if (contextClass == SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z) {
                wallFace = 5u;
            }
            if (face == wallFace) {
                depthFixed = faceCell.y * 16u;
            }
        } else if ((context & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u) {
            if (face == 3u) {
                // R3: MicroPrepare may have already quantized every column of
                // this parent. The cached value is produced by the same shared
                // function, so reading it is an exact substitution, not an
                // approximation of one.
                let columnIndex = sparseVoxelExactMicroColumnCacheIndex(
                    subdivision, faceCell);
                var coordinate = 0u;
                if (sparseVoxelExactMicroColumnCacheValid(columnCache)) {
                    coordinate = sparseVoxelExactMicroColumnCacheDepth(
                        columnCache, columnIndex);
                } else {
                    coordinate = sparseVoxelExactMicroStaircaseSurfaceDepth(
                        subdivision, surfaceData, faceCell);
                }
                depthFixed = coordinate * 16u;
            }
        }
    }
    return f32(depthFixed) / 16.0;
}

struct SparseVoxelExactMicroCellBounds {
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    occupied : bool,
}

fn sparseVoxelExactMicroCellBounds(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32,
    parentKey : vec3<i32>,
    cell : vec3<u32>, context : u32,
    surfaceData : u32, auxContext : u32,
    columnCache : vec4<u32>) -> SparseVoxelExactMicroCellBounds {
    let subdivision = material.geometry & 0x0fu;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    let inverseSubdivision = 1.0 / f32(max(subdivision, 1u));
    var minimum = vec3<f32>(cell) * inverseSubdivision;
    var maximum = vec3<f32>(cell + vec3<u32>(1u)) * inverseSubdivision;
#ifdef VOXEL_GEOMETRY_IR_ENABLED
    if ((geometryExecutionFlags &
         VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u) {
        return SparseVoxelExactMicroCellBounds(
            minimum, maximum, voxelGeometryFieldCacheOccupied(
                parentKey, descriptorId, cell, subdivision));
    }
    if ((geometryExecutionFlags &
         (VOXEL_GEOMETRY_PROGRAM_ENABLED | 2u)) ==
            (VOXEL_GEOMETRY_PROGRAM_ENABLED | 2u)) {
        let evaluated = voxelGeometryEvaluate(
            descriptorId, parentKey, vec2<u32>(cell.xy), cell, 6u,
            subdivision, material.geometry >> 24u, context,
            VOXEL_GEOMETRY_FIXED_ONE, auxContext,
            voxelGeometryDefaultMaterialFields(descriptorId));
        return SparseVoxelExactMicroCellBounds(
            minimum, maximum, evaluated.coverage > 0);
    }
#endif
    if (profile == SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY) {
        let maximumDepthFixed = (material.geometry >> 4u) & 0xffu;
        let seed = material.geometry >> 24u;
        let densityFixed = sparseVoxelExactMicroVolumeHash(
            seed, parentKey, cell, subdivision, profile) %
            (subdivision * 16u);
        return SparseVoxelExactMicroCellBounds(
            minimum, maximum, densityFixed >= maximumDepthFixed);
    }
    for (var face = 0u; face < 6u; face += 1u) {
        if ((faceMask & (1u << face)) == 0u) {
            continue;
        }
        let frame = sparseVoxelExactMicroFaceFrame(face);
        let orientedU = select(
            cell[frame.y], subdivision - 1u - cell[frame.y],
            (frame.w & 1u) != 0u);
        let orientedV = select(
            cell[frame.z], subdivision - 1u - cell[frame.z],
            (frame.w & 2u) != 0u);
        let carveDepth = sparseVoxelExactMicroCarveDepth(
            material, descriptorId, geometryExecutionFlags, parentKey, face,
            vec2<u32>(orientedU, orientedV),
            context, surfaceData, auxContext, columnCache);
        let carveBoundary = carveDepth * inverseSubdivision;
        if ((face & 1u) != 0u) {
            maximum[frame.x] = min(
                maximum[frame.x], 1.0 - carveBoundary);
        } else {
            minimum[frame.x] = max(minimum[frame.x], carveBoundary);
        }
        if (minimum[frame.x] >= maximum[frame.x] - 1.0e-6) {
            return SparseVoxelExactMicroCellBounds(
                minimum, maximum, false);
        }
    }
    return SparseVoxelExactMicroCellBounds(minimum, maximum, true);
}

fn sparseVoxelExactMicroIntersectColumn(
    origin : vec3<f32>, direction : vec3<f32>, inverseDirection : vec3<f32>,
    minimum : vec3<f32>, maximum : vec3<f32>, minimumDistance : f32,
    maximumDistance : f32, descriptorId : u32, subdivision : u32,
    steps : u32) -> SparseVoxelExactMicroHit {
    let slabA = (minimum - origin) * inverseDirection;
    let slabB = (maximum - origin) * inverseDirection;
    let nearT = min(slabA, slabB);
    let farT = max(slabA, slabB);
    let entry = max(minimumDistance, max(nearT.x, max(nearT.y, nearT.z)));
    let exit = min(maximumDistance, min(farT.x, min(farT.y, farT.z)));
    if (entry > exit + 1.0e-6) {
        return sparseVoxelExactMicroMiss(maximumDistance, descriptorId, steps);
    }
    var axis = 0u;
    if (nearT.y > nearT.x) {
        axis = 1u;
    }
    if (nearT.z > max(nearT.x, nearT.y)) {
        axis = 2u;
    }
    let face = axis | select(0u, 4u, direction[axis] < 0.0);
    return SparseVoxelExactMicroHit(
        entry, sparseVoxelExactMicroCellAt(
            origin, direction, entry, subdivision),
        face, descriptorId, steps, true);
}

// Interior Staircase parents are exactly solid. Preserve the first iteration
// of the former volume traversal without evaluating five zero-depth carve
// faces or entering the DDA loop. The same bounds and intersection helper
// preserve face ties, cell ownership, payload, and temporal identity.
fn traceSparseVoxelExactMicroSolidStaircaseInterior(
    descriptorId : u32, subdivision : u32,
    origin : vec3<f32>, direction : vec3<f32>,
    minimumDistance : f32,
    maximumDistance : f32) -> SparseVoxelExactMicroHit {
    let cell = sparseVoxelExactMicroCellAt(
        origin, direction, minimumDistance, subdivision);
    let inverseSubdivision = 1.0 / f32(subdivision);
    let minimum = vec3<f32>(cell) * inverseSubdivision;
    let maximum = vec3<f32>(cell + vec3<u32>(1u)) * inverseSubdivision;

    var next = vec3<f32>(1.0e30);
    let step = vec3<i32>(sign(direction));
    for (var axis = 0u; axis < 3u; axis += 1u) {
        if (step[axis] != 0) {
            let boundaryCell = select(
                i32(cell[axis]), i32(cell[axis]) + 1,
                step[axis] > 0);
            let boundary = f32(boundaryCell) / f32(subdivision);
            next[axis] = (boundary - origin[axis]) / direction[axis];
        }
    }
    let segmentExit = min(
        maximumDistance, min(next.x, min(next.y, next.z)));

    let parallel = abs(direction) < vec3<f32>(1.0e-20);
    let parallelDirection = select(
        vec3<f32>(-1.0e-20), vec3<f32>(1.0e-20),
        direction >= vec3<f32>(0.0));
    let inverseDirection = vec3<f32>(1.0) /
        select(direction, parallelDirection, parallel);
    return sparseVoxelExactMicroIntersectColumn(
        origin, direction, inverseDirection, minimum, maximum,
        minimumDistance, segmentExit, descriptorId, subdivision, 1u);
}

fn traceSparseVoxelExactMicroOneFace(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32,
    parentKey : vec3<i32>, origin : vec3<f32>, direction : vec3<f32>,
    minimumDistance : f32, maximumDistance : f32,
    fallbackFace : u32, context : u32,
    surfaceData : u32, auxContext : u32,
    columnCache : vec4<u32>) -> SparseVoxelExactMicroHit {
    let subdivision = material.geometry & 0x0fu;
    let maximumDepthFixed = (material.geometry >> 4u) & 0xffu;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    let staircaseSurface =
        sparseVoxelExactMicroIsTerrainStaircaseSurface(profile, context);
    if (subdivision < 2u ||
        subdivision > SPARSE_VOXEL_EXACT_MICRO_MAX_SUBDIVISION ||
        maximumDepthFixed == 0u ||
        maximumDepthFixed > subdivision * 16u ||
        (countOneBits(faceMask) != 1u && !staircaseSurface) ||
        (profile != SPARSE_VOXEL_EXACT_MICRO_CONSTANT &&
         profile != SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS &&
         profile != SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS &&
         profile != SPARSE_VOXEL_EXACT_MICRO_STAIRCASE)) {
        // Invalid data falls back conservatively to the solid parent.
        return SparseVoxelExactMicroHit(
            minimumDistance, sparseVoxelExactMicroCellAt(
                origin, direction, minimumDistance, max(subdivision, 1u)),
            fallbackFace, descriptorId, 0u, true);
    }

    var carveFace = 3u;
    if (!staircaseSurface) {
        for (var candidate = 0u; candidate < 6u; candidate += 1u) {
            if ((faceMask & (1u << candidate)) != 0u) {
                carveFace = candidate;
                break;
            }
        }
    }
    let frame = sparseVoxelExactMicroFaceFrame(carveFace);
    let axis = frame.x;
    let uAxis = frame.y;
    let vAxis = frame.z;
    let invertU = (frame.w & 1u) != 0u;
    let invertV = (frame.w & 2u) != 0u;
    let entryPoint = origin + direction * minimumDistance;
    let u = select(entryPoint[uAxis], 1.0 - entryPoint[uAxis], invertU);
    let v = select(entryPoint[vAxis], 1.0 - entryPoint[vAxis], invertV);
    let du = select(direction[uAxis], -direction[uAxis], invertU);
    let dv = select(direction[vAxis], -direction[vAxis], invertV);
    var uCell = i32(sparseVoxelExactMicroCellCoordinate(u, du, subdivision));
    var vCell = i32(sparseVoxelExactMicroCellCoordinate(v, dv, subdivision));
    let uStep = select(select(0, -1, du < 0.0), 1, du > 0.0);
    let vStep = select(select(0, -1, dv < 0.0), 1, dv > 0.0);
    var nextU = 1.0e30;
    if (uStep != 0) {
        let boundary = f32(select(uCell, uCell + 1, uStep > 0)) /
            f32(subdivision);
        nextU = minimumDistance + (boundary - u) / du;
    }
    var nextV = 1.0e30;
    if (vStep != 0) {
        let boundary = f32(select(vCell, vCell + 1, vStep > 0)) /
            f32(subdivision);
        nextV = minimumDistance + (boundary - v) / dv;
    }
    let deltaU = select(
        1.0e30, 1.0 / (abs(du) * f32(subdivision)), uStep != 0);
    let deltaV = select(
        1.0e30, 1.0 / (abs(dv) * f32(subdivision)), vStep != 0);
    let parallel = abs(direction) < vec3<f32>(1.0e-20);
    let parallelDirection = select(
        vec3<f32>(-1.0e-20), vec3<f32>(1.0e-20),
        direction >= vec3<f32>(0.0));
    let inverseDirection = vec3<f32>(1.0) /
        select(direction, parallelDirection, parallel);
    var current = minimumDistance;
    var walkedSteps = 0u;

    for (var stepIndex = 0u; stepIndex < 18u; stepIndex += 1u) {
        if (stepIndex >= 2u * subdivision + 2u || uCell < 0 || vCell < 0 ||
            uCell >= i32(subdivision) || vCell >= i32(subdivision)) {
            break;
        }
        let steps = stepIndex + 1u;
        walkedSteps = steps;
        let carveDepth = sparseVoxelExactMicroCarveDepth(
            material, descriptorId, geometryExecutionFlags, parentKey,
            carveFace,
            vec2<u32>(u32(uCell), u32(vCell)), context, surfaceData,
            auxContext, columnCache);
        let columnExit = min(maximumDistance, min(nextU, nextV));
        if (carveDepth < f32(subdivision)) {
            var columnMinimum = vec3<f32>(0.0);
            var columnMaximum = vec3<f32>(1.0);
            let orientedUMinimum = f32(uCell) / f32(subdivision);
            let orientedUMaximum = f32(uCell + 1) / f32(subdivision);
            let orientedVMinimum = f32(vCell) / f32(subdivision);
            let orientedVMaximum = f32(vCell + 1) / f32(subdivision);
            columnMinimum[uAxis] = select(
                orientedUMinimum, 1.0 - orientedUMaximum, invertU);
            columnMaximum[uAxis] = select(
                orientedUMaximum, 1.0 - orientedUMinimum, invertU);
            columnMinimum[vAxis] = select(
                orientedVMinimum, 1.0 - orientedVMaximum, invertV);
            columnMaximum[vAxis] = select(
                orientedVMaximum, 1.0 - orientedVMinimum, invertV);
            if ((carveFace & 1u) != 0u) {
                columnMaximum[axis] =
                    (f32(subdivision) - carveDepth) / f32(subdivision);
            } else {
                columnMinimum[axis] = f32(carveDepth) / f32(subdivision);
            }
            let hit = sparseVoxelExactMicroIntersectColumn(
                origin, direction, inverseDirection, columnMinimum,
                columnMaximum, current, columnExit, descriptorId,
                subdivision, steps);
            if (hit.valid) {
                return hit;
            }
        }
        if (columnExit >= maximumDistance - 1.0e-6) {
            return sparseVoxelExactMicroMiss(
                maximumDistance, descriptorId, steps);
        }
        current = columnExit;
        if (nextU <= nextV) {
            uCell += uStep;
            nextU += deltaU;
        } else {
            vCell += vStep;
            nextV += deltaV;
        }
    }
    return sparseVoxelExactMicroMiss(
        maximumDistance, descriptorId, walkedSteps);
}

// General subtractive microvolume traversal. Every enabled descriptor face
// removes cells inward from its side; corners therefore have one unambiguous
// occupancy result. A ray can cross at most 3*N cell boundaries, with three
// extra iterations retained for boundary ownership and a start-on-face cell.
fn traceSparseVoxelExactMicroVolume(
    material : SparseVoxelMicroMaterial, descriptorId : u32,
    geometryExecutionFlags : u32,
    parentKey : vec3<i32>, origin : vec3<f32>, direction : vec3<f32>,
    minimumDistance : f32, maximumDistance : f32,
    fallbackFace : u32, context : u32,
    surfaceData : u32, auxContext : u32) -> SparseVoxelExactMicroHit {
    let subdivision = material.geometry & 0x0fu;
    let maximumDepthFixed = (material.geometry >> 4u) & 0xffu;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    if (subdivision < 2u ||
        subdivision > SPARSE_VOXEL_EXACT_MICRO_MAX_SUBDIVISION ||
        maximumDepthFixed == 0u ||
        maximumDepthFixed > subdivision * 16u || faceMask == 0u ||
        (profile != SPARSE_VOXEL_EXACT_MICRO_CONSTANT &&
         profile != SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS &&
         profile != SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY &&
         profile != SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS &&
         profile != SPARSE_VOXEL_EXACT_MICRO_STAIRCASE)) {
        return SparseVoxelExactMicroHit(
            minimumDistance, sparseVoxelExactMicroCellAt(
                origin, direction, minimumDistance, max(subdivision, 1u)),
            fallbackFace, descriptorId, 0u, true);
    }

    var cell = sparseVoxelExactMicroCellAt(
        origin, direction, minimumDistance, subdivision);
    let step = vec3<i32>(sign(direction));
    var next = vec3<f32>(1.0e30);
    var delta = vec3<f32>(1.0e30);
    for (var axis = 0u; axis < 3u; axis += 1u) {
        if (step[axis] == 0) {
            continue;
        }
        let boundaryCell = select(
            i32(cell[axis]), i32(cell[axis]) + 1, step[axis] > 0);
        let boundary = f32(boundaryCell) / f32(subdivision);
        next[axis] = (boundary - origin[axis]) / direction[axis];
        delta[axis] = 1.0 / (abs(direction[axis]) * f32(subdivision));
    }

    var current = minimumDistance;
    var walkedSteps = 0u;
    let parallel = abs(direction) < vec3<f32>(1.0e-20);
    let parallelDirection = select(
        vec3<f32>(-1.0e-20), vec3<f32>(1.0e-20),
        direction >= vec3<f32>(0.0));
    let inverseDirection = vec3<f32>(1.0) /
        select(direction, parallelDirection, parallel);
    for (var stepIndex = 0u; stepIndex < 27u; stepIndex += 1u) {
        if (stepIndex >= 3u * subdivision + 3u) {
            break;
        }
        walkedSteps = stepIndex + 1u;
        let bounds = sparseVoxelExactMicroCellBounds(
            material, descriptorId, geometryExecutionFlags, parentKey, cell,
            context, surfaceData, auxContext, vec4<u32>(0u));
        if (bounds.occupied) {
            let segmentExit = min(
                maximumDistance, min(next.x, min(next.y, next.z)));
            let hit = sparseVoxelExactMicroIntersectColumn(
                origin, direction, inverseDirection,
                bounds.minimum, bounds.maximum, current, segmentExit,
                descriptorId, subdivision, walkedSteps);
            if (hit.valid) {
                return hit;
            }
        }

        var axis = 0u;
        if (next.y < next.x) {
            axis = 1u;
        }
        if (next.z < next[axis]) {
            axis = 2u;
        }
        current = next[axis];
        if (current > maximumDistance + 1.0e-6) {
            break;
        }
        let nextCell = i32(cell[axis]) + step[axis];
        if (nextCell < 0 || nextCell >= i32(subdivision)) {
            break;
        }
        cell[axis] = u32(nextCell);
        next[axis] += delta[axis];
    }
    return sparseVoxelExactMicroMiss(
        maximumDistance, descriptorId, walkedSteps);
}
