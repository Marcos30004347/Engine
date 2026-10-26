#include "sparse_voxel_scene_ray_types.wgsl"
#include "sparse_voxel_tree64_common.wgsl"

// direction.w carries how far this ray was advanced from the original surface
// origin. Dynamic visibility traversal also uses it to recover the canonical
// receiver identity; the merge pass turns local hit distance back into an
// absolute one.
struct SparseVoxelTree64Ray { originMaximumDistance : vec4<f32>, direction : vec4<f32>, }
struct SparseVoxelTree64Dispatch {
    rayCount : atomic<u32>, maximumQueries : u32, chunkCapacity : u32, nodeCapacity : u32,
    nodeSlotCapacity : u32, reserved0 : u32, reserved1 : u32, reserved2 : u32,
}
struct DispatchIndirectArgs { x : atomic<u32>, y : atomic<u32>, z : atomic<u32>, }
struct CompactSettings {
    denseRayCount : u32,
    capacity : u32,
    screenTraceEnabled : u32,
    // Skip the near-field span the screen tracer resolved. See the start
    // distance below for why this is opt-in.
    resumeAfterScreenTrace : u32,
    traceScreenHitsForConfirmation : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

@group(0) @binding(0) var<uniform> settings : CompactSettings;
@group(0) @binding(1) var<storage, read> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read> screenHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(3) var<storage, read_write> compactRays : array<SparseVoxelTree64Ray>;
@group(0) @binding(4) var<storage, read_write> compactToCanonical : array<u32>;
@group(0) @binding(5) var<storage, read_write> canonicalToCompact : array<u32>;
@group(0) @binding(6) var<storage, read_write> treeDispatch : SparseVoxelTree64Dispatch;
@group(0) @binding(7) var<storage, read_write> indirectArgs : DispatchIndirectArgs;
@group(0) @binding(8) var<storage, read> treeState : SparseVoxelTree64State;
@group(0) @binding(9) var<storage, read> activeRayIndices : array<u32>;
@group(0) @binding(10) var<storage, read> activeRayState : array<u32>;

@compute @workgroup_size(1, 1, 1)
fn resetSparseVoxelSceneRayCompaction(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    var activeCount = 0u;
    var activeGroups = 0u;
    if (arrayLength(&activeRayState) >= 4u) {
        activeCount = min(activeRayState[3], settings.capacity);
        activeGroups = (activeCount + 63u) / 64u;
    }
    // The active-ray list produced by ray creation is already a dense
    // compaction. Trace it directly. Filtering screen-space hits here used
    // three workgroup barriers and atomics per 64 rays; on Metal that cost
    // several times more than tracing the small number of redundant rays.
    atomicStore(&treeDispatch.rayCount, activeCount);
    atomicStore(&indirectArgs.x, activeGroups);
    atomicStore(&indirectArgs.y, 1u);
    atomicStore(&indirectArgs.z, 1u);
}

// Workgroup-local coherence sorting: direction-octant + coarse-origin Morton
// code gives a 12-bit key. A bitonic sort within each 64-thread workgroup
// reorders rays so nearby origins travelling in the same octant land in
// adjacent compact slots, improving cache hit rates during Tree64 traversal.
const COMPACT_WORKGROUP_SIZE = 64u;
var<workgroup> sortKeys : array<u32, COMPACT_WORKGROUP_SIZE>;
var<workgroup> sortLocalIndices : array<u32, COMPACT_WORKGROUP_SIZE>;

// Build a 12-bit coherence key: 3 direction-sign bits (octant) in the high
// part, 9 Morton-coded origin bits (3 per axis) in the low part. Rays in the
// same octant with nearby origins sort together.
fn compactCoherenceKey(origin : vec3<f32>, direction : vec3<f32>) -> u32 {
    let signs = select(0u, 1u, direction.x >= 0.0) |
        (select(0u, 1u, direction.y >= 0.0) << 1u) |
        (select(0u, 1u, direction.z >= 0.0) << 2u);
    // Quantize origin into a coarse 8^3 grid relative to the tree volume.
    let volumeMin = vec3<f32>(treeState.originCell) * treeState.cellSize;
    let volumeSize = vec3<f32>(treeState.logicalCellResolution) * treeState.cellSize;
    let normalized = clamp(
        (origin - volumeMin) / max(volumeSize, vec3<f32>(1.0e-9)),
        vec3<f32>(0.0), vec3<f32>(1.0));
    let q = vec3<u32>(normalized * 7.999);
    // 3-bit-per-axis Morton interleave.
    let morton =
        (q.x & 1u) | ((q.y & 1u) << 1u) | ((q.z & 1u) << 2u) |
        ((q.x & 2u) << 2u) | ((q.y & 2u) << 3u) | ((q.z & 2u) << 4u) |
        ((q.x & 4u) << 4u) | ((q.y & 4u) << 5u) | ((q.z & 4u) << 6u);
    return (signs << 9u) | morton;
}

fn compactReceiverFace(flags : u32) -> u32 {
    return (flags & SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_MASK) >>
        SPARSE_VOXEL_SCENE_RAY_RECEIVER_FACE_SHIFT;
}

// A compact Tree64 ray has no spare word for receiver identity. Encode the
// six face values in the otherwise-negative half of direction.w; ordinary
// callers keep their legacy non-negative start distance. The 128 m stride is
// larger than the screen-resume span and retains sub-millimetre start-distance
// precision even for face six.
fn compactRayMetadata(startDistance : f32, face : u32) -> f32 {
    if (face == 0u || face > 6u) { return startDistance; }
    return -(1.0 + f32(face) * 128.0 + startDistance);
}

@compute @workgroup_size(64, 1, 1)
fn compactSparseVoxelSceneRays(
    @builtin(global_invocation_id) gid : vec3<u32>,
    @builtin(local_invocation_id) lid : vec3<u32>,
    @builtin(workgroup_id) wid : vec3<u32>) {
    let localLane = lid.x;
    let workIndex = gid.x;
    let workValid =
        arrayLength(&activeRayState) >= 4u &&
        workIndex < activeRayState[3] &&
        workIndex < arrayLength(&activeRayIndices);
    var index = 0u;
    if (workValid) {
        index = activeRayIndices[workIndex];
    }
    let indexValid = workValid &&
        index < settings.denseRayCount &&
        index < arrayLength(&rays) &&
        index < arrayLength(&canonicalToCompact) &&
        (settings.screenTraceEnabled == 0u ||
         index < arrayLength(&screenHits));
    var ray = SparseVoxelSceneRay(
        vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u),
        vec4<u32>(0u), vec4<f32>(0.0));
    if (indexValid) {
        canonicalToCompact[index] = 0xffffffffu;
        ray = rays[index];
    }
    let unresolved = indexValid &&
        (ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u;

    // Compute the coherence key. Inactive lanes use 0xFFFFFFFF so they sort
    // to the end of the workgroup and never write output.
    var key = 0xffffffffu;
    var origin = vec3<f32>(0.0);
    var direction = vec3<f32>(0.0, 0.0, 1.0);
    var maximumDistance = 0.0;
    if (unresolved) {
        origin = ray.originMinimumDistance.xyz;
        // Ray creation and both reservoir reconnect paths store unit vectors.
        // Preserve that invariant instead of paying an inverse square root in
        // both the key-generation and output paths.
        direction = ray.directionMaximumDistance.xyz;
        maximumDistance = ray.directionMaximumDistance.w;
        key = compactCoherenceKey(origin, direction);
    }

    // Load into workgroup memory for the bitonic sort.
    sortKeys[localLane] = key;
    sortLocalIndices[localLane] = localLane;
    workgroupBarrier();

    // Bitonic sort over 64 elements: 6 stages (log2(64) = 6), with passes
    // within each stage. Total 15 comparison passes, each separated by a
    // barrier. Within a single wave this is effectively free.
    for (var stageSize = 2u; stageSize <= COMPACT_WORKGROUP_SIZE; stageSize = stageSize << 1u) {
        for (var halfStep = stageSize >> 1u; halfStep >= 1u; halfStep = halfStep >> 1u) {
            let partnerId = localLane ^ halfStep;
            if (partnerId > localLane) {
                let myKey = sortKeys[localLane];
                let partnerKey = sortKeys[partnerId];
                let myIdx = sortLocalIndices[localLane];
                let partnerIdx = sortLocalIndices[partnerId];
                // Ascending within each bitonic subsequence. The sort
                // direction flips with stageSize.
                let ascending = (localLane & stageSize) == 0u;
                let swap = select(
                    myKey < partnerKey,
                    myKey > partnerKey,
                    ascending);
                if (swap) {
                    sortKeys[localLane] = partnerKey;
                    sortKeys[partnerId] = myKey;
                    sortLocalIndices[localLane] = partnerIdx;
                    sortLocalIndices[partnerId] = myIdx;
                }
            }
            workgroupBarrier();
        }
    }

    // Read the sorted position for this lane.
    let sortedOriginalLane = sortLocalIndices[localLane];
    let sortedKey = sortKeys[localLane];

    // Inactive lanes sorted to the end — do not emit output.
    if (sortedKey == 0xffffffffu) { return; }

    // The compact index is the workgroup base plus the sorted position within
    // the workgroup. The workgroup base stays workgroupId * 64.
    let workgroupBase = wid.x * COMPACT_WORKGROUP_SIZE;
    let compactIndex = workgroupBase + localLane;
    // Recover the original work item's canonical index.
    let sortedWorkIndex = workgroupBase + sortedOriginalLane;
    let sortedCanonicalIndex = activeRayIndices[sortedWorkIndex];

    if (compactIndex >= settings.capacity ||
        compactIndex >= arrayLength(&compactRays) ||
        compactIndex >= arrayLength(&compactToCanonical)) { return; }

    // Re-read the ray for the sorted lane. The ray was already loaded above
    // but only for the *original* lane; the sorted lane may differ.
    let sortedRay = rays[sortedCanonicalIndex];
    let sortedOrigin = sortedRay.originMinimumDistance.xyz;
    let sortedDirection = sortedRay.directionMaximumDistance.xyz;
    let sortedMaximumDistance = sortedRay.directionMaximumDistance.w;
    // Keep world visibility anchored to the exact raster receiver. Even one
    // world-f32 ULP is 3.125 cm at the captured 260 km coordinate, enough to
    // detach a 10 cm voxel's contact shadow and cross Tree64's old receiver
    // recovery inset. Face metadata lets Tree64 reject the receiver without
    // moving the actual traversal ray. originMinimumDistance.w remains
    // available for an explicit caller offset.
    var startDistance = max(sortedRay.originMinimumDistance.w, 0.0);

    // By default the world ray re-traces the whole segment rather than resuming
    // where the screen tracer stopped.
    //
    // Resuming looks safe - the skipped span was marched against the depth
    // buffer - but that buffer holds only geometry that is on screen and facing
    // the camera. Anything else is invisible to the march, so skipping the span
    // silently drops those occluders. Inside a sealed box almost all of the
    // enclosing geometry is off screen, which let sky and analytical lights leak
    // in. Re-tracing costs traversal; getting occlusion wrong costs correctness.
    //
    // The opt-in resume narrows that to the one case where the objection is
    // weakest: the screen tracer exhausted its near-field budget while every
    // sample stayed on screen (MISS_TMAX, not ESCAPED_SCENE). A ray that left
    // the screen is never resumed, and neither is one the tracer could not
    // start. It remains opt-in because "stayed on screen" still says nothing
    // about geometry facing away from the camera.
    if (settings.resumeAfterScreenTrace != 0u &&
        settings.screenTraceEnabled != 0u &&
        sortedCanonicalIndex < arrayLength(&screenHits)) {
        let screen = screenHits[sortedCanonicalIndex];
        if (screen.status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX &&
            screen.identity.z == SPARSE_VOXEL_SCENE_RAY_BACKEND_SCREEN) {
            startDistance = max(startDistance, screen.hitPositionDistance.w);
        }
    }
    startDistance = clamp(startDistance, 0.0, max(sortedMaximumDistance, 0.0));

    // When confirmation is disabled, a screen-space hit is the final answer
    // for this ray and the merge never reads the traversal result. Give those
    // rays a zero-length span so the traversal rejects them immediately.
    //
    // This is the same saving the reset pass rejected as "filtering screen-space
    // hits", but without its cost: that attempt compacted the ray list, which
    // needed workgroup barriers and atomics per 64 rays. Shortening a ray in
    // place keeps the dispatch, the ordering and the index mapping exactly as
    // they are, and costs one already-bound buffer read.
    var worldSpan = max(sortedMaximumDistance - startDistance, 0.0);
    if (settings.traceScreenHitsForConfirmation == 0u &&
        settings.screenTraceEnabled != 0u &&
        sortedCanonicalIndex < arrayLength(&screenHits) &&
        screenHits[sortedCanonicalIndex].status ==
            SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        worldSpan = 0.0;
    }

    compactRays[compactIndex] = SparseVoxelTree64Ray(
        vec4<f32>(
            sortedOrigin + sortedDirection * startDistance,
            worldSpan),
        vec4<f32>(sortedDirection, compactRayMetadata(
            startDistance, compactReceiverFace(sortedRay.seedFlags.y))));
    compactToCanonical[compactIndex] = sortedCanonicalIndex;
    canonicalToCompact[sortedCanonicalIndex] = compactIndex;
}
