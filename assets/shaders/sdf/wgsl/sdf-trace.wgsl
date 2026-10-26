#ifdef SDF_TRACE_DEBUG
struct TraceUniforms {
    inverseViewProjection : mat4x4<f32>,
    cameraPosition : vec3<f32>,
    maxTraceDistance : f32,
    outputWidth : u32,
    outputHeight : u32,
    mode : u32,
    maxSteps : u32,
    hitThresholdScale : f32,
    stepScale : f32,
    minStepScale : f32,
    reverseZ : u32,
    enabled : u32,
    _padding0x : u32,
    _padding0y : u32,
    _padding0z : u32,
    traceHeatmapLowColor : vec4<f32>,
    traceHeatmapMidColor : vec4<f32>,
    traceHeatmapHighColor : vec4<f32>,
    traceHeatmapCounts : vec4<f32>,
}
#else
struct TraceUniforms {
    rayCount : u32,
    rayOffset : u32,
    hitOffset : u32,
    maxSteps : u32,
    maxTraceDistance : f32,
    hitThresholdScale : f32,
    stepScale : f32,
    minStepScale : f32,
}
#endif

struct SDFSceneInfo {
    worldBoundsMinAndMaxDepth : vec4<f32>,
    worldBoundsMaxAndRootNode : vec4<f32>,
    counts : vec4<u32>,
    settings : vec4<u32>,
}

#ifndef SDF_TRACE_DEBUG
struct TraceRay {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
}

struct TraceHit {
    worldPositionAndDistance : vec4<f32>,
    worldNormalAndStepCount : vec4<f32>,
    localUVWAndAux : vec4<f32>,
    metadata : vec4<u32>,
    traceInfo : vec4<u32>,
    surfaceMaterialReference : vec4<u32>,
}
#endif

struct SDFInstance {
    localToWorld : mat4x4<f32>,
    worldToLocal : mat4x4<f32>,
    worldBoundsMin : vec4<f32>,
    worldBoundsMaxAndConservativeScale : vec4<f32>,
    idsAndBlas : vec4<u32>,
    materialAndFlags : vec4<u32>,
}

struct SDFBlas {
    localBoundsMin : vec4<f32>,
    localBoundsMaxAndRootVoxelSize : vec4<f32>,
    offsetsAndFormat : vec4<u32>,
    countsAndNarrowBand : vec4<u32>,
}

struct SDFNode {
    brickIndex : u32,
    firstChild : u32,
    childMaskWords : array<u32, 16>,
    validMaskWords : array<u32, 16>,
    traceMaskWords : array<u32, 16>,
    levelFlags : u32,
    padding0 : u32,
}

struct SDFBrick {
    distanceOffset : u32,
    minMaxDistancePacked : u32,
    voxelSize : f32,
    maxError : f32,
}

struct TracePage {
    maskOffset : u32,
    brickOffset : u32,
    childCount : u32,
    padding0 : u32,
}

struct SDFTlasNode {
    boundsMin : vec4<f32>,
    boundsMax : vec4<f32>,
    childAndRefs : vec4<u32>,
    depthFlags : vec4<u32>,
    childIndices0 : vec4<u32>,
    childIndices1 : vec4<u32>,
}

struct SDFSceneGridInfo {
    worldBoundsMin : vec4<f32>,
    worldBoundsMaxAndResolution : vec4<f32>,
    counts : vec4<u32>,
    settings : vec4<u32>,
}

struct SDFSceneGridCell {
    offsetCountAndFlags : vec4<u32>,
}

struct SDFSceneGridRef {
    boundsMin : vec4<f32>,
    boundsMax : vec4<f32>,
    kindInstanceAndPage : vec4<u32>,
}

struct SDFSample {
    distance : f32,
    nodeIndex : u32,
    brickIndex : u32,
    level : u32,
    voxelSize : f32,
    validCell : u32,
    traceCell : u32,
    cellBoundsMin : vec3<f32>,
    cellBoundsMax : vec3<f32>,
    nodeBoundsMin : vec3<f32>,
    nodeBoundsMax : vec3<f32>,
}

struct SDFCandidateHit {
    hit : bool,
    t : f32,
    instanceIndex : u32,
    instanceId : u32,
    blasIndex : u32,
    nodeIndex : u32,
    brickIndex : u32,
    level : u32,
    steps : u32,
    tlasVisits : u32,
    instanceTests : u32,
    localPosition : vec3<f32>,
    worldPosition : vec3<f32>,
    worldNormal : vec3<f32>,
}

struct SDFTraversalItem {
    nodeIndex : u32,
    tMin : f32,
    tMax : f32,
    boundsMin : vec3<f32>,
    boundsMax : vec3<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : TraceUniforms;
@group(0) @binding(1) var<uniform> sceneInfo : SDFSceneInfo;
#ifndef SDF_TRACE_DEBUG
@group(0) @binding(2) var<storage, read> rays : array<TraceRay>;
@group(0) @binding(3) var<storage, read_write> hits : array<TraceHit>;
#endif
@group(0) @binding(4) var<storage, read> instances : array<SDFInstance>;
@group(0) @binding(5) var<storage, read> blases : array<SDFBlas>;
@group(0) @binding(6) var<storage, read> blasNodes : array<SDFNode>;
@group(0) @binding(7) var<storage, read> blasBricks : array<SDFBrick>;
@group(0) @binding(8) var<storage, read> distanceWords : array<u32>;
@group(0) @binding(9) var<storage, read> tlasNodes : array<SDFTlasNode>;
@group(0) @binding(10) var<storage, read> tlasInstanceRefs : array<u32>;
#ifdef SDF_TRACE_DEBUG
@group(0) @binding(11) var outputTexture : texture_storage_2d<rgba16float, write>;
#endif
@group(0) @binding(12) var distanceAtlasTexture : texture_3d<f32>;
@group(0) @binding(13) var distanceAtlasSampler : sampler;
@group(0) @binding(14) var<storage, read> blasNodeChildWordPrefixes : array<u32>;
@group(0) @binding(17) var<storage, read> tracePageTables : array<u32>;
@group(0) @binding(18) var<storage, read> tracePages : array<TracePage>;
@group(0) @binding(19) var<storage, read> tracePageMasks : array<u32>;
@group(0) @binding(20) var<storage, read> traceBrickMaps : array<u32>;
@group(0) @binding(21) var<storage, read> sceneGridInfoBuffer : array<SDFSceneGridInfo>;
@group(0) @binding(22) var<storage, read> sceneGridCells : array<SDFSceneGridCell>;
@group(0) @binding(23) var<storage, read> sceneGridRefs : array<SDFSceneGridRef>;
@group(0) @binding(24) var<storage, read> sceneGridGlobalRefs : array<SDFSceneGridRef>;

#ifndef SDF_TRACE_DEBUG
const TRACE_RAY_FLAG_ACTIVE : u32 = 1u;
const TRACE_HIT_FLAG_HIT : u32 = 1u;
#endif
const INVALID_INDEX : u32 = 0xffffffffu;
const BRICK_RESOLUTION : u32 = 8u;
const DISTANCE_RESOLUTION : u32 = BRICK_RESOLUTION + 1u;
const DISTANCE_CELL_COUNT : u32 = DISTANCE_RESOLUTION * DISTANCE_RESOLUTION * DISTANCE_RESOLUTION;
const DISTANCE_STRIDE : u32 = ((DISTANCE_CELL_COUNT + 3u) / 4u) * 4u;
const TRACE_GRID_RESOLUTION : u32 = 64u;
const TRACE_ROOT_RESOLUTION : u32 = 8u;
const TRACE_ROOT_CELL_COUNT : u32 = TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION;
const TRACE_PAGE_RESOLUTION : u32 = 8u;
const TRACE_PAGE_CELL_COUNT : u32 = TRACE_PAGE_RESOLUTION * TRACE_PAGE_RESOLUTION * TRACE_PAGE_RESOLUTION;
const TRACE_PAGE_MASK_WORD_COUNT : u32 = TRACE_PAGE_CELL_COUNT / 32u;
const TRACE_INVALID_BRICK : u32 = 0xffffffffu;
const MAILBOX_SIZE : u32 = 32u;
const SCENE_GRID_REF_INSTANCE : u32 = 1u;
const SCENE_GRID_REF_PAGE : u32 = 2u;

fn invalid_hit(maxT: f32) -> SDFCandidateHit {
    return SDFCandidateHit(false, maxT, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX, 0u, 0u, 0u, 0u, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0, 0.0, 1.0));
}

fn ray_aabb(origin: vec3<f32>, direction: vec3<f32>, boundsMin: vec3<f32>, boundsMax: vec3<f32>, tMinIn: f32, tMaxIn: f32) -> vec2<f32> {
    var tMin = tMinIn;
    var tMax = tMaxIn;
    for (var axis = 0u; axis < 3u; axis = axis + 1u) {
        let o = origin[axis];
        let d = direction[axis];
        if (abs(d) < 1.0e-8) {
            if (o < boundsMin[axis] || o > boundsMax[axis]) {
                return vec2<f32>(1.0, 0.0);
            }
        } else {
            let invD = 1.0 / d;
            var t0 = (boundsMin[axis] - o) * invD;
            var t1 = (boundsMax[axis] - o) * invD;
            if (t0 > t1) {
                let tmp = t0;
                t0 = t1;
                t1 = tmp;
            }
            tMin = max(tMin, t0);
            tMax = min(tMax, t1);
            if (tMax < tMin) {
                return vec2<f32>(1.0, 0.0);
            }
        }
    }
    return vec2<f32>(tMin, tMax);
}

fn transform_point(m: mat4x4<f32>, p: vec3<f32>) -> vec3<f32> {
    return (m * vec4<f32>(p, 1.0)).xyz;
}

fn transform_vector(m: mat4x4<f32>, v: vec3<f32>) -> vec3<f32> {
    return (m * vec4<f32>(v, 0.0)).xyz;
}

fn popcount_before_node(nodeIndex: u32, bit: u32) -> u32 {
    var count = 0u;
    let wordIndex = bit / 32u;
    for (var i = 0u; i < wordIndex; i = i + 1u) {
        count = count + countOneBits(blasNodes[nodeIndex].childMaskWords[i]);
    }
    let localBit = bit % 32u;
    if (localBit != 0u) {
        count = count + countOneBits(blasNodes[nodeIndex].childMaskWords[wordIndex] & ((1u << localBit) - 1u));
    }
    return count;
}

fn child_ordinal_before_node(nodeIndex: u32, bit: u32) -> u32 {
    let wordIndex = bit / 32u;
    let localBit = bit % 32u;
    let prefixIndex = nodeIndex * 16u + wordIndex;
    var prefix = 0u;
    if (prefixIndex < arrayLength(&blasNodeChildWordPrefixes)) {
        prefix = blasNodeChildWordPrefixes[prefixIndex];
    } else {
        return popcount_before_node(nodeIndex, bit);
    }
    var localMask = 0u;
    if (localBit != 0u) {
        localMask = (1u << localBit) - 1u;
    }
    return prefix + countOneBits(blasNodes[nodeIndex].childMaskWords[wordIndex] & localMask);
}

fn load_distance_byte(byteOffset: u32) -> u32 {
    let wordIndex = byteOffset / 4u;
    if (wordIndex >= arrayLength(&distanceWords)) {
        return 0u;
    }
    let shift = (byteOffset & 3u) * 8u;
    return (distanceWords[wordIndex] >> shift) & 255u;
}

fn decode_snorm8(value: u32) -> f32 {
    let signedValue = select(i32(value), i32(value) - 256, value >= 128u);
    return max(-1.0, f32(signedValue) / 127.0);
}

fn brick_flat_cell(cell: vec3<u32>) -> u32 {
    return cell.x + cell.y * BRICK_RESOLUTION + cell.z * BRICK_RESOLUTION * BRICK_RESOLUTION;
}

fn brick_flat_distance_sample(sampleCoord: vec3<u32>) -> u32 {
    return sampleCoord.x + sampleCoord.y * DISTANCE_RESOLUTION + sampleCoord.z * DISTANCE_RESOLUTION * DISTANCE_RESOLUTION;
}

fn sample_brick_distance(blas: SDFBlas, brick: SDFBrick, cell: vec3<u32>) -> f32 {
    let encoded = load_distance_byte(blas.offsetsAndFormat.w + brick.distanceOffset + brick_flat_distance_sample(cell));
    return decode_snorm8(encoded) * f32(max(blas.countsAndNarrowBand.w, 1u)) * brick.voxelSize;
}

fn sample_brick_distance_trilinear(blas: SDFBlas, brick: SDFBrick, uvw: vec3<f32>) -> f32 {
    let atlasBricksPerAxis = max(sceneInfo.settings.z, 1u);
    let globalDistanceOffset = blas.offsetsAndFormat.w + brick.distanceOffset;
    let atlasBrickIndex = globalDistanceOffset / DISTANCE_STRIDE;
    let atlasBrickCoord = vec3<u32>(
        atlasBrickIndex % atlasBricksPerAxis,
        (atlasBrickIndex / atlasBricksPerAxis) % atlasBricksPerAxis,
        atlasBrickIndex / (atlasBricksPerAxis * atlasBricksPerAxis));
    let atlasResolution = f32(atlasBricksPerAxis * DISTANCE_RESOLUTION);
    let brickOffset = vec3<f32>(atlasBrickCoord) * f32(DISTANCE_RESOLUTION);
    let atlasUvw = (brickOffset + vec3<f32>(0.5) + clamp(uvw, vec3<f32>(0.0), vec3<f32>(1.0)) * f32(DISTANCE_RESOLUTION - 1u)) / atlasResolution;
    return textureSampleLevel(distanceAtlasTexture, distanceAtlasSampler, atlasUvw, 0.0).x * f32(max(blas.countsAndNarrowBand.w, 1u)) * brick.voxelSize;
}

fn sample_brick_distance_trilinear_buffer(blas: SDFBlas, brick: SDFBrick, uvw: vec3<f32>) -> f32 {
    let grid = clamp(uvw * f32(BRICK_RESOLUTION), vec3<f32>(0.0), vec3<f32>(f32(BRICK_RESOLUTION)));
    let cell0 = vec3<u32>(floor(grid));
    let cell1 = min(cell0 + vec3<u32>(1u), vec3<u32>(DISTANCE_RESOLUTION - 1u));
    let f = grid - vec3<f32>(cell0);

    let d000 = sample_brick_distance(blas, brick, vec3<u32>(cell0.x, cell0.y, cell0.z));
    let d100 = sample_brick_distance(blas, brick, vec3<u32>(cell1.x, cell0.y, cell0.z));
    let d010 = sample_brick_distance(blas, brick, vec3<u32>(cell0.x, cell1.y, cell0.z));
    let d110 = sample_brick_distance(blas, brick, vec3<u32>(cell1.x, cell1.y, cell0.z));
    let d001 = sample_brick_distance(blas, brick, vec3<u32>(cell0.x, cell0.y, cell1.z));
    let d101 = sample_brick_distance(blas, brick, vec3<u32>(cell1.x, cell0.y, cell1.z));
    let d011 = sample_brick_distance(blas, brick, vec3<u32>(cell0.x, cell1.y, cell1.z));
    let d111 = sample_brick_distance(blas, brick, vec3<u32>(cell1.x, cell1.y, cell1.z));

    let d00 = mix(d000, d100, f.x);
    let d10 = mix(d010, d110, f.x);
    let d01 = mix(d001, d101, f.x);
    let d11 = mix(d011, d111, f.x);
    let d0 = mix(d00, d10, f.y);
    let d1 = mix(d01, d11, f.y);
    return mix(d0, d1, f.z);
}

fn trace_flatten3d(coord: vec3<u32>, dimension: u32) -> u32 {
    return coord.x + coord.y * dimension + coord.z * dimension * dimension;
}

fn load_trace_page_index(blasIndex: u32, rootCell: vec3<u32>) -> u32 {
    let pageTableIndex = blasIndex * TRACE_ROOT_CELL_COUNT + trace_flatten3d(rootCell, TRACE_ROOT_RESOLUTION);
    if (pageTableIndex >= arrayLength(&tracePageTables)) {
        return TRACE_INVALID_BRICK;
    }
    return tracePageTables[pageTableIndex];
}

fn load_trace_page_child_brick(pageIndex: u32, localCell: vec3<u32>) -> u32 {
    if (pageIndex == TRACE_INVALID_BRICK || pageIndex >= arrayLength(&tracePages)) {
        return TRACE_INVALID_BRICK;
    }

    let page = tracePages[pageIndex];
    let localFlat = trace_flatten3d(localCell, TRACE_PAGE_RESOLUTION);
    let wordIndex = localFlat / 32u;
    let localBit = localFlat % 32u;
    if (wordIndex >= TRACE_PAGE_MASK_WORD_COUNT || page.maskOffset + wordIndex >= arrayLength(&tracePageMasks)) {
        return TRACE_INVALID_BRICK;
    }
    let word = tracePageMasks[page.maskOffset + wordIndex];
    let bit = 1u << localBit;
    if ((word & bit) == 0u) {
        return TRACE_INVALID_BRICK;
    }

    var ordinal = 0u;
    for (var i = 0u; i < wordIndex; i = i + 1u) {
        ordinal = ordinal + countOneBits(tracePageMasks[page.maskOffset + i]);
    }
    if (localBit != 0u) {
        ordinal = ordinal + countOneBits(word & ((1u << localBit) - 1u));
    }
    if (ordinal >= page.childCount || page.brickOffset + ordinal >= arrayLength(&traceBrickMaps)) {
        return TRACE_INVALID_BRICK;
    }
    return traceBrickMaps[page.brickOffset + ordinal];
}

fn load_trace_brick_map_entry(blasIndex: u32, cell: vec3<u32>) -> u32 {
    let clampedCell = min(cell, vec3<u32>(TRACE_GRID_RESOLUTION - 1u));
    let rootCell = clampedCell / vec3<u32>(TRACE_PAGE_RESOLUTION);
    let localCell = clampedCell - rootCell * vec3<u32>(TRACE_PAGE_RESOLUTION);
    return load_trace_page_child_brick(load_trace_page_index(blasIndex, rootCell), localCell);
}

fn node_level_by_index(nodeIndex: u32) -> u32 {
    return blasNodes[nodeIndex].levelFlags & 0xffffu;
}

fn cell_bounds_min(boundsMin: vec3<f32>, extent: vec3<f32>, cell: vec3<u32>) -> vec3<f32> {
    return boundsMin + extent * (vec3<f32>(cell) / f32(BRICK_RESOLUTION));
}

fn trace_bounds_empty(packed: u32) -> bool {
    return packed == INVALID_INDEX;
}

fn trace_bounds_min(nodeBoundsMin: vec3<f32>, extent: vec3<f32>, packed: u32) -> vec3<f32> {
    let cell = vec3<u32>(packed & 7u, (packed >> 3u) & 7u, (packed >> 6u) & 7u);
    return cell_bounds_min(nodeBoundsMin, extent, cell);
}

fn trace_bounds_max(nodeBoundsMin: vec3<f32>, extent: vec3<f32>, packed: u32) -> vec3<f32> {
    let cell = vec3<u32>((packed >> 9u) & 7u, (packed >> 12u) & 7u, (packed >> 15u) & 7u);
    return cell_bounds_min(nodeBoundsMin, extent, cell + vec3<u32>(1u));
}

fn sample_local_sdf(instanceIndex: u32, localPoint: vec3<f32>) -> SDFSample {
    if (instanceIndex >= arrayLength(&instances)) {
        return SDFSample(1.0e30, INVALID_INDEX, INVALID_INDEX, 0u, 1.0, 0u, 0u, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0));
    }
    let instance = instances[instanceIndex];
    let blasIndex = instance.idsAndBlas.y;
    if (blasIndex >= arrayLength(&blases)) {
        return SDFSample(1.0e30, INVALID_INDEX, INVALID_INDEX, 0u, 1.0, 0u, 0u, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0));
    }
    let blas = blases[blasIndex];
    var localBoundsMin = blas.localBoundsMin.xyz;
    var localBoundsMax = blas.localBoundsMaxAndRootVoxelSize.xyz;
    var nodeIndex = blas.offsetsAndFormat.x + blas.offsetsAndFormat.y;
    var lastSample = SDFSample(1.0e30, INVALID_INDEX, INVALID_INDEX, 0u, max(blas.localBoundsMaxAndRootVoxelSize.w, 1.0e-4), 0u, 0u, localBoundsMin, localBoundsMax, localBoundsMin, localBoundsMax);

    for (var depth = 0u; depth < 16u; depth = depth + 1u) {
        if (nodeIndex >= arrayLength(&blasNodes)) {
            return lastSample;
        }
        let brickIndex = blas.offsetsAndFormat.z + blasNodes[nodeIndex].brickIndex;
        if (brickIndex >= arrayLength(&blasBricks)) {
            return lastSample;
        }
        let brick = blasBricks[brickIndex];
        let extent = max(localBoundsMax - localBoundsMin, vec3<f32>(1.0e-8));
        let uvw = clamp((localPoint - localBoundsMin) / extent, vec3<f32>(0.0), vec3<f32>(0.999999));
        let cell = vec3<u32>(min(vec3<u32>(uvw * f32(BRICK_RESOLUTION)), vec3<u32>(BRICK_RESOLUTION - 1u)));
        let flatCell = brick_flat_cell(cell);
        let childBit = 1u << (flatCell % 32u);
        let hasChild = blasNodes[nodeIndex].firstChild != INVALID_INDEX &&
            ((blasNodes[nodeIndex].childMaskWords[flatCell / 32u] & childBit) != 0u);
        if (hasChild) {
            nodeIndex = blas.offsetsAndFormat.y + blasNodes[nodeIndex].firstChild + child_ordinal_before_node(nodeIndex, flatCell);
            let childMin = cell_bounds_min(localBoundsMin, extent, cell);
            let childMax = cell_bounds_min(localBoundsMin, extent, cell + vec3<u32>(1u));
            localBoundsMin = childMin;
            localBoundsMax = childMax;
            continue;
        }

        let cellMin = cell_bounds_min(localBoundsMin, extent, cell);
        let cellMax = cell_bounds_min(localBoundsMin, extent, cell + vec3<u32>(1u));
        let validCell = (blasNodes[nodeIndex].validMaskWords[flatCell / 32u] & childBit) != 0u;
        let traceCell = (blasNodes[nodeIndex].traceMaskWords[flatCell / 32u] & childBit) != 0u;
        if (!traceCell) {
            return SDFSample(brick.voxelSize, nodeIndex, blasNodes[nodeIndex].brickIndex, node_level_by_index(nodeIndex), brick.voxelSize, 0u, 0u, cellMin, cellMax, localBoundsMin, localBoundsMax);
        }
        let distance = sample_brick_distance_trilinear(blas, brick, uvw);
        return SDFSample(distance, nodeIndex, blasNodes[nodeIndex].brickIndex, node_level_by_index(nodeIndex), brick.voxelSize, select(0u, 1u, validCell), 1u, cellMin, cellMax, localBoundsMin, localBoundsMax);
    }
    return lastSample;
}

fn bounds_exit_advance(localPoint: vec3<f32>, localDirection: vec3<f32>, boundsMin: vec3<f32>, boundsMax: vec3<f32>, fallbackSize: f32) -> f32 {
    var exitT = 1.0e30;
    for (var axis = 0u; axis < 3u; axis = axis + 1u) {
        let d = localDirection[axis];
        if (abs(d) <= 1.0e-8) {
            continue;
        }
        let boundary = select(boundsMin[axis], boundsMax[axis], d > 0.0);
        let candidate = (boundary - localPoint[axis]) / d;
        if (candidate > 1.0e-6) {
            exitT = min(exitT, candidate);
        }
    }
    let localSpeed = max(length(localDirection), 1.0e-6);
    return select(fallbackSize / localSpeed, exitT, exitT < 1.0e29);
}

fn empty_cell_advance(localPoint: vec3<f32>, localDirection: vec3<f32>, sample: SDFSample) -> f32 {
    return bounds_exit_advance(localPoint, localDirection, sample.cellBoundsMin, sample.cellBoundsMax, sample.voxelSize);
}

fn sample_leaf_sdf(blas: SDFBlas, leaf: SDFSample, localPoint: vec3<f32>) -> SDFSample {
    if (leaf.nodeIndex >= arrayLength(&blasNodes)) {
        return leaf;
    }
    let brickIndex = blas.offsetsAndFormat.z + leaf.brickIndex;
    if (brickIndex >= arrayLength(&blasBricks)) {
        return leaf;
    }
    let brick = blasBricks[brickIndex];
    let extent = max(leaf.nodeBoundsMax - leaf.nodeBoundsMin, vec3<f32>(1.0e-8));
    let uvw = clamp((localPoint - leaf.nodeBoundsMin) / extent, vec3<f32>(0.0), vec3<f32>(0.999999));
    let cell = vec3<u32>(min(vec3<u32>(uvw * f32(BRICK_RESOLUTION)), vec3<u32>(BRICK_RESOLUTION - 1u)));
    let flatCell = brick_flat_cell(cell);
    let cellBit = 1u << (flatCell % 32u);
    let cellWord = flatCell / 32u;
    let validCell = (blasNodes[leaf.nodeIndex].validMaskWords[cellWord] & cellBit) != 0u;
    let traceCell = (blasNodes[leaf.nodeIndex].traceMaskWords[cellWord] & cellBit) != 0u;
    let cellMin = cell_bounds_min(leaf.nodeBoundsMin, extent, cell);
    let cellMax = cell_bounds_min(leaf.nodeBoundsMin, extent, cell + vec3<u32>(1u));
    let distance = sample_brick_distance_trilinear(blas, brick, uvw);
    return SDFSample(distance, leaf.nodeIndex, leaf.brickIndex, leaf.level, leaf.voxelSize, select(0u, 1u, validCell), select(0u, 1u, traceCell), cellMin, cellMax, leaf.nodeBoundsMin, leaf.nodeBoundsMax);
}

fn point_inside_leaf_node(leaf: SDFSample, localPoint: vec3<f32>) -> bool {
    return all(localPoint >= leaf.nodeBoundsMin) && all(localPoint <= leaf.nodeBoundsMax);
}

fn sample_leaf_or_local_sdf(instanceIndex: u32, blas: SDFBlas, leaf: SDFSample, localPoint: vec3<f32>) -> SDFSample {
    if (point_inside_leaf_node(leaf, localPoint)) {
        return sample_leaf_sdf(blas, leaf, localPoint);
    }
    return sample_local_sdf(instanceIndex, localPoint);
}

fn safe_normalize(v: vec3<f32>, fallback: vec3<f32>) -> vec3<f32> {
    let l2 = dot(v, v);
    if (l2 <= 1.0e-12) {
        return fallback;
    }
    return v * inverseSqrt(l2);
}

fn orient_normal_to_ray(normal: vec3<f32>, rayDirection: vec3<f32>) -> vec3<f32> {
    return select(normal, -normal, dot(normal, rayDirection) > 0.0);
}

fn sdf_normal(instanceIndex: u32, localPoint: vec3<f32>, worldToLocal: mat4x4<f32>, eps: f32) -> vec3<f32> {
    let dx = sample_local_sdf(instanceIndex, localPoint + vec3<f32>(eps, 0.0, 0.0)).distance - sample_local_sdf(instanceIndex, localPoint - vec3<f32>(eps, 0.0, 0.0)).distance;
    let dy = sample_local_sdf(instanceIndex, localPoint + vec3<f32>(0.0, eps, 0.0)).distance - sample_local_sdf(instanceIndex, localPoint - vec3<f32>(0.0, eps, 0.0)).distance;
    let dz = sample_local_sdf(instanceIndex, localPoint + vec3<f32>(0.0, 0.0, eps)).distance - sample_local_sdf(instanceIndex, localPoint - vec3<f32>(0.0, 0.0, eps)).distance;
    let localNormal = safe_normalize(vec3<f32>(dx, dy, dz), vec3<f32>(0.0, 0.0, 1.0));
    return safe_normalize(transform_vector(transpose(worldToLocal), localNormal), vec3<f32>(0.0, 0.0, 1.0));
}

fn sample_leaf_brick_distance(blas: SDFBlas, leaf: SDFSample, localPoint: vec3<f32>) -> f32 {
    let brickIndex = blas.offsetsAndFormat.z + leaf.brickIndex;
    if (brickIndex >= arrayLength(&blasBricks)) {
        return leaf.distance;
    }

    let brick = blasBricks[brickIndex];
    let extent = max(leaf.nodeBoundsMax - leaf.nodeBoundsMin, vec3<f32>(1.0e-8));
    let uvw = clamp((localPoint - leaf.nodeBoundsMin) / extent, vec3<f32>(0.0), vec3<f32>(1.0));
    return sample_brick_distance_trilinear(blas, brick, uvw);
}

fn sdf_normal_leaf(instanceIndex: u32, blas: SDFBlas, leaf: SDFSample, localPoint: vec3<f32>, worldToLocal: mat4x4<f32>, eps: f32) -> vec3<f32> {
    let k0 = vec3<f32>(1.0, -1.0, -1.0);
    let k1 = vec3<f32>(-1.0, -1.0, 1.0);
    let k2 = vec3<f32>(-1.0, 1.0, -1.0);
    let k3 = vec3<f32>(1.0, 1.0, 1.0);
    let localEps = min(eps, max(leaf.voxelSize * 0.35, 1.0e-5));
    let d0 = sample_leaf_brick_distance(blas, leaf, localPoint + k0 * localEps);
    let d1 = sample_leaf_brick_distance(blas, leaf, localPoint + k1 * localEps);
    let d2 = sample_leaf_brick_distance(blas, leaf, localPoint + k2 * localEps);
    let d3 = sample_leaf_brick_distance(blas, leaf, localPoint + k3 * localEps);
    let localNormal = safe_normalize(k0 * d0 + k1 * d1 + k2 * d2 + k3 * d3, vec3<f32>(0.0, 0.0, 1.0));
    return safe_normalize(transform_vector(transpose(worldToLocal), localNormal), vec3<f32>(0.0, 0.0, 1.0));
}

fn sample_mapped_trace_brick(blas: SDFBlas, brick: SDFBrick, localPoint: vec3<f32>, boundsMin: vec3<f32>, boundsMax: vec3<f32>) -> f32 {
    let uvw = clamp((localPoint - boundsMin) / max(boundsMax - boundsMin, vec3<f32>(1.0e-8)), vec3<f32>(0.0), vec3<f32>(1.0));
    return sample_brick_distance_trilinear(blas, brick, uvw);
}

fn trace_cell_sample_bounds(cellMin: vec3<f32>, gridCellSize: vec3<f32>) -> vec4<f32> {
    let pad = max(max(gridCellSize.x, max(gridCellSize.y, gridCellSize.z)), 1.0e-8) / f32(BRICK_RESOLUTION);
    return vec4<f32>(cellMin - vec3<f32>(pad), pad);
}

fn sample_sparse_trace_sdf(
    blasIndex: u32,
    blas: SDFBlas,
    fallbackBrick: SDFBrick,
    localPoint: vec3<f32>,
    gridMin: vec3<f32>,
    gridCellSize: vec3<f32>,
    fallbackBoundsMin: vec3<f32>,
    fallbackBoundsMax: vec3<f32>) -> f32 {
    let gridMax = blas.localBoundsMaxAndRootVoxelSize.xyz;
    if (any(localPoint < gridMin) || any(localPoint > gridMax)) {
        return sample_mapped_trace_brick(blas, fallbackBrick, localPoint, fallbackBoundsMin, fallbackBoundsMax);
    }

    let cellCoord = min(vec3<u32>(floor((localPoint - gridMin) / gridCellSize)), vec3<u32>(TRACE_GRID_RESOLUTION - 1u));
    let localBrickIndex = load_trace_brick_map_entry(blasIndex, cellCoord);
    if (localBrickIndex == TRACE_INVALID_BRICK) {
        return sample_mapped_trace_brick(blas, fallbackBrick, localPoint, fallbackBoundsMin, fallbackBoundsMax);
    }

    let brickIndex = blas.offsetsAndFormat.z + localBrickIndex;
    if (brickIndex >= arrayLength(&blasBricks)) {
        return sample_mapped_trace_brick(blas, fallbackBrick, localPoint, fallbackBoundsMin, fallbackBoundsMax);
    }

    let cellMin = gridMin + vec3<f32>(cellCoord) * gridCellSize;
    let sampleBounds = trace_cell_sample_bounds(cellMin, gridCellSize);
    return sample_mapped_trace_brick(blas, blasBricks[brickIndex], localPoint, sampleBounds.xyz, cellMin + gridCellSize + vec3<f32>(sampleBounds.w));
}

fn mapped_trace_brick_normal(
    blasIndex: u32,
    blas: SDFBlas,
    brick: SDFBrick,
    localPoint: vec3<f32>,
    boundsMin: vec3<f32>,
    boundsMax: vec3<f32>,
    gridMin: vec3<f32>,
    gridCellSize: vec3<f32>,
    worldToLocal: mat4x4<f32>) -> vec3<f32> {
    let eps = max(brick.voxelSize * 0.35, 1.0e-5);
    let k0 = vec3<f32>(1.0, -1.0, -1.0);
    let k1 = vec3<f32>(-1.0, -1.0, 1.0);
    let k2 = vec3<f32>(-1.0, 1.0, -1.0);
    let k3 = vec3<f32>(1.0, 1.0, 1.0);
    let d0 = sample_mapped_trace_brick(blas, brick, localPoint + k0 * eps, boundsMin, boundsMax);
    let d1 = sample_mapped_trace_brick(blas, brick, localPoint + k1 * eps, boundsMin, boundsMax);
    let d2 = sample_mapped_trace_brick(blas, brick, localPoint + k2 * eps, boundsMin, boundsMax);
    let d3 = sample_mapped_trace_brick(blas, brick, localPoint + k3 * eps, boundsMin, boundsMax);
    let localNormal = safe_normalize(k0 * d0 + k1 * d1 + k2 * d2 + k3 * d3, vec3<f32>(0.0, 0.0, 1.0));
    return safe_normalize(transform_vector(transpose(worldToLocal), localNormal), vec3<f32>(0.0, 0.0, 1.0));
}

fn trace_mapped_trace_brick(
    instanceIndex: u32,
    instance: SDFInstance,
    blasIndex: u32,
    blas: SDFBlas,
    localBrickIndex: u32,
    origin: vec3<f32>,
    direction: vec3<f32>,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    cellCoord: vec3<u32>,
    cellStartT: f32,
    cellEndT: f32,
    gridMin: vec3<f32>,
    gridCellSize: vec3<f32>,
    stepBase: u32,
    requireNormal: bool) -> SDFCandidateHit {
    let brickIndex = blas.offsetsAndFormat.z + localBrickIndex;
    if (brickIndex >= arrayLength(&blasBricks)) {
        return invalid_hit(1.0e30);
    }

    let brick = blasBricks[brickIndex];
    let cellMin = gridMin + vec3<f32>(cellCoord) * gridCellSize;
    let cellMax = cellMin + gridCellSize;
    let sampleBounds = trace_cell_sample_bounds(cellMin, gridCellSize);
    let sampleMin = sampleBounds.xyz;
    let sampleMax = cellMax + vec3<f32>(sampleBounds.w);
    let localSpeed = max(length(localDirection), 1.0e-6);
    let thresholdLocal = max(brick.voxelSize * uniforms.hitThresholdScale, 1.0e-5 / max(instance.worldBoundsMaxAndConservativeScale.w, 1.0e-6));
    var t = cellStartT;
    var previousDistance = sample_mapped_trace_brick(blas, brick, localOrigin + localDirection * t, sampleMin, sampleMax);
    var previousT = t;

    for (var innerStep = 0u; innerStep < 8u && t <= cellEndT; innerStep = innerStep + 1u) {
        let localPoint = localOrigin + localDirection * t;
        let distanceLocal = sample_mapped_trace_brick(blas, brick, localPoint, sampleMin, sampleMax);
        let crossedSurface = previousDistance > 0.0 && distanceLocal <= 0.0;
        if (abs(distanceLocal) <= thresholdLocal || crossedSurface) {
            var hitT = t;
            if (crossedSurface) {
                var loT = previousT;
                var hiT = t;
                for (var refine = 0u; refine < 5u; refine = refine + 1u) {
                    let midT = (loT + hiT) * 0.5;
                    let midDistance = sample_mapped_trace_brick(blas, brick, localOrigin + localDirection * midT, sampleMin, sampleMax);
                    if (midDistance <= 0.0) {
                        hiT = midT;
                    } else {
                        loT = midT;
                    }
                }
                hitT = hiT;
            }
            let hitLocal = localOrigin + localDirection * hitT;
            let pWorld = origin + direction * hitT;
            var normal = vec3<f32>(0.0, 0.0, 1.0);
            if (requireNormal) {
                normal = sdf_normal(instanceIndex, hitLocal, instance.worldToLocal, max(brick.voxelSize * 0.5, 1.0e-4));
            }
            return SDFCandidateHit(true, hitT, instanceIndex, instance.idsAndBlas.x, blasIndex, INVALID_INDEX, localBrickIndex, 0u, stepBase + innerStep + 1u, 0u, 0u, hitLocal, pWorld, orient_normal_to_ray(normal, direction));
        }

        previousT = t;
        previousDistance = distanceLocal;
        let advanceT = max(abs(distanceLocal) - thresholdLocal * 0.5, brick.voxelSize * max(uniforms.minStepScale, 0.05)) / localSpeed;
        t = max(t + advanceT, t + 1.0e-6);
    }

    return invalid_hit(1.0e30);
}

fn make_hit(
    instanceIndex: u32,
    instance: SDFInstance,
    blasIndex: u32,
    blas: SDFBlas,
    leaf: SDFSample,
    hitSample: SDFSample,
    origin: vec3<f32>,
    direction: vec3<f32>,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    hitT: f32,
    stepCount: u32,
    requireNormal: bool) -> SDFCandidateHit {
    let hitLocal = localOrigin + localDirection * hitT;
    let pWorld = origin + direction * hitT;
    var normal = vec3<f32>(0.0, 0.0, 1.0);
    if (requireNormal) {
        normal = sdf_normal_leaf(instanceIndex, blas, leaf, hitLocal, instance.worldToLocal, max(hitSample.voxelSize * 0.5, 1.0e-4));
    }
    return SDFCandidateHit(true, hitT, instanceIndex, instance.idsAndBlas.x, blasIndex, hitSample.nodeIndex, hitSample.brickIndex, hitSample.level, stepCount, 0u, 0u, hitLocal, pWorld, orient_normal_to_ray(normal, direction));
}

fn solve_valid_leaf_segment(
    instanceIndex: u32,
    instance: SDFInstance,
    blasIndex: u32,
    blas: SDFBlas,
    leaf: SDFSample,
    origin: vec3<f32>,
    direction: vec3<f32>,
    localOrigin: vec3<f32>,
    localDirection: vec3<f32>,
    segmentStart: f32,
    segmentEnd: f32,
    previousT: f32,
    previousWorldDistance: f32,
    hasPreviousSample: bool,
    stepBase: u32,
    requireNormal: bool) -> SDFCandidateHit {
    let threshold = max(leaf.voxelSize * instance.worldBoundsMaxAndConservativeScale.w * uniforms.hitThresholdScale, 1.0e-5);
    let segmentLength = max(segmentEnd - segmentStart, 0.0);
    var bestAbsDistance = 1.0e30;
    var bestT = segmentStart;
    var bestSample = leaf;
    var lastT = segmentStart;
    var lastDistance = previousWorldDistance;
    var hasLast = hasPreviousSample;

    for (var probe = 0u; probe < 3u; probe = probe + 1u) {
        let u = f32(probe) * 0.5;
        let probeT = segmentStart + segmentLength * u;
        let probeLocal = localOrigin + localDirection * probeT;
        let probeSample = sample_leaf_or_local_sdf(instanceIndex, blas, leaf, probeLocal);
        let worldDistance = probeSample.distance * instance.worldBoundsMaxAndConservativeScale.w;
        let absDistance = abs(worldDistance);
        if (absDistance < bestAbsDistance) {
            bestAbsDistance = absDistance;
            bestT = probeT;
            bestSample = probeSample;
        }
        if (probeSample.validCell != 0u && hasLast && lastDistance > 0.0 && worldDistance <= 0.0) {
            var loT = lastT;
            var hiT = probeT;
            var hitSample = probeSample;
            for (var refine = 0u; refine < 5u; refine = refine + 1u) {
                let midT = (loT + hiT) * 0.5;
                let midLocal = localOrigin + localDirection * midT;
                let midSample = sample_leaf_or_local_sdf(instanceIndex, blas, leaf, midLocal);
                let midWorldDistance = midSample.distance * instance.worldBoundsMaxAndConservativeScale.w;
                if (midSample.validCell != 0u && midWorldDistance <= 0.0) {
                    hiT = midT;
                    hitSample = midSample;
                } else {
                    loT = midT;
                }
            }
            return make_hit(instanceIndex, instance, blasIndex, blas, leaf, hitSample, origin, direction, localOrigin, localDirection, hiT, stepBase + probe + 6u, requireNormal);
        }
        hasLast = true;
        lastT = probeT;
        lastDistance = worldDistance;
    }

    if (bestSample.validCell != 0u && bestAbsDistance <= threshold * 1.5) {
        let probeRadius = segmentLength * 0.5;
        var loT = max(segmentStart, bestT - probeRadius);
        var hiT = min(segmentEnd, bestT + probeRadius);
        var refinedT = bestT;
        var refinedSample = bestSample;
        var refinedAbsDistance = bestAbsDistance;
        for (var refine = 0u; refine < 4u; refine = refine + 1u) {
            let leftT = mix(loT, hiT, 0.33333334);
            let rightT = mix(loT, hiT, 0.6666667);
            let leftSample = sample_leaf_or_local_sdf(instanceIndex, blas, leaf, localOrigin + localDirection * leftT);
            let rightSample = sample_leaf_or_local_sdf(instanceIndex, blas, leaf, localOrigin + localDirection * rightT);
            let leftAbsDistance = abs(leftSample.distance * instance.worldBoundsMaxAndConservativeScale.w);
            let rightAbsDistance = abs(rightSample.distance * instance.worldBoundsMaxAndConservativeScale.w);
            if (leftSample.validCell != 0u && leftAbsDistance < refinedAbsDistance) {
                refinedAbsDistance = leftAbsDistance;
                refinedT = leftT;
                refinedSample = leftSample;
            }
            if (rightSample.validCell != 0u && rightAbsDistance < refinedAbsDistance) {
                refinedAbsDistance = rightAbsDistance;
                refinedT = rightT;
                refinedSample = rightSample;
            }
            if (leftAbsDistance < rightAbsDistance) {
                hiT = rightT;
            } else {
                loT = leftT;
            }
        }
        return make_hit(instanceIndex, instance, blasIndex, blas, leaf, refinedSample, origin, direction, localOrigin, localDirection, refinedT, stepBase + 14u, requireNormal);
    }
    return invalid_hit(1.0e30);
}

fn trace_instance(instanceIndex: u32, origin: vec3<f32>, direction: vec3<f32>, tMinWorld: f32, best: SDFCandidateHit, requireNormal: bool) -> SDFCandidateHit {
    if (instanceIndex >= arrayLength(&instances)) {
        return best;
    }
    let instance = instances[instanceIndex];
    let blasIndex = instance.idsAndBlas.y;
    if (blasIndex >= arrayLength(&blases)) {
        return best;
    }
    let blas = blases[blasIndex];
    let localOrigin = transform_point(instance.worldToLocal, origin);
    let localFar = transform_point(instance.worldToLocal, origin + direction * best.t);
    let localDirection = (localFar - localOrigin) / max(best.t, 1.0e-6);
    let localBoundsT = ray_aabb(localOrigin, localDirection, blas.localBoundsMin.xyz, blas.localBoundsMaxAndRootVoxelSize.xyz, tMinWorld, best.t);
    if (localBoundsT.x > localBoundsT.y) {
        return best;
    }
    var outHit = best;
    var step = 0u;
    let traversalStepLimit = max(uniforms.maxSteps, 32u);
    var rayT = max(max(tMinWorld, localBoundsT.x), 0.0);
    let traceHitMax = min(localBoundsT.y, outHit.t);

    var hasPreviousSample = false;
    var previousT = rayT;
    var previousWorldDistance = 1.0e30;
    var previousSample = SDFSample(1.0e30, INVALID_INDEX, INVALID_INDEX, 0u, 1.0, 0u, 0u, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0));
    while (rayT <= traceHitMax && rayT < outHit.t && step < traversalStepLimit) {
        step = step + 1u;
        let localPoint = localOrigin + localDirection * rayT;
        let sample = sample_local_sdf(instanceIndex, localPoint);
        let worldDistance = sample.distance * instance.worldBoundsMaxAndConservativeScale.w;
        let threshold = max(sample.voxelSize * instance.worldBoundsMaxAndConservativeScale.w * uniforms.hitThresholdScale, 1.0e-5);

        let crossedSurface = hasPreviousSample &&
            previousSample.traceCell != 0u &&
            sample.traceCell != 0u &&
            previousWorldDistance > 0.0 &&
            worldDistance <= 0.0;
        let validCrossing = crossedSurface && (sample.validCell != 0u || previousSample.validCell != 0u);
        if ((sample.validCell != 0u && abs(worldDistance) <= threshold) || validCrossing) {
            var hitT = rayT;
            var hitSample = sample;
            if (hitSample.validCell == 0u && previousSample.validCell != 0u) {
                hitSample = previousSample;
            }
            if (crossedSurface) {
                var loT = previousT;
                var hiT = rayT;
                for (var refine = 0u; refine < 6u; refine = refine + 1u) {
                    let midT = (loT + hiT) * 0.5;
                    let midSample = sample_local_sdf(instanceIndex, localOrigin + localDirection * midT);
                    let midWorldDistance = midSample.distance * instance.worldBoundsMaxAndConservativeScale.w;
                    if (midSample.validCell != 0u && midWorldDistance <= 0.0) {
                        hiT = midT;
                        hitSample = midSample;
                    } else {
                        loT = midT;
                    }
                }
                hitT = hiT;
            }
            return make_hit(instanceIndex, instance, blasIndex, blas, hitSample, hitSample, origin, direction, localOrigin, localDirection, hitT, step, requireNormal);
        }

        let minStep = max(sample.voxelSize * instance.worldBoundsMaxAndConservativeScale.w * max(uniforms.minStepScale, 0.05), 1.0e-5);
        let signedDistanceAdvance = max(abs(worldDistance) * uniforms.stepScale, minStep);
        let emptyAdvance = max(empty_cell_advance(localPoint, localDirection, sample), minStep) + minStep * 0.25;
        let surfaceAdvance = min(signedDistanceAdvance, max(emptyAdvance * 0.75, minStep));
        previousT = rayT;
        previousWorldDistance = worldDistance;
        previousSample = sample;
        hasPreviousSample = true;
        rayT = rayT + select(emptyAdvance, surfaceAdvance, sample.traceCell != 0u);
        if (rayT <= previousT) {
            rayT = previousT + minStep;
        }
    }
    outHit.steps = max(outHit.steps, step);
    return outHit;
}

fn trace_root_page_coord(rootPageFlat: u32) -> vec3<u32> {
    return vec3<u32>(
        rootPageFlat % TRACE_ROOT_RESOLUTION,
        (rootPageFlat / TRACE_ROOT_RESOLUTION) % TRACE_ROOT_RESOLUTION,
        rootPageFlat / (TRACE_ROOT_RESOLUTION * TRACE_ROOT_RESOLUTION));
}

fn trace_instance_page(instanceIndex: u32, rootPageFlat: u32, origin: vec3<f32>, direction: vec3<f32>, tMinWorld: f32, best: SDFCandidateHit, requireNormal: bool) -> SDFCandidateHit {
    if (instanceIndex >= arrayLength(&instances) || rootPageFlat >= TRACE_ROOT_CELL_COUNT) {
        return best;
    }
    let instance = instances[instanceIndex];
    let blasIndex = instance.idsAndBlas.y;
    if (blasIndex >= arrayLength(&blases)) {
        return best;
    }
    let blas = blases[blasIndex];
    let pageIndex = load_trace_page_index(blasIndex, trace_root_page_coord(rootPageFlat));
    if (pageIndex == TRACE_INVALID_BRICK) {
        return best;
    }

    let localOrigin = transform_point(instance.worldToLocal, origin);
    let localFar = transform_point(instance.worldToLocal, origin + direction * best.t);
    let localDirection = (localFar - localOrigin) / max(best.t, 1.0e-6);
    let gridMin = blas.localBoundsMin.xyz;
    let gridMax = blas.localBoundsMaxAndRootVoxelSize.xyz;
    let gridCellSize = (gridMax - gridMin) / vec3<f32>(f32(TRACE_GRID_RESOLUTION));
    let pageCoord = trace_root_page_coord(rootPageFlat);
    let pageMin = gridMin + vec3<f32>(pageCoord * vec3<u32>(TRACE_PAGE_RESOLUTION)) * gridCellSize;
    let pageMax = pageMin + vec3<f32>(TRACE_PAGE_RESOLUTION) * gridCellSize;
    let localBoundsT = ray_aabb(localOrigin, localDirection, pageMin, pageMax, tMinWorld, best.t);
    if (localBoundsT.x > localBoundsT.y) {
        return best;
    }

    var outHit = best;
    var step = 0u;
    let traversalStepLimit = max(uniforms.maxSteps / 2u, 32u);
    var rayT = max(max(tMinWorld, localBoundsT.x), 0.0);
    let traceHitMax = min(localBoundsT.y, outHit.t);

    var hasPreviousSample = false;
    var previousT = rayT;
    var previousWorldDistance = 1.0e30;
    var previousSample = SDFSample(1.0e30, INVALID_INDEX, INVALID_INDEX, 0u, 1.0, 0u, 0u, vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0));
    while (rayT <= traceHitMax && rayT < outHit.t && step < traversalStepLimit) {
        step = step + 1u;
        let localPoint = localOrigin + localDirection * rayT;
        let sample = sample_local_sdf(instanceIndex, localPoint);
        let worldDistance = sample.distance * instance.worldBoundsMaxAndConservativeScale.w;
        let threshold = max(sample.voxelSize * instance.worldBoundsMaxAndConservativeScale.w * uniforms.hitThresholdScale, 1.0e-5);

        let crossedSurface = hasPreviousSample &&
            previousSample.traceCell != 0u &&
            sample.traceCell != 0u &&
            previousWorldDistance > 0.0 &&
            worldDistance <= 0.0;
        let validCrossing = crossedSurface && (sample.validCell != 0u || previousSample.validCell != 0u);
        if ((sample.validCell != 0u && abs(worldDistance) <= threshold) || validCrossing) {
            var hitT = rayT;
            var hitSample = sample;
            if (hitSample.validCell == 0u && previousSample.validCell != 0u) {
                hitSample = previousSample;
            }
            if (crossedSurface) {
                var loT = previousT;
                var hiT = rayT;
                for (var refine = 0u; refine < 6u; refine = refine + 1u) {
                    let midT = (loT + hiT) * 0.5;
                    let midSample = sample_local_sdf(instanceIndex, localOrigin + localDirection * midT);
                    let midWorldDistance = midSample.distance * instance.worldBoundsMaxAndConservativeScale.w;
                    if (midSample.validCell != 0u && midWorldDistance <= 0.0) {
                        hiT = midT;
                        hitSample = midSample;
                    } else {
                        loT = midT;
                    }
                }
                hitT = hiT;
            }
            return make_hit(instanceIndex, instance, blasIndex, blas, hitSample, hitSample, origin, direction, localOrigin, localDirection, hitT, step, requireNormal);
        }

        let minStep = max(sample.voxelSize * instance.worldBoundsMaxAndConservativeScale.w * max(uniforms.minStepScale, 0.05), 1.0e-5);
        let signedDistanceAdvance = max(abs(worldDistance) * uniforms.stepScale, minStep);
        let emptyAdvance = max(empty_cell_advance(localPoint, localDirection, sample), minStep) + minStep * 0.25;
        let surfaceAdvance = min(signedDistanceAdvance, max(emptyAdvance * 0.75, minStep));
        previousT = rayT;
        previousWorldDistance = worldDistance;
        previousSample = sample;
        hasPreviousSample = true;
        rayT = rayT + select(emptyAdvance, surfaceAdvance, sample.traceCell != 0u);
        if (rayT <= previousT) {
            rayT = previousT + minStep;
        }
    }
    outHit.steps = max(outHit.steps, step);
    return outHit;
}

fn scene_grid_ref_mailbox_key(gridRef: SDFSceneGridRef) -> u32 {
    let kind = gridRef.kindInstanceAndPage.x;
    let instanceIndex = gridRef.kindInstanceAndPage.y & 0x7fffu;
    if (kind == SCENE_GRID_REF_PAGE) {
        return 0x80000000u | (instanceIndex << 16u) | (gridRef.kindInstanceAndPage.z & 0xffffu);
    }
    return instanceIndex;
}

fn trace_scene_grid_ref(gridRef: SDFSceneGridRef, origin: vec3<f32>, direction: vec3<f32>, tMin: f32, best: SDFCandidateHit, requireNormal: bool) -> SDFCandidateHit {
    let refT = ray_aabb(origin, direction, gridRef.boundsMin.xyz, gridRef.boundsMax.xyz, tMin, best.t);
    if (refT.x > refT.y) {
        return best;
    }
    let kind = gridRef.kindInstanceAndPage.x;
    let instanceIndex = gridRef.kindInstanceAndPage.y;
    if (kind == SCENE_GRID_REF_PAGE) {
        return trace_instance_page(instanceIndex, gridRef.kindInstanceAndPage.z, origin, direction, max(tMin, refT.x), best, requireNormal);
    }
    return trace_instance(instanceIndex, origin, direction, max(tMin, refT.x), best, requireNormal);
}

fn trace_scene_grid(origin: vec3<f32>, direction: vec3<f32>, tMin: f32, tMax: f32, traceInstances: bool, requireNormal: bool) -> SDFCandidateHit {
    var best = invalid_hit(tMax);
    if (!traceInstances || arrayLength(&sceneGridInfoBuffer) == 0u) {
        return best;
    }
    let gridInfo = sceneGridInfoBuffer[0u];
    let gridRes = u32(gridInfo.worldBoundsMaxAndResolution.w);
    if (gridRes == 0u || gridInfo.counts.x == 0u || gridInfo.counts.y == 0u) {
        return best;
    }
    let gridMin = gridInfo.worldBoundsMin.xyz;
    let gridMax = gridInfo.worldBoundsMaxAndResolution.xyz;
    let rootT = ray_aabb(origin, direction, gridMin, gridMax, tMin, tMax);
    if (rootT.x > rootT.y) {
        return best;
    }

    var mailbox : array<u32, MAILBOX_SIZE>;
    var mailboxCount = 0u;
    var instanceTests = 0u;
    var tlasVisits = 0u;

    let globalCount = min(gridInfo.counts.w, arrayLength(&sceneGridGlobalRefs));
    for (var i = 0u; i < globalCount; i = i + 1u) {
        let gridRef = sceneGridGlobalRefs[i];
        let key = scene_grid_ref_mailbox_key(gridRef);
        var seen = false;
        for (var m = 0u; m < mailboxCount; m = m + 1u) {
            if (mailbox[m] == key) {
                seen = true;
                break;
            }
        }
        if (!seen) {
            if (mailboxCount < MAILBOX_SIZE) {
                mailbox[mailboxCount] = key;
                mailboxCount = mailboxCount + 1u;
            }
            instanceTests = instanceTests + 1u;
            best = trace_scene_grid_ref(gridRef, origin, direction, tMin, best, requireNormal);
        }
    }

    let cellSize = (gridMax - gridMin) / vec3<f32>(f32(gridRes));
    var rayT = max(rootT.x, tMin);
    let rayEndT = min(rootT.y, tMax);
    var p = clamp(origin + direction * rayT, gridMin, gridMax - cellSize * 1.0e-4);
    var cell = clamp(vec3<i32>(floor((p - gridMin) / cellSize)), vec3<i32>(0), vec3<i32>(i32(gridRes) - 1));
    let positiveDirection = direction >= vec3<f32>(0.0);
    let step = select(vec3<i32>(-1), vec3<i32>(1), positiveDirection);
    let boundaryOffset = select(vec3<i32>(0), vec3<i32>(1), positiveDirection);
    let cellF = vec3<f32>(f32(cell.x), f32(cell.y), f32(cell.z));
    let boundaryOffsetF = vec3<f32>(f32(boundaryOffset.x), f32(boundaryOffset.y), f32(boundaryOffset.z));
    let nextBoundary = gridMin + (cellF + boundaryOffsetF) * cellSize;
    var tMaxAxis = vec3<f32>(
        select(1.0e30, (nextBoundary.x - origin.x) / direction.x, abs(direction.x) > 1.0e-8),
        select(1.0e30, (nextBoundary.y - origin.y) / direction.y, abs(direction.y) > 1.0e-8),
        select(1.0e30, (nextBoundary.z - origin.z) / direction.z, abs(direction.z) > 1.0e-8));
    let tDelta = vec3<f32>(
        select(1.0e30, cellSize.x / abs(direction.x), abs(direction.x) > 1.0e-8),
        select(1.0e30, cellSize.y / abs(direction.y), abs(direction.y) > 1.0e-8),
        select(1.0e30, cellSize.z / abs(direction.z), abs(direction.z) > 1.0e-8));

    for (var iter = 0u; iter < 512u && rayT <= rayEndT && rayT <= best.t; iter = iter + 1u) {
        if (any(cell < vec3<i32>(0)) || any(cell >= vec3<i32>(i32(gridRes)))) {
            break;
        }
        let cellIndex = u32(cell.x) + u32(cell.y) * gridRes + u32(cell.z) * gridRes * gridRes;
        if (cellIndex < arrayLength(&sceneGridCells)) {
            tlasVisits = tlasVisits + 1u;
            let gridCell = sceneGridCells[cellIndex];
            let firstRef = gridCell.offsetCountAndFlags.x;
            let refCount = min(gridCell.offsetCountAndFlags.y, gridInfo.counts.y);
            let endRef = min(firstRef + refCount, arrayLength(&sceneGridRefs));
            for (var refIndex = firstRef; refIndex < endRef; refIndex = refIndex + 1u) {
                let gridRef = sceneGridRefs[refIndex];
                let key = scene_grid_ref_mailbox_key(gridRef);
                var seen = false;
                for (var m = 0u; m < mailboxCount; m = m + 1u) {
                    if (mailbox[m] == key) {
                        seen = true;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                if (mailboxCount < MAILBOX_SIZE) {
                    mailbox[mailboxCount] = key;
                    mailboxCount = mailboxCount + 1u;
                }
                instanceTests = instanceTests + 1u;
                best = trace_scene_grid_ref(gridRef, origin, direction, tMin, best, requireNormal);
            }
        }

        if (tMaxAxis.x <= tMaxAxis.y && tMaxAxis.x <= tMaxAxis.z) {
            rayT = tMaxAxis.x;
            tMaxAxis.x = tMaxAxis.x + tDelta.x;
            cell.x = cell.x + step.x;
        } else if (tMaxAxis.y <= tMaxAxis.z) {
            rayT = tMaxAxis.y;
            tMaxAxis.y = tMaxAxis.y + tDelta.y;
            cell.y = cell.y + step.y;
        } else {
            rayT = tMaxAxis.z;
            tMaxAxis.z = tMaxAxis.z + tDelta.z;
            cell.z = cell.z + step.z;
        }
    }

    best.tlasVisits = tlasVisits;
    best.instanceTests = instanceTests;
    return best;
}

fn trace_scene(origin: vec3<f32>, direction: vec3<f32>, tMin: f32, tMax: f32, traceInstances: bool, requireNormal: bool) -> SDFCandidateHit {
    var best = invalid_hit(tMax);
    if (sceneInfo.counts.x == 0u || sceneInfo.counts.z == 0u || sceneInfo.worldBoundsMaxAndRootNode.w < 0.0) {
        return best;
    }

    if (arrayLength(&sceneGridInfoBuffer) > 0u && sceneGridInfoBuffer[0u].counts.x != 0u) {
        return trace_scene_grid(origin, direction, tMin, tMax, traceInstances, requireNormal);
    }

    var mailbox : array<u32, MAILBOX_SIZE>;
    var mailboxCount = 0u;
    var stackNode : array<u32, 64>;
    var stackTMin : array<f32, 64>;
    var stackCount = 0u;
    var tlasVisits = 0u;
    var instanceTests = 0u;
    let rootT = ray_aabb(origin, direction, sceneInfo.worldBoundsMinAndMaxDepth.xyz, sceneInfo.worldBoundsMaxAndRootNode.xyz, tMin, tMax);
    if (rootT.x > rootT.y) {
        return best;
    }
    if (sceneInfo.counts.x == 1u) {
        best.tlasVisits = 1u;
        if (0u < arrayLength(&instances)) {
            let instance = instances[0u];
            let instanceT = ray_aabb(origin, direction, instance.worldBoundsMin.xyz, instance.worldBoundsMaxAndConservativeScale.xyz, tMin, best.t);
            if (instanceT.x <= instanceT.y) {
                best.instanceTests = 1u;
                if (traceInstances) {
                    best = trace_instance(0u, origin, direction, max(tMin, instanceT.x), best, requireNormal);
                    best.tlasVisits = 1u;
                    best.instanceTests = 1u;
                }
            }
        }
        return best;
    }
    stackNode[0] = u32(sceneInfo.worldBoundsMaxAndRootNode.w);
    stackTMin[0] = rootT.x;
    stackCount = 1u;

    for (var iter = 0u; iter < 256u && stackCount > 0u; iter = iter + 1u) {
        var bestStack = 0u;
        var bestStackT = stackTMin[0];
        for (var i = 1u; i < stackCount; i = i + 1u) {
            if (stackTMin[i] < bestStackT) {
                bestStack = i;
                bestStackT = stackTMin[i];
            }
        }
        stackCount = stackCount - 1u;
        let nodeIndex = stackNode[bestStack];
        let nodeT = stackTMin[bestStack];
        stackNode[bestStack] = stackNode[stackCount];
        stackTMin[bestStack] = stackTMin[stackCount];
        if (nodeIndex >= arrayLength(&tlasNodes) || nodeT > best.t) {
            continue;
        }
        tlasVisits = tlasVisits + 1u;
        let node = tlasNodes[nodeIndex];
        let firstRef = node.childAndRefs.z;
        let refCount = node.childAndRefs.w;
        let endRef = min(firstRef + refCount, arrayLength(&tlasInstanceRefs));
        for (var refIndex = firstRef; refIndex < endRef; refIndex = refIndex + 1u) {
            let instanceIndex = tlasInstanceRefs[refIndex];
            if (instanceIndex >= arrayLength(&instances)) {
                continue;
            }
            var seen = false;
            for (var m = 0u; m < mailboxCount; m = m + 1u) {
                if (mailbox[m] == instanceIndex) {
                    seen = true;
                    break;
                }
            }
            if (seen) {
                continue;
            }
            if (mailboxCount < MAILBOX_SIZE) {
                mailbox[mailboxCount] = instanceIndex;
                mailboxCount = mailboxCount + 1u;
            }
            let instance = instances[instanceIndex];
            let instanceT = ray_aabb(origin, direction, instance.worldBoundsMin.xyz, instance.worldBoundsMaxAndConservativeScale.xyz, tMin, best.t);
            if (instanceT.x <= instanceT.y) {
                instanceTests = instanceTests + 1u;
                if (traceInstances) {
                    best = trace_instance(instanceIndex, origin, direction, max(tMin, instanceT.x), best, requireNormal);
                }
            }
        }
        let childMask = node.childAndRefs.x;
        if (childMask == 0u) {
            continue;
        }
        for (var child = 0u; child < 8u; child = child + 1u) {
            if ((childMask & (1u << child)) == 0u) {
                continue;
            }
            var childIndex = INVALID_INDEX;
            if (child < 4u) {
                childIndex = node.childIndices0[child];
            } else {
                childIndex = node.childIndices1[child - 4u];
            }
            if (childIndex >= arrayLength(&tlasNodes)) {
                continue;
            }
            let childNode = tlasNodes[childIndex];
            let childT = ray_aabb(origin, direction, childNode.boundsMin.xyz, childNode.boundsMax.xyz, tMin, best.t);
            if (childT.x <= childT.y && stackCount < 64u) {
                stackNode[stackCount] = childIndex;
                stackTMin[stackCount] = childT.x;
                stackCount = stackCount + 1u;
            }
        }
    }
    best.tlasVisits = tlasVisits;
    best.instanceTests = instanceTests;
    return best;
}

#ifdef SDF_TRACE_DEBUG
const DEBUG_MODE_LOCAL_INSTANCE_RAYMARCH : u32 = 6u;
const DEBUG_MODE_LOCAL_INSTANCE_NORMAL : u32 = 7u;
const DEBUG_MODE_LOCAL_INSTANCE_ALBEDO : u32 = 8u;
const DEBUG_MODE_LOCAL_INSTANCE_CASCADES : u32 = 9u;
const DEBUG_MODE_LOCAL_INSTANCE_CASCADE_INSTANCES : u32 = 10u;
const DEBUG_MODE_LOCAL_INSTANCE_BLINN : u32 = 11u;
const DEBUG_MODE_LOCAL_INSTANCE_STEP_COUNT : u32 = 12u;

fn reconstruct_world_position(uv: vec2<f32>, depthValue: f32) -> vec3<f32> {
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = uniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn instance_debug_color(instanceIndex: u32) -> vec3<f32> {
    let seed = f32((instanceIndex * 1664525u + 1013904223u) & 255u) / 255.0;
    return 0.35 + 0.65 * fract(vec3<f32>(seed + 0.00, seed + 0.37, seed + 0.71));
}

fn heatmap_color(value: f32) -> vec3<f32> {
    let t = clamp(value, 0.0, 1.0);
    let lowColor = clamp(uniforms.traceHeatmapLowColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let midColor = clamp(uniforms.traceHeatmapMidColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let highColor = clamp(uniforms.traceHeatmapHighColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let lowMid = mix(lowColor, midColor, min(t * 2.0, 1.0));
    let midHigh = mix(midColor, highColor, max(t * 2.0 - 1.0, 0.0));
    return select(lowMid, midHigh, t > 0.5);
}

fn trace_count_heatmap(value: f32) -> vec3<f32> {
    let lowCount = max(uniforms.traceHeatmapCounts.x, 0.0);
    let midCount = max(uniforms.traceHeatmapCounts.y, lowCount + 1.0e-3);
    let highCount = max(uniforms.traceHeatmapCounts.z, midCount + 1.0e-3);
    let lowColor = clamp(uniforms.traceHeatmapLowColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let midColor = clamp(uniforms.traceHeatmapMidColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let highColor = clamp(uniforms.traceHeatmapHighColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    if (value <= midCount) {
        let t = clamp((value - lowCount) / max(midCount - lowCount, 1.0e-3), 0.0, 1.0);
        return mix(lowColor, midColor, t);
    }
    let t = clamp((value - midCount) / max(highCount - midCount, 1.0e-3), 0.0, 1.0);
    return mix(midColor, highColor, t);
}

fn tlas_count_heatmap(value: f32) -> vec3<f32> {
    if (value <= 0.0) {
        return vec3<f32>(0.0);
    }
    let midCount = max(uniforms.traceHeatmapCounts.x, 1.0);
    let highCount = max(uniforms.traceHeatmapCounts.y, midCount + 1.0e-3);
    let lowColor = clamp(uniforms.traceHeatmapLowColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let midColor = clamp(uniforms.traceHeatmapMidColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let highColor = clamp(uniforms.traceHeatmapHighColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    if (value <= midCount) {
        let t = clamp(value / midCount, 0.0, 1.0);
        return mix(lowColor, midColor, t);
    }
    let t = clamp((value - midCount) / max(highCount - midCount, 1.0e-3), 0.0, 1.0);
    return mix(midColor, highColor, t);
}

fn full_trace_cost(hit: SDFCandidateHit) -> f32 {
    return f32(hit.steps + hit.tlasVisits + hit.instanceTests);
}

fn shade_debug_hit(hit: SDFCandidateHit, rayDirection: vec3<f32>) -> vec3<f32> {
    if (!hit.hit) {
        if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_STEP_COUNT) {
            return trace_count_heatmap(full_trace_cost(hit));
        }
        if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_CASCADES) {
            return tlas_count_heatmap(f32(hit.tlasVisits));
        }
        if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_CASCADE_INSTANCES) {
            return tlas_count_heatmap(f32(hit.instanceTests));
        }
        return vec3<f32>(0.0);
    }

    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_NORMAL) {
        return hit.worldNormal * 0.5 + vec3<f32>(0.5);
    }
    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_STEP_COUNT) {
        return trace_count_heatmap(full_trace_cost(hit));
    }
    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_CASCADES) {
        return tlas_count_heatmap(f32(hit.tlasVisits));
    }
    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_CASCADE_INSTANCES) {
        return tlas_count_heatmap(f32(hit.instanceTests));
    }

    let baseColor = instance_debug_color(hit.instanceIndex);
    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_ALBEDO) {
        return baseColor;
    }

    let lightDirection = normalize(vec3<f32>(0.35, 0.75, 0.55));
    let ndotl = max(dot(hit.worldNormal, lightDirection), 0.0);
    let diffuse = baseColor * (0.18 + 0.82 * ndotl);
    if (uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_BLINN) {
        let halfVector = normalize(lightDirection - rayDirection);
        let specular = pow(max(dot(hit.worldNormal, halfVector), 0.0), 48.0);
        return diffuse + vec3<f32>(specular);
    }
    return diffuse;
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= uniforms.outputWidth || globalId.y >= uniforms.outputHeight) {
        return;
    }

    let pixel = vec2<u32>(globalId.xy);
    if (uniforms.enabled == 0u) {
        textureStore(outputTexture, pixel, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    let resolution = max(vec2<f32>(f32(uniforms.outputWidth), f32(uniforms.outputHeight)), vec2<f32>(1.0));
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / resolution;
    let rayDepth = select(0.75, 0.25, uniforms.reverseZ != 0u);
    let rayPoint = reconstruct_world_position(uv, rayDepth);
    let rayDirection = normalize(rayPoint - uniforms.cameraPosition);
    let requireNormal = uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_RAYMARCH ||
        uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_NORMAL ||
        uniforms.mode == DEBUG_MODE_LOCAL_INSTANCE_BLINN;
    let traceInstances = uniforms.mode != DEBUG_MODE_LOCAL_INSTANCE_CASCADES &&
        uniforms.mode != DEBUG_MODE_LOCAL_INSTANCE_CASCADE_INSTANCES;
    let hit = trace_scene(uniforms.cameraPosition, rayDirection, 0.001, uniforms.maxTraceDistance, traceInstances, requireNormal);
    textureStore(outputTexture, pixel, vec4<f32>(shade_debug_hit(hit, rayDirection), 1.0));
}
#else
@compute @workgroup_size(64)
fn cs_main(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= uniforms.rayCount) {
        return;
    }
    let rayIndex = uniforms.rayOffset + globalId.x;
    let hitIndex = uniforms.hitOffset + globalId.x;
    if (rayIndex >= arrayLength(&rays) || hitIndex >= arrayLength(&hits)) {
        return;
    }

    let ray = rays[rayIndex];
    var out : TraceHit;
    let rayIsActive = (ray.metadata.x & TRACE_RAY_FLAG_ACTIVE) != 0u;
    let directionLength = length(ray.directionAndMaxT.xyz);
    if (!rayIsActive || directionLength <= 1.0e-8) {
        out.metadata = vec4<u32>(0u, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX);
        hits[hitIndex] = out;
        return;
    }

    let direction = ray.directionAndMaxT.xyz / directionLength;
    let tMin = max(ray.originAndMinT.w, 0.0);
    let tMax = min(ray.directionAndMaxT.w, uniforms.maxTraceDistance);
    let hit = trace_scene(ray.originAndMinT.xyz, direction, tMin, tMax, true, true);
    if (hit.hit) {
        out.worldPositionAndDistance = vec4<f32>(hit.worldPosition, hit.t);
        out.worldNormalAndStepCount = vec4<f32>(hit.worldNormal, f32(hit.steps));
        out.localUVWAndAux = vec4<f32>(hit.localPosition, f32(hit.level));
        out.metadata = vec4<u32>(TRACE_HIT_FLAG_HIT, hit.instanceId, hit.blasIndex, hit.instanceIndex);
        out.traceInfo = vec4<u32>(hit.nodeIndex, hit.brickIndex, hit.level, hit.steps);
        out.surfaceMaterialReference = vec4<u32>(0u);
    } else {
        out.worldPositionAndDistance = vec4<f32>(0.0, 0.0, 0.0, tMax);
        out.worldNormalAndStepCount = vec4<f32>(0.0, 0.0, 1.0, 0.0);
        out.localUVWAndAux = vec4<f32>(0.0);
        out.metadata = vec4<u32>(0u, INVALID_INDEX, INVALID_INDEX, INVALID_INDEX);
        out.traceInfo = vec4<u32>(0u);
        out.surfaceMaterialReference = vec4<u32>(0u);
    }
    hits[hitIndex] = out;
}
#endif
