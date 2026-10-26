#include "sparse_voxel_scene_ray_types.wgsl"

struct SparseVoxelTemporalSceneRayHit {
    hit : SparseVoxelSceneRayHit,
    receiverVisibility : vec4<f32>,
    momentsConfidence : vec4<f32>,
    state : vec4<u32>,
}
struct SparseVoxelSceneHitTemporalSettings {
    rayCount : u32, maximumSamples : u32, historyValid : u32, contentGeneration : u32,
    distanceThreshold : f32, receiverThreshold : f32, reserved1 : f32, reserved2 : f32,
    rayDimensions : vec4<u32>,
}
@group(0) @binding(0) var<uniform> settings : SparseVoxelSceneHitTemporalSettings;
@group(0) @binding(1) var<storage, read> currentHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(2) var<storage, read> currentRays : array<SparseVoxelSceneRay>;
@group(0) @binding(3) var<storage, read> previousHistory : array<SparseVoxelTemporalSceneRayHit>;
@group(0) @binding(4) var<storage, read_write> history : array<SparseVoxelTemporalSceneRayHit>;
@group(0) @binding(5) var<storage, read_write> stableHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(6) var<storage, read_write> stableVisibility : array<f32>;
@group(0) @binding(7) var motionVectorTexture : texture_2d<f32>;

fn temporal_visibility(hit : SparseVoxelSceneRayHit) -> f32 {
    if (hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) { return 0.0; }
    if (hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
        hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE) { return 1.0; }
    if (hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
        hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
        hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT) { return 0.0; }
    return 0.0;
}

fn temporal_receiver_position(ray : SparseVoxelSceneRay) -> vec3<f32> {
    return ray.originMinimumDistance.xyz -
        ray.directionMaximumDistance.xyz *
            max(ray.originMinimumDistance.w, 0.0);
}

fn reprojected_history_index(
    index : u32, ray : SparseVoxelSceneRay, allowEdgeClamp : bool) -> u32 {
    if (settings.historyValid == 0u) { return 0xffffffffu; }
    let dimensions = textureDimensions(motionVectorTexture);
    let rayDimensions = max(
        settings.rayDimensions.xy, vec2<u32>(1u));
    let rayPixelCount = rayDimensions.x * rayDimensions.y;
    let raysPerPixel = max(
        settings.rayCount / max(rayPixelCount, 1u), 1u);
    let pixelIndex = index / raysPerPixel;
    let rayLane = index % raysPerPixel;
    if (dimensions.x == 0u || dimensions.y == 0u ||
        pixelIndex >= rayPixelCount) {
        return 0xffffffffu;
    }
    // History is laid out canonically as pixel-major ray lanes. Inactive
    // checkerboard slots deliberately publish only ACTIVE=0 and may retain
    // stale payload words, including pixelRayLight, from an older frame.
    // Reading that stale coordinate made every inactive slot reproject some
    // unrelated pixel and discarded most settled shadow history on even a
    // tiny camera rotation. The buffer index is authoritative for both active
    // and carried lanes.
    let canonicalPixel = vec2<u32>(
        pixelIndex % rayDimensions.x,
        pixelIndex / rayDimensions.x);
    let pixel = min(
        canonicalPixel, dimensions - vec2<u32>(1u));
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(dimensions);
    let motion = textureLoad(
        motionVectorTexture, vec2<i32>(pixel), 0).xy;
    let historyUv = uv - motion;
    // Motion vectors near viewport edges can place a still-visible receiver just
    // outside [0,1) during small camera rotations. Keep a tiny guard band and
    // let receiver-position validation decide whether the clamped history really
    // belongs to this ray; only larger excursions are true disocclusions.
    let guardBand = vec2<f32>(2.0) / vec2<f32>(rayDimensions);
    if (!allowEdgeClamp &&
        (any(historyUv < vec2<f32>(0.0)) ||
         any(historyUv >= vec2<f32>(1.0)))) {
        return 0xffffffffu;
    }
    if (any(historyUv < -guardBand) ||
        any(historyUv >= vec2<f32>(1.0) + guardBand)) {
        return 0xffffffffu;
    }
    let clampedHistoryUv = clamp(
        historyUv, vec2<f32>(0.0),
        vec2<f32>(1.0) - vec2<f32>(0.5) / vec2<f32>(rayDimensions));
    let historyPixel = min(
        vec2<u32>(clampedHistoryUv * vec2<f32>(rayDimensions)),
        rayDimensions - vec2<u32>(1u));
    return (historyPixel.y * rayDimensions.x + historyPixel.x) *
        raysPerPixel + rayLane;
}

// Reuse verdict: zero means the history sample is compatible; any other
// value identifies the gate that rejected it (stored for diagnostics in the
// history record's spare lane).
const HIT_TEMPORAL_REUSE_OK : u32 = 0u;
const HIT_TEMPORAL_REJECT_HISTORY : u32 = 1u;
const HIT_TEMPORAL_REJECT_EMPTY : u32 = 2u;
const HIT_TEMPORAL_REJECT_GENERATION : u32 = 3u;
const HIT_TEMPORAL_REJECT_LIGHT : u32 = 4u;
const HIT_TEMPORAL_REJECT_STATUS : u32 = 5u;
const HIT_TEMPORAL_REJECT_RECEIVER : u32 = 6u;
const HIT_TEMPORAL_REJECT_DISTANCE : u32 = 7u;
const HIT_TEMPORAL_REJECT_REPROJECTION : u32 = 8u;

fn can_reuse(
    current : SparseVoxelSceneRayHit,
    ray : SparseVoxelSceneRay,
    previous : SparseVoxelTemporalSceneRayHit) -> u32 {
    // History is matched by receiver position, light id, and hit distance
    // only. The ray seed (identity.x) is derived from the ray buffer index,
    // so comparing it would reject every motion-reprojected history sample
    // (the reprojected index never equals the current index) and drop all
    // accumulation as soon as the camera moves.
    if (settings.historyValid == 0u) { return HIT_TEMPORAL_REJECT_HISTORY; }
    if (previous.state.x == 0u) { return HIT_TEMPORAL_REJECT_EMPTY; }
    if (previous.state.y != settings.contentGeneration) { return HIT_TEMPORAL_REJECT_GENERATION; }
    if (previous.state.z != current.identity.y) { return HIT_TEMPORAL_REJECT_LIGHT; }
    if (current.status == SPARSE_VOXEL_SCENE_RAY_STATUS_INVALID ||
        current.status == SPARSE_VOXEL_SCENE_RAY_STATUS_PENDING) { return HIT_TEMPORAL_REJECT_STATUS; }
    // Hit and miss are the two ordinary outcomes of successive sky/sun-disk
    // directions.  They must share history or a stochastic visibility signal
    // can never become a fractional AO/penumbra estimate.  Invalid and pending
    // samples were rejected above; geometry generations still invalidate the
    // record explicitly.
    // Fresh candidate rays offset their origin along the receiver normal
    // plus a quarter bias along the sampled direction, so the recovered
    // receiver position wanders by up to ~1.75 * bias as the sampled
    // direction changes from frame to frame. Widen the reuse gate by the
    // stored bias so this jitter is not mistaken for a moved receiver.
    let receiverTolerance = settings.receiverThreshold +
        2.0 * max(ray.originMinimumDistance.w, 0.0);
    if (distance(
            temporal_receiver_position(ray),
            previous.receiverVisibility.xyz) >
        receiverTolerance) { return HIT_TEMPORAL_REJECT_RECEIVER; }
    // This history estimates binary visibility, not the closest-hit point.
    // Screen and Tree64 can agree that a ray is blocked while reporting
    // different points on the same occluder. Rejecting that sample by hit
    // distance discarded valid sharp-shadow history as the screen-space
    // backend changed with camera angle. Receiver reprojection and content
    // generation already own geometric validity; hit distance is diagnostic.
    return HIT_TEMPORAL_REUSE_OK;
}

@compute @workgroup_size(64, 1, 1)
fn accumulateSparseVoxelSceneHits(@builtin(global_invocation_id) gid : vec3<u32>) {
    let index = gid.x;
    if (index >= settings.rayCount || index >= arrayLength(&currentHits) ||
        index >= arrayLength(&currentRays) ||
        index >= arrayLength(&previousHistory) ||
        index >= arrayLength(&history) || index >= arrayLength(&stableHits) ||
        index >= arrayLength(&stableVisibility)) { return; }
    let current = currentHits[index];
    let ray = currentRays[index];
    // Checkerboard tracing leaves half the slots untraced each frame. Such a
    // slot must keep the estimate it already has: folding in its untraced
    // (invalid) hit would drive the running mean to zero every other frame and
    // destroy exactly the accumulation the pattern relies on. Carry the
    // reprojected record forward untouched instead.
    if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u &&
        settings.historyValid != 0u) {
        let carryIndex = reprojected_history_index(index, ray, false);
        if (carryIndex < arrayLength(&previousHistory)) {
            let carried = previousHistory[carryIndex];
            if (carried.state.x != 0u &&
                carried.state.y == settings.contentGeneration) {
                history[index] = carried;
                stableHits[index] = carried.hit;
                stableVisibility[index] = carried.receiverVisibility.w;
                return;
            }
        }
    }
    let currentVisibility = temporal_visibility(current);
    var stable = current;
    var count = 1u;
    var visibilityMean = currentVisibility;
    var visibilitySecondMoment = currentVisibility * currentVisibility;
    var confidence = 1.0;
    let historyIndex = reprojected_history_index(index, ray, true);
    var reuseVerdict = HIT_TEMPORAL_REJECT_REPROJECTION;
    if (historyIndex < arrayLength(&previousHistory)) {
        reuseVerdict = can_reuse(current, ray, previousHistory[historyIndex]);
    }
    if (reuseVerdict == HIT_TEMPORAL_REUSE_OK) {
        let previous = previousHistory[historyIndex];
        count = min(previous.state.x + 1u, max(settings.maximumSamples, 1u));
        confidence = f32(count) / f32(max(settings.maximumSamples, 1u));
        let blendWeight = 1.0 / f32(count);
        visibilityMean = mix(previous.receiverVisibility.w, currentVisibility, blendWeight);
        visibilitySecondMoment = mix(previous.momentsConfidence.x, currentVisibility * currentVisibility, blendWeight);
        if (current.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT &&
            previous.hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
            let historyWeight = 1.0 - blendWeight;
            stable.hitPositionDistance = mix(current.hitPositionDistance, previous.hit.hitPositionDistance, historyWeight);
        }
    }
    let variance = max(visibilitySecondMoment - visibilityMean * visibilityMean, 0.0);
    stableHits[index] = stable;
    stableVisibility[index] = visibilityMean;
    history[index] = SparseVoxelTemporalSceneRayHit(
        stable,
        vec4<f32>(
            temporal_receiver_position(ray), visibilityMean),
        vec4<f32>(visibilitySecondMoment, confidence, variance, 0.0),
        vec4<u32>(count, settings.contentGeneration, current.identity.y, reuseVerdict));
}
