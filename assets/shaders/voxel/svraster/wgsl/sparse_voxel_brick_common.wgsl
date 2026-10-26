// Shared brick hierarchy definitions. Mirrors SparseVoxelBrick.hpp.

const SPARSE_VOXEL_BRICK_SIDE : u32 = 8u;
const SPARSE_VOXEL_BRICK_VOXEL_COUNT : u32 = 512u;
const SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS : u32 = 16u;
// Coarse level: one bit per 2x2x2 block of voxels, so 4^3 = 64 bits for the
// whole brick. Small enough to live in the node, travel as two flat varyings
// and be marched entirely in registers.
const SPARSE_VOXEL_BRICK_COARSE_SIDE : u32 = 4u;
const SPARSE_VOXEL_BRICK_INVALID_INDEX : u32 = 0xffffffffu;
const SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX : u32 = 0xffffffffu;
const SPARSE_VOXEL_BRICK_LOD_GROUP_BOUNDARY_MASK : u32 = 0x3fu;
const SPARSE_VOXEL_BRICK_LOD_GROUP_SURFACE_REPRESENTATIVE_CERTIFIED : u32 =
    1u << 6u;

// One entry per 32 voxels: the occupancy bits, and the popcount of every bit
// before this word.
//
// The prefix is what makes the material lookup a single load. Addressing a
// voxel's material by the popcount of the bits preceding it otherwise means
// summing up to fifteen words at the end of every DDA that hits - fifteen
// dependent storage loads and fifteen popcounts, per fragment.
struct SparseVoxelBrickOccupancyCell {
    bits : u32,
    prefix : u32,
}

struct SparseVoxelBrickOccupancy {
    cells : array<SparseVoxelBrickOccupancyCell, SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS>,
}

// Appearance for one occupied voxel, in eight bytes.
//
// The octree material record is 32 bytes reached through a 4-byte index, so a
// shading fragment pays a 4-byte load, a dependent 32-byte load, and a cache
// miss for a scattered index. Voxels need very little of that: a base colour,
// two scalars and an emissive term. Packing them inline removes the
// indirection and the record shrinks 4x.
//
//   packed.x : R (0..7) | G (8..15) | B (16..23) | roughness (24..31)
//   packed.y : emissive (0..7) | metallic (8..15) |
//              transient face/hit (16..19) | bottom palette index (20..27) |
//              top/bottom blend (28) | side blend (29) |
//              secondary enabled (30) | reserved (31)
//
// Stored records leave 16..19 clear. The draw pass writes its four-bit surface
// tag there; per-voxel terrain context lives in the normalized micro-context
// word. The other high bits retain optional two-colour metadata end-to-end.
struct SparseVoxelBrickMaterial {
    packed : vec2<u32>,
}

const SPARSE_VOXEL_BRICK_SURFACE_SHIFT : u32 = 16u;
const SPARSE_VOXEL_BRICK_TRANSIENT_MASK : u32 = 0x0fu <<
    SPARSE_VOXEL_BRICK_SURFACE_SHIFT;
const SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_SHIFT : u32 = 20u;
const SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_MASK : u32 = 0xffu;
const SPARSE_VOXEL_BRICK_BLEND_TOP_BOTTOM_FACES : u32 = 1u << 28u;
const SPARSE_VOXEL_BRICK_BLEND_SIDE_FACES : u32 = 1u << 29u;
const SPARSE_VOXEL_BRICK_SECONDARY_ENABLED : u32 = 1u << 30u;
const SPARSE_VOXEL_BRICK_MICRO_ENABLED : u32 = 1u << 31u;
const SPARSE_VOXEL_BRICK_TERRAIN_CLIP_FLAG : u32 = 1u << 31u;
const SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK : u32 = 0x7fu;
const SPARSE_VOXEL_BRICK_TERRAIN_CLIP_QUANTIZATION : f32 = 8.0;
// Compatibility hook for the old overlapping-terrain-ring ownership mask.
// The published runtime cut must be the only source of drawable ownership:
// a second packed XZ exclusion rectangle in cull/draw can reject otherwise
// valid DDA hits and creates camera-angle-dependent triangular holes. Keep the
// decode helpers around for old payloads, but make the mask inert.
const SPARSE_VOXEL_BRICK_ENABLE_TERRAIN_EXCLUSION_CLIP : bool = false;
const SPARSE_VOXEL_BRICK_FORCE_LEAF_RASTER : u32 = 1u << 16u;

struct SparseVoxelBrickNode {
    // xyz: world minimum corner. w: voxel size, so the brick spans w * 8.
    minimumVoxelSize : vec4<f32>,
    // Inclusive occupied cell range, six 3-bit fields.
    packedOccupiedBounds : u32,
    // 4^3 coarse occupancy: bit (x + 4y + 16z) is set when any of the eight
    // voxels under it is occupied. Low word is z < 2, high word is z >= 2.
    coarseOccupancyLow : u32,
    coarseOccupancyHigh : u32,
    occupancyIndex : u32,
    // Base of this brick's run in the material array. The first five fields
    // are everything the vertex shader forwards, so they sit together.
    materialBase : u32,
    parentIndex : u32,
    childCount : u32,
    // Low byte: hierarchy-local LOD; next byte: terrain clipmap LOD; bit 16
    // forces an authored-leaf raster cut for spatial terrain pages.
    level : u32,
    occupiedCount : u32,
    firstChild : u32,
    // Instance these voxels came from, or SPARSE_VOXEL_BRICK_INVALID_INDEX.
    // The instance transform is already baked into the world-space brick grid,
    // so this is not used to place anything - it selects per-instance data,
    // starting with the colour palette a material byte indexes into.
    instanceIndex : u32,
    // First palette entry this brick's material bytes are relative to. Stored
    // rather than derived from instanceIndex: a brick whose cells come from
    // more than one instance carries a palette of its own. Planet terrain
    // pages use bit 31 of instanceIndex to tag packed local clip coordinates.
    paletteBase : u32,
    // Stable logical material-field region; never a physical residency slot.
    regionMappingId : u32,
    // Measured deviation of this brick, and of the ancestor above it, from the
    // finest representation of the same volume, in world units. These occupy
    // what WGSL would otherwise pad implicitly before the vec4<i32> below, so
    // naming them does not move any field.
    selfError : f32,
    parentError : f32,
    reservedTailPadding : u32,
    // xyz is the signed brick origin in the canonical finest-parent grid; w is
    // the number of finest parents represented by one cell at this node.
    canonicalGridOriginScale : vec4<i32>,
}

struct SparseVoxelBrickLODGroup {
    minimumExtent : vec4<f32>,
    canonicalGridOriginScale : vec4<i32>,
    firstNode : u32,
    nodeCount : u32,
    level : u32,
    flags : u32,
    sharedError : f32,
    minimumError : f32,
    maximumError : f32,
    representativeNode : u32,
}

fn brickIsPackedTerrainNode(node : SparseVoxelBrickNode) -> bool {
    return node.instanceIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX &&
        (node.instanceIndex & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_FLAG) != 0u;
}

fn brickHasFinerTerrainClip(node : SparseVoxelBrickNode) -> bool {
    return SPARSE_VOXEL_BRICK_ENABLE_TERRAIN_EXCLUSION_CLIP &&
        brickIsPackedTerrainNode(node);
}

fn brickFinerTerrainClipBoundsPacked(
    minimumVoxelSize : vec4<f32>, packed : u32) -> vec4<f32> {
    let step = minimumVoxelSize.w /
        SPARSE_VOXEL_BRICK_TERRAIN_CLIP_QUANTIZATION;
    return vec4<f32>(
        minimumVoxelSize.x +
            f32(packed & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        minimumVoxelSize.z +
            f32((packed >> 7u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        minimumVoxelSize.x +
            f32((packed >> 14u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        minimumVoxelSize.z +
            f32((packed >> 21u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step);
}

fn brickFinerTerrainClipBounds(node : SparseVoxelBrickNode) -> vec4<f32> {
    return brickFinerTerrainClipBoundsPacked(
        node.minimumVoxelSize, node.instanceIndex);
}

// Cascade ownership is closed on all four edges. The packed maximum is the
// first point owned by the finer cascade, but a coarse voxel ray can report its
// side hit at that exact coordinate. Treating maximum edges as half-open lets
// those +X/+Z faces survive as long, one-sided walls. The tolerance covers the
// final world-space add/DDA rounding without removing a meaningful terrain
// strip.
fn brickFinerTerrainClipContainsWorld(
    minimumVoxelSize : vec4<f32>, packed : u32,
    world : vec3<f32>) -> bool {
    if (!SPARSE_VOXEL_BRICK_ENABLE_TERRAIN_EXCLUSION_CLIP ||
        packed == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        (packed & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_FLAG) == 0u) {
        return false;
    }
    let finer = brickFinerTerrainClipBoundsPacked(
        minimumVoxelSize, packed);
    let maximumCoordinate = max(
        max(abs(finer.x), abs(finer.y)),
        max(abs(finer.z), abs(finer.w)));
    let epsilon = max(minimumVoxelSize.w * 1.0e-4,
        maximumCoordinate * 1.0e-7);
    return world.x >= finer.x - epsilon &&
        world.x <= finer.z + epsilon &&
        world.z >= finer.y - epsilon &&
        world.z <= finer.w + epsilon;
}

struct SparseVoxelBrickCellRange {
    minimumCell : vec3<f32>,
    maximumCell : vec3<f32>,
}

// Inclusive occupied cell range, as the half-open cell-space box the ray needs.
fn brickOccupiedCellRange(packed : u32) -> SparseVoxelBrickCellRange {
    return SparseVoxelBrickCellRange(
        vec3<f32>(
            f32(packed & 7u),
            f32((packed >> 3u) & 7u),
            f32((packed >> 6u) & 7u)),
        vec3<f32>(
            f32(((packed >> 9u) & 7u) + 1u),
            f32(((packed >> 12u) & 7u) + 1u),
            f32(((packed >> 15u) & 7u) + 1u)));
}

struct SparseVoxelBrickCullUniforms {
    worldToClip : mat4x4<f32>,
    cameraWorldPosition : vec4<f32>,
    focalPrincipal : vec4<f32>,
    viewport : vec4<u32>,
    rootCount : u32,
    nodeCount : u32,
    visibleCapacity : u32,
    lodGroupCount : u32,
    lodPixelThreshold : f32,
    cullingFlags : u32,
    depth : u32,
    levelCount : u32,
    hzbMipCount : u32,
    hzbFirstValidMip : u32,
    phase : u32,
    // Fraction of a brick's own voxel size that the occlusion test moves its
    // nearest point toward the eye before comparing. See the comment on the
    // test itself: an LOD-coarse brick's rasterized surface is dilated by up to
    // one of its own voxels, so the pyramid it produces is nearer than the true
    // surface and would otherwise cull whatever peeks past it.
    occlusionDilationVoxels : f32,
    terrainPageRequestCapacity : u32,
    // Mirror of SparseVoxelBrickDrawUniforms::microTuning.y so the cull-side
    // binner and the draw's vertex stage read one routing threshold.
    routeToComputePixelsBits : u32,
    terrainSplitErrorPixels : f32,
    terrainMergeErrorPixels : f32,
    terrainOutputPixelScale : f32,
    terrainMinimumResidentFrames : u32,
    terrainFrameIndex : u32,
    terrainFeedbackEnabled : u32,
    terrainDistanceErrorGrowthPerKilometre : f32,
    terrainDistanceErrorExponent : f32,
    hzbHistoryValid : u32,
    canonicalPadding : u32,
    // Same camera-relative integer lattice used by brick rasterization.
    canonicalWorldGrid : vec4<f32>,
    canonicalCameraCell : vec4<i32>,
    canonicalCameraOffset : vec4<f32>,
}

struct SparseVoxelTerrainPageState {
    generation : u32,
    currentLOD : u32,
    publishedFrame : u32,
    flags : u32,
}

struct SparseVoxelBrickDrawUniforms {
    worldToClip : mat4x4<f32>,
    cameraToWorld : mat4x4<f32>,
    cameraWorldPosition : vec4<f32>,
    focalPrincipal : vec4<f32>,
    viewport : vec4<u32>,
    // x: count fragment invocations per pixel. See SparseVoxelBrickDrawUniformsGPU.
    // y: optional exact-micro distance cutoff as raw f32 bits. Only the
    //    separately compiled cutoff variant reads it.
    // z: collect exact microgeometry counters.
    // w: output pixels per raster pixel in unsigned Q16.
    debugFlags : vec4<u32>,
    // x: exact-micro handoff scale as raw f32 bits. The descriptor's own
    //    lodHandoff can only make the handoff happen *later* - it is clamped to
    //    [0.25, 1.0] and multiplies a 0.5..1.0 pixel fade band - so there is no
    //    way to retire exact microgeometry earlier than one projected pixel.
    //    This divides the projected size, so 2.0 retires it at twice the
    //    distance. 1.0 is the historical behaviour and is bit-exact.
    // y: H5 routing. Projected occupied-box footprint, in pixels, at or below
    //    which a brick is owned by the tiled compute raster instead of the
    //    hardware one. Raw f32 bits; zero disables routing and keeps every
    //    brick on the hardware path.
    // z: largest voxel size allowed to use exact microgeometry, as f32 bits.
    // w: gap-only compute raster. Threads with hardware coverage participate
    //    in barriers but skip candidate traversal.
    microTuning : vec4<u32>,
    // Generated streamed terrain reconstructs every voxel plane from one
    // integer lattice. w == 0 keeps ordinary/package brick coordinates.
    canonicalWorldGrid : vec4<f32>,
    // Camera-relative split of the same lattice. Integer subtraction happens
    // before conversion to f32, so neighbouring bricks produce bit-identical
    // shared planes at planetary world coordinates.
    canonicalCameraCell : vec4<i32>,
    canonicalCameraOffset : vec4<f32>,
}

// Exact-microgeometry handoff scale. Divides the projected microvoxel size, so
// values above one retire exact carving at proportionally greater distance.
// Zero (an uninitialised uniform) and one are both the historical behaviour.
fn sparseVoxelMicroHandoffScale(bits : u32) -> f32 {
    let scale = bitcast<f32>(bits);
    if (!(scale > 0.0)) {
        return 1.0;
    }
    return scale;
}

// H5 routing predicate, shared by the hardware vertex stage and the compute
// binner so the two can never disagree about who owns a brick. Disagreement is
// not a cosmetic bug here: both skipping it leaves a hole, both drawing it
// doubles the work and reintroduces the overdraw the routing exists to remove.
//
// The measure is the projected screen area of the tight occupied box, in
// pixels. Small footprints are where a per-pixel compute walk wins and where
// the hardware's fixed-function coverage earns least.
fn sparseVoxelBrickRoutedToCompute(
    routeBits : u32, projectedPixels : f32) -> bool {
    let threshold = bitcast<f32>(routeBits);
    return threshold > 0.0 && projectedPixels <= threshold;
}

struct SparseVoxelBrickCounters {
    visibleCount : atomic<u32>,
    occludedCount : atomic<u32>,
    overflow : atomic<u32>,
    // Summed screen area of the visible bricks, in pixels. Divided by the
    // covered pixel count this is the overdraw factor, which is what decides
    // whether early-Z would be worth chasing.
    //
    // Only accumulated when BRICK_CULL_MEASURE_AREA is set: it costs eight
    // clip-space transforms and a contended global atomic for every brick that
    // survives, which is the most expensive thing in the pass and buys nothing
    // outside a profiling session.
    visibleAreaPixels : atomic<u32>,
    // Why bricks survive the occlusion test, counted per reason.
    //
    // The test has five ways to answer "visible" and only one of them is a real
    // depth comparison; the other four are conservative bail-outs. A cull rate
    // that looks poor is almost always one bail-out firing on most of the
    // scene, and a screen-wide "occluded vs visible" pair cannot tell which.
    // Only accumulated under BRICK_CULL_MEASURE_AREA, like visibleAreaPixels.
    testedCount : atomic<u32>,
    bailNearPlane : atomic<u32>,
    bailOffscreen : atomic<u32>,
    bailSpan : atomic<u32>,
    bailDegenerate : atomic<u32>,
    // Reached into a coarse mip's uncovered remainder strip, which the pyramid
    // holds no data for. Distinct from bailOffscreen: that one is now clipped
    // away, this one genuinely cannot be answered.
    bailCoverage : atomic<u32>,
    // Failed the depth comparison, i.e. genuinely proven hidden.
    occludedByDepth : atomic<u32>,
    // Proven hidden but redrawn anyway because the two-frame hysteresis has not
    // elapsed. Persistent pressure here means the hysteresis, not the test, is
    // what is costing the fill rate.
    bailHysteresis : atomic<u32>,
    // Trace-only signature of the complete brick LOD cut, accumulated before
    // frustum and HZB rejection so visibility cannot masquerade as LOD churn.
    lodSelectedCount : atomic<u32>,
    lodSelectedHashXor : atomic<u32>,
    lodSelectedHashSum : atomic<u32>,
    lodTransitionCount : atomic<u32>,
}

// Frame-wide exact micro-geometry diagnostics. The two cull phases each reset
// their own SparseVoxelBrickCounters, so this separate block is reset only by
// phase zero and then accumulated by both draw phases.
struct SparseVoxelMicroCounters {
    eligibleParentHits : atomic<u32>,
    descriptorLoads : atomic<u32>,
    acceptedMicroHits : atomic<u32>,
    carvedParents : atomic<u32>,
    parentContinuations : atomic<u32>,
    finalDiscards : atomic<u32>,
    heightfieldSteps : atomic<u32>,
    volumeSteps : atomic<u32>,
    exactTraceInvocations : atomic<u32>,
    fallbackCandidates : atomic<u32>,
    fallbackRejected : atomic<u32>,
    staircaseInteriorCandidates : atomic<u32>,
    staircaseFlatCandidates : atomic<u32>,
    staircaseSlopeCandidates : atomic<u32>,
    staircaseWallCandidates : atomic<u32>,
    staircaseOtherCandidates : atomic<u32>,
    oneFaceInvocations : atomic<u32>,
    oneFaceHits : atomic<u32>,
    oneFaceMisses : atomic<u32>,
    volumeInvocations : atomic<u32>,
    volumeHits : atomic<u32>,
    volumeMisses : atomic<u32>,
    solidInteriorShortcuts : atomic<u32>,
    wallOneFaceEligible : atomic<u32>,
    scale4GroupsTested : atomic<u32>,
    scale4GroupsSkipped : atomic<u32>,
    scale2CellsVisited : atomic<u32>,
    fineOccupancyProbes : atomic<u32>,
    firstHitExits : atomic<u32>,
    traversalMisses : atomic<u32>,
    traceContextInterior : atomic<u32>,
    traceContextFlat : atomic<u32>,
    traceContextSlope : atomic<u32>,
    traceContextWall : atomic<u32>,
    traceContextGeneralVolume : atomic<u32>,
    traceContextOtherHeightfield : atomic<u32>,
}

struct SparseVoxelBrickDrawIndirect {
    vertexCount : u32,
    instanceCount : u32,
    firstVertex : u32,
    firstInstance : u32,
}

fn brickCellIndex(cell : vec3<u32>) -> u32 {
    return cell.x + SPARSE_VOXEL_BRICK_SIDE * (cell.y + SPARSE_VOXEL_BRICK_SIDE * cell.z);
}

fn brickCoarseCellIndex(cell : vec3<u32>) -> u32 {
    return cell.x + SPARSE_VOXEL_BRICK_COARSE_SIDE * (cell.y + SPARSE_VOXEL_BRICK_COARSE_SIDE * cell.z);
}
