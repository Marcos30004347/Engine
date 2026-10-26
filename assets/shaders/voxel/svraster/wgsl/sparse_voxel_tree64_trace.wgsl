#include "sparse_voxel_tree64_common.wgsl"

struct SparseVoxelTree64Ray {
    originMaximumDistance : vec4<f32>,
    direction : vec4<f32>,
}

struct SparseVoxelTree64Result {
    globalCell : vec3<i32>,
    status : u32,
    distance : f32,
    skippedDistance : f32,
    queryCount : u32,
    baseCellSteps : u32,
    highestLevel : u32,
    contentGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct Tree64Interval {
    entry : f32,
    exit : f32,
    valid : u32,
}

struct Tree64Cell {
    cell : vec3<i32>,
    valid : u32,
}

struct Tree64ChunkTrace {
    cell : vec3<i32>,
    status : u32,
    distance : f32,
    queryCount : u32,
    nodeVisits : u32,
    highestLevel : u32,
    maximumStack : u32,
}

@group(0) @binding(0) var<storage, read> treeState : SparseVoxelTree64State;
@group(0) @binding(1) var<storage, read> chunks : array<SparseVoxelTree64Chunk>;
@group(0) @binding(2) var<storage, read> nodes : array<SparseVoxelTree64Node>;
#ifdef TREE64_PRIMARY_RAYS
// Primary-ray variant: the ray is derived from the invocation index, so binding
// 3 carries the camera rather than a per-pixel ray array.
@group(0) @binding(3) var<uniform> primaryCamera : SparseVoxelTree64PrimaryCamera;
#else
@group(0) @binding(3) var<storage, read> rays : array<SparseVoxelTree64Ray>;
#endif
@group(0) @binding(4) var<storage, read_write> results : array<SparseVoxelTree64Result>;
#ifdef TREE64_DYNAMIC_LAYER
#ifdef TREE64_STORAGE_DISPATCH
@group(0) @binding(5) var<storage, read> dispatch : SparseVoxelTree64DynamicTraceDispatch;
#else
@group(0) @binding(5) var<uniform> dispatch : SparseVoxelTree64DynamicTraceDispatch;
#endif
@group(0) @binding(6) var<storage, read> dynamicMemberships : array<SparseVoxelTree64DynamicMembership>;
@group(0) @binding(7) var<storage, read> dynamicReferences : array<u32>;
@group(0) @binding(8) var<storage, read> dynamicInstances : array<SparseVoxelTree64DynamicInstance>;
@group(0) @binding(9) var<storage, read> dynamicModels : array<SparseVoxelTree64DynamicModel>;
#ifdef TREE64_DYNAMIC_PADDED_NODE
@group(0) @binding(10) var<storage, read> dynamicNodes : array<vec4<u32>>;
#else
@group(0) @binding(10) var<storage, read> dynamicNodes : array<SparseVoxelTree64Node>;
#endif
struct SparseVoxelTerrainPageState {
    generation : u32,
    currentLOD : u32,
    publishedFrame : u32,
    flags : u32,
}
@group(0) @binding(19) var<storage, read> terrainPageStates :
    array<SparseVoxelTerrainPageState>;
@group(0) @binding(20) var<storage, read> terrainTree64Descriptors :
    array<SparseVoxelTerrainTree64Descriptor>;
@group(0) @binding(21) var<storage, read> terrainTree64NodePageGenerations :
    array<u32>;
#ifdef TREE64_DYNAMIC_PADDED_NODE
@group(0) @binding(22) var<storage, read> terrainTree64PageNodes :
    array<vec4<u32>>;
#else
@group(0) @binding(22) var<storage, read> terrainTree64PageNodes :
    array<SparseVoxelTree64Node>;
#endif
@group(0) @binding(23) var<storage, read>
    terrainTree64MaterialPageGenerations : array<u32>;
@group(0) @binding(24) var<storage, read> terrainTree64PageMaterialCells :
    array<u32>;
#else
#ifdef TREE64_STORAGE_DISPATCH
@group(0) @binding(5) var<storage, read> dispatch : SparseVoxelTree64TraceDispatch;
#else
@group(0) @binding(5) var<uniform> dispatch : SparseVoxelTree64TraceDispatch;
#endif

#endif

#ifdef TREE64_MATERIAL_GEOMETRY
struct SparseVoxelMicroMaterial {
    geometry : u32,
    shading : u32,
}
struct SparseVoxelTree64MaterialCell {
    globalCell : vec3<i32>,
    materialAndFlags : u32,
    surfaceData : u32,
    auxContext : u32,
    regionMappingId : u32,
    reserved1 : u32,
}
struct SparseVoxelTree64MaterialCounters {
    candidateParents : atomic<u32>,
    sidecarProbes : atomic<u32>,
    contextLoads : atomic<u32>,
    interpreterInstructions : atomic<u32>,
    microCellSteps : atomic<u32>,
    carvedParents : atomic<u32>,
    acceptedParents : atomic<u32>,
    reserved0 : atomic<u32>,
}
#define VOXEL_GEOMETRY_IR_ENABLED
#include "voxel_geometry_ir.wgsl"
#include "voxel_geometry_field_cache.wgsl"
#ifdef TREE64_DYNAMIC_LAYER
@group(0) @binding(11) var<storage, read> tree64MaterialCells : array<SparseVoxelTree64MaterialCell>;
@group(0) @binding(12) var<storage, read> tree64MicroMaterials : array<SparseVoxelMicroMaterial>;
@group(0) @binding(13) var<storage, read> voxelGeometryPrograms : array<VoxelGeometryProgram>;
@group(0) @binding(14) var<storage, read> voxelGeometryInstructions : array<VoxelGeometryInstruction>;
@group(0) @binding(15) var<storage, read> voxelGeometryParameters : array<VoxelGeometryParameters>;
@group(0) @binding(16) var<storage, read_write> tree64MaterialCounters : SparseVoxelTree64MaterialCounters;
@group(0) @binding(17) var<storage, read> voxelMaterialFieldDefaults : array<VoxelMaterialFieldDefaults>;
@group(0) @binding(18) var<storage, read> voxelGeometryFieldCache :
    array<SparseVoxelGeometryFieldCacheRecord>;
#else
@group(0) @binding(6) var<storage, read> tree64MaterialCells : array<SparseVoxelTree64MaterialCell>;
@group(0) @binding(7) var<storage, read> tree64MicroMaterials : array<SparseVoxelMicroMaterial>;
@group(0) @binding(8) var<storage, read> voxelGeometryPrograms : array<VoxelGeometryProgram>;
@group(0) @binding(9) var<storage, read> voxelGeometryInstructions : array<VoxelGeometryInstruction>;
@group(0) @binding(10) var<storage, read> voxelGeometryParameters : array<VoxelGeometryParameters>;
@group(0) @binding(11) var<storage, read_write> tree64MaterialCounters : SparseVoxelTree64MaterialCounters;
@group(0) @binding(12) var<storage, read> voxelMaterialFieldDefaults : array<VoxelMaterialFieldDefaults>;
@group(0) @binding(13) var<storage, read> voxelGeometryFieldCache :
    array<SparseVoxelGeometryFieldCacheRecord>;
#endif
#include "sparse_voxel_micro_geometry.wgsl"
#include "sparse_voxel_tree64_material.wgsl"
#endif

#ifdef TREE64_DYNAMIC_LAYER
fn tree64ReadDynamicNode(
    index : u32, terrainPage : bool) -> SparseVoxelTree64Node {
    if (terrainPage) {
#ifdef TREE64_DYNAMIC_PADDED_NODE
        let words = terrainTree64PageNodes[index];
        return SparseVoxelTree64Node(words.x, words.y, words.z);
#else
        return terrainTree64PageNodes[index];
#endif
    }
#ifdef TREE64_DYNAMIC_PADDED_NODE
    let words = dynamicNodes[index];
    return SparseVoxelTree64Node(words.x, words.y, words.z);
#else
    return dynamicNodes[index];
#endif
}
#endif

#ifdef TREE64_FAST_TRAVERSAL
// One u32 ancestor index per possible model Tree64 level and lane. Authored
// octrees support 16 binary levels, which become eight 4-way Tree64 levels.
const TREE64_ANCESTOR_LEVELS = 8u;
const TREE64_TRACE_WORKGROUP_SIZE = 64u;
var<workgroup> tree64AncestorNodes :
    array<u32, TREE64_ANCESTOR_LEVELS * TREE64_TRACE_WORKGROUP_SIZE>;

// Depth-major, lane-minor. Lane-major (lane * levels + depth) gives adjacent
// lanes a stride of eight words, so the whole wave lands on one bank in eight
// and every stack access serializes eightfold. Depth-major makes a wave's
// access to one level fully coalesced, and the stack is only ever indexed at a
// uniform-per-lane depth anyway.
fn tree64SetAncestor(
    localLane : u32,
    depth : u32,
    nodeIndex : u32) {
    tree64AncestorNodes[
        depth * TREE64_TRACE_WORKGROUP_SIZE + localLane] = nodeIndex;
}

fn tree64Ancestor(
    localLane : u32,
    depth : u32) -> u32 {
    return tree64AncestorNodes[
        depth * TREE64_TRACE_WORKGROUP_SIZE + localLane];
}
#endif

const TREE64_STATUS_INVALID = 0u;
const TREE64_STATUS_MISS = 1u;
const TREE64_STATUS_HIT = 2u;
const TREE64_STATUS_QUERY_LIMIT = 3u;
const TREE64_STATUS_UNKNOWN = 4u;
const TREE64_INVALID_REASON_INSTANCE_FLAGS = 1u;
const TREE64_INVALID_REASON_DYNAMIC_GENERATION = 2u;
const TREE64_INVALID_REASON_MODEL_INDEX = 3u;
const TREE64_INVALID_REASON_TERRAIN_STATE = 4u;
const TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_BOUNDS = 5u;
const TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_IDENTITY = 6u;
const TREE64_INVALID_REASON_TERRAIN_NODE_PAGE = 7u;
const TREE64_INVALID_REASON_TERRAIN_MATERIAL_PAGE = 8u;
const TREE64_INVALID_REASON_TLAS_OVERFLOW = 9u;
const TREE64_INFINITY = 3.402823466e+38f;

#ifdef TREE64_PRIMARY_RAYS
// Must match the ray the G-buffer reconstructs for the same pixel, so the
// surface it shades lies on the ray that produced the hit.
fn tree64PrimaryRay(rayIndex : u32) -> SparseVoxelTree64Ray {
    let width = max(1u, primaryCamera.viewport.x);
    let pixel = vec2<u32>(rayIndex % width, rayIndex / width);
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - primaryCamera.focalPrincipal.z) /
            primaryCamera.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - primaryCamera.focalPrincipal.w) /
            primaryCamera.focalPrincipal.y,
        1.0);
    let worldDirection = normalize(
        (primaryCamera.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    return SparseVoxelTree64Ray(
        primaryCamera.originMaximumDistance,
        vec4<f32>(worldDirection, 0.0));
}
#endif

fn tree64NextFloatUp(value : f32) -> f32 {
    if (value == TREE64_INFINITY) {
        return value;
    }
    if (value == 0.0) {
        return bitcast<f32>(0x00000001u);
    }
    let bits = bitcast<u32>(value);
    return bitcast<f32>(select(bits - 1u, bits + 1u, value > 0.0));
}

fn tree64NextFloatDirection(value : f32, positive : bool) -> f32 {
    if (value == 0.0) {
        return select(-bitcast<f32>(1u), bitcast<f32>(1u), positive);
    }
    var bits = bitcast<u32>(value);
    let increment = (value > 0.0) == positive;
    bits = select(bits - 1u, bits + 1u, increment);
    return bitcast<f32>(bits);
}

// Scene visibility compaction stores the receiver face in negative
// direction.w values. Primary/closest-hit callers retain the legacy
// non-negative start-distance representation.
fn tree64SceneRayReceiverFace(metadata : f32) -> u32 {
    if (metadata >= 0.0) { return 0u; }
    return min(u32(floor(max(-metadata - 1.0, 0.0) / 128.0)), 6u);
}

fn tree64SceneRayStartDistance(metadata : f32) -> f32 {
    if (metadata >= 0.0) { return metadata; }
    let packed = max(-metadata - 1.0, 0.0);
    let face = floor(packed / 128.0);
    return max(packed - face * 128.0, 0.0);
}

fn tree64ReceiverFaceNormal(face : u32) -> vec3<f32> {
    if (face == 0u || face > 6u) { return vec3<f32>(0.0); }
    let axis = (face - 1u) >> 1u;
    let positive = ((face - 1u) & 1u) == 0u;
    var normal = vec3<f32>(0.0);
    normal[axis] = select(-1.0, 1.0, positive);
    return normal;
}

fn tree64CanonicalSceneReceiverOrigin(
    ray : SparseVoxelTree64Ray, receiverFace : u32,
    startDistance : f32) -> vec3<f32> {
    return ray.originMaximumDistance.xyz -
        ray.direction.xyz * startDistance;
}

// Reconstruct the authored face plane after transforming to instance-local
// coordinates. At planet-scale world positions, subtracting the page
// translation from a world-f32 receiver leaves up to 3.125 cm of error. A ray
// that should start on top of a 10 cm voxel can consequently start inside the
// coplanar row beside it; skipping only its own cell then reports the adjacent
// cell as an occluder and draws long grid/rectangle shadows. The inward sample
// identifies the discrete receiver cell robustly, so its encoded face gives us
// the exact local boundary without moving the world-space ray or adding bias.
fn tree64CanonicalLocalReceiverOrigin(
    localOrigin : vec3<f32>, localMinimum : vec3<f32>,
    receiverCell : vec3<i32>, receiverCellSize : f32,
    localReceiverNormal : vec3<f32>) -> vec3<f32> {
    let absoluteNormal = abs(localReceiverNormal);
    var axis = 0u;
    if (absoluteNormal.y > absoluteNormal.x) { axis = 1u; }
    if (absoluteNormal.z > absoluteNormal[axis]) { axis = 2u; }
    var result = localOrigin;
    let positive = localReceiverNormal[axis] > 0.0;
    let boundary = f32(receiverCell[axis]) + select(0.0, 1.0, positive);
    result[axis] = localMinimum[axis] + boundary * receiverCellSize;
    return result;
}

fn tree64FloorDivide(value : i32, divisor : i32) -> i32 {
    let quotient = value / divisor;
    let remainder = value % divisor;
    return quotient - select(0, 1, remainder < 0);
}

// Slab test, evaluated on all three axes at once.
//
// Entry is monotonically increasing across axes and exit monotonically
// decreasing, so testing the two against each other once at the end decides
// exactly what the per-axis early-out decided - and the six divisions the
// scalar loop performed collapse into one reciprocal and two vector multiplies.
// Callers only read entry/exit when valid is set, so the intermediate values an
// early-out would have frozen are never observed.
fn tree64IntersectBox(
    ray : SparseVoxelTree64Ray,
    minimum : vec3<f32>,
    maximum : vec3<f32>,
    lower : f32,
    upper : f32) -> Tree64Interval {
    let origin = ray.originMaximumDistance.xyz;
    let direction = ray.direction.xyz;
    let moving = direction != vec3<f32>(0.0);
    let inverse = select(
        vec3<f32>(0.0), vec3<f32>(1.0) / direction, moving);
    let first = (minimum - origin) * inverse;
    let last = (maximum - origin) * inverse;
    let near = select(
        vec3<f32>(-TREE64_INFINITY), min(first, last), moving);
    let far = select(
        vec3<f32>(TREE64_INFINITY), max(first, last), moving);
    var interval = Tree64Interval(
        max(lower, max(near.x, max(near.y, near.z))),
        min(upper, min(far.x, min(far.y, far.z))),
        1u);
    // An axis the ray does not travel along never enters or leaves its slab, so
    // the origin has to already be inside it. Half-open, matching cell
    // ownership everywhere else. Three scalar tests rather than a masked vector
    // one: this runs once per box, not per marching step, and boolean vectors
    // are the part of WGSL that translates least predictably.
    var stationaryOutside = false;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        if (!moving[axis] &&
            (origin[axis] < minimum[axis] ||
             origin[axis] >= maximum[axis])) {
            stationaryOutside = true;
        }
    }
    if (stationaryOutside || !(interval.exit > interval.entry)) {
        interval.valid = 0u;
    }
    return interval;
}

fn tree64IntersectVolume(
    ray : SparseVoxelTree64Ray) -> Tree64Interval {
    let minimum =
        vec3<f32>(treeState.originCell) * treeState.cellSize;
    let maximum =
        (vec3<f32>(treeState.originCell) +
         vec3<f32>(treeState.logicalCellResolution)) *
        treeState.cellSize;
    return tree64IntersectBox(
        ray, minimum, maximum, 0.0, ray.originMaximumDistance.w);
}

fn tree64BaseCellAt(
    ray : SparseVoxelTree64Ray,
    distance : f32) -> Tree64Cell {
    var result = Tree64Cell(vec3<i32>(0), 1u);
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let position =
            ray.originMaximumDistance[axis] +
            ray.direction[axis] * distance;
        let coordinate = position / treeState.cellSize[axis];
        var cell = i32(floor(coordinate));
        if (ray.direction[axis] < 0.0) {
            let nearestBoundary = round(coordinate);
            let tolerance =
                4.0 * 1.1920928955078125e-7 *
                max(1.0, abs(coordinate));
            if (abs(coordinate - nearestBoundary) <= tolerance) {
                cell = i32(nearestBoundary) - 1;
            }
        }
        result.cell[axis] = cell;
        let logical = cell - treeState.originCell[axis];
        if (logical < 0 ||
            logical >= i32(treeState.logicalCellResolution[axis])) {
            result.valid = 0u;
        }
    }
    return result;
}

fn tree64DirectoryIndex(worldChunk : vec3<i32>) -> u32 {
    let logical = worldChunk - treeState.originWorldChunk;
    let physical =
        (vec3<u32>(logical) + treeState.wrapOffset) %
        treeState.logicalChunkResolution;
    return physical.x +
        treeState.logicalChunkResolution.x *
        (physical.y +
         treeState.logicalChunkResolution.y * physical.z);
}

fn tree64ChunkExit(
    ray : SparseVoxelTree64Ray,
    reciprocal : Tree64RayReciprocal,
    worldChunk : vec3<i32>) -> f32 {
    let side = f32(treeState.chunkSide);
    let boundaryCell = select(
        vec3<f32>(worldChunk) * side,
        (vec3<f32>(worldChunk) + vec3<f32>(1.0)) * side,
        ray.direction.xyz > vec3<f32>(0.0));
    return tree64SlabExit(
        boundaryCell * treeState.cellSize,
        ray.originMaximumDistance.xyz,
        reciprocal);
}

struct Tree64RegionLookup {
    cell : vec3<i32>,
    regionOrigin : vec3<u32>,
    regionSize : u32,
    status : u32,
    nodeVisits : u32,
    highestLevel : u32,
}

// Tree64 node extents are powers of four. Expressing origin/child extraction
// as bit operations avoids the vector integer divisions otherwise emitted for
// every visited node (including dynamic BLAS nodes).
fn tree64NodeOrigin(
    localCell : vec3<u32>, nodeSize : u32) -> vec3<u32> {
    return localCell & vec3<u32>(~(nodeSize - 1u));
}

fn tree64ChildCoordinate(
    localCell : vec3<u32>, nodeOrigin : vec3<u32>,
    childSize : u32) -> vec3<u32> {
    return (localCell - nodeOrigin) >>
        vec3<u32>(firstTrailingBit(childSize));
}

// log4(size) - 1, for the powers of four a Tree64 node size can take.
//
// This runs once per node visit, which is the innermost thing the traversal
// does, and it exists only to fill a diagnostic field. The obvious divide-until-
// small loop costs an integer division per level there; sizes are exact powers
// of two, so the trailing-bit index is log2 and the level follows by a shift.
#ifdef TREE64_TRACE_STATISTICS
fn tree64NodeLevel(size : u32) -> u32 {
    return (firstTrailingBit(max(size, 4u)) - 2u) >> 1u;
}
#endif

// Per-axis reciprocal of the ray direction, with the axes the ray does not
// travel along flagged instead of divided.
//
// Slab exits are the innermost arithmetic in the marcher and were three scalar
// divisions each. Hoisting the reciprocal to once per chunk turns every later
// exit into a vector multiply; the validity mask keeps a zero component from
// producing the inf * 0 = NaN that a plain reciprocal would.
struct Tree64RayReciprocal {
    inverseDirection : vec3<f32>,
    // Infinity on the axes the ray does not travel along, zero elsewhere.
    //
    // Added rather than selected on a stored mask: a vec3<bool> struct member
    // does not survive the SPIR-V to MSL translation as a boolean vector, and
    // the additive form needs no mask at all. inverseDirection is zero on a
    // stationary axis, so the product vanishes and the bias alone remains -
    // which is exactly "this axis never bounds the exit".
    stationaryBias : vec3<f32>,
}

fn tree64RayReciprocal(
    direction : vec3<f32>) -> Tree64RayReciprocal {
    let moving = direction != vec3<f32>(0.0);
    return Tree64RayReciprocal(
        select(vec3<f32>(0.0), vec3<f32>(1.0) / direction, moving),
        select(vec3<f32>(TREE64_INFINITY), vec3<f32>(0.0), moving));
}

fn tree64SlabExit(
    boundary : vec3<f32>,
    origin : vec3<f32>,
    reciprocal : Tree64RayReciprocal) -> f32 {
    let distances =
        (boundary - origin) * reciprocal.inverseDirection +
        reciprocal.stationaryBias;
    return min(distances.x, min(distances.y, distances.z));
}

// World size below which a populated region is accepted as a hit at ray
// distance `distance`. Non-positive means "refine to leaves", which is the
// exact pre-LOD behaviour.
fn tree64LODCutoff(distance : f32) -> f32 {
    if (!tree64LODEnabled() || distance < dispatch.lodStartDistance) {
        return 0.0;
    }
    var cutoff = max(dispatch.lodWorldSize, 0.0) +
        max(dispatch.lodDistanceSlope, 0.0) *
        max(distance - max(dispatch.lodStartDistance, 0.0), 0.0);
    if (dispatch.lodMaximumWorldSize > 0.0) {
        cutoff = min(cutoff, dispatch.lodMaximumWorldSize);
    }
    return cutoff;
}

fn tree64LODEnabled() -> bool {
    return dispatch.lodWorldSize > 0.0 ||
        dispatch.lodDistanceSlope > 0.0;
}

fn tree64LocalCellAt(
    ray : SparseVoxelTree64Ray,
    distance : f32,
    side : u32) -> vec3<u32> {
    var cell = vec3<u32>(0u);
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let coordinate =
            ray.originMaximumDistance[axis] +
            ray.direction[axis] * distance;
        var signedCell = i32(floor(coordinate));
        if (ray.direction[axis] < 0.0) {
            let nearestBoundary = round(coordinate);
            let tolerance =
                4.0 * 1.1920928955078125e-7 *
                max(1.0, abs(coordinate));
            if (abs(coordinate - nearestBoundary) <= tolerance) {
                signedCell = i32(nearestBoundary) - 1;
            }
        }
        cell[axis] = u32(clamp(
            signedCell, 0, i32(side) - 1));
    }
    return cell;
}

fn tree64RegionExit(
    ray : SparseVoxelTree64Ray,
    reciprocal : Tree64RayReciprocal,
    origin : vec3<u32>,
    size : u32) -> f32 {
    let boundary = select(
        vec3<f32>(origin),
        vec3<f32>(origin + vec3<u32>(size)),
        ray.direction.xyz > vec3<f32>(0.0));
    return tree64SlabExit(
        boundary, ray.originMaximumDistance.xyz, reciprocal);
}

fn tree64EmptySiblingGroup(
    node : SparseVoxelTree64Node,
    childCoordinate : vec3<u32>) -> bool {
    let groupBase =
        (childCoordinate.x & 2u) +
        4u * (childCoordinate.y & 2u) +
        16u * (childCoordinate.z & 2u);
    let groupMask =
        0x00330033u << (groupBase & 31u);
    return select(
        (node.populationMaskLo & groupMask) == 0u,
        (node.populationMaskHi & groupMask) == 0u,
        groupBase >= 32u);
}

fn tree64LookupRegion(
    descriptor : SparseVoxelTree64Chunk,
    localCell : vec3<u32>,
    lodCellCutoff : f32) -> Tree64RegionLookup {
    var output = Tree64RegionLookup(
        vec3<i32>(localCell),
        vec3<u32>(0u),
        treeState.chunkSide,
        TREE64_STATUS_INVALID,
        0u,
        0u);
    let slotBase =
        descriptor.nodeSlot * dispatch.nodeSlotCapacity;
    var nodeIndex = descriptor.rootNodeOffset;
    var nodeOrigin = vec3<u32>(0u);
    var nodeSize = treeState.chunkSide;

    loop {
        if (nodeSize < 4u ||
            nodeIndex >= descriptor.nodeCount ||
            slotBase + nodeIndex >= dispatch.nodeCapacity) {
            return output;
        }
#ifdef TREE64_SUBGROUP_BROADCAST
        let node = tree64ReadNodeCoherent(slotBase + nodeIndex);
#else
        let node = nodes[slotBase + nodeIndex];
#endif
#ifdef TREE64_TRACE_STATISTICS
        output.nodeVisits += 1u;
        output.highestLevel =
            max(output.highestLevel, tree64NodeLevel(nodeSize));
#endif
        let childSize = nodeSize >> 2u;
        let childCoordinate = tree64ChildCoordinate(
            localCell, nodeOrigin, childSize);
        if (any(childCoordinate >= vec3<u32>(4u))) {
            return output;
        }
        let child =
            childCoordinate.x +
            4u * childCoordinate.y +
            16u * childCoordinate.z;
        output.regionOrigin =
            nodeOrigin + childCoordinate * vec3<u32>(childSize);
        output.regionSize = childSize;
        if (!tree64PopulationContains(node, child)) {
            if (tree64EmptySiblingGroup(
                    node, childCoordinate)) {
                output.regionOrigin =
                    nodeOrigin +
                    (childCoordinate & vec3<u32>(2u)) *
                        vec3<u32>(childSize);
                output.regionSize = childSize * 2u;
            }
            output.status = TREE64_STATUS_MISS;
            return output;
        }
        let terminal =
            (node.childBaseAndFlags &
             TREE64_TERMINAL_FLAG) != 0u;
        // A populated child at or below the LOD cut-off is the answer: refining
        // it can only narrow *where* inside it the surface is, which no
        // consumer of a cut-off this coarse can use.
        if (terminal || f32(childSize) <= lodCellCutoff) {
            output.status = TREE64_STATUS_HIT;
            return output;
        }
        let childBase = node.childBaseAndFlags >> 1u;
        let compactChild =
            childBase +
            tree64PopulationCountBefore(node, child);
        if (childBase == 0u ||
            compactChild >= descriptor.nodeCount) {
            return output;
        }
        nodeIndex = compactChild;
        nodeOrigin = output.regionOrigin;
        nodeSize = childSize;
    }
    return output;
}

// VoxelRT's fast Tree64 path marches the current sparse region instead of
// intersecting all 64 children and sorting a large candidate stack. This
// variant follows that control-flow shape while retaining our half-open cell
// ownership, directory ABI and conservative invalid-state behavior.
fn tree64TraceChunkRegion(
    ray : SparseVoxelTree64Ray,
    descriptor : SparseVoxelTree64Chunk,
    lower : f32,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper,
        0u, 0u, 0u, 0u);
    if ((descriptor.flags & TREE64_CHUNK_OCCUPIED) == 0u) {
        return output;
    }
    if (descriptor.nodeCount == 0u ||
        descriptor.nodeSlot >=
            dispatch.nodeCapacity / dispatch.nodeSlotCapacity) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }
    let slotBase =
        descriptor.nodeSlot * dispatch.nodeSlotCapacity;
    if (descriptor.rootNodeOffset >= descriptor.nodeCount ||
        slotBase + descriptor.nodeCount > dispatch.nodeCapacity) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }

    let chunkCellOrigin =
        descriptor.worldChunk * i32(treeState.chunkSide);
    let localRay = SparseVoxelTree64Ray(
        vec4<f32>(
            ray.originMaximumDistance.xyz / treeState.cellSize -
                vec3<f32>(chunkCellOrigin),
            ray.originMaximumDistance.w),
        vec4<f32>(
            ray.direction.xyz / treeState.cellSize, 0.0));
    let rootInterval = tree64IntersectBox(
        localRay,
        vec3<f32>(0.0),
        vec3<f32>(f32(treeState.chunkSide)),
        lower,
        upper);
    if (rootInterval.valid == 0u) {
        return output;
    }

    let reciprocal = tree64RayReciprocal(localRay.direction.xyz);
    // Cell size is uniform in practice; the smallest component is the
    // conservative reading when it is not.
    let inverseCellSize = 1.0 / max(
        min(treeState.cellSize.x,
            min(treeState.cellSize.y, treeState.cellSize.z)),
        1.0e-9);
    var current = rootInterval.entry;
    loop {
        if (!(current < rootInterval.exit)) {
            break;
        }
        if (output.queryCount >= queryBudget) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = current;
            return output;
        }
        output.queryCount += 1u;
        let localCell = tree64LocalCellAt(
            localRay, current, treeState.chunkSide);
        let lookup = tree64LookupRegion(
            descriptor, localCell,
            tree64LODCutoff(current) * inverseCellSize);
#ifdef TREE64_TRACE_STATISTICS
        output.nodeVisits += lookup.nodeVisits;
        output.highestLevel =
            max(output.highestLevel, lookup.highestLevel);
#endif
        if (lookup.status == TREE64_STATUS_HIT) {
#ifdef TREE64_MATERIAL_GEOMETRY
            // Coarse LOD hits remain conservatively occupied. Exact leaves can
            // be rejected, in which case the region marcher resumes after the
            // carved parent instead of terminating the ray.
            if (lookup.regionSize == 1u) {
                let globalCell = chunkCellOrigin + vec3<i32>(localCell);
                let materialHit = tree64ResolveMaterialLeaf(
                    ray, globalCell, current, rootInterval.exit);
                if (!materialHit.accepted) {
                    current = tree64NextFloatUp(materialHit.parentExit);
                    continue;
                }
                output.status = TREE64_STATUS_HIT;
                output.cell = vec3<i32>(localCell);
                output.distance = materialHit.distance;
                return output;
            }
#endif
            output.status = TREE64_STATUS_HIT;
            output.cell = lookup.cell;
            output.distance = current;
            return output;
        }
        if (lookup.status == TREE64_STATUS_INVALID) {
            output.status = TREE64_STATUS_INVALID;
            output.distance = current;
            return output;
        }
        let regionExit = min(
            rootInterval.exit,
            tree64RegionExit(
                localRay,
                reciprocal,
                lookup.regionOrigin,
                lookup.regionSize));
        if (!(regionExit > current)) {
            current = tree64NextFloatUp(current);
        } else {
            current = tree64NextFloatUp(regionExit);
        }
    }
    output.distance = rootInterval.exit;
    return output;
}

#ifdef TREE64_FAST_TRAVERSAL
fn tree64MirroredRay(
    ray : SparseVoxelTree64Ray,
    side : u32) -> SparseVoxelTree64Ray {
    let mirror = ray.direction.xyz > vec3<f32>(0.0);
    return SparseVoxelTree64Ray(
        vec4<f32>(
            select(
                ray.originMaximumDistance.xyz,
                vec3<f32>(f32(side)) -
                    ray.originMaximumDistance.xyz,
                mirror),
            ray.originMaximumDistance.w),
        vec4<f32>(-abs(ray.direction.xyz), 0.0));
}

fn tree64UnmirrorCell(
    mirroredCell : vec3<u32>,
    mirror : vec3<bool>,
    side : u32) -> vec3<u32> {
    return select(
        mirroredCell,
        vec3<u32>(side - 1u) - mirroredCell,
        mirror);
}

fn tree64MirrorRegionOrigin(
    origin : vec3<u32>,
    size : u32,
    mirror : vec3<bool>,
    side : u32) -> vec3<u32> {
    return select(
        origin,
        vec3<u32>(side) -
            (origin + vec3<u32>(size)),
        mirror);
}

fn tree64RegionExitNegative(
    ray : SparseVoxelTree64Ray,
    reciprocal : Tree64RayReciprocal,
    origin : vec3<u32>) -> f32 {
    return tree64SlabExit(
        vec3<f32>(origin), ray.originMaximumDistance.xyz, reciprocal);
}

fn tree64TraceChunkFast(
    ray : SparseVoxelTree64Ray,
    descriptor : SparseVoxelTree64Chunk,
    lower : f32,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper,
        0u, 0u, 0u, 0u);
    if ((descriptor.flags & TREE64_CHUNK_OCCUPIED) == 0u) {
        return output;
    }
    if (descriptor.nodeCount == 0u ||
        descriptor.nodeSlot >=
            dispatch.nodeCapacity / dispatch.nodeSlotCapacity) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }
    let slotBase =
        descriptor.nodeSlot * dispatch.nodeSlotCapacity;
    if (descriptor.rootNodeOffset >= descriptor.nodeCount ||
        slotBase + descriptor.nodeCount > dispatch.nodeCapacity) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }

    let chunkCellOrigin =
        descriptor.worldChunk * i32(treeState.chunkSide);
    let localRay = SparseVoxelTree64Ray(
        vec4<f32>(
            ray.originMaximumDistance.xyz / treeState.cellSize -
                vec3<f32>(chunkCellOrigin),
            ray.originMaximumDistance.w),
        vec4<f32>(
            ray.direction.xyz / treeState.cellSize, 0.0));
    let rootInterval = tree64IntersectBox(
        localRay,
        vec3<f32>(0.0),
        vec3<f32>(f32(treeState.chunkSide)),
        lower,
        upper);
    if (rootInterval.valid == 0u) {
        return output;
    }

    let mirror = localRay.direction.xyz > vec3<f32>(0.0);
    let mirroredRay =
        tree64MirroredRay(localRay, treeState.chunkSide);
    let reciprocal = tree64RayReciprocal(mirroredRay.direction.xyz);
    let inverseCellSize = 1.0 / max(
        min(treeState.cellSize.x,
            min(treeState.cellSize.y, treeState.cellSize.z)),
        1.0e-9);
    var current = rootInterval.entry;
    var nodeIndex = descriptor.rootNodeOffset;
    var nodeSize = treeState.chunkSide;
    var depth = 0u;
    tree64SetAncestor(localLane, 0u, nodeIndex);
#ifdef TREE64_TRACE_STATISTICS
    output.maximumStack = 1u;
#endif
    // Carried across the step so the cell for `current` is derived once. The
    // pop-up phase already had to compute it to decide how far to unwind, and
    // the top of the loop then recomputed the identical value - two floor/round
    // boundary resolutions per axis per step, for one step's worth of answer.
    var localCell = tree64UnmirrorCell(
        tree64LocalCellAt(mirroredRay, current, treeState.chunkSide),
        mirror,
        treeState.chunkSide);

    loop {
        if (!(current < rootInterval.exit)) {
            break;
        }
        if (output.queryCount >= queryBudget) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = current;
            return output;
        }
        output.queryCount += 1u;
        let lodCellCutoff = tree64LODCutoff(current) * inverseCellSize;

        var regionOrigin = vec3<u32>(0u);
        var regionSize = 0u;
        loop {
            if (nodeSize < 4u ||
                nodeIndex >= descriptor.nodeCount ||
                slotBase + nodeIndex >= dispatch.nodeCapacity) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
#ifdef TREE64_SUBGROUP_BROADCAST
            let node = tree64ReadNodeCoherent(slotBase + nodeIndex);
#else
            let node = nodes[slotBase + nodeIndex];
#endif
#ifdef TREE64_TRACE_STATISTICS
            output.nodeVisits += 1u;
            output.highestLevel = max(
                output.highestLevel,
                tree64NodeLevel(nodeSize));
#endif
            let nodeOrigin = tree64NodeOrigin(localCell, nodeSize);
            let childSize = nodeSize >> 2u;
            let childCoordinate = tree64ChildCoordinate(
                localCell, nodeOrigin, childSize);
            if (any(childCoordinate >= vec3<u32>(4u))) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            let child =
                childCoordinate.x +
                4u * childCoordinate.y +
                16u * childCoordinate.z;
            regionOrigin =
                nodeOrigin +
                childCoordinate * vec3<u32>(childSize);
            regionSize = childSize;

            if (!tree64PopulationContains(node, child)) {
                if (tree64EmptySiblingGroup(
                        node, childCoordinate)) {
                    regionOrigin =
                        nodeOrigin +
                        (childCoordinate & vec3<u32>(2u)) *
                            vec3<u32>(childSize);
                    regionSize = childSize * 2u;
                }
                break;
            }
            let terminal =
                (node.childBaseAndFlags &
                 TREE64_TERMINAL_FLAG) != 0u;
            if (terminal || f32(childSize) <= lodCellCutoff) {
#ifdef TREE64_MATERIAL_GEOMETRY
                if (childSize == 1u) {
                    let globalCell =
                        chunkCellOrigin + vec3<i32>(localCell);
                    let materialHit = tree64ResolveMaterialLeaf(
                        ray, globalCell, current, rootInterval.exit);
                    if (!materialHit.accepted) {
                        regionOrigin =
                            vec3<u32>(localCell);
                        regionSize = 1u;
                        break;
                    }
                    output.status = TREE64_STATUS_HIT;
                    output.cell = vec3<i32>(localCell);
                    output.distance = materialHit.distance;
                    return output;
                }
#endif
                output.status = TREE64_STATUS_HIT;
                output.cell = vec3<i32>(localCell);
                output.distance = current;
                return output;
            }

            let childBase =
                node.childBaseAndFlags >> 1u;
            let compactChild =
                childBase +
                tree64PopulationCountBefore(node, child);
            if (childBase == 0u ||
                compactChild >= descriptor.nodeCount ||
                depth + 1u >= TREE64_ANCESTOR_LEVELS) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            tree64SetAncestor(localLane, depth, nodeIndex);
            depth += 1u;
            nodeIndex = compactChild;
            nodeSize = childSize;
            tree64SetAncestor(localLane, depth, nodeIndex);
#ifdef TREE64_TRACE_STATISTICS
            output.maximumStack =
                max(output.maximumStack, depth + 1u);
#endif
        }

        let mirroredRegionOrigin =
            tree64MirrorRegionOrigin(
                regionOrigin,
                regionSize,
                mirror,
                treeState.chunkSide);
        let regionExit = min(
            rootInterval.exit,
            tree64RegionExitNegative(
                mirroredRay, reciprocal, mirroredRegionOrigin));
        if (!(regionExit > current)) {
            current = tree64NextFloatUp(current);
        } else {
            current = tree64NextFloatUp(regionExit);
        }
        if (!(current < rootInterval.exit)) {
            break;
        }

        let nextLocalCell = tree64UnmirrorCell(
            tree64LocalCellAt(
                mirroredRay, current, treeState.chunkSide),
            mirror,
            treeState.chunkSide);
        loop {
            let nodeOrigin = tree64NodeOrigin(localCell, nodeSize);
            if (all(nextLocalCell >= nodeOrigin) &&
                all(nextLocalCell <
                    nodeOrigin + vec3<u32>(nodeSize))) {
                break;
            }
            if (depth == 0u) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            depth -= 1u;
            nodeSize <<= 2u;
            nodeIndex =
                tree64Ancestor(localLane, depth);
        }
        localCell = nextLocalCell;
    }
    output.distance = rootInterval.exit;
    return output;
}
#endif

fn tree64TraceChunk(
    ray : SparseVoxelTree64Ray,
    descriptor : SparseVoxelTree64Chunk,
    lower : f32,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
#ifdef TREE64_FAST_TRAVERSAL
    return tree64TraceChunkFast(
        ray, descriptor, lower, upper, queryBudget, localLane);
#else
    return tree64TraceChunkRegion(
        ray, descriptor, lower, upper, queryBudget, localLane);
#endif
}

#ifdef TREE64_DYNAMIC_LAYER
fn tree64LookupDynamicRegion(
    model : SparseVoxelTree64DynamicModel,
    localCell : vec3<u32>,
    lodCellCutoff : f32,
    terrainPage : bool) -> Tree64RegionLookup {
    var output = Tree64RegionLookup(
        vec3<i32>(localCell),
        vec3<u32>(0u),
        model.side,
        TREE64_STATUS_INVALID,
        0u,
        0u);
    var nodeIndex = model.rootNodeOffset;
    var nodeOrigin = vec3<u32>(0u);
    var nodeSize = model.side;
    let resolvedNodeCapacity = select(
        dispatch.dynamicNodeCount,
        arrayLength(&terrainTree64PageNodes), terrainPage);

    loop {
        if (nodeSize < 4u ||
            nodeIndex >= model.nodeCount ||
            model.nodeOffset + nodeIndex >=
                resolvedNodeCapacity) {
            return output;
        }
        let node = tree64ReadDynamicNode(
            model.nodeOffset + nodeIndex, terrainPage);
#ifdef TREE64_TRACE_STATISTICS
        output.nodeVisits += 1u;
        output.highestLevel =
            max(output.highestLevel, tree64NodeLevel(nodeSize));
#endif
        let childSize = nodeSize >> 2u;
        let childCoordinate = tree64ChildCoordinate(
            localCell, nodeOrigin, childSize);
        if (any(childCoordinate >= vec3<u32>(4u))) {
            return output;
        }
        let child =
            childCoordinate.x +
            4u * childCoordinate.y +
            16u * childCoordinate.z;
        output.regionOrigin =
            nodeOrigin + childCoordinate * vec3<u32>(childSize);
        output.regionSize = childSize;
        if (!tree64PopulationContains(node, child)) {
            if (tree64EmptySiblingGroup(
                    node, childCoordinate)) {
                output.regionOrigin =
                    nodeOrigin +
                    (childCoordinate & vec3<u32>(2u)) *
                        vec3<u32>(childSize);
                output.regionSize = childSize * 2u;
            }
            output.status = TREE64_STATUS_MISS;
            return output;
        }
        let terminal =
            (node.childBaseAndFlags &
             TREE64_TERMINAL_FLAG) != 0u;
        if (terminal || f32(childSize) <= lodCellCutoff) {
            output.status = TREE64_STATUS_HIT;
            return output;
        }
        let childBase = node.childBaseAndFlags >> 1u;
        let compactChild =
            childBase +
            tree64PopulationCountBefore(node, child);
        if (childBase == 0u ||
            compactChild >= model.nodeCount) {
            return output;
        }
        nodeIndex = compactChild;
        nodeOrigin = output.regionOrigin;
        nodeSize = childSize;
    }
    return output;
}

fn tree64TraceDynamicInstanceRegion(
    ray : SparseVoxelTree64Ray,
    instance : SparseVoxelTree64DynamicInstance,
    model : SparseVoxelTree64DynamicModel,
    terrainPage : bool,
    materialPageSlot : u32,
    materialCellsPerPage : u32,
    materialCellOffset : u32,
    materialCellCount : u32,
    worldInterval : Tree64Interval,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper,
        0u, 0u, 0u, 0u);
    let resolvedNodeCapacity = select(
        dispatch.dynamicNodeCount,
        arrayLength(&terrainTree64PageNodes), terrainPage);
    if ((model.flags & TREE64_DYNAMIC_VALID) == 0u ||
        model.nodeCount == 0u ||
        model.side == 0u ||
        any(model.localCellSize <= vec3<f32>(0.0)) ||
        model.rootNodeOffset >= model.nodeCount ||
        model.nodeOffset > resolvedNodeCapacity ||
        model.nodeCount >
            resolvedNodeCapacity - model.nodeOffset) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }
    // A pure translation needs three adds on the origin and leaves the
    // direction untouched. Terrain pages are all placed this way, so the
    // general path below was a 4x4 multiply per ray per instance whose
    // direction half returned its own input.
    let translationOnly =
        (instance.flags & TREE64_DYNAMIC_TRANSLATION_ONLY) != 0u;
    let localOrigin = select(
        (instance.worldToModel *
         vec4<f32>(ray.originMaximumDistance.xyz, 1.0)).xyz,
        ray.originMaximumDistance.xyz + instance.worldToModel[3].xyz,
        translationOnly);
    let localDirection = select(
        (instance.worldToModel *
         vec4<f32>(ray.direction.xyz, 0.0)).xyz,
        ray.direction.xyz,
        translationOnly);
    let requestedReceiverCellSize =
        bitcast<f32>(instance.receiverCellSizeBits);
    let receiverCellSize = select(
        model.localCellSize.x,
        requestedReceiverCellSize,
        requestedReceiverCellSize > 0.0);
    // The traversal origin remains exactly on the rendered receiver. Identify
    // its authored cell by moving halfway inward along the encoded face
    // normal, independently of the stochastic light direction. Half a cell is
    // deliberately larger than planet-scale world-f32 quantization (3.125 cm
    // in the captured view) while remaining inside a 10 cm receiver.
    let receiverStartDistance =
        tree64SceneRayStartDistance(ray.direction.w);
    let receiverFace = tree64SceneRayReceiverFace(ray.direction.w);
    let hasReceiverFace = receiverFace > 0u;
    let canonicalWorldReceiver = tree64CanonicalSceneReceiverOrigin(
        ray, receiverFace, receiverStartDistance);
    let localReceiverOrigin = select(
        (instance.worldToModel *
         vec4<f32>(canonicalWorldReceiver, 1.0)).xyz,
        canonicalWorldReceiver + instance.worldToModel[3].xyz,
        translationOnly);
    let worldReceiverNormal = tree64ReceiverFaceNormal(receiverFace);
    let localReceiverNormalUnnormalized = select(
        (instance.worldToModel *
         vec4<f32>(worldReceiverNormal, 0.0)).xyz,
        worldReceiverNormal,
        translationOnly);
    let localReceiverNormalLength = length(
        localReceiverNormalUnnormalized);
    let localReceiverNormal = select(
        vec3<f32>(0.0),
        localReceiverNormalUnnormalized /
            max(localReceiverNormalLength, 1.0e-8),
        localReceiverNormalLength > 1.0e-8);
    let receiverIdentityOrigin = localReceiverOrigin -
        localReceiverNormal * (receiverCellSize * 0.5);
    let receiverDirectionCells = localDirection / receiverCellSize;
    let receiverRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (receiverIdentityOrigin - model.localMinimum) /
                receiverCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(receiverDirectionCells, 0.0));
    let skipReceiverCell =
        (instance.flags & TREE64_DYNAMIC_SKIP_RECEIVER_CELL) != 0u &&
        all(receiverIdentityOrigin >= model.localMinimum) &&
        all(receiverIdentityOrigin <
            model.localMinimum + vec3<f32>(f32(model.side)) *
                model.localCellSize);
    let receiverCell = vec3<i32>(floor(
        receiverRay.originMaximumDistance.xyz));
    let traversalLocalOrigin = select(
        localOrigin,
        tree64CanonicalLocalReceiverOrigin(
            localOrigin, model.localMinimum, receiverCell,
            receiverCellSize, localReceiverNormal),
        skipReceiverCell && hasReceiverFace);
    let localRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (traversalLocalOrigin - model.localMinimum) /
                model.localCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(
            localDirection / model.localCellSize, 0.0));
    let receiverTraversalRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (traversalLocalOrigin - model.localMinimum) / receiverCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(receiverDirectionCells, 0.0));
    let receiverModelCell = vec3<u32>(floor(
        (receiverIdentityOrigin - model.localMinimum) /
            model.localCellSize));
    let rootInterval = tree64IntersectBox(
        localRay,
        vec3<f32>(0.0),
        vec3<f32>(f32(model.side)),
        worldInterval.entry,
        worldInterval.exit);
    if (rootInterval.valid == 0u) {
        return output;
    }

    let reciprocal = tree64RayReciprocal(localRay.direction.xyz);
    let inverseCellSize = 1.0 / max(
        min(model.localCellSize.x,
            min(model.localCellSize.y, model.localCellSize.z)),
        1.0e-9);
    var current = rootInterval.entry;
    if (skipReceiverCell) {
        // Begin just beyond the rendered receiver voxel, which can be smaller
        // than a conservatively OR-reduced Tree64 leaf.
        let receiverExit = tree64RegionExit(
            receiverTraversalRay,
            tree64RayReciprocal(receiverTraversalRay.direction.xyz),
            vec3<u32>(receiverCell), 1u);
        // Tree64 may conservatively reduce several raster receivers into one
        // leaf. After leaving the exact receiver, immediately testing that
        // same leaf reports its coplanar neighbours as blockers and projects
        // the leaf grid onto a flat surface. For a face-tagged surface ray the
        // direction is in the receiver hemisphere, so no authored occluder can
        // lie between that face and the outward boundary of its containing
        // Tree64 cell. Escape both representations before testing occupancy.
        let modelReceiverExit = select(
            receiverExit,
            tree64RegionExit(
                localRay, tree64RayReciprocal(localRay.direction.xyz),
                receiverModelCell, 1u),
            hasReceiverFace);
        let receiverRepresentationExit = max(
            receiverExit, modelReceiverExit);
        if (receiverRepresentationExit > current) {
            current = tree64NextFloatUp(min(
                rootInterval.exit, receiverRepresentationExit));
        }
    }
    loop {
        if (!(current < rootInterval.exit)) {
            break;
        }
        if (output.queryCount >= queryBudget) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = current;
            return output;
        }
        output.queryCount += 1u;
        let localCell = tree64LocalCellAt(
            localRay, current, model.side);
        let lookup = tree64LookupDynamicRegion(
            model, localCell,
            tree64LODCutoff(current) * inverseCellSize,
            terrainPage);
#ifdef TREE64_TRACE_STATISTICS
        output.nodeVisits += lookup.nodeVisits;
        output.highestLevel =
            max(output.highestLevel, lookup.highestLevel);
#endif
        if (lookup.status == TREE64_STATUS_HIT) {
#ifdef TREE64_MATERIAL_GEOMETRY
            if (lookup.regionSize == 1u) {
                let materialHit = tree64ResolveDynamicMaterialLeaf(
                    localRay, model.modelId, lookup.cell,
                    terrainPage, materialPageSlot,
                    materialCellsPerPage, materialCellOffset,
                    materialCellCount,
                    current, rootInterval.exit);
                if (!materialHit.accepted) {
                    current = tree64NextFloatUp(materialHit.parentExit);
                    continue;
                }
                output.status = TREE64_STATUS_HIT;
                output.cell = lookup.cell;
                output.distance = materialHit.distance;
                return output;
            }
#endif
            output.status = TREE64_STATUS_HIT;
            output.cell = lookup.cell;
            output.distance = current;
            return output;
        }
        if (lookup.status == TREE64_STATUS_INVALID) {
            output.status = TREE64_STATUS_INVALID;
            output.distance = current;
            return output;
        }
        let regionExit = min(
            rootInterval.exit,
            tree64RegionExit(
                localRay,
                reciprocal,
                lookup.regionOrigin,
                lookup.regionSize));
        if (!(regionExit > current)) {
            current = tree64NextFloatUp(current);
        } else {
            current = tree64NextFloatUp(regionExit);
        }
    }
    output.distance = rootInterval.exit;
    return output;
}

#ifdef TREE64_FAST_TRAVERSAL
fn tree64TraceDynamicInstanceFast(
    ray : SparseVoxelTree64Ray,
    instance : SparseVoxelTree64DynamicInstance,
    model : SparseVoxelTree64DynamicModel,
    terrainPage : bool,
    materialPageSlot : u32,
    materialCellsPerPage : u32,
    materialCellOffset : u32,
    materialCellCount : u32,
    worldInterval : Tree64Interval,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper,
        0u, 0u, 0u, 0u);
    let resolvedNodeCapacity = select(
        dispatch.dynamicNodeCount,
        arrayLength(&terrainTree64PageNodes), terrainPage);
    if ((model.flags & TREE64_DYNAMIC_VALID) == 0u ||
        model.nodeCount == 0u ||
        model.side == 0u ||
        any(model.localCellSize <= vec3<f32>(0.0)) ||
        model.rootNodeOffset >= model.nodeCount ||
        model.nodeOffset > resolvedNodeCapacity ||
        model.nodeCount >
            resolvedNodeCapacity - model.nodeOffset) {
        output.status = TREE64_STATUS_INVALID;
        return output;
    }
    // A pure translation needs three adds on the origin and leaves the
    // direction untouched. Terrain pages are all placed this way, so the
    // general path below was a 4x4 multiply per ray per instance whose
    // direction half returned its own input.
    let translationOnly =
        (instance.flags & TREE64_DYNAMIC_TRANSLATION_ONLY) != 0u;
    let localOrigin = select(
        (instance.worldToModel *
         vec4<f32>(ray.originMaximumDistance.xyz, 1.0)).xyz,
        ray.originMaximumDistance.xyz + instance.worldToModel[3].xyz,
        translationOnly);
    let localDirection = select(
        (instance.worldToModel *
         vec4<f32>(ray.direction.xyz, 0.0)).xyz,
        ray.direction.xyz,
        translationOnly);
    let requestedReceiverCellSize =
        bitcast<f32>(instance.receiverCellSizeBits);
    let receiverCellSize = select(
        model.localCellSize.x,
        requestedReceiverCellSize,
        requestedReceiverCellSize > 0.0);
    // See the region traversal above: classify the canonical face receiver
    // from the middle of its cell, not from the outgoing ray direction.
    let receiverStartDistance =
        tree64SceneRayStartDistance(ray.direction.w);
    let receiverFace = tree64SceneRayReceiverFace(ray.direction.w);
    let hasReceiverFace = receiverFace > 0u;
    let canonicalWorldReceiver = tree64CanonicalSceneReceiverOrigin(
        ray, receiverFace, receiverStartDistance);
    let localReceiverOrigin = select(
        (instance.worldToModel *
         vec4<f32>(canonicalWorldReceiver, 1.0)).xyz,
        canonicalWorldReceiver + instance.worldToModel[3].xyz,
        translationOnly);
    let worldReceiverNormal = tree64ReceiverFaceNormal(receiverFace);
    let localReceiverNormalUnnormalized = select(
        (instance.worldToModel *
         vec4<f32>(worldReceiverNormal, 0.0)).xyz,
        worldReceiverNormal,
        translationOnly);
    let localReceiverNormalLength = length(
        localReceiverNormalUnnormalized);
    let localReceiverNormal = select(
        vec3<f32>(0.0),
        localReceiverNormalUnnormalized /
            max(localReceiverNormalLength, 1.0e-8),
        localReceiverNormalLength > 1.0e-8);
    let receiverIdentityOrigin = localReceiverOrigin -
        localReceiverNormal * (receiverCellSize * 0.5);
    let receiverDirectionCells = localDirection / receiverCellSize;
    let receiverRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (receiverIdentityOrigin - model.localMinimum) /
                receiverCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(receiverDirectionCells, 0.0));
    let skipReceiverCell =
        (instance.flags & TREE64_DYNAMIC_SKIP_RECEIVER_CELL) != 0u &&
        all(receiverIdentityOrigin >= model.localMinimum) &&
        all(receiverIdentityOrigin <
            model.localMinimum + vec3<f32>(f32(model.side)) *
                model.localCellSize);
    let receiverCell = vec3<i32>(floor(
        receiverRay.originMaximumDistance.xyz));
    let traversalLocalOrigin = select(
        localOrigin,
        tree64CanonicalLocalReceiverOrigin(
            localOrigin, model.localMinimum, receiverCell,
            receiverCellSize, localReceiverNormal),
        skipReceiverCell && hasReceiverFace);
    let localRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (traversalLocalOrigin - model.localMinimum) /
                model.localCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(
            localDirection / model.localCellSize, 0.0));
    let receiverTraversalRay = SparseVoxelTree64Ray(
        vec4<f32>(
            (traversalLocalOrigin - model.localMinimum) / receiverCellSize,
            ray.originMaximumDistance.w),
        vec4<f32>(receiverDirectionCells, 0.0));
    let receiverModelCell = vec3<u32>(floor(
        (receiverIdentityOrigin - model.localMinimum) /
            model.localCellSize));
    let rootInterval = tree64IntersectBox(
        localRay,
        vec3<f32>(0.0),
        vec3<f32>(f32(model.side)),
        worldInterval.entry,
        worldInterval.exit);
    if (rootInterval.valid == 0u) {
        return output;
    }

    let mirror = localRay.direction.xyz > vec3<f32>(0.0);
    let mirroredRay =
        tree64MirroredRay(localRay, model.side);
    let reciprocal = tree64RayReciprocal(mirroredRay.direction.xyz);
    let inverseCellSize = 1.0 / max(
        min(model.localCellSize.x,
            min(model.localCellSize.y, model.localCellSize.z)),
        1.0e-9);
    var current = rootInterval.entry;
    if (skipReceiverCell) {
        // Use the unmirrored ray for the receiver leaf exit. The regular
        // mirrored traversal resumes immediately afterward.
        let receiverExit = tree64RegionExit(
            receiverTraversalRay,
            tree64RayReciprocal(receiverTraversalRay.direction.xyz),
            vec3<u32>(receiverCell), 1u);
        let modelReceiverExit = select(
            receiverExit,
            tree64RegionExit(
                localRay, tree64RayReciprocal(localRay.direction.xyz),
                receiverModelCell, 1u),
            hasReceiverFace);
        let receiverRepresentationExit = max(
            receiverExit, modelReceiverExit);
        if (receiverRepresentationExit > current) {
            current = tree64NextFloatUp(min(
                rootInterval.exit, receiverRepresentationExit));
        }
    }
    var nodeIndex = model.rootNodeOffset;
    var nodeSize = model.side;
    var depth = 0u;
    tree64SetAncestor(localLane, 0u, nodeIndex);
#ifdef TREE64_TRACE_STATISTICS
    output.maximumStack = 1u;
#endif
    var localCell = tree64UnmirrorCell(
        tree64LocalCellAt(mirroredRay, current, model.side),
        mirror,
        model.side);

    loop {
        if (!(current < rootInterval.exit)) {
            break;
        }
        if (output.queryCount >= queryBudget) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = current;
            return output;
        }
        output.queryCount += 1u;
        let lodCellCutoff = tree64LODCutoff(current) * inverseCellSize;

        var regionOrigin = vec3<u32>(0u);
        var regionSize = 0u;
        loop {
            if (nodeSize < 4u ||
                nodeIndex >= model.nodeCount ||
                model.nodeOffset + nodeIndex >=
                    resolvedNodeCapacity) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            let node = tree64ReadDynamicNode(
                model.nodeOffset + nodeIndex, terrainPage);
#ifdef TREE64_TRACE_STATISTICS
            output.nodeVisits += 1u;
            output.highestLevel = max(
                output.highestLevel,
                tree64NodeLevel(nodeSize));
#endif
            let nodeOrigin = tree64NodeOrigin(localCell, nodeSize);
            let childSize = nodeSize >> 2u;
            let childCoordinate = tree64ChildCoordinate(
                localCell, nodeOrigin, childSize);
            if (any(childCoordinate >= vec3<u32>(4u))) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            let child =
                childCoordinate.x +
                4u * childCoordinate.y +
                16u * childCoordinate.z;
            regionOrigin =
                nodeOrigin +
                childCoordinate * vec3<u32>(childSize);
            regionSize = childSize;

            if (!tree64PopulationContains(node, child)) {
                if (tree64EmptySiblingGroup(
                        node, childCoordinate)) {
                    regionOrigin =
                        nodeOrigin +
                        (childCoordinate & vec3<u32>(2u)) *
                            vec3<u32>(childSize);
                    regionSize = childSize * 2u;
                }
                break;
            }
            let terminal =
                (node.childBaseAndFlags &
                 TREE64_TERMINAL_FLAG) != 0u;
            if (terminal || f32(childSize) <= lodCellCutoff) {
#ifdef TREE64_MATERIAL_GEOMETRY
                if (childSize == 1u) {
                    let materialHit = tree64ResolveDynamicMaterialLeaf(
                        localRay, model.modelId, vec3<i32>(localCell),
                        terrainPage, materialPageSlot,
                        materialCellsPerPage, materialCellOffset,
                        materialCellCount,
                        current, rootInterval.exit);
                    if (!materialHit.accepted) {
                        regionOrigin = vec3<u32>(localCell);
                        regionSize = 1u;
                        break;
                    }
                    output.status = TREE64_STATUS_HIT;
                    output.cell = vec3<i32>(localCell);
                    output.distance = materialHit.distance;
                    return output;
                }
#endif
                output.status = TREE64_STATUS_HIT;
                output.cell = vec3<i32>(localCell);
                output.distance = current;
                return output;
            }

            let childBase =
                node.childBaseAndFlags >> 1u;
            let compactChild =
                childBase +
                tree64PopulationCountBefore(node, child);
            if (childBase == 0u ||
                compactChild >= model.nodeCount ||
                depth + 1u >= TREE64_ANCESTOR_LEVELS) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            tree64SetAncestor(localLane, depth, nodeIndex);
            depth += 1u;
            nodeIndex = compactChild;
            nodeSize = childSize;
            tree64SetAncestor(localLane, depth, nodeIndex);
#ifdef TREE64_TRACE_STATISTICS
            output.maximumStack =
                max(output.maximumStack, depth + 1u);
#endif
        }

        let mirroredRegionOrigin =
            tree64MirrorRegionOrigin(
                regionOrigin,
                regionSize,
                mirror,
                model.side);
        let regionExit = min(
            rootInterval.exit,
            tree64RegionExitNegative(
                mirroredRay, reciprocal, mirroredRegionOrigin));
        if (!(regionExit > current)) {
            current = tree64NextFloatUp(current);
        } else {
            current = tree64NextFloatUp(regionExit);
        }
        if (!(current < rootInterval.exit)) {
            break;
        }

        let nextLocalCell = tree64UnmirrorCell(
            tree64LocalCellAt(mirroredRay, current, model.side),
            mirror,
            model.side);
        loop {
            let nodeOrigin = tree64NodeOrigin(localCell, nodeSize);
            if (all(nextLocalCell >= nodeOrigin) &&
                all(nextLocalCell <
                    nodeOrigin + vec3<u32>(nodeSize))) {
                break;
            }
            if (depth == 0u) {
                output.status = TREE64_STATUS_INVALID;
                output.distance = current;
                return output;
            }
            depth -= 1u;
            nodeSize <<= 2u;
            nodeIndex =
                tree64Ancestor(localLane, depth);
        }
        localCell = nextLocalCell;
    }
    output.distance = rootInterval.exit;
    return output;
}
#endif

fn tree64TraceDynamicInstance(
    ray : SparseVoxelTree64Ray,
    instanceIndex : u32,
    lower : f32,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper,
        0u, 0u, 0u, 0u);

    // Do not pass a storage-buffer instance value across the function boundary
    // until its compact publication fields and world bounds accept the ray.
    // A dynamic instance is 128 bytes because it carries a full world-to-model
    // matrix. The global visibility path examines every published terrain page,
    // and materializing that complete value before the AABB rejection makes a
    // miss load transform data it cannot use. Field-addressed loads keep misses
    // to the publication metadata and two bounds vectors. The complete instance
    // is fetched only for the small candidate set that reaches model traversal.
    let instanceFlags = dynamicInstances[instanceIndex].flags;
    let instanceGeneration =
        dynamicInstances[instanceIndex].contentGeneration;
    let modelIndex = dynamicInstances[instanceIndex].modelIndex;
    let terrainGeneration =
        dynamicInstances[instanceIndex].terrainGeneration;
    let terrainPageInstance = terrainGeneration != 0u;
    if ((instanceFlags & TREE64_DYNAMIC_VALID) == 0u) {
        // Stable terrain arrays are indexed by physical slot and therefore
        // contain zero-filled holes while the asynchronous chunk window is
        // still being admitted.  A never-published slot is empty space, not
        // unknown residency.  Returning INVALID here poisons the complete
        // global any-hit ray at the first hole and leaves screen-probe tiles on
        // their black default until that unrelated slot happens to publish.
        // Non-zero invalid records remain corruption/stale data and preserve
        // the conservative UNKNOWN path below.
        if (instanceGeneration == 0u) {
            output.status = TREE64_STATUS_MISS;
        } else {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack = TREE64_INVALID_REASON_INSTANCE_FLAGS;
        }
        return output;
    }
#ifdef TREE64_ANY_HIT_GLOBAL_DYNAMIC
    // Parent and child terrain pages overlap briefly so raster ownership can
    // switch without a hole. The publication transaction marks the retiring
    // parent with this bit at the exact instant its children become traceable;
    // otherwise conservative parent occupancy fabricates a frame of almost
    // fully occluded lighting which temporal accumulation spreads over time.
    if ((instanceFlags & TREE64_DYNAMIC_DISABLE_ANY_HIT) != 0u) {
        return output;
    }
#endif
    if (!terrainPageInstance &&
        instanceGeneration != dispatch.dynamicGeneration) {
        output.status = TREE64_STATUS_INVALID;
        output.maximumStack = TREE64_INVALID_REASON_DYNAMIC_GENERATION;
        return output;
    }
    if (modelIndex >= dispatch.modelCount) {
        output.status = TREE64_STATUS_INVALID;
        output.maximumStack = TREE64_INVALID_REASON_MODEL_INDEX;
        return output;
    }
    let worldMinimum =
        dynamicInstances[instanceIndex].worldBoundsMinimum.xyz;
    let worldMaximum =
        dynamicInstances[instanceIndex].worldBoundsMaximum.xyz;
    let worldInterval = tree64IntersectBox(
        ray, worldMinimum, worldMaximum, lower, upper);
    if (worldInterval.valid == 0u) {
        return output;
    }
    var model = dynamicModels[modelIndex];
    var terrainPage = false;
    var materialPageSlot = 0u;
    var materialCellsPerPage = 0u;
    var materialCellOffset = 0u;
    var materialCellCount = 0u;
    if (terrainGeneration != 0u) {
        let terrainSlot = dynamicInstances[instanceIndex].terrainSlot;
        if (terrainSlot >= arrayLength(&terrainPageStates)) {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack = TREE64_INVALID_REASON_TERRAIN_STATE;
            return output;
        }
        if (terrainPageStates[terrainSlot].generation !=
                terrainGeneration) {
            // Stable terrain slots outlive individual page owners. Once the
            // page-state generation no longer names this instance, the slot is
            // retired or has moved to a newer owner. Treat the old instance as
            // empty space for lighting instead of poisoning the whole any-hit
            // ray as UNKNOWN_RESIDENCY. The current owner is still guarded
            // below by descriptor/page-generation checks.
            output.status = TREE64_STATUS_MISS;
            return output;
        }
        if (terrainSlot >= arrayLength(&terrainTree64Descriptors)) {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack =
                TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_BOUNDS;
            return output;
        }
        let descriptor = terrainTree64Descriptors[terrainSlot];
        let nodePageCount =
            arrayLength(&terrainTree64NodePageGenerations);
        let materialPageCount =
            arrayLength(&terrainTree64MaterialPageGenerations);
        let nodePoolCount = arrayLength(&terrainTree64PageNodes);
        let materialPoolWordCount =
            arrayLength(&terrainTree64PageMaterialCells);
        var nodesPerPage = 0u;
        if (nodePageCount != 0u) {
            nodesPerPage = nodePoolCount / nodePageCount;
        }
        materialCellsPerPage = 0u;
        if (materialPageCount != 0u &&
            materialPageCount <= 0x1fffffffu) {
            materialCellsPerPage =
                materialPoolWordCount / (materialPageCount * 8u);
        }
        if ((descriptor.flags & TREE64_TERRAIN_PAGE_VALID) == 0u ||
            descriptor.terrainSlot != terrainSlot ||
            descriptor.terrainGeneration != terrainGeneration ||
            descriptor.internalLOD !=
                terrainPageStates[terrainSlot].currentLOD) {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack =
                TREE64_INVALID_REASON_TERRAIN_DESCRIPTOR_IDENTITY;
            return output;
        }
        if (descriptor.nodePageGeneration == 0u ||
            descriptor.nodePageSlot >= nodePageCount ||
            terrainTree64NodePageGenerations[
                descriptor.nodePageSlot] !=
                descriptor.nodePageGeneration ||
            nodesPerPage == 0u ||
            nodePoolCount % nodePageCount != 0u ||
            descriptor.nodeCount == 0u ||
            descriptor.nodeCount > nodesPerPage ||
            descriptor.rootNodeOffset >= descriptor.nodeCount) {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack = TREE64_INVALID_REASON_TERRAIN_NODE_PAGE;
            return output;
        }
        if (descriptor.materialPageGeneration == 0u ||
            descriptor.materialPageSlot >= materialPageCount ||
            terrainTree64MaterialPageGenerations[
                descriptor.materialPageSlot] !=
                descriptor.materialPageGeneration ||
            materialCellsPerPage == 0u ||
            materialPoolWordCount % (materialPageCount * 8u) != 0u ||
            descriptor.materialCellOffset > materialCellsPerPage ||
            descriptor.materialCellCount >
                materialCellsPerPage - descriptor.materialCellOffset) {
            output.status = TREE64_STATUS_INVALID;
            output.maximumStack =
                TREE64_INVALID_REASON_TERRAIN_MATERIAL_PAGE;
            return output;
        }
#ifdef TREE64_ANY_HIT_GLOBAL_DYNAMIC
        // The rasterizer intersects an analytic height patch inside each
        // generated terrain voxel. Tree64 stores conservative occupancy only,
        // so coarse streaming pages are not valid binary shadow/GI occluders:
        // their full cells create dark terraces and dotted silhouettes that do
        // not exist on the visible surface. Admit them once the source cell is
        // fine enough to approximate that surface.
        let terrainReceiverCellSize = bitcast<f32>(
            dynamicInstances[instanceIndex].receiverCellSizeBits);
        if (terrainReceiverCellSize > 1.0) {
            return output;
        }
#endif
        model.nodeOffset = descriptor.nodePageSlot * nodesPerPage;
        model.rootNodeOffset = descriptor.rootNodeOffset;
        model.nodeCount = descriptor.nodeCount;
        model.side = descriptor.modelSide;
        model.localMinimum = vec3<f32>(0.0);
        model.localCellSize = vec3<f32>(
            bitcast<f32>(descriptor.modelCellSizeBits));
        // GPU-owned terrain slots are allocated after startup and therefore
        // do not have a CPU-built dynamic-model record.  The page descriptor
        // above is the complete model for these slots: it supplies the
        // physical nodes, root, side, and cell size.  Leaving the bootstrap
        // model's zero flags in place makes the otherwise valid reconstructed
        // model fail immediately in tree64TraceDynamicInstance*(), turning
        // every intersecting lighting ray into UNKNOWN_RESIDENCY once terrain
        // refines below the coarse-page lighting cutoff.
        model.flags = TREE64_DYNAMIC_VALID;
        materialPageSlot = descriptor.materialPageSlot;
        materialCellOffset = descriptor.materialCellOffset;
        materialCellCount = descriptor.materialCellCount;
        terrainPage = true;
    }

    let instance = dynamicInstances[instanceIndex];
#ifdef TREE64_FAST_TRAVERSAL
    return tree64TraceDynamicInstanceFast(
        ray, instance, model, terrainPage, materialPageSlot,
        materialCellsPerPage, materialCellOffset, materialCellCount,
        worldInterval, upper, queryBudget, localLane);
#else
    return tree64TraceDynamicInstanceRegion(
        ray, instance, model, terrainPage, materialPageSlot,
        materialCellsPerPage, materialCellOffset, materialCellCount,
        worldInterval, upper, queryBudget, localLane);
#endif
}
#endif

#ifdef TREE64_TLAS_BVH4
// BVH4 broad phase, replacing the clipmap directory march.
//
// The directory is a uniform grid, so an instance costs one reference per cell
// it touches: the shipping 16 km cut reports 677970 references for 930
// instances because one cell size (64-128 m) serves pages spanning 12.8 m to
// 13 km. A BVH stores each instance once, and that gap only widens once
// instances stop being a regular grid of similar-sized pages.
//
// This descent preserves the grid marcher's useful property: candidates are
// enumerated front-to-back, so an any-hit ray still tests the nearest plausible
// occluder first and can stop without letting a distant page mask a nearer
// invalid publication or query limit.
//
// Layout matches SoftwareTLASManager::BVH4NodeGPU exactly: childMeta is the
// child kind (0 empty, 1 instance leaf, 2 interior) and childIndex is either an
// instance index or a node index.
struct SparseVoxelTree64Bvh4Node {
    childMinX : vec4<f32>,
    childMinY : vec4<f32>,
    childMinZ : vec4<f32>,
    childIndex : vec4<u32>,
    childMaxX : vec4<f32>,
    childMaxY : vec4<f32>,
    childMaxZ : vec4<f32>,
    childMeta : vec4<u32>,
}

@group(0) @binding(30) var<storage, read> tlasBvh4Nodes :
    array<SparseVoxelTree64Bvh4Node>;

// TLAS instance index -> Tree64 dynamic instance index.
//
// These are two different numbering schemes and the leaf index is the wrong
// one. The builder compacts its input, so a BVH4 leaf names a position among
// the instances it accepted; the Tree64 dynamic layer places every terrain page
// at its own *stable slot*, which is sparse by construction and does not move
// when a neighbouring page streams out. Indexing dynamicInstances with the leaf
// index directly traverses a real, wrong page: it traces cleanly, reports hits,
// and puts every shadow in the wrong place.
@group(0) @binding(31) var<storage, read> tlasInstanceSlots : array<u32>;

const TREE64_TLAS_STACK_SIZE : u32 = 32u;
const TREE64_TLAS_CHILD_EMPTY : u32 = 0u;
const TREE64_TLAS_CHILD_INSTANCE : u32 = 1u;
const TREE64_TLAS_CHILD_INTERIOR : u32 = 2u;

struct SparseVoxelTree64TlasEntry {
    kind : u32,
    index : u32,
    nearT : f32,
    farT : f32,
}

fn tree64TraceTlasBvh4(
    ray : SparseVoxelTree64Ray,
    worldReciprocal : Tree64RayReciprocal,
    lower : f32,
    upper : f32,
    queryBudget : u32,
    localLane : u32) -> Tree64ChunkTrace {
    var output = Tree64ChunkTrace(
        vec3<i32>(0), TREE64_STATUS_MISS, upper, 0u, 0u, 0u, 0u);
    if (dispatch.tlasNodeCount == 0u ||
        dispatch.tlasRootNode >= dispatch.tlasNodeCount ||
        dispatch.tlasRootNode >= arrayLength(&tlasBvh4Nodes)) {
        return output;
    }

    var stack : array<SparseVoxelTree64TlasEntry, TREE64_TLAS_STACK_SIZE>;
    var stackPointer = 1u;
    stack[0] = SparseVoxelTree64TlasEntry(
        TREE64_TLAS_CHILD_INTERIOR, dispatch.tlasRootNode, lower, upper);
    // Every exit is a `break` with a single trailing `return`. Returning from
    // inside a `loop` that never breaks makes naga reject the whole function
    // with only "Function is invalid" to go on.
    loop {
        if (stackPointer == 0u) { break; }
        stackPointer -= 1u;
        let entry = stack[stackPointer];
        if (entry.kind == TREE64_TLAS_CHILD_INSTANCE) {
            if (entry.index >= arrayLength(&tlasInstanceSlots)) { continue; }
            let instanceIndex = tlasInstanceSlots[entry.index];
            if (instanceIndex >= dispatch.instanceCount) { continue; }
            if (output.queryCount >= queryBudget) {
                output.status = TREE64_STATUS_QUERY_LIMIT;
                break;
            }
            let instanceResult = tree64TraceDynamicInstance(
                ray, instanceIndex, lower, upper,
                queryBudget - output.queryCount, localLane);
            output.queryCount += instanceResult.queryCount;
#ifdef TREE64_TRACE_STATISTICS
            output.nodeVisits += instanceResult.nodeVisits;
#endif
            // Fail closed on an untrustworthy publication, exactly as the
            // directory path does: it reads as fully shadowed rather than as
            // empty space. Any hit ends a visibility ray.
            if (instanceResult.status == TREE64_STATUS_INVALID ||
                instanceResult.status == TREE64_STATUS_HIT) {
                output.status = TREE64_STATUS_HIT;
                output.cell = instanceResult.cell;
                output.distance = instanceResult.distance;
                break;
            }
            if (instanceResult.status == TREE64_STATUS_QUERY_LIMIT) {
                output.status = TREE64_STATUS_QUERY_LIMIT;
                output.distance = instanceResult.distance;
                break;
            }
            continue;
        }
        if (entry.index >= arrayLength(&tlasBvh4Nodes)) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            break;
        }
        let node = tlasBvh4Nodes[entry.index];
#ifdef TREE64_TRACE_STATISTICS
        output.nodeVisits += 1u;
#endif
        // Slab test against all four children at once. The exit-plane form
        // matches tree64SlabExit's convention so a stationary axis cannot
        // produce inf * 0.
        let origin = ray.originMaximumDistance.xyz;
        let inverse = worldReciprocal.inverseDirection;
        let bias = worldReciprocal.stationaryBias;
        let tx0 = (node.childMinX - vec4<f32>(origin.x)) * vec4<f32>(inverse.x) - vec4<f32>(bias.x);
        let tx1 = (node.childMaxX - vec4<f32>(origin.x)) * vec4<f32>(inverse.x) + vec4<f32>(bias.x);
        let ty0 = (node.childMinY - vec4<f32>(origin.y)) * vec4<f32>(inverse.y) - vec4<f32>(bias.y);
        let ty1 = (node.childMaxY - vec4<f32>(origin.y)) * vec4<f32>(inverse.y) + vec4<f32>(bias.y);
        let tz0 = (node.childMinZ - vec4<f32>(origin.z)) * vec4<f32>(inverse.z) - vec4<f32>(bias.z);
        let tz1 = (node.childMaxZ - vec4<f32>(origin.z)) * vec4<f32>(inverse.z) + vec4<f32>(bias.z);
        let nearT = max(
            max(max(min(tx0, tx1), min(ty0, ty1)), min(tz0, tz1)),
            vec4<f32>(lower));
        let farT = min(
            min(min(max(tx0, tx1), max(ty0, ty1)), max(tz0, tz1)),
            vec4<f32>(upper));

        // Vectors are copied into addressable arrays: dynamic indexing of a
        // vec4 is what the reference traversal avoids by unrolling.
        var childKinds = array<u32, 4>(
            node.childMeta.x, node.childMeta.y,
            node.childMeta.z, node.childMeta.w);
        var childIndices = array<u32, 4>(
            node.childIndex.x, node.childIndex.y,
            node.childIndex.z, node.childIndex.w);
        var childNear = array<f32, 4>(nearT.x, nearT.y, nearT.z, nearT.w);
        var childFar = array<f32, 4>(farT.x, farT.y, farT.z, farT.w);

        var candidates : array<SparseVoxelTree64TlasEntry, 4>;
        var candidateCount = 0u;
        for (var child = 0u; child < 4u; child += 1u) {
            let kind = childKinds[child];
            if (kind == TREE64_TLAS_CHILD_EMPTY) { continue; }
            if (childNear[child] > childFar[child]) { continue; }
            candidates[candidateCount] = SparseVoxelTree64TlasEntry(
                kind, childIndices[child], childNear[child], childFar[child]);
            candidateCount += 1u;
        }

        // Sort the at-most-four children by entry distance. They are pushed in
        // reverse order below so the stack pops the nearest candidate first.
        for (var i = 1u; i < candidateCount; i += 1u) {
            let key = candidates[i];
            var j = i;
            loop {
                if (j == 0u || candidates[j - 1u].nearT <= key.nearT) {
                    break;
                }
                candidates[j] = candidates[j - 1u];
                j -= 1u;
            }
            candidates[j] = key;
        }

        var pushIndex = candidateCount;
        loop {
            if (pushIndex == 0u) { break; }
            pushIndex -= 1u;
            if (stackPointer >= TREE64_TLAS_STACK_SIZE) {
                // Overflow fails closed too; silently dropping a subtree is a
                // missing shadow that no counter would show.
                output.status = TREE64_STATUS_QUERY_LIMIT;
                output.maximumStack = TREE64_INVALID_REASON_TLAS_OVERFLOW;
                break;
            }
            stack[stackPointer] = candidates[pushIndex];
            stackPointer += 1u;
        }
        if (output.status == TREE64_STATUS_QUERY_LIMIT) { break; }
    }
    return output;
}
#endif

@compute @workgroup_size(64, 1, 1)
fn traceTree64(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>) {
    let rayIndex = globalId.x;
    if (rayIndex >= dispatch.rayCount) {
        return;
    }
#ifdef TREE64_PRIMARY_RAYS
    let ray = tree64PrimaryRay(rayIndex);
#else
    let ray = rays[rayIndex];
#endif
    var output = SparseVoxelTree64Result(
        vec3<i32>(0), TREE64_STATUS_INVALID,
        0.0, 0.0, 0u, 0u, 0u,
        treeState.contentGeneration, 0u, 0u);
    let finiteInputs =
        all(abs(ray.originMaximumDistance.xyz) <=
            vec3<f32>(TREE64_INFINITY)) &&
        all(abs(ray.direction.xyz) <=
            vec3<f32>(TREE64_INFINITY));
    if (treeState.valid == 0u ||
        treeState.chunkSide == 0u ||
        any(treeState.logicalChunkResolution == vec3<u32>(0u)) ||
        any(treeState.logicalCellResolution == vec3<u32>(0u)) ||
        any(treeState.cellSize <= vec3<f32>(0.0)) ||
        dispatch.maximumQueries == 0u ||
        dispatch.nodeSlotCapacity == 0u ||
        !finiteInputs ||
        ray.originMaximumDistance.w < 0.0 ||
        all(ray.direction.xyz == vec3<f32>(0.0))) {
        results[rayIndex] = output;
        return;
    }
    let volumeInterval = tree64IntersectVolume(ray);
    if (volumeInterval.valid == 0u) {
        output.status = TREE64_STATUS_MISS;
        results[rayIndex] = output;
        return;
    }

#ifdef TREE64_DYNAMIC_LAYER
#ifdef TREE64_ANY_HIT_GLOBAL_DYNAMIC
    // Visibility rays only need to know whether anything blocks the segment.
    // Testing the committed instances once over the complete volume interval
    // avoids looking up chunk membership and retracing the same instance in
    // every world chunk its bounds overlap. Closest-hit/primary variants keep
    // the chunk-interleaved path below because they must preserve hit order.
    for (var instanceIndex = 0u;
         instanceIndex < dispatch.instanceCount;
         instanceIndex += 1u) {
        if (output.queryCount >= dispatch.maximumQueries) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = volumeInterval.entry;
            results[rayIndex] = output;
            return;
        }
        let dynamicResult = tree64TraceDynamicInstance(
            ray,
            instanceIndex,
            volumeInterval.entry,
            volumeInterval.exit,
            dispatch.maximumQueries - output.queryCount,
            localId.x);
        output.queryCount += dynamicResult.queryCount;
#ifdef TREE64_TRACE_STATISTICS
        output.baseCellSteps += dynamicResult.nodeVisits;
        output.highestLevel = max(
            output.highestLevel, dynamicResult.highestLevel);
        output.reserved1 = max(
            output.reserved1, dynamicResult.maximumStack);
#endif
        if (dynamicResult.status == TREE64_STATUS_INVALID) {
            // Unpublishable dynamic data means "unknown", and unknown must not
            // be reported as "occluded". This branch used to fabricate a HIT at
            // the volume entry - fail-closed - and that is measurably the
            // renderer's episodic light blink: a fabricated hit is
            // indistinguishable downstream from a real occluder, and a screen
            // probe rebuilds its whole hemisphere from one frame of rays, so
            // the moment a cohort of rays takes this branch the probe field
            // darkens scene-wide and stays dark until the data publishes again.
            //
            // Measured on `--test-camera-interior` over 400 settled frames: the
            // scene's mean probe irradiance collapsed by 2.9x for tens of
            // frames at a time and recovered, and the collapse is what the
            // per-probe flicker figure was picking up. Propagating INVALID
            // instead takes the worst 20-frame window from 0.0023 to 0.0052 and
            // the mean from 0.0054 to 0.0067.
            //
            // INVALID is what the sibling static/chunk path already propagates.
            // It merges to UNKNOWN_RESIDENCY in `sparse_voxel_scene_hit_merge`,
            // which every consumer treats as "keep history" - the correct
            // answer for a ray whose occlusion genuinely is not known this
            // frame.
            output.status = TREE64_STATUS_INVALID;
            output.reserved1 = dynamicResult.maximumStack;
            let invalidCell = tree64BaseCellAt(
                ray, volumeInterval.entry);
            output.globalCell = invalidCell.cell;
            output.distance = volumeInterval.entry;
            results[rayIndex] = output;
            return;
        }
        if (dynamicResult.status == TREE64_STATUS_QUERY_LIMIT) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = dynamicResult.distance;
            results[rayIndex] = output;
            return;
        }
        if (dynamicResult.status == TREE64_STATUS_HIT) {
            output.status = TREE64_STATUS_HIT;
            let dynamicCell = tree64BaseCellAt(
                ray, dynamicResult.distance);
            output.globalCell = dynamicCell.cell;
            output.distance = dynamicResult.distance;
            results[rayIndex] = output;
            return;
        }
    }
#endif
#endif

    let worldReciprocal = tree64RayReciprocal(ray.direction.xyz);
#ifdef TREE64_TLAS_BVH4
    // A published BVH4 replaces the directory march entirely. Terrain keeps no
    // static chunk content - the static directory is empty by design in dynamic
    // mode - so the cell march exists only to find instances, which is exactly
    // what the BVH does without the grid's per-cell reference replication.
    // A zero node count means nothing has been published yet, and the directory
    // march below still runs.
    if (dispatch.tlasNodeCount != 0u) {
        let tlasResult = tree64TraceTlasBvh4(
            ray, worldReciprocal, volumeInterval.entry, volumeInterval.exit,
            dispatch.maximumQueries, localId.x);
        output.queryCount += tlasResult.queryCount;
#ifdef TREE64_TRACE_STATISTICS
        output.baseCellSteps += tlasResult.nodeVisits;
#endif
        output.status = tlasResult.status;
        output.globalCell = tlasResult.cell;
        output.distance = tlasResult.distance;
        results[rayIndex] = output;
        return;
    }
#endif
    var current = volumeInterval.entry;
    loop {
        if (current > volumeInterval.exit) {
            break;
        }
        let cellResult = tree64BaseCellAt(ray, current);
        if (cellResult.valid == 0u) {
            break;
        }
        let worldChunk = vec3<i32>(
            tree64FloorDivide(
                cellResult.cell.x, i32(treeState.chunkSide)),
            tree64FloorDivide(
                cellResult.cell.y, i32(treeState.chunkSide)),
            tree64FloorDivide(
                cellResult.cell.z, i32(treeState.chunkSide)));
        let logicalChunk = worldChunk - treeState.originWorldChunk;
        if (any(logicalChunk < vec3<i32>(0)) ||
            any(logicalChunk >=
                vec3<i32>(treeState.logicalChunkResolution))) {
            output.status = TREE64_STATUS_INVALID;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        let directoryIndex = tree64DirectoryIndex(worldChunk);
        if (directoryIndex >= dispatch.chunkCapacity) {
            output.status = TREE64_STATUS_INVALID;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        let descriptor = chunks[directoryIndex];
        if ((descriptor.flags & TREE64_CHUNK_VALID) == 0u ||
            any(descriptor.worldChunk != worldChunk)) {
            output.status = TREE64_STATUS_INVALID;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }

        let chunkExit = min(
            volumeInterval.exit,
            tree64ChunkExit(ray, worldReciprocal, worldChunk));
        if ((descriptor.flags & TREE64_CHUNK_UNKNOWN) != 0u) {
#ifdef TREE64_STORAGE_DISPATCH
            output.status = TREE64_STATUS_UNKNOWN;
#else
            output.status = TREE64_STATUS_HIT;
#endif
            output.globalCell = cellResult.cell;
            output.distance = current;
            output.reserved0 += 1u;
            results[rayIndex] = output;
            return;
        }
        let remainingQueries =
            dispatch.maximumQueries - output.queryCount;
        let chunkResult = tree64TraceChunk(
            ray,
            descriptor,
            current,
            chunkExit,
            remainingQueries,
            localId.x);
        output.queryCount += chunkResult.queryCount;
#ifdef TREE64_TRACE_STATISTICS
        output.baseCellSteps += chunkResult.nodeVisits;
        output.highestLevel =
            max(output.highestLevel, chunkResult.highestLevel);
#endif
        output.reserved0 += 1u;
#ifdef TREE64_TRACE_STATISTICS
        output.reserved1 =
            max(output.reserved1, chunkResult.maximumStack);
#endif
#ifdef TREE64_DYNAMIC_LAYER
#ifdef TREE64_ANY_HIT_GLOBAL_DYNAMIC
        if (chunkResult.status == TREE64_STATUS_INVALID) {
            output.status = TREE64_STATUS_HIT;
            output.globalCell = cellResult.cell;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        if (chunkResult.status == TREE64_STATUS_QUERY_LIMIT) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = chunkResult.distance;
            results[rayIndex] = output;
            return;
        }
#else
        if (chunkResult.status == TREE64_STATUS_INVALID) {
            output.status = TREE64_STATUS_HIT;
            output.globalCell = cellResult.cell;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        if (chunkResult.status == TREE64_STATUS_QUERY_LIMIT) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = chunkResult.distance;
            results[rayIndex] = output;
            return;
        }
        if (directoryIndex >= dispatch.membershipCapacity) {
            output.status = TREE64_STATUS_HIT;
            output.globalCell = cellResult.cell;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        let membership = dynamicMemberships[directoryIndex];
        if ((membership.flags & TREE64_DYNAMIC_VALID) == 0u ||
            any(membership.worldChunk != worldChunk) ||
            membership.contentGeneration !=
                dispatch.dynamicGeneration ||
            membership.referenceCount >
                dispatch.maximumReferencesPerChunk ||
            membership.firstReference >
                dispatch.referenceCount ||
            membership.referenceCount >
                dispatch.referenceCount -
                    membership.firstReference ||
            (membership.flags & TREE64_DYNAMIC_UNKNOWN) != 0u) {
            output.status = TREE64_STATUS_INVALID;
            output.globalCell = cellResult.cell;
            output.distance = current;
            results[rayIndex] = output;
            return;
        }
        var nearestDynamic = chunkExit;
        var dynamicHit = false;
        if (chunkResult.status == TREE64_STATUS_HIT) {
            nearestDynamic = chunkResult.distance;
        }
        for (var referenceOffset = 0u;
             referenceOffset < membership.referenceCount;
             referenceOffset += 1u) {
            if (output.queryCount >= dispatch.maximumQueries) {
                output.status = TREE64_STATUS_QUERY_LIMIT;
                output.distance = current;
                results[rayIndex] = output;
                return;
            }
            let referenceIndex =
                membership.firstReference + referenceOffset;
            let instanceIndex =
                dynamicReferences[referenceIndex];
            if (instanceIndex >= dispatch.instanceCount) {
                output.status = TREE64_STATUS_INVALID;
                output.globalCell = cellResult.cell;
                output.distance = current;
                results[rayIndex] = output;
                return;
            }
            let remainingDynamicQueries =
                dispatch.maximumQueries - output.queryCount;
            let dynamicResult = tree64TraceDynamicInstance(
                ray,
                instanceIndex,
                current,
                nearestDynamic,
                remainingDynamicQueries,
                localId.x);
            output.queryCount += dynamicResult.queryCount;
#ifdef TREE64_TRACE_STATISTICS
            output.baseCellSteps += dynamicResult.nodeVisits;
            output.reserved1 = max(
                output.reserved1,
                dynamicResult.maximumStack);
#endif
            if (dynamicResult.status == TREE64_STATUS_INVALID) {
                output.status = TREE64_STATUS_INVALID;
                output.globalCell = cellResult.cell;
                output.distance = current;
                results[rayIndex] = output;
                return;
            }
            if (dynamicResult.status == TREE64_STATUS_QUERY_LIMIT) {
                output.status = TREE64_STATUS_QUERY_LIMIT;
                output.distance = dynamicResult.distance;
                results[rayIndex] = output;
                return;
            }
            if (dynamicResult.status == TREE64_STATUS_HIT) {
                nearestDynamic = dynamicResult.distance;
                dynamicHit = true;
            }
        }
        if (dynamicHit) {
            output.status = TREE64_STATUS_HIT;
            let dynamicCell =
                tree64BaseCellAt(ray, nearestDynamic);
            output.globalCell = dynamicCell.cell;
            output.distance = nearestDynamic;
            results[rayIndex] = output;
            return;
        }
#endif
#endif
        if (chunkResult.status == TREE64_STATUS_HIT) {
            output.status = TREE64_STATUS_HIT;
            output.globalCell =
                worldChunk * i32(treeState.chunkSide) +
                chunkResult.cell;
            output.distance = chunkResult.distance;
            results[rayIndex] = output;
            return;
        }
        if (chunkResult.status == TREE64_STATUS_INVALID ||
            chunkResult.status == TREE64_STATUS_QUERY_LIMIT) {
            output.status = chunkResult.status;
            output.distance = chunkResult.distance;
            results[rayIndex] = output;
            return;
        }
        output.skippedDistance += chunkExit - current;
        if (output.queryCount >= dispatch.maximumQueries) {
            output.status = TREE64_STATUS_QUERY_LIMIT;
            output.distance = chunkExit;
            results[rayIndex] = output;
            return;
        }
        current = tree64NextFloatUp(max(current, chunkExit));
    }
    output.status = TREE64_STATUS_MISS;
    output.distance = volumeInterval.exit;
    results[rayIndex] = output;
}
