// ============================================================================
// Terrain GPU v2 - generated virtual-geometry page emit
//
// Turns one lane of fine raster bricks (the GPU terrain generator's output for
// one terrain page at one level) into decoded virtual-geometry clusters that
// the culler, both rasterizers and the visibility resolve consume directly.
//
// Two dispatch modes, one per transaction:
//   count : enumerate the exposed faces and count them per tile, so the CPU
//           can reserve exactly the clusters, vertices and indices the page
//           needs (a blind maximum reservation exhausted the scene after a
//           few hundred pages);
//   emit  : write the faces into the reserved range, tile by tile, so every
//           cluster is spatially coherent and belongs to one quadrant of the
//           page.
//
// A page is an 8x8 grid of tiles (one tile per 8-voxel brick column at 64
// columns per page); tile t owns clusters [tileClusterBase[t],
// tileClusterBase[t+1]) of the page. The LOD cut across levels is decided per
// page, with the same sphere and error on both sides of every parent/child
// pair: a cluster's self test uses its own page's sphere and voxel, its parent
// test the parent page's sphere and voxel, exactly the numbers the parent's
// clusters use for their own self test. A cluster whose quadrant child page
// is not resident carries DECODED_CLUSTER_FLAG_STREAMING_LEAF, which the
// culler turns into "self error zero": it draws at any closeness until the
// child arrives, and drops out through the parent test once the child is in.
// ============================================================================

#include "sparse_voxel_brick_common.wgsl"
#include "virtualgeometrydata.wgsl"

#ifndef TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP
#define TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP 128
#endif

const TERRAIN_V2_GENERATED_VG_STATUS_EMPTY : u32 = 0u;
const TERRAIN_V2_GENERATED_VG_STATUS_READY : u32 = 1u;
const TERRAIN_V2_GENERATED_VG_STATUS_OVERFLOW : u32 = 2u;
const TERRAIN_V2_GENERATED_VG_STATUS_INVALID : u32 = 3u;
const TERRAIN_V2_GENERATED_VG_STATUS_STALE_SOURCE : u32 = 4u;
const TERRAIN_V2_GENERATED_VG_STATUS_COUNTED : u32 = 5u;

const TERRAIN_V2_FINE_RASTER_STATUS_READY : u32 = 2u;
const TERRAIN_V2_GENERATED_VG_ERROR_INVALID_DESCRIPTOR : u32 = 1u << 0u;
const TERRAIN_V2_GENERATED_VG_ERROR_STALE_SOURCE : u32 = 1u << 1u;
const TERRAIN_V2_GENERATED_VG_ERROR_FACE_OVERFLOW : u32 = 1u << 2u;
const TERRAIN_V2_GENERATED_VG_ERROR_CLUSTER_OVERFLOW : u32 = 1u << 3u;
const TERRAIN_V2_GENERATED_VG_ERROR_TILE_OVERFLOW : u32 = 1u << 4u;

const TERRAIN_V2_GENERATED_VG_FLAG_OMIT_NEGATIVE_Y_FACE : u32 = 1u << 0u;
const TERRAIN_V2_GENERATED_VG_FLAG_COUNT_MODE : u32 = 1u << 1u;
const TERRAIN_V2_GENERATED_VG_FLAG_TRUE_LEAF : u32 = 1u << 2u;

const TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS : u32 = 8u;
const TERRAIN_V2_GENERATED_VG_TILE_COUNT : u32 = 64u;
// Per request: word 0 is the page's face total, words 1..64 the tile counts.
const TERRAIN_V2_GENERATED_VG_COUNTER_WORDS : u32 = 65u;

struct TerrainGPUV2FineRasterResult {
    status : u32,
    overflowStage : u32,
    validationErrors : u32,
    flags : u32,
    denseBrickCount : u32,
    nodeCount : u32,
    materialCount : u32,
    rootCount : u32,
    occupiedVoxelCount : u32,
    terrainGeneration : u32,
    generatorRevision : u32,
    selectedWorkgroup : u32,
    scratchWords : u32,
    perNodeVoxelScratchWords : u32,
    microSurfaceBytes : u32,
    reserved : u32,
}

struct TerrainGPUV2GeneratedVGEmitRequest {
    ticketIdLow : u32,
    ticketIdHigh : u32,
    localPageIndex : u32,
    globalPageTableIndex : u32,

    sourceLane : u32,
    sourceNodeOffset : u32,
    sourceNodeCount : u32,
    sourceNodeCapacity : u32,

    clusterOffset : u32,
    clusterCount : u32,
    decodedVertexOffset : u32,
    decodedVertexCount : u32,

    decodedIndexOffset : u32,
    decodedIndexCount : u32,
    maxFacesPerCluster : u32,
    expectedTerrainGeneration : u32,

    boundsMin : vec4<f32>,
    boundsMax : vec4<f32>,

    selfError : f32,
    parentError : f32,
    rootPage : u32,
    flags : u32,

    parentBoundsMin : vec4<f32>,
    parentBoundsMax : vec4<f32>,

    residentChildMask : u32,
    pageBricksPerAxis : u32,
    reserved0 : u32,
    reserved1 : u32,

    tileClusterBase : array<u32, 64>,
    // Faces of every tile before this one. Cluster ranges are packed tight:
    // reserving a whole 48-face cluster for each tile's partial last cluster
    // wasted a quarter of the decoded arena and stopped refinement at 372
    // pages on the slope test.
    tileFaceBase : array<u32, 64>,
}

struct TerrainGPUV2GeneratedVGEmitResult {
    status : u32,
    validationErrors : u32,
    ticketIdLow : u32,
    ticketIdHigh : u32,

    localPageIndex : u32,
    globalPageTableIndex : u32,
    emittedFaces : u32,
    emittedTriangles : u32,

    emittedVertices : u32,
    emittedIndices : u32,
    emittedClusters : u32,
    overflowFaces : u32,

    sourceNodes : u32,
    terrainGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,

    tileFaces : array<u32, 64>,
}

struct TerrainGPUV2GeneratedVGFlagRequest {
    clusterOffset : u32,
    clusterCount : u32,
    residentChildMask : u32,
    flags : u32,
    boundsMin : vec4<f32>,
    boundsMax : vec4<f32>,
    parentBoundsMin : vec4<f32>,
    parentBoundsMax : vec4<f32>,
    selfError : f32,
    parentError : f32,
    rootPage : u32,
    updateCullMetadata : u32,
}

struct SparseVoxelGeneratedVGEmitUniforms {
    requestCount : u32,
    counterStride : u32,
    sourceNodeLaneStride : u32,
    flagRequestCount : u32,
    // Threads the face passes are dispatched with. A page's candidate count
    // is only known on the GPU (the raster compacts the occupied bricks), so
    // the dispatch is a fixed grid and every thread strides over its share.
    // Sizing it from the lane's brick capacity instead launched 25 million
    // threads for a page holding a hundred bricks and lost the device.
    faceThreadCount : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelGeneratedVGEmitUniforms;
@group(0) @binding(1) var<storage, read> requests : array<TerrainGPUV2GeneratedVGEmitRequest>;
@group(0) @binding(2) var<storage, read> fineResults : array<TerrainGPUV2FineRasterResult>;
@group(0) @binding(3) var<storage, read> sourceNodes : array<SparseVoxelBrickNode>;
@group(0) @binding(4) var<storage, read> sourceOccupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(5) var<storage, read> sourceMaterialWords : array<u32>;
@group(0) @binding(6) var<storage, read_write> decodedClusterCullBuffer : array<DecodedClusterCullData>;
@group(0) @binding(7) var<storage, read_write> decodedClusterGeometryBuffer : array<DecodedClusterGeometryInfo>;
@group(0) @binding(8) var<storage, read_write> decodedVertexBuffer : array<DecodedVertexData>;
@group(0) @binding(9) var<storage, read_write> decodedIndexBuffer : array<u32>;
@group(0) @binding(10) var<storage, read_write> counters : array<atomic<u32>>;
@group(0) @binding(11) var<storage, read_write> results : array<TerrainGPUV2GeneratedVGEmitResult>;
@group(0) @binding(12) var<storage, read> flagRequests : array<TerrainGPUV2GeneratedVGFlagRequest>;
// Six neighbour brick indices per source brick, lane-local, built once per
// transaction. Without it every boundary face searched the whole page for its
// neighbour: a page holds around 150 bricks and about 1800 boundary faces
// each, so the search was tens of millions of comparisons per page and it is
// what made the emit the slowest stage.
@group(0) @binding(13) var<storage, read_write> neighbourNodes : array<u32>;

fn counterBase(requestIndex : u32) -> u32 {
    return requestIndex * uniforms.counterStride;
}

fn tileCounterIndex(requestIndex : u32, tile : u32) -> u32 {
    return counterBase(requestIndex) + 1u + tile;
}

fn isCountMode(request : TerrainGPUV2GeneratedVGEmitRequest) -> bool {
    return (request.flags & TERRAIN_V2_GENERATED_VG_FLAG_COUNT_MODE) != 0u;
}

fn requestIsValid(request : TerrainGPUV2GeneratedVGEmitRequest) -> bool {
    if (request.sourceNodeCount == 0u ||
        request.sourceNodeCount > request.sourceNodeCapacity) {
        return false;
    }
    if (isCountMode(request)) {
        return true;
    }
    if (request.clusterCount == 0u || request.clusterCount > MAX_CLUSTERS_PER_PAGE) {
        return false;
    }
    if (request.decodedVertexCount < 4u || request.decodedIndexCount < 6u) {
        return false;
    }
    if (request.maxFacesPerCluster == 0u ||
        request.maxFacesPerCluster * 4u > MAX_CLUSTER_VERTICES ||
        request.maxFacesPerCluster * 2u > CLUSTER_SIZE) {
        return false;
    }
    return true;
}

// Tile of a brick: its brick column inside the page, folded onto the 8x8 tile
// grid when a page holds more than eight bricks per axis.
fn tileOfNode(request : TerrainGPUV2GeneratedVGEmitRequest, node : SparseVoxelBrickNode) -> u32 {
    let brickEdge = 8.0 * node.minimumVoxelSize.w;
    let bx = u32(max(floor((node.minimumVoxelSize.x - request.boundsMin.x) / brickEdge + 0.5), 0.0));
    let bz = u32(max(floor((node.minimumVoxelSize.z - request.boundsMin.z) / brickEdge + 0.5), 0.0));
    let bricksPerAxis = max(request.pageBricksPerAxis, TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS);
    let tx = min(bx * TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / bricksPerAxis,
                 TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS - 1u);
    let tz = min(bz * TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / bricksPerAxis,
                 TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS - 1u);
    return tx + tz * TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS;
}

// Quadrant of a tile: bit 0 is the +x half, bit 1 the +z half. The CPU packs
// residentChildMask with the same numbering from the child pages' bounds.
fn quadrantOfTile(tile : u32) -> u32 {
    let tx = tile % TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS;
    let tz = tile / TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS;
    return select(0u, 1u, tx >= TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / 2u) |
        select(0u, 2u, tz >= TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / 2u);
}

fn clusterFlagsForTile(tile : u32, residentChildMask : u32, flags : u32) -> u32 {
    let trueLeaf = (flags & TERRAIN_V2_GENERATED_VG_FLAG_TRUE_LEAF) != 0u;
    let childResident = ((residentChildMask >> quadrantOfTile(tile)) & 1u) != 0u;
    return select(0u, DECODED_CLUSTER_FLAG_STREAMING_LEAF, trueLeaf || !childResident);
}

// The tile owning local cluster `cluster`, from the tile cluster bases.
fn tileOfCluster(requestIndex : u32, cluster : u32) -> u32 {
    var tile = 0u;
    for (var candidate = 1u; candidate < TERRAIN_V2_GENERATED_VG_TILE_COUNT; candidate += 1u) {
        if (requests[requestIndex].tileClusterBase[candidate] <= cluster) {
            tile = candidate;
        }
    }
    return tile;
}

fn tileClusterEnd(requestIndex : u32, tile : u32) -> u32 {
    if (tile + 1u < TERRAIN_V2_GENERATED_VG_TILE_COUNT) {
        return requests[requestIndex].tileClusterBase[tile + 1u];
    }
    return requests[requestIndex].clusterCount;
}

fn cellOccupied(occupancyBase : u32, node : SparseVoxelBrickNode, cell : u32) -> bool {
    let occupancyIndex = occupancyBase + node.occupancyIndex;
    return (sourceOccupancy[occupancyIndex].cells[cell >> 5u].bits &
        (1u << (cell & 31u))) != 0u;
}

fn sourceNodeIndexForCanonicalBrick(
    request : TerrainGPUV2GeneratedVGEmitRequest,
    origin : vec3<i32>,
    scale : i32) -> u32 {
    let base = request.sourceNodeOffset +
        request.sourceLane * uniforms.sourceNodeLaneStride;
    // The fine raster compacts only occupied dense bricks, so neighbouring
    // bricks are not addressable by dense index here; compare canonical brick
    // origins directly. This removes interior brick faces before terrain
    // becomes ordinary virtual-geometry triangles.
    //
    // The scan is bounded by the bricks the raster actually produced, not by
    // the lane's capacity: a page holds around a hundred bricks of a possible
    // 8192, and scanning the capacity for every boundary face took the device
    // past its watchdog (the G-buffer smoke test lost the device outright).
    let limit = min(request.sourceNodeCount, fineResults[request.sourceLane].nodeCount);
    for (var local = 0u; local < limit; local += 1u) {
        let candidate = sourceNodes[base + local];
        if (candidate.canonicalGridOriginScale.w == scale &&
            candidate.canonicalGridOriginScale.x == origin.x &&
            candidate.canonicalGridOriginScale.y == origin.y &&
            candidate.canonicalGridOriginScale.z == origin.z) {
            return local;
        }
    }
    return SPARSE_VOXEL_BRICK_INVALID_INDEX;
}

fn neighbourTableIndex(request : TerrainGPUV2GeneratedVGEmitRequest, localNode : u32, face : u32) -> u32 {
    return (request.sourceLane * uniforms.sourceNodeLaneStride + localNode) * 6u + face;
}

// The brick one step along `face` from this one, or SPARSE_VOXEL_BRICK_INVALID_INDEX.
fn neighbourBrickOrigin(node : SparseVoxelBrickNode, face : u32) -> vec3<i32> {
    var origin = node.canonicalGridOriginScale.xyz;
    let brickStep = 8 * node.canonicalGridOriginScale.w;
    if (face == 0u) { origin.x -= brickStep; }
    else if (face == 1u) { origin.x += brickStep; }
    else if (face == 2u) { origin.y -= brickStep; }
    else if (face == 3u) { origin.y += brickStep; }
    else if (face == 4u) { origin.z -= brickStep; }
    else { origin.z += brickStep; }
    return origin;
}

@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn buildTerrainGPUV2GeneratedVGNeighbours(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    let fine = fineResults[request.sourceLane];
    let nodeCount = min(request.sourceNodeCount, fine.nodeCount);
    let localNode = globalId.x / 6u;
    let face = globalId.x % 6u;
    if (localNode >= nodeCount) {
        return;
    }
    let base = request.sourceNodeOffset + request.sourceLane * uniforms.sourceNodeLaneStride;
    let node = sourceNodes[base + localNode];
    let origin = neighbourBrickOrigin(node, face);
    let scale = node.canonicalGridOriginScale.w;
    neighbourNodes[neighbourTableIndex(request, localNode, face)] =
        sourceNodeIndexForCanonicalBrick(request, origin, scale);
}

fn neighbourBrickCell(
    request : TerrainGPUV2GeneratedVGEmitRequest,
    occupancyBase : u32,
    node : SparseVoxelBrickNode,
    localNode : u32,
    localCell : vec3<u32>,
    face : u32) -> bool {
    var neighbourOrigin = node.canonicalGridOriginScale.xyz;
    var neighbourCell = localCell;
    let brickStep = 8 * node.canonicalGridOriginScale.w;
    if (face == 0u) {
        neighbourOrigin.x -= brickStep;
        neighbourCell.x = 7u;
    } else if (face == 1u) {
        neighbourOrigin.x += brickStep;
        neighbourCell.x = 0u;
    } else if (face == 2u) {
        neighbourOrigin.y -= brickStep;
        neighbourCell.y = 7u;
    } else if (face == 3u) {
        neighbourOrigin.y += brickStep;
        neighbourCell.y = 0u;
    } else if (face == 4u) {
        neighbourOrigin.z -= brickStep;
        neighbourCell.z = 7u;
    } else {
        neighbourOrigin.z += brickStep;
        neighbourCell.z = 0u;
    }
    let neighbourLocalNode = neighbourNodes[neighbourTableIndex(request, localNode, face)];
    if (neighbourLocalNode == SPARSE_VOXEL_BRICK_INVALID_INDEX) {
        return false;
    }
    let nodeIndex = request.sourceNodeOffset +
        request.sourceLane * uniforms.sourceNodeLaneStride + neighbourLocalNode;
    let neighbour = sourceNodes[nodeIndex];
    return cellOccupied(
        occupancyBase,
        neighbour,
        neighbourCell.x + neighbourCell.y * 8u + neighbourCell.z * 64u);
}

fn neighborOccupied(
    request : TerrainGPUV2GeneratedVGEmitRequest,
    occupancyBase : u32,
    node : SparseVoxelBrickNode,
    localNode : u32,
    x : u32,
    y : u32,
    z : u32,
    face : u32) -> bool {
    if (face == 0u) {
        if (x == 0u) {
            return neighbourBrickCell(
                request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
        }
        return cellOccupied(occupancyBase, node, (x - 1u) + y * 8u + z * 64u);
    }
    if (face == 1u) {
        if (x + 1u >= 8u) {
            return neighbourBrickCell(
                request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
        }
        return cellOccupied(occupancyBase, node, (x + 1u) + y * 8u + z * 64u);
    }
    if (face == 2u) {
        if (y == 0u) {
            return neighbourBrickCell(
                request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
        }
        return cellOccupied(occupancyBase, node, x + (y - 1u) * 8u + z * 64u);
    }
    if (face == 3u) {
        if (y + 1u >= 8u) {
            return neighbourBrickCell(
                request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
        }
        return cellOccupied(occupancyBase, node, x + (y + 1u) * 8u + z * 64u);
    }
    if (face == 4u) {
        if (z == 0u) {
            return neighbourBrickCell(
                request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
        }
        return cellOccupied(occupancyBase, node, x + y * 8u + (z - 1u) * 64u);
    }
    if (z + 1u >= 8u) {
        return neighbourBrickCell(
            request, occupancyBase, node, localNode, vec3<u32>(x, y, z), face);
    }
    return cellOccupied(occupancyBase, node, x + y * 8u + (z + 1u) * 64u);
}

fn faceNormal(face : u32) -> vec3<f32> {
    if (face == 0u) { return vec3<f32>(-1.0, 0.0, 0.0); }
    if (face == 1u) { return vec3<f32>( 1.0, 0.0, 0.0); }
    if (face == 2u) { return vec3<f32>(0.0, -1.0, 0.0); }
    if (face == 3u) { return vec3<f32>(0.0,  1.0, 0.0); }
    if (face == 4u) { return vec3<f32>(0.0, 0.0, -1.0); }
    return vec3<f32>(0.0, 0.0, 1.0);
}

fn faceCorner(face : u32, corner : u32) -> vec3<f32> {
    if (face == 0u) {
        if (corner == 0u) { return vec3<f32>(0.0, 0.0, 0.0); }
        if (corner == 1u) { return vec3<f32>(0.0, 0.0, 1.0); }
        if (corner == 2u) { return vec3<f32>(0.0, 1.0, 1.0); }
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    if (face == 1u) {
        if (corner == 0u) { return vec3<f32>(1.0, 0.0, 1.0); }
        if (corner == 1u) { return vec3<f32>(1.0, 0.0, 0.0); }
        if (corner == 2u) { return vec3<f32>(1.0, 1.0, 0.0); }
        return vec3<f32>(1.0, 1.0, 1.0);
    }
    if (face == 2u) {
        if (corner == 0u) { return vec3<f32>(0.0, 0.0, 1.0); }
        if (corner == 1u) { return vec3<f32>(0.0, 0.0, 0.0); }
        if (corner == 2u) { return vec3<f32>(1.0, 0.0, 0.0); }
        return vec3<f32>(1.0, 0.0, 1.0);
    }
    if (face == 3u) {
        if (corner == 0u) { return vec3<f32>(0.0, 1.0, 0.0); }
        if (corner == 1u) { return vec3<f32>(0.0, 1.0, 1.0); }
        if (corner == 2u) { return vec3<f32>(1.0, 1.0, 1.0); }
        return vec3<f32>(1.0, 1.0, 0.0);
    }
    if (face == 4u) {
        if (corner == 0u) { return vec3<f32>(1.0, 0.0, 0.0); }
        if (corner == 1u) { return vec3<f32>(0.0, 0.0, 0.0); }
        if (corner == 2u) { return vec3<f32>(0.0, 1.0, 0.0); }
        return vec3<f32>(1.0, 1.0, 0.0);
    }
    if (corner == 0u) { return vec3<f32>(0.0, 0.0, 1.0); }
    if (corner == 1u) { return vec3<f32>(1.0, 0.0, 1.0); }
    if (corner == 2u) { return vec3<f32>(1.0, 1.0, 1.0); }
    return vec3<f32>(0.0, 1.0, 1.0);
}

fn faceUV(corner : u32) -> vec2<f32> {
    if (corner == 0u) { return vec2<f32>(0.0, 0.0); }
    if (corner == 1u) { return vec2<f32>(1.0, 0.0); }
    if (corner == 2u) { return vec2<f32>(1.0, 1.0); }
    return vec2<f32>(0.0, 1.0);
}

fn writeDecodedVertex(index : u32, position : vec3<f32>, normal : vec3<f32>, uv : vec2<f32>) {
    decodedVertexBuffer[index].positionX = position.x;
    decodedVertexBuffer[index].positionY = position.y;
    decodedVertexBuffer[index].positionZ = position.z;
    decodedVertexBuffer[index].boneWeightOffset = 0u;
    decodedVertexBuffer[index].normalX = normal.x;
    decodedVertexBuffer[index].normalY = normal.y;
    decodedVertexBuffer[index].normalZ = normal.z;
    decodedVertexBuffer[index].boneWeightCount = 0u;
    decodedVertexBuffer[index].uvX = uv.x;
    decodedVertexBuffer[index].uvY = uv.y;
    decodedVertexBuffer[index]._padding0 = 0u;
    decodedVertexBuffer[index]._padding1 = 0u;
}

// The square of one tile of a page, with the page's height range. The LOD
// test runs on this rather than on the whole page: a page seen from above
// spans a wide depth range, and deciding its level from its centre made the
// voxels visibly coarser at the near edge and finer at the far edge of every
// page. A tile is an eighth of the page on each axis, so the decision is 64
// times finer while residency stays per page.
fn tileBounds(
    boundsMin : vec3<f32>,
    boundsMax : vec3<f32>,
    tileX : u32,
    tileZ : u32,
    tilesPerAxis : u32) -> array<vec3<f32>, 2> {
    let span = (boundsMax - boundsMin) / f32(tilesPerAxis);
    let minimum = vec3<f32>(
        boundsMin.x + span.x * f32(tileX),
        boundsMin.y,
        boundsMin.z + span.z * f32(tileZ));
    let maximum = vec3<f32>(
        minimum.x + span.x,
        boundsMax.y,
        minimum.z + span.z);
    return array<vec3<f32>, 2>(minimum, maximum);
}

fn writeClusterSpheres(
    globalCluster : u32,
    request : TerrainGPUV2GeneratedVGEmitRequest,
    tile : u32) {
    let tileX = tile % TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS;
    let tileZ = tile / TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS;
    let own = tileBounds(
        request.boundsMin.xyz, request.boundsMax.xyz, tileX, tileZ,
        TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS);
    let selfCenter = (own[0] + own[1]) * 0.5;
    let selfRadius = length(own[1] - own[0]) * 0.5;
    var parentCenter = selfCenter;
    var parentRadius = selfRadius;
    if (request.rootPage == 0u) {
        // The parent tile covering this tile's ground. A page occupies one
        // quadrant of its parent, so its 8x8 tiles map onto a 4x4 block of
        // the parent's. The parent's own clusters compute the identical
        // sphere for their self test, which is what keeps the two sides of
        // the cut exact.
        let pageCenter = (request.boundsMin.xz + request.boundsMax.xz) * 0.5;
        let parentPageCenter = (request.parentBoundsMin.xz + request.parentBoundsMax.xz) * 0.5;
        let quadrantX = select(0u, 1u, pageCenter.x >= parentPageCenter.x);
        let quadrantZ = select(0u, 1u, pageCenter.y >= parentPageCenter.y);
        let parentTileX = quadrantX * (TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / 2u) + tileX / 2u;
        let parentTileZ = quadrantZ * (TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS / 2u) + tileZ / 2u;
        let parent = tileBounds(
            request.parentBoundsMin.xyz, request.parentBoundsMax.xyz,
            parentTileX, parentTileZ, TERRAIN_V2_GENERATED_VG_TILES_PER_AXIS);
        parentCenter = (parent[0] + parent[1]) * 0.5;
        parentRadius = length(parent[1] - parent[0]) * 0.5;
    }
    decodedClusterCullBuffer[globalCluster].selfCenterX = selfCenter.x;
    decodedClusterCullBuffer[globalCluster].selfCenterY = selfCenter.y;
    decodedClusterCullBuffer[globalCluster].selfCenterZ = selfCenter.z;
    decodedClusterCullBuffer[globalCluster].selfRadius = selfRadius;
    decodedClusterCullBuffer[globalCluster].parentCenterX = parentCenter.x;
    decodedClusterCullBuffer[globalCluster].parentCenterY = parentCenter.y;
    decodedClusterCullBuffer[globalCluster].parentCenterZ = parentCenter.z;
    decodedClusterCullBuffer[globalCluster].parentRadius = parentRadius;
    // Voxel faces point every way inside one cluster: no cone (a cutoff of
    // one is the culler's "never backfacing"; minus one culled everything).
    decodedClusterCullBuffer[globalCluster].coneAxisX = 0.0;
    decodedClusterCullBuffer[globalCluster].coneAxisY = 1.0;
    decodedClusterCullBuffer[globalCluster].coneAxisZ = 0.0;
    decodedClusterCullBuffer[globalCluster].coneCutoff = 1.0;
    decodedClusterCullBuffer[globalCluster].selfError = request.selfError;
    // A root page has nothing coarser to fall back to: its parent test must
    // always pass.
    decodedClusterCullBuffer[globalCluster].parentError =
        select(max(request.parentError, request.selfError), CULL_INFINITE_ERROR, request.rootPage != 0u);
}

// Shared face enumeration: returns the tile of the candidate's brick when the
// candidate is an exposed face of an occupied cell, or 0xffffffff otherwise.
struct FaceCandidate {
    valid : bool,
    node : SparseVoxelBrickNode,
    cell : vec3<u32>,
    face : u32,
    tile : u32,
}

fn resolveFaceCandidate(
    request : TerrainGPUV2GeneratedVGEmitRequest,
    fine : TerrainGPUV2FineRasterResult,
    candidate : u32) -> FaceCandidate {
    var result : FaceCandidate;
    result.valid = false;
    let face = candidate % 6u;
    // Terrain pages are ordinary virtual-geometry voxel-face payloads: every
    // exposed face of every occupied cell, four private vertices, two
    // triangles, one face normal and one cube-unwrap UV island per face.
    // Runtime terrain surface pages may omit face 2 (-Y), the hidden
    // underside.
    if ((request.flags & TERRAIN_V2_GENERATED_VG_FLAG_OMIT_NEGATIVE_Y_FACE) != 0u &&
        face == 2u) {
        return result;
    }
    let cell = (candidate / 6u) % SPARSE_VOXEL_BRICK_VOXEL_COUNT;
    let localNode = candidate / (6u * SPARSE_VOXEL_BRICK_VOXEL_COUNT);
    if (localNode >= request.sourceNodeCount || localNode >= fine.nodeCount) {
        return result;
    }
    let nodeIndex = request.sourceNodeOffset +
        request.sourceLane * uniforms.sourceNodeLaneStride + localNode;
    let node = sourceNodes[nodeIndex];
    let occupancyBase = request.sourceLane * uniforms.sourceNodeLaneStride;
    if (node.occupancyIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        !cellOccupied(occupancyBase, node, cell)) {
        return result;
    }
    let x = cell & 7u;
    let y = (cell >> 3u) & 7u;
    let z = (cell >> 6u) & 7u;
    if (neighborOccupied(request, occupancyBase, node, localNode, x, y, z, face)) {
        return result;
    }
    result.valid = true;
    result.node = node;
    result.cell = vec3<u32>(x, y, z);
    result.face = face;
    result.tile = tileOfNode(request, node);
    return result;
}

fn sourceIsStale(request : TerrainGPUV2GeneratedVGEmitRequest) -> bool {
    let fine = fineResults[request.sourceLane];
    return fine.status != TERRAIN_V2_FINE_RASTER_STATUS_READY ||
        fine.validationErrors != 0u ||
        fine.terrainGeneration != request.expectedTerrainGeneration;
}

// ---------------------------------------------------------------------------
// Stage 0: reset. Counters, result record and, in emit mode, the cluster
// records of the reserved range.
// ---------------------------------------------------------------------------
@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn resetTerrainGPUV2GeneratedVGEmit(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    let index = globalId.x;
    if (index < TERRAIN_V2_GENERATED_VG_COUNTER_WORDS) {
        atomicStore(&counters[counterBase(requestIndex) + index], 0u);
    }
    if (index < TERRAIN_V2_GENERATED_VG_TILE_COUNT) {
        results[requestIndex].tileFaces[index] = 0u;
    }
    if (index == 0u) {
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_EMPTY;
        results[requestIndex].validationErrors = 0u;
        results[requestIndex].ticketIdLow = request.ticketIdLow;
        results[requestIndex].ticketIdHigh = request.ticketIdHigh;
        results[requestIndex].localPageIndex = request.localPageIndex;
        results[requestIndex].globalPageTableIndex = request.globalPageTableIndex;
        results[requestIndex].emittedFaces = 0u;
        results[requestIndex].emittedTriangles = 0u;
        results[requestIndex].emittedVertices = 0u;
        results[requestIndex].emittedIndices = 0u;
        results[requestIndex].emittedClusters = 0u;
        results[requestIndex].overflowFaces = 0u;
        results[requestIndex].sourceNodes = request.sourceNodeCount;
        results[requestIndex].terrainGeneration = 0u;
        results[requestIndex].reserved0 = 0u;
        results[requestIndex].reserved1 = 0u;
    }
    if (isCountMode(request) || index >= request.clusterCount) {
        return;
    }
    let localCluster = index;
    let tile = tileOfCluster(requestIndex, localCluster);
    let globalCluster = request.clusterOffset + localCluster;
    decodedClusterCullBuffer[globalCluster].triangleCount = 0u;
    decodedClusterCullBuffer[globalCluster].localMinX = request.boundsMin.x;
    decodedClusterCullBuffer[globalCluster].localMinY = request.boundsMin.y;
    decodedClusterCullBuffer[globalCluster].localMinZ = request.boundsMin.z;
    decodedClusterCullBuffer[globalCluster].localMaxX = request.boundsMax.x;
    decodedClusterCullBuffer[globalCluster].localMaxY = request.boundsMax.y;
    decodedClusterCullBuffer[globalCluster].localMaxZ = request.boundsMax.z;
    decodedClusterCullBuffer[globalCluster]._padding0 = 0u;
    writeClusterSpheres(globalCluster, request, tile);
    decodedClusterCullBuffer[globalCluster].flags =
        clusterFlagsForTile(tile, request.residentChildMask, request.flags);
    decodedClusterCullBuffer[globalCluster]._padding2 = 0u;
    let facesBefore = requests[requestIndex].tileFaceBase[tile] +
        (localCluster - requests[requestIndex].tileClusterBase[tile]) * request.maxFacesPerCluster;
    decodedClusterGeometryBuffer[globalCluster].vertexOffset =
        request.decodedVertexOffset + facesBefore * 4u;
    decodedClusterGeometryBuffer[globalCluster].indexOffset =
        request.decodedIndexOffset + facesBefore * 6u;
    decodedClusterGeometryBuffer[globalCluster].vertexCount = 0u;
    decodedClusterGeometryBuffer[globalCluster].triangleCount = 0u;
    // Generated pages record the cluster's tile and its index inside the tile
    // here; the flag update kernel and the finalize read them back.
    decodedClusterGeometryBuffer[globalCluster].pageLocalGroupIndex = tile;
    decodedClusterGeometryBuffer[globalCluster].clusterIndexInGroup =
        localCluster - requests[requestIndex].tileClusterBase[tile];
    decodedClusterGeometryBuffer[globalCluster]._padding0 = 0u;
    decodedClusterGeometryBuffer[globalCluster]._padding1 = 0u;
}

// ---------------------------------------------------------------------------
// Stage 1: count faces per tile.
// ---------------------------------------------------------------------------
@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn countTerrainGPUV2GeneratedVGFaces(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    if (!isCountMode(request)) {
        return;
    }
    if (!requestIsValid(request)) {
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_INVALID;
        results[requestIndex].validationErrors = TERRAIN_V2_GENERATED_VG_ERROR_INVALID_DESCRIPTOR;
        return;
    }
    if (sourceIsStale(request)) {
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_STALE_SOURCE;
        results[requestIndex].validationErrors = TERRAIN_V2_GENERATED_VG_ERROR_STALE_SOURCE;
        return;
    }
    let fine = fineResults[request.sourceLane];
    let candidateCount = min(request.sourceNodeCount, fine.nodeCount) *
        SPARSE_VOXEL_BRICK_VOXEL_COUNT * 6u;
    var index = globalId.x;
    loop {
        if (index >= candidateCount) {
            break;
        }
        let candidate = resolveFaceCandidate(request, fine, index);
        if (candidate.valid) {
            atomicAdd(&counters[tileCounterIndex(requestIndex, candidate.tile)], 1u);
            atomicAdd(&counters[counterBase(requestIndex)], 1u);
        }
        index += uniforms.faceThreadCount;
    }
}

// ---------------------------------------------------------------------------
// Stage 2: publish the counts.
// ---------------------------------------------------------------------------
@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn finalizeTerrainGPUV2GeneratedVGCount(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    if (!isCountMode(request)) {
        return;
    }
    let index = globalId.x;
    if (index < TERRAIN_V2_GENERATED_VG_TILE_COUNT) {
        results[requestIndex].tileFaces[index] =
            atomicLoad(&counters[tileCounterIndex(requestIndex, index)]);
    }
    if (index == 0u) {
        let faceCount = atomicLoad(&counters[counterBase(requestIndex)]);
        results[requestIndex].emittedFaces = faceCount;
        results[requestIndex].emittedTriangles = faceCount * 2u;
        results[requestIndex].emittedVertices = faceCount * 4u;
        results[requestIndex].emittedIndices = faceCount * 6u;
        results[requestIndex].emittedClusters = 0u;
        results[requestIndex].sourceNodes = request.sourceNodeCount;
        results[requestIndex].terrainGeneration = request.expectedTerrainGeneration;
        if (results[requestIndex].validationErrors == 0u) {
            results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_COUNTED;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 3: emit faces into the reserved range, tile by tile.
// ---------------------------------------------------------------------------
fn emitFaceCandidate(
    requestIndex : u32,
    request : TerrainGPUV2GeneratedVGEmitRequest,
    fine : TerrainGPUV2FineRasterResult,
    index : u32) {
    let candidate = resolveFaceCandidate(request, fine, index);
    if (!candidate.valid) {
        return;
    }
    let tile = candidate.tile;
    let faceInTile = atomicAdd(&counters[tileCounterIndex(requestIndex, tile)], 1u);
    atomicAdd(&counters[counterBase(requestIndex)], 1u);
    let localCluster = requests[requestIndex].tileClusterBase[tile] +
        faceInTile / request.maxFacesPerCluster;
    if (localCluster >= tileClusterEnd(requestIndex, tile) ||
        localCluster >= request.clusterCount) {
        // The count pass sized this tile; only a source that changed under us
        // can get here.
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_OVERFLOW;
        results[requestIndex].validationErrors =
            results[requestIndex].validationErrors | TERRAIN_V2_GENERATED_VG_ERROR_TILE_OVERFLOW;
        return;
    }
    let faceInCluster = faceInTile % request.maxFacesPerCluster;
    let globalCluster = request.clusterOffset + localCluster;
    let vertexBase = decodedClusterGeometryBuffer[globalCluster].vertexOffset + faceInCluster * 4u;
    let indexBase = decodedClusterGeometryBuffer[globalCluster].indexOffset + faceInCluster * 6u;
    let node = candidate.node;
    let face = candidate.face;
    let cellOrigin = node.minimumVoxelSize.xyz +
        vec3<f32>(f32(candidate.cell.x), f32(candidate.cell.y), f32(candidate.cell.z)) *
        node.minimumVoxelSize.w;
    let normal = faceNormal(face);
    for (var corner = 0u; corner < 4u; corner++) {
        writeDecodedVertex(
            vertexBase + corner,
            cellOrigin + faceCorner(face, corner) * node.minimumVoxelSize.w,
            normal,
            faceUV(corner));
    }
    decodedIndexBuffer[indexBase + 0u] = faceInCluster * 4u + 0u;
    decodedIndexBuffer[indexBase + 1u] = faceInCluster * 4u + 1u;
    decodedIndexBuffer[indexBase + 2u] = faceInCluster * 4u + 2u;
    decodedIndexBuffer[indexBase + 3u] = faceInCluster * 4u + 0u;
    decodedIndexBuffer[indexBase + 4u] = faceInCluster * 4u + 2u;
    decodedIndexBuffer[indexBase + 5u] = faceInCluster * 4u + 3u;
}

// ---------------------------------------------------------------------------
// Stage 4: finalize the emitted clusters and the result record.
// ---------------------------------------------------------------------------

@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn emitTerrainGPUV2GeneratedVGFaces(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    if (isCountMode(request)) {
        return;
    }
    if (!requestIsValid(request)) {
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_INVALID;
        results[requestIndex].validationErrors = TERRAIN_V2_GENERATED_VG_ERROR_INVALID_DESCRIPTOR;
        return;
    }
    if (sourceIsStale(request)) {
        results[requestIndex].status = TERRAIN_V2_GENERATED_VG_STATUS_STALE_SOURCE;
        results[requestIndex].validationErrors = TERRAIN_V2_GENERATED_VG_ERROR_STALE_SOURCE;
        return;
    }
    let fine = fineResults[request.sourceLane];
    let candidateCount = min(request.sourceNodeCount, fine.nodeCount) *
        SPARSE_VOXEL_BRICK_VOXEL_COUNT * 6u;
    var index = globalId.x;
    loop {
        if (index >= candidateCount) {
            break;
        }
        emitFaceCandidate(requestIndex, request, fine, index);
        index += uniforms.faceThreadCount;
    }
}

@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn finalizeTerrainGPUV2GeneratedVGEmit(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }
    let request = requests[requestIndex];
    if (isCountMode(request)) {
        return;
    }
    let localCluster = globalId.x;
    if (localCluster < TERRAIN_V2_GENERATED_VG_TILE_COUNT) {
        results[requestIndex].tileFaces[localCluster] =
            atomicLoad(&counters[tileCounterIndex(requestIndex, localCluster)]);
    }
    if (localCluster < request.clusterCount) {
        let globalCluster = request.clusterOffset + localCluster;
        let tile = decodedClusterGeometryBuffer[globalCluster].pageLocalGroupIndex;
        let indexInTile = decodedClusterGeometryBuffer[globalCluster].clusterIndexInGroup;
        let tileFaces = atomicLoad(&counters[tileCounterIndex(requestIndex, tile)]);
        let consumed = indexInTile * request.maxFacesPerCluster;
        let faceCount = min(
            select(0u, tileFaces - consumed, tileFaces > consumed),
            request.maxFacesPerCluster);
        let vertexBase = decodedClusterGeometryBuffer[globalCluster].vertexOffset;
        let vertexCount = faceCount * 4u;
        if (vertexCount > 0u) {
            var localMin = vec3<f32>(3.402823e38, 3.402823e38, 3.402823e38);
            var localMax = vec3<f32>(-3.402823e38, -3.402823e38, -3.402823e38);
            for (var vertex = 0u; vertex < vertexCount; vertex += 1u) {
                let decoded = decodedVertexBuffer[vertexBase + vertex];
                let position = vec3<f32>(decoded.positionX, decoded.positionY, decoded.positionZ);
                localMin = min(localMin, position);
                localMax = max(localMax, position);
            }
            decodedClusterCullBuffer[globalCluster].localMinX = localMin.x;
            decodedClusterCullBuffer[globalCluster].localMinY = localMin.y;
            decodedClusterCullBuffer[globalCluster].localMinZ = localMin.z;
            decodedClusterCullBuffer[globalCluster].localMaxX = localMax.x;
            decodedClusterCullBuffer[globalCluster].localMaxY = localMax.y;
            decodedClusterCullBuffer[globalCluster].localMaxZ = localMax.z;
        }
        // Spheres and errors are the page's, written by the reset: the LOD
        // decision is per page so both sides of a parent/child pair agree.
        decodedClusterCullBuffer[globalCluster].triangleCount = faceCount * 2u;
        decodedClusterGeometryBuffer[globalCluster].vertexCount = vertexCount;
        decodedClusterGeometryBuffer[globalCluster].triangleCount = faceCount * 2u;
        // Contract words: clusters that hold faces, and the faces they hold.
    }
    if (localCluster == 0u) {
        let faceCount = atomicLoad(&counters[counterBase(requestIndex)]);
        let maxFacesByVertex = request.decodedVertexCount / 4u;
        let maxFacesByIndex = request.decodedIndexCount / 6u;
        let maxFacesByCluster = request.clusterCount * request.maxFacesPerCluster;
        let maxFaces = min(min(maxFacesByVertex, maxFacesByIndex), maxFacesByCluster);
        let writtenFaces = min(faceCount, maxFaces);
        results[requestIndex].emittedFaces = writtenFaces;
        results[requestIndex].emittedTriangles = writtenFaces * 2u;
        results[requestIndex].emittedVertices = writtenFaces * 4u;
        results[requestIndex].emittedIndices = writtenFaces * 6u;
        results[requestIndex].emittedClusters = request.clusterCount;
        results[requestIndex].sourceNodes = request.sourceNodeCount;
        results[requestIndex].terrainGeneration = request.expectedTerrainGeneration;
        // Contract words: the faces the tile counters hold and the clusters
        // they imply. Both must equal the face total and the cluster count.
        var tileFaceSum = 0u;
        var tileClusters = 0u;
        for (var tile = 0u; tile < TERRAIN_V2_GENERATED_VG_TILE_COUNT; tile += 1u) {
            let tileFaces = atomicLoad(&counters[tileCounterIndex(requestIndex, tile)]);
            tileFaceSum += tileFaces;
            tileClusters += (tileFaces + request.maxFacesPerCluster - 1u) / request.maxFacesPerCluster;
        }
        results[requestIndex].reserved0 = tileFaceSum;
        results[requestIndex].reserved1 = tileClusters;
        if (results[requestIndex].validationErrors == 0u) {
            results[requestIndex].status = select(
                TERRAIN_V2_GENERATED_VG_STATUS_OVERFLOW,
                TERRAIN_V2_GENERATED_VG_STATUS_READY,
                faceCount <= maxFaces);
        }
        if (faceCount > maxFaces) {
            results[requestIndex].overflowFaces = faceCount - maxFaces;
            results[requestIndex].validationErrors =
                results[requestIndex].validationErrors |
                TERRAIN_V2_GENERATED_VG_ERROR_FACE_OVERFLOW;
        }
    }
}

// ---------------------------------------------------------------------------
// Stage 5: rewrite the streaming-leaf flags of an installed page when its
// child pages come and go.
// ---------------------------------------------------------------------------
@compute @workgroup_size(TERRAIN_V2_GENERATED_VG_EMIT_WORKGROUP)
fn updateTerrainGPUV2GeneratedVGFlags(
    @builtin(global_invocation_id) globalId : vec3<u32>
) {
    let requestIndex = globalId.z;
    if (requestIndex >= uniforms.flagRequestCount) {
        return;
    }
    let request = flagRequests[requestIndex];
    let localCluster = globalId.x;
    if (localCluster >= request.clusterCount) {
        return;
    }
    let globalCluster = request.clusterOffset + localCluster;
    let tile = decodedClusterGeometryBuffer[globalCluster].pageLocalGroupIndex;
    decodedClusterCullBuffer[globalCluster].flags =
        clusterFlagsForTile(tile, request.residentChildMask, request.flags);
    if (request.updateCullMetadata != 0u) {
        var metric : TerrainGPUV2GeneratedVGEmitRequest;
        metric.boundsMin = request.boundsMin;
        metric.boundsMax = request.boundsMax;
        metric.parentBoundsMin = request.parentBoundsMin;
        metric.parentBoundsMax = request.parentBoundsMax;
        metric.selfError = request.selfError;
        metric.parentError = request.parentError;
        metric.rootPage = request.rootPage;
        writeClusterSpheres(globalCluster, metric, tile);
    }
}
