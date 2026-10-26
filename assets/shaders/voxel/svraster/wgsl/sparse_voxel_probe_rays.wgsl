#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_probe_common.wgsl"
#include "sparse_voxel_local_light_hash.wgsl"

// Probe ray generation.
//
// How much of its hemisphere a probe measures, and how often, is what this pass
// decides. Both scale with how settled the probe's own estimate is: an awake
// probe traces its full direction set every frame, a settled one traces a
// fraction of it, and the most settled also skip frames entirely.
//
// A reduced ray count is *not* an arbitrary subset of the directions. The
// budget is split explicitly between the stratified and next-event estimators
// by sparse_voxel_probe_cell_schedule, because both of the strides involved are
// powers of two and every natural way of picking N of 64 cells lands on a
// congruence class that is either exactly the next-event set or disjoint from
// it - measuring, respectively, no sky or no finite emitter at all.
//
// Between updates the sharing pass reprojects the radiance map forward, so a
// probe that traced only part of its hemisphere - or none of it - still tracks
// the camera; it just stops re-measuring directions its own history says are
// not changing. A probe wakes when its surface is new, when its estimate stops
// agreeing with itself, when it moves off the plane it was describing, or when
// one of the lights actually reaching it changes - the last of which is a
// per-probe test, so editing a lamp wakes the probes it illuminates and leaves
// the rest of the screen asleep.
//
// Direction slots are split between two estimators:
//   - stratified slots march their own hemi-octahedral cell and pick up sky on
//     a miss;
//   - next-event slots explicitly sample finite and directional emitters and
//     carry their contribution back along the sampled direction.
//
// A directional light must be in the second estimator. Treating it as a tiny
// cone in the sky sampler makes a reduced eight-ray probe miss the cone on most
// updates and occasionally inject radiance divided by its tiny solid angle.
// That is an unbiased estimator, but visually it is the periodic bright/dark
// blotch that motivated this split.
// Projecting both along the direction they actually travelled keeps the
// resulting SH field directionally consistent.
//
// The next-event slots are a *rotating* strided subset of the cells rather than
// a fixed tail. A fixed tail permanently blanks one contiguous region of every
// probe's hemisphere, so whatever sky or bounce lived in that region is never
// measured by any probe and no amount of accumulation recovers it.

struct SparseVoxelProbeRaySettings {
    // xy raster dimensions, z probe tile size, w frame index.
    dimensionsTile : vec4<u32>,
    // x probes across, y probes down, z light capacity, w next-event slots.
    probeCounts : vec4<u32>,
    // x maximum trace distance, y origin bias, z trace cell size, w how far a
    // probe may slide along its surface between frames - as a fraction of its
    // own world radius - before it is held awake.
    trace : vec4<f32>,
    // x probes in total, including the refinement overflow past the end of the
    // uniform grid. The grid dimensions alone no longer bound the probe array.
    counts : vec4<u32>,
    // Fallback directional light direction.
    lightDirection : vec4<f32>,
    // Frames between updates per tier: x awake, y converged, z asleep, w frames
    // a probe stays awake after its lights change. Every interval is clamped to
    // at least one.
    intervals : vec4<u32>,
    // Directions traced per update per tier: x awake, y converged, z asleep.
    // Together with the intervals these set a probe's amortised ray rate; the
    // ray count decides how much of the hemisphere one update covers, and the
    // interval decides how often an update happens at all.
    rayCounts : vec4<u32>,
    // x minimum traced updates before a probe may sleep, y maximum relative
    // standard error for an update to count as stable, z runs to reach the
    // converged tier, w runs to reach the asleep tier.
    thresholds : vec4<f32>,
    // xy distance-density transitions, z local-light fade start, w the point
    // beyond which traversal stops and resolve uses analytic unshadowed light.
    distances : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelProbeRaySettings;
@group(0) @binding(2) var<storage, read> probes : array<SparseVoxelProbe>;
@group(0) @binding(3) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(4) var<storage, read> lightBVHInfo : WorldGILightBVHInfo;
@group(0) @binding(5) var<storage, read_write> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(6) var<storage, read_write> activeRayIndices : array<u32>;
@group(0) @binding(7) var<storage, read_write> activeRayState : array<atomic<u32>>;
@group(0) @binding(8) var<storage, read> previousSchedule :
    array<SparseVoxelProbeSchedule>;
@group(0) @binding(9) var<storage, read_write> schedule :
    array<SparseVoxelProbeSchedule>;
@group(0) @binding(10) var<storage, read> previousProbes :
    array<SparseVoxelProbe>;
// Current-frame directional prior written by the sharing pass. Reading it here
// lets a new probe distinguish a true cold start from one already seeded by
// compatible probes around its screen tile.
@group(0) @binding(11) var<storage, read> radiance : array<vec4<f32>>;

fn probe_hash(value : u32) -> f32 {
    var bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    bits = bits * 277803737u;
    return f32((bits >> 8u) & 0x00ffffffu) / 16777216.0;
}

fn probe_light_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn probe_is_finite_light(light : WorldGILight) -> bool {
    return light.flags.y == WORLD_GI_LIGHT_TYPE_POINT ||
        light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE ||
        light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR;
}

fn probe_sample_directional_disk(
    light : WorldGILight, u : vec2<f32>) -> vec3<f32> {
    let axis = -normalize(light.directionType.xyz);
    let angularRadius = clamp(light.shapeParams.x, 0.0, 0.25);
    if (angularRadius <= 1.0e-6) { return axis; }
    let fallback = select(vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(1.0, 0.0, 0.0), abs(axis.y) > 0.95);
    let tangent = normalize(cross(fallback, axis));
    let bitangent = cross(axis, tangent);
    // Uniform solid-angle sample over the apparent solar disk. Rotating this
    // sample every probe update turns hard Tree64 cell silhouettes into a
    // stable penumbra through the existing temporal and spatial accumulation.
    let cosine = mix(1.0, cos(angularRadius), u.x);
    let sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    let phi = 6.28318530718 * u.y;
    return normalize(axis * cosine +
        (tangent * cos(phi) + bitangent * sin(phi)) * sine);
}

fn probe_has_directional_light() -> bool {
    let lightCount = min(lightBVHInfo.lightCount, arrayLength(&lights));
    for (var i = 0u; i < lightCount; i += 1u) {
        if (probe_light_enabled(lights[i]) &&
            lights[i].flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
            return true;
        }
    }
    return false;
}

fn probe_shadow_trace_weight(origin : vec3<f32>) -> f32 {
    let cameraDistance = distance(origin, raster.cameraWorldPositionNear.xyz);
    return 1.0 - smoothstep(
        settings.distances.z, settings.distances.w, cameraDistance);
}

// Cheap receiver-space estimate of how much a finite emitter can contribute.
// It is used only as a proposal density: probe_sample_finite_light still
// computes the actual sample and the selected density is divided out below.
// Guiding by power, distance and receiver orientation prevents a dim distant
// lamp from consuming as many scarce next-event rays as a nearby bright one.
fn probe_finite_light_importance(
    light : WorldGILight, origin : vec3<f32>, normal : vec3<f32>) -> f32 {
    let offset = light.positionRange.xyz - origin;
    let distance2 = max(dot(offset, offset), 1.0e-6);
    let travel = sqrt(distance2);
    var influenceDistance = max(light.positionRange.w, 0.0);
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        influenceDistance += max(light.shapeParams.x, 0.0);
    }
    if (light.flags.y != WORLD_GI_LIGHT_TYPE_PLANAR &&
        influenceDistance > 0.0 && travel > influenceDistance) {
        return 0.0;
    }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR &&
        distance(
            worldgi_planar_light_closest_point(light, origin), origin) >
            max(light.positionRange.w, 0.0)) {
        return 0.0;
    }
    let receiverCosine = max(dot(normal, offset / travel), 0.0);
    if (receiverCosine <= 0.0) { return 0.0; }

    var emittingArea = 1.0;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        let radius = max(light.shapeParams.x, 1.0e-4);
        emittingArea = 4.0 * 3.14159265359 * radius * radius;
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let halfExtent = max(light.shapeParams.yz, vec2<f32>(1.0e-4));
        emittingArea = 4.0 * halfExtent.x * halfExtent.y;
        let facing = normalize(light.directionType.xyz);
        emittingArea *= abs(dot(facing, offset / travel));
    }
    let power = sparse_voxel_probe_luminance(
        light.colorIntensity.xyz * light.colorIntensity.w);
    return max(power * emittingArea * receiverCosine / distance2, 0.0);
}

fn probe_event_light_importance(
    light : WorldGILight, origin : vec3<f32>, normal : vec3<f32>) -> f32 {
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let toLight = -normalize(light.directionType.xyz);
        let receiverCosine = max(dot(normal, toLight), 0.0);
        let power = sparse_voxel_probe_luminance(
            light.colorIntensity.xyz * light.colorIntensity.w);
        return power * receiverCosine;
    }
    return probe_finite_light_importance(light, origin, normal);
}

struct ProbeEmitterSample {
    direction : vec3<f32>,
    distance : f32,
    // Radiance the receiver would see from this direction if unoccluded,
    // already divided by the sampling density.
    radiance : vec3<f32>,
    valid : bool,
}

// Rank this lane among the next-event cells that actually trace this frame.
// Combined with a shared random rotation, the rank turns N independent light
// picks into one stratified walk over the guided light CDF. Every slot remains
// marginally distributed according to that CDF, while their union covers it
// far more evenly when only a few rays are available.
fn probe_next_event_ordinal(
    cellIndex : u32, traceCount : u32, slots : u32, frame : u32) -> u32 {
    var ordinal = 0u;
    for (var cell = 0u; cell < cellIndex; cell += 1u) {
        let candidate = sparse_voxel_probe_cell_schedule(
            cell, traceCount, slots, frame);
        if (candidate.traced && candidate.nextEvent) { ordinal += 1u; }
    }
    return ordinal;
}

fn probe_sample_event_light(
    light : WorldGILight, origin : vec3<f32>, normal : vec3<f32>,
    u : vec2<f32>) -> ProbeEmitterSample {
    let invalid = ProbeEmitterSample(
        vec3<f32>(0.0, 1.0, 0.0), 0.0, vec3<f32>(0.0), false);
    let colour = light.colorIntensity.xyz * light.colorIntensity.w;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let direction = probe_sample_directional_disk(light, u);
        if (dot(normal, direction) <= 0.0) { return invalid; }
        return ProbeEmitterSample(
            direction, max(settings.trace.x, 1.0e-3), colour, true);
    }
    var emitterPoint = light.positionRange.xyz;
    var areaPdf = 1.0;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        // Uniform point on the emitter sphere. Cheap and unbiased; the probe
        // averages many of these across its slots.
        let radius = max(light.shapeParams.x, 1.0e-4);
        let z = 1.0 - 2.0 * u.x;
        let r = sqrt(max(1.0 - z * z, 0.0));
        let phi = 6.28318530718 * u.y;
        emitterPoint = light.positionRange.xyz +
            vec3<f32>(r * cos(phi), r * sin(phi), z) * radius;
        areaPdf = 1.0 / max(4.0 * 3.14159265359 * radius * radius, 1.0e-6);
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        // shapeParams.yz carries the quad's half extents along the basis the
        // shared helper derives from the light's forward direction. Not .xy:
        // slot x is the sphere radius and is left zero for a quad, so reading
        // the pair from x shrinks one extent to the 1e-4 floor. The emitter is
        // then sampled along a hairline instead of over its area, and because
        // the area also divides the sampling density the light's contribution
        // is scaled down by the ratio of the real area to that sliver - a
        // 300x300 quad ends up some six orders of magnitude too dim, which
        // reads as the light having no effect at all.
        let basis = worldgi_planar_light_basis(light);
        let halfExtent = max(light.shapeParams.yz, vec2<f32>(1.0e-4));
        emitterPoint = light.positionRange.xyz +
            basis.tangent * (u.x * 2.0 - 1.0) * halfExtent.x +
            basis.bitangent * (u.y * 2.0 - 1.0) * halfExtent.y;
        areaPdf = 1.0 / max(
            4.0 * halfExtent.x * halfExtent.y, 1.0e-6);
    }
    let offset = emitterPoint - origin;
    let distance2 = dot(offset, offset);
    if (distance2 <= 1.0e-8) { return invalid; }
    let travel = sqrt(distance2);
    let direction = offset / travel;
    let cosine = dot(normal, direction);
    if (cosine <= 0.0) { return invalid; }
    // Point emitters have no area; their density is already a solid angle.
    var radiance = colour / distance2;
    if (light.flags.y != WORLD_GI_LIGHT_TYPE_POINT) {
        // Area sampling: the emitter's own foreshortening belongs in the
        // estimator. Leaving it out makes a quad seen edge-on as bright as one
        // seen face-on, which is a large and very visible error at grazing
        // angles rather than a subtlety.
        var emitterCosine = 1.0;
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
            let facing = normalize(light.directionType.xyz);
            emitterCosine = abs(dot(facing, direction));
        }
        radiance = colour * emitterCosine / (areaPdf * distance2);
    }
    let range = light.positionRange.w;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT && range > 0.0 &&
        travel > range) {
        return invalid;
    }
    return ProbeEmitterSample(direction, travel, radiance, true);
}

// Decide how many directions this probe traces this frame, and publish its
// scheduling state for the next one. Run by a single lane per probe. Returns
// zero when this frame is not the probe's turn.
//
// The state is carried through the probe's *history* index rather than its own,
// because a refined probe's index is reallocated by a race every frame and
// means nothing across frames - the same reason the placement pass carries its
// sample count that way.
fn probe_schedule_update(
    probeIndex : u32, probe : SparseVoxelProbe,
    seededCellCount : u32) -> u32 {
    let historyIndex = probe.pixelFrameHistory.w;
    let carried =
        (probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_REPROJECTED) != 0u &&
        historyIndex < arrayLength(&previousSchedule);
    var previous = sparse_voxel_probe_empty_schedule();
    if (carried) { previous = previousSchedule[historyIndex]; }

    // Only the lights whose influence volume actually contains this probe enter
    // the signature, so editing one lamp perturbs the probes it reaches and
    // nothing else. A light that never changes never changes its revision and
    // so never changes this value: static lights cost nothing to ignore.
    var signature = sparse_voxel_local_light_hash(
        probe.positionDepth.xyz, settings.probeCounts.z);
    // Crossing the analytical boundary changes the estimator even though no
    // scene light record changed. Mix that state into the signature so traced
    // event history cannot linger after visibility has been intentionally
    // replaced by the unshadowed analytical term.
    if (probe_shadow_trace_weight(probe.positionDepth.xyz) <= 0.0) {
        signature = sparse_voxel_probe_hash(signature ^ 0xa511e9b3u);
        if (signature == 0u) { signature = 1u; }
    }
    // A stored zero is an uninitialised slot rather than a different set of
    // lights. Reading it as a change would wake every probe on the frame after
    // a resize, and would keep waking probes that had just been reprojected.
    let signatureChanged = carried && previous.lightSignature != 0u &&
        previous.lightSignature != signature;

    // How far the probe moved *off the plane it was describing* since the frame
    // it reprojected from, measured against the footprint it serves.
    //
    // Convergence alone is not enough to let a probe sleep. The statistics only
    // compare traced frames, and once a probe is skipping fifteen of every
    // sixteen those comparisons are far enough apart that the running mean
    // absorbs a steady drift and still reports the probe as stable - so a probe
    // that has moved onto different geometry can talk itself into sleeping, and
    // every time its turn finally comes round the answer jumps. That is the
    // flicker a temporal filter is worst at hiding, and it is why the per-pixel
    // scheduler gates on motion separately too.
    //
    // Only the component along the probe's normal counts. Placement re-elects
    // the probe's pixel inside its tile every frame, so a probe slides across
    // its own surface constantly even under a completely static camera; that
    // shake is how it integrates its tile, and the sharing pass reprojects
    // through the hit point specifically so an in-plane slide stays valid.
    // Measuring raw displacement instead makes the jitter indistinguishable
    // from real motion, and the gate then holds every probe awake forever.
    // Leaving the plane is the thing that means this probe now describes
    // somewhere else.
    var moved = 0.0;
    if (carried && historyIndex < arrayLength(&previousProbes)) {
        let offset = probe.positionDepth.xyz -
            previousProbes[historyIndex].positionDepth.xyz;
        moved = abs(dot(offset, probe.normalRadius.xyz)) /
            max(probe.normalRadius.w, 1.0e-4);
    }
    let settling = moved > max(settings.trace.w, 0.0);

    var tier = sparse_voxel_probe_tier(
        previous, probe.identityState.w, carried, signatureChanged,
        settings.thresholds);
    if (settling) { tier = SPARSE_VOXEL_PROBE_TIER_AWAKE; }
    let interval = max(
        sparse_voxel_probe_tier_value(settings.intervals, tier), 1u);
    var rayCount = min(
        sparse_voxel_probe_tier_value(settings.rayCounts, tier),
        SPARSE_VOXEL_PROBE_DIRECTION_COUNT);
    // A new probe normally needs the full awake budget because unmeasured
    // cells are black. When sharing filled a useful fraction of the map from
    // neighbours, those cells are already real directional samples; spend the
    // seeded budget validating and completing that prior instead of tracing
    // the whole hemisphere again. Eight valid cells is deliberately
    // conservative enough to reject an isolated reprojection splat.
    //
    // A carried probe that only entered the awake tier through the motion gate
    // has an even stronger prior: every valid cell was reprojected from the
    // same surface. Camera motion can keep most of the screen in this state,
    // so paying the cold-start 64-ray burst here makes traversal cost scale
    // with camera motion even though no lighting history was lost. Rotate the
    // same seeded-sized validation slice while it moves; a genuinely new probe
    // with no usable neighbours still retains the full cold-start budget.
    // A carried probe can still have no usable prior: the analytical far field
    // deliberately publishes NO_SAMPLE so crossing back inside the cutoff
    // cannot inherit unshadowed cells. Do not mistake that empty reprojection
    // for a motion-validating prior and reduce its cold-start budget. A light
    // signature change invalidates even populated seeds, so it likewise keeps
    // the full awake budget.
    if (!signatureChanged && seededCellCount >= 8u &&
        (settling || !carried)) {
        rayCount = min(rayCount, max(settings.rayCounts.w, 1u));
    }

    var runs = sparse_voxel_probe_stable_runs(previous.counters);
    var wake = sparse_voxel_probe_wake_frames(previous.counters);
    var mean = previous.meanLuminance;
    var secondMoment = previous.secondMomentLuminance;
    if (!carried) {
        runs = 0u;
        wake = 0u;
        mean = 0.0;
        secondMoment = 0.0;
    }
    if (signatureChanged) {
        wake = settings.intervals.w;
        runs = 0u;
        // The old statistics describe lighting that no longer exists. Keeping
        // them would make the convergence test compare each new measurement
        // against the previous scene and read the difference as instability for
        // as long as the mean took to catch up.
        mean = 0.0;
        secondMoment = 0.0;
    } else if (wake != 0u) {
        // Waking is spent in frames rather than in traced updates, so a change
        // that arrived while the probe was off phase still expires on schedule
        // instead of pinning it awake until its turn came round.
        wake -= 1u;
    }
    // A probe that is still moving has to earn its way back down the tiers. Its
    // statistics describe the surface patches it has been sliding across, not
    // the one it will settle on, so whatever run length they had accumulated is
    // not evidence about where it is now.
    if (settling) { runs = 0u; }

    let traces = sparse_voxel_probe_traces_this_frame(
        probe.pixelFrameHistory.xy / max(settings.dimensionsTile.z, 1u),
        interval, settings.dimensionsTile.w);
    if (probeIndex < arrayLength(&schedule)) {
        var packedCounters = sparse_voxel_probe_pack_counters(
            runs, wake, tier);
        if (signatureChanged) {
            packedCounters |= SPARSE_VOXEL_PROBE_LIGHT_CHANGED;
        }
        schedule[probeIndex] = SparseVoxelProbeSchedule(
            signature, mean, secondMoment,
            packedCounters);
    }
    return select(0u, rayCount, traces);
}

// The compact index range this workgroup's probe reserved, broadcast from the
// lane that performed the allocation.
var<workgroup> probeRayBase : u32;
// Directions this probe's tier grants it this frame, zero when its turn has not
// come up. Every lane in the workgroup belongs to one probe, so this stays
// uniform and the early outs below leave the barriers well formed.
var<workgroup> probeRayCount : u32;
// Lanes that actually claimed a slot. The reservation is made from this rather
// than from the granted count, so the compact list can never contain a slot no
// lane wrote - which would hand the tracer a stale ray.
var<workgroup> probeClaimed : atomic<u32>;
// Valid directional cells the sharing pass supplied to this probe. This is a
// workgroup reduction because the scheduling decision is made once per probe.
var<workgroup> probeSeededCellCount : atomic<u32>;

@compute @workgroup_size(64, 1, 1)
fn createSparseVoxelProbeRays(
    @builtin(global_invocation_id) gid : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let probeCount = max(
        settings.counts.x, settings.probeCounts.x * settings.probeCounts.y);
    let rayCount = probeCount * SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    let rayIndex = gid.x;
    let probeIndex = rayIndex / SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    let directionIndex = rayIndex % SPARSE_VOXEL_PROBE_DIRECTION_COUNT;
    let inBounds = rayIndex < rayCount && rayIndex < arrayLength(&rays) &&
        probeIndex < arrayLength(&probes);
    var probe : SparseVoxelProbe;
    var probeValid = false;
    if (inBounds) {
        probe = probes[probeIndex];
        probeValid =
            (probe.identityState.z & SPARSE_VOXEL_PROBE_STATE_VALID) != 0u;
        if (!probeValid) {
            // The flag is the only thing downstream uses to decide whether a
            // canonical slot belongs to this frame, so an unspawned probe's
            // slots have to be cleared even though they are never listed.
            rays[rayIndex].seedFlags.y = 0u;
        }
    }

    // A probe's direction count equals the workgroup size, so one workgroup is
    // exactly one probe and every lane in it agrees on probeValid. That lets a
    // single lane reserve the whole probe's range with one atomic.
    //
    // The count cannot be derived on lane zero from probeCount alone: probes
    // that found no surface emit no rays, and lane zero belongs to probe zero,
    // which is itself frequently unspawned. Writing the state there left it at
    // whatever the reset pass put down - zero - so the compaction dispatched no
    // groups and every probe resolved black.
    if (localIndex == 0u) {
        probeRayBase = 0xffffffffu;
        probeRayCount = 0u;
        atomicStore(&probeClaimed, 0u);
        atomicStore(&probeSeededCellCount, 0u);
    }
    workgroupBarrier();
    if (!probeValid) { return; }

    if (rayIndex < arrayLength(&radiance) && radiance[rayIndex].w >= 0.0) {
        atomicAdd(&probeSeededCellCount, 1u);
    }
    workgroupBarrier();
    if (localIndex == 0u) {
        probeRayCount = probe_schedule_update(
            probeIndex, probe, atomicLoad(&probeSeededCellCount));
        // Past the analytical cutoff, resolve projects the environment and
        // directional emitters without visibility. Keep scheduling state alive
        // for light signatures and reprojection, but reserve no traversal work.
        if (distance(
                probe.positionDepth.xyz,
                raster.cameraWorldPositionNear.xyz) >= settings.distances.w) {
            probeRayCount = 0u;
        }
    }
    workgroupBarrier();

    if (probeRayCount == 0u) {
        // The probe either sits this scheduled frame out or is past the
        // analytical cutoff. Retire every canonical slot before avoiding the
        // light scan, cell scheduling, atomics, and remaining barriers below.
        rays[rayIndex].seedFlags.y = 0u;
        return;
    }

    let frame = settings.dimensionsTile.w;
    let configuredNextEventSlots = min(
        settings.probeCounts.w, SPARSE_VOXEL_PROBE_DIRECTION_COUNT / 2u);
    // Sunlight remains explicitly sampled below the analytical cutoff.
    // One reserved class is sufficient to keep it deterministic; the regular
    // configured budget still controls finite-emitter quality.
    let nextEventSlots = max(
        configuredNextEventSlots,
        select(0u, 1u, probe_has_directional_light()));
    // Which directions this probe measures this frame, and which estimator each
    // one serves. At the full ray count this reduces exactly to the original
    // rotating next-event stride.
    let cellSchedule = sparse_voxel_probe_cell_schedule(
        directionIndex, probeRayCount, nextEventSlots, frame);
    var compactSlot = 0xffffffffu;
    if (cellSchedule.traced) { compactSlot = atomicAdd(&probeClaimed, 1u); }
    workgroupBarrier();

    // Reserve exactly what was claimed. Deriving the reservation from the
    // granted count instead would leave unwritten tail slots in the compact
    // list whenever the class budgets round down, and the tracer would consume
    // whatever the previous frame left in them.
    if (localIndex == 0u) {
        let claimed = atomicLoad(&probeClaimed);
        if (claimed != 0u) {
            probeRayBase = atomicAdd(&activeRayState[3], claimed);
            // Reservations are no longer whole workgroups, so a workgroup can
            // span two probes. That is fine: the compaction indexes the active
            // list directly and never assumes a group belongs to one probe.
            atomicMax(
                &activeRayState[0], (probeRayBase + claimed + 63u) / 64u);
        }
    }
    workgroupBarrier();

    if (compactSlot == 0xffffffffu) {
        // Retire the canonical slot. The resolve reads rays by probe and cell
        // rather than through the active list, so a stale ACTIVE flag here
        // would pair this frame's cell with the previous frame's hit. Cleared,
        // the cell falls through to the shared value the sharing pass
        // reprojected into it, which is exactly the intent.
        rays[rayIndex].seedFlags.y = 0u;
        return;
    }

    let normal = probe.normalRadius.xyz;
    let receiverOrigin = probe.positionDepth.xyz;
    let seed = (probeIndex * 0x9e3779b9u) ^
        (directionIndex * 0x85ebca6bu) ^ (frame * 0xc2b2ae35u);
    let isNextEvent = cellSchedule.nextEvent;

    var direction = vec3<f32>(0.0, 1.0, 0.0);
    var maximumDistance = max(settings.trace.x, 1.0e-3);
    var radiance = vec3<f32>(0.0);
    var lightIndex = 0xffffffffu;
    var valid = false;

    if (isNextEvent) {
        // Reserve one deterministic sample for the directional light, then
        // build a receiver-local CDF for finite emitters. Letting the sun enter
        // the same lottery as point lights makes the two-ray sleeping tier
        // randomly gain or lose its dominant contribution every frame.
        //
        // The finite-light CDF is intentionally independent of the light BVH
        // ordering: stable light data gives a stable density, and the estimator
        // divides the density back out so the result remains unbiased as lights
        // move or change power.
        let lightCount = min(lightBVHInfo.lightCount, arrayLength(&lights));
        var directionalLightIndex = 0xffffffffu;
        var totalImportance = 0.0;
        var eligibleLightCount = 0u;
        for (var i = 0u; i < lightCount; i += 1u) {
            if (!probe_light_enabled(lights[i])) { continue; }
            let importance = probe_event_light_importance(
                lights[i], receiverOrigin, normal);
            if (importance <= 0.0) { continue; }
            if (lights[i].flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL &&
                directionalLightIndex == 0xffffffffu) {
                directionalLightIndex = i;
            } else if (probe_is_finite_light(lights[i])) {
                totalImportance += importance;
                eligibleLightCount += 1u;
            }
        }
        var selectedSamplingWeight = 0.0;
        var estimatorSlots = 0u;
        var selectionPdf = 1.0;
        let tileCoord = probe.pixelFrameHistory.xy /
            max(settings.dimensionsTile.z, 1u);
        let ordinal = probe_next_event_ordinal(
            directionIndex, probeRayCount, nextEventSlots, frame);
        let slotCount = max(cellSchedule.nextEventCount, 1u);
        let directionalSlots = select(
            0u, 1u, directionalLightIndex != 0xffffffffu);
        if (directionalSlots != 0u && ordinal == 0u) {
            lightIndex = directionalLightIndex;
            estimatorSlots = 1u;
        } else if (totalImportance > 1.0e-8 &&
            eligibleLightCount != 0u && slotCount > directionalSlots) {
            // Pure importance sampling gives a very small proposal density to
            // a weak or distant light. When that light is eventually selected,
            // 1/pdf turns its single shadow result into the bright splat seen
            // with several emitters. Mix the guided proposal with 50% uniform
            // exploration. The mixture still sums to totalImportance, remains
            // unbiased, and bounds the selection weight to 2 * lightCount.
            let uniformImportance =
                totalImportance / f32(eligibleLightCount);
            let finiteSlotCount = slotCount - directionalSlots;
            let finiteOrdinal = ordinal - directionalSlots;
            let rotation = probe_hash(
                (tileCoord.x * 0x6c8e9cf5u) ^
                (tileCoord.y * 0xb5297a4du) ^
                (frame * 0x1b56c4e9u));
            let cdfSample = fract(
                (f32(finiteOrdinal) + 0.5) / f32(finiteSlotCount) +
                    rotation);
            let cdfTarget = cdfSample * totalImportance;
            var accumulated = 0.0;
            for (var i = 0u; i < lightCount; i += 1u) {
                if (!probe_light_enabled(lights[i]) ||
                    !probe_is_finite_light(lights[i])) { continue; }
                let importance = probe_event_light_importance(
                    lights[i], receiverOrigin, normal);
                if (importance <= 0.0) { continue; }
                let samplingWeight =
                    0.5 * (importance + uniformImportance);
                accumulated += samplingWeight;
                if (cdfTarget <= accumulated || i + 1u == lightCount) {
                    lightIndex = i;
                    selectedSamplingWeight = samplingWeight;
                    break;
                }
            }
            estimatorSlots = finiteSlotCount;
            selectionPdf = max(
                selectedSamplingWeight / totalImportance, 1.0e-8);
        }
        if (lightIndex != 0xffffffffu && estimatorSlots != 0u) {
            let emitter = probe_sample_event_light(
                lights[lightIndex], receiverOrigin, normal,
                vec2<f32>(
                    probe_hash(seed ^ 0x1b56c4e9u),
                    probe_hash(seed ^ 0x68bc21ebu)));
            if (emitter.valid) {
                direction = emitter.direction;
                // Stop just short of the emitter so it does not occlude itself,
                // and scale by the light-selection density.
                maximumDistance = select(
                    max(emitter.distance * 0.999, 1.0e-4),
                    emitter.distance,
                    lights[lightIndex].flags.y ==
                        WORLD_GI_LIGHT_TYPE_DIRECTIONAL);
                // What travels on the ray is the emitter's *contribution*,
                // already divided by the number of slots assigned to its
                // estimator, so resolve can add it to the SH directly. It is
                // not a radiance sample and must not be scaled by a cell's
                // solid angle the way the stratified cells are.
                //
                // The receiver cosine is deliberately absent: the cosine lobe
                // is applied once, at interpolation. Applying it here as well
                // is what turns a light close to a surface into a firefly.
                // Divide by both proposal densities: the slots that sample a
                // finite emitter this frame and its mixed guided probability.
                // The directional estimator has one reserved slot and pdf one.
                // Keep this estimator unweighted in temporal history. Resolve
                // applies the current camera-distance trace weight *after*
                // accumulation, adds the analytical directional complement,
                // and leaves finite/local emitters without a complement so
                // they fade to zero. Baking the weight into this sample makes
                // a moving probe carry yesterday's distance for up to the full
                // event-history window and then pop at the hard cutoff.
                radiance = emitter.radiance /
                    (f32(estimatorSlots) * selectionPdf);
                valid = true;
            }
        }
    }

    if (!valid) {
        // Stratified hemi-octahedral direction in the probe's own frame. The
        // whole cell grid maps to the hemisphere above the surface, so every
        // one of the 64 cells is a usable direction and the cells all subtend
        // the same solid angle. Sampling the full sphere and folding the lower
        // half back with a reflection - the obvious alternative - spends half
        // the rays on directions that can only return the receiver itself and
        // destroys the equal-measure property the estimator relies on.
        // The jitter phase is keyed on the probe's tile, not on its index: a
        // refined probe's index is reallocated every frame and would otherwise
        // scramble its own direction sequence.
        let tileCoord = probe.pixelFrameHistory.xy /
            max(settings.dimensionsTile.z, 1u);
        direction = sparse_voxel_probe_direction(
            normal, directionIndex,
            sparse_voxel_probe_tile_phase(tileCoord, frame),
            probeIndex, frame);
        // Sky radiance is resolved from the direction on a miss, so the ray
        // itself only needs to answer the visibility question.
        radiance = vec3<f32>(0.0);
        lightIndex = 0xffffffffu;
        valid = true;
    }

    // Probe and per-pixel rays share the exact reconstructed receiver origin.
    // Moving this point with the sampled direction makes static geometry
    // produce a different shadow origin every frame.
    let origin = receiverOrigin;
    rays[rayIndex] = SparseVoxelSceneRay(
        // The same conservative minimum distance per-pixel rays carry. The
        // screen tracer runs for probe rays too and reconstructs its own start
        // from the probe's source pixel, so this bounds only the world tracer.
        vec4<f32>(origin, 0.0),
        vec4<f32>(direction, maximumDistance),
        vec4<u32>(
            probe.pixelFrameHistory.x, probe.pixelFrameHistory.y,
            directionIndex, select(0u, lightIndex, lightIndex != 0xffffffffu)),
        vec4<u32>(
            seed,
            SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE |
                SPARSE_VOXEL_SCENE_RAY_FLAG_SHADOW |
                SPARSE_VOXEL_SCENE_RAY_FLAG_PROBE,
            probeIndex,
            select(0u, 1u, isNextEvent && lightIndex != 0xffffffffu)),
        // The next-event radiance is carried on the ray so the resolve pass
        // does not have to re-sample the emitter to know what the unoccluded
        // path would have delivered.
        vec4<f32>(radiance, 1.0));

    // Publishing the list here is what lets the existing Tree64 trace and hit
    // merge chain run unchanged: they consume only activeRayIndices and the
    // count, so probe mode needs no worklist build pass of its own. The list
    // stays dense - a workgroup's 64 slots are contiguous and entirely active -
    // which is the invariant the compaction's per-workgroup sort relies on.
    let activeIndex = probeRayBase + compactSlot;
    if (probeRayBase != 0xffffffffu &&
        activeIndex < arrayLength(&activeRayIndices)) {
        activeRayIndices[activeIndex] = rayIndex;
    }
}
