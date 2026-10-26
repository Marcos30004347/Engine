// Brick hierarchy culling: frustum, occlusion and LOD selection in one pass.
//
// The hierarchy is a mip chain - a parent brick's voxel is exactly twice its
// child's - but terrain measured error and simplification need the same
// coherence rule as virtual geometry cluster groups. Parent-backed LOD groups
// make the cut decision once for the representative top node: either the
// simplified parent is selected, or the member children are eligible together.
// Nodes without group metadata keep the legacy independent test as a safe
// fallback for older payloads.
#include "sparse_voxel_brick_common.wgsl"
#include "sparse_voxel_hzb_common.wgsl"
#include "sparse_voxel_brick_project.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBrickCullUniforms;
@group(0) @binding(1) var<storage, read> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read_write> visibleBricks : array<u32>;
@group(0) @binding(3) var<storage, read_write> counters : SparseVoxelBrickCounters;
@group(0) @binding(4) var<storage, read_write> drawArgs : SparseVoxelBrickDrawIndirect;
@group(0) @binding(5) var hzb : texture_2d<f32>;
// Per brick: how many consecutive frames it has tested occluded. Zero means
// draw it in phase one.
//
// A plain visible/hidden flag makes phase one's set flip on a single frame's
// test. Under fast rotation the pyramid phase one builds is seeded from a stale
// set, so a brick can test occluded once, drop out, and take several frames to
// be re-admitted - a visible transient. Requiring agreement across consecutive
// frames costs one frame of extra drawing for a brick that really did become
// hidden, and removes the transient entirely.
// The high bits double-buffer LOD ownership. The pending cut is committed by
// a separate dispatch after every cull invocation has finished reading the
// previous cut, avoiding a cross-workgroup parent/child race.
@group(0) @binding(6) var<storage, read_write> previouslyVisible : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> microCounters : SparseVoxelMicroCounters;
// One request bit and one maximum screen-space priority per stable terrain
// page slot. Ordinary brick scenes bind one-word dummies and set capacity zero.
@group(0) @binding(8) var<storage, read_write> terrainPageRequestBits :
    array<atomic<u32>>;
@group(0) @binding(9) var<storage, read_write> terrainPageRequestPriorities :
    array<atomic<u32>>;
@group(0) @binding(10) var<storage, read> terrainPageStates :
    array<SparseVoxelTerrainPageState>;
@group(0) @binding(11) var<storage, read_write> terrainPageRequestGenerations :
    array<atomic<u32>>;
@group(0) @binding(12) var<storage, read_write> terrainPageRequestDesiredLODs :
    array<atomic<u32>>;
@group(0) @binding(13) var<storage, read> lodGroups :
    array<SparseVoxelBrickLODGroup>;
@group(0) @binding(14) var<storage, read> nodeLODGroupIndices : array<u32>;
@group(0) @binding(15) var<storage, read> representativeLODGroupIndices :
    array<u32>;
const TERRAIN_PAGE_STATE_PROGRESSIVE_GENERATED : u32 = 1u;
const BRICK_OCCLUDED_FRAMES_TO_DROP : u32 = 2u;
const BRICK_HISTORY_OCCLUSION_MASK : u32 = 3u;
// Set by phase zero only when the brick really contributed to this frame's
// occluder seed. Phase one consults this bit instead of recomputing the seed
// predicate, because phase zero may reject a candidate against a valid previous
// HZB.
const BRICK_HISTORY_PHASE_ZERO_DRAWN : u32 = 1u << 28u;
const BRICK_HISTORY_LOD_INITIALIZED : u32 = 1u << 29u;
const BRICK_HISTORY_PREVIOUS_LOD : u32 = 1u << 30u;
const BRICK_HISTORY_PENDING_LOD : u32 = 1u << 31u;
// Camera motion used to cross the 15% band often enough that an entire
// parent/child cut changed on consecutive frames. The cut itself remained
// complete, but its depth, material identity and probe receivers all changed
// together and the transition read as a rectangular flash. A 25% band still
// uses the same parent/child threshold (and therefore remains crack-free), but
// makes the representation sticky enough to survive ordinary dolly motion and
// sub-pixel projection changes.
const BRICK_LOD_REFINE_HYSTERESIS : f32 = 1.25;
const BRICK_LOD_COARSEN_HYSTERESIS : f32 = 0.75;

const BRICK_CULL_FRUSTUM : u32 = 1u;
const BRICK_CULL_OCCLUSION : u32 = 2u;
const BRICK_CULL_REVERSE_Z : u32 = 4u;
// Diagnostic only: see SparseVoxelBrickCounters::visibleAreaPixels.
const BRICK_CULL_MEASURE_AREA : u32 = 8u;
// Diagnostic only: the shipping raster shader never writes microCounters, so
// phase zero skips clearing that bank unless its diagnostic variant is active.
const BRICK_CULL_RASTER_DIAGNOSTICS : u32 = 16u;
// Trace-only cut signature. Disabled in shipping frames because global atomics
// per selected brick would distort the cull timing being measured.
const BRICK_CULL_LOD_DIAGNOSTICS : u32 = 32u;

// Phase zero is an occluder seed, not the complete previous visible set. This
// follows the same two-step shape as Niagara's early/late cull path, but bricks
// are much fatter screen proxies than meshlets: drawing every large brick in
// the early pass creates the overdraw we are trying to avoid. Use an
// area-weighted deterministic sample instead. Omitting an occluder only weakens
// rejection and is therefore conservative; phase one still owns final visibility.
const BRICK_PHASE_ZERO_MINIMUM_AREA_PIXELS : f32 = 1024.0;
const BRICK_PHASE_ZERO_SMALL_SAMPLE_MASK : u32 = 31u;
const BRICK_PHASE_ZERO_MEDIUM_AREA_PIXELS : f32 = 4096.0;
const BRICK_PHASE_ZERO_LARGE_AREA_PIXELS : f32 = 16384.0;
const BRICK_PHASE_ZERO_MEDIUM_SAMPLE_MASK : u32 = 7u;
const BRICK_PHASE_ZERO_LARGE_SAMPLE_MASK : u32 = 3u;

// Projected size of one voxel of this node, in pixels. Distance is to the
// nearest point of the brick, so a node the camera is inside never reports a
// tiny footprint and never gets selected over its children.
fn brickProjectedVoxelPixels(bounds : BrickBounds, voxelSize : f32) -> f32 {
    let closest = clamp(vec3<f32>(0.0), bounds.minimum, bounds.maximum);
    let distance = max(length(closest), 1.0e-4);
    return voxelSize * uniforms.focalPrincipal.x / distance;
}

// Generated worlds keep fixed-size chunk owners and use the same hierarchy
// cut as every other brick scene. This curve changes only that cut's error
// metric; it does not create spatial rings or select a second geometry path.
// The multiplier is dimensionless, so raster LOD and output-scaled page
// feedback agree even when the renderer runs below display resolution.
fn brickTerrainDistanceErrorMultiplier(bounds : BrickBounds) -> f32 {
    if (uniforms.terrainFeedbackEnabled == 0u ||
        !(uniforms.terrainDistanceErrorGrowthPerKilometre > 0.0)) {
        return 1.0;
    }
    let closest = clamp(vec3<f32>(0.0), bounds.minimum, bounds.maximum);
    let distanceKilometres = length(closest) * 0.001;
    let exponent = clamp(uniforms.terrainDistanceErrorExponent, 0.125, 8.0);
    return 1.0 + uniforms.terrainDistanceErrorGrowthPerKilometre *
        pow(max(distanceKilometres, 0.0), exponent);
}

// The cut metric. `error` is the brick's measured deviation from the finest
// representation of the same volume, not its voxel size: a coarsened flat plain
// deviates by almost nothing while a coarsened cliff loses most of a voxel, and
// projecting voxel size cannot tell them apart. The builder floors it at a
// fraction of the voxel, so a brick that is geometrically exact but materially
// varied still refines with distance.
fn brickLODMetricPixels(bounds : BrickBounds, error : f32) -> f32 {
    return brickProjectedVoxelPixels(bounds, error) /
        brickTerrainDistanceErrorMultiplier(bounds);
}

fn brickWasInLOD(index : u32) -> bool {
    return (atomicLoad(&previouslyVisible[index]) &
        BRICK_HISTORY_PREVIOUS_LOD) != 0u;
}

// Give the previous cut a 25% dead band. Ungrouped nodes still use the legacy
// parent/child hysteresis. Grouped nodes use the representative node's history
// so all members flip together.
fn brickStableLODThreshold(index : u32, node : SparseVoxelBrickNode) -> f32 {
    let threshold = max(uniforms.lodPixelThreshold, 0.0);
    if (threshold == 0.0) {
        return 0.0;
    }
    let history = atomicLoad(&previouslyVisible[index]);
    if ((history & BRICK_HISTORY_LOD_INITIALIZED) == 0u) {
        return threshold;
    }
    if (brickWasInLOD(index)) {
        return threshold * BRICK_LOD_REFINE_HYSTERESIS;
    }
    return threshold * BRICK_LOD_COARSEN_HYSTERESIS;
}

fn brickValidLODGroupIndex(groupIndex : u32) -> bool {
    return groupIndex != SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX &&
        groupIndex < uniforms.lodGroupCount &&
        groupIndex < arrayLength(&lodGroups);
}

fn brickNodeFullBoundsCameraRelative(node : SparseVoxelBrickNode) -> BrickBounds {
    return brickFullBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
}

fn brickGroupSelectsRepresentative(groupIndex : u32) -> bool {
    if (!brickValidLODGroupIndex(groupIndex)) {
        return false;
    }
    let group = lodGroups[groupIndex];
    let representativeIndex = group.representativeNode;
    if (representativeIndex == SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX ||
        representativeIndex >= uniforms.nodeCount ||
        representativeIndex >= arrayLength(&nodes)) {
        return false;
    }
    let representative = nodes[representativeIndex];
    if (representative.occupiedCount == 0u ||
        representative.minimumVoxelSize.w <= 0.0 ||
        (representative.level & SPARSE_VOXEL_BRICK_FORCE_LEAF_RASTER) != 0u) {
        return false;
    }
    if (representative.reservedTailPadding != 0u) {
        let certifiedSurfaceRepresentative =
            (group.flags &
                SPARSE_VOXEL_BRICK_LOD_GROUP_SURFACE_REPRESENTATIVE_CERTIFIED) != 0u;
        if (representative.childCount != 8u ||
            (!certifiedSurfaceRepresentative &&
                ((group.flags & SPARSE_VOXEL_BRICK_LOD_GROUP_BOUNDARY_MASK) != 0u ||
                 representative.occupiedCount != SPARSE_VOXEL_BRICK_VOXEL_COUNT))) {
            // GPU-generated terrain pages can be refined independently by
            // streaming, and their visible surface is a heightfield silhouette.
            // A complete solid interior is safe by construction. A non-solid
            // or page-boundary parent also needs the publication-side
            // seam-certified surface-representative bit, which means the parent
            // was built from the same stitched conservative surface data as its
            // children. Without either proof, force children to own the cut.
            return false;
        }
    }

    // The group representative is the simplification. Its error controls the
    // parent-backed group cut, while the group's boundary flags identify faces
    // that later seam/neighbor logic must treat as edge-locked. The first GPU
    // consumer is conservative about topology by requiring an actual
    // representative node; unrepresented buckets never coarsen as a group.
    let groupMetric = select(
        representative.minimumVoxelSize.w,
        group.sharedError,
        group.sharedError > 0.0);
    let groupPixels = brickLODMetricPixels(
        brickNodeFullBoundsCameraRelative(representative), groupMetric);
    let groupThreshold =
        brickStableLODThreshold(representativeIndex, representative);
    if (groupPixels > groupThreshold) {
        return false;
    }

    return true;
}

struct BrickGroupLODOverride {
    enabled : bool,
    selected : bool,
    bypassParentLOD : bool,
}

fn brickGroupLODOverrideForNode(
    index : u32, node : SparseVoxelBrickNode) -> BrickGroupLODOverride {
    if (uniforms.lodGroupCount == 0u) {
        return BrickGroupLODOverride(false, false, false);
    }
    var bypassParentLOD = false;
    if (index < arrayLength(&nodeLODGroupIndices)) {
        let memberGroup = nodeLODGroupIndices[index];
        if (brickValidLODGroupIndex(memberGroup)) {
            let group = lodGroups[memberGroup];
            if (group.representativeNode !=
                SPARSE_VOXEL_BRICK_LOD_GROUP_INVALID_INDEX) {
                if (brickGroupSelectsRepresentative(memberGroup)) {
                    return BrickGroupLODOverride(true, false, false);
                }
                // The representative lost; children now continue through the
                // leaf/self test, but must not be rejected again by the same
                // parent in the legacy parent test below. Because every member
                // sees the same representative decision, the group refines as
                // one patch.
                bypassParentLOD = true;
            }
        }
    }
    if (index < arrayLength(&representativeLODGroupIndices)) {
        let representedGroup = representativeLODGroupIndices[index];
        if (brickValidLODGroupIndex(representedGroup)) {
            return BrickGroupLODOverride(
                true,
                brickGroupSelectsRepresentative(representedGroup),
                bypassParentLOD);
        }
    }
    return BrickGroupLODOverride(false, false, bypassParentLOD);
}

fn brickLODSelectionHash(index : u32, node : SparseVoxelBrickNode) -> u32 {
    var value = index ^ ((node.level & 0xffffu) * 0x9e3779b9u);
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    value ^= value >> 16u;
    return value;
}

fn brickPhaseZeroOccluderSeed(
    index : u32, node : SparseVoxelBrickNode,
    projection : BrickProjection) -> bool {
    if (projection.crossesNearPlane) {
        // Camera-near geometry is both the strongest occluder and the least
        // trustworthy thing to test against a projected rectangle.
        return true;
    }
    let viewport = vec2<f32>(uniforms.viewport.xy);
    let minimum = clamp(
        projection.screenMinimum, vec2<f32>(0.0), vec2<f32>(1.0));
    let maximum = clamp(
        projection.screenMaximum, vec2<f32>(0.0), vec2<f32>(1.0));
    let extent = max((maximum - minimum) * viewport, vec2<f32>(0.0));
    let area = extent.x * extent.y;
    let hash = brickLODSelectionHash(index, node);
    if (area >= BRICK_PHASE_ZERO_LARGE_AREA_PIXELS) {
        return (hash & BRICK_PHASE_ZERO_LARGE_SAMPLE_MASK) == 0u;
    }
    if (area >= BRICK_PHASE_ZERO_MEDIUM_AREA_PIXELS) {
        return (hash & BRICK_PHASE_ZERO_MEDIUM_SAMPLE_MASK) == 0u;
    }
    if (area >= BRICK_PHASE_ZERO_MINIMUM_AREA_PIXELS) {
        return (hash & BRICK_PHASE_ZERO_SMALL_SAMPLE_MASK) == 0u;
    }
    return (hash & BRICK_PHASE_ZERO_SMALL_SAMPLE_MASK) == 0u;
}

fn brickPublishPendingLOD(
    index : u32,
    node : SparseVoxelBrickNode,
    selected : bool) {
    if (uniforms.phase == 0u) {
        return;
    }
    let history = atomicLoad(&previouslyVisible[index]);
    if ((uniforms.cullingFlags & BRICK_CULL_LOD_DIAGNOSTICS) != 0u) {
        let wasSelected =
            (history & BRICK_HISTORY_PREVIOUS_LOD) != 0u;
        if ((history & BRICK_HISTORY_LOD_INITIALIZED) != 0u &&
            wasSelected != selected) {
            atomicAdd(&counters.lodTransitionCount, 1u);
        }
        if (selected) {
            let hash = brickLODSelectionHash(index, node);
            atomicAdd(&counters.lodSelectedCount, 1u);
            atomicXor(&counters.lodSelectedHashXor, hash);
            atomicAdd(&counters.lodSelectedHashSum, hash);
        }
    }
    if (selected) {
        atomicOr(&previouslyVisible[index], BRICK_HISTORY_PENDING_LOD);
    } else {
        atomicAnd(&previouslyVisible[index], ~BRICK_HISTORY_PENDING_LOD);
    }
}

// Conservative HZB test, mirroring the virtual geometry occlusion test.
//
// Four things make this conservative, and leaving any of them out rejects
// geometry that is really visible:
//
//   1. Anything crossing the near plane is kept unconditionally - its corners
//      have w <= 0 and the screen rectangle means nothing.
//
//      A rectangle that merely leaves the screen is *clipped* to it, not bailed
//      on. A brick is only ever visible where it is on screen, so the pixels
//      that matter are exactly its on-screen footprint, and testing over that
//      sub-rectangle is both correct and strictly stronger: restricting the
//      region can only make the reduced occluder harder to beat. Bailing here
//      instead was measured as 57% of everything the interior camera drew -
//      1673 of 2918 bricks were drawn purely because the test declined to
//      judge them. What is *not* safe is clipping to the coarse mip's covered
//      strip, which is a region the pyramid genuinely has no data for; that
//      one still bails.
//   2. The sample covers the rectangle's own texel span, not a fixed window
//      centred on it. A 2x2 window is only guaranteed to cover a rectangle that
//      fits inside one texel; anything wider leaves a corner untested, and an
//      untested corner is where a visible brick gets culled.
//   3. The occluder is the farthest depth over that span - the same reduction
//      the pyramid performs one level up.
//   4. The nearest depth handed to this test comes from a box dilated by one
//      voxel (see `occlusionDilationVoxels` where the projections are taken).
//      The screen rectangle still comes from the tight occupied box: that is
//      the proxy the draw pass actually submits, so expanding the sampled HZB
//      region only pulls unrelated depth discontinuities into the reduction.
//      The pyramid is rasterized from bricks selected by the same LOD cut, and
//      a coarse brick's OR-reduced occupancy dilates its surface by up to one
//      coarse voxel - so the depth in the pyramid is *nearer* than the geometry
//      it stands for. Without the depth margin, a brick sitting just behind a
//      coarse neighbour is culled even though it is genuinely visible, and it
//      flickers as the cut moves with the camera. Projecting the dilated depth
//      bound rather than biasing the depth value keeps this exact for both
//      forward and reverse Z.
fn brickVisibleAgainstHZB(projection : BrickProjection, count : bool) -> bool {
    if (count) { atomicAdd(&counters.testedCount, 1u); }
    if (projection.crossesNearPlane) {
        // Straddles the near plane; keeping it is the only safe answer.
        if (count) { atomicAdd(&counters.bailNearPlane, 1u); }
        return true;
    }
    // Clip to the viewport. Only the on-screen part of a brick can be seen, so
    // this is the region the test has to cover - and a smaller region only ever
    // culls more.
    let screenMinimum = clamp(
        projection.screenMinimum, vec2<f32>(0.0), vec2<f32>(1.0));
    let screenMaximum = clamp(
        projection.screenMaximum, vec2<f32>(0.0), vec2<f32>(1.0));
    if (any(screenMaximum < screenMinimum)) {
        // Entirely off screen. The frustum test owns this case; keeping it
        // costs one brick and avoids disagreeing with a different rejection.
        if (count) { atomicAdd(&counters.bailOffscreen, 1u); }
        return true;
    }

    let firstValidMip = i32(uniforms.hzbFirstValidMip);
    let levelCount = i32(textureNumLevels(hzb));
    let fullDimensions = vec2<f32>(textureDimensions(hzb, 0));
    let baseDimensions =
        sparseVoxelHZBTexelsPerUV(fullDimensions, firstValidMip);
    let sizeTexels = max(
        (screenMaximum.x - screenMinimum.x) * baseDimensions.x,
        (screenMaximum.y - screenMinimum.y) * baseDimensions.y);
    if (!(sizeTexels > 0.0)) {
        if (count) { atomicAdd(&counters.bailDegenerate, 1u); }
        return true;
    }
    // No extra coarsening: the sample loop below covers the rectangle's own
    // texel span, so the safety margin a single centre tap would need is
    // already paid for. Coarsening on top of it only loses culling.
    let desiredLevel = clamp(
        firstValidMip + i32(max(ceil(log2(max(sizeTexels, 1.0))), 0.0)),
        firstValidMip,
        levelCount - 1);

    // A coarse mip covers only floor(base / 2^level) * 2^level pixels; the
    // remainder at the right and bottom is never reduced into it, and nothing
    // can be said about a brick reaching into that strip.
    //
    // Rather than give up, step to a finer level - each one loses a smaller
    // remainder, and mip 0 loses none - until the rectangle is covered. The
    // cost is a wider texel span, which the sample loop below already handles
    // and which the span cap bounds; on this content that cap never fires, so
    // the headroom is free. Only a rectangle that still is not covered at the
    // finest usable level bails, which is the genuinely unanswerable case.
    var level = desiredLevel;
    var mipDimensions = vec2<f32>(textureDimensions(hzb, level));
    var covered =
        sparseVoxelHZBCoveredFraction(fullDimensions, mipDimensions, level);
    loop {
        if (!any(screenMaximum > covered) || level <= firstValidMip) { break; }
        level -= 1;
        mipDimensions = vec2<f32>(textureDimensions(hzb, level));
        covered =
            sparseVoxelHZBCoveredFraction(fullDimensions, mipDimensions, level);
    }
    if (any(screenMaximum > covered)) {
        if (count) { atomicAdd(&counters.bailCoverage, 1u); }
        return true;
    }
    // Texels per unit UV, derived from the base dimensions rather than the
    // stored mip size. See `sparseVoxelHZBTexelsPerUV`: the stored size rounds
    // down, and scaling UV by it stretches the mip across the full screen so
    // every texel is read at an offset that grows toward the far edge.
    let texelsPerUV = sparseVoxelHZBTexelsPerUV(fullDimensions, level);
    let maxTexel = vec2<i32>(mipDimensions) - vec2<i32>(1);
    let lowTexel = clamp(
        vec2<i32>(floor(screenMinimum * texelsPerUV - vec2<f32>(0.5))),
        vec2<i32>(0),
        maxTexel);
    let highTexel = clamp(
        vec2<i32>(ceil(screenMaximum * texelsPerUV + vec2<f32>(0.5))),
        vec2<i32>(0),
        maxTexel);
    // The chain runs out of levels before the coarsest bricks run out of
    // screen area, and at that point the span is unbounded - a near brick can
    // cover the whole coarsest mip. Reading all of it would cost hundreds of
    // taps for the least useful test in the pass, so bail out as visible
    // instead.
    //
    // Swept on the interior camera, which is the one with depth complexity.
    // The bricks that trip this are the large near ones, so they are worth more
    // than their count suggests - they are what dominates overdraw:
    //
    //   cap |  drawn | occluded | bailSpan
    //     4 |   2022 |     4630 |      322
    //     6 |   1955 |     4690 |      248
    //     8 |   1829 |     4816 |      109
    //    12 |   1798 |     4847 |       12
    //
    // The tight occupied bounds reduce the frequency of wide spans enough
    // that twelve taps per axis is a net win: it lets the HZB reject the
    // remaining large occluded bricks instead of sending them through the
    // fragment DDA. The cap is still finite, so a near, screen-filling brick
    // cannot turn one culling lane into an unbounded texture walk.
    const BRICK_HZB_MAXIMUM_TEXEL_SPAN : i32 = 12;
    if (highTexel.x - lowTexel.x > BRICK_HZB_MAXIMUM_TEXEL_SPAN ||
        highTexel.y - lowTexel.y > BRICK_HZB_MAXIMUM_TEXEL_SPAN) {
        if (count) { atomicAdd(&counters.bailSpan, 1u); }
        return true;
    }

    let reverseZ = (uniforms.cullingFlags & BRICK_CULL_REVERSE_Z) != 0u;
    var occluder = select(-1.0e30, 1.0e30, reverseZ);
    for (var y = lowTexel.y; y <= highTexel.y; y += 1) {
        for (var x = lowTexel.x; x <= highTexel.x; x += 1) {
            let sampled = textureLoad(hzb, vec2<i32>(x, y), level).x;
            occluder = select(max(occluder, sampled), min(occluder, sampled), reverseZ);
        }
    }

    // Ties stay visible: coplanar geometry must not cull itself.
    let visible = select(
        projection.nearestDepth <= occluder,
        projection.nearestReverseDepth >= occluder,
        reverseZ);
    if (count && !visible) { atomicAdd(&counters.occludedByDepth, 1u); }
    return visible;
}

fn recordTerrainPageFeedback(
    node : SparseVoxelBrickNode,
    selfPixels : f32) {
    let terrainPageLOD = (node.level >> 8u) & 0xffu;
    let terrainPageSlot = node.instanceIndex;
    if (uniforms.terrainFeedbackEnabled == 0u ||
        brickHasFinerTerrainClip(node) ||
        terrainPageSlot >= uniforms.terrainPageRequestCapacity ||
        terrainPageSlot >= arrayLength(&terrainPageStates)) {
        return;
    }

    let pageState = terrainPageStates[terrainPageSlot];
    let age = select(
        0u,
        uniforms.terrainFrameIndex - pageState.publishedFrame,
        uniforms.terrainFrameIndex >= pageState.publishedFrame);
    let outputPixels = selfPixels * uniforms.terrainOutputPixelScale;
    let splitError = max(
        uniforms.terrainSplitErrorPixels,
        uniforms.terrainMergeErrorPixels);
    let validOwner =
        (pageState.flags & TERRAIN_PAGE_STATE_PROGRESSIVE_GENERATED) != 0u &&
        pageState.generation != 0u &&
        pageState.currentLOD == terrainPageLOD;
    let canRefine = validOwner && terrainPageLOD != 0u &&
        age >= uniforms.terrainMinimumResidentFrames &&
        outputPixels > splitError;
    let word = terrainPageSlot >> 5u;
    if (validOwner &&
        terrainPageSlot < arrayLength(&terrainPageRequestPriorities) &&
        terrainPageSlot < arrayLength(&terrainPageRequestGenerations)) {
        atomicMax(
            &terrainPageRequestPriorities[terrainPageSlot],
            u32(clamp(outputPixels * 65536.0, 1.0, 4294967040.0)));
        atomicStore(
            &terrainPageRequestGenerations[terrainPageSlot],
            pageState.generation);
    }
    if (canRefine && word < arrayLength(&terrainPageRequestBits) &&
        terrainPageSlot < arrayLength(&terrainPageRequestPriorities) &&
        terrainPageSlot < arrayLength(&terrainPageRequestGenerations) &&
        terrainPageSlot < arrayLength(&terrainPageRequestDesiredLODs)) {
        atomicOr(
            &terrainPageRequestBits[word],
            1u << (terrainPageSlot & 31u));
        atomicMin(
            &terrainPageRequestDesiredLODs[terrainPageSlot],
            pageState.currentLOD - 1u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn cullBricks(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index >= uniforms.nodeCount) {
        return;
    }
    if (uniforms.phase == 0u) {
        atomicAnd(&previouslyVisible[index], ~BRICK_HISTORY_PHASE_ZERO_DRAWN);
    }
    let node = nodes[index];
    // Terrain clipmaps use fixed-capacity page slots so an entering page can
    // rewrite only its own ranges. Unused capacity is zero-filled and never
    // participates in the LOD cut.
    if (node.occupiedCount == 0u || node.minimumVoxelSize.w <= 0.0) {
        brickPublishPendingLOD(index, node, false);
        return;
    }
    let terrainPageLOD = (node.level >> 8u) & 0xffu;
    let possibleStreamedTerrainNode =
        node.reservedTailPadding != 0u &&
        node.instanceIndex < uniforms.terrainPageRequestCapacity &&
        node.instanceIndex < arrayLength(&terrainPageStates);
    if (possibleStreamedTerrainNode) {
        let owner = terrainPageStates[node.instanceIndex];
        let streamedTerrainNode = owner.generation != 0u &&
            (owner.flags & TERRAIN_PAGE_STATE_PROGRESSIVE_GENERATED) != 0u;
        if (!streamedTerrainNode ||
            owner.generation != node.reservedTailPadding ||
            owner.currentLOD != terrainPageLOD) {
            brickPublishPendingLOD(index, node, false);
            return;
        }
    }
    let voxelSize = node.minimumVoxelSize.w;
    // LOD deliberately remains based on the full grid cell: its monotonic
    // parent/child relationship is what gives the hierarchy a crack-free cut.
    // Frustum/HZB culling instead follows the occupied box the draw pass uses.
    let bounds = brickBounds(node);
    let occupiedBounds = brickOccupiedBounds(node);
    let cameraRelativeBounds = brickFullBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
    let occupiedCameraRelativeBounds = brickOccupiedBoundsCameraRelative(
        node, uniforms.canonicalWorldGrid, uniforms.canonicalCameraCell,
        uniforms.canonicalCameraOffset, uniforms.cameraWorldPosition);
    let finer = brickFinerTerrainClipBounds(node);
    if (brickHasFinerTerrainClip(node) &&
        occupiedBounds.minimum.x >= finer.x &&
        occupiedBounds.maximum.x <= finer.z &&
        occupiedBounds.minimum.z >= finer.y &&
        occupiedBounds.maximum.z <= finer.w) {
        // This brick is wholly owned by a finer terrain cascade. Resetting its
        // visibility history lets the same physical slot re-enter immediately
        // when a later streamed window moves the ownership boundary.
        if (uniforms.phase != 0u) {
            atomicAnd(&previouslyVisible[index],
                ~(BRICK_HISTORY_PENDING_LOD | BRICK_HISTORY_OCCLUSION_MASK));
        }
        return;
    }

    // The LOD cut, first because it rejects the overwhelming majority of nodes.
    // Parent-backed groups behave like virtual-geometry cluster groups: the
    // representative parent either wins for the whole group or all children are
    // allowed to refine together. Older/unrepresented payloads fall back to the
    // historical per-node monotonic test below.
    let groupOverride = brickGroupLODOverrideForNode(index, node);
    var selfPixels = 0.0;
    if (groupOverride.enabled) {
        brickPublishPendingLOD(index, node, groupOverride.selected);
        if (!groupOverride.selected) {
            return;
        }
        let representativeMetric =
            select(voxelSize, node.selfError, node.selfError > 0.0);
        selfPixels =
            brickLODMetricPixels(cameraRelativeBounds, representativeMetric);
    } else {
        // A leaf is always fine enough - there is nothing finer to refine to -
        // and a root has no parent to have been too coarse. The hierarchy
        // builder floors measured error at a fraction of the brick's voxel, so
        // a positive value always means "measured". Zero means the node came
        // from a producer that predates the field, and projecting zero would
        // read as "arbitrarily coarse is acceptable" and collapse the cut to
        // its roots. Fall back to the voxel size those producers were written
        // against.
        let selfMetric = select(voxelSize, node.selfError, node.selfError > 0.0);
        selfPixels = brickLODMetricPixels(cameraRelativeBounds, selfMetric);
        let selfThreshold = brickStableLODThreshold(index, node);
        // Pages that carry FORCE_LEAF_RASTER use exact authored leaves.
        // Generated spatial pages may omit the flag once their parent bricks
        // carry measured heightfield error; then the ordinary projected-error
        // test below selects only coarse parents whose raster deviation is
        // below the pixel threshold.
        let forceLeafRaster =
            (node.level & SPARSE_VOXEL_BRICK_FORCE_LEAF_RASTER) != 0u;
        let fineEnough = node.childCount == 0u ||
            (!forceLeafRaster && selfPixels <= selfThreshold);
        if (!fineEnough) {
            brickPublishPendingLOD(index, node, false);
            return;
        }
        if (!groupOverride.bypassParentLOD &&
            !forceLeafRaster &&
            node.parentIndex != SPARSE_VOXEL_BRICK_INVALID_INDEX) {
            let parent = nodes[node.parentIndex];
            // node.parentError rather than the fetched parent's selfError: the
            // two are the same value, and the stored copy is what keeps
            // monotonicity a property of the data instead of a second load that
            // could disagree.
            let parentMetric = select(
                parent.minimumVoxelSize.w, node.parentError,
                node.parentError > 0.0 && node.selfError > 0.0);
            let parentPixels = brickLODMetricPixels(
                brickNodeFullBoundsCameraRelative(parent), parentMetric);
            let parentThreshold = brickStableLODThreshold(
                node.parentIndex, parent);
            if (parentPixels <= parentThreshold) {
                // The parent already covers this volume at acceptable detail,
                // so it is the cut and this node is below it.
                brickPublishPendingLOD(index, node, false);
                return;
            }
        }
        brickPublishPendingLOD(index, node, true);
    }

    let frustumEnabled = (uniforms.cullingFlags & BRICK_CULL_FRUSTUM) != 0u;
    let occlusionEnabled = (uniforms.cullingFlags & BRICK_CULL_OCCLUSION) != 0u;
    let measureArea = (uniforms.cullingFlags & BRICK_CULL_MEASURE_AREA) != 0u;
    // Frustum rejection and the HZB footprint use the tight occupied proxy the
    // draw submits. Occlusion depth receives a separately projected safety
    // dilation below; coupling that dilation to the screen rectangle made a
    // brick sample unrelated neighbours and needlessly survive the HZB.
    // Phase zero also projects candidates to choose the current-frame
    // occluder seed. Previously an HZB-enabled run with frustum culling
    // disabled passed an uninitialized projection to that selector and could
    // erase the complete seed (and therefore the complete frame). Projection
    // is required by either feature in both phases.
    let needsProjection = frustumEnabled || occlusionEnabled;
    // Dilated by one of this brick's own voxels, which is the bound on how far
    // an LOD-coarse brick's OR-reduced surface can sit in front of the geometry
    // it stands for. Point 4 on brickVisibleAgainstHZB has the argument.
    let dilation = voxelSize * uniforms.occlusionDilationVoxels;
    var projection : BrickProjection;
    if (needsProjection) {
        projection = brickProjectCameraRelative(
            occupiedCameraRelativeBounds,
            uniforms.worldToClip, uniforms.cameraWorldPosition.w);
        if (uniforms.phase != 0u && occlusionEnabled && dilation > 0.0) {
            let depthProjection = brickProjectCameraRelativeDepthBounds(
                BrickBounds(
                    occupiedCameraRelativeBounds.minimum -
                        vec3<f32>(dilation),
                    occupiedCameraRelativeBounds.maximum +
                        vec3<f32>(dilation)),
                uniforms.worldToClip, uniforms.cameraWorldPosition.w);
            // A dilated box reaching the near plane makes its depth bound
            // unusable, but does not change the exact proxy footprint or its
            // frustum classification.
            projection.crossesNearPlane =
                projection.crossesNearPlane ||
                depthProjection.crossesNearPlane;
            projection.nearestDepth = depthProjection.nearestDepth;
            projection.nearestReverseDepth =
                depthProjection.nearestReverseDepth;
        }
    }
    var occupiedProjection : BrickProjection;
    if (measureArea) {
        occupiedProjection = brickProjectCameraRelative(
            occupiedCameraRelativeBounds, uniforms.worldToClip,
            uniforms.cameraWorldPosition.w);
    }
    let inFrustum = !frustumEnabled || !projection.outsideFrustum;

    // Phase one redraws last frame's visible set with no occlusion test - it
    // exists to produce this frame's depth, so testing it against last frame's
    // pyramid would defeat the point. Phase two tests everything else against
    // the pyramid phase one just built, which is why occlusion survives camera
    // motion here where a single-phase test collapses.
    let history = atomicLoad(&previouslyVisible[index]);
    let occludedFrames = history & BRICK_HISTORY_OCCLUSION_MASK;
    let wasVisible = occludedFrames < BRICK_OCCLUDED_FRAMES_TO_DROP;
    let phaseZeroSeed = wasVisible &&
        (!occlusionEnabled ||
         brickPhaseZeroOccluderSeed(index, node, projection));
    if (uniforms.phase == 0u) {
        if (!phaseZeroSeed || !inFrustum) {
            return;
        }
        if (occlusionEnabled && uniforms.hzbHistoryValid != 0u &&
            !brickVisibleAgainstHZB(projection, measureArea)) {
            atomicStore(
                &previouslyVisible[index],
                (history & ~(BRICK_HISTORY_OCCLUSION_MASK |
                    BRICK_HISTORY_PHASE_ZERO_DRAWN)) |
                    min(occludedFrames + 1u, BRICK_OCCLUDED_FRAMES_TO_DROP));
            return;
        }
        atomicOr(&previouslyVisible[index], BRICK_HISTORY_PHASE_ZERO_DRAWN);
    } else {
        let phaseZeroDrawn =
            (history & BRICK_HISTORY_PHASE_ZERO_DRAWN) != 0u;
        // A brick actually drawn by phase zero is already in the target and
        // therefore in the HZB. Testing it can only find its own depth tie.
        // Keep its history live and avoid the texture walk; non-drawn seeds
        // below are still judged by phase one's pyramid.
        if (phaseZeroDrawn) {
            atomicStore(
                &previouslyVisible[index],
                history & ~(BRICK_HISTORY_OCCLUSION_MASK |
                    BRICK_HISTORY_PHASE_ZERO_DRAWN));
            if (inFrustum) {
                let pagePixels = brickLODMetricPixels(
                    cameraRelativeBounds, voxelSize);
                recordTerrainPageFeedback(node, pagePixels);
            }
            return;
        }
        // Phase two owns the flag for the next frame, and it has to evaluate
        // occlusion for *every* brick to do that - including ones phase one
        // already drew. Skipping the test for those would make the flag
        // monotonic: once visible, always visible, so phase one would keep
        // redrawing the whole scene and occlusion could never take effect.
        // Out of frustum already loses, so the HZB test cannot change the
        // answer - and running it there is not free: every brick behind the
        // eye has corners with w <= 0 and takes the near-plane bail, which both
        // wasted the work and buried the real near-plane count in the
        // attribution.
        var unoccluded = true;
        if (occlusionEnabled && inFrustum) {
            unoccluded = brickVisibleAgainstHZB(projection, measureArea);
        }
        let visibleNow = inFrustum && unoccluded;
        // Out of frustum resets rather than accumulates: re-entering the
        // frustum should not inherit an occlusion history from a different
        // view.
        let nextOccludedFrames = select(
            0u,
            min(occludedFrames + 1u, BRICK_OCCLUDED_FRAMES_TO_DROP),
            inFrustum && !unoccluded);
        atomicStore(
            &previouslyVisible[index],
            (history & ~BRICK_HISTORY_OCCLUSION_MASK) |
                nextOccludedFrames);
        if (!visibleNow && (!wasVisible || !inFrustum)) {
            if (inFrustum) {
                atomicAdd(&counters.occludedCount, 1u);
            }
            return;
        }
        // A recently visible non-seed gets the existing two-frame grace, but
        // it is drawn here after the HZB test rather than assumed to have been
        // drawn in phase zero. This preserves hysteresis without poisoning the
        // occluder pyramid with every candidate's own depth.
        if (!visibleNow && measureArea) {
            atomicAdd(&counters.bailHysteresis, 1u);
        }
    }

    // Screen footprint of what actually gets drawn, for the overdraw metric.
    // Off by default: the atomic is global and every surviving brick contends
    // for it, which costs more than the rest of the pass put together.
    if (measureArea && !occupiedProjection.crossesNearPlane) {
        let viewport = vec2<f32>(uniforms.viewport.xy);
        let areaMinimum = max(
            occupiedProjection.screenMinimum * viewport, vec2<f32>(0.0));
        let areaMaximum = min(
            occupiedProjection.screenMaximum * viewport, viewport);
        let extent = max(areaMaximum - areaMinimum, vec2<f32>(0.0));
        atomicAdd(&counters.visibleAreaPixels, u32(extent.x * extent.y));
    }

    // Spatial page publication and CPU cut construction both use geometric
    // voxel size as page error. Feeding measured leaf error here made the GPU
    // merge a page after its grace period even though the CPU's full-voxel
    // metric immediately split the parent again, producing a coarse/fine loop.
    // Brick raster LOD may use measured error; page ownership may not mix the
    // two metrics.
    if (uniforms.phase != 0u) {
        let pagePixels = brickLODMetricPixels(
            cameraRelativeBounds, voxelSize);
        recordTerrainPageFeedback(node, pagePixels);
    }

    let slot = atomicAdd(&counters.visibleCount, 1u);
    if (slot < uniforms.visibleCapacity && slot < arrayLength(&visibleBricks)) {
        visibleBricks[slot] = index;
    } else {
        atomicStore(&counters.overflow, 1u);
    }
}

// Commit only after cullBricks has finished globally. The render graph inserts
// the storage dependency between these dispatches, so every invocation reads
// one immutable previous cut and the next frame observes the complete new cut.
@compute @workgroup_size(64, 1, 1)
fn commitBrickLODHistory(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index >= uniforms.nodeCount) {
        return;
    }
    let history = atomicLoad(&previouslyVisible[index]);
    let selected = (history & BRICK_HISTORY_PENDING_LOD) != 0u;
    var committed = (history | BRICK_HISTORY_LOD_INITIALIZED) &
        ~(BRICK_HISTORY_PREVIOUS_LOD | BRICK_HISTORY_PENDING_LOD);
    if (selected) {
        committed |= BRICK_HISTORY_PREVIOUS_LOD;
    }
    atomicStore(&previouslyVisible[index], committed);
}

// Three camera-facing cube faces per visible brick, two triangles each.
const BRICK_VERTICES_PER_INSTANCE : u32 = 18u;

@compute @workgroup_size(1, 1, 1)
fn prepareBrickDrawArgs() {
    let visible = min(
        atomicLoad(&counters.visibleCount), uniforms.visibleCapacity);
    drawArgs.vertexCount = BRICK_VERTICES_PER_INSTANCE;
    drawArgs.instanceCount = visible;
    drawArgs.firstVertex = 0u;
    drawArgs.firstInstance = 0u;
}

@compute @workgroup_size(1, 1, 1)
fn resetBrickCounters() {
    atomicStore(&counters.visibleCount, 0u);
    atomicStore(&counters.occludedCount, 0u);
    atomicStore(&counters.overflow, 0u);
    atomicStore(&counters.visibleAreaPixels, 0u);
    atomicStore(&counters.testedCount, 0u);
    atomicStore(&counters.bailNearPlane, 0u);
    atomicStore(&counters.bailOffscreen, 0u);
    atomicStore(&counters.bailSpan, 0u);
    atomicStore(&counters.bailDegenerate, 0u);
    atomicStore(&counters.bailCoverage, 0u);
    atomicStore(&counters.occludedByDepth, 0u);
    atomicStore(&counters.bailHysteresis, 0u);
    atomicStore(&counters.lodSelectedCount, 0u);
    atomicStore(&counters.lodSelectedHashXor, 0u);
    atomicStore(&counters.lodSelectedHashSum, 0u);
    atomicStore(&counters.lodTransitionCount, 0u);
    // Phase one must preserve what phase zero's draw accumulated. The shipping
    // raster shader never writes this bank, so its phase-zero reset skips all
    // of these atomics as well.
    if (uniforms.phase == 0u &&
        (uniforms.cullingFlags & BRICK_CULL_RASTER_DIAGNOSTICS) != 0u) {
        atomicStore(&microCounters.eligibleParentHits, 0u);
        atomicStore(&microCounters.descriptorLoads, 0u);
        atomicStore(&microCounters.acceptedMicroHits, 0u);
        atomicStore(&microCounters.carvedParents, 0u);
        atomicStore(&microCounters.parentContinuations, 0u);
        atomicStore(&microCounters.finalDiscards, 0u);
        atomicStore(&microCounters.heightfieldSteps, 0u);
        atomicStore(&microCounters.volumeSteps, 0u);
        atomicStore(&microCounters.exactTraceInvocations, 0u);
        atomicStore(&microCounters.fallbackCandidates, 0u);
        atomicStore(&microCounters.fallbackRejected, 0u);
        atomicStore(&microCounters.staircaseInteriorCandidates, 0u);
        atomicStore(&microCounters.staircaseFlatCandidates, 0u);
        atomicStore(&microCounters.staircaseSlopeCandidates, 0u);
        atomicStore(&microCounters.staircaseWallCandidates, 0u);
        atomicStore(&microCounters.staircaseOtherCandidates, 0u);
        atomicStore(&microCounters.oneFaceInvocations, 0u);
        atomicStore(&microCounters.oneFaceHits, 0u);
        atomicStore(&microCounters.oneFaceMisses, 0u);
        atomicStore(&microCounters.volumeInvocations, 0u);
        atomicStore(&microCounters.volumeHits, 0u);
        atomicStore(&microCounters.volumeMisses, 0u);
        atomicStore(&microCounters.solidInteriorShortcuts, 0u);
        atomicStore(&microCounters.wallOneFaceEligible, 0u);
        atomicStore(&microCounters.scale4GroupsTested, 0u);
        atomicStore(&microCounters.scale4GroupsSkipped, 0u);
        atomicStore(&microCounters.scale2CellsVisited, 0u);
        atomicStore(&microCounters.fineOccupancyProbes, 0u);
        atomicStore(&microCounters.firstHitExits, 0u);
        atomicStore(&microCounters.traversalMisses, 0u);
        atomicStore(&microCounters.traceContextInterior, 0u);
        atomicStore(&microCounters.traceContextFlat, 0u);
        atomicStore(&microCounters.traceContextSlope, 0u);
        atomicStore(&microCounters.traceContextWall, 0u);
        atomicStore(&microCounters.traceContextGeneralVolume, 0u);
        atomicStore(&microCounters.traceContextOtherHeightfield, 0u);
    }
    // Phase zero starts the frame's request set. Phase one fills it only after
    // current-frustum/current-HZB confirmation, so hysteresis-visible pages do
    // not retain generation priority after they leave the screen.
    if (uniforms.phase == 0u) {
        let requestWords =
            (uniforms.terrainPageRequestCapacity + 31u) / 32u;
        for (var word = 0u;
             word < requestWords &&
                 word < arrayLength(&terrainPageRequestBits);
             word += 1u) {
            atomicStore(&terrainPageRequestBits[word], 0u);
        }
        for (var page = 0u;
             page < uniforms.terrainPageRequestCapacity &&
                 page < arrayLength(&terrainPageRequestPriorities);
             page += 1u) {
            atomicStore(&terrainPageRequestPriorities[page], 0u);
            if (page < arrayLength(&terrainPageRequestGenerations)) {
                atomicStore(&terrainPageRequestGenerations[page], 0u);
            }
            if (page < arrayLength(&terrainPageRequestDesiredLODs)) {
                atomicStore(&terrainPageRequestDesiredLODs[page], 0xffffffffu);
            }
        }
    }
    drawArgs.vertexCount = BRICK_VERTICES_PER_INSTANCE;
    drawArgs.instanceCount = 0u;
    drawArgs.firstVertex = 0u;
    drawArgs.firstInstance = 0u;
}
