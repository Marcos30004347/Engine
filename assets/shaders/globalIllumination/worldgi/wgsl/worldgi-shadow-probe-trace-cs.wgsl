#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGIShadowProbeTraceUniforms {
    probeGrid : vec2<u32>,
    controls : vec2<u32>,
    frameIndex : u32,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
};

struct SoftwareTraceRay {
    originTMin : vec4<f32>,
    directionTMax : vec4<f32>,
};

struct SoftwareTraceHit {
    instanceId : u32,
    triangleId : u32,
    hitT : f32,
    baryU : f32,
    baryV : f32,
    tlasInstanceIndex : u32,
    flags : u32,
    _padding0 : u32,
};

struct ScreenTraceHit {
    metadata : vec4<u32>,
    hit : vec4<f32>,
};

const SOFTWARE_TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;
const SCREEN_TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;

@group(0) @binding(0) var<uniform> uniforms : WorldGIShadowProbeTraceUniforms;
@group(0) @binding(1) var<storage, read_write> shadowProbes : array<WorldGIShadowProbe>;
@group(0) @binding(2) var<storage, read> softwareShadowRays : array<SoftwareTraceRay>;
@group(0) @binding(3) var<storage, read> softwareShadowHits : array<SoftwareTraceHit>;
@group(0) @binding(4) var<storage, read_write> shadowProbeDirections : array<vec4<f32>>;
@group(0) @binding(5) var<storage, read_write> shadowReservoirs : array<WorldGIShadowLightReservoir>;
@group(0) @binding(6) var<storage, read> screenShadowHits : array<ScreenTraceHit>;

fn swrt_ray_is_active(ray : SoftwareTraceRay) -> bool {
    return dot(ray.directionTMax.xyz, ray.directionTMax.xyz) > 1.0e-8 && ray.directionTMax.w > ray.originTMin.w;
}

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn oct_wrap(value : vec2<f32>) -> vec2<f32> {
    return (vec2<f32>(1.0) - abs(value.yx)) * select(vec2<f32>(-1.0), vec2<f32>(1.0), value.xy >= vec2<f32>(0.0));
}

fn oct_encode_unorm(directionInput : vec3<f32>) -> vec2<f32> {
    var direction = safe_normalize(directionInput, vec3<f32>(0.0, 1.0, 0.0));
    direction = direction / max(abs(direction.x) + abs(direction.y) + abs(direction.z), 1.0e-6);
    var encoded = direction.xy;
    if (direction.z < 0.0) {
        encoded = oct_wrap(encoded);
    }
    return encoded * 0.5 + vec2<f32>(0.5);
}

fn oct_texel_index(direction : vec3<f32>) -> u32 {
    let encoded = clamp(oct_encode_unorm(direction), vec2<f32>(0.0), vec2<f32>(0.999999));
    let texel = vec2<u32>(encoded * f32(WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION));
    return min(texel.y, WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION - 1u) * WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION +
        min(texel.x, WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION - 1u);
}

fn reservoir_is_valid(reservoir : WorldGIShadowLightReservoir) -> bool {
    return (reservoir.lightSampleFlags.w & WORLD_GI_SHADOW_RESERVOIR_FLAG_VALID) != 0u &&
        reservoir.lightSampleFlags.x != 0xffffffffu;
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let probeCount = uniforms.probeGrid.x * uniforms.probeGrid.y;
    let rayIndex = gid.x;
    let probeIndex = rayIndex / WORLD_GI_SHADOW_RAYS_PER_PROBE;
    let sampleIndex = rayIndex - probeIndex * WORLD_GI_SHADOW_RAYS_PER_PROBE;
    if (probeIndex >= probeCount || probeIndex >= arrayLength(&shadowProbes)) {
        return;
    }

    var probe = shadowProbes[probeIndex];
    let screenBlocked = rayIndex < arrayLength(&screenShadowHits) && (screenShadowHits[rayIndex].metadata.x & SCREEN_TRACE_HIT_FLAG_HIT) != 0u;
    if (uniforms.controls.x == 0u ||
        sampleIndex >= uniforms.controls.y ||
        rayIndex >= probeCount * WORLD_GI_SHADOW_RAYS_PER_PROBE ||
        rayIndex >= arrayLength(&softwareShadowRays) ||
        rayIndex >= arrayLength(&softwareShadowHits) ||
        (!screenBlocked && !swrt_ray_is_active(softwareShadowRays[rayIndex]))) {
        return;
    }
    let reservoirIndex = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE + sampleIndex;
    if (reservoirIndex >= arrayLength(&shadowReservoirs)) {
        return;
    }
    var reservoir = shadowReservoirs[reservoirIndex];
    if (!reservoir_is_valid(reservoir)) {
        return;
    }

    let hit = softwareShadowHits[rayIndex];
    let blocked = screenBlocked || (hit.flags & SOFTWARE_TRACE_HIT_FLAG_HIT) != 0u;
    let visibility = select(1.0, 0.0, blocked);
    probe.normalConfidence.w = 1.0;
    probe.visibilityVarianceHistory = vec4<f32>(max(probe.visibilityVarianceHistory.x, visibility), 0.0, 1.0, f32(uniforms.frameIndex));
    shadowProbes[probeIndex] = probe;

    let directionTexel = oct_texel_index(softwareShadowRays[rayIndex].directionTMax.xyz);
    let directionIndex = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT + directionTexel;
    let unoccludedRadiance = max(reservoir.radianceVisibility.rgb, vec3<f32>(0.0));
    let currentRadiance = unoccludedRadiance * visibility;
    let currentLuma = dot(currentRadiance, vec3<f32>(0.2126, 0.7152, 0.0722));

    var accumulatedRadiance = currentRadiance;
    var accumulatedSampleCount = 1.0;
    if (directionIndex < arrayLength(&shadowProbeDirections)) {
        let history = shadowProbeDirections[directionIndex];
        let historySampleCount = clamp(history.a, 0.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT - 1.0);
        let historyRadiance = max(history.rgb, vec3<f32>(0.0));
        let historyLuma = dot(historyRadiance, vec3<f32>(0.2126, 0.7152, 0.0722));
        let lumaDelta = abs(currentLuma - historyLuma);
        let reactive = clamp(lumaDelta / max(max(currentLuma, historyLuma), 0.10), 0.0, 1.0);
        let darkHistory = historyLuma < 1.0e-4 && currentLuma > 1.0e-4;
        let effectiveHistoryCount = min(historySampleCount * (1.0 - reactive * 0.25), select(32.0, 16.0, darkHistory));
        accumulatedSampleCount = min(effectiveHistoryCount + 1.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT);
        let historyWeight = effectiveHistoryCount / max(accumulatedSampleCount, 1.0);
        accumulatedRadiance = mix(currentRadiance, historyRadiance, historyWeight);
    }

    reservoir.radianceVisibility = vec4<f32>(accumulatedRadiance, visibility);
    reservoir.weightTargetConfidence.w = clamp(accumulatedSampleCount / WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT, 0.0, 1.0);
    shadowReservoirs[reservoirIndex] = reservoir;

    if (directionIndex < arrayLength(&shadowProbeDirections)) {
        shadowProbeDirections[directionIndex] = vec4<f32>(accumulatedRadiance, accumulatedSampleCount);
    }
}
