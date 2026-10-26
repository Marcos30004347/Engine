#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGIShadowProbeFillUniforms {
    inverseViewProjection : mat4x4<f32>,
    previousViewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    probeGrid : vec2<u32>,
    controls : vec4<u32>,
    filterParams : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGIShadowProbeFillUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(3) var<storage, read> shadowProbes : array<WorldGIShadowProbe>;
@group(0) @binding(4) var rawOutputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var filteredOutputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(6) var guideOutputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var outputDepthHistoryTexture : texture_storage_2d<r32float, write>;
@group(0) @binding(8) var outputNormalHistoryTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(9) var<storage, read> shadowProbeDirections : array<vec4<f32>>;
@group(0) @binding(10) var<storage, read> shadowReservoirs : array<WorldGIShadowLightReservoir>;
@group(0) @binding(11) var previousFilteredTexture : texture_2d<f32>;
@group(0) @binding(12) var previousDepthHistoryTexture : texture_2d<f32>;
@group(0) @binding(13) var previousNormalHistoryTexture : texture_2d<f32>;

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, uniforms.controls.z != 0u);
}

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn oct_decode_unorm(encodedUnorm : vec2<f32>) -> vec3<f32> {
    let encoded = encodedUnorm * 2.0 - vec2<f32>(1.0);
    var direction = vec3<f32>(encoded.x, encoded.y, 1.0 - abs(encoded.x) - abs(encoded.y));
    let wrapped = (vec2<f32>(1.0) - abs(direction.yx)) * select(vec2<f32>(-1.0), vec2<f32>(1.0), direction.xy >= vec2<f32>(0.0));
    direction.x = select(wrapped.x, direction.x, direction.z >= 0.0);
    direction.y = select(wrapped.y, direction.y, direction.z >= 0.0);
    return safe_normalize(direction, vec3<f32>(0.0, 0.0, 1.0));
}

fn reconstruct_world_position(pixelCoord : vec2<u32>, depthValue : f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixelCoord) + vec2<f32>(0.5)) / vec2<f32>(uniforms.viewport);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = uniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn probe_is_valid(probe : WorldGIShadowProbe) -> bool {
    return (probe.pixelLightFlags.w & WORLD_GI_SHADOW_PROBE_FLAG_VALID) != 0u && probe.normalConfidence.w > 0.0;
}

fn probe_visibility(probe : WorldGIShadowProbe) -> f32 {
    return clamp(probe.visibilityVarianceHistory.x, 0.0, 1.0);
}

fn probe_lighting(probe : WorldGIShadowProbe) -> vec3<f32> {
    return max(probe.lightRadiancePdf.rgb, vec3<f32>(0.0)) * probe_visibility(probe);
}

fn probe_index_at_tile(tile : vec2<i32>) -> u32 {
    if (tile.x < 0 || tile.y < 0 || tile.x >= i32(uniforms.probeGrid.x) || tile.y >= i32(uniforms.probeGrid.y)) {
        return 0xffffffffu;
    }
    let index = u32(tile.y) * uniforms.probeGrid.x + u32(tile.x);
    return select(index, 0xffffffffu, index >= arrayLength(&shadowProbes));
}

fn invalid_probe() -> WorldGIShadowProbe {
    return WorldGIShadowProbe(vec4<f32>(0.0), vec4<f32>(0.0, 1.0, 0.0, 0.0), vec4<u32>(0u), vec4<f32>(0.0, 1.0, 0.0, 0.0), vec4<f32>(0.0, 0.0, 0.0, 1.0), vec4<f32>(0.0));
}

fn probe_at_index(index : u32) -> WorldGIShadowProbe {
    if (index == 0xffffffffu || index >= arrayLength(&shadowProbes)) {
        return invalid_probe();
    }
    return shadowProbes[index];
}

fn probe_at_tile(tile : vec2<i32>) -> WorldGIShadowProbe {
    let index = probe_index_at_tile(tile);
    return probe_at_index(index);
}

struct ProbeDirectionalLighting {
    radiance : vec3<f32>,
    sampleWeight : f32,
};

fn probe_directional_lighting_estimate(probeIndex : u32) -> ProbeDirectionalLighting {
    if (probeIndex == 0xffffffffu) {
        return ProbeDirectionalLighting(vec3<f32>(0.0), 0.0);
    }
    let baseIndex = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT;
    var radiance = vec3<f32>(0.0);
    var sampleWeight = 0.0;
    for (var texel = 0u; texel < WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT; texel = texel + 1u) {
        let directionIndex = baseIndex + texel;
        if (directionIndex < arrayLength(&shadowProbeDirections)) {
            let sample = shadowProbeDirections[directionIndex];
            let confidence = clamp(sample.a / WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT, 0.0, 1.0);
            let energy = dot(sample.rgb, sample.rgb);
            if (confidence > 0.0 || energy > 1.0e-6) {
                let stableWeight = max(confidence, select(0.0, 0.05, energy > 1.0e-6));
                radiance = radiance + max(sample.rgb, vec3<f32>(0.0)) * stableWeight;
                sampleWeight = sampleWeight + stableWeight;
            }
        }
    }
    return ProbeDirectionalLighting(radiance, sampleWeight);
}

fn probe_directional_lighting(probeIndex : u32, fallbackProbe : WorldGIShadowProbe) -> vec3<f32> {
    let estimate = probe_directional_lighting_estimate(probeIndex);
    return select(probe_lighting(fallbackProbe), estimate.radiance / max(estimate.sampleWeight, 1.0e-5), estimate.sampleWeight > 0.0);
}

struct ProbeReservoirLighting {
    radiance : vec3<f32>,
    sampleCount : f32,
};

fn probe_reservoir_lighting_direct(probeIndex : u32, pixelNormal : vec3<f32>) -> ProbeReservoirLighting {
    if (probeIndex == 0xffffffffu) {
        return ProbeReservoirLighting(vec3<f32>(0.0), 0.0);
    }

    let baseIndex = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;
    var radiance = vec3<f32>(0.0);
    var acceptedLightIds = vec4<u32>(0xffffffffu);
    var acceptedCount = 0u;
    var sampleCount = 0.0;

    for (var lane = 0u; lane < WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE; lane = lane + 1u) {
        let reservoirIndex = baseIndex + lane;
        if (reservoirIndex >= arrayLength(&shadowReservoirs)) {
            continue;
        }

        let reservoir = shadowReservoirs[reservoirIndex];
        if (!reservoir_is_valid(reservoir)) {
            continue;
        }

        let lightId = reservoir.lightSampleFlags.x;
        var duplicate = false;
        for (var acceptedIndex = 0u; acceptedIndex < acceptedCount; acceptedIndex = acceptedIndex + 1u) {
            duplicate = duplicate || acceptedLightIds[acceptedIndex] == lightId;
        }
        if (duplicate) {
            continue;
        }

        let sampleDirection = safe_normalize(reservoir.sampleDirectionDistance.xyz, pixelNormal);
        let receiverTerm = max(dot(pixelNormal, sampleDirection), 0.0);
        let laneRadiance = max(reservoir.radianceVisibility.rgb, vec3<f32>(0.0)) * receiverTerm;

        radiance = radiance + laneRadiance;
        if (acceptedCount < 4u) {
            acceptedLightIds[acceptedCount] = lightId;
            acceptedCount = acceptedCount + 1u;
        }
        sampleCount = sampleCount + 1.0;
    }

    return ProbeReservoirLighting(radiance, sampleCount);
}

fn probe_reservoir_lighting(probeIndex : u32, fallbackProbe : WorldGIShadowProbe, pixelNormal : vec3<f32>) -> vec3<f32> {
    let directional = probe_directional_lighting_estimate(probeIndex);
    let estimate = probe_reservoir_lighting_direct(probeIndex, pixelNormal);
    if (estimate.sampleCount > 0.0) {
        if (directional.sampleWeight > 0.10) {
            let directionalLighting = directional.radiance / max(directional.sampleWeight, 1.0e-5);
            let historyWeight = clamp(directional.sampleWeight / (directional.sampleWeight + estimate.sampleCount), 0.0, 0.60);
            return mix(estimate.radiance, directionalLighting, historyWeight);
        }
        return estimate.radiance;
    }
    return select(probe_lighting(fallbackProbe), directional.radiance / max(directional.sampleWeight, 1.0e-5), directional.sampleWeight > 0.0);
}

fn probe_direction_texel(probeIndex : u32, texel : vec2<u32>) -> vec4<f32> {
    if (probeIndex == 0xffffffffu) {
        return vec4<f32>(0.0);
    }
    let clampedTexel = min(texel, vec2<u32>(WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION - 1u));
    let directionIndex = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT + clampedTexel.y * WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION + clampedTexel.x;
    if (directionIndex >= arrayLength(&shadowProbeDirections)) {
        return vec4<f32>(0.0);
    }
    return shadowProbeDirections[directionIndex];
}

fn hash_debug_color(value : u32) -> vec3<f32> {
    let x = f32(value);
    let r = fract(sin(x * 12.9898 + 0.1234) * 43758.5453);
    let g = fract(sin(x * 78.2330 + 1.2345) * 24634.6345);
    let b = fract(sin(x * 39.4250 + 2.3456) * 56445.2344);
    return vec3<f32>(r, g, b);
}

fn reservoir_is_valid(reservoir : WorldGIShadowLightReservoir) -> bool {
    return (reservoir.lightSampleFlags.w & WORLD_GI_SHADOW_RESERVOIR_FLAG_VALID) != 0u &&
        reservoir.lightSampleFlags.x != 0xffffffffu;
}

fn light_type_debug_color(lightType : u32) -> vec3<f32> {
    if (lightType == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        return vec3<f32>(0.18, 0.45, 1.0);
    }
    if (lightType == WORLD_GI_LIGHT_TYPE_POINT) {
        return vec3<f32>(1.0, 0.58, 0.12);
    }
    return vec3<f32>(0.65, 0.30, 0.95);
}

fn shadow_lane_debug(probeIndex : u32, localPixel : vec2<u32>, tileSize : u32) -> vec4<f32> {
    if (probeIndex == 0xffffffffu) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let clampedTileSize = max(tileSize, 1u);
    let laneCoord = min((localPixel * vec2<u32>(2u)) / vec2<u32>(clampedTileSize), vec2<u32>(1u));
    let laneIndex = laneCoord.y * 2u + laneCoord.x;
    let reservoirIndex = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE + laneIndex;
    if (laneIndex >= WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE || reservoirIndex >= arrayLength(&shadowReservoirs)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let reservoir = shadowReservoirs[reservoirIndex];
    if (!reservoir_is_valid(reservoir)) {
        return vec4<f32>(0.015, 0.015, 0.020, 1.0);
    }

    let lightId = reservoir.lightSampleFlags.x;
    let lightType = reservoir.lightSampleFlags.y;
    let candidateCount = f32(reservoir.lightSampleFlags.z);
    let selectedPdf = clamp(reservoir.weightTargetConfidence.z, 0.0, 1.0);
    let visibility = clamp(reservoir.radianceVisibility.a, 0.0, 1.0);
    let radiance = max(reservoir.radianceVisibility.rgb, vec3<f32>(0.0));
    let luma = dot(radiance, vec3<f32>(0.2126, 0.7152, 0.0722));
    let energy = clamp(luma / (1.0 + luma), 0.0, 1.0);
    let typeColor = light_type_debug_color(lightType);
    let idTint = hash_debug_color(lightId + 1u);
    let selectedColor = mix(typeColor, idTint, 0.28);
    let blockedColor = vec3<f32>(0.62, 0.045, 0.025);
    var color = mix(blockedColor, selectedColor, visibility);
    color = color * (0.18 + 0.52 * max(energy, selectedPdf) + 0.20 * clamp(candidateCount / 8.0, 0.0, 1.0));

    let localInLane = localPixel - laneCoord * vec2<u32>((clampedTileSize + 1u) / 2u);
    let edge = localPixel.x == 0u || localPixel.y == 0u ||
        localPixel.x + 1u >= clampedTileSize || localPixel.y + 1u >= clampedTileSize ||
        localInLane.x == 0u || localInLane.y == 0u;
    color = select(color, vec3<f32>(0.0), edge);
    return vec4<f32>(color, 1.0);
}

fn packed_probe_coord(tile : vec2<i32>) -> u32 {
    let index = probe_index_at_tile(tile);
    if (index == 0xffffffffu) {
        return 0xffffffffu;
    }
    return u32(tile.x) | (u32(tile.y) << 16u);
}

fn unpack_probe_coord(packed : u32) -> vec2<i32> {
    return vec2<i32>(i32(packed & 0xffffu), i32((packed >> 16u) & 0xffffu));
}

fn world_position_for_pixel_or_current(pixel : vec2<u32>, fallbackDepth : f32, fallbackWorldPosition : vec3<f32>) -> vec3<f32> {
    let clampedPixel = min(pixel, uniforms.viewport - vec2<u32>(1u));
    let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(clampedPixel), 0);
    if (!valid_depth(depthValue)) {
        return fallbackWorldPosition;
    }
    return reconstruct_world_position(clampedPixel, depthValue);
}

fn interpolation_eps(pixel : vec2<u32>, depthValue : f32, worldPosition : vec3<f32>) -> f32 {
    let tileSize = max(uniforms.controls.w, 1u);
    let halfTile = max(tileSize / 2u, 1u);
    let px = min(pixel + vec2<u32>(halfTile, 0u), uniforms.viewport - vec2<u32>(1u));
    let py = min(pixel + vec2<u32>(0u, halfTile), uniforms.viewport - vec2<u32>(1u));
    let wx = world_position_for_pixel_or_current(px, depthValue, worldPosition);
    let wy = world_position_for_pixel_or_current(py, depthValue, worldPosition);
    let footprint = max(length(wx - worldPosition), length(wy - worldPosition));
    return max(max(footprint * 12.0, uniforms.filterParams.x * 32.0), 2.00);
}

fn probe_weight(pixel : vec2<u32>, pixelNormal : vec3<f32>, pixelWorldPosition : vec3<f32>, epsSize : f32, probeTile : vec2<i32>, probe : WorldGIShadowProbe) -> f32 {
    if (!probe_is_valid(probe)) {
        return 0.0;
    }
    let tileSize = f32(max(uniforms.controls.w, 1u));
    let probeGridPosition = (vec2<f32>(pixel) + vec2<f32>(0.5)) / tileSize - vec2<f32>(0.5);
    let tileDelta = vec2<f32>(probeTile) - probeGridPosition;
    let gridSigma = 2.25;
    let gridTerm = exp(-dot(tileDelta, tileDelta) / (2.0 * gridSigma * gridSigma));
    let probeNormal = safe_normalize(probe.normalConfidence.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let toProbe = probe.worldPositionDepth.xyz - pixelWorldPosition;
    let planeDistance = abs(dot(toProbe, pixelNormal));
    let worldDistance = length(toProbe);
    let normalTerm = clamp(dot(pixelNormal, probeNormal), 0.0, 1.0);
    let distanceTerm = exp(-worldDistance / max(epsSize * 2.0, 1.0e-4));
    let screenDelta = vec2<f32>(pixel) - vec2<f32>(f32(probe.pixelLightFlags.x), f32(probe.pixelLightFlags.y));
    let screenSigma = max(uniforms.filterParams.z, 1.0);
    let screenTerm = 1.0 / (1.0 + dot(screenDelta, screenDelta) / screenSigma);
    let planeTerm = exp(-planeDistance / max(epsSize, 1.0e-4));
    return gridTerm * pow(normalTerm, max(uniforms.filterParams.y * 0.03125, 0.25)) * distanceTerm * planeTerm * mix(0.75, 1.0, screenTerm);
}

struct TemporalShadowHistory {
    lighting : vec3<f32>,
    sampleCount : f32,
    valid : bool,
};

fn invalid_temporal_shadow_history() -> TemporalShadowHistory {
    return TemporalShadowHistory(vec3<f32>(0.0), 0.0, false);
}

fn reproject_history_pixel(worldPosition : vec3<f32>) -> vec4<f32> {
    let previousClip = uniforms.previousViewProjection * vec4<f32>(worldPosition, 1.0);
    if (abs(previousClip.w) <= 1.0e-6) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    let previousNdc = previousClip.xyz / previousClip.w;
    let previousUv = vec2<f32>(previousNdc.x * 0.5 + 0.5, 0.5 - previousNdc.y * 0.5);
    if (any(previousUv < vec2<f32>(0.0)) || any(previousUv >= vec2<f32>(1.0))) {
        return vec4<f32>(0.0, 0.0, previousNdc.z, 0.0);
    }

    let previousPixel = floor(previousUv * vec2<f32>(uniforms.viewport));
    return vec4<f32>(previousPixel, previousNdc.z, 1.0);
}

fn load_temporal_shadow_history(historyCoord : vec2<i32>, previousProjectedDepth : f32, currentNormal : vec3<f32>) -> TemporalShadowHistory {
    let previousLightingCount = textureLoad(previousFilteredTexture, historyCoord, 0);
    let previousDepth = textureLoad(previousDepthHistoryTexture, historyCoord, 0).x;
    let previousNormalRaw = textureLoad(previousNormalHistoryTexture, historyCoord, 0);
    let previousNormal = safe_normalize(previousNormalRaw.xyz, currentNormal);
    let previousSampleCount = clamp(previousLightingCount.a, 0.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT);
    let hasHistory = previousSampleCount > 0.5;
    let depthTolerance = max(0.0005, abs(previousProjectedDepth) * 0.0010);
    let depthMatches = valid_depth(previousDepth) && abs(previousDepth - previousProjectedDepth) <= depthTolerance;
    let normalMatches = previousNormalRaw.a > 0.0 && dot(previousNormal, currentNormal) > 0.96;
    let finiteLighting = all(previousLightingCount.rgb == previousLightingCount.rgb);
    let valid = hasHistory && depthMatches && normalMatches && finiteLighting;
    return TemporalShadowHistory(max(previousLightingCount.rgb, vec3<f32>(0.0)), previousSampleCount, valid);
}

fn clamp_history_to_current(historyLighting : vec3<f32>, currentLighting : vec3<f32>) -> vec3<f32> {
    let currentLuma = dot(currentLighting, vec3<f32>(0.2126, 0.7152, 0.0722));
    let historyLuma = dot(historyLighting, vec3<f32>(0.2126, 0.7152, 0.0722));
    let lumaLimit = max(currentLuma * 2.50 + 0.10, 0.10);
    let scale = select(1.0, lumaLimit / max(historyLuma, 1.0e-5), historyLuma > lumaLimit);
    return max(historyLighting * scale, vec3<f32>(0.0));
}

fn resolve_temporal_shadow(currentLighting : vec3<f32>, currentValid : bool, history : TemporalShadowHistory) -> vec4<f32> {
    if (!currentValid) {
        let decayedCount = max(history.sampleCount - 4.0, 0.0);
        let decayedLighting = history.lighting * clamp(decayedCount / max(history.sampleCount, 1.0), 0.0, 1.0);
        return select(vec4<f32>(0.0), vec4<f32>(decayedLighting, decayedCount), history.valid);
    }

    if (!history.valid) {
        return vec4<f32>(currentLighting, 1.0);
    }

    let historySampleCount = clamp(history.sampleCount, 1.0, 24.0);
    let clampedHistory = clamp_history_to_current(history.lighting, currentLighting);
    let currentLuma = dot(currentLighting, vec3<f32>(0.2126, 0.7152, 0.0722));
    let historyLuma = dot(clampedHistory, vec3<f32>(0.2126, 0.7152, 0.0722));
    let lumaDelta = abs(currentLuma - historyLuma);
    let reactiveWeight = clamp(lumaDelta / max(max(currentLuma, historyLuma), 0.10), 0.0, 1.0) * 0.65;
    let temporalWeight = max(1.0 / (historySampleCount + 1.0), reactiveWeight);
    let lighting = mix(clampedHistory, currentLighting, temporalWeight);
    return vec4<f32>(max(lighting, vec3<f32>(0.0)), min(historySampleCount + 1.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT));
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.viewport.x || gid.y >= uniforms.viewport.y) {
        return;
    }

    let pixel = gid.xy;
    let pixelCoord = vec2<i32>(i32(pixel.x), i32(pixel.y));
    let depthValue = textureLoad(sceneDepthTexture, pixelCoord, 0);
    let normal = safe_normalize(textureLoad(geometryNormalTexture, pixelCoord, 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    textureStore(outputDepthHistoryTexture, pixelCoord, vec4<f32>(depthValue, 0.0, 0.0, 0.0));
    textureStore(outputNormalHistoryTexture, pixelCoord, vec4<f32>(normal, 1.0));

    if (uniforms.controls.x == 0u || !valid_depth(depthValue)) {
        textureStore(rawOutputTexture, pixelCoord, vec4<f32>(0.0));
        textureStore(filteredOutputTexture, pixelCoord, vec4<f32>(0.0));
        textureStore(guideOutputTexture, pixelCoord, vec4<f32>(0.0));
        return;
    }

    let tileSize = max(uniforms.controls.w, 1u);
    let tile = min(pixel / vec2<u32>(tileSize), uniforms.probeGrid - vec2<u32>(1u));
    let centerTile = vec2<i32>(i32(tile.x), i32(tile.y));
    let centerProbeIndex = probe_index_at_tile(centerTile);
    let centerProbe = probe_at_index(centerProbeIndex);
    let rawValid = probe_is_valid(centerProbe);
    let localPixelForRawProbe = pixel - tile * vec2<u32>(tileSize);
    let rawOctTexel = min(vec2<u32>((vec2<f32>(localPixelForRawProbe) + vec2<f32>(0.5)) * f32(WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION) / f32(tileSize)), vec2<u32>(WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION - 1u));
    let rawDirectionSample = probe_direction_texel(centerProbeIndex, rawOctTexel);
    let rawDirection = oct_decode_unorm((vec2<f32>(rawOctTexel) + vec2<f32>(0.5)) / f32(WORLD_GI_SHADOW_PROBE_OCT_RESOLUTION));
    let rawDirectionBasis = rawDirection * vec3<f32>(0.5) + vec3<f32>(0.5);
    let rawRadiance = max(rawDirectionSample.rgb, vec3<f32>(0.0));
    let rawLuma = dot(rawRadiance, vec3<f32>(0.2126, 0.7152, 0.0722));
    let rawSamplePresent = rawDirectionSample.a > 0.0 || rawLuma > 1.0e-6;
    let rawToneMappedRadiance = rawRadiance / (vec3<f32>(1.0) + vec3<f32>(rawLuma));
    let rawProbeDebug = select(rawDirectionBasis * 0.03, rawToneMappedRadiance * (rawDirectionBasis * 0.75 + vec3<f32>(0.25)), rawSamplePresent);
    textureStore(rawOutputTexture, pixelCoord, vec4<f32>(select(vec3<f32>(0.0), rawProbeDebug, rawValid), select(0.0, 1.0, rawValid)));

    let worldPosition = reconstruct_world_position(pixel, depthValue);
    let epsSize = interpolation_eps(pixel, depthValue, worldPosition);
    var lightingSum = vec3<f32>(0.0);
    var weightSum = 0.0;

    for (var offsetY = -3; offsetY <= 3; offsetY = offsetY + 1) {
        for (var offsetX = -3; offsetX <= 3; offsetX = offsetX + 1) {
            let probeTile = centerTile + vec2<i32>(offsetX, offsetY);
            let probeIndex = probe_index_at_tile(probeTile);
            let probe = probe_at_index(probeIndex);
            let weight = probe_weight(pixel, normal, worldPosition, epsSize, probeTile, probe);
            if (weight <= 1.0e-5) {
                continue;
            }
            lightingSum = lightingSum + probe_reservoir_lighting(probeIndex, probe, normal) * weight;
            weightSum = weightSum + weight;
        }
    }

    let filteredValid = weightSum > 1.0e-5;
    let filteredLighting = select(vec3<f32>(0.0), lightingSum / max(weightSum, 1.0e-5), filteredValid);
    let historyPixelDepthValid = reproject_history_pixel(worldPosition);
    var temporalHistory = invalid_temporal_shadow_history();
    if (historyPixelDepthValid.w > 0.0) {
        temporalHistory = load_temporal_shadow_history(vec2<i32>(historyPixelDepthValid.xy), historyPixelDepthValid.z, normal);
    }
    let temporalFilteredLighting = resolve_temporal_shadow(filteredLighting, filteredValid, temporalHistory);
    textureStore(filteredOutputTexture, pixelCoord, temporalFilteredLighting);
    let localPixel = pixel - tile * vec2<u32>(tileSize);
    textureStore(guideOutputTexture, pixelCoord, select(vec4<f32>(0.0, 0.0, 0.0, 1.0), shadow_lane_debug(centerProbeIndex, localPixel, tileSize), rawValid));
}
