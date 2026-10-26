#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_di_target.wgsl"

struct SparseVoxelDISpatialSettings {
    dimensionsGeneration : vec4<u32>,
    receiverRadius : f32,
    normalThreshold : f32,
    depthThreshold : f32,
    maximumWeight : f32,
    // xy: source (full-resolution) dimensions, z: per-frame seed,
    // w: independently traced rays/reservoir lanes per pixel.
    sourceDimensions : vec4<u32>,
    // x: light capacity, y: reachability entries,
    // z: exploration floor as f32 bits.
    reachability : vec4<u32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelDISpatialSettings;
@group(0) @binding(1) var<storage, read> candidates : array<SparseVoxelSceneRay>;
@group(0) @binding(2) var<storage, read> inputRays : array<SparseVoxelSceneRay>;
@group(0) @binding(3) var<storage, read> inputReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(4) var<storage, read_write> outputRays : array<SparseVoxelSceneRay>;
@group(0) @binding(5) var<storage, read_write> outputReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(6) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(7) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(8) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(9) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(10) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(11) var<storage, read> lightReachability : array<u32>;
@group(0) @binding(12) var reservoirReceiverTex : texture_2d<u32>;
@group(0) @binding(13) var reservoirCoreTex : texture_2d<u32>;
@group(0) @binding(14) var<storage, read> activeRayIndices : array<u32>;
@group(0) @binding(15) var<storage, read> activeRayState : array<u32>;

fn spatial_reservoir_hash(value : u32) -> f32 {
    var bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    bits = bits * 277803737u;
    return f32((bits >> 8u) & 0x00ffffffu) / 16777216.0;
}

fn spatial_receiver_normal(pixel : vec2<u32>) -> vec3<f32> {
    return sparse_voxel_safe_normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz,
        vec3<f32>(0.0, 1.0, 0.0));
}

fn spatial_receiver_identity(pixel : vec2<u32>) -> vec4<u32> {
    return vec4<u32>(
        textureLoad(instanceIdTexture, vec2<i32>(pixel), 0).x,
        textureLoad(voxelIdTexture, vec2<i32>(pixel), 0).x,
        1u,
        0u);
}

fn spatial_invalid_reservoir(
    generation : u32, normalDepth : vec4<f32>,
    identity : vec4<u32>) -> SparseVoxelDIReservoir {
    return SparseVoxelDIReservoir(
        vec4<u32>(0xffffffffu, 0xffffffffu, 0u, generation),
        vec4<f32>(0.0), vec4<u32>(0u), normalDepth, identity);
}

fn spatial_light_is_infinite(light : WorldGILight) -> bool {
    return sparse_voxel_di_light_is_infinite(light);
}

fn spatial_target_proxy(
    ray : SparseVoxelSceneRay, light : WorldGILight,
    normal : vec3<f32>) -> f32 {
    return sparse_voxel_di_target(light, ray, normal);
}

fn reconnect_spatial_ray(
    neighborRay : SparseVoxelSceneRay,
    currentCandidate : SparseVoxelSceneRay,
    light : WorldGILight) -> SparseVoxelSceneRay {
    var result = neighborRay;
    result.seedFlags.y = sparse_voxel_copy_receiver_face(
        result.seedFlags.y, currentCandidate.seedFlags.y);
    let receiverPosition = currentCandidate.originMinimumDistance.xyz;
    if (spatial_light_is_infinite(light)) {
        result.originMinimumDistance = vec4<f32>(receiverPosition, 0.0);
        return result;
    }
    let sampledPoint = neighborRay.originMinimumDistance.xyz +
        neighborRay.directionMaximumDistance.xyz * neighborRay.directionMaximumDistance.w;
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

fn spatial_receiver_matches(
    currentNormalDepth : vec4<f32>,
    neighbor : SparseVoxelDIReservoir) -> bool {
    let neighborNormal = sparse_voxel_safe_normalize(
        neighbor.receiverNormalDepth.xyz, vec3<f32>(0.0, 1.0, 0.0));
    if (dot(currentNormalDepth.xyz, neighborNormal) < settings.normalThreshold) { return false; }
    let depthScale = max(
        max(abs(currentNormalDepth.w), abs(neighbor.receiverNormalDepth.w)),
        1.0e-3);
    return abs(currentNormalDepth.w - neighbor.receiverNormalDepth.w) <=
        settings.depthThreshold * depthScale;
}

// Spatial reuse accepts receiver geometry with normal and depth only. Sky
// directions remain receiver-local because sharing one stochastic direction
// creates coherent blocks of identical binary visibility.
@compute @workgroup_size(64, 1, 1)
fn resampleSparseVoxelDISpatially(@builtin(global_invocation_id) gid : vec3<u32>) {
    let dimensions = settings.dimensionsGeneration.xy;
    let raysPerPixel = max(settings.sourceDimensions.w, 1u);
    let compactIndex = gid.x;
    if (arrayLength(&activeRayState) < 4u ||
        compactIndex >= activeRayState[3] ||
        compactIndex >= arrayLength(&activeRayIndices)) { return; }
    let canonicalIndex = activeRayIndices[compactIndex];
    // Lane zero owns the pixel reservoir. Additional visibility lanes are
    // traced independently and must not race this single spatial output.
    if ((canonicalIndex % raysPerPixel) != 0u) { return; }
    let pixelIndex = canonicalIndex / raysPerPixel;
    if (pixelIndex >= dimensions.x * dimensions.y) { return; }
    let outputIndex = pixelIndex * raysPerPixel;
    let reservoirIndex = pixelIndex;
    let pixel = vec2<u32>(
        pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    if (any(pixel >= dimensions)) { return; }
    if (outputIndex >= arrayLength(&inputRays) ||
        reservoirIndex >= arrayLength(&inputReservoirs) ||
        outputIndex >= arrayLength(&outputRays) ||
        reservoirIndex >= arrayLength(&outputReservoirs)) { return; }
    // Initial candidates are pre-resampled into the same pixel-major ray-lane
    // layout as the temporal reservoirs.
    let candidateIndex = outputIndex;
    if (candidateIndex >= arrayLength(&candidates)) {
        let currentNormalDepth = vec4<f32>(0.0);
        let currentIdentity = vec4<u32>(0u);
        outputRays[outputIndex] = SparseVoxelSceneRay(
            vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u), vec4<u32>(0u), vec4<f32>(0.0));
        outputReservoirs[reservoirIndex] = spatial_invalid_reservoir(
            settings.dimensionsGeneration.w, currentNormalDepth, currentIdentity);
        return;
    }
    let reference = candidates[candidateIndex];
    // Sparse scheduling is authoritative. At quarter rate three out of four
    // slots are inactive, so reject them before loading depth, normals and two
    // identity textures. Downstream passes gate all ray fields on ACTIVE and
    // all reservoir fields on state.z, so invalidate those two words instead
    // of rewriting both large records.
    if ((reference.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) {
        outputRays[outputIndex].seedFlags.y = 0u;
        outputReservoirs[reservoirIndex].state.z = 0u;
        return;
    }
    let sourceDimensions = max(
        settings.sourceDimensions.xy, vec2<u32>(1u));
    let sourcePixel = min(
        reference.pixelRayLight.xy,
        sourceDimensions - vec2<u32>(1u));
    let sourceIndex =
        sourcePixel.y * sourceDimensions.x + sourcePixel.x;
    // Both outputs are frame-local, so a slot this dispatch leaves unwritten
    // keeps whatever the same slot held maxFramesInFlight frames ago. Publish
    // an explicitly invalid reservoir rather than returning early.
    if (sourceIndex >= arrayLength(&linearDepth)) {
        outputRays[outputIndex] = SparseVoxelSceneRay(
            vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u), vec4<u32>(0u),
            vec4<f32>(0.0));
        outputReservoirs[reservoirIndex] = spatial_invalid_reservoir(
            settings.dimensionsGeneration.w, vec4<f32>(0.0), vec4<u32>(0u));
        return;
    }
    let currentNormal = spatial_receiver_normal(sourcePixel);
    let currentNormalDepth =
        vec4<f32>(currentNormal, linearDepth[sourceIndex]);
    let currentIdentity = spatial_receiver_identity(sourcePixel);
    // Spatial reuse may improve a ray selected for this frame, but it must not
    // manufacture active rays for unscheduled full-resolution slots.
    var selected = inputRays[outputIndex];
    var selectedReservoir = inputReservoirs[reservoirIndex];
    var selectedTarget = selectedReservoir.weights.y;
    var weightSum = selectedReservoir.weights.x;
    var m = selectedReservoir.identity.z;
    var valid = selectedReservoir.state.z != 0u &&
        selectedReservoir.identity.w == settings.dimensionsGeneration.w &&
        spatial_receiver_matches(currentNormalDepth, selectedReservoir) &&
        (selected.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
        selectedTarget > 0.0 && weightSum > 0.0;
    if (!valid) { weightSum = 0.0; m = 0u; selectedTarget = 0.0; }

    // The temporal pass deliberately emits fresh sky directions. Forward
    // those unchanged instead of correlating neighbouring receivers here.
    if (valid && selectedReservoir.identity.x < arrayLength(&lights) &&
        lights[selectedReservoir.identity.x].flags.y ==
            WORLD_GI_LIGHT_TYPE_SKY) {
        selectedReservoir.state.z = 2u;
        outputRays[outputIndex] = selected;
        outputReservoirs[reservoirIndex] = selectedReservoir;
        return;
    }

    // Classic four-neighbour spatial ReSTIR. A fixed footprint avoids changing
    // the proposal population according to the current reservoir state.
    let offsets = array<vec2<i32>, 4>(
        vec2<i32>(-1, 0), vec2<i32>(1, 0),
        vec2<i32>(0, -1), vec2<i32>(0, 1));
    for (var neighborSlot = 0u; neighborSlot < 4u; neighborSlot = neighborSlot + 1u) {
        let neighborPixel = vec2<i32>(pixel) + offsets[neighborSlot];
        if (any(neighborPixel < vec2<i32>(0)) || any(neighborPixel >= vec2<i32>(dimensions))) { continue; }
        let neighborReservoirIndex =
            u32(neighborPixel.y) * dimensions.x + u32(neighborPixel.x);
        let neighborRayIndex =
            neighborReservoirIndex * raysPerPixel;
        if (neighborRayIndex >= arrayLength(&inputRays) ||
            neighborReservoirIndex >= arrayLength(&inputReservoirs)) {
            continue;
        }
        // 1. Texture reads (32 bytes, 2D-cached) — cheap neighbor probe.
        let receiverData = textureLoad(reservoirReceiverTex, neighborPixel, 0);
        let coreData = textureLoad(reservoirCoreTex, neighborPixel, 0);

        // 2. Early rejection from texture data (validity + active flag).
        let validity = coreData.w & 0xFFu;
        let activeFlag = (coreData.w >> 8u) & 0x1u;
        let cachedGeneration = (coreData.w >> 9u) & 0x007fffffu;
        if (validity == 0u || activeFlag == 0u ||
            cachedGeneration !=
                (settings.dimensionsGeneration.w & 0x007fffffu)) {
            continue;
        }

        // 3. Receiver validation from texture (normal and depth only).
        let neighborNormal =
            sparse_voxel_unpack_octahedral_normal(receiverData.z);
        let neighborDepth = bitcast<f32>(receiverData.w);
        if (dot(currentNormalDepth.xyz, neighborNormal) < settings.normalThreshold) { continue; }
        let depthScale = max(
            max(abs(currentNormalDepth.w), abs(neighborDepth)), 1.0e-3);
        if (abs(currentNormalDepth.w - neighborDepth) > settings.depthThreshold * depthScale) { continue; }

        // 4. Light bounds check from texture.
        let neighborLightIndex = coreData.x;
        if (neighborLightIndex >= arrayLength(&lights)) { continue; }
        if (lights[neighborLightIndex].flags.y ==
            WORLD_GI_LIGHT_TYPE_SKY) { continue; }

        // 5. Buffer read only for normal/depth-compatible neighbours.
        let neighborRay = inputRays[neighborRayIndex];

        // 6. RIS weight and M from texture (no reservoir buffer read needed).
        let neighborM = max(coreData.y, 1u);
        let reconnected = reconnect_spatial_ray(
            neighborRay, reference,
            lights[neighborLightIndex]);
        if ((reconnected.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) { continue; }
        let currentTarget = spatial_target_proxy(
            reconnected, lights[neighborLightIndex], currentNormal);
        let neighborRisWeight = bitcast<f32>(coreData.z);
        let neighborWeightSum = currentTarget * neighborRisWeight * f32(neighborM);
        if (neighborWeightSum <= 0.0) { continue; }
        m = m + neighborM;
        weightSum = weightSum + neighborWeightSum;
        // Mix in the per-frame seed so the neighbor-replace decision is
        // re-drawn every frame instead of locking to one deterministic
        // outcome per pixel.
        let replace = spatial_reservoir_hash(
            reference.seedFlags.x ^ neighborRay.seedFlags.x ^
            (neighborSlot * 3266489917u) ^
            (settings.sourceDimensions.z * 0x85ebca6bu)) <
            neighborWeightSum / max(weightSum, 1.0e-20);
        if (replace) {
            // 7. Lazy buffer read only when accepted.
            let neighborReservoir =
                inputReservoirs[neighborReservoirIndex];
            selected = reconnected;
            selected.pixelRayLight.x = reference.pixelRayLight.x;
            selected.pixelRayLight.y = reference.pixelRayLight.y;
            selected.pixelRayLight.z = neighborReservoir.identity.y;
            selected.seedFlags.z = settings.dimensionsGeneration.w;
            selected.seedFlags.w = reference.seedFlags.w;
            selectedTarget = currentTarget;
            selectedReservoir = neighborReservoir;
            valid = true;
        }
    }
    if (!valid || m == 0u || selectedTarget <= 0.0) {
        outputRays[outputIndex] = SparseVoxelSceneRay(
            vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u), vec4<u32>(0u), vec4<f32>(0.0));
        outputReservoirs[reservoirIndex] = spatial_invalid_reservoir(
            settings.dimensionsGeneration.w, currentNormalDepth, currentIdentity);
        return;
    }
    // Bound W in ray space; see the matching clamp in the temporal pass.
    let risWeight = min(
        weightSum / (f32(m) * selectedTarget),
        max(settings.maximumWeight, 1.0e-4));
    selectedReservoir.identity.z = m;
    selectedReservoir.identity.w = settings.dimensionsGeneration.w;
    selectedReservoir.weights = vec4<f32>(
        weightSum, selectedTarget, selectedReservoir.weights.z, risWeight);
    selectedReservoir.state.z = 2u;
    selectedReservoir.receiverNormalDepth = currentNormalDepth;
    selectedReservoir.receiverIdentity = currentIdentity;
    outputRays[outputIndex] = selected;
    outputReservoirs[reservoirIndex] = selectedReservoir;
}
