// Builds this frame's probe rays for the world irradiance cache, in the
// Tree64 tracer's own ray format, so the existing trace pass consumes them
// unchanged. Nothing about tracing is reimplemented here: the cache gets the
// dynamic layer, the material geometry and every traversal fix for free.
#include "worldgi_ddgi_common.wgsl"

struct WorldGIDDGITree64Ray {
    originMaximumDistance : vec4<f32>,
    direction : vec4<f32>,
}

@group(0) @binding(0) var<uniform> settings : WorldGIDDGISettings;
@group(0) @binding(1) var<storage, read> volumes : array<WorldGIDDGIVolume>;
@group(0) @binding(2) var<storage, read_write> rays : array<WorldGIDDGITree64Ray>;
// Shared with the update kernel, which owns advancing it.
@group(0) @binding(3) var<storage, read> schedule : array<u32>;

@compute @workgroup_size(64, 1, 1)
fn generateWorldGIDDGIRays(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let rayCount = max(settings.cascadeRaysProbes.y, 1u);
    let probesThisFrame = settings.cascadeRaysProbes.z;
    let totalRays = probesThisFrame * rayCount;
    let index = globalId.x;
    if (index >= totalRays || index >= arrayLength(&rays)) {
        return;
    }

    let batchProbe = index / rayCount;
    let rayIndex = index % rayCount;
    let totalProbes = max(settings.cascadeRaysProbes.w, 1u);
    // Probes are refreshed round robin from a rolling cursor, so every probe is
    // revisited on a fixed period regardless of what the camera does.
    let cursor = select(0u, schedule[0], arrayLength(&schedule) > 0u);
    let probeSlot = (cursor + batchProbe) % totalProbes;

    // Which cascade owns this slot. Cascades are contiguous, so a linear walk
    // over at most four volumes resolves it without a division per probe.
    var cascade = 0u;
    var localSlot = probeSlot;
    let cascadeCount = min(settings.cascadeRaysProbes.x, WORLDGI_DDGI_MAX_CASCADES);
    for (var candidate = 0u; candidate < cascadeCount; candidate = candidate + 1u) {
        let volume = volumes[candidate];
        let probes = volume.dimensionsCascade.x * volume.dimensionsCascade.y *
            volume.dimensionsCascade.z;
        if (localSlot < probes) {
            cascade = candidate;
            break;
        }
        localSlot = localSlot - probes;
        cascade = candidate + 1u;
    }
    if (cascade >= cascadeCount) {
        return;
    }

    let volume = volumes[cascade];
    let dimensions = volume.dimensionsCascade.xyz;
    let physical = vec3<u32>(
        localSlot % dimensions.x,
        (localSlot / dimensions.x) % dimensions.y,
        localSlot / (dimensions.x * dimensions.y));
    // The slot holds whichever logical probe the scroll maps to it. Getting
    // this inverse wrong makes a probe trace one position and store into
    // another, which reads as stable-but-wrong lighting rather than noise.
    let logical = worldGIDDGILogicalFromPhysical(
        physical, volume.scrollOffsetGeneration.xyz, dimensions);
    let origin = worldGIDDGIProbePosition(volume, logical);

    let rotationSeed = worldGIDDGIHash(
        cursor * 9781u + select(0u, schedule[1], arrayLength(&schedule) > 1u));
    let rotation = vec2<f32>(
        f32(rotationSeed & 0xffffu) / 65535.0 * 6.2831853,
        f32((rotationSeed >> 16u) & 0xffffu) / 65535.0 * 6.2831853);
    // Ray zero is the sun. A 32-ray uniform sphere essentially never lands
    // inside a sun disc of realistic angular size, so sampling the sun
    // stochastically captured none of it - the cache came out 46x darker than
    // the screen probes it was meant to replace, because outdoor irradiance is
    // sun dominated. One dedicated ray answers "is the sun visible from this
    // probe", and the update kernel adds the sun analytically.
    var direction = worldGIDDGIRayDirection(rayIndex, rayCount, rotation);
    if (rayIndex == 0u) {
        direction = normalize(settings.sunDirectionIntensity.xyz);
    }

    var ray : WorldGIDDGITree64Ray;
    ray.originMaximumDistance = vec4<f32>(
        origin, max(settings.blendTuning.y, 0.0));
    // The slot this ray was generated for travels with the ray. The update
    // kernel then needs no shared cursor at all: passes disagreed about the
    // value of that scalar - the advance pass saw it advance while the refresh
    // pass read zero - and only the first batch of probes ever retained data.
    ray.direction = vec4<f32>(direction, f32(probeSlot));
    rays[index] = ray;
}
