#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_sky_guide_common.wgsl"
#include "sparse_voxel_light_reachability.wgsl"
#include "sparse_voxel_local_light_hash.wgsl"

// Full-resolution lighting field.
//
// Lighting used to be accumulated in *ray slot* space at a reduced ray grid,
// and the shading pass reconstructed full resolution from it. That capped
// detail at the ray-grid resolution regardless of later filtering.
//
// Here the canonical ray domain and accumulator are per pixel instead. The
// rate setting schedules only a subset of those slots each frame. Unscheduled
// pixels keep depth/normal-compatible history; a fresh disocclusion receives a
// zero-confidence analytic prior until its own phase traces. Resolved
// visibility never crosses receivers. The field remains full resolution at
// every rate.

struct SparseVoxelLightFieldSettings {
    // xy: raster dimensions, zw: ray grid dimensions.
    dimensions : vec4<u32>,
    // x: history valid, y: maximum accumulated samples, z: frame index,
    // w: light count.
    controls : vec4<u32>,
    // x: normal threshold, y: relative depth threshold, z: spatial fill
    // radius in pixels, w: history normal floor.
    thresholds : vec4<f32>,
    // x: scene/light generation, y: reachability entry count.
    reachability : vec4<u32>,
    // x: motion threshold in pixels, y: relative bright-to-dark threshold,
    // z: maximum retained samples during a change, w: history clamp strength.
    antiLag : vec4<f32>,
    // x: tile size, y: wake duration, z: regional heartbeat enabled.
    regionStability : vec4<u32>,
    // x: soft-visibility distribution delta, y: consecutive deviant windows,
    // z: active-ray prefill stride, w: relative view-depth threshold for
    // terrain history whose physical page ownership changed. The rolling
    // window is fixed at 16 binary observations so every light fits in one
    // atomic word.
    regionSoftProbe : vec4<f32>,
    // xyz: legacy post-trace reconstruction controls, retained in the uniform
    // ABI but deliberately unused. w: reserved.
    spatialReuse : vec4<f32>,
    // x: firefly clamp in standard deviations, zero disables. yzw reserved.
    spatialReuse2 : vec4<f32>,
    // x: nonzero while terrain-publication diagnostics are active. It must not
    // relax receiver validation: a publication can expose a different voxel
    // face even when its page still describes the same terrain region.
    commit : vec4<u32>,
}

@group(0) @binding(0) var<uniform> settings : SparseVoxelLightFieldSettings;
@group(0) @binding(1) var<storage, read> coverage : array<u32>;
@group(0) @binding(2) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(3) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(4) var shadingNormalTexture : texture_2d<f32>;
@group(0) @binding(5) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(6) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(7) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(8) var<storage, read> sceneRays : array<SparseVoxelSceneRay>;
@group(0) @binding(9) var<storage, read> sceneDIReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(13) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(14) var historyFieldTexture : texture_2d<f32>;
@group(0) @binding(15) var historyIdTexture : texture_2d<u32>;
@group(0) @binding(16) var outputFieldTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(17) var outputIdTexture : texture_storage_2d<rg32uint, write>;
@group(0) @binding(18) var<storage, read> currentHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(19) var reactiveOutput : texture_storage_2d<r16float, write>;
@group(0) @binding(20) var<storage, read_write> lightReachability : array<atomic<u32>>;
// Per guide bin: observation count followed by quantized visible sky
// luminance. A tiny tile pass turns this into next frame's sampling CDF.
@group(0) @binding(21) var<storage, read_write> skyGuideFeedback : array<atomic<u32>>;
@group(0) @binding(22) var historyMomentsTexture : texture_2d<f32>;
@group(0) @binding(23) var outputMomentsTexture : texture_storage_2d<rg16float, write>;
@group(0) @binding(24) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(25) var historyLightHashTexture : texture_2d<u32>;
@group(0) @binding(26) var outputLightHashTexture : texture_storage_2d<r32uint, write>;
@group(0) @binding(27) var<storage, read_write> regionStability :
    array<atomic<u32>>;
// Adaptive tile-budget records. This pass closes the loop for the two counters
// only it can observe: how many of a tile's scheduled rays came back resolved,
// and how many sentinels reported a changed occluder.
@group(0) @binding(28) var<storage, read_write> rayTileBudget :
    array<atomic<u32>>;

// Reconstruction coverage counters. Sparse ray schedules fill a full-resolution
// accumulator from a smaller ray grid, and when every candidate tap fails the
// identity/normal/depth test the pixel silently keeps history instead of
// getting a measurement. That failure is invisible in the image until it is
// large, and it is the thing that decides how far --lighting-scale can be
// pushed, so it is counted rather than inferred.
//   [0] covered pixels considered
//   [1] covered pixels that accepted no tap at all
//   [2] accepted taps, summed - divide by [0] for the mean
// History-rejection causes. A pixel that loses every history tap in one frame
// re-converges from scratch and that is what reads as a light pop while
// walking, so the cause is counted rather than inferred.
//   [3] pixels that attempted a history reprojection
//   [4] of those, pixels that recovered no history at all
//   [5] history taps rejected on face class (quantized normal)
//   [6] history taps rejected on the local light hash
//   [7] history taps accepted only at the reduced LOD weight
// Localized pop metric. A whole-screen mean cannot see a few thousand pixels
// changing brightness at once, which is the artifact this pass is judged on, so
// count the pixels directly: those that had settled history, kept their
// geometry, and still moved more than half a stop in one frame.
//   [8] pixels with settled history considered
//   [9] of those, pixels whose radiance jumped
// Confidence-truncation causes. Reprojection is not the only thing that can
// throw history away: a tile declared changed, and the anti-lag darkening
// response, both truncate a *validly reprojected* record. Under camera rotation
// these dominate, and they are invisible to the reprojection counters above.
//   [10] pixels truncated by a region visibility change
//   [11] pixels truncated by the anti-lag darkening response
//   [12] pixels whose surface identity is not exact this frame
//   [13] pixels whose confidence the surprise decay actually reduced
//   [14] history taps rejected because the instances differ and both are not terrain
//   [15] history taps rejected on the view-depth tolerance
//   [16] history taps skipped because the history texel held no record
//   [17] inexact taps outside depth tolerance on a non-commit frame
//   [18] commit-frame taps outside depth tolerance that were not both terrain
//   [19] commit-frame terrain taps settled at reduced confidence
// Variance-normalized localized pop metric. This is stricter than [8..9]: both
// depth and normal must agree, and the luminance delta must exceed the pixel's
// own accumulated standard deviation rather than a fixed relative threshold.
//   [20] stable-geometry pixels with settled moments considered
//   [21] of those, pixels whose radiance changed by more than three sigma
//   [22] [20] subset measured during a terrain commit window
//   [23] [21] subset measured during a terrain commit window
@group(0) @binding(29) var<storage, read_write> reconstructionStatistics :
    array<atomic<u32>>;
@group(0) @binding(30) var historyViewDepthTexture : texture_2d<f32>;
@group(0) @binding(31) var outputViewDepthTexture :
    texture_storage_2d<r32float, write>;
@group(0) @binding(32) var<storage, read> terrainOwnershipIds : array<u32>;
// Previous frame's geometry normal, octahedral-encoded. History validation
// needs an *orientation* comparison, and the 3-bit face class it replaces was a
// hard six-way quantization of a continuous quantity: on any slope near a
// dominant-axis tie the class flips between frames while the surface has not
// moved, and the tap was rejected outright. Terrain is where that hurts, and it
// gets worse the more relief the world has - measured 0.84% of taps rejected on
// face class on a 267 m-relief footprint against 1.52% on a 1565 m one.
@group(0) @binding(33) var historyNormalTexture : texture_2d<f32>;
@group(0) @binding(34) var outputNormalTexture :
    texture_storage_2d<rgba16float, write>;

fn light_field_stat_enabled() -> bool {
    return arrayLength(&reconstructionStatistics) >= 3u;
}

fn light_field_cause_stat_enabled() -> bool {
    return arrayLength(&reconstructionStatistics) >= 8u;
}

fn light_field_cause_add(slot : u32, value : u32) {
    if (!light_field_cause_stat_enabled() || value == 0u) { return; }
    atomicAdd(&reconstructionStatistics[slot], value);
}

fn light_field_pop_stat_enabled() -> bool {
    return arrayLength(&reconstructionStatistics) >= 10u;
}

fn light_field_variance_pop_stat_enabled() -> bool {
    return arrayLength(&reconstructionStatistics) >= 24u;
}

// Legacy gather-gate breakdown, slots 24-27. Post-trace gather is gone; slot 27
// now records scheduled receivers whose own trace produced no resolved sample.
fn light_field_gate_stat_enabled() -> bool {
    return arrayLength(&reconstructionStatistics) >= 28u;
}

fn light_field_gate_add(slot : u32, value : u32) {
    if (!light_field_gate_stat_enabled() || value == 0u) { return; }
    atomicAdd(&reconstructionStatistics[slot], value);
}

fn light_field_truncation_add(slot : u32) {
    if (arrayLength(&reconstructionStatistics) < 20u) { return; }
    atomicAdd(&reconstructionStatistics[slot], 1u);
}

const LIGHT_FIELD_POP_THRESHOLD : f32 = 0.5;
const LIGHT_FIELD_POP_SETTLED_SAMPLES : f32 = 8.0;
const LIGHT_FIELD_POP_NORMAL_ALIGNMENT : f32 = 0.95;
const LIGHT_FIELD_POP_SIGMA_THRESHOLD : f32 = 3.0;
const LIGHT_FIELD_POP_MINIMUM_SIGMA : f32 = 1.0e-3;

struct LightFieldRawHistory {
    luminance : f32,
    sampleCount : f32,
    secondMoment : f32,
    normal : vec3<f32>,
}

// The previous frame's published radiance at the reprojected footprint, with no
// validation of any kind.
//
// Deliberately unvalidated. The artifact is "the light at this place on screen
// changed between frames", and every validated read is blind to it by
// construction: a pixel that loses its history has a *zero* validated history
// count, so gating the measurement on that count excludes exactly the pixels
// being measured. A first attempt did that and reported 0.0000% on both the
// broken and the fixed build.
fn light_field_raw_history(historyUv : vec2<f32>) -> LightFieldRawHistory {
    let dimensions = light_field_raster_dimensions();
    let position = historyUv * vec2<f32>(dimensions) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(position));
    let fraction = fract(position);
    var luminanceSum = 0.0;
    var countSum = 0.0;
    var secondMomentSum = 0.0;
    var normalSum = vec3<f32>(0.0);
    var weightSum = 0.0;
    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let tap = base + vec2<i32>(x, y);
            // Off-screen history does not exist. Clamping duplicated the edge
            // texel into the missing part of a rotating footprint and produced
            // a bright/dark strip that followed the viewport boundary.
            if (any(tap < vec2<i32>(0)) ||
                any(tap >= vec2<i32>(dimensions))) { continue; }
            let record = textureLoad(historyFieldTexture, tap, 0);
            if (record.a <= 0.0) { continue; }
            let weight =
                select(1.0 - fraction.x, fraction.x, x == 1) *
                select(1.0 - fraction.y, fraction.y, y == 1);
            luminanceSum = luminanceSum +
                dot(max(record.rgb, vec3<f32>(0.0)),
                    vec3<f32>(0.2126, 0.7152, 0.0722)) * weight;
            countSum = countSum + record.a * weight;
            secondMomentSum = secondMomentSum +
                max(textureLoad(historyMomentsTexture, tap, 0).x, 0.0) *
                weight;
            normalSum = normalSum + light_field_decode_normal(
                textureLoad(historyNormalTexture, tap, 0)) * weight;
            weightSum = weightSum + weight;
        }
    }
    if (weightSum <= 1.0e-5) {
        return LightFieldRawHistory(
            0.0, 0.0, 0.0, vec3<f32>(0.0));
    }
    return LightFieldRawHistory(
        luminanceSum / weightSum,
        countSum / weightSum,
        secondMomentSum / weightSum,
        light_field_normalize(normalSum, vec3<f32>(0.0)));
}

// A pixel pops when the surface in front of it did not move but its brightness
// did. Depth agreement is what separates that from a genuine disocclusion,
// where the light is *supposed* to change.
fn light_field_record_pop(
    rawHistory : LightFieldRawHistory, value : vec3<f32>,
    historyViewDepth : f32, expectedHistoryViewDepth : f32,
    currentNormal : vec3<f32>) {
    if (!light_field_pop_stat_enabled() ||
        rawHistory.sampleCount < LIGHT_FIELD_POP_SETTLED_SAMPLES ||
        !(historyViewDepth > 0.0) || !(expectedHistoryViewDepth > 0.0)) {
        return;
    }
    let depthScale = max(abs(expectedHistoryViewDepth), 1.0);
    if (abs(historyViewDepth - expectedHistoryViewDepth) / depthScale > 0.1) {
        return;
    }
    atomicAdd(&reconstructionStatistics[8u], 1u);
    let after = dot(max(value, vec3<f32>(0.0)),
        vec3<f32>(0.2126, 0.7152, 0.0722));
    let relative =
        abs(after - rawHistory.luminance) /
        max(max(rawHistory.luminance, after), 1.0e-3);
    if (relative > LIGHT_FIELD_POP_THRESHOLD) {
        atomicAdd(&reconstructionStatistics[9u], 1u);
    }

    if (!light_field_variance_pop_stat_enabled() ||
        dot(rawHistory.normal, currentNormal) <
            LIGHT_FIELD_POP_NORMAL_ALIGNMENT) {
        return;
    }
    atomicAdd(&reconstructionStatistics[20u], 1u);
    let variance = max(
        rawHistory.secondMoment -
            rawHistory.luminance * rawHistory.luminance,
        0.0);
    let standardDeviation = max(
        sqrt(variance), LIGHT_FIELD_POP_MINIMUM_SIGMA);
    let variancePop =
        abs(after - rawHistory.luminance) >
        LIGHT_FIELD_POP_SIGMA_THRESHOLD * standardDeviation;
    if (variancePop) {
        atomicAdd(&reconstructionStatistics[21u], 1u);
    }
    if (settings.commit.x != 0u) {
        atomicAdd(&reconstructionStatistics[22u], 1u);
        if (variancePop) {
            atomicAdd(&reconstructionStatistics[23u], 1u);
        }
    }
}

fn light_field_stat_add(slot : u32, value : u32) {
    if (!light_field_stat_enabled() || value == 0u) { return; }
    atomicAdd(&reconstructionStatistics[slot], value);
}


const LIGHT_FIELD_INV_PI : f32 = 0.31830988618;

fn light_field_raster_dimensions() -> vec2<u32> {
    return max(settings.dimensions.xy, vec2<u32>(1u));
}

fn light_field_ray_dimensions() -> vec2<u32> {
    return max(settings.dimensions.zw, vec2<u32>(1u));
}

fn light_field_rays_per_pixel() -> u32 {
    return max(settings.reachability.z, 1u);
}

fn light_field_uses_independent_rays() -> bool {
    return settings.reachability.w != 0u;
}

fn light_field_ray_group_base(rayPixel : vec2<u32>) -> u32 {
    let rayDimensions = light_field_ray_dimensions();
    return (rayPixel.y * rayDimensions.x + rayPixel.x) *
        light_field_rays_per_pixel();
}

fn light_field_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let lengthValue = length(value);
    return select(fallback, value / max(lengthValue, 1.0e-6), lengthValue > 1.0e-6);
}

fn light_field_clip_to_uv(clip : vec4<f32>) -> vec2<f32> {
    let ndc = clip.xy / clip.w;
    return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

fn light_field_object_motion_pixels(
    surfacePosition : vec3<f32>, motion : vec2<f32>,
    dimensions : vec2<u32>) -> f32 {
    let currentClip = raster.worldToClip * vec4<f32>(surfacePosition, 1.0);
    let previousClip =
        raster.previousWorldToClip * vec4<f32>(surfacePosition, 1.0);
    if (abs(currentClip.w) <= 1.0e-6 ||
        abs(previousClip.w) <= 1.0e-6) {
        return length(motion * vec2<f32>(dimensions));
    }
    let cameraMotion =
        light_field_clip_to_uv(currentClip) -
        light_field_clip_to_uv(previousClip);
    return length((motion - cameraMotion) * vec2<f32>(dimensions));
}

// Previous-frame normal, with an explicit validity channel.
//
// The validity channel is the point. A consumer that does not wire this texture
// - several test harnesses pass an empty handle and get the pass's own
// never-written fallback - reads zeros, and a packed encoding cannot tell that
// from a legitimate normal. Encoded octahedrally, zero decodes to +Z, so every
// tap on a surface facing any other direction was rejected and the pass lost
// all of its history. w <= 0 means "no orientation recorded", and the caller
// then falls back to the tests it used before this existed.
fn light_field_decode_normal(encoded : vec4<f32>) -> vec3<f32> {
    if (!(encoded.w > 0.0)) { return vec3<f32>(0.0); }
    return light_field_normalize(encoded.xyz, vec3<f32>(0.0));
}

// Which face of the voxel this pixel sees: dominant axis of the geometry
// normal, plus its sign. Three bits.
fn light_field_face_class(pixel : vec2<i32>) -> u32 {
    let normal = textureLoad(geometryNormalTexture, pixel, 0).xyz;
    let magnitude = abs(normal);
    var axis = 0u;
    if (magnitude.y > magnitude.x) { axis = 1u; }
    if (magnitude.z > max(magnitude.x, magnitude.y)) { axis = 2u; }
    return axis | select(0u, 4u, normal[axis] < 0.0);
}

// Surface identity for history validation.
//
// The face class is folded into the spare high bits of the instance channel
// because instance and voxel alone do not identify a *surface*. A shell one
// voxel thick presents the same voxel from both sides, so walking the camera
// through it keeps the same identity while the visible face flips - and the
// history test, which is exact equality and nothing else, accepts the outside's
// lit history on the inside wall. It then decays over the accumulation window
// rather than being rejected outright, which reads as light leaking through for
// a few frames.
//
// SVGF does not have this problem because it also compares normals. The light
// field has no history normal to compare against, so the face travels inside
// the identity instead - same cost, same texture, no extra binding.
const LIGHT_FIELD_FACE_SHIFT : u32 = 29u;
const LIGHT_FIELD_TERRAIN_BIT : u32 = 1u << 28u;
const LIGHT_FIELD_INSTANCE_MASK : u32 = LIGHT_FIELD_TERRAIN_BIT - 1u;

fn light_field_identity(pixel : vec2<i32>) -> vec2<u32> {
    let instance = textureLoad(instanceIdTexture, pixel, 0).x;
    let dimensions = light_field_raster_dimensions();
    let pixelIndex = u32(pixel.y) * dimensions.x + u32(pixel.x);
    let terrain = pixelIndex < arrayLength(&terrainOwnershipIds) &&
        terrainOwnershipIds[pixelIndex] != 0u;
    return vec2<u32>(
        (instance & LIGHT_FIELD_INSTANCE_MASK) |
            select(0u, LIGHT_FIELD_TERRAIN_BIT, terrain) |
            (light_field_face_class(pixel) << LIGHT_FIELD_FACE_SHIFT),
        textureLoad(voxelIdTexture, pixel, 0).x);
}

// Reprojection predicts the previous-frame view depth of this exact
// world-space receiver. Normal and depth are the complete temporal validity
// rule; voxel, instance/page, face-class, and local-light hashes do not
// participate.
fn light_field_depth_tolerance(viewDepth : f32) -> f32 {
    let relative = max(settings.regionSoftProbe.w, 1.0e-5);
    // Reprojection already predicts the previous-frame view depth of this
    // exact world-space receiver. A grazing-angle expansion therefore admits
    // unrelated parallel surfaces rather than compensating a projection
    // error. Keep a purely relative depth gate with only a numerical floor.
    let depthScale = max(
        abs(viewDepth), max(raster.cameraWorldPositionNear.w, 1.0e-3));
    return relative * depthScale;
}

fn light_field_history_geometry_weight(
    historyNormal : vec3<f32>, currentNormal : vec3<f32>,
    historyViewDepth : f32, expectedHistoryViewDepth : f32,
    depthTolerance : f32) -> f32 {
    // Binary gates are intentional. Fractional geometry weights made the
    // effective history count pulse as a reprojected footprint moved across a
    // voxel edge, turning stationary Monte Carlo noise into crawling noise.
    if (dot(historyNormal, historyNormal) <= 0.0) { return 0.0; }
    let normalFloor = clamp(settings.thresholds.w, -0.99, 0.99);
    if (dot(historyNormal, currentNormal) < normalFloor) { return 0.0; }
    if (!(historyViewDepth > 0.0) || !(expectedHistoryViewDepth > 0.0)) {
        return 0.0;
    }
    if (abs(historyViewDepth - expectedHistoryViewDepth) > depthTolerance) {
        return 0.0;
    }
    return 1.0;
}

fn light_field_reproject_history(
    historyUv : vec2<f32>, expectedHistoryViewDepth : f32,
    currentNormal : vec3<f32>, depthTolerance : f32) -> vec4<f32> {
    let dimensions = light_field_raster_dimensions();
    let historyPosition =
        historyUv * vec2<f32>(dimensions) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(historyPosition));
    let fraction = fract(historyPosition);
    var valueSum = vec3<f32>(0.0);
    var countSum = 0.0;
    var weightSum = 0.0;
    var geometryRejects = 0u;
    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let bilinearWeight =
                select(1.0 - fraction.x, fraction.x, x == 1) *
                select(1.0 - fraction.y, fraction.y, y == 1);
            let tap = base + vec2<i32>(x, y);
            if (any(tap < vec2<i32>(0)) ||
                any(tap >= vec2<i32>(dimensions))) { continue; }
            let tapNormal = light_field_decode_normal(
                textureLoad(historyNormalTexture, tap, 0));
            let geometryWeight = light_field_history_geometry_weight(
                tapNormal, currentNormal,
                textureLoad(historyViewDepthTexture, tap, 0).x,
                expectedHistoryViewDepth, depthTolerance);
            if (geometryWeight <= 0.0) {
                geometryRejects = geometryRejects + 1u;
                continue;
            }
            let record = textureLoad(historyFieldTexture, tap, 0);
            if (record.a <= 0.0) {
                light_field_truncation_add(16u);
                continue;
            }
            let tapWeight = geometryWeight * bilinearWeight;
            valueSum = valueSum + max(record.rgb, vec3<f32>(0.0)) * tapWeight;
            countSum = countSum + record.a * tapWeight;
            weightSum = weightSum + tapWeight;
        }
    }
    light_field_cause_add(3u, 1u);
    light_field_cause_add(5u, geometryRejects);
    if (weightSum <= 1.0e-5) {
        light_field_cause_add(4u, 1u);
        return vec4<f32>(0.0);
    }
    // Radiance is renormalised over the taps that agreed - a partial footprint
    // still measures this surface.
    //
    // Geometry acceptance is binary. Once at least one bilinear tap passes
    // normal and depth, preserve its complete history confidence; making the
    // count depend on the accepted footprint area caused it to breathe as the
    // camera moved across voxel boundaries.
    return vec4<f32>(
        valueSum / weightSum,
        min(countSum / weightSum, f32(max(settings.controls.y, 1u))));
}

// Reproject the variance/stability record with the exact same normal/depth
// footprint as radiance. Keeping this as a separate compact texture avoids
// sacrificing a lighting channel or weakening the 32-bit surface identity.
// x is E[luminance^2], y is consecutive stable direct updates.
fn light_field_reproject_moments(
    historyUv : vec2<f32>, expectedHistoryViewDepth : f32,
    currentNormal : vec3<f32>, depthTolerance : f32) -> vec2<f32> {
    let dimensions = light_field_raster_dimensions();
    let historyPosition =
        historyUv * vec2<f32>(dimensions) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(historyPosition));
    let fraction = fract(historyPosition);
    var momentsSum = vec2<f32>(0.0);
    var weightSum = 0.0;
    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let tap = base + vec2<i32>(x, y);
            if (any(tap < vec2<i32>(0)) ||
                any(tap >= vec2<i32>(dimensions))) { continue; }
            let geometryWeight = light_field_history_geometry_weight(
                light_field_decode_normal(
                    textureLoad(historyNormalTexture, tap, 0)),
                currentNormal,
                textureLoad(historyViewDepthTexture, tap, 0).x,
                expectedHistoryViewDepth, depthTolerance);
            if (geometryWeight <= 0.0 ||
                textureLoad(historyFieldTexture, tap, 0).a <= 0.0) {
                continue;
            }
            let tapWeight = geometryWeight *
                select(1.0 - fraction.x, fraction.x, x == 1) *
                select(1.0 - fraction.y, fraction.y, y == 1);
            momentsSum = momentsSum +
                max(textureLoad(historyMomentsTexture, tap, 0).xy,
                    vec2<f32>(0.0)) * tapWeight;
            weightSum = weightSum + tapWeight;
        }
    }
    return select(
        vec2<f32>(0.0), momentsSum / max(weightSum, 1.0e-5),
        weightSum > 1.0e-5);
}

fn light_field_update_reachability_stat(
    index : u32, visible : bool) {
    for (var attempt = 0u; attempt < 8u; attempt = attempt + 1u) {
        let old = atomicLoad(&lightReachability[index]);
        var total = old & 0xffffu;
        var visibleCount = old >> 16u;
        if (total >= 1024u) {
            total = total >> 1u;
            visibleCount = visibleCount >> 1u;
        }
        let updated = min(total + 1u, 0xffffu) |
            (min(visibleCount + select(0u, 1u, visible), 0xffffu) << 16u);
        if (atomicCompareExchangeWeak(
                &lightReachability[index], old, updated).exchanged) {
            return;
        }
    }
}

fn light_field_record_reachability(pixelIndex : u32) {
    // Updating every pixel would make large planar regions serialize on a
    // handful of atomics. One deterministic sample per 16 scheduled pixels
    // learns the same regional probability within a frame or two.
    let feedbackHash = sparse_voxel_reachability_mix(
        pixelIndex ^ settings.controls.z * 0x9e3779b9u);
    let dimensions = light_field_raster_dimensions();
    let pixel = vec2<u32>(pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    let rayIndex = light_field_owning_slot(pixel);
    if ((feedbackHash & 15u) != 0u ||
        rayIndex >= arrayLength(&sceneRays) ||
        rayIndex >= arrayLength(&currentHits)) {
        return;
    }
    let ray = sceneRays[rayIndex];
    // A reduced ray slot represents one full-resolution source pixel. Every
    // pixel in its reconstruction block maps to the same slot, but feedback
    // must be emitted once and attributed to that source pixel's tile.
    if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u ||
        any(ray.pixelRayLight.xy != pixel)) {
        return;
    }
    let lightIndex = ray.pixelRayLight.w;
    let lightCapacity = max(settings.controls.w, 1u);
    let entryCount = settings.reachability.y;
    let stride = lightCapacity + 2u;
    if (lightIndex >= lightCapacity || entryCount == 0u ||
        arrayLength(&lightReachability) < entryCount * stride) {
        return;
    }
    let status = currentHits[rayIndex].status;
    let visible =
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE;
    if (!visible && status != SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        return;
    }
    let key = ray.seedFlags.w;
    if (key == 0u) { return; }
    let generation = sparse_voxel_reachability_generation(
        settings.reachability.x);
    for (var probe = 0u;
         probe < SPARSE_VOXEL_LIGHT_REACHABILITY_PROBES;
         probe = probe + 1u) {
        let entry = sparse_voxel_reachability_probe(key, probe, entryCount);
        let base = entry * stride;
        let storedGeneration = atomicLoad(&lightReachability[base]);
        if (storedGeneration == generation) {
            if (atomicLoad(&lightReachability[base + 1u]) == key) {
                light_field_update_reachability_stat(
                    base + 2u + lightIndex, visible);
                return;
            }
            continue;
        }
        if (storedGeneration == SPARSE_VOXEL_LIGHT_REACHABILITY_LOCKED) {
            continue;
        }
        let claim = atomicCompareExchangeWeak(
            &lightReachability[base], storedGeneration,
            SPARSE_VOXEL_LIGHT_REACHABILITY_LOCKED);
        if (!claim.exchanged) { continue; }
        atomicStore(&lightReachability[base + 1u], key);
        for (var clearLight = 0u; clearLight < lightCapacity;
             clearLight = clearLight + 1u) {
            atomicStore(&lightReachability[base + 2u + clearLight], 0u);
        }
        atomicStore(&lightReachability[base], generation);
        light_field_update_reachability_stat(
            base + 2u + lightIndex, visible);
        return;
    }
}

const LIGHT_FIELD_SOFT_HISTORY_MASK : u32 = 0xffffu;
const LIGHT_FIELD_SOFT_COUNT_SHIFT : u32 = 16u;
const LIGHT_FIELD_SOFT_REFERENCE_SHIFT : u32 = 21u;
const LIGHT_FIELD_SOFT_STREAK_SHIFT : u32 = 26u;
const LIGHT_FIELD_SOFT_SIGNATURE_SHIFT : u32 = 29u;
const LIGHT_FIELD_SOFT_FIELD_MASK : u32 = 0x1fu;
const LIGHT_FIELD_SOFT_STREAK_MASK : u32 = 0x7u;
const LIGHT_FIELD_SOFT_WINDOW : u32 = 16u;

fn light_field_region_state_base(pixel : vec2<u32>) -> u32 {
    let tileSize = max(settings.regionStability.x, 1u);
    let dimensions = light_field_raster_dimensions();
    let tileDimensions =
        (dimensions + vec2<u32>(tileSize - 1u)) / tileSize;
    let tile = pixel / tileSize;
    return (tile.y * tileDimensions.x + tile.x) *
        SPARSE_VOXEL_REGION_STABILITY_WORDS_PER_TILE;
}

fn light_field_region_is_awake(pixel : vec2<u32>) -> bool {
    if (settings.regionStability.z == 0u) { return false; }
    let base = light_field_region_state_base(pixel);
    return base + 2u < arrayLength(&regionStability) &&
        atomicLoad(&regionStability[base + 2u]) > settings.controls.z;
}

fn light_field_ray_budget_bound() -> bool {
    return settings.regionStability.w != 0u &&
        arrayLength(&rayTileBudget) > SPARSE_VOXEL_RAY_BUDGET_HEADER_WORDS;
}

fn light_field_ray_budget_tile_base(pixel : vec2<u32>) -> u32 {
    let tileSize = max(settings.regionStability.x, 1u);
    let dimensions = light_field_raster_dimensions();
    let tileDimensions =
        (dimensions + vec2<u32>(tileSize - 1u)) / tileSize;
    let tile = pixel / tileSize;
    return SPARSE_VOXEL_RAY_BUDGET_HEADER_WORDS +
        (tile.y * tileDimensions.x + tile.x) *
        SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE;
}

// Close the budget loop. The scheduler knows how many rays it handed a tile;
// only this pass knows how many of them produced a usable observation and
// whether a sentinel found the occluder state changed. Both counters are
// cleared by the next frame's scheduler, so they always describe one frame.
fn light_field_record_ray_budget(
    pixel : vec2<u32>, resolved : u32, visibilityChanged : bool) {
    if ((resolved == 0u && !visibilityChanged) ||
        !light_field_ray_budget_bound()) { return; }
    let base = light_field_ray_budget_tile_base(pixel);
    if (base + SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE >
        arrayLength(&rayTileBudget)) { return; }
    // The scheduler stamped this frame's bank; accumulating into the same one
    // keeps a readback of that bank internally consistent.
    let bank = (settings.controls.z & 1u) *
        SPARSE_VOXEL_RAY_BUDGET_BANK_WORDS;
    if (resolved != 0u) {
        atomicAdd(
            &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_RESOLVED],
            resolved);
        atomicAdd(
            &rayTileBudget[bank +
                SPARSE_VOXEL_RAY_BUDGET_STAT_RESOLVED_RAYS],
            resolved);
    }
    if (visibilityChanged) {
        atomicAdd(
            &rayTileBudget[base +
                SPARSE_VOXEL_RAY_BUDGET_TILE_VISIBILITY_CHANGES], 1u);
        atomicAdd(
            &rayTileBudget[bank +
                SPARSE_VOXEL_RAY_BUDGET_STAT_VISIBILITY_CHANGES], 1u);
    }
}

// Identifies both the light and the coarse surface the window is describing.
//
// The window below is one 16-observation record per screen tile, but the pixels
// feeding it are chosen by a hash stride that rotates with the frame index, so
// on a tile straddling two surfaces - a wall and the floor in front of it, say -
// it collects samples from both in a proportion that changes every frame. Sky
// visibility genuinely differs between those surfaces, so the window deviates
// from its reference for no reason other than which pixels it happened to
// sample, declares a change, and hard-resets the tile's sentinel pixel. That is
// one wrongly reset pixel per tile, which the a-trous then spreads into a blob,
// at regular tile spacing.
//
// The instance is deliberately included but the exact voxel id is not. A
// rotating tile heartbeat almost never visits the same exact voxel twice, so
// including it continuously reset the 16-observation window before it could
// validate a real change. The distribution may span several cells of one
// object; its hysteresis is specifically what makes that safe.
fn light_field_soft_probe_signature(
    light : WorldGILight, identity : vec2<u32>) -> u32 {
    return sparse_voxel_local_light_hash_mix(
        light.flags.z ^ (light.flags.w * 0xc2b2ae35u) ^
        (light.flags.y * 0x85ebca6bu) ^
        (identity.x * 0x9e3779b9u)) &
        LIGHT_FIELD_SOFT_STREAK_MASK;
}

fn light_field_pack_soft_probe(
    observations : u32, count : u32, reference : u32,
    streak : u32, signature : u32) -> u32 {
    return (observations & LIGHT_FIELD_SOFT_HISTORY_MASK) |
        ((count & LIGHT_FIELD_SOFT_FIELD_MASK) <<
            LIGHT_FIELD_SOFT_COUNT_SHIFT) |
        ((reference & LIGHT_FIELD_SOFT_FIELD_MASK) <<
            LIGHT_FIELD_SOFT_REFERENCE_SHIFT) |
        ((streak & LIGHT_FIELD_SOFT_STREAK_MASK) <<
            LIGHT_FIELD_SOFT_STREAK_SHIFT) |
        ((signature & LIGHT_FIELD_SOFT_STREAK_MASK) <<
            LIGHT_FIELD_SOFT_SIGNATURE_SHIFT);
}

// Sphere, planar, and sky samples do not have deterministic binary outcomes:
// the emitter point/direction and the receiver within a tile both rotate. Keep
// a 16-observation visibility window and compare it with a persistent reference
// distribution. Requiring consecutive deviant windows makes a real occluder
// transition wake the tile without turning ordinary soft-shadow variance into
// perpetual work.
fn light_field_update_soft_region_visibility(
    stateIndex : u32, light : WorldGILight, identity : vec2<u32>,
    visible : bool, reseed : bool) -> bool {
    let signature = light_field_soft_probe_signature(light, identity);
    let threshold = max(u32(ceil(clamp(
        settings.regionSoftProbe.x, 0.125, 0.75) *
        f32(LIGHT_FIELD_SOFT_WINDOW))), 2u);
    let requiredStreak = clamp(
        u32(round(settings.regionSoftProbe.y)), 1u,
        LIGHT_FIELD_SOFT_STREAK_MASK);
    for (var attempt = 0u; attempt < 8u; attempt = attempt + 1u) {
        let old = atomicLoad(&regionStability[stateIndex]);
        var oldCount = (old >> LIGHT_FIELD_SOFT_COUNT_SHIFT) &
            LIGHT_FIELD_SOFT_FIELD_MASK;
        let oldSignature = (old >> LIGHT_FIELD_SOFT_SIGNATURE_SHIFT) &
            LIGHT_FIELD_SOFT_STREAK_MASK;
        var observations = old & LIGHT_FIELD_SOFT_HISTORY_MASK;
        var reference = (old >> LIGHT_FIELD_SOFT_REFERENCE_SHIFT) &
            LIGHT_FIELD_SOFT_FIELD_MASK;
        var streak = (old >> LIGHT_FIELD_SOFT_STREAK_SHIFT) &
            LIGHT_FIELD_SOFT_STREAK_MASK;
        if (reseed || (oldCount != 0u && oldSignature != signature)) {
            oldCount = 0u;
            observations = 0u;
            reference = 0u;
            streak = 0u;
        }

        observations = ((observations << 1u) |
            select(0u, 1u, visible)) & LIGHT_FIELD_SOFT_HISTORY_MASK;
        let count = min(oldCount + 1u, LIGHT_FIELD_SOFT_WINDOW);
        let recentVisible = countOneBits(observations);
        var changed = false;
        if (count == LIGHT_FIELD_SOFT_WINDOW) {
            if (oldCount < LIGHT_FIELD_SOFT_WINDOW) {
                // The first complete window establishes the stationary
                // distribution; it is evidence, not a change event.
                reference = recentVisible;
                streak = 0u;
            } else {
                let delta = select(
                    reference - recentVisible,
                    recentVisible - reference,
                    recentVisible >= reference);
                if (delta >= threshold) {
                    streak = min(streak + 1u,
                        LIGHT_FIELD_SOFT_STREAK_MASK);
                } else {
                    streak = 0u;
                    // Track slow Monte Carlo drift, but do not absorb a
                    // developing step change before hysteresis can see it.
                    if (delta <= 1u) { reference = recentVisible; }
                }
                changed = streak >= requiredStreak;
                if (changed) {
                    reference = recentVisible;
                    streak = 0u;
                }
            }
        }
        let updated = light_field_pack_soft_probe(
            observations, count, reference, streak, signature);
        if (atomicCompareExchangeWeak(
                &regionStability[stateIndex], old, updated).exchanged) {
            return changed;
        }
    }
    return false;
}

const LIGHT_FIELD_REGION_EPOCH_MASK : u32 = 0x7fffffffu;
const LIGHT_FIELD_REGION_DISOCCLUSION_BIT : u32 = 0x80000000u;

fn light_field_invalidate_region(pixel : vec2<u32>) -> bool {
    if (settings.regionStability.z == 0u) { return false; }
    let base = light_field_region_state_base(pixel);
    if (base + SPARSE_VOXEL_REGION_STABILITY_WORDS_PER_TILE >
        arrayLength(&regionStability)) { return false; }
    let marker = ((settings.controls.z + 1u) &
        LIGHT_FIELD_REGION_EPOCH_MASK) |
        LIGHT_FIELD_REGION_DISOCCLUSION_BIT;
    if (atomicExchange(&regionStability[base + 3u], marker) == marker) {
        return false;
    }
    atomicStore(&regionStability[base], 0u);
    atomicStore(&regionStability[base + 1u], 0u);
    atomicMax(
        &regionStability[base + 2u],
        settings.controls.z + max(settings.regionStability.y, 1u) + 1u);
    for (var lightIndex = 0u;
         lightIndex < SPARSE_VOXEL_REGION_STABILITY_LIGHT_CAPACITY;
         lightIndex = lightIndex + 1u) {
        atomicStore(
            &regionStability[base +
                SPARSE_VOXEL_REGION_STABILITY_HEADER_WORDS + lightIndex],
            0u);
    }
    return true;
}

fn light_field_reseed_region_for_camera(base : u32) {
    let marker = (settings.controls.z + 1u) &
        LIGHT_FIELD_REGION_EPOCH_MASK;
    // At most one invocation clears a tile in a frame. The reserved header
    // word is a cheap epoch; an old observation racing with the clear can only
    // delay baseline warm-up, never create a false change event.
    let oldMarker = atomicLoad(&regionStability[base + 3u]);
    if ((oldMarker & LIGHT_FIELD_REGION_EPOCH_MASK) == marker ||
        !atomicCompareExchangeWeak(
            &regionStability[base + 3u], oldMarker, marker).exchanged) {
        return;
    }
    atomicStore(&regionStability[base], 0u);
    atomicStore(&regionStability[base + 1u], 0u);
    for (var lightIndex = 0u;
         lightIndex < SPARSE_VOXEL_REGION_STABILITY_LIGHT_CAPACITY;
         lightIndex = lightIndex + 1u) {
        atomicStore(
            &regionStability[base +
                SPARSE_VOXEL_REGION_STABILITY_HEADER_WORDS + lightIndex],
            0u);
    }
}

// Sentinel rays are the low-rate heartbeat for otherwise sleeping tiles. All
// light types use the rolling distribution above. Treating point/directional
// lights as one immediate bit per tile compared different receivers at a
// static shadow boundary, repeatedly declared a change, and produced the
// regular tile-shaped artifacts this mechanism was meant to remove. A sparse
// subset of ordinary active rays prefills the baseline before a region sleeps.
fn light_field_record_region_visibility(
    pixel : vec2<u32>, pixelIndex : u32, identity : vec2<u32>,
    cameraOnlyMotion : bool) -> bool {
    if (settings.regionStability.z == 0u) { return false; }
    let rayIndex = light_field_owning_slot(pixel);
    if (rayIndex >= arrayLength(&sceneRays) ||
        rayIndex >= arrayLength(&currentHits)) { return false; }
    let ray = sceneRays[rayIndex];
    if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u ||
        any(ray.pixelRayLight.xy != pixel)) {
        return false;
    }
    let sentinel = (ray.seedFlags.y &
        SPARSE_VOXEL_SCENE_RAY_FLAG_REGION_SENTINEL) != 0u;
    let prefillStride = max(u32(round(settings.regionSoftProbe.z)), 1u);
    let prefill = sparse_voxel_local_light_hash_mix(
        pixelIndex ^ settings.controls.z * 0x9e3779b9u) %
        prefillStride == 0u;
    if (!sentinel && !prefill) { return false; }
    let lightIndex = ray.pixelRayLight.w;
    if (lightIndex >= min(arrayLength(&lights),
            SPARSE_VOXEL_REGION_STABILITY_LIGHT_CAPACITY)) { return false; }
    let light = lights[lightIndex];
    let lightType = light.flags.y;
    let status = currentHits[rayIndex].status;
    let visible =
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE;
    if (!visible && status != SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
        return false;
    }
    let base = light_field_region_state_base(pixel);
    if (base + SPARSE_VOXEL_REGION_STABILITY_WORDS_PER_TILE >
        arrayLength(&regionStability)) { return false; }
    if (cameraOnlyMotion) {
        light_field_reseed_region_for_camera(base);
    }
    var changed = false;
    if (lightType == WORLD_GI_LIGHT_TYPE_POINT ||
        lightType == WORLD_GI_LIGHT_TYPE_DIRECTIONAL ||
        lightType == WORLD_GI_LIGHT_TYPE_SPHERE ||
        lightType == WORLD_GI_LIGHT_TYPE_PLANAR ||
        lightType == WORLD_GI_LIGHT_TYPE_SKY) {
        let stateIndex = base +
            SPARSE_VOXEL_REGION_STABILITY_HEADER_WORDS + lightIndex;
        if (stateIndex >= arrayLength(&regionStability)) { return false; }
        changed = light_field_update_soft_region_visibility(
            stateIndex, light, identity, visible, cameraOnlyMotion);
    }
    if (changed) {
        atomicMax(
            &regionStability[base + 2u],
            settings.controls.z + max(settings.regionStability.y, 1u) + 1u);
    }
    return changed;
}

fn light_field_record_sky_guide(pixel : vec2<u32>) {
    if (settings.commit.z == 0u) { return; }
    let tileDimensions = (light_field_raster_dimensions() +
        vec2<u32>(SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE - 1u)) /
        SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE;
    let tile = pixel / SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE;
    let tileIndex = tile.y * tileDimensions.x + tile.x;
    let normal = light_field_normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    let rayBase = light_field_owning_slot(pixel);
    let representative = light_field_group_representative(rayBase);
    if (representative >= arrayLength(&sceneRays) ||
        any(sceneRays[representative].pixelRayLight.xy != pixel)) {
        return;
    }
    for (var lane = 0u; lane < light_field_rays_per_pixel();
         lane = lane + 1u) {
        let rayIndex = rayBase + lane;
        if (rayIndex >= arrayLength(&sceneRays) ||
            rayIndex >= arrayLength(&currentHits)) {
            continue;
        }
        let ray = sceneRays[rayIndex];
        if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u ||
            ray.pixelRayLight.w >= arrayLength(&lights)) {
            continue;
        }
        let light = lights[ray.pixelRayLight.w];
        if (light.flags.y != WORLD_GI_LIGHT_TYPE_SKY) { continue; }
        let status = currentHits[rayIndex].status;
        let visible =
            status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
            status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE;
        if (!visible && status != SPARSE_VOXEL_SCENE_RAY_STATUS_HIT) {
            continue;
        }
        let parameter = sparse_voxel_cosine_hemisphere_parameter(
            normal, ray.directionMaximumDistance.xyz);
        let bin = sparse_voxel_sky_guide_bin(parameter);
        let guideBin =
            tileIndex * SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT + bin;
        let feedbackIndex =
            guideBin * SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE;
        if (feedbackIndex + 1u >= arrayLength(&skyGuideFeedback)) {
            continue;
        }
        atomicAdd(&skyGuideFeedback[feedbackIndex], 1u);
        if (visible) {
            let radiance = sparse_voxel_sky_radiance(
                light, ray.directionMaximumDistance.xyz);
            let luminance = dot(max(radiance, vec3<f32>(0.0)),
                vec3<f32>(0.2126, 0.7152, 0.0722));
            let quantized = u32(clamp(
                luminance * SPARSE_VOXEL_SKY_GUIDE_RADIANCE_SCALE,
                0.0, 65535.0));
            atomicAdd(&skyGuideFeedback[feedbackIndex + 1u], quantized);
        }
    }
}

fn light_field_request_sky_guide_reset(pixel : vec2<u32>) {
    if (settings.commit.z == 0u) { return; }
    let tileDimensions = (light_field_raster_dimensions() +
        vec2<u32>(SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE - 1u)) /
        SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE;
    let tile = pixel / SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE;
    let tileIndex = tile.y * tileDimensions.x + tile.x;
    let feedbackIndex = tileIndex * SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT *
        SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE;
    if (feedbackIndex < arrayLength(&skyGuideFeedback)) {
        atomicOr(
            &skyGuideFeedback[feedbackIndex],
            SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_RESET_BIT);
    }
}

// The full-resolution pixel represented by a canonical ray slot.
fn light_field_ray_source_pixel(rayIndex : u32) -> vec2<u32> {
    let rasterDimensions = light_field_raster_dimensions();
    if (rayIndex >= arrayLength(&sceneRays)) { return vec2<u32>(0u); }
    return min(sceneRays[rayIndex].pixelRayLight.xy, rasterDimensions - vec2<u32>(1u));
}

// Lane zero is the reusable non-sky reservoir, but a receiver with only sky
// illumination legitimately has no active lane zero. Return any active lane
// in the group so ownership, identity, and neighbour reconstruction still see
// that receiver's dedicated sky observation.
fn light_field_group_representative(base : u32) -> u32 {
    for (var lane = 0u; lane < light_field_rays_per_pixel(); lane += 1u) {
        let rayIndex = base + lane;
        if (rayIndex < arrayLength(&sceneRays) &&
            (sceneRays[rayIndex].seedFlags.y &
                SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u) {
            return rayIndex;
        }
    }
    return base;
}

// Only these outcomes are complete visibility observations. Incomplete world
// traversal is not the same thing as an occluded shadow ray: averaging an
// UNKNOWN_RESIDENCY, QUERY_LIMIT, or ESCAPED_TREE_VOLUME result as zero burns
// a black sample into the long-lived light field and produces dark speckles
// whenever streaming, camera motion, or an LOD transition makes a query
// temporarily inconclusive.
fn light_field_trace_resolved(rayIndex : u32) -> bool {
    if (rayIndex >= arrayLength(&currentHits)) { return false; }
    let status = currentHits[rayIndex].status;
    return status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT ||
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE;
}

// Diffuse light multiplier contributed by one traced ray slot, evaluated
// against this pixel's shading normal. Material-independent: visibility,
// light colour, attenuation and cosine only, matching what the compose pass
// expects downstream.
fn light_field_finite_contribution(
    rayIndex : u32, normal : vec3<f32>) -> vec3<f32> {
    if (rayIndex >= arrayLength(&sceneRays) ||
        rayIndex >= arrayLength(&currentHits)) { return vec3<f32>(0.0); }
    let ray = sceneRays[rayIndex];
    if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) { return vec3<f32>(0.0); }
    var sampleWeight = 0.0;
    let reservoirLane = rayIndex % light_field_rays_per_pixel();
    if (light_field_uses_independent_rays() && reservoirLane != 0u) {
        // A directly traced candidate is an ordinary Monte Carlo sample.
        // The two proposal factors are light selection and emitter/direction
        // sampling respectively.
        let proposal =
            max(ray.lightSamplePdfWeight.x, 0.0) *
            max(ray.lightSamplePdfWeight.y, 0.0);
        if (proposal <= 0.0) { return vec3<f32>(0.0); }
        sampleWeight = 1.0 / proposal;
    } else {
        let reservoirIndex =
            rayIndex / light_field_rays_per_pixel();
        if (reservoirIndex >= arrayLength(&sceneDIReservoirs)) {
            return vec3<f32>(0.0);
        }
        let reservoir = sceneDIReservoirs[reservoirIndex];
        if (reservoir.state.z == 0u) { return vec3<f32>(0.0); }
        sampleWeight = reservoir.weights.w;
    }
    if (arrayLength(&lights) == 0u) { return vec3<f32>(0.0); }
    let light = lights[min(ray.pixelRayLight.w, arrayLength(&lights) - 1u)];
    let distanceSq = max(
        ray.directionMaximumDistance.w * ray.directionMaximumDistance.w, 1.0e-4);
    let finite = light.flags.y != WORLD_GI_LIGHT_TYPE_DIRECTIONAL &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_SKY;
    let attenuation = select(1.0, 1.0 / distanceSq, finite);
    var emitterCosine = 1.0;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        if (light.shapeParams.x > 1.0e-6) {
            let sampledPoint = ray.originMinimumDistance.xyz +
                ray.directionMaximumDistance.xyz * ray.directionMaximumDistance.w;
            let emitterNormal = light_field_normalize(
                sampledPoint - light.positionRange.xyz,
                -ray.directionMaximumDistance.xyz);
            emitterCosine = max(dot(emitterNormal, -ray.directionMaximumDistance.xyz), 0.0);
        }
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let emitterNormal = light_field_normalize(
            light.directionType.xyz, vec3<f32>(0.0, -1.0, 0.0));
        emitterCosine = max(dot(emitterNormal, -ray.directionMaximumDistance.xyz), 0.0);
    }
    var radiance = max(light.colorIntensity.rgb, vec3<f32>(0.0)) *
        max(light.colorIntensity.w, 0.0) * LIGHT_FIELD_INV_PI;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
        radiance = sparse_voxel_sky_radiance(
            light, ray.directionMaximumDistance.xyz) * LIGHT_FIELD_INV_PI;
    }
    let status = currentHits[rayIndex].status;
    let visibility = select(
        0.0, 1.0,
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
        status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE);
    return radiance * attenuation *
        max(dot(normal, ray.directionMaximumDistance.xyz), 0.0) *
        emitterCosine * visibility * sampleWeight;
}

// Unshadowed sky and sun for a normal, with no traced visibility.
//
// The last-resort value for a pixel that has no measurement, no history and no
// neighbour to infer from. Publishing zero there is what makes a fast camera
// rotation reveal black regions: the estimate is not "no light", it is "no
// evidence yet", and the brightest plausible answer converges downward as real
// samples arrive rather than flashing dark and brightening. Analytic emitters
// only - a finite light would need the distance and visibility this pixel does
// not have.
fn light_field_unshadowed_analytic(normal : vec3<f32>) -> vec3<f32> {
    var total = vec3<f32>(0.0);
    let lightCount = min(arrayLength(&lights), max(settings.controls.w, 1u));
    for (var lightIndex = 0u; lightIndex < lightCount; lightIndex += 1u) {
        let light = lights[lightIndex];
        if ((light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) == 0u) { continue; }
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
            let direction = light_field_normalize(
                -light.directionType.xyz, vec3<f32>(0.0, 1.0, 0.0));
            total += max(light.colorIntensity.rgb, vec3<f32>(0.0)) *
                max(light.colorIntensity.w, 0.0) * LIGHT_FIELD_INV_PI *
                max(dot(normal, direction), 0.0);
        } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
            // A cosine-weighted hemisphere of sky, sampled along the normal and
            // the horizon rather than integrated: two taps are enough for a
            // placeholder that a real sample replaces outright.
            let up = light_field_normalize(
                vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(0.0, 1.0, 0.0));
            let horizon = light_field_normalize(
                normal - up * dot(normal, up), up);
            total += (sparse_voxel_sky_radiance(light, normal) * 0.75 +
                sparse_voxel_sky_radiance(light, horizon) * 0.25) *
                LIGHT_FIELD_INV_PI * 0.5;
        }
    }
    return total;
}

// Whether a ray slot may contribute to this pixel: it must have been traced
// against the same surface. Without this a slot bleeds light across a
// silhouette, which is what the reduced-resolution upsample had to guard too.
fn light_field_slot_matches(
    rayPixel : vec2<u32>, identity : vec2<u32>,
    normal : vec3<f32>, depth : f32) -> bool {
    let rasterDimensions = light_field_raster_dimensions();
    let rayIndex = light_field_ray_group_base(rayPixel);
    let representative = light_field_group_representative(rayIndex);
    let rayActive = representative < arrayLength(&sceneRays) &&
        (sceneRays[representative].seedFlags.y &
            SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u;
    if (!rayActive) { return false; }
    if (!light_field_uses_independent_rays()) {
        let reservoirIndex =
            rayIndex / light_field_rays_per_pixel();
        if (reservoirIndex >= arrayLength(&sceneDIReservoirs) ||
            sceneDIReservoirs[reservoirIndex].state.z == 0u ||
            !light_field_trace_resolved(rayIndex)) {
            return false;
        }
    } else {
        var anyResolved = false;
        for (var raySlot = 0u;
             raySlot < light_field_rays_per_pixel();
             raySlot = raySlot + 1u) {
            let slotIndex = rayIndex + raySlot;
            if (slotIndex < arrayLength(&sceneRays) &&
                (sceneRays[slotIndex].seedFlags.y &
                    SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
                light_field_trace_resolved(slotIndex)) {
                anyResolved = true;
                break;
            }
        }
        if (!anyResolved) { return false; }
    }
    let sourcePixel = light_field_ray_source_pixel(representative);
    let sourceIndex = sourcePixel.y * rasterDimensions.x + sourcePixel.x;
    if (sourceIndex >= arrayLength(&linearDepth)) { return false; }
    if (any(light_field_identity(vec2<i32>(sourcePixel)) != identity)) { return false; }
    let sampleNormal = light_field_normalize(
        textureLoad(geometryNormalTexture, vec2<i32>(sourcePixel), 0).xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    if (dot(normal, sampleNormal) < settings.thresholds.x) { return false; }
    let sampleDepth = linearDepth[sourceIndex];
    // `linearDepth` is view depth.  A one-metre floor makes a receiver one
    // centimetre from the eye accept samples from faces 5--30 cm away.  Scale
    // by the actual receiver depth and only use the projection near plane as
    // the numerical floor, exactly like temporal receiver validation.
    let depthScale = max(max(abs(depth), abs(sampleDepth)),
        max(raster.cameraWorldPositionNear.w, 1.0e-3));
    return abs(depth - sampleDepth) / depthScale <= settings.thresholds.y;
}

struct LightFieldSample {
    value : vec3<f32>,
    sampleCount : f32,
    valid : bool,
    direct : bool,
}

// The canonical ray slot for this pixel. The general ratio remains useful to
// keep this pass valid if its input and output domains are decoupled again.
fn light_field_owning_slot(pixel : vec2<u32>) -> u32 {
    let rayDimensions = light_field_ray_dimensions();
    let rasterDimensions = light_field_raster_dimensions();
    let rayPixel = min(
        (pixel * rayDimensions) / rasterDimensions,
        rayDimensions - vec2<u32>(1u));
    return light_field_ray_group_base(rayPixel);
}

fn light_field_ray_group_sample(
    rayPixel : vec2<u32>, normal : vec3<f32>) -> LightFieldSample {
    let base = light_field_ray_group_base(rayPixel);
    let slotCount = select(
        1u, light_field_rays_per_pixel(),
        light_field_uses_independent_rays());
    // Production uses a dedicated sky lane plus one or more non-sky ReSTIR
    // lanes. They are disjoint estimator strata: average duplicate samples
    // inside each stratum, then add the two estimates. Averaging sky with the
    // direct-light reservoir would halve both terms and, more importantly,
    // would make adding a guaranteed AO ray change scene exposure.
    var skySum = vec3<f32>(0.0);
    var skyCount = 0.0;
    var emitterSum = vec3<f32>(0.0);
    var emitterCount = 0.0;
    var validCount = 0.0;
    for (var raySlot = 0u; raySlot < slotCount;
         raySlot = raySlot + 1u) {
        let rayIndex = base + raySlot;
        if (rayIndex >= arrayLength(&sceneRays) ||
            rayIndex >= arrayLength(&currentHits) ||
            (sceneRays[rayIndex].seedFlags.y &
                SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u ||
            !light_field_trace_resolved(rayIndex)) {
            continue;
        }
        let requiresReservoir =
            !light_field_uses_independent_rays() || raySlot == 0u;
        let reservoirIndex =
            rayIndex / light_field_rays_per_pixel();
        if (requiresReservoir &&
            (reservoirIndex >= arrayLength(&sceneDIReservoirs) ||
             sceneDIReservoirs[reservoirIndex].state.z == 0u)) {
            continue;
        }
        let contribution =
            light_field_finite_contribution(rayIndex, normal);
        let lightIndex = sceneRays[rayIndex].pixelRayLight.w;
        let sky = lightIndex < arrayLength(&lights) &&
            lights[lightIndex].flags.y == WORLD_GI_LIGHT_TYPE_SKY;
        if (sky) {
            skySum = skySum + contribution;
            skyCount = skyCount + 1.0;
        } else {
            emitterSum = emitterSum + contribution;
            emitterCount = emitterCount + 1.0;
        }
        validCount = validCount + 1.0;
    }
    let sum = skySum / max(skyCount, 1.0) +
        emitterSum / max(emitterCount, 1.0);
    return LightFieldSample(
        sum, validCount,
        validCount > 0.0, false);
}

// This frame's lighting for a pixel. A resolved visibility result is valid only
// for the receiver that fired the ray. Sparse pixels without a current trace
// are handled by temporal history (or a zero-confidence prior) in the resolve
// pass; copying a neighbour's binary visibility would spread shadows laterally.
fn light_field_current_sample(
    pixel : vec2<u32>, identity : vec2<u32>,
    geometryNormal : vec3<f32>, shadingNormal : vec3<f32>,
    depth : f32) -> LightFieldSample {
    let rayDimensions = light_field_ray_dimensions();
    let rasterDimensions = light_field_raster_dimensions();

    // A resolved visibility sample belongs only to the receiver that fired it.
    // Spatial ReSTIR has already reconnected and retraced any reused proposal.
    var directSample =
        LightFieldSample(vec3<f32>(0.0), 0.0, false, true);
    let ownRayPixel = min(
        (pixel * rayDimensions) / rasterDimensions,
        rayDimensions - vec2<u32>(1u));
    let candidateSlot = light_field_ray_group_base(ownRayPixel);
    let representative = light_field_group_representative(candidateSlot);
    if (representative < arrayLength(&sceneRays)) {
        let owningRay = sceneRays[representative];
        if ((owningRay.seedFlags.y &
                SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
            all(owningRay.pixelRayLight.xy == pixel)) {
            let ownSample =
                light_field_ray_group_sample(ownRayPixel, shadingNormal);
            if (ownSample.valid) {
                directSample = ownSample;
                directSample.direct = true;
            }
        }
    }
    light_field_stat_add(0u, 1u);
    if (!directSample.valid) {
        light_field_gate_add(27u, 1u);
        light_field_stat_add(1u, 1u);
        return directSample;
    }
    light_field_stat_add(2u, 1u);
    return directSample;
}

@compute @workgroup_size(8, 8, 1)
fn resolveSparseVoxelLightField(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let rasterDimensions = light_field_raster_dimensions();
    let pixel = globalId.xy;
    if (any(pixel >= rasterDimensions)) { return; }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * rasterDimensions.x + pixel.x;

    let identity = light_field_identity(pixelI);
    if (pixelIndex >= arrayLength(&coverage) || coverage[pixelIndex] == 0u) {
        textureStore(outputFieldTexture, pixelI, vec4<f32>(0.0));
        textureStore(outputIdTexture, pixelI, vec4<u32>(identity, 0u, 0u));
        textureStore(reactiveOutput, pixelI, vec4<f32>(0.0));
        textureStore(outputMomentsTexture, pixelI, vec4<f32>(0.0));
        textureStore(outputLightHashTexture, pixelI, vec4<u32>(0u));
        textureStore(outputViewDepthTexture, pixelI, vec4<f32>(0.0));
        textureStore(outputNormalTexture, pixelI, vec4<f32>(0.0));
        return;
    }

    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - raster.focalPrincipal.z) /
            raster.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - raster.focalPrincipal.w) /
            raster.focalPrincipal.y,
        1.0);
    let worldDirection = normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    let surfacePosition = raster.cameraWorldPositionNear.xyz +
        worldDirection * linearDepth[pixelIndex];
    let currentClip =
        raster.worldToClip * vec4<f32>(surfacePosition, 1.0);
    let previousClip =
        raster.previousWorldToClip * vec4<f32>(surfacePosition, 1.0);
    let currentViewDepth = abs(currentClip.w);
    let expectedHistoryViewDepth = abs(previousClip.w);
    textureStore(
        outputViewDepthTexture, pixelI, vec4<f32>(currentViewDepth));
    // Published here rather than beside the accumulator's own normal fetch:
    // that one is inside a branch taken only by scheduled or unconverged
    // pixels, and history validation needs an orientation for every covered
    // pixel every frame.
    let historyGeometryNormal = light_field_normalize(
        textureLoad(geometryNormalTexture, pixelI, 0).xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    textureStore(
        outputNormalTexture, pixelI,
        vec4<f32>(historyGeometryNormal, 1.0));
    let historyDepthTolerance =
        light_field_depth_tolerance(expectedHistoryViewDepth);
    let currentLightHash = sparse_voxel_local_light_hash(
        surfacePosition, settings.controls.w);

    let motion = textureLoad(motionVectorTexture, pixelI, 0).xy;
    let screenMotionPixels = length(
        motion * vec2<f32>(rasterDimensions));
    let residualObjectMotionPixels = light_field_object_motion_pixels(
        surfacePosition, motion, rasterDimensions);
    let exactSurfaceIdentity =
        (identity.y & SPARSE_VOXEL_TEMPORAL_ID_EXACT) != 0u;
    let cameraOnlyScreenMotion = settings.controls.x != 0u &&
        screenMotionPixels > 0.25 &&
        residualObjectMotionPixels <= 0.25;
    let cameraOnlyProbeMotion =
        exactSurfaceIdentity && cameraOnlyScreenMotion;

    // The old debug-present pass produced this mask while also evaluating
    // every diagnostic mode and rewriting an otherwise-unused colour target.
    // Keep the useful fail-closed signal in the light-field pass that already
    // visits every shaded pixel.
    var reactive = 0.0;
    var resolvedRays = 0u;
    let reactiveBase = light_field_owning_slot(pixel);
    for (var raySlot = 0u;
         raySlot < light_field_rays_per_pixel();
         raySlot = raySlot + 1u) {
        let rayIndex = reactiveBase + raySlot;
        if (rayIndex >= arrayLength(&sceneRays) ||
            rayIndex >= arrayLength(&currentHits) ||
            (sceneRays[rayIndex].seedFlags.y &
                SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) == 0u) {
            continue;
        }
        let status = currentHits[rayIndex].status;
        if (status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME ||
            status == SPARSE_VOXEL_SCENE_RAY_STATUS_UNKNOWN_RESIDENCY ||
            status == SPARSE_VOXEL_SCENE_RAY_STATUS_QUERY_LIMIT) {
            // Fail closed for TAA, and keep counting: the budget wants the
            // resolved total for every lane, not just the ones before the
            // first inconclusive trace.
            reactive = 1.0;
        } else if (light_field_trace_resolved(rayIndex)) {
            resolvedRays = resolvedRays + 1u;
        }
    }
    // Feed this frame's resolved visibility back into the world-region cache.
    // This is after all screen/Tree64 tracing and before next frame's light
    // selection, so candidate PDFs only depend on completed observations.
    light_field_record_reachability(pixelIndex);
    light_field_record_sky_guide(pixel);

    // Reproject the per-pixel history. Validated on geometry identity, which
    // is the only signal that separates two adjacent voxels: a whole wall
    // shares one axis-aligned normal.
    var historyValue = vec3<f32>(0.0);
    var historyCount = 0.0;
    var historyMoments = vec2<f32>(0.0);
    var motionPixels = screenMotionPixels;
    var rawHistory = LightFieldRawHistory(
        0.0, 0.0, 0.0, vec3<f32>(0.0));
    var reprojectedViewDepth = 0.0;
    if (settings.controls.x != 0u) {
        let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(rasterDimensions);
        let historyUv = uv - motion;
        if (all(historyUv >= vec2<f32>(0.0)) && all(historyUv < vec2<f32>(1.0))) {
            rawHistory = light_field_raw_history(historyUv);
            reprojectedViewDepth = textureLoad(
                historyViewDepthTexture,
                clamp(
                    vec2<i32>(historyUv * vec2<f32>(rasterDimensions)),
                    vec2<i32>(0), vec2<i32>(rasterDimensions) - vec2<i32>(1)),
                0).x;
            let history = light_field_reproject_history(
                historyUv, expectedHistoryViewDepth, historyGeometryNormal,
                historyDepthTolerance);
            historyValue = history.rgb;
            historyCount = history.a;
            if (historyCount > 0.0) {
                historyMoments =
                    light_field_reproject_moments(
                        historyUv, expectedHistoryViewDepth,
                        historyGeometryNormal,
                        historyDepthTolerance);
            }
        }
    }
    let disoccluded = settings.controls.x != 0u && historyCount <= 0.0;
    var regionInvalidated = false;
    if (disoccluded) {
        // Reject SVGF/TAA history at disocclusions and wherever the local set
        // of affecting light revisions changed.
        reactive = 1.0;
        // Screen-tile probe evidence belongs to the geometry that previously
        // occupied this footprint. Newly revealed geometry must not inherit
        // that distribution: clear it and spend a short local wake window.
        regionInvalidated = light_field_invalidate_region(pixel);
    }
    // Disocclusions are sparse and must reset immediately. During continuous
    // camera motion, a deterministic sparse election avoids hundreds of
    // thousands of redundant atomicOr operations on the same tile flag.
    let skyCameraResetSample =
        (sparse_voxel_local_light_hash_mix(
            pixelIndex ^ settings.controls.z * 0x9e3779b9u) & 63u) == 0u;
    if (regionInvalidated ||
        (cameraOnlyScreenMotion && skyCameraResetSample)) {
        light_field_request_sky_guide_reset(pixel);
    }
    let regionVisibilityChanged = light_field_record_region_visibility(
        pixel, pixelIndex, identity,
        cameraOnlyProbeMotion && !disoccluded);
    light_field_record_ray_budget(
        pixel, resolvedRays, regionVisibilityChanged);
    if (regionVisibilityChanged) { reactive = 1.0; }
    // Region wake-up is a scheduling signal, not per-pixel proof that lighting
    // changed. A near-ground camera translation can disocclude one receiver in
    // a tile while every neighbour still has valid, stable history. Truncating
    // all of those records and marking the whole tile reactive made one
    // reprojection miss reject both light-field and SVGF/TAA history over a
    // 16x16 block. The resulting fresh binary shadow samples appeared as dark
    // regions blinking on while the camera moved.
    //
    // Keep the tile awake so the scheduler gathers fresh evidence. Only the
    // receiver that disoccluded above, or a sentinel that validated a real
    // visibility change, is allowed to invalidate accumulated lighting.
    let regionAwake = light_field_region_is_awake(pixel);

    // A completed shadow ray measures visibility only at its own receiver.
    // ReSTIR may spatially reuse a light proposal before tracing, because that
    // proposal is reconnected to this receiver and visibility is evaluated
    // here. Post-trace sharing is biased and produced screen-space shadow
    // bands, so an unscheduled pixel can use only compatible history or a
    // zero-confidence prior until its own phase is active.
    let owningSlot = light_field_owning_slot(pixel);
    var directScheduled = false;
    let owningRepresentative = light_field_group_representative(owningSlot);
    if (owningRepresentative < arrayLength(&sceneRays)) {
        let owningRay = sceneRays[owningRepresentative];
        directScheduled =
            (owningRay.seedFlags.y &
                SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
            all(owningRay.pixelRayLight.xy == pixel);
    }
    var sample =
        LightFieldSample(vec3<f32>(0.0), 0.0, false, false);
    var geometryNormal = vec3<f32>(0.0, 1.0, 0.0);
    if (directScheduled) {
        geometryNormal = light_field_normalize(
            textureLoad(geometryNormalTexture, pixelI, 0).xyz,
            vec3<f32>(0.0, 1.0, 0.0));
        let shadingNormal = light_field_normalize(
            textureLoad(shadingNormalTexture, pixelI, 0).xyz,
            geometryNormal);
        let depth = select(
            0.0, linearDepth[pixelIndex],
            pixelIndex < arrayLength(&linearDepth));
        sample = light_field_current_sample(
            pixel, identity, geometryNormal, shadingNormal, depth);
    }

    // One fixed bounded accumulation window for every receiver. Per-pixel
    // variance-driven windows made adjacent pixels adapt at different rates,
    // which turned ordinary stochastic variation into moving patches.
    let maximumSamples = f32(max(settings.controls.y, 1u));
    let blendWindow = maximumSamples;
    var value = historyValue;
    var count = historyCount;
    var secondMoment = historyMoments.x;
    // A tile that is rebuilding is not ready to sleep even though its radiance
    // remains valid. Reset only the scheduler's stability evidence; preserving
    // radiance confidence and the reactive mask is what keeps camera-induced
    // wake-up visually neutral.
    var stableUpdates = select(historyMoments.y, 0.0, regionAwake);
    var signalValid = historyCount > 0.0;
    if (sample.valid) {
        // Only this receiver's completed trace reaches this branch. ReSTIR
        // spatial reuse has already happened upstream and retraced visibility
        // from this exact surface point.
        let observations = max(sample.sampleCount, 1.0);
        let effectiveHistoryCount = historyCount;
        let blendHistoryValue = historyValue;
        count = min(historyCount + observations, maximumSamples);
        // Firefly rejection. A guided sky sample divided by a small bin
        // probability, or a barely covered voxel that only ever gets one ray,
        // can produce a radiance orders of magnitude above this receiver's own
        // running distribution and then sit in the mean for the whole window.
        // Clamp the incoming sample to a multiple of its measured standard
        // deviation once there is enough history to know what that is. The
        // clamp is one-sided: only implausible brightening is suppressed, so a
        // genuine transition into shadow still lands immediately.
        //
        // The gate is in observations, not frames, and it is deliberately not
        // the four the responsiveness window can collapse to. The variance this
        // reads is an exponentially weighted estimate whose effective sample
        // size is one over its own blend rate, so at a narrow window it is
        // built from a handful of binary visibility samples and is far more
        // often too small than too large. Too small means a ceiling barely
        // above the running mean, which suppresses the pixel's next honest
        // brightening and leaves it dark while its neighbours recover - a stain
        // the clamp created rather than prevented.
        var sampleValue = sample.value;
        if (!regionAwake && effectiveHistoryCount >= 8.0 &&
            settings.spatialReuse2.x > 0.0) {
            let historyMean = dot(max(blendHistoryValue, vec3<f32>(0.0)),
                vec3<f32>(0.2126, 0.7152, 0.0722));
            let historyVariance = max(
                secondMoment - historyMean * historyMean, 0.0);
            let ceiling = historyMean +
                settings.spatialReuse2.x * sqrt(historyVariance) +
                max(historyMean * 0.1, 1.0e-3);
            let sampleLum = dot(max(sampleValue, vec3<f32>(0.0)),
                vec3<f32>(0.2126, 0.7152, 0.0722));
            if (sampleLum > ceiling) {
                // Scale rather than clip so hue survives the clamp.
                sampleValue = sampleValue * (ceiling / max(sampleLum, 1.0e-6));
            }
        }
        // At the sample cap this remains a bounded moving average. Multiple
        // rays receive proportionally more weight than a one-ray frame. The
        // blend uses the responsiveness window rather than the evidence count,
        // which is what lets a noisy receiver track its true mean quickly
        // without pretending it has seen fewer samples than it has.
        let blendAlpha = min(
            observations / max(min(count, blendWindow), 1.0), 1.0);
        value = mix(blendHistoryValue, sampleValue, blendAlpha);
        let sampleLuminance = dot(max(sampleValue, vec3<f32>(0.0)),
            vec3<f32>(0.2126, 0.7152, 0.0722));
        // The second moment keeps the *full* window even when the mean is
        // allowed to move faster.
        //
        // Sharing the responsiveness alpha ties the two together: narrowing the
        // window to track a lighting change also shrinks the variance
        // estimator's effective sample size, so exactly when the pixel is
        // moving - and its true variance is highest - the measured variance
        // collapses toward zero. The firefly ceiling above then closes on the
        // mean and holds the pixel where it was. Measuring spread over the long
        // window and the mean over the short one is the point of having two.
        let momentAlpha = min(
            observations / max(min(count, maximumSamples), 1.0), 1.0);
        secondMoment = mix(
            secondMoment, sampleLuminance * sampleLuminance, momentAlpha);
        signalValid = true;
    } else if (historyCount > 0.0) {
        // No completed current observation. Hold measured history rather than
        // turning an inconclusive trace into black illumination.
        value = historyValue;
        count = historyCount;
        signalValid = true;
    } else {
        // A slot without history gets a zero-confidence analytic prior, never
        // another receiver's resolved visibility. Its own checkerboard phase
        // replaces the prior on the next scheduled frame.
        //
        // This also covers a *scheduled* receiver whose own trace came back
        // inconclusive - escaped tree volume, unknown residency, query limit -
        // and that has no history to hold. Guarding the branch on
        // `!directScheduled` left that pixel with no branch at all: it kept
        // signalValid false and published radiance 0 with a sample count of 0,
        // which every consumer reads as an uncovered pixel and paints black.
        value = light_field_unshadowed_analytic(
            light_field_normalize(
                textureLoad(shadingNormalTexture, pixelI, 0).xyz,
                geometryNormal)) *
            clamp(settings.spatialReuse2.w, 0.0, 1.0);
        count = 0.0;
        signalValid = true;
    }

    // Alpha is the measured sample count. Negative one is a transient
    // "reconstructed but valid" marker: it remains zero-count to this pass on
    // the next frame, while allowing SVGF to filter the inferred RGB instead
    // of treating it as an uncovered pixel and replacing it with black.
    let encodedCount = select(
        count, -1.0, signalValid && count <= 0.0);
    let outputLuminance = dot(max(value, vec3<f32>(0.0)),
        vec3<f32>(0.2126, 0.7152, 0.0722));
    let variance = max(secondMoment - outputLuminance * outputLuminance, 0.0);
    let relativeStandardError =
        sqrt(variance / max(count, 1.0)) / max(outputLuminance, 0.05);
    // Stability is graded evidence, not an unbroken streak.
    //
    // This used to require N consecutive *direct* observations and reset the
    // counter to zero on any single miss. With one binary visibility sample per
    // observation that is a random-restart process: a lone unlucky sample trips
    // the error test, the streak collapses, and the receiver may never be
    // allowed to sleep however good its accumulated estimate already is. It was
    // the real reason sky-only lighting stayed noisy - the radiance estimate
    // converged long before the scheduler would believe it, so the pixel kept
    // being retraced and kept being re-noised. Averaging four rays per pixel hid
    // the problem by making a single observation reliable enough to pass, which
    // is why it appeared that only more rays helped.
    //
    // Decay on a miss instead of resetting. Every valid current sample now
    // belongs to this exact receiver; proposal reuse occurred before tracing.
    if (sample.valid) {
        let stableObservation =
            count >= 16.0 && relativeStandardError <= 0.35 &&
            motionPixels <= 0.25 && reactive <= 0.0;
        let credit = select(
            clamp(settings.spatialReuse2.y, 0.0, 1.0), 1.0, sample.direct);
        stableUpdates = select(
            max(stableUpdates - max(settings.spatialReuse2.z, 0.0), 0.0),
            min(stableUpdates + credit, 255.0),
            stableObservation);
    }
    light_field_record_pop(
        rawHistory, value, reprojectedViewDepth, expectedHistoryViewDepth,
        historyGeometryNormal);
    textureStore(
        outputFieldTexture, pixelI,
        vec4<f32>(max(value, vec3<f32>(0.0)), encodedCount));
    textureStore(outputIdTexture, pixelI, vec4<u32>(identity, 0u, 0u));
    textureStore(
        reactiveOutput, pixelI,
        vec4<f32>(clamp(reactive, 0.0, 1.0), 0.0, 0.0, 0.0));
    textureStore(
        outputMomentsTexture, pixelI,
        vec4<f32>(max(secondMoment, 0.0), stableUpdates, 0.0, 0.0));
    textureStore(
        outputLightHashTexture, pixelI,
        vec4<u32>(currentLightHash, 0u, 0u, 0u));
}
