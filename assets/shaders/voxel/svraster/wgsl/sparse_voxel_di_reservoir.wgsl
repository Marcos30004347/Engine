#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_di_target.wgsl"
#include "sparse_voxel_light_reachability.wgsl"

struct SparseVoxelDIReservoirSettings {
    pixelCount : u32,
    candidatesPerPixel : u32,
    contentGeneration : u32,
    historyValid : u32,
    width : u32,
    height : u32,
    reserved0 : u32,
    reserved1 : u32,
    normalThreshold : f32,
    depthThreshold : f32,
    maximumHistorySamples : u32,
    frameSeed : u32,
    maximumWeight : f32,
    reachabilityEntryCount : u32,
    reachabilityExplorationFloor : f32,
    lightCapacity : u32,
    raysPerPixel : u32,
    reserved2 : u32,
    reserved3 : u32,
    reserved4 : u32,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelDIReservoirSettings;
@group(0) @binding(1) var<storage, read> candidates : array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read> previousSelectedRays : array<SparseVoxelSceneRay>;
@group(0) @binding(3) var<storage, read> previousReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(4) var<storage, read_write> selectedRays : array<SparseVoxelSceneRay>;
@group(0) @binding(5) var<storage, read_write> reservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(6) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(7) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(8) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(9) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(10) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(11) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(12) var<storage, read> lightReachability : array<u32>;
@group(0) @binding(13) var reservoirReceiverTex : texture_storage_2d<rgba32uint, write>;
@group(0) @binding(14) var reservoirCoreTex : texture_storage_2d<rgba32uint, write>;
@group(0) @binding(15) var<storage, read> activeRayIndices : array<u32>;
// xyz are DispatchIndirectArgs; w is the exact compact lane count.
@group(0) @binding(16) var<storage, read> activeRayState : array<u32>;

fn reservoir_hash(value : u32) -> f32 {
    var bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    bits = bits * 277803737u;
    return f32((bits >> 8u) & 0x00ffffffu) / 16777216.0;
}

fn receiver_normal(pixel : vec2<u32>) -> vec3<f32> {
    return sparse_voxel_safe_normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz,
        vec3<f32>(0.0, 1.0, 0.0));
}

fn receiver_identity(pixel : vec2<u32>) -> vec4<u32> {
    return vec4<u32>(
        textureLoad(instanceIdTexture, vec2<i32>(pixel), 0).x,
        textureLoad(voxelIdTexture, vec2<i32>(pixel), 0).x,
        1u,
        0u);
}

fn invalid_reservoir(
    generation : u32, normalDepth : vec4<f32>,
    identity : vec4<u32>) -> SparseVoxelDIReservoir {
    return SparseVoxelDIReservoir(
        vec4<u32>(0xffffffffu, 0xffffffffu, 0u, generation),
        vec4<f32>(0.0), vec4<u32>(0u), normalDepth, identity);
}

fn temporal_light_is_infinite(light : WorldGILight) -> bool {
    return sparse_voxel_di_light_is_infinite(light);
}

fn reconnect_temporal_ray(
    previousRay : SparseVoxelSceneRay,
    currentCandidate : SparseVoxelSceneRay,
    light : WorldGILight) -> SparseVoxelSceneRay {
    var result = previousRay;
    result.seedFlags.y = sparse_voxel_copy_receiver_face(
        result.seedFlags.y, currentCandidate.seedFlags.y);
    let receiverPosition = currentCandidate.originMinimumDistance.xyz;
    if (temporal_light_is_infinite(light)) {
        result.originMinimumDistance = vec4<f32>(receiverPosition, 0.0);
        return result;
    }
    let sampledPoint = previousRay.originMinimumDistance.xyz +
        previousRay.directionMaximumDistance.xyz * previousRay.directionMaximumDistance.w;
    let toLight = sampledPoint - receiverPosition;
    let distanceToLight = length(toLight);
    if (distanceToLight <= 1.0e-5) {
        result.seedFlags.y = 0u;
        result.originMinimumDistance = vec4<f32>(receiverPosition, 0.0);
        result.directionMaximumDistance = vec4<f32>(0.0);
        return result;
    }
    let direction = toLight / distanceToLight;
    result.originMinimumDistance = vec4<f32>(receiverPosition, 0.0);
    result.directionMaximumDistance = vec4<f32>(direction, distanceToLight);
    return result;
}

fn temporal_target_proxy(
    ray : SparseVoxelSceneRay, light : WorldGILight,
    normal : vec3<f32>) -> f32 {
    return sparse_voxel_di_target(light, ray, normal);
}

fn reprojected_history_index(sourcePixel : vec2<u32>) -> u32 {
    if (settings.historyValid == 0u) { return 0xffffffffu; }
    let dimensions = vec2<u32>(max(settings.width, 1u), max(settings.height, 1u));
    let sourceDimensions = vec2<u32>(
        max(settings.reserved0, 1u), max(settings.reserved1, 1u));
    let uv = (vec2<f32>(sourcePixel) + vec2<f32>(0.5)) /
        vec2<f32>(sourceDimensions);
    let motion = textureLoad(
        motionVectorTexture, vec2<i32>(sourcePixel), 0).xy;
    let historyUv = uv - motion;
    if (any(historyUv < vec2<f32>(0.0)) || any(historyUv >= vec2<f32>(1.0))) {
        return 0xffffffffu;
    }
    let historyPixel = min(vec2<u32>(historyUv * vec2<f32>(dimensions)), dimensions - vec2<u32>(1u));
    return historyPixel.y * dimensions.x + historyPixel.x;
}

fn receiver_history_valid(
    currentNormalDepth : vec4<f32>,
    currentMotion : vec2<f32>,
    currentRay : SparseVoxelSceneRay,
    previous : SparseVoxelDIReservoir,
    previousRay : SparseVoxelSceneRay) -> bool {
    let previousNormal = sparse_voxel_safe_normalize(
        previous.receiverNormalDepth.xyz, vec3<f32>(0.0, 1.0, 0.0));
    if (dot(currentNormalDepth.xyz, previousNormal) < settings.normalThreshold) { return false; }
    let dimensions = vec2<f32>(max(
        vec2<u32>(settings.width, settings.height), vec2<u32>(1u)));
    let motionPixels = length(currentMotion * dimensions);
    if (motionPixels <= 0.5) {
        let depthScale = max(
            max(abs(currentNormalDepth.w),
                abs(previous.receiverNormalDepth.w)), 1.0);
        if (abs(currentNormalDepth.w - previous.receiverNormalDepth.w) /
                depthScale > settings.depthThreshold) {
            return false;
        }
    }
    // The old gate compared current and previous *view* depth. Camera motion
    // changes view depth for a stationary world point, so that test either
    // discarded valid proposal history or accepted a different point with a
    // coincidentally similar depth. Both rays already store their world-space
    // receiver, making the correct comparison cheaper and unambiguous.
    let receiverDelta = previousRay.originMinimumDistance.xyz -
        currentRay.originMinimumDistance.xyz;
    let pixelTolerance = max(
        0.02,
        abs(currentNormalDepth.w) /
            f32(max(settings.reserved1, 1u)) * 4.0);
    if (length(receiverDelta) > pixelTolerance) { return false; }
    let planeNormal = sparse_voxel_safe_normalize(
        currentNormalDepth.xyz + previousNormal, currentNormalDepth.xyz);
    if (abs(dot(receiverDelta, planeNormal)) > pixelTolerance * 0.5) {
        return false;
    }
    return true;
}

// Candidate RIS plus motion-reprojected temporal reservoir reuse. Receiver
// validity is deliberately only normal plus depth; voxel/page identity churn
// must not reset otherwise stable lighting at rasterized voxel boundaries.
fn resample_sparse_voxel_di_pixel(pixelIndex : u32) {
    let raysPerPixel = max(settings.raysPerPixel, 1u);
    if (pixelIndex >= settings.pixelCount) { return; }
    let outputIndex = pixelIndex * raysPerPixel;
    if (outputIndex >= settings.pixelCount * raysPerPixel ||
        outputIndex >= arrayLength(&selectedRays)) { return; }
    let reservoirIndex = pixelIndex;
    if (reservoirIndex >= arrayLength(&reservoirs)) { return; }

    let dimensions = vec2<u32>(max(settings.width, 1u), max(settings.height, 1u));
    let pixel = vec2<u32>(pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    // Initial sampling has already streamed all fresh proposals into one ray
    // per reservoir lane, so candidate storage is O(traced rays), not
    // O(initial proposals).
    let referenceIndex = outputIndex;
    if (referenceIndex >= arrayLength(&candidates)) {
        selectedRays[outputIndex] = SparseVoxelSceneRay(
            vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u), vec4<u32>(0u),
            vec4<f32>(0.0));
        reservoirs[reservoirIndex] = invalid_reservoir(
            settings.contentGeneration, vec4<f32>(0.0), vec4<u32>(0u));
        textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(0u));
        textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(0u));
        return;
    }
    let sourceDimensions = vec2<u32>(
        max(settings.reserved0, 1u), max(settings.reserved1, 1u));
    // Ray creation applies the sparse schedule per pixel, so all candidates
    // for this slot share the active bit. Reject an inactive slot before
    // reading its G-buffer attributes or motion-reprojecting history. At the
    // default quarter rate this is the common path for 75% of invocations.
    if ((candidates[referenceIndex].seedFlags.y &
            SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) {
        // Consumers gate every other ray field on ACTIVE, and temporal reuse
        // gates the reservoir on state.z. Invalidate only those authoritative
        // words instead of writing the full 144-byte ray+reservoir pair for
        // the 75% of slots skipped by quarter-rate scheduling.
        selectedRays[outputIndex].seedFlags.y = 0u;
        reservoirs[reservoirIndex].state.z = 0u;
        textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(0u));
        textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(0u));
        return;
    }
    // One temporal reservoir per pixel is sufficient. Higher lanes are fresh
    // visibility samples and retain their ordinary proposal weight.
    var sourcePixel = min(
        vec2<u32>(
            (vec2<f32>(pixel) + vec2<f32>(0.5)) *
            vec2<f32>(sourceDimensions) / vec2<f32>(dimensions)),
        sourceDimensions - vec2<u32>(1u));
    sourcePixel = min(
        candidates[referenceIndex].pixelRayLight.xy,
        sourceDimensions - vec2<u32>(1u));
    let sourceIndex =
        sourcePixel.y * sourceDimensions.x + sourcePixel.x;
    // Both outputs are frame-local, so a slot this dispatch leaves unwritten
    // keeps whatever the same slot held maxFramesInFlight frames ago. Publish
    // an explicitly invalid reservoir rather than returning early.
    if (sourceIndex >= arrayLength(&linearDepth)) {
        selectedRays[outputIndex] = SparseVoxelSceneRay(
            vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u), vec4<u32>(0u),
            vec4<f32>(0.0));
        reservoirs[reservoirIndex] = invalid_reservoir(
            settings.contentGeneration, vec4<f32>(0.0), vec4<u32>(0u));
        textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(0u));
        textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(0u));
        return;
    }
    let currentNormal = receiver_normal(sourcePixel);
    let currentNormalDepth =
        vec4<f32>(currentNormal, linearDepth[sourceIndex]);
    let currentIdentity = receiver_identity(sourcePixel);
    var selected = candidates[referenceIndex];
    let packedInitial = selected.pixelRayLight.z;
    let packedInitialM = packedInitial >> 16u;
    var selectedSlot = select(
        packedInitial, packedInitial & 0xffffu, packedInitialM != 0u);
    var selectedTarget = max(selected.lightSamplePdfWeight.z, 0.0);
    var selectedProposal =
        max(selected.lightSamplePdfWeight.x, 0.0) *
        max(selected.lightSamplePdfWeight.y, 0.0);
    var m = select(1u, packedInitialM, packedInitialM != 0u);
    var weightSum = select(
        selectedTarget / max(selectedProposal, 1.0e-20),
        selected.lightSamplePdfWeight.w,
        packedInitialM != 0u &&
            selected.lightSamplePdfWeight.w > 0.0);
    if ((selected.seedFlags.y &
            SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u ||
        selectedTarget <= 0.0 || selectedProposal <= 0.0 ||
        weightSum <= 0.0) {
        selectedSlot = 0xffffffffu;
        selectedTarget = 0.0;
        selectedProposal = 0.0;
        weightSum = 0.0;
        m = 0u;
    }
    var temporalAge = 0u;

    let historyPixelIndex = reprojected_history_index(sourcePixel);
    let historyRayIndex = select(
        0xffffffffu,
        historyPixelIndex * raysPerPixel,
        historyPixelIndex != 0xffffffffu);
    // Temporal reuse reconnects the stored ray to the *current* candidate's
    // receiver, so it is only meaningful when this slot produced a candidate
    // this frame. Under checkerboard tracing an untraced slot has none, and
    // reconnecting to its zeroed origin would resurrect a reservoir anchored
    // to a garbage receiver position.
    if (historyPixelIndex < arrayLength(&previousReservoirs) &&
        historyRayIndex < arrayLength(&previousSelectedRays) &&
        (candidates[referenceIndex].seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u) {
        let previous = previousReservoirs[historyPixelIndex];
        let previousRay = previousSelectedRays[historyRayIndex];
        if (previous.identity.w == settings.contentGeneration && previous.state.z != 0u &&
            receiver_history_valid(
                currentNormalDepth,
                textureLoad(
                    motionVectorTexture, vec2<i32>(sourcePixel), 0).xy,
                candidates[referenceIndex], previous, previousRay) &&
            (previousRay.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
            previous.identity.x < arrayLength(&lights) &&
            // A sky sample is one stochastic direction of a hemispheric
            // integral. Keeping that direction in a ReSTIR reservoir while
            // the light field also accumulates its binary visibility applies
            // temporal reuse twice and turns one blocked direction into a
            // coherent, slowly moving shadow patch. The light field owns sky
            // convergence; its ReSTIR proposal must be fresh every frame.
            lights[previous.identity.x].flags.y != WORLD_GI_LIGHT_TYPE_SKY &&
            previous.weights.w > 0.0 && previous.weights.y > 0.0 && previous.weights.z > 0.0) {
            let previousM = min(
                max(previous.identity.z, 1u),
                max(settings.maximumHistorySamples, 1u));
            let reconnected = reconnect_temporal_ray(
                previousRay, candidates[referenceIndex],
                lights[previous.identity.x]);
            let currentTarget = temporal_target_proxy(
                reconnected, lights[previous.identity.x], currentNormal);
            let previousWeightSum = currentTarget * previous.weights.w * f32(previousM);
            if ((reconnected.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
                previousWeightSum > 0.0) {
                m = m + previousM;
                weightSum = weightSum + previousWeightSum;
                let replace = reservoir_hash(previous.state.x ^ settings.contentGeneration ^
                    (outputIndex * 2246822519u) ^ (settings.frameSeed * 0x85ebca6bu)) <
                    previousWeightSum / max(weightSum, 1.0e-20);
                if (replace) {
                    selected = reconnected;
                    selected.pixelRayLight.x = candidates[referenceIndex].pixelRayLight.x;
                    selected.pixelRayLight.y = candidates[referenceIndex].pixelRayLight.y;
                    selected.pixelRayLight.z = previous.identity.y;
                    selected.seedFlags.z = settings.contentGeneration;
                    selected.seedFlags.w =
                        candidates[referenceIndex].seedFlags.w;
                    selectedTarget = currentTarget;
                    selectedProposal = previous.weights.z;
                    selectedSlot = previous.identity.y;
                    temporalAge = min(previous.state.y + 1u, 255u);
                }
            }
        }
    }
    if (m == 0u || selectedSlot == 0xffffffffu || selectedTarget <= 0.0) {
        selectedRays[outputIndex] = selected;
        reservoirs[reservoirIndex] = invalid_reservoir(
            settings.contentGeneration, currentNormalDepth, currentIdentity);
        textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(0u));
        textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(0u));
        return;
    }
    selected.pixelRayLight.z = selectedSlot;
    // Clamp W here, in ray space, rather than downstream. The shading pass
    // upsamples one reduced-resolution reservoir across a whole divisor-sized
    // pixel block, so an outlier W that escapes this pass arrives at the
    // denoiser as a solid bright block - larger than SVGF's clipping
    // neighbourhood, which then accepts it as signal and accumulates it. A
    // firefly is still a single texel at this point. The clamp is biased by
    // construction; the bound is set high enough that only true outliers reach
    // it.
    let risWeight = min(
        weightSum / (f32(m) * selectedTarget),
        max(settings.maximumWeight, 1.0e-4));
    reservoirs[reservoirIndex] = SparseVoxelDIReservoir(
        vec4<u32>(selected.pixelRayLight.w, selectedSlot, m, settings.contentGeneration),
        vec4<f32>(weightSum, selectedTarget, selectedProposal, risWeight),
        vec4<u32>(selected.seedFlags.x, temporalAge, 1u, 0u),
        currentNormalDepth,
        currentIdentity);
    selectedRays[outputIndex] = selected;
    textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(
        currentIdentity.x,
        currentIdentity.y,
        sparse_voxel_pack_octahedral_normal(currentNormalDepth.xyz),
        bitcast<u32>(currentNormalDepth.w)));
    textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(
        selected.pixelRayLight.w,
        m,
        bitcast<u32>(risWeight),
        1u | (select(0u, 1u,
            (selected.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u) << 8u) |
            ((settings.contentGeneration & 0x007fffffu) << 9u)));
}

@compute @workgroup_size(64, 1, 1)
fn resetSparseVoxelDIOutputs(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    let pixelIndex = gid.x;
    if (pixelIndex >= settings.pixelCount) { return; }
    let raysPerPixel = max(settings.raysPerPixel, 1u);
    let outputIndex = pixelIndex * raysPerPixel;
    if (outputIndex < arrayLength(&selectedRays)) {
        selectedRays[outputIndex].seedFlags.y = 0u;
    }
    if (pixelIndex < arrayLength(&reservoirs)) {
        reservoirs[pixelIndex].state.z = 0u;
    }
    let dimensions = vec2<u32>(
        max(settings.width, 1u), max(settings.height, 1u));
    let pixel = vec2<u32>(pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    textureStore(reservoirReceiverTex, vec2<i32>(pixel), vec4<u32>(0u));
    textureStore(reservoirCoreTex, vec2<i32>(pixel), vec4<u32>(0u));
}

@compute @workgroup_size(64, 1, 1)
fn resampleSparseVoxelDICandidates(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    resample_sparse_voxel_di_pixel(gid.x);
}

@compute @workgroup_size(64, 1, 1)
fn resampleSparseVoxelDICompact(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (arrayLength(&activeRayState) < 4u ||
        gid.x >= activeRayState[3] ||
        gid.x >= arrayLength(&activeRayIndices)) {
        return;
    }
    let raysPerPixel = max(settings.raysPerPixel, 1u);
    let canonicalRayIndex = activeRayIndices[gid.x];
    // Only lane zero owns the temporal reservoir. Higher lanes retain their
    // fresh independent visibility samples.
    if ((canonicalRayIndex % raysPerPixel) != 0u) { return; }
    resample_sparse_voxel_di_pixel(canonicalRayIndex / raysPerPixel);
}
