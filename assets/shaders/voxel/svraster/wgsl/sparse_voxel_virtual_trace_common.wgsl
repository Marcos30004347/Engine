struct VoxelTracePage {
    pageId : u32,
    generation : u32,
    geometryRevisionLo : u32,
    geometryRevisionHi : u32,

    format : u32,
    rootNode : u32,
    traceNodeOffset : u32,
    traceNodeCount : u32,

    brickOffset : u32,
    brickCount : u32,
    materialOffset : u32,
    materialCount : u32,

    localOriginX : f32,
    localOriginY : f32,
    localOriginZ : f32,
    rootSide : f32,

    errorMin : f32,
    errorMax : f32,
    errorSidecarOffset : u32,
    flags : u32,
}

struct VoxelTraceNode64 {
    childBaseAndFlags : u32,
    occupancyMaskLo : u32,
    occupancyMaskHi : u32,
    payloadOrSidecar : u32,
}

struct VoxelTraceErrorSidecar {
    errorMin : f32,
    errorMax : f32,
    childErrorWordOffset : u32,
    childErrorWordCount : u32,
}

struct VoxelTraceBounds8Node {
    origin : vec3<f32>,
    scaleExponentsAndChildCount : u32,

    childBase : u32,
    payloadBase : u32,
    childMask : u32,
    internalMask : u32,

    qLoX : array<u32, 2>,
    qLoY : array<u32, 2>,
    qLoZ : array<u32, 2>,
    qHiX : array<u32, 2>,
    qHiY : array<u32, 2>,
    qHiZ : array<u32, 2>,
}

struct VoxelTraceBounds16Node {
    origin : vec3<f32>,
    scaleExponentsAndChildCount : u32,

    childBase : u32,
    payloadBase : u32,
    childMask : u32,
    internalMask : u32,

    qLoX : array<u32, 4>,
    qLoY : array<u32, 4>,
    qLoZ : array<u32, 4>,
    qHiX : array<u32, 4>,
    qHiY : array<u32, 4>,
    qHiZ : array<u32, 4>,
}

struct VoxelTraceRay {
    origin : vec3<f32>,
    tMin : f32,
    direction : vec3<f32>,
    tMax : f32,
}

struct VoxelTraceAabbHit {
    hit : u32,
    nearT : f32,
    farT : f32,
    _pad : u32,
}

const VOXEL_TRACE_BRANCHING = 4u;
const VOXEL_TRACE_CHILD_COUNT = 64u;
const VOXEL_TRACE_NODE64_TERMINAL_FLAG = 1u;

const VOXEL_TRACE_FORMAT_INVALID = 0u;
const VOXEL_TRACE_FORMAT_CUBIC64 = 1u;
const VOXEL_TRACE_FORMAT_BOUNDS8 = 2u;
const VOXEL_TRACE_FORMAT_BOUNDS16 = 3u;

const VOXEL_TRACE_PAGE_VALID = 1u;
const VOXEL_TRACE_PAGE_CONSERVATIVE_FALLBACK = 2u;
const VOXEL_TRACE_INVALID_OFFSET = 0xffffffffu;

fn voxelTraceFormatCanBeImplicitFallbackFor(
    candidateFormat : u32,
    requestedFormat : u32,
    pageFlags : u32) -> bool {
    if (candidateFormat == requestedFormat) {
        return true;
    }
    let conservative = (pageFlags & VOXEL_TRACE_PAGE_CONSERVATIVE_FALLBACK) != 0u;
    if (!conservative) {
        return false;
    }
    if (requestedFormat == VOXEL_TRACE_FORMAT_CUBIC64) {
        return candidateFormat == VOXEL_TRACE_FORMAT_BOUNDS8 ||
            candidateFormat == VOXEL_TRACE_FORMAT_BOUNDS16;
    }
    return false;
}

fn voxelTraceNode64ChildIndex(x : u32, y : u32, z : u32) -> u32 {
    return x + VOXEL_TRACE_BRANCHING * y +
        VOXEL_TRACE_BRANCHING * VOXEL_TRACE_BRANCHING * z;
}

fn voxelTraceNode64ChildCoord(child : u32) -> vec3<u32> {
    return vec3<u32>(child & 3u, (child >> 2u) & 3u, (child >> 4u) & 3u);
}

fn voxelTraceNode64ChildOctant(child : u32) -> u32 {
    let coord = voxelTraceNode64ChildCoord(child);
    return (coord.x >> 1u) | ((coord.y >> 1u) << 1u) | ((coord.z >> 1u) << 2u);
}

fn voxelTraceNode64OctantMaskLo(octant : u32) -> u32 {
    var mask = 0u;
    let baseX = select(0u, 2u, (octant & 1u) != 0u);
    let baseY = select(0u, 2u, (octant & 2u) != 0u);
    let baseZ = select(0u, 2u, (octant & 4u) != 0u);
    for (var z = 0u; z < 2u; z = z + 1u) {
        for (var y = 0u; y < 2u; y = y + 1u) {
            for (var x = 0u; x < 2u; x = x + 1u) {
                let child = voxelTraceNode64ChildIndex(baseX + x, baseY + y, baseZ + z);
                if (child < 32u) {
                    mask = mask | (1u << child);
                }
            }
        }
    }
    return mask;
}

fn voxelTraceNode64OctantMaskHi(octant : u32) -> u32 {
    var mask = 0u;
    let baseX = select(0u, 2u, (octant & 1u) != 0u);
    let baseY = select(0u, 2u, (octant & 2u) != 0u);
    let baseZ = select(0u, 2u, (octant & 4u) != 0u);
    for (var z = 0u; z < 2u; z = z + 1u) {
        for (var y = 0u; y < 2u; y = y + 1u) {
            for (var x = 0u; x < 2u; x = x + 1u) {
                let child = voxelTraceNode64ChildIndex(baseX + x, baseY + y, baseZ + z);
                if (child >= 32u) {
                    mask = mask | (1u << (child - 32u));
                }
            }
        }
    }
    return mask;
}

fn voxelTraceNode64ChildOrigin(
    nodeOrigin : vec3<f32>,
    nodeSide : f32,
    child : u32) -> vec3<f32> {
    let coord = vec3<f32>(voxelTraceNode64ChildCoord(child));
    return nodeOrigin + coord * (nodeSide / f32(VOXEL_TRACE_BRANCHING));
}

fn voxelTraceNode64OctantOrigin(
    nodeOrigin : vec3<f32>,
    nodeSide : f32,
    octant : u32) -> vec3<f32> {
    let octantSide = nodeSide * 0.5;
    return nodeOrigin + vec3<f32>(
        select(0.0, octantSide, (octant & 1u) != 0u),
        select(0.0, octantSide, (octant & 2u) != 0u),
        select(0.0, octantSide, (octant & 4u) != 0u));
}

fn voxelTraceRayAabb(
    ray : VoxelTraceRay,
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> VoxelTraceAabbHit {
    var nearT = ray.tMin;
    var farT = ray.tMax;
    for (var axis = 0u; axis < 3u; axis = axis + 1u) {
        let origin = ray.origin[axis];
        let direction = ray.direction[axis];
        let minValue = minimum[axis];
        let maxValue = maximum[axis];
        if (abs(direction) <= 0.00000001) {
            if (origin < minValue || origin > maxValue) {
                return VoxelTraceAabbHit(0u, nearT, farT, 0u);
            }
        } else {
            let invDirection = 1.0 / direction;
            let a = (minValue - origin) * invDirection;
            let b = (maxValue - origin) * invDirection;
            nearT = max(nearT, min(a, b));
            farT = min(farT, max(a, b));
            if (nearT > farT) {
                return VoxelTraceAabbHit(0u, nearT, farT, 0u);
            }
        }
    }
    return VoxelTraceAabbHit(1u, nearT, farT, 0u);
}

fn voxelTraceDistanceToAabb(
    point : vec3<f32>,
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> f32 {
    let below = max(minimum - point, vec3<f32>(0.0));
    let above = max(point - maximum, vec3<f32>(0.0));
    let delta = below + above;
    return length(delta);
}

fn voxelTraceProjectedErrorDistanceThreshold(
    cameraPosition : vec3<f32>,
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    projectedErrorThreshold : f32,
    minimumCameraDistance : f32) -> f32 {
    if (!(projectedErrorThreshold > 0.0)) {
        return 0.0;
    }
    let distance = max(
        voxelTraceDistanceToAabb(cameraPosition, minimum, maximum),
        max(minimumCameraDistance, 0.000001));
    return projectedErrorThreshold * distance;
}

fn voxelTraceNode64PopulationContains(
    node : VoxelTraceNode64,
    child : u32) -> bool {
    if (child >= VOXEL_TRACE_CHILD_COUNT) {
        return false;
    }
    if (child < 32u) {
        return (node.occupancyMaskLo & (1u << child)) != 0u;
    }
    return (node.occupancyMaskHi & (1u << (child - 32u))) != 0u;
}

fn voxelTraceNode64PopulationCountBefore(
    node : VoxelTraceNode64,
    child : u32) -> u32 {
    if (child == 0u) {
        return 0u;
    }
    if (child < 32u) {
        return countOneBits(node.occupancyMaskLo & ((1u << child) - 1u));
    }
    let highChild = min(child - 32u, 32u);
    let highMask = select(
        0u,
        (1u << highChild) - 1u,
        highChild != 0u && highChild < 32u);
    let saturatedHighMask = select(highMask, 0xffffffffu, highChild >= 32u);
    return countOneBits(node.occupancyMaskLo) +
        countOneBits(node.occupancyMaskHi & saturatedHighMask);
}

fn voxelTraceNode64Terminal(node : VoxelTraceNode64) -> bool {
    return (node.childBaseAndFlags & VOXEL_TRACE_NODE64_TERMINAL_FLAG) != 0u;
}

fn voxelTraceNode64ChildBase(node : VoxelTraceNode64) -> u32 {
    return node.childBaseAndFlags >> 1u;
}

fn voxelTracePackQ8Error4(x : u32, y : u32, z : u32, w : u32) -> u32 {
    return (x & 0xffu) |
        ((y & 0xffu) << 8u) |
        ((z & 0xffu) << 16u) |
        ((w & 0xffu) << 24u);
}

fn voxelTraceUnpackQ8Error(packed : u32, lane : u32) -> u32 {
    return (packed >> ((lane & 3u) * 8u)) & 0xffu;
}

fn voxelTraceDecodeSignedU8(value : u32) -> i32 {
    let byteValue = value & 0xffu;
    return select(i32(byteValue), i32(byteValue) - 256, byteValue >= 128u);
}

fn voxelTraceEncodeSignedExponent8(extent : f32) -> u32 {
    if (!(extent > 0.0)) {
        return 0u;
    }
    var exponent = clamp(i32(ceil(log2(extent / 255.0))), -128, 127);
    let scale = exp2(f32(exponent));
    if (exponent < 127 && ceil(extent / scale) > 255.0) {
        exponent += 1;
    }
    return select(u32(exponent), u32(exponent + 256), exponent < 0) & 0xffu;
}

fn voxelTraceDecodeExponentScale(encodedExponent : u32) -> f32 {
    return exp2(f32(voxelTraceDecodeSignedU8(encodedExponent & 0xffu)));
}

fn voxelTraceBoundsQuantizeLower(value : f32, parentMin : f32, scale : f32) -> u32 {
    if (!(scale > 0.0)) {
        return 0u;
    }
    var q = u32(clamp(floor((value - parentMin) / scale), 0.0, 255.0));
    if (q > 0u) {
        q -= 1u;
    }
    return q;
}

fn voxelTraceBoundsQuantizeUpper(value : f32, parentMin : f32, scale : f32) -> u32 {
    if (!(scale > 0.0)) {
        return 0u;
    }
    var q = u32(clamp(ceil((value - parentMin) / scale), 0.0, 255.0));
    if (q < 255u) {
        q += 1u;
    }
    return q;
}

fn voxelTraceBoundsPackScaleExponents(x : u32, y : u32, z : u32, childCount : u32) -> u32 {
    return voxelTracePackQ8Error4(x, y, z, childCount);
}

fn voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount : u32, axis : u32) -> u32 {
    return voxelTraceUnpackQ8Error(scaleExponentsAndChildCount, axis);
}

fn voxelTraceBoundsChildCount(scaleExponentsAndChildCount : u32) -> u32 {
    return voxelTraceUnpackQ8Error(scaleExponentsAndChildCount, 3u);
}

fn voxelTraceBoundsDecodeChildMinimum(
    origin : vec3<f32>,
    scaleExponentsAndChildCount : u32,
    qLoX : u32,
    qLoY : u32,
    qLoZ : u32,
    slot : u32) -> vec3<f32> {
    let scale = vec3<f32>(
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 0u)),
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 1u)),
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 2u)));
    return origin + vec3<f32>(
        f32(voxelTraceUnpackQ8Error(qLoX, slot & 3u)) * scale.x,
        f32(voxelTraceUnpackQ8Error(qLoY, slot & 3u)) * scale.y,
        f32(voxelTraceUnpackQ8Error(qLoZ, slot & 3u)) * scale.z);
}

fn voxelTraceBoundsDecodeChildMaximum(
    origin : vec3<f32>,
    scaleExponentsAndChildCount : u32,
    qHiX : u32,
    qHiY : u32,
    qHiZ : u32,
    slot : u32) -> vec3<f32> {
    let scale = vec3<f32>(
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 0u)),
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 1u)),
        voxelTraceDecodeExponentScale(voxelTraceBoundsScaleExponent(scaleExponentsAndChildCount, 2u)));
    return origin + vec3<f32>(
        f32(voxelTraceUnpackQ8Error(qHiX, slot & 3u)) * scale.x,
        f32(voxelTraceUnpackQ8Error(qHiY, slot & 3u)) * scale.y,
        f32(voxelTraceUnpackQ8Error(qHiZ, slot & 3u)) * scale.z);
}

fn voxelTraceEncodeQ8Error(
    error : f32,
    errorMin : f32,
    errorMax : f32) -> u32 {
    let range = errorMax - errorMin;
    if (!(range > 0.0)) {
        return 0u;
    }
    let normalized = clamp((error - errorMin) / range, 0.0, 1.0);
    return u32(round(normalized * 255.0));
}

fn voxelTraceDecodeQ8Error(
    encoded : u32,
    errorMin : f32,
    errorMax : f32) -> f32 {
    let range = errorMax - errorMin;
    if (!(range > 0.0)) {
        return errorMin;
    }
    return errorMin + (f32(encoded & 0xffu) / 255.0) * range;
}

const VOXEL_TRACE_CULL_EXACT_FALLBACK = 0u;
const VOXEL_TRACE_CULL_DEFINITELY_SKIP = 1u;
const VOXEL_TRACE_CULL_MUST_EXPAND = 2u;

fn voxelTraceCullExpansionDecisionQ8(
    encodedError : u32,
    errorMin : f32,
    errorMax : f32,
    threshold : f32) -> u32 {
    let range = errorMax - errorMin;
    if (!(range > 0.0)) {
        return select(
            VOXEL_TRACE_CULL_DEFINITELY_SKIP,
            VOXEL_TRACE_CULL_MUST_EXPAND,
            errorMin > threshold);
    }
    let decoded = voxelTraceDecodeQ8Error(encodedError, errorMin, errorMax);
    let halfStep = range / 510.0;
    let possibleMin = select(max(errorMin, decoded - halfStep), errorMin, encodedError == 0u);
    let possibleMax = select(min(errorMax, decoded + halfStep), errorMax, encodedError == 255u);
    if (possibleMax <= threshold) {
        return VOXEL_TRACE_CULL_DEFINITELY_SKIP;
    }
    if (possibleMin > threshold) {
        return VOXEL_TRACE_CULL_MUST_EXPAND;
    }
    return VOXEL_TRACE_CULL_EXACT_FALLBACK;
}
