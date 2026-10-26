#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGIShadowProbeSpawnUniforms {
    inverseViewProjection : mat4x4<f32>,
    previousViewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    probeGrid : vec2<u32>,
    controls : vec4<u32>,
    historyControls : vec4<u32>,
    traceParams : vec4<f32>,
};

struct SoftwareTraceRay {
    originTMin : vec4<f32>,
    directionTMax : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGIShadowProbeSpawnUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(3) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(4) var<storage, read> lightBVHNodes : array<WorldGILightBVHNode>;
@group(0) @binding(5) var<storage, read> lightBVHInfo : WorldGILightBVHInfo;
@group(0) @binding(6) var<storage, read_write> shadowProbes : array<WorldGIShadowProbe>;
@group(0) @binding(7) var<storage, read_write> softwareShadowRays : array<SoftwareTraceRay>;
@group(0) @binding(8) var<storage, read_write> shadowProbeDirections : array<vec4<f32>>;
@group(0) @binding(9) var<storage, read_write> shadowReservoirs : array<WorldGIShadowLightReservoir>;
@group(0) @binding(10) var<storage, read> shadowProbeDirectionHistory : array<vec4<f32>>;
@group(0) @binding(11) var<storage, read> shadowReservoirHistory : array<WorldGIShadowLightReservoir>;

fn invalid_probe() -> WorldGIShadowProbe {
    return WorldGIShadowProbe(
        vec4<f32>(0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<u32>(0u, 0u, 0xffffffffu, 0u),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0, 0.0, 0.0, 1.0),
        vec4<f32>(0.0));
}

fn invalid_software_ray() -> SoftwareTraceRay {
    return SoftwareTraceRay(vec4<f32>(0.0), vec4<f32>(0.0));
}

fn hash_u32(value : u32) -> u32 {
    var state = value * 747796405u + 2891336453u;
    var word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

fn random_01(seed : u32) -> f32 {
    return f32(hash_u32(seed) & 0x00ffffffu) / 16777216.0;
}

fn invalid_reservoir() -> WorldGIShadowLightReservoir {
    return WorldGIShadowLightReservoir(
        vec4<u32>(0xffffffffu, 0u, 0u, 0u),
        vec4<f32>(0.0, 0.0, 1.0, 0.0),
        vec4<f32>(0.0),
        vec4<f32>(0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0));
}

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, uniforms.controls.z != 0u);
}

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn reconstruct_world_position(pixelCoord : vec2<u32>, depthValue : f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixelCoord) + vec2<f32>(0.5)) / vec2<f32>(uniforms.viewport);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = uniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn project_previous_pixel(worldPosition : vec3<f32>) -> vec2<i32> {
    let projected = uniforms.previousViewProjection * vec4<f32>(worldPosition, 1.0);
    if (abs(projected.w) <= 1.0e-6) {
        return vec2<i32>(-1);
    }
    let ndc = projected.xyz / projected.w;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    if (any(uv < vec2<f32>(0.0)) || any(uv >= vec2<f32>(1.0))) {
        return vec2<i32>(-1);
    }
    return vec2<i32>(floor(uv * vec2<f32>(uniforms.viewport)));
}

const BLUE_NOISE_8X8_RANKS : array<u32, 64> = array<u32, 64>(
    0u, 48u, 12u, 60u, 3u, 51u, 15u, 63u,
    32u, 16u, 44u, 28u, 35u, 19u, 47u, 31u,
    8u, 56u, 4u, 52u, 11u, 59u, 7u, 55u,
    40u, 24u, 36u, 20u, 43u, 27u, 39u, 23u,
    2u, 50u, 14u, 62u, 1u, 49u, 13u, 61u,
    34u, 18u, 46u, 30u, 33u, 17u, 45u, 29u,
    10u, 58u, 6u, 54u, 9u, 57u, 5u, 53u,
    42u, 26u, 38u, 22u, 41u, 25u, 37u, 21u);

fn radical_inverse_vdc(bitsInput : u32) -> f32 {
    var bits = bitsInput;
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xaaaaaaaau) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xccccccccu) >> 2u);
    bits = ((bits & 0x0f0f0f0fu) << 4u) | ((bits & 0xf0f0f0f0u) >> 4u);
    bits = ((bits & 0x00ff00ffu) << 8u) | ((bits & 0xff00ff00u) >> 8u);
    return f32(bits) * 2.3283064365386963e-10;
}

fn hammersley(index : u32, count : u32) -> vec2<f32> {
    return vec2<f32>(f32(index) / f32(max(count, 1u)), radical_inverse_vdc(index));
}

fn blue_noise_8x8_coord(seed : u32) -> vec2<u32> {
    let targetRank = seed & 63u;
    var ranked = vec2<u32>(targetRank & 7u, targetRank >> 3u);
    for (var index = 0u; index < 64u; index = index + 1u) {
        if (BLUE_NOISE_8X8_RANKS[index] == targetRank) {
            ranked = vec2<u32>(index & 7u, index >> 3u);
            break;
        }
    }
    let rot = (seed >> 6u) & 3u;
    if (rot == 1u) {
        ranked = vec2<u32>(ranked.y, 7u - ranked.x);
    } else if (rot == 2u) {
        ranked = vec2<u32>(7u - ranked.x, 7u - ranked.y);
    } else if (rot == 3u) {
        ranked = vec2<u32>(7u - ranked.y, ranked.x);
    }
    return ranked;
}

fn tile_candidate_pixel(tile : vec2<u32>, attempt : u32) -> vec2<u32> {
    let tileSize = max(uniforms.controls.w, 1u);
    let tileSeed = hash_u32(uniforms.controls.y + hash_u32(tile.x + hash_u32(tile.y + hash_u32(attempt))));
    var jitter = hammersley((tileSeed + hash_u32(attempt)) & 63u, 64u);
    if (tileSize == 8u) {
        let blue = blue_noise_8x8_coord(tileSeed + attempt * 17u);
        jitter = (vec2<f32>(blue) + vec2<f32>(0.5)) / 8.0;
    }
    let offset = min(vec2<u32>(floor(jitter * f32(tileSize))), vec2<u32>(tileSize - 1u));
    return min(tile * tileSize + min(offset, vec2<u32>(tileSize - 1u)), uniforms.viewport - vec2<u32>(1u));
}

fn jittered_tile_pixel(tile : vec2<u32>) -> vec2<u32> {
    var fallbackPixel = tile_candidate_pixel(tile, 0u);
    for (var attempt = 0u; attempt < 8u; attempt = attempt + 1u) {
        let candidate = tile_candidate_pixel(tile, attempt);
        let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(candidate), 0);
        if (valid_depth(depthValue)) {
            return candidate;
        }
        fallbackPixel = candidate;
    }
    return fallbackPixel;
}

fn light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn luminance(value : vec3<f32>) -> f32 {
    return dot(max(value, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722));
}

struct ShadowLightCandidate {
    direction : vec3<f32>,
    maxT : f32,
    radiance : vec3<f32>,
    distance : f32,
    targetWeight : f32,
    attenuation : f32,
    valid : u32,
    _padding0 : u32,
};

fn invalid_shadow_light_candidate() -> ShadowLightCandidate {
    return ShadowLightCandidate(
        vec3<f32>(0.0, 1.0, 0.0),
        0.0,
        vec3<f32>(0.0),
        0.0,
        0.0,
        0.0,
        0u,
        0u);
}

fn shadow_light_candidate(light : WorldGILight, position : vec3<f32>, normal : vec3<f32>) -> ShadowLightCandidate {
    if (!light_is_enabled(light)) {
        return invalid_shadow_light_candidate();
    }

    let lightPower = max(light.colorIntensity.rgb, vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0);
    var direction = vec3<f32>(0.0, 1.0, 0.0);
    var maxT = uniforms.traceParams.y;
    var distanceToLight = uniforms.traceParams.y;
    var attenuation = 1.0;

    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        direction = safe_normalize(-light.directionType.xyz, normal);
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_POINT) {
        let toLight = light.positionRange.xyz - position;
        distanceToLight = length(toLight);
        if (distanceToLight <= 1.0e-4 || distanceToLight > max(light.positionRange.w, 0.0)) {
            return invalid_shadow_light_candidate();
        }
        direction = toLight / distanceToLight;
        maxT = min(max(distanceToLight - uniforms.traceParams.z, uniforms.traceParams.x), uniforms.traceParams.y);
        let distanceSq = max(distanceToLight * distanceToLight, 1.0e-4);
        let rangeAttenuation = max(1.0 - distanceToLight / max(light.positionRange.w, 1.0e-4), 0.0);
        attenuation = rangeAttenuation * rangeAttenuation / distanceSq;
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        let toCenter = light.positionRange.xyz - position;
        let centerDistance = length(toCenter);
        let radius = max(light.shapeParams.x, 0.0);
        distanceToLight = max(centerDistance - radius, 1.0e-4);
        if (centerDistance <= 1.0e-4 || distanceToLight > max(light.positionRange.w, 0.0)) {
            return invalid_shadow_light_candidate();
        }
        direction = toCenter / centerDistance;
        maxT = min(max(distanceToLight - uniforms.traceParams.z, uniforms.traceParams.x), uniforms.traceParams.y);
        let distanceSq = max(distanceToLight * distanceToLight, 1.0e-4);
        let rangeAttenuation = max(1.0 - distanceToLight / max(light.positionRange.w, 1.0e-4), 0.0);
        attenuation = rangeAttenuation * rangeAttenuation / distanceSq;
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let lightPosition = worldgi_planar_light_closest_point(light, position);
        let toLight = lightPosition - position;
        distanceToLight = length(toLight);
        if (distanceToLight <= 1.0e-4 || distanceToLight > max(light.positionRange.w, 0.0)) {
            return invalid_shadow_light_candidate();
        }
        direction = toLight / distanceToLight;
        let basis = worldgi_planar_light_basis(light);
        let emissionTerm = max(dot(basis.normal, -direction), 0.0);
        if (emissionTerm <= 0.0) {
            return invalid_shadow_light_candidate();
        }
        maxT = min(max(distanceToLight - uniforms.traceParams.z, uniforms.traceParams.x), uniforms.traceParams.y);
        let distanceSq = max(distanceToLight * distanceToLight, 1.0e-4);
        let rangeAttenuation = max(1.0 - distanceToLight / max(light.positionRange.w, 1.0e-4), 0.0);
        attenuation = emissionTerm * rangeAttenuation * rangeAttenuation / distanceSq;
    } else {
        return invalid_shadow_light_candidate();
    }

    let receiverTerm = max(dot(normal, direction), 0.0);
    let radiance = lightPower * attenuation;
    let targetWeight = luminance(radiance) * receiverTerm;
    if (targetWeight <= 1.0e-8 || maxT <= uniforms.traceParams.x) {
        return invalid_shadow_light_candidate();
    }

    return ShadowLightCandidate(
        direction,
        maxT,
        radiance,
        distanceToLight,
        targetWeight,
        attenuation,
        1u,
        0u);
}

struct ShadowLightSelection {
    lightIndex : u32,
    sampleCount : u32,
    _padding0 : u32,
    _padding1 : u32,
    weightSum : f32,
    selectedWeight : f32,
    pdf : f32,
    confidence : f32,
};

fn reservoir_is_valid(reservoir : WorldGIShadowLightReservoir) -> bool {
    return (reservoir.lightSampleFlags.w & WORLD_GI_SHADOW_RESERVOIR_FLAG_VALID) != 0u &&
        reservoir.lightSampleFlags.x != 0xffffffffu &&
        reservoir.weightTargetConfidence.y > 0.0;
}

fn reservoir_matches_surface(reservoir : WorldGIShadowLightReservoir, position : vec3<f32>, normal : vec3<f32>, depthValue : f32) -> bool {
    if (!reservoir_is_valid(reservoir)) {
        return false;
    }
    let previousNormal = safe_normalize(reservoir.normalHistory.xyz, normal);
    let normalOk = dot(previousNormal, normal) > 0.82;
    let planeDistance = abs(dot(position - reservoir.worldPositionDepth.xyz, normal));
    let depthOk = abs(depthValue - reservoir.worldPositionDepth.w) < 0.02 || planeDistance < max(uniforms.traceParams.z * 12.0, 0.20);
    return normalOk && depthOk;
}

fn reservoir_matches_surface_strict(reservoir : WorldGIShadowLightReservoir, position : vec3<f32>, normal : vec3<f32>, depthValue : f32) -> bool {
    if (!reservoir_is_valid(reservoir)) {
        return false;
    }
    let previousNormal = safe_normalize(reservoir.normalHistory.xyz, normal);
    let planeDistance = abs(dot(position - reservoir.worldPositionDepth.xyz, normal));
    let worldDistance = length(position - reservoir.worldPositionDepth.xyz);
    let normalOk = dot(previousNormal, normal) > 0.94;
    let planeOk = planeDistance < max(uniforms.traceParams.z * 6.0, 0.08);
    let depthOk = abs(depthValue - reservoir.worldPositionDepth.w) < 0.008 || planeOk;
    return normalOk && depthOk && worldDistance < max(uniforms.traceParams.z * 32.0, 0.75);
}

fn light_allowed_for_slot(lightType : u32, sampleIndex : u32) -> bool {
    if (sampleIndex == 0u) {
        return lightType == WORLD_GI_LIGHT_TYPE_DIRECTIONAL;
    }
    if (sampleIndex == 1u) {
        return lightType == WORLD_GI_LIGHT_TYPE_POINT ||
            lightType == WORLD_GI_LIGHT_TYPE_SPHERE ||
            lightType == WORLD_GI_LIGHT_TYPE_PLANAR;
    }
    return lightType == WORLD_GI_LIGHT_TYPE_DIRECTIONAL ||
        lightType == WORLD_GI_LIGHT_TYPE_POINT ||
        lightType == WORLD_GI_LIGHT_TYPE_SPHERE ||
        lightType == WORLD_GI_LIGHT_TYPE_PLANAR;
}

fn select_shadow_light(position : vec3<f32>, normal : vec3<f32>, depthValue : f32, probeIndex : u32, sampleIndex : u32, seed : u32) -> ShadowLightSelection {
    let lightCount = min(arrayLength(&lights), lightBVHInfo.lightCount);
    var selected = 0xffffffffu;
    var selectedWeight = 0.0;
    var weightSum = 0.0;
    var sampleCount = 0u;
    for (var lightIndex = 0u; lightIndex < lightCount; lightIndex = lightIndex + 1u) {
        let light = lights[lightIndex];
        if (!light_allowed_for_slot(light.flags.y, sampleIndex)) {
            continue;
        }
        let candidate = shadow_light_candidate(light, position, normal);
        let weight = candidate.targetWeight;
        if (weight > 0.0) {
            sampleCount = sampleCount + 1u;
            weightSum = weightSum + weight;

            if (sampleIndex < 2u) {
                if (weight > selectedWeight) {
                    selected = lightIndex;
                    selectedWeight = weight;
                }
                continue;
            }

            let keepProbability = weight / max(weightSum, 1.0e-6);
            if (random_01(seed ^ hash_u32(lightIndex * 0x9e3779b9u + sampleCount)) < keepProbability) {
                selected = lightIndex;
                selectedWeight = weight;
            }
        }
    }
    let pdf = select(1.0, selectedWeight / max(weightSum, 1.0e-6), selected != 0xffffffffu && weightSum > 0.0);
    return ShadowLightSelection(selected, sampleCount, 0u, 0u, weightSum, selectedWeight, pdf, 0.0);
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

fn seed_reprojected_direction_history(probeIndex : u32, worldPosition : vec3<f32>, normal : vec3<f32>, depthValue : f32, lightIndex : u32, lightType : u32, direction : vec3<f32>, lightDistance : f32) {
    if (uniforms.historyControls.x == 0u) {
        return;
    }
    let previousPixel = project_previous_pixel(worldPosition);
    if (previousPixel.x < 0 || previousPixel.y < 0) {
        return;
    }
    let tileSize = max(uniforms.controls.w, 1u);
    let previousTile = vec2<u32>(previousPixel) / vec2<u32>(tileSize);
    if (previousTile.x >= uniforms.probeGrid.x || previousTile.y >= uniforms.probeGrid.y) {
        return;
    }

    let previousProbeIndex = previousTile.y * uniforms.probeGrid.x + previousTile.x;
    let previousReservoirBase = previousProbeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;
    if (previousReservoirBase >= arrayLength(&shadowReservoirHistory)) {
        return;
    }

    let currentDirectionIndex = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT + oct_texel_index(direction);
    if (currentDirectionIndex >= arrayLength(&shadowProbeDirections)) {
        return;
    }

    var bestHistory = vec4<f32>(0.0);
    var bestScore = 0.0;
    for (var lane = 0u; lane < WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE; lane = lane + 1u) {
        let previousReservoirIndex = previousReservoirBase + lane;
        if (previousReservoirIndex >= arrayLength(&shadowReservoirHistory)) {
            continue;
        }

        let previousReservoir = shadowReservoirHistory[previousReservoirIndex];
        if (!reservoir_matches_surface(previousReservoir, worldPosition, normal, depthValue) ||
            previousReservoir.lightSampleFlags.x != lightIndex ||
            previousReservoir.lightSampleFlags.y != lightType) {
            continue;
        }

        let previousDirection = safe_normalize(previousReservoir.sampleDirectionDistance.xyz, direction);
        let directionAgreement = dot(previousDirection, direction);
        let previousDistance = previousReservoir.sampleDirectionDistance.w;
        let distanceOk = lightType == WORLD_GI_LIGHT_TYPE_DIRECTIONAL ||
            abs(previousDistance - lightDistance) <= max(0.25, max(previousDistance, lightDistance) * 0.10);
        if (directionAgreement < 0.94 || !distanceOk) {
            continue;
        }

        let previousDirectionIndex = previousProbeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT + oct_texel_index(previousDirection);
        if (previousDirectionIndex >= arrayLength(&shadowProbeDirectionHistory)) {
            continue;
        }

        let history = shadowProbeDirectionHistory[previousDirectionIndex];
        let historyCount = clamp(history.a, 0.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT);
        let historyLuma = dot(max(history.rgb, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722));
        let score = historyCount * max(directionAgreement, 0.0) + select(0.0, 1.0, historyLuma > 1.0e-5);
        if (score > bestScore) {
            bestScore = score;
            bestHistory = history;
        }
    }

    if (bestScore > 0.0) {
        let historyLuma = dot(max(bestHistory.rgb, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722));
        let maxSeedCount = select(8.0, 16.0, historyLuma > 1.0e-4);
        let seededCount = clamp(bestHistory.a * 0.50, 0.0, maxSeedCount);
        shadowProbeDirections[currentDirectionIndex] = vec4<f32>(max(bestHistory.rgb, vec3<f32>(0.0)), seededCount);
    }
}

fn reproject_probe_history(probeIndex : u32, worldPosition : vec3<f32>, normal : vec3<f32>, depthValue : f32) -> bool {
    if (uniforms.historyControls.x == 0u) {
        return false;
    }
    let previousPixel = project_previous_pixel(worldPosition);
    if (previousPixel.x < 0 || previousPixel.y < 0) {
        return false;
    }

    let tileSize = max(uniforms.controls.w, 1u);
    let previousTile = vec2<u32>(previousPixel) / vec2<u32>(tileSize);
    if (previousTile.x >= uniforms.probeGrid.x || previousTile.y >= uniforms.probeGrid.y) {
        return false;
    }

    let previousProbeIndex = previousTile.y * uniforms.probeGrid.x + previousTile.x;
    let previousReservoirBase = previousProbeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;
    if (previousReservoirBase >= arrayLength(&shadowReservoirHistory)) {
        return false;
    }

    var validSurface = false;
    for (var lane = 0u; lane < WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE; lane = lane + 1u) {
        let previousReservoirIndex = previousReservoirBase + lane;
        if (previousReservoirIndex < arrayLength(&shadowReservoirHistory)) {
            validSurface = validSurface || reservoir_matches_surface_strict(shadowReservoirHistory[previousReservoirIndex], worldPosition, normal, depthValue);
        }
    }
    if (!validSurface) {
        return false;
    }

    let currentDirectionBase = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT;
    let previousDirectionBase = previousProbeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT;
    for (var texel = 0u; texel < WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT; texel = texel + 1u) {
        let currentIndex = currentDirectionBase + texel;
        let previousIndex = previousDirectionBase + texel;
        if (currentIndex < arrayLength(&shadowProbeDirections) && previousIndex < arrayLength(&shadowProbeDirectionHistory)) {
            let history = shadowProbeDirectionHistory[previousIndex];
            let historyLuma = dot(max(history.rgb, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722));
            let decayedCount = select(0.0, clamp(history.a - 0.25, 0.0, WORLD_GI_SHADOW_TEMPORAL_SAMPLE_COUNT - 1.0), history.a > 0.0 || historyLuma > 1.0e-6);
            shadowProbeDirections[currentIndex] = vec4<f32>(max(history.rgb, vec3<f32>(0.0)), decayedCount);
        }
    }

    let currentReservoirBase = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;
    for (var lane = 0u; lane < WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE; lane = lane + 1u) {
        let currentIndex = currentReservoirBase + lane;
        let previousIndex = previousReservoirBase + lane;
        if (currentIndex < arrayLength(&shadowReservoirs) && previousIndex < arrayLength(&shadowReservoirHistory)) {
            var reservoir = shadowReservoirHistory[previousIndex];
            if (reservoir_matches_surface_strict(reservoir, worldPosition, normal, depthValue)) {
                reservoir.worldPositionDepth = vec4<f32>(worldPosition, depthValue);
                reservoir.normalHistory = vec4<f32>(normal, max(reservoir.normalHistory.w, 1.0));
                reservoir.weightTargetConfidence.w = clamp(reservoir.weightTargetConfidence.w * 0.98, 0.0, 1.0);
                shadowReservoirs[currentIndex] = reservoir;
            }
        }
    }
    return true;
}

fn make_probe_and_ray(pixel : vec2<u32>, probeIndex : u32, sampleIndex : u32) -> WorldGIShadowProbe {
    let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(pixel), 0);
    if (!valid_depth(depthValue)) {
        return invalid_probe();
    }

    let normal = safe_normalize(textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let worldPosition = reconstruct_world_position(pixel, depthValue);
    let seed = hash_u32(probeIndex * 0x45d9f3bu ^ sampleIndex * 0x9e3779b9u ^ uniforms.controls.y * 0x7f4a7c15u);
    let selection = select_shadow_light(worldPosition, normal, depthValue, probeIndex, sampleIndex, seed);
    let lightIndex = selection.lightIndex;
    if (lightIndex == 0xffffffffu) {
        return invalid_probe();
    }

    let light = lights[lightIndex];
    let candidate = shadow_light_candidate(light, worldPosition, normal);
    if (candidate.valid == 0u) {
        return invalid_probe();
    }
    let direction = candidate.direction;
    let maxT = candidate.maxT;
    let unshadowedRadiance = candidate.radiance;

    seed_reprojected_direction_history(probeIndex, worldPosition, normal, depthValue, lightIndex, light.flags.y, direction, candidate.distance);

    let origin = worldPosition + normal * uniforms.traceParams.z;
    let rayIndex = probeIndex * WORLD_GI_SHADOW_RAYS_PER_PROBE + sampleIndex;
    if (rayIndex < arrayLength(&softwareShadowRays)) {
        softwareShadowRays[rayIndex] = SoftwareTraceRay(vec4<f32>(origin, uniforms.traceParams.x), vec4<f32>(direction, maxT));
    }

    let reservoirIndex = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE + sampleIndex;
    if (reservoirIndex < arrayLength(&shadowReservoirs)) {
        shadowReservoirs[reservoirIndex] = WorldGIShadowLightReservoir(
            vec4<u32>(lightIndex, light.flags.y, selection.sampleCount, WORLD_GI_SHADOW_RESERVOIR_FLAG_VALID),
            vec4<f32>(selection.weightSum, selection.selectedWeight, max(selection.pdf, 1.0e-6), selection.confidence),
            vec4<f32>(unshadowedRadiance, 1.0),
            vec4<f32>(worldPosition, depthValue),
            vec4<f32>(normal, 1.0),
            vec4<f32>(direction, candidate.distance));
    }

    return WorldGIShadowProbe(
        vec4<f32>(worldPosition, depthValue),
        vec4<f32>(normal, 0.0),
        vec4<u32>(pixel.x, pixel.y, lightIndex, WORLD_GI_SHADOW_PROBE_FLAG_VALID | WORLD_GI_SHADOW_PROBE_FLAG_ACTIVE_RAY),
        vec4<f32>(direction, maxT),
        vec4<f32>(unshadowedRadiance, max(selection.pdf, 1.0e-6)),
        vec4<f32>(0.0));
}

fn clear_probe_temporal(probeIndex : u32) {
    let directionBase = probeIndex * WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT;
    for (var directionTexel = 0u; directionTexel < WORLD_GI_SHADOW_PROBE_OCT_TEXEL_COUNT; directionTexel = directionTexel + 1u) {
        let directionIndex = directionBase + directionTexel;
        if (directionIndex < arrayLength(&shadowProbeDirections)) {
            shadowProbeDirections[directionIndex] = vec4<f32>(0.0);
        }
    }
    let reservoirBase = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;
    for (var sampleIndex = 0u; sampleIndex < WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE; sampleIndex = sampleIndex + 1u) {
        let reservoirIndex = reservoirBase + sampleIndex;
        if (reservoirIndex < arrayLength(&shadowReservoirs)) {
            shadowReservoirs[reservoirIndex] = invalid_reservoir();
        }
    }
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.probeGrid.x || gid.y >= uniforms.probeGrid.y) {
        return;
    }

    let probeIndex = gid.y * uniforms.probeGrid.x + gid.x;
    if (probeIndex >= arrayLength(&shadowProbes)) {
        return;
    }
    for (var sampleIndex = 0u; sampleIndex < WORLD_GI_SHADOW_RAYS_PER_PROBE; sampleIndex = sampleIndex + 1u) {
        let rayIndex = probeIndex * WORLD_GI_SHADOW_RAYS_PER_PROBE + sampleIndex;
        if (rayIndex < arrayLength(&softwareShadowRays)) {
            softwareShadowRays[rayIndex] = invalid_software_ray();
        }
    }

    var probe = invalid_probe();
    if (uniforms.controls.x == 0u) {
        clear_probe_temporal(probeIndex);
        shadowProbes[probeIndex] = probe;
        return;
    }

    let pixel = jittered_tile_pixel(gid.xy);
    let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(pixel), 0);
    if (!valid_depth(depthValue)) {
        clear_probe_temporal(probeIndex);
        shadowProbes[probeIndex] = probe;
        return;
    }

    let normal = safe_normalize(textureLoad(geometryNormalTexture, vec2<i32>(pixel), 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let worldPosition = reconstruct_world_position(pixel, depthValue);
    clear_probe_temporal(probeIndex);
    let _historyAccepted = reproject_probe_history(probeIndex, worldPosition, normal, depthValue);
    if (_historyAccepted) {
        probe = WorldGIShadowProbe(
            vec4<f32>(worldPosition, depthValue),
            vec4<f32>(normal, 1.0),
            vec4<u32>(pixel.x, pixel.y, 0xffffffffu, WORLD_GI_SHADOW_PROBE_FLAG_VALID),
            vec4<f32>(normal, uniforms.traceParams.y),
            vec4<f32>(0.0, 0.0, 0.0, 1.0),
            vec4<f32>(0.0));
    }
    let reservoirBase = probeIndex * WORLD_GI_SHADOW_RESERVOIR_SLOTS_PER_PROBE;

    let activeRayCount = clamp(u32(uniforms.traceParams.w), 1u, WORLD_GI_SHADOW_RAYS_PER_PROBE);
    for (var sampleIndex = 0u; sampleIndex < WORLD_GI_SHADOW_RAYS_PER_PROBE; sampleIndex = sampleIndex + 1u) {
        if (sampleIndex >= activeRayCount) {
            let reservoirIndex = reservoirBase + sampleIndex;
            if (reservoirIndex < arrayLength(&shadowReservoirs)) {
                shadowReservoirs[reservoirIndex] = invalid_reservoir();
            }
            continue;
        }
        let candidate = make_probe_and_ray(pixel, probeIndex, sampleIndex);
        if ((probe.pixelLightFlags.w & WORLD_GI_SHADOW_PROBE_FLAG_VALID) == 0u &&
            (candidate.pixelLightFlags.w & WORLD_GI_SHADOW_PROBE_FLAG_VALID) != 0u) {
            probe = candidate;
        }
    }
    shadowProbes[probeIndex] = probe;
}
