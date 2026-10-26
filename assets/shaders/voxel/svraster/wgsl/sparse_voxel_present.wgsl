#include "sparse_voxel_types.wgsl"

struct DebugPresentUniforms {
    width : u32,
    height : u32,
    mode : u32,
    linearDepthScale : f32,
    shadowWidth : u32,
    shadowHeight : u32,
    reserved0 : u32,
    reserved1 : u32,
    skyUpExponent : vec4<f32>,
    skyHorizonIntensity : vec4<f32>,
    skyZenith : vec4<f32>,
    skyGround : vec4<f32>,
}

struct SparseVoxelOccupancyDDAResult {
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

const DEBUG_BASE_COLOR : u32 = 0u;
const DEBUG_GEOMETRY_NORMAL : u32 = 1u;
const DEBUG_SHADING_NORMAL : u32 = 2u;
const DEBUG_ROUGHNESS : u32 = 3u;
const DEBUG_METALLIC : u32 = 4u;
const DEBUG_EMISSIVE : u32 = 5u;
const DEBUG_MOTION_VECTOR : u32 = 6u;
const DEBUG_HARDWARE_DEPTH : u32 = 7u;
const DEBUG_LINEAR_DEPTH : u32 = 8u;
const DEBUG_COVERAGE : u32 = 9u;
const DEBUG_INSTANCE_ID : u32 = 10u;
const DEBUG_VOXEL_ID : u32 = 11u;
const DEBUG_OCCUPANCY_SHADOW : u32 = 12u;
const DEBUG_OCCUPANCY_HIT_CELL : u32 = 13u;
const DEBUG_SCENE_RAY_SHADING : u32 = 14u;
const DEBUG_SCENE_RAY_STATUS : u32 = 15u;
const DEBUG_SCENE_RAY_SCREEN_TRACE : u32 = 16u;
const DEBUG_SCENE_RAY_WORLD_TRACE : u32 = 17u;
const DEBUG_SCENE_RAY_LIGHTING_ACCUMULATION : u32 = 18u;
const DEBUG_BRICK_OVERDRAW : u32 = 19u;
const DEBUG_MICRO_SURFACE : u32 = 20u;
const DEBUG_MATERIAL_HANDLE : u32 = 21u;
const DEBUG_MATERIAL_PROGRAM : u32 = 22u;
const DEBUG_MATERIAL_CONTEXT : u32 = 23u;
const DEBUG_MATERIAL_BASE_HEIGHT : u32 = 24u;
const DEBUG_MATERIAL_MODIFIER : u32 = 25u;
const DEBUG_MATERIAL_FINAL_OCCUPANCY : u32 = 26u;
const DEBUG_MATERIAL_LAYER_WEIGHT : u32 = 27u;
const DEBUG_MATERIAL_EXACT_FALLBACK : u32 = 28u;
const DEBUG_TERRAIN_CHUNK_OWNER : u32 = 29u;
const DEBUG_TERRAIN_CHUNK_BOUNDARIES : u32 = 30u;
const DEBUG_TERRAIN_CHUNK_LOD : u32 = 31u;
const DEBUG_TERRAIN_CHUNK_SLOT : u32 = 32u;
const DEBUG_TERRAIN_CHUNK_GENERATION : u32 = 33u;
const DEBUG_TERRAIN_BRICK_LOD : u32 = 34u;
const DEBUG_LAMBERT : u32 = 35u;
const MICRO_PAYLOAD_VALID : u32 = 1u << 28u;
const MICRO_PAYLOAD_FALLBACK : u32 = 1u << 30u;
const BRICK_LOD_DEBUG_LOCAL_SHIFT : u32 = 16u;
const BRICK_LOD_DEBUG_CLIPMAP_SHIFT : u32 = 0u;
const BRICK_LOD_DEBUG_MASK : u32 = 0xffu;

// Fragment invocations per pixel, mapped logarithmically. A linear 0..16 ramp
// made ordinary 8-16x terrain overlap occupy only the yellow/white end and a
// genuinely pathological pixel indistinguishable from a moderately expensive
// one. The stops now read approximately as 1, 4, 16, and 64 invocations, with
// white reserved for 64x or worse.
const BRICK_OVERDRAW_SATURATION : f32 = 64.0;

fn brickOverdrawColor(count : u32) -> vec3<f32> {
    if (count == 0u) { return vec3<f32>(0.0, 0.0, 0.12); }
    let t = clamp(
        log2(f32(count) + 1.0) /
            log2(BRICK_OVERDRAW_SATURATION + 1.0),
        0.0, 1.0);
    // Blue -> cyan -> green -> yellow -> red, then white past saturation, so
    // the eye reads both the magnitude and the point where it runs out of
    // range. A single-channel ramp hides an order of magnitude in the dark end.
    let ramp = vec3<f32>(
        clamp(t * 3.0 - 1.0, 0.0, 1.0),
        clamp(select(t * 3.0, 3.0 - t * 3.0, t > 0.5), 0.0, 1.0),
        clamp(1.0 - t * 3.0, 0.0, 1.0));
    return select(ramp, vec3<f32>(1.0), f32(count) >= BRICK_OVERDRAW_SATURATION);
}
const DDA_STATUS_HIT : u32 = 2u;
const DDA_STATUS_QUERY_LIMIT : u32 = 3u;
const SCENE_RAY_STATUS_PENDING : u32 = 1u;
const SCENE_RAY_STATUS_HIT : u32 = 2u;
const SCENE_RAY_STATUS_MISS_TMAX : u32 = 3u;
const SCENE_RAY_STATUS_ESCAPED_SCENE : u32 = 4u;
const SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME : u32 = 5u;
const SCENE_RAY_STATUS_UNKNOWN_RESIDENCY : u32 = 6u;
const SCENE_RAY_STATUS_QUERY_LIMIT : u32 = 7u;
const INV_PI : f32 = 0.31830988618;

fn debugSkyRadiance(direction : vec3<f32>) -> vec3<f32> {
    let up = uniforms.skyUpExponent.xyz /
        max(length(uniforms.skyUpExponent.xyz), 1.0e-6);
    let elevation = clamp(dot(normalize(direction), up), -1.0, 1.0);
    let destination = select(
        uniforms.skyGround.xyz, uniforms.skyZenith.xyz, elevation >= 0.0);
    let curve = pow(abs(elevation), max(uniforms.skyUpExponent.w, 0.01));
    return mix(uniforms.skyHorizonIntensity.xyz, destination, curve) *
        max(uniforms.skyHorizonIntensity.w, 0.0);
}

fn debugSkyBackground(pixel : vec2<u32>) -> vec3<f32> {
    // Use the stable, unjittered principal point. The analytic background has
    // no geometry edge that benefits from temporal jitter.
    let principal = vec2<f32>(
        f32(uniforms.width), f32(uniforms.height)) * 0.5;
    let cameraDirection = normalize(vec3<f32>(
        (f32(pixel.x) + 0.5 - principal.x) /
            max(raster.focalPrincipal.x, 1.0e-6),
        (f32(pixel.y) + 0.5 - principal.y) /
            max(raster.focalPrincipal.y, 1.0e-6),
        1.0));
    let worldDirection = normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    return debugSkyRadiance(worldDirection);
}

fn debugSkyDiffuse(normal : vec3<f32>) -> vec3<f32> {
    let up = uniforms.skyUpExponent.xyz /
        max(length(uniforms.skyUpExponent.xyz), 1.0e-6);
    // A cheap hemispherical diffuse approximation: sample the analytic sky in
    // the normal direction and blend in the opposite hemisphere so vertical
    // faces and undersides do not collapse to black.
    let facing = dot(normalize(normal), up) * 0.5 + 0.5;
    let upper = debugSkyRadiance(normalize(normal + up * 0.35));
    let lower = max(uniforms.skyGround.xyz, vec3<f32>(0.02)) *
        max(uniforms.skyHorizonIntensity.w, 0.0);
    return max(mix(lower, upper, 0.3 + 0.7 * facing), vec3<f32>(0.03));
}

struct SparseVoxelSceneRayHit {
    hitPositionDistance : vec4<f32>,
    globalCell : vec3<i32>,
    status : u32,
    identity : vec4<u32>,
}

struct SparseVoxelSceneRay {
    originMinimumDistance : vec4<f32>,
    directionMaximumDistance : vec4<f32>,
    pixelRayLight : vec4<u32>,
    seedFlags : vec4<u32>,
    lightSamplePdfWeight : vec4<f32>,
}

struct WorldGILight {
    positionRange : vec4<f32>,
    directionType : vec4<f32>,
    shapeParams : vec4<f32>,
    colorIntensity : vec4<f32>,
    flags : vec4<u32>,
}

#include "sparse_voxel_sky.wgsl"

struct SparseVoxelDIReservoir {
    identity : vec4<u32>,
    weights : vec4<f32>,
    state : vec4<u32>,
    receiverNormalDepth : vec4<f32>,
    receiverIdentity : vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : DebugPresentUniforms;
@group(0) @binding(1) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(2) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(3) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(4) var materialUvTexture : texture_2d<f32>;
@group(0) @binding(5) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(6) var materialPropertiesTexture : texture_2d<f32>;
@group(0) @binding(7) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(8) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(9) var normalTexture : texture_2d<f32>;
@group(0) @binding(10) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(11) var depthTexture : texture_depth_2d;
@group(0) @binding(12) var<storage, read> coverage : array<u32>;
@group(0) @binding(13) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(14) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var<storage, read> occupancyShadowResults : array<SparseVoxelOccupancyDDAResult>;
@group(0) @binding(16) var<storage, read> stableSceneHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(17) var<storage, read> sceneRays : array<SparseVoxelSceneRay>;
@group(0) @binding(18) var<storage, read> sceneLights : array<WorldGILight>;
@group(0) @binding(19) var<storage, read> sceneDIReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(20) var<storage, read> screenSceneHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(21) var<storage, read> mergedSceneHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(22) var<storage, read_write> sceneRayStatistics : array<atomic<u32>>;
@group(0) @binding(23) var<storage, read> skyRays : array<SparseVoxelSceneRay>;
@group(0) @binding(24) var<storage, read> skyMergedHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(25) var<storage, read> skyStableVisibility : array<f32>;
@group(0) @binding(26) var<storage, read> stableSceneVisibility : array<f32>;
@group(0) @binding(27) var reactiveOutput : texture_storage_2d<r16float, write>;
@group(0) @binding(28) var<uniform> raster : SparseVoxelRasterUniforms;
// Per-pixel fragment invocation counts published by the brick resolve pass. A
// 1-word stub in every mode but DEBUG_BRICK_OVERDRAW.
@group(0) @binding(29) var<storage, read> brickOverdraw : array<u32>;
@group(0) @binding(30) var<storage, read> microPayloads : array<u32>;

struct SparseVoxelTerrainOwnership {
    regionMappingId : u32,
    chunkAddressX : i32,
    chunkAddressY : i32,
    spatialLOD : u32,
    physicalSlot : u32,
    publicationGeneration : u32,
    rasterRootCount : u32,
    tree64StableInstanceId : u32,
}

@group(0) @binding(31) var<storage, read> terrainOwnershipIds : array<u32>;
@group(0) @binding(32) var<storage, read> terrainOwnershipTable :
    array<SparseVoxelTerrainOwnership>;

const TERRAIN_INVALID_SLOT : u32 = 0xffffffffu;

fn terrainOwnershipHash(valueIn : u32) -> u32 {
    var value = valueIn;
    value = value ^ (value >> 16u);
    value = value * 0x7feb352du;
    value = value ^ (value >> 15u);
    value = value * 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn terrainOwnership(mappingId : u32) -> SparseVoxelTerrainOwnership {
    var missing = SparseVoxelTerrainOwnership(
        mappingId, 0, 0, 0u, TERRAIN_INVALID_SLOT, 0u, 0u,
        TERRAIN_INVALID_SLOT);
    let capacity = arrayLength(&terrainOwnershipTable);
    if (capacity == 0u) { return missing; }
    var bucket = terrainOwnershipHash(mappingId) & (capacity - 1u);
    for (var probe = 0u; probe < capacity; probe = probe + 1u) {
        let candidate = terrainOwnershipTable[bucket];
        if (candidate.physicalSlot == TERRAIN_INVALID_SLOT) {
            return missing;
        }
        if (candidate.regionMappingId == mappingId) {
            return candidate;
        }
        bucket = (bucket + 1u) & (capacity - 1u);
    }
    return missing;
}

fn terrainChunkBoundary(pixel : vec2<u32>, mappingId : u32) -> bool {
    let size = vec2<u32>(uniforms.width, uniforms.height);
    let left = vec2<u32>(select(pixel.x, pixel.x - 1u, pixel.x > 0u), pixel.y);
    let right = vec2<u32>(min(pixel.x + 1u, size.x - 1u), pixel.y);
    let up = vec2<u32>(pixel.x, select(pixel.y, pixel.y - 1u, pixel.y > 0u));
    let down = vec2<u32>(pixel.x, min(pixel.y + 1u, size.y - 1u));
    let neighbors = array<vec2<u32>, 4>(left, right, up, down);
    for (var index = 0u; index < 4u; index = index + 1u) {
        let neighbor = neighbors[index];
        let neighborIndex = neighbor.y * size.x + neighbor.x;
        if (neighborIndex < arrayLength(&terrainOwnershipIds) &&
            coverage[neighborIndex] != 0u &&
            terrainOwnershipIds[neighborIndex] != mappingId) {
            return true;
        }
    }
    return false;
}

fn hashId(value : u32) -> vec3<f32> {
    var hash = value + 0x9e3779b9u;
    hash = (hash ^ (hash >> 16u)) * 0x85ebca6bu;
    hash = (hash ^ (hash >> 13u)) * 0xc2b2ae35u;
    hash = hash ^ (hash >> 16u);
    return vec3<f32>(
        f32(hash & 0xffu),
        f32((hash >> 8u) & 0xffu),
        f32((hash >> 16u) & 0xffu)) / 255.0;
}

fn occupancyCellId(cell : vec3<i32>) -> u32 {
    let bits = bitcast<vec3<u32>>(cell);
    return (bits.x * 0x8da6b343u) ^
        (bits.y * 0xd8163841u) ^
        (bits.z * 0xcb1ab31fu);
}

fn sceneRayVisibility(hit : SparseVoxelSceneRayHit) -> f32 {
    if (hit.status == SCENE_RAY_STATUS_HIT) { return 0.0; }
    if (hit.status == SCENE_RAY_STATUS_MISS_TMAX || hit.status == SCENE_RAY_STATUS_ESCAPED_SCENE) { return 1.0; }
    // Missing Tree64 coverage or an incomplete traversal must fail closed for
    // shadow rays; treating uncertainty as visible leaks sky/finite lights.
    if (hit.status == SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
        hit.status == SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
        hit.status == SCENE_RAY_STATUS_QUERY_LIMIT) { return 0.0; }
    return 0.0;
}

fn sceneRayTraceResolved(hit : SparseVoxelSceneRayHit) -> bool {
    return hit.status == SCENE_RAY_STATUS_HIT ||
        hit.status == SCENE_RAY_STATUS_MISS_TMAX ||
        hit.status == SCENE_RAY_STATUS_ESCAPED_SCENE;
}

fn sceneRayStatusColor(status : u32) -> vec3<f32> {
    if (status == SCENE_RAY_STATUS_HIT) { return vec3<f32>(0.0); }
    if (status == SCENE_RAY_STATUS_MISS_TMAX) { return vec3<f32>(1.0); }
    if (status == SCENE_RAY_STATUS_ESCAPED_SCENE) { return vec3<f32>(0.2, 0.6, 1.0); }
    if (status == SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME) { return vec3<f32>(1.0, 0.8, 0.1); }
    if (status == SCENE_RAY_STATUS_UNKNOWN_RESIDENCY) { return vec3<f32>(1.0, 0.2, 1.0); }
    if (status == SCENE_RAY_STATUS_QUERY_LIMIT) { return vec3<f32>(1.0, 0.0, 0.0); }
    return vec3<f32>(0.25);
}

fn present_transform_vector(
    matrix : mat4x4<f32>, value : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(value, 0.0)).xyz;
}

fn present_camera_direction(pixel : vec2<u32>) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - raster.focalPrincipal.z) /
            max(raster.focalPrincipal.x, 1.0e-6),
        (f32(pixel.y) + 0.5 - raster.focalPrincipal.w) /
            max(raster.focalPrincipal.y, 1.0e-6),
        1.0);
    return normalize(present_transform_vector(raster.cameraToWorld, cameraDirection));
}

fn present_sky_radiance(direction : vec3<f32>) -> vec3<f32> {
    for (var lightIndex = 0u; lightIndex < arrayLength(&sceneLights); lightIndex = lightIndex + 1u) {
        let light = sceneLights[lightIndex];
        if (light.flags.y == 8u && (light.flags.x & 1u) != 0u &&
            light.colorIntensity.w > 0.0) {
            return sparse_voxel_sky_radiance(light, direction);
        }
    }
    return vec3<f32>(0.0);
}

fn present_scene_ray_dimensions() -> vec2<u32> {
    return vec2<u32>(
        max(uniforms.reserved0 & 0x00ffffffu, 1u),
        max(uniforms.reserved1, 1u));
}

fn present_rays_per_pixel() -> u32 {
    return max((uniforms.reserved0 >> 24u) & 0xfu, 1u);
}

fn present_uses_independent_rays() -> bool {
    return (uniforms.reserved0 & 0x40000000u) != 0u;
}

fn present_screen_trace_enabled() -> bool {
    return (uniforms.reserved0 & 0x20000000u) != 0u;
}

fn present_ray_group_base(rayPixel : vec2<u32>) -> u32 {
    let rayDimensions = present_scene_ray_dimensions();
    return (rayPixel.y * rayDimensions.x + rayPixel.x) *
        present_rays_per_pixel();
}

fn present_reservoir_index(rayIndex : u32) -> u32 {
    return rayIndex / present_rays_per_pixel();
}

// Lighting contribution of one reduced-resolution ray slot. Split out of the
// shading body so the upsample can evaluate several slots and blend them.
fn present_finite_lighting(rayIndex : u32, normal : vec3<f32>) -> vec3<f32> {
    if (rayIndex >= arrayLength(&sceneRays) ||
        rayIndex >= arrayLength(&stableSceneHits)) {
        return vec3<f32>(0.0);
    }
    let ray = sceneRays[rayIndex];
    if ((ray.seedFlags.y & 1u) == 0u) { return vec3<f32>(0.0); }
    var sampleWeight = 0.0;
    let reservoirLane = rayIndex % present_rays_per_pixel();
    if (present_uses_independent_rays() && reservoirLane != 0u) {
        let proposal =
            max(ray.lightSamplePdfWeight.x, 0.0) *
            max(ray.lightSamplePdfWeight.y, 0.0);
        if (proposal <= 0.0) { return vec3<f32>(0.0); }
        sampleWeight = 1.0 / proposal;
    } else {
        let reservoirIndex = present_reservoir_index(rayIndex);
        if (reservoirIndex >= arrayLength(&sceneDIReservoirs)) {
            return vec3<f32>(0.0);
        }
        let reservoir = sceneDIReservoirs[reservoirIndex];
        if (reservoir.state.z == 0u) { return vec3<f32>(0.0); }
        sampleWeight = reservoir.weights.w;
    }
    let light = sceneLights[min(ray.pixelRayLight.w, arrayLength(&sceneLights) - 1u)];
    let distanceSq = max(ray.directionMaximumDistance.w * ray.directionMaximumDistance.w, 1.0e-4);
    let finite = light.flags.y != 1u && light.flags.y != 8u;
    let attenuation = select(1.0, 1.0 / distanceSq, finite);
    var emitterCosine = 1.0;
    if (light.flags.y == 6u) {
        if (light.shapeParams.x > 1.0e-6) {
            let sampledPoint = ray.originMinimumDistance.xyz +
                ray.directionMaximumDistance.xyz * ray.directionMaximumDistance.w;
            let emitterNormal = normalize(sampledPoint - light.positionRange.xyz);
            emitterCosine = max(dot(emitterNormal, -ray.directionMaximumDistance.xyz), 0.0);
        }
    } else if (light.flags.y == 7u) {
        let normalLength = length(light.directionType.xyz);
        let emitterNormal = select(
            vec3<f32>(0.0, -1.0, 0.0),
            light.directionType.xyz / max(normalLength, 1.0e-6),
            normalLength > 1.0e-6);
        emitterCosine = max(dot(emitterNormal, -ray.directionMaximumDistance.xyz), 0.0);
    }
    var lightRadiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) *
        max(light.colorIntensity.w, 0.0) * INV_PI;
    if (light.flags.y == 8u) {
        lightRadiance = sparse_voxel_sky_radiance(light, ray.directionMaximumDistance.xyz) * INV_PI;
    }
    // Production does not allocate the diagnostic per-ray visibility history;
    // its fallback buffer contains one unoccluded value. Reading that fallback
    // made every direct shadow ray visible regardless of the actual trace.
    // Temporal lighting accumulation is a separate full-resolution stage, so
    // this resolve must publish the current traced visibility only.
    let visibility = sceneRayVisibility(stableSceneHits[rayIndex]);
    return lightRadiance * attenuation *
        max(dot(normal, ray.directionMaximumDistance.xyz), 0.0) *
        emitterCosine * visibility * sampleWeight;
}

fn present_sky_lighting(rayIndex : u32) -> vec3<f32> {
    if (rayIndex >= arrayLength(&skyRays)) { return vec3<f32>(0.0); }
    let skyRay = skyRays[rayIndex];
    if ((skyRay.seedFlags.y & 1u) == 0u) { return vec3<f32>(0.0); }
    let skyLight = sceneLights[min(skyRay.pixelRayLight.w, arrayLength(&sceneLights) - 1u)];
    // Accumulated mean only; see present_finite_lighting.
    let visibility = skyStableVisibility[
        min(rayIndex, arrayLength(&skyStableVisibility) - 1u)];
    return sparse_voxel_sky_radiance(
        skyLight, skyRay.directionMaximumDistance.xyz) * visibility;
}

struct PresentLightingSample {
    value : vec3<f32>,
    valid : bool,
}

fn present_ray_group_lighting(
    rayPixel : vec2<u32>, normal : vec3<f32>) -> PresentLightingSample {
    let base = present_ray_group_base(rayPixel);
    let slotCount = select(
        1u, present_rays_per_pixel(), present_uses_independent_rays());
    // Match the production light field: the dedicated sky visibility lane and
    // the non-sky ReSTIR lanes are separate estimator strata. Average within
    // each group and add the groups.
    var skySum = vec3<f32>(0.0);
    var skyCount = 0.0;
    var emitterSum = vec3<f32>(0.0);
    var emitterCount = 0.0;
    var count = 0.0;
    for (var raySlot = 0u; raySlot < slotCount;
         raySlot = raySlot + 1u) {
        let rayIndex = base + raySlot;
        if (rayIndex >= arrayLength(&sceneRays) ||
            rayIndex >= arrayLength(&stableSceneHits) ||
            (sceneRays[rayIndex].seedFlags.y & 1u) == 0u) {
            continue;
        }
        if (!sceneRayTraceResolved(stableSceneHits[rayIndex])) {
            continue;
        }
        let requiresReservoir =
            !present_uses_independent_rays() || raySlot == 0u;
        let reservoirIndex = present_reservoir_index(rayIndex);
        if (requiresReservoir &&
            (reservoirIndex >= arrayLength(&sceneDIReservoirs) ||
             sceneDIReservoirs[reservoirIndex].state.z == 0u)) {
            continue;
        }
        let finiteLighting = present_finite_lighting(rayIndex, normal);
        let lightIndex = sceneRays[rayIndex].pixelRayLight.w;
        let sky = lightIndex < arrayLength(&sceneLights) &&
            sceneLights[lightIndex].flags.y == 8u;
        if (sky) {
            skySum = skySum + finiteLighting;
            skyCount = skyCount + 1.0;
        } else {
            emitterSum = emitterSum + finiteLighting;
            emitterCount = emitterCount + 1.0;
        }
        // Legacy separate-sky buffers are empty in the production ReSTIR
        // path, but keep their contribution valid for focused debug callers.
        let legacySky = present_sky_lighting(rayIndex);
        if (any(legacySky != vec3<f32>(0.0))) {
            skySum = skySum + legacySky;
            skyCount = skyCount + 1.0;
        }
        count = count + 1.0;
    }
    let sum = skySum / max(skyCount, 1.0) +
        emitterSum / max(emitterCount, 1.0);
    return PresentLightingSample(
        sum, count > 0.0);
}

// The full-resolution pixel that a reduced-resolution ray slot sampled from.
// Mirrors the mapping the ray-create pass uses so it needs no buffer lookup.
fn present_ray_source_pixel(
    rayPixel : vec2<u32>, rayDimensions : vec2<u32>,
    rasterDimensions : vec2<u32>) -> vec2<u32> {
    return min(
        vec2<u32>(
            (vec2<f32>(rayPixel) + vec2<f32>(0.5)) *
            vec2<f32>(rasterDimensions) / vec2<f32>(rayDimensions)),
        rasterDimensions - vec2<u32>(1u));
}

// Whether a reduced-resolution ray slot may contribute to this pixel: it must
// hold data this frame and sit on the same surface.
fn present_tap_usable(
    rayPixel : vec2<u32>, rayDimensions : vec2<u32>,
    rasterDimensions : vec2<u32>, currentInstance : u32,
    currentNormal : vec3<f32>, currentDepth : f32) -> bool {
    let rayIndex = present_ray_group_base(rayPixel);
    var hasActiveRay = false;
    let slotCount = select(
        1u, present_rays_per_pixel(), present_uses_independent_rays());
    for (var raySlot = 0u; raySlot < slotCount;
         raySlot = raySlot + 1u) {
        let slotIndex = rayIndex + raySlot;
        let rayActive = slotIndex < arrayLength(&sceneRays) &&
            (sceneRays[slotIndex].seedFlags.y & 1u) != 0u;
        let finiteValid = rayActive &&
            slotIndex < arrayLength(&stableSceneHits) &&
            sceneRayTraceResolved(stableSceneHits[slotIndex]) &&
            ((present_uses_independent_rays() && raySlot != 0u) ||
             (present_reservoir_index(slotIndex) <
                  arrayLength(&sceneDIReservoirs) &&
              sceneDIReservoirs[
                  present_reservoir_index(slotIndex)].state.z != 0u));
        let skyActive = slotIndex < arrayLength(&skyRays) &&
            (skyRays[slotIndex].seedFlags.y & 1u) != 0u;
        if (finiteValid || skyActive) {
            hasActiveRay = true;
            break;
        }
    }
    if (!hasActiveRay) { return false; }
    let sourcePixel = present_ray_source_pixel(
        rayPixel, rayDimensions, rasterDimensions);
    let sourceIndex = sourcePixel.y * uniforms.width + sourcePixel.x;
    if (sourceIndex >= arrayLength(&linearDepth)) { return false; }
    if (textureLoad(instanceIdTexture, vec2<i32>(sourcePixel), 0).x !=
        currentInstance) { return false; }
    let sampleNormalRaw = textureLoad(
        geometryNormalTexture, vec2<i32>(sourcePixel), 0).xyz;
    let sampleNormal = sampleNormalRaw /
        max(length(sampleNormalRaw), 1.0e-6);
    if (dot(currentNormal, sampleNormal) < 0.85) { return false; }
    let sampleDepth = linearDepth[sourceIndex];
    let depthScale = max(max(abs(currentDepth), abs(sampleDepth)), 1.0);
    return abs(currentDepth - sampleDepth) / depthScale <= 0.05;
}

@compute @workgroup_size(8, 8, 1)
fn debugPresent(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (pixel.x >= uniforms.width || pixel.y >= uniforms.height) {
        return;
    }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * uniforms.width + pixel.x;
    var color = textureLoad(baseColorTexture, pixelI, 0);
    // Geometry-only terrain shading. Return before reading any ray or light
    // history; this path requires only the current G-buffer and coverage.
    if (uniforms.mode == DEBUG_LAMBERT) {
        if (coverage[pixelIndex] != 0u) {
            let rawNormal = textureLoad(normalTexture, pixelI, 0).xyz;
            let normal = rawNormal / max(length(rawNormal), 1.0e-6);
            let light = normalize(vec3<f32>(0.4, 0.8, 0.3));
            color = vec4<f32>(color.rgb * (0.25 + 0.75 * max(dot(normal, light), 0.0)), 1.0);
        } else {
            color = vec4<f32>(debugSkyBackground(pixel), 1.0);
        }
        textureStore(reactiveOutput, pixelI, vec4<f32>(0.0));
        textureStore(outputTexture, pixelI, color);
        return;
    }
    var lightingColor = color;
    // Current-frame ReSTIR visibility remains receiver-local. The old resolve
    // bilinearly gathered traced neighbours and then fell back to a four-tap
    // cross. That projected each binary shadow result into screen-space bands
    // before either temporal accumulation or A-trous filtering could see it.
    // Unscheduled receivers stay explicitly unresolved here; the independent
    // accumulator retains their own validated history and the stateless A-trous
    // stage reconstructs only after that.
    let rayDimensions = present_scene_ray_dimensions();
    let rasterDimensions = max(
        vec2<u32>(uniforms.width, uniforms.height), vec2<u32>(1u));
    let nearestRayPixel = min(
        vec2<u32>(
            (vec2<f32>(pixel) + vec2<f32>(0.5)) *
            vec2<f32>(rayDimensions) / vec2<f32>(rasterDimensions)),
        rayDimensions - vec2<u32>(1u));
    let nearestRayIndex = present_ray_group_base(nearestRayPixel);

    let currentInstance = textureLoad(instanceIdTexture, pixelI, 0).x;
    let currentGeometryNormalRaw =
        textureLoad(geometryNormalTexture, pixelI, 0).xyz;
    let currentGeometryNormal = currentGeometryNormalRaw /
        max(length(currentGeometryNormalRaw), 1.0e-6);
    let currentLinearDepth = linearDepth[pixelIndex];
    let shadeNormalRaw = textureLoad(normalTexture, pixelI, 0).xyz;
    let shadeNormal = shadeNormalRaw / max(length(shadeNormalRaw), 1.0e-6);

    var sceneRayBase = nearestRayIndex;
    var lightingSum = vec3<f32>(0.0);
    var lightingWeight = 0.0;
    // Uncovered pixels get their colour from the environment in the compose
    // pass, so none of the tap work below would be used.
    let shadeThisPixel = pixelIndex < arrayLength(&coverage) && coverage[pixelIndex] != 0u;
    var directlyMeasured = false;
    if (shadeThisPixel) {
        let sourcePixel = present_ray_source_pixel(
            nearestRayPixel, rayDimensions, rasterDimensions);
        directlyMeasured = all(sourcePixel == pixel) && present_tap_usable(
            nearestRayPixel, rayDimensions, rasterDimensions,
            currentInstance, currentGeometryNormal, currentLinearDepth);
        if (directlyMeasured) {
            let groupLighting = present_ray_group_lighting(
                nearestRayPixel, shadeNormal);
            if (groupLighting.valid) {
                lightingSum = groupLighting.value;
                lightingWeight = 1.0;
            }
        }
    }
    let sceneHit = stableSceneHits[min(sceneRayBase, arrayLength(&stableSceneHits) - 1u)];
    {
        var directLighting = vec3<f32>(0.0);
        if (lightingWeight > 1.0e-5) {
            directLighting = lightingSum / lightingWeight;
        }
        // Material-independent diffuse light multiplier: visibility, light
        // colour, attenuation and cosine terms only. The compose pass applies
        // albedo, emissive and the environment after denoising.
        // Alpha is a current-frame reconstruction contract:
        //   1  = this pixel was measured by a traced receiver,
        //  -1  = a compatible current-frame neighbour supplied an estimate,
        //  -2  = covered, but no compatible measurement reached it yet.
        // Keeping unresolved pixels distinct is essential: treating them as
        // valid black samples makes the sparse pattern survive every filter.
        let reconstructionMarker = select(
            -2.0, -1.0, lightingWeight > 1.0e-5);
        lightingColor = vec4<f32>(
            directLighting,
            select(reconstructionMarker, 1.0, directlyMeasured));
        if (coverage[pixelIndex] == 0u) {
            lightingColor = vec4<f32>(0.0);
        }
    }
    var rawScreenHit = SparseVoxelSceneRayHit(
        vec4<f32>(0.0), vec3<i32>(-2147483648),
        SCENE_RAY_STATUS_PENDING, vec4<u32>(0u));
    if (present_screen_trace_enabled()) {
        rawScreenHit = screenSceneHits[
            min(sceneRayBase, arrayLength(&screenSceneHits) - 1u)];
    }
    let mergedHit = mergedSceneHits[min(sceneRayBase, arrayLength(&mergedSceneHits) - 1u)];
    let screenTraceColor = vec4<f32>(sceneRayStatusColor(rawScreenHit.status), 1.0);
    let worldTraceColor = vec4<f32>(select(vec3<f32>(0.08), sceneRayStatusColor(mergedHit.status), mergedHit.identity.z == 2u), 1.0);
    let collectStatistics = (uniforms.reserved0 & 0x80000000u) != 0u;
    if (collectStatistics && coverage[pixelIndex] != 0u) {
        let statisticSlotCount = select(
            1u, present_rays_per_pixel(), present_uses_independent_rays());
        for (var raySlot = 0u; raySlot < statisticSlotCount;
             raySlot = raySlot + 1u) {
            let rayIndex = sceneRayBase + raySlot;
            if (rayIndex >= arrayLength(&sceneRays)) { continue; }
            let statisticsRay = sceneRays[rayIndex];
            if (any(statisticsRay.pixelRayLight.xy != pixel)) { continue; }
            var statisticsScreenHit = SparseVoxelSceneRayHit(
                vec4<f32>(0.0), vec3<i32>(-2147483648),
                SCENE_RAY_STATUS_PENDING, vec4<u32>(0u));
            if (present_screen_trace_enabled()) {
                statisticsScreenHit = screenSceneHits[
                    min(rayIndex, arrayLength(&screenSceneHits) - 1u)];
            }
            let statisticsMergedHit = mergedSceneHits[
                min(rayIndex, arrayLength(&mergedSceneHits) - 1u)];
            atomicAdd(&sceneRayStatistics[0], 1u);
            let statisticsLightIndex = statisticsRay.pixelRayLight.w;
            if (statisticsLightIndex < arrayLength(&sceneLights) &&
                sceneLights[statisticsLightIndex].flags.y == 8u) {
                atomicAdd(&sceneRayStatistics[15], 1u);
                if (statisticsMergedHit.status == SCENE_RAY_STATUS_HIT) {
                    atomicAdd(&sceneRayStatistics[14], 1u);
                }
            }
            if (statisticsScreenHit.status == SCENE_RAY_STATUS_HIT) {
                atomicAdd(&sceneRayStatistics[1], 1u);
            } else {
                atomicAdd(&sceneRayStatistics[2], 1u);
            }
            if (statisticsMergedHit.identity.z == 2u &&
                statisticsMergedHit.status == SCENE_RAY_STATUS_HIT) {
                atomicAdd(&sceneRayStatistics[3], 1u);
            }
            if (statisticsMergedHit.identity.z == 2u &&
                statisticsMergedHit.status != SCENE_RAY_STATUS_HIT) {
                atomicAdd(&sceneRayStatistics[4], 1u);
            }
            if (statisticsMergedHit.status == SCENE_RAY_STATUS_ESCAPED_SCENE) {
                atomicAdd(&sceneRayStatistics[5], 1u);
            }
            if (statisticsMergedHit.status == SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
                statisticsMergedHit.status == SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
                statisticsMergedHit.status == SCENE_RAY_STATUS_QUERY_LIMIT) {
                atomicAdd(&sceneRayStatistics[6], 1u);
            }
            // The three reasons behind slot 6, separated. Their sum is slot 6,
            // but the total is not actionable: escaping the volume is a
            // clipmap sizing problem, unknown residency is the dynamic layer
            // declining to answer, and the query limit is a traversal budget.
            if (statisticsMergedHit.status == SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME) {
                atomicAdd(&sceneRayStatistics[11], 1u);
            }
            if (statisticsMergedHit.status == SCENE_RAY_STATUS_UNKNOWN_RESIDENCY) {
                atomicAdd(&sceneRayStatistics[12], 1u);
            }
            if (statisticsMergedHit.status == SCENE_RAY_STATUS_QUERY_LIMIT) {
                atomicAdd(&sceneRayStatistics[13], 1u);
            }
            if (!present_uses_independent_rays() &&
                present_reservoir_index(rayIndex) <
                    arrayLength(&sceneDIReservoirs)) {
                let statisticsReservoir = sceneDIReservoirs[
                    present_reservoir_index(rayIndex)];
                atomicAdd(
                    &sceneRayStatistics[7], statisticsReservoir.identity.z);
                if (statisticsReservoir.state.y != 0u) {
                    atomicAdd(&sceneRayStatistics[8], 1u);
                }
                if (statisticsReservoir.state.z == 2u) {
                    atomicAdd(&sceneRayStatistics[9], 1u);
                }
            }
            if ((statisticsRay.seedFlags.y & 1u) == 0u) {
                atomicAdd(&sceneRayStatistics[10], 1u);
            }
        }
    }
    let uncertain = mergedHit.status == SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
        mergedHit.status == SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
        mergedHit.status == SCENE_RAY_STATUS_QUERY_LIMIT;
    // A stochastic visibility sample differing from its running mean is
    // ordinary Monte Carlo noise, not geometric disocclusion. Marking it
    // reactive prevents both SVGF and TSR from accumulating sky lighting.
    let reactive = select(
        0.0,
        select(0.0, 1.0, uncertain),
        coverage[pixelIndex] != 0u);
    textureStore(reactiveOutput, pixelI, vec4<f32>(reactive, 0.0, 0.0, 0.0));
    if (uniforms.mode == DEBUG_SCENE_RAY_SHADING) {
        color = lightingColor;
    } else if (uniforms.mode == DEBUG_SCENE_RAY_STATUS) {
        color = vec4<f32>(sceneRayStatusColor(sceneHit.status), 1.0);
    } else if (uniforms.mode == DEBUG_SCENE_RAY_SCREEN_TRACE) {
        color = screenTraceColor;
    } else if (uniforms.mode == DEBUG_SCENE_RAY_WORLD_TRACE) {
        color = worldTraceColor;
    } else if (uniforms.mode == DEBUG_SCENE_RAY_LIGHTING_ACCUMULATION) {
        // Training/debug contract: raw current-frame sparse radiance in RGB
        // and the exact traced-receiver mask in alpha. Do not run the spatial
        // gather here; DEBUG_SCENE_RAY_SHADING is the reconstructed baseline.
        color = vec4<f32>(0.0);
        let sourcePixel = present_ray_source_pixel(
            nearestRayPixel, rayDimensions, rasterDimensions);
        if (shadeThisPixel && all(sourcePixel == pixel) &&
            present_tap_usable(
                nearestRayPixel, rayDimensions, rasterDimensions,
                currentInstance, currentGeometryNormal,
                currentLinearDepth)) {
            let sample = present_ray_group_lighting(
                nearestRayPixel, shadeNormal);
            if (sample.valid) {
                color = vec4<f32>(sample.value, 1.0);
            }
        }
    } else if (uniforms.mode == DEBUG_GEOMETRY_NORMAL) {
        color = vec4<f32>(textureLoad(geometryNormalTexture, pixelI, 0).xyz * 0.5 + 0.5, 1.0);
    } else if (uniforms.mode == DEBUG_SHADING_NORMAL) {
        color = vec4<f32>(textureLoad(normalTexture, pixelI, 0).xyz * 0.5 + 0.5, 1.0);
    } else if (uniforms.mode == DEBUG_ROUGHNESS) {
        color = vec4<f32>(vec3<f32>(textureLoad(materialPropertiesTexture, pixelI, 0).x), 1.0);
    } else if (uniforms.mode == DEBUG_METALLIC) {
        color = vec4<f32>(vec3<f32>(textureLoad(materialPropertiesTexture, pixelI, 0).y), 1.0);
    } else if (uniforms.mode == DEBUG_EMISSIVE) {
        let emission = max(textureLoad(emissiveTexture, pixelI, 0).rgb, vec3<f32>(0.0));
        color = vec4<f32>(emission / (vec3<f32>(1.0) + emission), 1.0);
    } else if (uniforms.mode == DEBUG_MOTION_VECTOR) {
        color = vec4<f32>(textureLoad(motionVectorTexture, pixelI, 0).xy + vec2<f32>(0.5), 0.5, 1.0);
    } else if (uniforms.mode == DEBUG_HARDWARE_DEPTH) {
        color = vec4<f32>(vec3<f32>(textureLoad(depthTexture, pixelI, 0)), 1.0);
    } else if (uniforms.mode == DEBUG_LINEAR_DEPTH) {
        color = vec4<f32>(vec3<f32>(clamp(linearDepth[pixelIndex] * uniforms.linearDepthScale, 0.0, 1.0)), 1.0);
    } else if (uniforms.mode == DEBUG_COVERAGE) {
        color = vec4<f32>(vec3<f32>(select(0.0, 1.0, coverage[pixelIndex] != 0u)), 1.0);
    } else if (uniforms.mode == DEBUG_INSTANCE_ID) {
        color = vec4<f32>(hashId(textureLoad(instanceIdTexture, pixelI, 0).x), 1.0);
    } else if (uniforms.mode == DEBUG_VOXEL_ID) {
        color = vec4<f32>(hashId(textureLoad(voxelIdTexture, pixelI, 0).x), 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_HANDLE ||
               uniforms.mode == DEBUG_MATERIAL_PROGRAM) {
        let payload = select(
            0u, microPayloads[pixelIndex],
            pixelIndex < arrayLength(&microPayloads));
        let descriptorId = payload & 0xffffu;
        let debugId = select(
            descriptorId, descriptorId ^ 0xa511e9b3u,
            uniforms.mode == DEBUG_MATERIAL_PROGRAM);
        color = vec4<f32>(select(
            vec3<f32>(0.02), hashId(debugId),
            (payload & MICRO_PAYLOAD_VALID) != 0u), 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_CONTEXT) {
        let context = clamp(
            textureLoad(materialPropertiesTexture, pixelI, 0).w / 255.0,
            0.0, 1.0);
        color = vec4<f32>(context, 1.0 - context, 0.25, 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_BASE_HEIGHT) {
        let payload = select(
            0u, microPayloads[pixelIndex],
            pixelIndex < arrayLength(&microPayloads));
        let depth = f32((payload >> 24u) & 0x0fu) / 15.0;
        color = vec4<f32>(vec3<f32>(1.0 - depth), 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_MODIFIER) {
        let payload = select(
            0u, microPayloads[pixelIndex],
            pixelIndex < arrayLength(&microPayloads));
        let depth = f32((payload >> 24u) & 0x0fu) / 15.0;
        color = vec4<f32>(depth, 0.1, 1.0 - depth, 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_FINAL_OCCUPANCY) {
        let occupied = coverage[pixelIndex] != 0u;
        color = vec4<f32>(select(
            vec3<f32>(0.02), vec3<f32>(0.2, 1.0, 0.25), occupied), 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_LAYER_WEIGHT) {
        let weight = clamp(
            textureLoad(materialPropertiesTexture, pixelI, 0).z, 0.0, 1.0);
        color = vec4<f32>(weight, weight * weight, 1.0 - weight, 1.0);
    } else if (uniforms.mode == DEBUG_MATERIAL_EXACT_FALLBACK) {
        let payload = select(
            0u, microPayloads[pixelIndex],
            pixelIndex < arrayLength(&microPayloads));
        let valid = (payload & MICRO_PAYLOAD_VALID) != 0u;
        let fallback = (payload & MICRO_PAYLOAD_FALLBACK) != 0u;
        color = vec4<f32>(select(
            vec3<f32>(0.02),
            select(vec3<f32>(0.15, 1.0, 0.25),
                   vec3<f32>(1.0, 0.15, 0.75), fallback),
            valid), 1.0);
    } else if (uniforms.mode >= DEBUG_TERRAIN_CHUNK_OWNER &&
               uniforms.mode <= DEBUG_TERRAIN_BRICK_LOD) {
        let mappingId = select(
            0u, terrainOwnershipIds[pixelIndex],
            pixelIndex < arrayLength(&terrainOwnershipIds));
        let owner = terrainOwnership(mappingId);
        if (uniforms.mode == DEBUG_TERRAIN_BRICK_LOD) {
            let payload = select(
                0u, microPayloads[pixelIndex],
                pixelIndex < arrayLength(&microPayloads));
            if ((payload & MICRO_PAYLOAD_VALID) != 0u) {
                color = vec4<f32>(1.0, 0.1, 0.75, 1.0);
            } else {
                let localLevel =
                    (payload >> BRICK_LOD_DEBUG_LOCAL_SHIFT) &
                    BRICK_LOD_DEBUG_MASK;
                let clipmapLevel =
                    (payload >> BRICK_LOD_DEBUG_CLIPMAP_SHIFT) &
                    BRICK_LOD_DEBUG_MASK;
                color = vec4<f32>(hashId(
                    (localLevel * 0x9e3779b9u) ^
                    (clipmapLevel * 0x85ebca6bu)), 1.0);
            }
        } else if (owner.physicalSlot == TERRAIN_INVALID_SLOT) {
            color = vec4<f32>(1.0, 0.0, 1.0, 1.0);
        } else if (uniforms.mode == DEBUG_TERRAIN_CHUNK_OWNER) {
            color = vec4<f32>(hashId(mappingId), 1.0);
        } else if (uniforms.mode == DEBUG_TERRAIN_CHUNK_BOUNDARIES) {
            let base = hashId(mappingId) * 0.28;
            color = vec4<f32>(select(
                base, vec3<f32>(1.0),
                terrainChunkBoundary(pixel, mappingId)), 1.0);
        } else if (uniforms.mode == DEBUG_TERRAIN_CHUNK_LOD) {
            color = vec4<f32>(hashId(owner.spatialLOD * 0x9e3779b9u), 1.0);
        } else if (uniforms.mode == DEBUG_TERRAIN_CHUNK_SLOT) {
            color = vec4<f32>(hashId(owner.physicalSlot), 1.0);
        } else {
            color = vec4<f32>(
                hashId(owner.publicationGeneration * 0x85ebca6bu), 1.0);
        }
    } else if (uniforms.mode == DEBUG_MICRO_SURFACE) {
        let payload = select(
            0u, microPayloads[pixelIndex],
            pixelIndex < arrayLength(&microPayloads));
        if ((payload & MICRO_PAYLOAD_VALID) != 0u) {
            let descriptorTint = hashId(payload & 0xffffu);
            if ((payload & MICRO_PAYLOAD_FALLBACK) != 0u) {
                color = vec4<f32>(
                    mix(descriptorTint, vec3<f32>(1.0, 0.1, 0.75), 0.7),
                    1.0);
            } else {
                let coordinate = vec3<f32>(
                    f32((payload >> 16u) & 0x0fu),
                    f32((payload >> 20u) & 0x0fu),
                    f32((payload >> 24u) & 0x0fu)) / 7.0;
                color = vec4<f32>(
                    mix(descriptorTint, clamp(coordinate, vec3<f32>(0.0),
                        vec3<f32>(1.0)), 0.7),
                    1.0);
            }
        } else {
            color = vec4<f32>(0.025, 0.025, 0.025, 1.0);
        }
    } else if (uniforms.mode == DEBUG_OCCUPANCY_SHADOW &&
               uniforms.shadowWidth != 0u &&
               uniforms.shadowHeight != 0u) {
        let shadowPixel = min(
            pixel / 2u,
            vec2<u32>(uniforms.shadowWidth - 1u,
                      uniforms.shadowHeight - 1u));
        let shadowIndex =
            shadowPixel.y * uniforms.shadowWidth + shadowPixel.x;
        let blocked = occupancyShadowResults[shadowIndex].status ==
            DDA_STATUS_HIT;
        color = vec4<f32>(vec3<f32>(select(1.0, 0.0, blocked)), 1.0);
    } else if (uniforms.mode == DEBUG_OCCUPANCY_HIT_CELL &&
               uniforms.shadowWidth != 0u &&
               uniforms.shadowHeight != 0u) {
        let resultPixel = min(
            pixel / 2u,
            vec2<u32>(uniforms.shadowWidth - 1u,
                      uniforms.shadowHeight - 1u));
        let resultIndex =
            resultPixel.y * uniforms.shadowWidth + resultPixel.x;
        let result = occupancyShadowResults[resultIndex];
        if (result.status == DDA_STATUS_HIT) {
            color = vec4<f32>(
                hashId(occupancyCellId(result.globalCell)), 1.0);
        } else if (result.status == DDA_STATUS_QUERY_LIMIT) {
            color = vec4<f32>(1.0, 0.0, 0.0, 1.0);
        } else {
            color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        }
    }
    let analyticSkyEnabled = uniforms.skyHorizonIntensity.w > 0.0;
    // BaseColor is a geometry/material diagnostic, not a cheap shaded mode.
    // Multiplying covered voxels by analytic sky diffuse made axis-aligned
    // side faces nearly black and caused lighting changes to look like raster
    // cracks while inspecting albedo. Keep the sky only for uncovered pixels.
    if (coverage[pixelIndex] == 0u && uniforms.mode == DEBUG_BASE_COLOR &&
        analyticSkyEnabled) {
        color = vec4<f32>(debugSkyBackground(pixel), 1.0);
    } else if (coverage[pixelIndex] == 0u &&
        uniforms.mode != DEBUG_OCCUPANCY_HIT_CELL &&
        uniforms.mode != DEBUG_BRICK_OVERDRAW &&
        uniforms.mode != DEBUG_MICRO_SURFACE) {
        color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    // After the coverage blackout, deliberately: overdraw on pixels that ended
    // up empty is exactly the wasted work this view exists to show.
    if (uniforms.mode == DEBUG_BRICK_OVERDRAW) {
        let count = select(
            0u, brickOverdraw[pixelIndex],
            pixelIndex < arrayLength(&brickOverdraw));
        color = vec4<f32>(brickOverdrawColor(count), 1.0);
    }
    textureStore(outputTexture, pixelI, color);
}
