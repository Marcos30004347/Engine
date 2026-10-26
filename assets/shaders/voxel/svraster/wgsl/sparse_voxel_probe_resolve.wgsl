#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_probe_common.wgsl"

// Probe resolve: this frame's traced directions join the shared radiance map,
// and the map becomes one SH probe.
//
// One workgroup owns one probe and one lane owns one direction cell, so the
// whole octahedral set is updated and projected to nine coefficients by a
// single workgroup reduction. Brixelizer splits this into a fill pass and a
// project pass because it also ray-marches the map for specular; the
// diffuse-only path here has no such consumer, so the two collapse.
//
// The temporal accumulation lives at the *cell* level rather than on the SH.
// That is the whole reason the sharing pass can work: a cell is a directional
// sample with a hit distance, so blending it with the reprojected value from
// the previous frame is blending two estimates of the same direction. Blending
// SH coefficients instead averages nine numbers that no longer know which
// direction they came from, which is both slower to converge and impossible to
// reproject.
//
// Cell and next-event histories hold the unweighted traced estimators. The
// current camera-distance weight is applied only while projecting the final SH,
// followed by the analytical sky/sun complement. Keeping that presentation
// weight out of history makes the transition follow camera motion immediately
// instead of converging toward a distance the probe occupied many frames ago.
//
// Three failure modes are handled explicitly here, because each one shows up as
// a probe going black rather than as a subtle error:
//
//   - A ray whose traversal ran out of *traced volume* found no occluder in
//     everything the tree actually contains. That is a miss, not an unknown. If
//     it were folded in as black, every probe would go dark the moment the
//     camera moved far enough into the scene for a probe ray to leave the
//     camera-centred clipmap while still inside the scene bounds.
//   - A ray that is genuinely inconclusive - unstreamed residency, exhausted
//     query budget - contributes nothing *and is excluded from the
//     normalisation*. Averaging it as zero darkens the probe in proportion to
//     how much of the tree was missing.
//   - A cell with no answer at all this frame keeps the shared value rather
//     than resetting, so a probe never drops to black while it waits.

struct SparseVoxelProbeResolveSettings {
    // xy raster dimensions, z probe tile size, w frame index.
    dimensionsTile : vec4<u32>,
    // x probes across, y probes down, z light count, w next-event slots.
    probeCounts : vec4<u32>,
    // x per-cell temporal blend against the shared value, y directional angular
    // cosine, z firefly clamp ratio against the probe mean, w unused.
    blend : vec4<f32>,
    // x probes in total, refinement overflow included.
    counts : vec4<u32>,
    // Scheduling thresholds, matching the ray pass: x minimum traced updates,
    // y maximum relative standard error for a stable update, z runs to reach
    // the converged tier, w runs to reach the asleep tier.
    thresholds : vec4<f32>,
    // Frames between updates per tier, and directions traced per update per
    // tier. Together they give how often any one *cell* is re-measured, which
    // is what the temporal blend has to be expressed in.
    intervals : vec4<u32>,
    rayCounts : vec4<u32>,
    // xy distance-density transitions, z local-light fade start, w sky-only.
    distances : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelProbeResolveSettings;
@group(0) @binding(2) var<storage, read> probes : array<SparseVoxelProbe>;
@group(0) @binding(3) var<storage, read> sceneRays : array<SparseVoxelSceneRay>;
@group(0) @binding(4) var<storage, read> sceneHits : array<SparseVoxelSceneRayHit>;
@group(0) @binding(5) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(6) var<storage, read_write> radiance : array<vec4<f32>>;
@group(0) @binding(7) var<storage, read_write> probeSH : array<vec4<f32>>;
@group(0) @binding(8) var<storage, read_write> schedule :
    array<SparseVoxelProbeSchedule>;
@group(0) @binding(9) var<storage, read> previousEventSH : array<vec4<f32>>;
@group(0) @binding(10) var<storage, read_write> eventSH : array<vec4<f32>>;

// One accumulator per SH coefficient per colour channel. 64 lanes reduce into
// these; the barrier cost is trivial next to the trace that produced the rays.
var<workgroup> shRed : array<atomic<u32>, 9>;
var<workgroup> shGreen : array<atomic<u32>, 9>;
var<workgroup> shBlue : array<atomic<u32>, 9>;
// Next-event samples are pdf-weighted emitter contributions rather than
// directional radiance cells. This includes the deterministic directional
// shadow sample as well as finite emitters. Accumulate them separately; the
// shared sky map already owns its own cell-level history.
var<workgroup> eventSHRed : array<atomic<u32>, 9>;
var<workgroup> eventSHGreen : array<atomic<u32>, 9>;
var<workgroup> eventSHBlue : array<atomic<u32>, 9>;
// Mean of this frame's traced cells, used both to clamp fireflies and to fill
// cells that came back with nothing.
var<workgroup> tracedRed : atomic<u32>;
var<workgroup> tracedGreen : atomic<u32>;
var<workgroup> tracedBlue : atomic<u32>;
var<workgroup> tracedCount : atomic<u32>;
// Mean luminance of this frame's next-event samples. Those samples never enter
// the radiance map, so they need their own reference: they are the estimator
// most prone to fireflies, because an emitter close to a receiver carries a
// 1/distance squared factor that a single sample cannot average away.
var<workgroup> eventLuminance : atomic<u32>;
var<workgroup> eventCount : atomic<u32>;
// How many of the 64 cells ended up carrying a usable radiance sample.
var<workgroup> mapCount : atomic<u32>;

// Radiance is non-negative and bounded in practice, so a fixed-point atomic
// reduction is both exact enough and far cheaper than a log-step shuffle.
const PROBE_SH_FIXED_SCALE : f32 = 4096.0;
const PROBE_MEAN_FIXED_SCALE : f32 = 1024.0;

fn probe_shadow_trace_weight(position : vec3<f32>) -> f32 {
    let cameraDistance = distance(
        position, raster.cameraWorldPositionNear.xyz);
    return 1.0 - smoothstep(
        settings.distances.z, settings.distances.w, cameraDistance);
}

fn probe_has_event_light(position : vec3<f32>) -> bool {
    let count = min(settings.probeCounts.z, arrayLength(&lights));
    for (var i = 0u; i < count; i += 1u) {
        let light = lights[i];
        let finite = light.flags.y == WORLD_GI_LIGHT_TYPE_POINT ||
            light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE ||
            light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR;
        let directional =
            light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL;
        var affects = true;
        let travel = distance(light.positionRange.xyz, position);
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
            affects = distance(
                worldgi_planar_light_closest_point(light, position),
                position) <= max(light.positionRange.w, 0.0);
        } else {
            let emitterRadius = select(
                0.0, max(light.shapeParams.x, 0.0),
                light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE);
            let influence = max(light.positionRange.w, 0.0) + emitterRadius;
            affects = influence <= 0.0 || travel <= influence;
        }
        if ((directional || finite) &&
            affects &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
            light.colorIntensity.w > 0.0) {
            return true;
        }
    }
    return false;
}

fn probe_analytical_event_coefficient(
    position : vec3<f32>, normal : vec3<f32>, coefficient : u32) -> vec3<f32> {
    let analyticalWeight = 1.0 - probe_shadow_trace_weight(position);
    if (analyticalWeight <= 0.0) { return vec3<f32>(0.0); }
    let count = min(settings.probeCounts.z, arrayLength(&lights));
    for (var i = 0u; i < count; i += 1u) {
        let light = lights[i];
        if ((light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) == 0u ||
            light.colorIntensity.w <= 0.0 ||
            light.flags.y != WORLD_GI_LIGHT_TYPE_DIRECTIONAL) { continue; }
        let colour = light.colorIntensity.xyz * light.colorIntensity.w;
        let direction = -normalize(light.directionType.xyz);
        // The receiver cosine is applied once when SH irradiance is evaluated.
        // This gate only keeps the sun inside the probe's represented hemi.
        // Finite emitters deliberately have no analytical complement: their
        // traced contribution fades to zero over the distance transition, so a
        // local lamp cannot illuminate distant terrain without visibility.
        if (dot(normal, direction) <= 0.0) { continue; }
        if (sparse_voxel_probe_luminance(colour) <= 0.0) { continue; }
        let basis = sparse_voxel_probe_sh_basis(direction);
        // The ray pass reserves its directional slot for the first eligible
        // sun. Match that selection here so the analytical complement replaces
        // exactly the traced estimator instead of fading in additional suns
        // that never had a shadow ray.
        return colour * analyticalWeight * basis[coefficient];
    }
    return vec3<f32>(0.0);
}

// Radiance arriving along a direction whose ray reached open space.
fn probe_miss_radiance(
    direction : vec3<f32>) -> vec3<f32> {
    var value = vec3<f32>(0.0);
    let count = min(settings.probeCounts.z, arrayLength(&lights));
    for (var i = 0u; i < count; i += 1u) {
        let light = lights[i];
        if ((light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) == 0u ||
            light.colorIntensity.w <= 0.0) { continue; }
        // Sum every enabled sky. The previous first-sky-only lookup made the
        // result depend on light buffer order when more than one environment
        // contribution was present.
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
            value += sparse_voxel_sky_radiance(light, direction);
            continue;
        }
        // Directional emitters are sampled explicitly by next-event slots.
        // Leaving the old tiny-cone estimator here double counts the sun and
        // reintroduces its rare, very bright Monte Carlo samples.
    }
    return value;
}

fn probe_accumulate_event_sh(
    contribution : vec3<f32>, basis : array<f32, 9>) {
    for (var c = 0u; c < SPARSE_VOXEL_PROBE_SH_COUNT; c += 1u) {
        let value = contribution * basis[c];
        atomicAdd(&eventSHRed[c], bitcast<u32>(
            i32(round(value.x * PROBE_SH_FIXED_SCALE))));
        atomicAdd(&eventSHGreen[c], bitcast<u32>(
            i32(round(value.y * PROBE_SH_FIXED_SCALE))));
        atomicAdd(&eventSHBlue[c], bitcast<u32>(
            i32(round(value.z * PROBE_SH_FIXED_SCALE))));
    }
}

fn probe_accumulate_sh(contribution : vec3<f32>, basis : array<f32, 9>) {
    for (var c = 0u; c < SPARSE_VOXEL_PROBE_SH_COUNT; c += 1u) {
        let value = contribution * basis[c];
        // Two's complement addition is bit-identical for signed and unsigned
        // operands, so a bitcast carries negative SH lobes through an unsigned
        // atomic with no bias to unwind.
        //
        // Rounded rather than truncated: a conversion to i32 truncates toward
        // zero for both signs, so every coefficient loses magnitude and the
        // reconstructed irradiance comes out low. Unlike the sharing pass this
        // is not a feedback loop, so the error does not compound - but it is
        // free to remove.
        atomicAdd(&shRed[c], bitcast<u32>(
            i32(round(value.x * PROBE_SH_FIXED_SCALE))));
        atomicAdd(&shGreen[c], bitcast<u32>(
            i32(round(value.y * PROBE_SH_FIXED_SCALE))));
        atomicAdd(&shBlue[c], bitcast<u32>(
            i32(round(value.z * PROBE_SH_FIXED_SCALE))));
    }
}

@compute @workgroup_size(64, 1, 1)
fn resolveSparseVoxelProbes(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let probeIndex = workgroupId.x;
    let probeCount = max(
        settings.counts.x, settings.probeCounts.x * settings.probeCounts.y);
    let cellIndex = localIndex;
    if (localIndex < SPARSE_VOXEL_PROBE_SH_COUNT) {
        atomicStore(&shRed[localIndex], 0u);
        atomicStore(&shGreen[localIndex], 0u);
        atomicStore(&shBlue[localIndex], 0u);
        atomicStore(&eventSHRed[localIndex], 0u);
        atomicStore(&eventSHGreen[localIndex], 0u);
        atomicStore(&eventSHBlue[localIndex], 0u);
    }
    if (localIndex == 0u) {
        atomicStore(&tracedRed, 0u);
        atomicStore(&tracedGreen, 0u);
        atomicStore(&tracedBlue, 0u);
        atomicStore(&tracedCount, 0u);
        atomicStore(&eventLuminance, 0u);
        atomicStore(&eventCount, 0u);
        atomicStore(&mapCount, 0u);
    }
    workgroupBarrier();
    if (probeIndex >= probeCount || probeIndex >= arrayLength(&probes)) {
        return;
    }
    let probe = probes[probeIndex];
    let shBase = probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT;
    let cellBase = probeIndex * SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    if ((probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) {
        if (cellBase + cellIndex < arrayLength(&radiance)) {
            radiance[cellBase + cellIndex] =
                vec4<f32>(0.0, 0.0, 0.0, SPARSE_VOXEL_PROBE_NO_SAMPLE);
        }
        if (localIndex < SPARSE_VOXEL_PROBE_SH_COUNT &&
            shBase + localIndex < arrayLength(&probeSH)) {
            probeSH[shBase + localIndex] = vec4<f32>(0.0);
        }
        if (localIndex < SPARSE_VOXEL_PROBE_SH_COUNT &&
            shBase + localIndex < arrayLength(&eventSH)) {
            eventSH[shBase + localIndex] = vec4<f32>(0.0);
        }
        return;
    }

    // What the sharing pass left here: the reprojected neighbourhood estimate
    // for this cell, or the probe's mean with a negative distance when nothing
    // reached it.
    var sharedValue = vec4<f32>(0.0, 0.0, 0.0, SPARSE_VOXEL_PROBE_NO_SAMPLE);
    if (cellBase + cellIndex < arrayLength(&radiance)) {
        sharedValue = radiance[cellBase + cellIndex];
    }
    let currentSchedule = schedule[min(
        probeIndex, arrayLength(&schedule) - 1u)];
    let lightingChanged = sparse_voxel_probe_light_changed(
        currentSchedule.counters);
    let analyticalFarField = distance(
        probe.positionDepth.xyz,
        raster.cameraWorldPositionNear.xyz) >= settings.distances.w;

    // Whether this cell spent its ray on an emitter is read back off the ray
    // rather than recomputed. Once the ray count varies per probe the schedule
    // is no longer a function of the cell index alone, and the resolve has no
    // way to know a probe's tier - but the ray it produced says so directly,
    // and that is authoritative in a way a recomputed guess would not be.
    var cellWasNextEvent = false;

    // Pass one: read this frame's trace and publish the traced mean, which the
    // firefly clamp below needs before any cell commits its value.
    let rayIndex = cellBase + cellIndex;
    var traced = vec3<f32>(0.0);
    var tracedDistance = SPARSE_VOXEL_PROBE_SKY_DISTANCE;
    var tracedValid = false;
    var nextEvent = vec3<f32>(0.0);
    var nextEventDirection = vec3<f32>(0.0, 0.0, 1.0);
    var hasNextEvent = false;
    if (analyticalFarField) {
        // Deterministic quadrature of the analytic environment. These lanes do
        // no scene traversal, so every direction is intentionally unoccluded.
        let direction = sparse_voxel_probe_cell_direction(
            probe.normalRadius.xyz, cellIndex);
        traced = probe_miss_radiance(direction);
        tracedDistance = SPARSE_VOXEL_PROBE_SKY_DISTANCE;
        tracedValid = true;
        atomicAdd(&tracedRed, u32(clamp(
            traced.x * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
        atomicAdd(&tracedGreen, u32(clamp(
            traced.y * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
        atomicAdd(&tracedBlue, u32(clamp(
            traced.z * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
        atomicAdd(&tracedCount, 1u);
    } else if (rayIndex < arrayLength(&sceneRays) &&
        rayIndex < arrayLength(&sceneHits)) {
        let ray = sceneRays[rayIndex];
        if ((ray.seedFlags.y & SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u) {
            let direction = ray.directionMaximumDistance.xyz;
            let hit = sceneHits[rayIndex];
            let occluded = hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_HIT;
            // A traversal that reached the end of the volume it can see found
            // no occluder in anything the tree holds. Treating that as unknown
            // is what makes probes black once the camera is inside the scene.
            let reachedOpenSpace =
                hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_MISS_TMAX ||
                hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_SCENE ||
                hit.status == SPARSE_VOXEL_SCENE_RAY_STATUS_ESCAPED_TREE_VOLUME;
            if (occluded || reachedOpenSpace) {
                if (ray.seedFlags.w != 0u) {
                    cellWasNextEvent = true;
                    // Next-event slot: the emitter contribution was computed
                    // when the ray was generated and survives only if nothing
                    // blocked the path to the emitter. It is a contribution,
                    // not a radiance sample, so it never enters the map.
                    hasNextEvent = !occluded;
                    nextEvent = ray.lightSamplePdfWeight.xyz;
                    nextEventDirection = direction;
                    if (hasNextEvent) {
                        atomicAdd(&eventLuminance, u32(clamp(
                            sparse_voxel_probe_luminance(nextEvent) *
                                PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
                    }
                    // Occluded emitter samples count too: they are a real zero,
                    // and excluding them would raise the reference every time a
                    // probe is mostly shadowed - exactly when the clamp matters.
                    atomicAdd(&eventCount, 1u);
                } else {
                    tracedValid = true;
                    if (occluded) {
                        // Store the unweighted traced estimator. The current
                        // distance fade is applied after cell history settles,
                        // when this map is projected to SH below.
                        traced = vec3<f32>(0.0);
                        tracedDistance = max(hit.hitPositionDistance.w, 0.0);
                    } else {
                        traced = probe_miss_radiance(direction);
                        tracedDistance = SPARSE_VOXEL_PROBE_SKY_DISTANCE;
                    }
                    atomicAdd(&tracedRed, u32(clamp(
                        traced.x * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
                    atomicAdd(&tracedGreen, u32(clamp(
                        traced.y * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
                    atomicAdd(&tracedBlue, u32(clamp(
                        traced.z * PROBE_MEAN_FIXED_SCALE + 0.5, 0.0, 2.0e9)));
                    atomicAdd(&tracedCount, 1u);
                }
            }
        }
    }
    workgroupBarrier();

    let tracedSamples = atomicLoad(&tracedCount);
    var tracedMean = vec3<f32>(0.0);
    if (tracedSamples != 0u) {
        tracedMean = vec3<f32>(
            f32(atomicLoad(&tracedRed)),
            f32(atomicLoad(&tracedGreen)),
            f32(atomicLoad(&tracedBlue))) /
            (PROBE_MEAN_FIXED_SCALE * f32(tracedSamples));
    }

    // Pass two: settle this cell's radiance and project it.
    var cellValue = vec3<f32>(0.0);
    var cellDistance = SPARSE_VOXEL_PROBE_NO_SAMPLE;
    var cellKnown = false;
    if (tracedValid) {
        cellValue = traced;
        cellDistance = tracedDistance;
        if (sharedValue.w >= 0.0 && !lightingChanged &&
            !analyticalFarField) {
            // Both estimate the same direction, so this is a real temporal
            // blend rather than a spatial smear.
            //
            // The weight is 1/(n+1) over the probe's observations on this
            // surface. A fixed responsiveness floor leaves a converged probe
            // permanently jittering at its own noise floor. At the sleeping
            // tier it was especially destructive: scaling the 10% frame floor
            // by the eight-frame cell period made every revisited sky direction
            // replace 50% of its history, producing an eight-frame cycle of
            // bright and dark tiles under completely static lighting.
            //
            // Both terms are counted in *observations of this cell*, not in
            // frames. Those were the same thing while every probe traced all 64
            // directions every frame; once the ray count and the interval vary
            // they are not, and a cell measured once every 32 frames would
            // otherwise be blended as though it had been measured 32 times.
            // That is what lets a wrong value - a stale sky sample in a
            // direction that has since become occluded - survive for seconds as
            // a visible patch, because each rare correction moves it by a tenth
            // and the next one is 32 frames away.
            let period = f32(max(
                sparse_voxel_probe_cell_period(
                    settings.rayCounts, settings.intervals,
                    sparse_voxel_probe_last_tier(
                        schedule[min(
                            probeIndex,
                            arrayLength(&schedule) - 1u)].counters)),
                1u));
            let samples = f32(min(
                probe.identityState.w, SPARSE_VOXEL_PROBE_MAXIMUM_SAMPLES)) /
                period;
            // New probes still move quickly because their observation count is
            // small. Old static probes continue reducing their weight instead
            // of stopping at eight effective samples.
            let alpha = 1.0 / (samples + 1.0);
            cellValue = mix(sharedValue.xyz, traced, alpha);
        }
        // One cell that came back far brighter than the rest of the hemisphere
        // is not something a diffuse probe should carry at full amplitude,
        // however real it is.
        //
        // The reference has to describe the hemisphere, and the mean of the
        // cells traced *this frame* only does so while a probe traces most of
        // them. At a reduced ray count that mean can be a handful of
        // directions, and a probe that happened to draw only occluded ones gets
        // a reference near zero - which then clamps every genuinely lit cell to
        // black. That is a dark blotch that persists for as long as the probe
        // keeps drawing badly.
        //
        // Below that sample count the probe's own accumulated mean irradiance
        // stands in. It is the same quantity measured over many frames rather
        // than one, so it is exactly the stable reference this needs; the
        // conversion undoes the band-zero projection to recover a mean radiance
        // from the stored coefficient luminance.
        var reference = sparse_voxel_probe_luminance(tracedMean);
        if (tracedSamples < SPARSE_VOXEL_PROBE_FIREFLY_MINIMUM_SAMPLES) {
            let accumulated = schedule[min(
                probeIndex, arrayLength(&schedule) - 1u)].meanLuminance /
                (6.28318530718 * SPARSE_VOXEL_PROBE_SH_C0);
            reference = max(reference, accumulated);
        }
        // A probe with neither a usable traced mean nor any history yet has no
        // reference at all, and clamping against zero would erase everything it
        // just measured.
        if (reference > 1.0e-6 && !analyticalFarField) {
            cellValue = sparse_voxel_probe_clamp_firefly(
                cellValue, reference, settings.blend.z);
        }
        cellKnown = true;
    } else if (sharedValue.w >= 0.0 && !lightingChanged) {
        // No answer this frame - either the cell spent its ray on next-event
        // sampling or the trace was inconclusive. The shared directional
        // estimate is still valid for this direction.
        cellValue = sharedValue.xyz;
        cellDistance = sharedValue.w;
        cellKnown = true;
    } else if (cellWasNextEvent && tracedSamples != 0u) {
        // A next-event cell with no shared history still has to stand in for
        // the sky it did not measure, otherwise reserving cells for emitters
        // would dim every probe by the fraction of cells reserved. A cell that
        // simply was not traced this frame is not such a cell: it measured
        // nothing rather than measuring an emitter instead, and standing in for
        // the sky there would invent a sample the probe never took.
        cellValue = tracedMean;
        cellKnown = true;
    }
    if (cellKnown) { atomicAdd(&mapCount, 1u); }
    if (cellBase + cellIndex < arrayLength(&radiance)) {
        // The analytical far field is an output, not a traced prior. Publishing
        // it as radiance history would seed an unshadowed map when the camera
        // moves back inside the cutoff, even though the boundary invalidates
        // the lighting estimator. Leave it empty so the re-entering probe pays
        // for a real traced update instead.
        radiance[cellBase + cellIndex] = select(
            vec4<f32>(cellValue, cellDistance),
            vec4<f32>(0.0, 0.0, 0.0, SPARSE_VOXEL_PROBE_NO_SAMPLE),
            analyticalFarField);
    }
    workgroupBarrier();

    // The map term integrates only over the cells that know something, so the
    // measure has to be the hemisphere divided by that count, not by 64.
    // Dividing by 64 regardless is what darkens a probe in proportion to how
    // much of the tree failed to answer, which reads as a spreading shadow.
    let known = atomicLoad(&mapCount);
    let cellMeasure = 6.28318530718 / f32(max(known, 1u));
    if (cellKnown) {
        let direction = sparse_voxel_probe_cell_direction(
            probe.normalRadius.xyz, cellIndex);
        // `cellValue` is unweighted traced sky/visibility history. Evaluate
        // the analytical environment for this exact cell and apply the current
        // distance weight only now, so camera motion changes the handoff in one
        // frame without discarding or polluting the traced history.
        let traceWeight = probe_shadow_trace_weight(
            probe.positionDepth.xyz);
        var projectedCellValue = cellValue;
        // This branch is probe-uniform. Keep the ordinary near-field path from
        // scanning every light once per direction cell when no analytical
        // complement is needed.
        if (traceWeight < 1.0) {
            projectedCellValue = mix(
                probe_miss_radiance(direction), cellValue, traceWeight);
        }
        probe_accumulate_sh(
            projectedCellValue * cellMeasure,
            sparse_voxel_probe_sh_basis(direction));
    }
    // Next-event contributions are added on top of the map, not into it: they
    // already carry their own sampling density and are divided by the number of
    // cells that sampled them.
    if (hasNextEvent) {
        let events = atomicLoad(&eventCount);
        var clamped = nextEvent;
        if (events >= SPARSE_VOXEL_PROBE_FIREFLY_MINIMUM_SAMPLES) {
            let mean = f32(atomicLoad(&eventLuminance)) /
                (PROBE_MEAN_FIXED_SCALE * f32(events));
            clamped = sparse_voxel_probe_clamp_firefly(
                nextEvent, mean, settings.blend.z);
        }
        probe_accumulate_event_sh(
            clamped, sparse_voxel_probe_sh_basis(nextEventDirection));
    }
    workgroupBarrier();

    if (localIndex >= SPARSE_VOXEL_PROBE_SH_COUNT) { return; }
    if (shBase + localIndex >= arrayLength(&probeSH)) { return; }
    let mapCoefficient = vec3<f32>(
        f32(bitcast<i32>(atomicLoad(&shRed[localIndex]))),
        f32(bitcast<i32>(atomicLoad(&shGreen[localIndex]))),
        f32(bitcast<i32>(atomicLoad(&shBlue[localIndex])))) /
        PROBE_SH_FIXED_SCALE;
    let freshTracedEventCoefficient = vec3<f32>(
        f32(bitcast<i32>(atomicLoad(&eventSHRed[localIndex]))),
        f32(bitcast<i32>(atomicLoad(&eventSHGreen[localIndex]))),
        f32(bitcast<i32>(atomicLoad(&eventSHBlue[localIndex])))) /
        PROBE_SH_FIXED_SCALE;

    // Reproject the *unweighted traced* emitter history through the same
    // predecessor index as the probe itself. The w channel stores the light
    // signature bit-for-bit, so a light edit invalidates this term immediately
    // instead of blending two different lighting states into a pulse. The
    // camera-distance weight and analytical directional complement are applied
    // only after this accumulation.
    let historyIndex = probe.pixelFrameHistory.w;
    let hasHistory =
        (probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_REPROJECTED) != 0u;
    let previousIndex =
        historyIndex * SPARSE_VOXEL_PROBE_SH_COUNT + localIndex;
    let signature = currentSchedule.lightSignature;
    var accumulatedTracedEvent = freshTracedEventCoefficient;
    var tracedEventValid = atomicLoad(&eventCount) != 0u;
    if (!analyticalFarField && hasHistory &&
        previousIndex < arrayLength(&previousEventSH)) {
        let previousEvent = previousEventSH[previousIndex];
        let matchingLights = bitcast<u32>(previousEvent.w) == signature;
        if (matchingLights) {
            if (tracedEventValid) {
                let samples = f32(min(
                    probe.identityState.w,
                    SPARSE_VOXEL_PROBE_EVENT_MAXIMUM_SAMPLES));
                // This is a stochastic emitter estimate, not a directional
                // map cell. A responsiveness floor leaves a fixed fraction of
                // fresh light-selection noise in it forever (10% with the
                // default), so several static lights keep trading bright and
                // dark splats. Light edits already invalidate this history via
                // the signature above, while surface reprojection handles
                // motion; use the progressive weight without a permanent
                // noise floor.
                let alpha = 1.0 / (samples + 1.0);
                accumulatedTracedEvent = mix(
                    previousEvent.xyz, freshTracedEventCoefficient, alpha);
            } else if (probe_has_event_light(probe.positionDepth.xyz)) {
                // No emitter ray was usable this frame. Carry the last estimate
                // rather than blinking sunlight or a finite light off.
                accumulatedTracedEvent = previousEvent.xyz;
                tracedEventValid = true;
            }
        }
    }
    if (!probe_has_event_light(probe.positionDepth.xyz)) {
        accumulatedTracedEvent = vec3<f32>(0.0);
        tracedEventValid = false;
    }
    if (shBase + localIndex < arrayLength(&eventSH)) {
        // Persist only the unweighted traced estimator. This includes both
        // directional and finite emitters; their different far-field behavior
        // is an output transform below, not a history representation change.
        eventSH[shBase + localIndex] = vec4<f32>(
            accumulatedTracedEvent, bitcast<f32>(signature));
    }
    let traceWeight = probe_shadow_trace_weight(probe.positionDepth.xyz);
    let analyticalEventCoefficient = probe_analytical_event_coefficient(
        probe.positionDepth.xyz, probe.normalRadius.xyz, localIndex);
    // Every traced event fades with the current camera distance. Only the
    // directional sun has an analytical complement; finite/local emitters
    // intentionally reach zero rather than becoming unshadowed at a distance.
    let projectedEvent =
        accumulatedTracedEvent * traceWeight + analyticalEventCoefficient;
    let analyticalEventValid =
        dot(analyticalEventCoefficient, analyticalEventCoefficient) > 0.0;
    let eventValid = (tracedEventValid && traceWeight > 0.0) ||
        analyticalEventValid;
    let coefficient = mapCoefficient + projectedEvent;
    // The count channel is the probe's own confidence: how many of its 64
    // directions carry a usable sample. Zero means the probe learned nothing
    // this frame and has no history to fall back on.
    probeSH[shBase + localIndex] = vec4<f32>(
        coefficient,
        f32(max(known, select(0u, SPARSE_VOXEL_PROBE_DIRECTION_COUNT,
            eventValid))));

    // Close the scheduling loop. Only the ray pass can decide whether a probe
    // traces; only this pass can say whether tracing changed the answer.
    if (localIndex != 0u || probeIndex >= arrayLength(&schedule)) { return; }
    // A frame the probe sat out carries no new evidence: its map is the
    // previous one reprojected, and folding that in would let a probe
    // accumulate stability it never actually measured, which is precisely how
    // a sleeping probe would talk itself into sleeping longer.
    if (tracedSamples == 0u && atomicLoad(&eventCount) == 0u) { return; }
    // The band-zero coefficient is the probe's mean irradiance - the quantity
    // the interpolation actually reconstructs - so it is the right scalar to
    // watch converge. Individual cells are far noisier than the signal any
    // pixel ends up seeing, and thresholding on them would keep probes awake
    // long after their output had stopped moving.
    schedule[probeIndex] = sparse_voxel_probe_accumulate_convergence(
        schedule[probeIndex], sparse_voxel_probe_luminance(coefficient),
        probe.identityState.w, settings.thresholds, settings.blend.x);
}
