struct SparseVoxelTree64Node {
    childBaseAndFlags : u32,
    populationMaskLo : u32,
    populationMaskHi : u32,
#ifdef TREE64_PADDED_NODE
    reserved0 : u32,
#endif
}

struct SparseVoxelTree64Chunk {
    worldChunk : vec3<i32>,
    nodeSlot : u32,
    rootNodeOffset : u32,
    nodeCount : u32,
    contentGeneration : u32,
    flags : u32,
}

// Stable logical terrain owner -> immutable node/material page mapping. This
// is published only after the replacement pages and matching raster payload
// are complete; traces validate both terrain and page generations.
struct SparseVoxelTerrainTree64Descriptor {
    terrainSlot : u32,
    terrainGeneration : u32,
    internalLOD : u32,
    flags : u32,

    nodePageSlot : u32,
    nodePageGeneration : u32,
    rootNodeOffset : u32,
    nodeCount : u32,

    materialPageSlot : u32,
    materialPageGeneration : u32,
    materialCellOffset : u32,
    materialCellCount : u32,

    geometryRevisionLo : u32,
    geometryRevisionHi : u32,
    modelSide : u32,
    modelCellSizeBits : u32,
}

struct SparseVoxelTree64State {
    logicalChunkResolution : vec3<u32>,
    chunkSide : u32,
    originWorldChunk : vec3<i32>,
    valid : u32,
    wrapOffset : vec3<u32>,
    contentGeneration : u32,
    originCell : vec3<i32>,
    reserved0 : u32,
    logicalCellResolution : vec3<u32>,
    reserved1 : u32,
    cellSize : vec3<f32>,
    reserved2 : u32,
    worldExtent : vec3<f32>,
    overflowStage : u32,
}

// Camera for primary-ray variants that derive their ray from the invocation
// index instead of reading a per-pixel ray array. Generating a full-resolution
// ray buffer and reading it straight back in the next pass is pure bandwidth;
// the ray is four ALU ops.
struct SparseVoxelTree64PrimaryCamera {
    cameraToWorld : mat4x4<f32>,
    // xyz: world-space ray origin. w: maximum ray distance.
    originMaximumDistance : vec4<f32>,
    focalPrincipal : vec4<f32>,
    // xy: viewport in pixels.
    viewport : vec4<u32>,
}

// Distance-scaled level of detail for traversal.
//
// A populated interior node whose world size has fallen below the cut-off is
// reported as a hit instead of being refined. That is voxel-cone tracing: past
// the cut-off the node is smaller than the detail the consumer can resolve, so
// descending four more levels to find out *which* of its voxels the ray meets
// buys nothing and costs the majority of the traversal.
//
//   cutoff(t) = min(
//       lodWorldSize + lodDistanceSlope * (t - lodStartDistance),
//       lodMaximumWorldSize)
//
// Traversal remains exact before lodStartDistance. A non-positive maximum is
// unbounded.
//
// Both zero disables it exactly - the traversal then refines to leaves as
// before, which is what primary rays and the equivalence tests require. It is
// conservative in one direction only: an occupied region can never be missed,
// it can only be reported nearer/coarser than the true surface. Shadow and
// occlusion rays only need the boolean, so that is free for them.
struct SparseVoxelTree64TraceLOD {
    lodWorldSize : f32,
    lodDistanceSlope : f32,
    lodStartDistance : f32,
    lodMaximumWorldSize : f32,
}

struct SparseVoxelTree64TraceDispatch {
    rayCount : u32,
    maximumQueries : u32,
    chunkCapacity : u32,
    nodeCapacity : u32,
    nodeSlotCapacity : u32,
    lodWorldSize : f32,
    lodDistanceSlope : f32,
    lodStartDistance : f32,
    lodMaximumWorldSize : f32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

#ifdef TREE64_DYNAMIC_LAYER
struct SparseVoxelTree64DynamicModel {
    localMinimum : vec3<f32>,
    nodeOffset : u32,
    localCellSize : vec3<f32>,
    nodeCount : u32,
    rootNodeOffset : u32,
    side : u32,
    modelId : u32,
    flags : u32,
}

struct SparseVoxelTree64DynamicInstance {
    worldToModel : mat4x4<f32>,
    worldBoundsMinimum : vec4<f32>,
    worldBoundsMaximum : vec4<f32>,
    modelIndex : u32,
    stableInstanceId : u32,
    contentGeneration : u32,
    flags : u32,
    terrainSlot : u32,
    terrainGeneration : u32,
    regionMappingId : u32,
    receiverCellSizeBits : u32,
}

struct SparseVoxelTree64DynamicMembership {
    worldChunk : vec3<i32>,
    firstReference : u32,
    referenceCount : u32,
    contentGeneration : u32,
    flags : u32,
    reserved0 : u32,
}

struct SparseVoxelTree64DynamicTraceDispatch {
    rayCount : u32,
    maximumQueries : u32,
    chunkCapacity : u32,
    nodeCapacity : u32,
    nodeSlotCapacity : u32,
    modelCount : u32,
    instanceCount : u32,
    membershipCapacity : u32,
    referenceCount : u32,
    dynamicNodeCount : u32,
    maximumReferencesPerChunk : u32,
    dynamicGeneration : u32,
    lodWorldSize : f32,
    lodDistanceSlope : f32,
    lodStartDistance : f32,
    lodMaximumWorldSize : f32,
    // BVH4 broad phase. Zero node count selects the clipmap directory march;
    // both are always present so the two can be A/B'd on one build.
    tlasRootNode : u32,
    tlasNodeCount : u32,
    tlasReserved0 : u32,
    tlasReserved1 : u32,
}
#endif

const TREE64_TERMINAL_FLAG = 1u;
const TREE64_CHUNK_VALID = 1u;
const TREE64_CHUNK_UNKNOWN = 2u;
const TREE64_CHUNK_OCCUPIED = 4u;
const TREE64_DYNAMIC_VALID = 1u;
const TREE64_DYNAMIC_UNKNOWN = 2u;
const TREE64_DYNAMIC_SKIP_RECEIVER_CELL = 4u;
// worldToModel is identity rotation plus translation; see
// kSparseVoxelTree64DynamicTranslationOnly.
const TREE64_DYNAMIC_TRANSLATION_ONLY = 8u;
// A valid ownership/raster instance which is temporarily excluded from global
// any-hit lighting while a finer terrain cut replaces it.
const TREE64_DYNAMIC_DISABLE_ANY_HIT = 16u;
const TREE64_TERRAIN_PAGE_VALID = 1u;

fn tree64PopulationContains(
    node : SparseVoxelTree64Node,
    child : u32) -> bool {
    if (child < 32u) {
        return (node.populationMaskLo & (1u << child)) != 0u;
    }
    return (node.populationMaskHi & (1u << (child - 32u))) != 0u;
}

fn tree64PopulationCountBefore(
    node : SparseVoxelTree64Node,
    child : u32) -> u32 {
    if (child < 32u) {
        let before = select(0u, (1u << child) - 1u, child != 0u);
        return countOneBits(node.populationMaskLo & before);
    }
    let highChild = child - 32u;
    let before = select(
        0u, (1u << highChild) - 1u, highChild != 0u);
    return countOneBits(node.populationMaskLo) +
        countOneBits(node.populationMaskHi & before);
}

#ifdef TREE64_SUBGROUP_BROADCAST
// Subgroup-assisted node sharing. When multiple lanes in a subgroup need
// the same node (common for upper tree levels with coherent primary rays),
// one lane reads it and broadcasts to the rest. Falls back to an individual
// read when the lane's address differs from the leader's.
fn tree64ReadNodeCoherent(
    index : u32) -> SparseVoxelTree64Node
{
    let leader = subgroupBroadcastFirst(index);
    // Subgroup operations must execute in uniform control flow. Load and
    // broadcast the leader node unconditionally, then select it only for
    // lanes that requested the same address.
    let leaderNode = nodes[leader];
    let broadcastNode = SparseVoxelTree64Node(
        subgroupBroadcastFirst(leaderNode.childBaseAndFlags),
        subgroupBroadcastFirst(leaderNode.populationMaskLo),
        subgroupBroadcastFirst(leaderNode.populationMaskHi)
#ifdef TREE64_PADDED_NODE
        , 0u
#endif
    );
    if (index == leader) {
        return broadcastNode;
    }
    // This lane needs a different node — individual read.
    return nodes[index];
}
#endif
