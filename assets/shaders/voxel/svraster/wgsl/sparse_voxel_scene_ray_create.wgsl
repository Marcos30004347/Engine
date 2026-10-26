#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_scene_ray_types.wgsl"
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "sparse_voxel_sky_guide_common.wgsl"
#include "sparse_voxel_di_target.wgsl"
#include "sparse_voxel_light_reachability.wgsl"
#include "sparse_voxel_local_light_hash.wgsl"

const SCENE_RAY_TAU : f32 = 6.28318530718;

// sampling.y is a mode bitfield.
const SCENE_RAY_MODE_SKY : u32 = 1u << 0u;
const SCENE_RAY_MODE_REPROJECT_DEMAND : u32 = 1u << 1u;
const SCENE_RAY_MODE_HISTORY_VALID : u32 = 1u << 2u;

struct SparseVoxelSceneRayCreateSettings {
    lightDirectionMaximumDistance : vec4<f32>,
    dimensionsGeneration : vec4<u32>,
    originBiasReserved : vec4<f32>,
    sampling : vec4<u32>,
    // x: legacy rotation period. y: sparse window side (1, 2, 4, 8 or 16).
    // z: reachability-cache entry count.
    rotation : vec4<u32>,
    // x: reachability region size, y: exploration floor.
    reachability : vec4<f32>,
    // x: cache generation, y: light capacity.
    reachabilityGeneration : vec4<u32>,
    // x: visible receiver table capacity, y: entry stride in words,
    // z: current frame generation, w: regional stability tile size.
    visibleReceiverMap : vec4<u32>,
    // x: minimum measured samples, y: maximum relative standard error,
    // z: required consecutive stable observations, w: camera distance beyond
    // which a receiver is lit analytically.
    stabilization : vec4<f32>,
    // Adaptive tile budget. x: fraction of the eligible receivers a tile of
    // average demand may trace, y: maximum redistribution boost for a tile
    // above the frame average, z: reserved heartbeat slots per covered tile,
    // w: nonzero when the tile-budget buffer is bound.
    tileBudget : vec4<f32>,
    // x: scheduler tile size in pixels, y: tile count, z: minimum full-rate
    // viewport edge width. x is always the real tile size, unlike
    // visibleReceiverMap.w which is zeroed to disable the regional wake state
    // when no stability buffer is bound.
    tileGrid : vec4<u32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelSceneRayCreateSettings;
@group(0) @binding(2) var<storage, read> coverage : array<u32>;
@group(0) @binding(3) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(4) var<storage, read_write> rays : array<SparseVoxelSceneRay>;
@group(0) @binding(5) var<storage, read> lights : array<WorldGILight>;
@group(0) @binding(6) var<storage, read> lightBVHNodes : array<WorldGILightBVHNode>;
@group(0) @binding(7) var<storage, read> lightBVHInfo : WorldGILightBVHInfo;
@group(0) @binding(8) var geometryNormalTexture : texture_2d<f32>;
// Previous frame's lighting field: rgb is the accumulated estimate, a is the
// number of direct samples that pixel has received. This pass runs before the
// field is rebuilt, so it necessarily reads last frame's - which is exactly
// what a demand signal should be.
@group(0) @binding(9) var lightFieldHistoryTexture : texture_2d<f32>;
@group(0) @binding(10) var<storage, read> lightReachability : array<u32>;
@group(0) @binding(11) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(12) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(13) var motionVectorTexture : texture_2d<f32>;
@group(0) @binding(14) var lightFieldHistoryIdTexture : texture_2d<u32>;
@group(0) @binding(15) var<storage, read_write> activeRayIndices : array<u32>;
// xyz are DispatchIndirectArgs; w is the exact active-lane count.
@group(0) @binding(16) var<storage, read_write> activeRayState : array<atomic<u32>>;
@group(0) @binding(17) var<storage, read_write> finalRays : array<SparseVoxelSceneRay>;
@group(0) @binding(18) var<storage, read_write> finalReservoirs : array<SparseVoxelDIReservoir>;
@group(0) @binding(19) var<storage, read_write> reservoirRayIndices : array<u32>;
@group(0) @binding(20) var<storage, read_write> reservoirRayState : array<atomic<u32>>;
@group(0) @binding(21) var<storage, read> skyGuideCDF : array<f32>;
@group(0) @binding(22) var lightFieldHistoryMomentsTexture : texture_2d<f32>;
@group(0) @binding(23) var lightFieldHistoryLightHashTexture : texture_2d<u32>;
@group(0) @binding(24) var<storage, read> visibleReceiverMap : array<u32>;
// Four words per screen tile: valid-light mask, visible-light mask,
// wake-until frame, reserved.
@group(0) @binding(25) var<storage, read> regionStability : array<u32>;
// Two statistics banks selected by frame parity, then eight words per screen
// tile. Written by the tile-budget scheduler, read back only for statistics.
@group(0) @binding(26) var<storage, read_write> rayTileBudget : array<atomic<u32>>;

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

@group(0) @binding(27) var<storage, read> terrainOwnershipIds : array<u32>;
@group(0) @binding(28) var<storage, read> terrainOwnershipTable :
    array<SparseVoxelTerrainOwnership>;

fn scene_ray_hash(value : u32) -> f32 {
    var bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    bits = bits * 277803737u;
    return f32((bits >> 8u) & 0x00ffffffu) / 16777216.0;
}

fn scene_ray_hash_bits(value : u32) -> u32 {
    var bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    return bits * 277803737u;
}

// Nested ordered receiver schedule. Procedural bit interleaving visits
// every cell once per cycle without storing a precomputed noise texture.
fn scene_ray_ordered_coord(rank : u32) -> vec2<u32> {
    let a = rank & 63u;
    let x = (a & 1u) * 4u + ((a >> 2u) & 1u) * 2u + ((a >> 4u) & 1u);
    let y = (((a >> 0u) ^ (a >> 1u)) & 1u) * 4u +
        (((a >> 2u) ^ (a >> 3u)) & 1u) * 2u + (((a >> 4u) ^ (a >> 5u)) & 1u);
    return vec2<u32>(x, y);
}

fn scene_ray_schedule_phase_count() -> u32 {
    let window = clamp(settings.rotation.y, 1u, 16u);
    return window * window;
}

// Procedural, per-receiver rotated R2 hemisphere samples.
fn scene_ray_sky_sequence(
    stableSeed : u32, frameIndex : u32) -> vec2<f32> {
    let rotation = vec2<f32>(
        scene_ray_hash(stableSeed ^ 0x68bc21ebu),
        scene_ray_hash(stableSeed ^ 0x02e5be93u));
    return fract(
        rotation + f32(frameIndex) *
            vec2<f32>(0.7548776662466927, 0.5698402909980532));
}

fn scene_ray_sky_guide_tile(pixel : vec2<u32>) -> u32 {
    let tileDimensions = (max(settings.sampling.zw, vec2<u32>(1u)) +
        vec2<u32>(SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE - 1u)) /
        SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE;
    let tile = min(
        pixel / SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE,
        tileDimensions - vec2<u32>(1u));
    return tile.y * tileDimensions.x + tile.x;
}

fn scene_ray_sky_guide_probability(
    cdfBase : u32, bin : u32) -> f32 {
    if (cdfBase + SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT >
        arrayLength(&skyGuideCDF)) {
        return 1.0 / f32(SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT);
    }
    let upper = clamp(skyGuideCDF[cdfBase + bin], 0.0, 1.0);
    var lower = 0.0;
    if (bin != 0u) {
        lower = clamp(skyGuideCDF[cdfBase + bin - 1u], 0.0, 1.0);
    }
    let probability = max(upper - lower, 0.0);
    return probability;
}

fn scene_ray_sample_guided_sky(
    normal : vec3<f32>, pixel : vec2<u32>, stableSeed : u32,
    frameIndex : u32) -> SparseVoxelSkyDirectionSample {
    let baseParameter = scene_ray_sky_sequence(stableSeed, frameIndex);
    var strength = clamp(settings.reachability.z, 0.0, 0.95);
    let tileIndex = scene_ray_sky_guide_tile(pixel);
    let cdfBase = tileIndex * SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT;
    if (cdfBase + SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT >
        arrayLength(&skyGuideCDF)) {
        strength = 0.0;
    } else if (skyGuideCDF[
            cdfBase + SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT - 1u] < 0.999) {
        // A newly allocated or reset guide is invalid until its uniform CDF
        // seed is visible. Falling back to the base proposal keeps both the
        // sample and its recorded PDF correct.
        strength = 0.0;
    }

    var parameter = baseParameter;
    // The branch must vary over time. Holding it fixed per receiver would make
    // some pixels permanently guided and others permanently unguided while
    // still recording the mixture PDF, which is a biased estimator.
    let guideSeed = stableSeed ^
        (frameIndex * 0x9e3779b9u);
    let chooseGuide = scene_ray_hash(guideSeed ^ 0xa511e9b3u) < strength;
    if (chooseGuide) {
        let selection = scene_ray_hash(guideSeed ^ 0x63d83595u);
        var lower = 0u;
        var upper = SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT - 1u;
        for (var step = 0u; step < 4u; step = step + 1u) {
            let middle = (lower + upper) >> 1u;
            if (selection < clamp(
                    skyGuideCDF[cdfBase + middle], 0.0, 1.0)) {
                upper = middle;
            } else {
                lower = middle + 1u;
            }
        }
        let radialBin = lower / SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS;
        let azimuthBin = lower % SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS;
        parameter = vec2<f32>(
            (f32(radialBin) + baseParameter.x) /
                f32(SPARSE_VOXEL_SKY_GUIDE_RADIAL_BINS),
            (f32(azimuthBin) + baseParameter.y) /
                f32(SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS));
    }

    var sample = sparse_voxel_sample_cosine_hemisphere_parameter(
        normal, parameter);
    let binProbability = scene_ray_sky_guide_probability(
        cdfBase, sparse_voxel_sky_guide_bin(parameter));
    let densityScale = (1.0 - strength) + strength * binProbability *
        f32(SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT);
    sample.pdf = sample.pdf * max(densityScale, 1.0e-6);
    return sample;
}

fn scene_light_is_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID && light.colorIntensity.w > 0.0;
}

fn scene_light_can_affect_position(light : WorldGILight, position : vec3<f32>) -> bool {
    if (!scene_light_is_enabled(light)) { return false; }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL ||
        light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) { return true; }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        // Quad emitters are one-sided in the shading pass, so a receiver
        // behind the plane can only ever draw samples that shade to black.
        // Rejecting it here frees the candidate slot for a light that can
        // actually contribute instead of spending it on a guaranteed zero.
        let emitterNormal = sparse_voxel_safe_normalize(
            light.directionType.xyz, vec3<f32>(0.0, -1.0, 0.0));
        if (dot(emitterNormal, position - light.positionRange.xyz) <= 0.0) {
            return false;
        }
        let closest = worldgi_planar_light_closest_point(light, position);
        return distance(closest, position) <= light.positionRange.w;
    }
    let radius = max(light.positionRange.w, 0.0) + select(0.0, max(light.shapeParams.x, 0.0), light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE);
    return distance(light.positionRange.xyz, position) <= radius;
}

struct SceneLightSelection { lightIndex : u32, selectionPdf : f32, candidateWeight : f32, }

fn scene_infinite_light_weight(light : WorldGILight) -> f32 {
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
        return sparse_voxel_sky_average_luminance(light);
    }
    return dot(max(light.colorIntensity.rgb, vec3<f32>(0.0)), vec3<f32>(0.2126, 0.7152, 0.0722)) *
        max(light.colorIntensity.w, 0.0);
}

fn scene_light_node_weight(node : WorldGILightBVHNode, position : vec3<f32>) -> f32 {
    if (any(position < node.boundsMinNodeType.xyz) ||
        any(position > node.boundsMaxImportance.xyz)) { return 0.0; }
    let closest = clamp(position, node.boundsMinNodeType.xyz, node.boundsMaxImportance.xyz);
    let distanceSq = max(dot(position - closest, position - closest), 1.0e-4);
    return max(node.boundsMaxImportance.w, 0.0) / distanceSq;
}

// Conservative upper bound on whether any part of an emitter lies above the
// receiver hemisphere. Reachability can learn that a geometrically valid ray
// is shadowed, but a light entirely behind the surface produces a zero target
// and no ray at all, so it can never generate cache feedback. Rejecting that
// candidate here prevents side faces from repeatedly wasting their only
// quarter-rate slot while the sky is available.
fn scene_light_receiver_importance(
    light : WorldGILight, position : vec3<f32>,
    normal : vec3<f32>) -> f32 {
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
        // Cosine-hemisphere sampling already incorporates receiver
        // orientation in its proposal.
        return 1.0;
    }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let directionLength = length(light.directionType.xyz);
        if (directionLength <= 1.0e-6) { return 0.0; }
        return max(
            dot(normal, -light.directionType.xyz / directionLength), 0.0);
    }

    let toCenter = light.positionRange.xyz - position;
    let centerDistance = length(toCenter);
    if (centerDistance <= 1.0e-6) { return 1.0; }
    var angularMargin = 0.0;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        angularMargin =
            max(light.shapeParams.x, 0.0) / centerDistance;
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        angularMargin = length(
            max(light.shapeParams.yz, vec2<f32>(0.0))) / centerDistance;
    }
    // Extended emitters may straddle the horizon even when their centre is
    // slightly behind it. The radius/diagonal margin keeps those candidates;
    // a point light has no margin and is rejected exactly at N.L <= 0.
    return clamp(
        dot(normal, toCenter / centerDistance) + angularMargin, 0.0, 1.0);
}

fn scene_sample_directional_disk(
    light : WorldGILight, u : vec2<f32>) -> vec3<f32> {
    let axis = -normalize(light.directionType.xyz);
    let angularRadius = clamp(light.shapeParams.x, 0.0, 0.25);
    if (angularRadius <= 1.0e-6) { return axis; }
    let fallback = select(vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(1.0, 0.0, 0.0), abs(axis.y) > 0.95);
    let tangent = normalize(cross(fallback, axis));
    let bitangent = cross(axis, tangent);
    let cosine = mix(1.0, cos(angularRadius), u.x);
    let sine = sqrt(max(1.0 - cosine * cosine, 0.0));
    let phi = 6.28318530718 * u.y;
    return normalize(axis * cosine +
        (tangent * cos(phi) + bitangent * sin(phi)) * sine);
}

fn scene_light_sampling_weight(
    light : WorldGILight, lightIndex : u32,
    position : vec3<f32>, normal : vec3<f32>, regionKey : u32) -> f32 {
    if (!scene_light_can_affect_position(light, position)) { return 0.0; }
    let receiverImportance =
        scene_light_receiver_importance(light, position, normal);
    if (receiverImportance <= 0.0) { return 0.0; }
    let baseWeight = select(
        sparse_voxel_di_light_power(light),
        scene_infinite_light_weight(light),
        sparse_voxel_di_light_is_infinite(light));
    // Keep the proposal distribution local and frame-independent. Regional
    // visibility feedback used to bias this PDF and made a cached binary
    // outcome persist through otherwise valid ReSTIR history.
    return max(baseWeight, 0.0) * receiverImportance;
}

fn scene_select_light(
    position : vec3<f32>, normal : vec3<f32>, regionKey : u32, seed : u32,
    excludeSky : bool)
    -> SceneLightSelection {
    let lightCount = min(lightBVHInfo.lightCount, arrayLength(&lights));
    var totalWeight = 0.0;
    for (var lightIndex = 0u; lightIndex < lightCount;
         lightIndex = lightIndex + 1u) {
        if (excludeSky &&
            lights[lightIndex].flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
            continue;
        }
        totalWeight = totalWeight + scene_light_sampling_weight(
            lights[lightIndex], lightIndex, position, normal, regionKey);
    }
    if (totalWeight <= 0.0) {
        return SceneLightSelection(0xffffffffu, 0.0, 0.0);
    }
    let selectedWeight = scene_ray_hash(seed ^ 0x02e5be93u) * totalWeight;
    var accumulatedWeight = 0.0;
    for (var lightIndex = 0u; lightIndex < lightCount;
         lightIndex = lightIndex + 1u) {
        if (excludeSky &&
            lights[lightIndex].flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
            continue;
        }
        let lightWeight = scene_light_sampling_weight(
            lights[lightIndex], lightIndex, position, normal, regionKey);
        accumulatedWeight = accumulatedWeight + lightWeight;
        if (lightWeight > 0.0 && selectedWeight <= accumulatedWeight) {
            return SceneLightSelection(
                lightIndex, lightWeight / totalWeight, lightWeight);
        }
    }
    return SceneLightSelection(0xffffffffu, 0.0, 0.0);
}

// BVH-accelerated light selection. Handles infinite lights (sky/directional)
// with a short linear scan, then traverses the spatial BVH top-down, picking
// children probabilistically by their importance weight at the shading point.
// Falls back to the O(N) scene_select_light() when the BVH is empty.
fn scene_select_light_bvh(
    position : vec3<f32>, normal : vec3<f32>, regionKey : u32, seed : u32,
    excludeSky : bool)
    -> SceneLightSelection {
    if (lightBVHInfo.nodeCount == 0u) {
        return scene_select_light(
            position, normal, regionKey, seed, excludeSky);
    }
    let lightCount = min(lightBVHInfo.lightCount, arrayLength(&lights));
    let nodeCount = min(lightBVHInfo.nodeCount, arrayLength(&lightBVHNodes));

    // --- Phase 1: infinite lights (sky / directional) ---
    var infiniteTotalWeight = 0.0;
    var infiniteSelectedIndex = 0xffffffffu;
    var infiniteSelectedWeight = 0.0;
    let infiniteCount = min(lightBVHInfo.infiniteLightCount, nodeCount);
    for (var i = 0u; i < infiniteCount; i = i + 1u) {
        let node = lightBVHNodes[i];
        let nodeType = u32(node.boundsMinNodeType.w);
        if (nodeType != WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE) { continue; }
        let li = node.childOrLightCount.z;
        if (li >= lightCount) { continue; }
        if (excludeSky && lights[li].flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
            continue;
        }
        let w = scene_light_sampling_weight(
            lights[li], li, position, normal, regionKey);
        infiniteTotalWeight = infiniteTotalWeight + w;
        if (w > 0.0) {
            infiniteSelectedIndex = li;
            infiniteSelectedWeight = w;
        }
    }
    // If there are multiple infinite lights, do a proper random selection.
    if (infiniteTotalWeight > 0.0 && infiniteCount > 1u) {
        let threshold = scene_ray_hash(seed ^ 0x1a2b3c4du) * infiniteTotalWeight;
        var acc = 0.0;
        infiniteSelectedIndex = 0xffffffffu;
        infiniteSelectedWeight = 0.0;
        for (var i = 0u; i < infiniteCount; i = i + 1u) {
            let node = lightBVHNodes[i];
            let nodeType = u32(node.boundsMinNodeType.w);
            if (nodeType != WORLD_GI_LIGHT_BVH_NODE_TYPE_INFINITE) { continue; }
            let li = node.childOrLightCount.z;
            if (li >= lightCount) { continue; }
            if (excludeSky &&
                lights[li].flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
                continue;
            }
            let w = scene_light_sampling_weight(
                lights[li], li, position, normal, regionKey);
            acc = acc + w;
            if (w > 0.0 && threshold <= acc) {
                infiniteSelectedIndex = li;
                infiniteSelectedWeight = w;
                break;
            }
        }
    }

    // --- Phase 2: BVH traversal for finite lights ---
    var bvhWeight = 0.0;
    var bvhSelectedIndex = 0xffffffffu;
    var bvhSelectedWeight = 0.0;
    var bvhPdfAccumulator = 1.0;

    let rootIndex = lightBVHInfo.rootNodeIndex;
    if (rootIndex < nodeCount) {
        let rootNode = lightBVHNodes[rootIndex];
        let rootType = u32(rootNode.boundsMinNodeType.w);
        if (rootType != WORLD_GI_LIGHT_BVH_NODE_TYPE_INVALID) {
            // Iterative traversal with a small stack. The BVH depth is
            // bounded by log2(lightCount); 16 levels covers 65536 lights.
            var stackIndex = rootIndex;
            var stackPdf = 1.0;
            // Hash seed evolves at each decision to decorrelate child picks.
            var hashSeed = seed ^ 0x9e3779b9u;

            // Iterative single-path descent: at each internal node pick one
            // child, at each leaf evaluate the light.
            for (var step = 0u; step < 64u; step = step + 1u) {
                if (stackIndex >= nodeCount ||
                    stackIndex == WORLD_GI_LIGHT_BVH_INVALID_NODE) {
                    break;
                }
                let node = lightBVHNodes[stackIndex];
                let nodeType = u32(node.boundsMinNodeType.w);

                if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_LEAF) {
                    let li = node.childOrLightCount.z;
                    if (li < lightCount) {
                        let w = scene_light_sampling_weight(
                            lights[li], li, position, normal, regionKey);
                        if (w > 0.0) {
                            bvhWeight = w;
                            bvhSelectedIndex = li;
                            bvhSelectedWeight = w;
                            bvhPdfAccumulator = stackPdf;
                        }
                    }
                    break;
                }
                if (nodeType == WORLD_GI_LIGHT_BVH_NODE_TYPE_INTERNAL) {
                    let leftIndex = node.childOrLightCount.x;
                    let rightIndex = node.childOrLightCount.y;
                    let leftValid = leftIndex < nodeCount &&
                        leftIndex != WORLD_GI_LIGHT_BVH_INVALID_NODE;
                    let rightValid = rightIndex < nodeCount &&
                        rightIndex != WORLD_GI_LIGHT_BVH_INVALID_NODE;

                    if (!leftValid && !rightValid) { break; }
                    if (!rightValid) {
                        stackIndex = leftIndex;
                        continue;
                    }
                    if (!leftValid) {
                        stackIndex = rightIndex;
                        continue;
                    }

                    let leftWeight = scene_light_node_weight(
                        lightBVHNodes[leftIndex], position);
                    let rightWeight = scene_light_node_weight(
                        lightBVHNodes[rightIndex], position);
                    let totalChildWeight = leftWeight + rightWeight;
                    if (totalChildWeight <= 0.0) { break; }

                    let leftProb = leftWeight / totalChildWeight;
                    hashSeed = hashSeed * 747796405u + 2891336453u;
                    let xi = scene_ray_hash(hashSeed);
                    if (xi < leftProb) {
                        stackIndex = leftIndex;
                        stackPdf = stackPdf * leftProb;
                    } else {
                        stackIndex = rightIndex;
                        stackPdf = stackPdf * (1.0 - leftProb);
                    }
                } else {
                    // Infinite or invalid node at non-root — skip.
                    break;
                }
            }
        }
    }

    // --- Phase 3: combine infinite and BVH selections ---
    let finiteTotalWeight = select(0.0, bvhWeight / max(bvhPdfAccumulator, 1.0e-20), bvhSelectedIndex != 0xffffffffu);
    let combinedWeight = infiniteTotalWeight + finiteTotalWeight;
    if (combinedWeight <= 0.0) {
        return SceneLightSelection(0xffffffffu, 0.0, 0.0);
    }

    let xi = scene_ray_hash(seed ^ 0x02e5be93u);
    let infiniteProb = infiniteTotalWeight / combinedWeight;

    if (xi < infiniteProb && infiniteSelectedIndex != 0xffffffffu) {
        // Selected an infinite light.
        let selectionPdf = (infiniteSelectedWeight / max(infiniteTotalWeight, 1.0e-20)) *
            infiniteProb;
        return SceneLightSelection(
            infiniteSelectedIndex, selectionPdf, infiniteSelectedWeight);
    }
    if (bvhSelectedIndex != 0xffffffffu) {
        // Selected from BVH. The PDF is the product of all branch decisions
        // times the probability of choosing the finite category.
        let selectionPdf = bvhPdfAccumulator * (1.0 - infiniteProb);
        return SceneLightSelection(
            bvhSelectedIndex, selectionPdf, bvhSelectedWeight);
    }
    return SceneLightSelection(0xffffffffu, 0.0, 0.0);
}

// Recover the history count belonging to the current surface. Reading the
// same screen coordinate after camera motion can pick up a well-converged
// record from an entirely different wall and starve a newly revealed surface.
// Use the same motion convention and identity-aware 2x2 footprint as the light
// field accumulator so disocclusions immediately report zero confidence.
// count, mean luminance, luminance second moment, consecutive stable updates.
fn scene_ray_history_state(
    pixel : vec2<u32>, currentLightHash : u32) -> vec4<f32> {
    if ((settings.sampling.y & SCENE_RAY_MODE_REPROJECT_DEMAND) == 0u) {
        let record = textureLoad(
            lightFieldHistoryTexture, vec2<i32>(pixel), 0);
        let moments = textureLoad(
            lightFieldHistoryMomentsTexture, vec2<i32>(pixel), 0).xy;
        return vec4<f32>(
            max(record.a, 0.0),
            dot(max(record.rgb, vec3<f32>(0.0)),
                vec3<f32>(0.2126, 0.7152, 0.0722)),
            max(moments.x, 0.0), max(moments.y, 0.0));
    }
    if ((settings.sampling.y & SCENE_RAY_MODE_HISTORY_VALID) == 0u) {
        return vec4<f32>(0.0);
    }
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let pixelI = vec2<i32>(pixel);
    // Must match light_field_identity(). A voxel can expose opposite faces,
    // and reusing the outside face's convergence on the inside is both a
    // light leak and a false "sleep" decision.
    let normal = textureLoad(geometryNormalTexture, pixelI, 0).xyz;
    let normalMagnitude = abs(normal);
    var faceAxis = 0u;
    if (normalMagnitude.y > normalMagnitude.x) { faceAxis = 1u; }
    if (normalMagnitude.z > max(normalMagnitude.x, normalMagnitude.y)) {
        faceAxis = 2u;
    }
    let faceClass = faceAxis |
        select(0u, 4u, normal[faceAxis] < 0.0);
    let instance = textureLoad(instanceIdTexture, pixelI, 0).x;
    let identity = vec2<u32>(
        (instance & ((1u << 29u) - 1u)) | (faceClass << 29u),
        textureLoad(voxelIdTexture, pixelI, 0).x);
    let motion = textureLoad(motionVectorTexture, pixelI, 0).xy;
    let uv =
        (vec2<f32>(pixel) + vec2<f32>(0.5)) / vec2<f32>(dimensions);
    let historyUv = uv - motion;
    if (any(historyUv < vec2<f32>(0.0)) ||
        any(historyUv >= vec2<f32>(1.0))) {
        return vec4<f32>(0.0);
    }
    let historyPosition =
        historyUv * vec2<f32>(dimensions) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(historyPosition));
    let fraction = fract(historyPosition);
    var countSum = 0.0;
    var luminanceSum = 0.0;
    var secondMomentSum = 0.0;
    var stableUpdateSum = 0.0;
    var weightSum = 0.0;
    for (var y = 0; y <= 1; y = y + 1) {
        for (var x = 0; x <= 1; x = x + 1) {
            let tap = clamp(
                base + vec2<i32>(x, y), vec2<i32>(0),
                vec2<i32>(dimensions) - vec2<i32>(1));
            if (any(textureLoad(
                    lightFieldHistoryIdTexture, tap, 0).xy != identity) ||
                textureLoad(
                    lightFieldHistoryLightHashTexture, tap, 0).x !=
                    currentLightHash) {
                continue;
            }
            let record = textureLoad(lightFieldHistoryTexture, tap, 0);
            let count = max(record.a, 0.0);
            if (count <= 0.0) { continue; }
            let moments = max(textureLoad(
                lightFieldHistoryMomentsTexture, tap, 0).xy,
                vec2<f32>(0.0));
            let tapWeight =
                select(1.0 - fraction.x, fraction.x, x == 1) *
                select(1.0 - fraction.y, fraction.y, y == 1);
            countSum = countSum + count * tapWeight;
            luminanceSum = luminanceSum + dot(
                max(record.rgb, vec3<f32>(0.0)),
                vec3<f32>(0.2126, 0.7152, 0.0722)) * tapWeight;
            secondMomentSum = secondMomentSum + moments.x * tapWeight;
            stableUpdateSum = stableUpdateSum + moments.y * tapWeight;
            weightSum = weightSum + tapWeight;
        }
    }
    return select(
        vec4<f32>(0.0),
        vec4<f32>(countSum, luminanceSum, secondMomentSum,
            stableUpdateSum) / max(weightSum, 1.0e-5),
        weightSum > 1.0e-5);
}

fn scene_ray_clip_to_uv(clip : vec4<f32>) -> vec2<f32> {
    let ndc = clip.xy / clip.w;
    return vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
}

// Motion vectors include camera and object motion. Exact brick identities can
// safely ignore the former after successful reprojection, but not the latter:
// moving geometry changes its local lighting and can also reveal an occluder.
fn scene_ray_object_motion_pixels(
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
        scene_ray_clip_to_uv(currentClip) -
        scene_ray_clip_to_uv(previousClip);
    return length((motion - cameraMotion) * vec2<f32>(dimensions));
}

fn scene_ray_region_state_base(pixel : vec2<u32>) -> u32 {
    let tileSize = settings.visibleReceiverMap.w;
    if (tileSize == 0u) { return 0xffffffffu; }
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let tileDimensions =
        (dimensions + vec2<u32>(tileSize - 1u)) / tileSize;
    let tile = pixel / tileSize;
    return (tile.y * tileDimensions.x + tile.x) *
        SPARSE_VOXEL_REGION_STABILITY_WORDS_PER_TILE;
}

fn scene_ray_region_is_awake(pixel : vec2<u32>, frame : u32) -> bool {
    let base = scene_ray_region_state_base(pixel);
    return base != 0xffffffffu && base + 2u < arrayLength(&regionStability) &&
        regionStability[base + 2u] > frame;
}

// Expected budget is one covered sentinel per tile per interval. A stochastic
// election avoids a separate full-screen prefix/map pass and rotates the
// receiver, so different surfaces and lights in the tile are eventually
// checked. Zero interval disables the heartbeat.
fn scene_ray_is_region_sentinel(pixel : vec2<u32>, frame : u32) -> bool {
    let tileSize = settings.visibleReceiverMap.w;
    let interval = settings.originBiasReserved.w;
    if (tileSize == 0u || interval <= 0.0) { return false; }
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    // Compensate for the sparse-window phase and the exact-face 2x2
    // representative filter that run before/after this election. The final
    // post-filter expectation remains one traced sentinel per covered tile.
    let schedulePhases = scene_ray_schedule_phase_count();
    let receiverWidth = select(
        1u, max(settings.reachabilityGeneration.w, 1u),
        settings.reachabilityGeneration.z != 0u);
    let eligibilityCompensation =
        f32(schedulePhases * receiverWidth * receiverWidth);
    let probability = eligibilityCompensation /
        max(f32(tileSize * tileSize) * interval, 1.0);
    return scene_ray_hash(
        pixelIndex * 2246822519u ^ frame * 0x9e3779b9u) <
        min(probability, 1.0);
}

// Per-receiver refresh is separate from the regional sentinel heartbeat. The
// public setting is an average frame interval for each settled receiver: one
// means every receiver every frame. The old implementation divided this
// probability by the tile area and therefore turned the default into roughly
// one refresh for the entire tile, allowing a wrong screen-space shadow sample
// to remain in a sleeping pixel indefinitely.
fn scene_ray_is_periodic_refresh(pixel : vec2<u32>, frame : u32) -> bool {
    let interval = settings.originBiasReserved.w;
    if (interval <= 0.0) { return false; }
    if (interval <= 1.0) { return true; }
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    return scene_ray_hash(
        pixelIndex * 2246822519u ^ frame * 0x9e3779b9u ^ 0x68bc21ebu) <
        min(1.0 / interval, 1.0);
}

// Classified demand for one receiver. The scalar drives the legacy stochastic
// gates; the class and the boolean breakdown drive the adaptive tile budget and
// its per-tile counters.
struct SceneRayDemandState {
    demand : f32,
    priorityClass : u32,
    disoccluded : bool,
    moving : bool,
    noisy : bool,
    unconverged : bool,
    regionAwake : bool,
    settled : bool,
}

fn scene_ray_empty_demand_state() -> SceneRayDemandState {
    return SceneRayDemandState(
        0.0, SPARSE_VOXEL_RAY_BUDGET_CLASS_NONE,
        false, false, false, false, false, true);
}

// Demand for a pixel: how far its lighting estimate is from converged. Pixels
// that have never been measured are maximally urgent; pixels at the sample cap
// are not urgent at all.
fn scene_ray_pixel_demand_state(pixel : vec2<u32>) -> SceneRayDemandState {
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
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
    let currentLightHash = sparse_voxel_local_light_hash(
        surfacePosition, settings.reachabilityGeneration.y);
    let history = scene_ray_history_state(pixel, currentLightHash);
    let temporalId = textureLoad(
        voxelIdTexture, vec2<i32>(pixel), 0).x;
    let exactSurfaceIdentity =
        (temporalId & SPARSE_VOXEL_TEMPORAL_ID_EXACT) != 0u;
    let motion = textureLoad(
        motionVectorTexture, vec2<i32>(pixel), 0).xy;
    let motionPixels = length(
        motion * vec2<f32>(dimensions));
    let objectMotionPixels = scene_ray_object_motion_pixels(
        surfacePosition, motion, dimensions);
    let variance = max(history.z - history.y * history.y, 0.0);
    let relativeStandardError =
        sqrt(variance / max(history.x, 1.0)) / max(history.y, 0.05);
    // A receiver is asleep after several independent observations agree. The
    // count/error fallback makes the decision robust when a half-precision
    // reprojection footprint repeatedly blends the consecutive-update lane at
    // voxel edges. A light/scene generation change clears historyValid, while
    // disocclusion or changed identity returns an empty state and wakes it.
    let minimumSamples = max(settings.stabilization.x, 4.0);
    let maximumError = clamp(settings.stabilization.y, 0.01, 1.0);
    let requiredStable = clamp(settings.stabilization.z, 1.0, 255.0);
    let settled = history.x >= minimumSamples &&
        history.w >= requiredStable &&
        relativeStandardError <= maximumError;
    let regionAwake = scene_ray_region_is_awake(
        pixel, settings.sampling.x);
    // Brick receivers carry an exact cell id. If reprojection found the same
    // cell/face, camera motion alone is not a lighting change and must not
    // re-noise an already converged surface. Coarser resolver identities keep
    // the conservative motion gate because one id can cover multiple cells.
    let effectiveMotionPixels = select(
        motionPixels, objectMotionPixels, exactSurfaceIdentity);
    if (settled && !regionAwake && effectiveMotionPixels <= 0.25) {
        return scene_ray_empty_demand_state();
    }
    // Demand for more evidence is relative to the threshold that actually lets
    // this receiver sleep, not to the light field's EMA capacity. Those are
    // different questions: the accumulator may be sized for 256 samples of
    // temporal stability while 64 is all the evidence the convergence test
    // asks for. Measuring against the capacity left every receiver reporting
    // three quarters demand forever, so no tile could ever report converged
    // and the budget had no low-priority work to deprioritise.
    let convergedSamples = minimumSamples;
    // Hold full demand for the first few samples of a newly revealed receiver.
    //
    // The sparse-rate gate is bypassed only at demand 1.0, and demand falls
    // below one the moment a pixel has a single sample: at half rate its
    // second sample then arrives with probability demand^3 / 2, so a
    // disocclusion collects roughly 0.7 samples per frame instead of one and
    // needs half again as many frames to reach the convergence count - at
    // quarter rate, more than twice as many. That is the couple of seconds a
    // newly revealed area takes to settle. Newly revealed receivers are sparse
    // and short-lived, so tracing them at full rate until they have real
    // evidence costs little and removes the interaction entirely.
    let freshSamples = min(4.0, convergedSamples);
    let countDemand = select(
        1.0 - clamp(history.x / convergedSamples, 0.0, 1.0),
        1.0,
        history.x < freshSamples);
    let noiseDemand = smoothstep(0.20, 0.60, relativeStandardError);
    let motionDemand = select(
        smoothstep(0.25, 2.0, motionPixels),
        smoothstep(0.25, 2.0, objectMotionPixels),
        exactSurfaceIdentity && history.x > 0.0);
    let regionDemand = select(
        0.0, 0.75, regionAwake);
    let demand =
        max(max(max(countDemand, noiseDemand), motionDemand), regionDemand);

    // A count of exactly zero under validated reprojection is the same signal
    // the light field uses for disocclusion: motion landed outside the frame,
    // or no reprojected tap carried this receiver's identity.
    let reprojecting =
        (settings.sampling.y & SCENE_RAY_MODE_REPROJECT_DEMAND) != 0u &&
        (settings.sampling.y & SCENE_RAY_MODE_HISTORY_VALID) != 0u;
    let disoccluded = reprojecting && history.x <= 0.0;
    let moving = motionDemand > 0.0;
    let noisy = noiseDemand > 0.0;
    let unconverged = countDemand > 0.0;

    // Priority order: newly revealed geometry, then moving geometry, then a
    // detected shadow/lighting change, then measured noise, then plain lack of
    // evidence. The first matching class wins outright; the scalar demand only
    // orders receivers inside one class.
    var priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_UNCONVERGED;
    if (disoccluded) {
        priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_DISOCCLUSION;
    } else if (moving) {
        priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_MOTION;
    } else if (regionAwake) {
        priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_LIGHTING;
    } else if (noisy) {
        priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_NOISE;
    }
    return SceneRayDemandState(
        demand, priorityClass, disoccluded, moving, noisy, unconverged,
        regionAwake, false);
}

fn scene_ray_pixel_demand(pixel : vec2<u32>) -> f32 {
    return scene_ray_pixel_demand_state(pixel).demand;
}

fn scene_ray_pixel_is_full_rate_edge(pixel : vec2<u32>) -> bool {
    let dimensions = settings.dimensionsGeneration.xy;
    let width = min(
        settings.tileGrid.z,
        min(dimensions.x, dimensions.y) / 2u);
    if (width == 0u) { return false; }
    return pixel.x < width || pixel.y < width ||
        pixel.x >= dimensions.x - width ||
        pixel.y >= dimensions.y - width;
}

fn scene_ray_pixel_reprojects_outside_viewport(pixel : vec2<u32>) -> bool {
    if ((settings.sampling.y & SCENE_RAY_MODE_HISTORY_VALID) == 0u) {
        return false;
    }
    let dimensions = settings.dimensionsGeneration.xy;
    let uv = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(dimensions);
    let motion = textureLoad(
        motionVectorTexture, vec2<i32>(pixel), 0).xy;
    let historyUv = uv - motion;
    return any(historyUv < vec2<f32>(0.0)) ||
        any(historyUv >= vec2<f32>(1.0));
}

fn scene_ray_pixel_requires_full_rate(pixel : vec2<u32>) -> bool {
    if (settings.tileGrid.z == 0u) { return false; }
    return scene_ray_pixel_is_full_rate_edge(pixel) ||
        scene_ray_pixel_reprojects_outside_viewport(pixel);
}

fn scene_ray_pixel_is_scheduled(pixel : vec2<u32>, frame : u32) -> bool {
    // Reprojection has no valid source beyond the previous viewport. A full
    // rate minimum border covers the filter footprint; the motion-derived
    // test expands it as far as a fast camera turn requires.
    if (scene_ray_pixel_requires_full_rate(pixel)) {
        return true;
    }
    let window = clamp(settings.rotation.y, 1u, 16u);
    if (window <= 1u) {
        return true;
    }
    let block = pixel / window;
    let blockSeed = scene_ray_hash_bits(
        block.x * 0x8da6b343u ^ block.y * 0xd8163841u);
    let phaseCount = window * window;
    let phase = (frame + (blockSeed & (phaseCount - 1u))) &
        (phaseCount - 1u);
    var selected = vec2<u32>(0u);
    if (window <= 8u) {
        selected = scene_ray_ordered_coord(phase) / (8u / window);
    } else {
        // Four nested 8x8 sequences cover the quadrants of a 16x16 window.
        // The combined 8-bit phase visits all 256 receivers before repeating.
        let quadrantIndex = phase >> 6u;
        let quadrant = vec2<u32>(
            quadrantIndex & 1u, quadrantIndex >> 1u);
        selected = quadrant * 8u +
            scene_ray_ordered_coord(phase & 63u);
    }
    return all((pixel % window) == selected);
}

// Legacy diagnostic visible-voxel tracing mode. Exact brick-face identities
// elect a single canonical receiver within each small screen tile. Completed
// visibility is receiver-specific and is never shared with the other pixels,
// so production rendering leaves this mode disabled and uses the explicit
// full-resolution schedule above. The helpers remain for ABI experiments.
fn scene_ray_visible_receiver_hash(identity : vec2<u32>) -> u32 {
    var value = identity.x ^ (identity.y * 0x9e3779b9u);
    value = (value ^ (value >> 16u)) * 0x7feb352du;
    value = (value ^ (value >> 15u)) * 0x846ca68bu;
    value = value ^ (value >> 16u);
    if (value == 0u || value == 0xfffffffeu) {
        value = value ^ 0xa511e9b3u;
    }
    return max(value, 1u);
}

// 0: table miss (use the local-tile fallback), 1: reject, 2: elected.
fn scene_ray_visible_receiver_decision(
    pixel : vec2<u32>, identity : vec2<u32>) -> u32 {
    let capacity = settings.visibleReceiverMap.x;
    let stride = max(settings.visibleReceiverMap.y, 14u);
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    if (capacity == 0u || pixelIndex >= 0x00ffffffu ||
        capacity * stride > arrayLength(&visibleReceiverMap)) {
        return 0u;
    }
    let hash = scene_ray_visible_receiver_hash(identity);
    for (var probe = 0u; probe < 8u; probe += 1u) {
        let entry = (hash + probe * probe) & (capacity - 1u);
        let base = entry * stride;
        if (visibleReceiverMap[base] != hash ||
            visibleReceiverMap[base + 1u] !=
                settings.visibleReceiverMap.z ||
            visibleReceiverMap[base + 2u] != identity.x ||
            visibleReceiverMap[base + 3u] != identity.y) {
            continue;
        }
        let minimum = vec2<u32>(
            visibleReceiverMap[base + 4u],
            visibleReceiverMap[base + 5u]);
        let maximum = vec2<u32>(
            visibleReceiverMap[base + 6u],
            visibleReceiverMap[base + 7u]);
        let projectedArea =
            (maximum.x - minimum.x + 1u) *
            (maximum.y - minimum.y + 1u);
        let representativeCount = select(1u, 5u, projectedArea > 4u);
        // A highly clipped/non-convex projection may have no pixel in the
        // one-pixel center neighbourhood. Fall back to the local tile mode in
        // that rare case instead of rejecting the whole face.
        if (visibleReceiverMap[base + 9u] == 0xffffffffu) { return 0u; }
        for (var sample = 0u; sample < representativeCount; sample += 1u) {
            if ((visibleReceiverMap[base + 9u + sample] & 0x00ffffffu) ==
                pixelIndex) {
                return 2u;
            }
        }
        return 1u;
    }
    return 0u;
}

fn scene_ray_is_voxel_tile_representative(pixel : vec2<u32>) -> bool {
    if (settings.reachabilityGeneration.z == 0u) { return true; }
    let dimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let temporalId = textureLoad(
        voxelIdTexture, vec2<i32>(pixel), 0).x;
    if ((temporalId & SPARSE_VOXEL_TEMPORAL_ID_EXACT) == 0u) {
        return true;
    }
    let tileWidth = clamp(settings.reachabilityGeneration.w, 1u, 4u);
    let tileOrigin = (pixel / tileWidth) * tileWidth;
    let normal = textureLoad(
        geometryNormalTexture, vec2<i32>(pixel), 0).xyz;
    let normalMagnitude = abs(normal);
    var faceAxis = 0u;
    if (normalMagnitude.y > normalMagnitude.x) { faceAxis = 1u; }
    if (normalMagnitude.z > max(normalMagnitude.x, normalMagnitude.y)) {
        faceAxis = 2u;
    }
    let faceClass = faceAxis | select(0u, 4u, normal[faceAxis] < 0.0);
    let instance = textureLoad(
        instanceIdTexture, vec2<i32>(pixel), 0).x;
    let identity = vec2<u32>(
        (instance & ((1u << 29u) - 1u)) | (faceClass << 29u),
        temporalId);
    let mapDecision = scene_ray_visible_receiver_decision(pixel, identity);
    if (mapDecision != 0u) { return mapDecision == 2u; }
    var representative = pixel;
    var representativeIndex = pixel.y * dimensions.x + pixel.x;
    for (var y = 0u; y < tileWidth; y = y + 1u) {
        for (var x = 0u; x < tileWidth; x = x + 1u) {
            let candidate = tileOrigin + vec2<u32>(x, y);
            if (any(candidate >= dimensions)) { continue; }
            let candidateI = vec2<i32>(candidate);
            let candidateNormal = textureLoad(
                geometryNormalTexture, candidateI, 0).xyz;
            let candidateMagnitude = abs(candidateNormal);
            var candidateAxis = 0u;
            if (candidateMagnitude.y > candidateMagnitude.x) {
                candidateAxis = 1u;
            }
            if (candidateMagnitude.z >
                max(candidateMagnitude.x, candidateMagnitude.y)) {
                candidateAxis = 2u;
            }
            let candidateFace = candidateAxis |
                select(0u, 4u, candidateNormal[candidateAxis] < 0.0);
            let candidateInstance = textureLoad(
                instanceIdTexture, candidateI, 0).x;
            let candidateIdentity = vec2<u32>(
                (candidateInstance & ((1u << 29u) - 1u)) |
                    (candidateFace << 29u),
                textureLoad(voxelIdTexture, candidateI, 0).x);
            let candidateIndex = candidate.y * dimensions.x + candidate.x;
            if (all(candidateIdentity == identity) &&
                candidateIndex < representativeIndex) {
                representative = candidate;
                representativeIndex = candidateIndex;
            }
        }
    }
    return all(pixel == representative);
}

fn scene_ray_transform_vector(
    matrix : mat4x4<f32>, value : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(value, 0.0)).xyz;
}

// An emitter sample together with the measure its pdf is expressed in.
// solidAnglePdf is zero when the sampler fell back to uniform area sampling,
// in which case areaPdf already holds the area-measure density; otherwise the
// caller converts solidAnglePdf into area measure once the final ray geometry
// is known, so the conversion uses exactly the distance the target will use.
struct SceneEmitterSample {
    position : vec3<f32>,
    areaPdf : f32,
    solidAnglePdf : f32,
}

fn scene_orthonormal_basis(normal : vec3<f32>) -> mat3x3<f32> {
    let sign = select(-1.0, 1.0, normal.z >= 0.0);
    let a = -1.0 / (sign + normal.z);
    let b = normal.x * normal.y * a;
    return mat3x3<f32>(
        vec3<f32>(1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x),
        vec3<f32>(b, sign + normal.y * normal.y * a, -normal.y),
        normal);
}

// Uniform sampling of the cone the sphere subtends at the receiver.
//
// Uniform *area* sampling of a sphere is what made spherical area lights
// fire: the resulting estimator carries a raw cos_emitter * area / d^2 that
// diverges as the receiver approaches the emitter, and half of every sample
// batch landed on the back hemisphere and shaded to black. Sampling the
// visible cap in solid angle removes both problems - after the area-measure
// conversion the RIS weight reduces to power * cos_receiver / solidAnglePdf,
// with no inverse-square term left to blow up.
fn scene_sample_sphere_light(
    light : WorldGILight, receiver : vec3<f32>,
    u0 : f32, u1 : f32) -> SceneEmitterSample {
    let radius = max(light.shapeParams.x, 0.0);
    let center = light.positionRange.xyz;
    let toCenter = center - receiver;
    let centerDistance = length(toCenter);
    // Degenerate radius, or a receiver inside the sphere, where no cone
    // encloses the emitter. Fall back to uniform area sampling.
    if (radius <= 1.0e-6 || centerDistance <= radius * 1.0001) {
        let z = u0 * 2.0 - 1.0;
        let phi = u1 * SCENE_RAY_TAU;
        let radial = sqrt(max(1.0 - z * z, 0.0));
        let point = center +
            vec3<f32>(cos(phi) * radial, z, sin(phi) * radial) * radius;
        let areaPdf = select(
            1.0, 1.0 / (2.0 * SCENE_RAY_TAU * radius * radius),
            radius > 1.0e-6);
        return SceneEmitterSample(point, areaPdf, 0.0);
    }
    let axis = toCenter / centerDistance;
    let basis = scene_orthonormal_basis(axis);
    let sinThetaMaxSq = (radius * radius) / (centerDistance * centerDistance);
    let cosThetaMax = sqrt(max(1.0 - sinThetaMaxSq, 0.0));
    let cosTheta = 1.0 - u0 * (1.0 - cosThetaMax);
    let sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    let phi = u1 * SCENE_RAY_TAU;
    let direction = normalize(
        basis * vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta));
    // Near intersection of the sampled direction with the sphere. The
    // discriminant is clamped so a direction grazing the silhouette lands on
    // the silhouette rather than missing the sphere entirely.
    let projected = centerDistance * cosTheta;
    let halfChord = sqrt(
        max(radius * radius - centerDistance * centerDistance * sinTheta * sinTheta, 0.0));
    let hitDistance = max(projected - halfChord, 1.0e-6);
    let point = receiver + direction * hitDistance;
    return SceneEmitterSample(
        point, 0.0, 1.0 / (SCENE_RAY_TAU * max(1.0 - cosThetaMax, 1.0e-9)));
}

// Spherical rectangle sampling (Urena, Fajardo, King 2013). Samples the solid
// angle the quad subtends at the receiver uniformly, which is what removes the
// inverse-square tail that uniform area sampling of a quad produces.
fn scene_sample_planar_light(
    light : WorldGILight, receiver : vec3<f32>,
    u0 : f32, u1 : f32) -> SceneEmitterSample {
    let basis = worldgi_planar_light_basis(light);
    let halfWidth = max(light.shapeParams.y, 0.0);
    let halfHeight = max(light.shapeParams.z, 0.0);
    let area = 4.0 * halfWidth * halfHeight;
    let center = light.positionRange.xyz;
    let areaPdf = select(1.0, 1.0 / area, area > 1.0e-6);
    let areaSample = SceneEmitterSample(
        center +
            basis.tangent * ((u0 * 2.0 - 1.0) * halfWidth) +
            basis.bitangent * ((u1 * 2.0 - 1.0) * halfHeight),
        areaPdf, 0.0);
    if (area <= 1.0e-6) { return areaSample; }

    var xAxis = basis.tangent;
    var yAxis = basis.bitangent;
    var zAxis = cross(xAxis, yAxis);
    let corner = center - xAxis * halfWidth - yAxis * halfHeight;
    let toCorner = corner - receiver;
    var z0 = dot(toCorner, zAxis);
    // Orient the local frame so the receiver is on the +z side of the quad.
    if (z0 > 0.0) {
        zAxis = -zAxis;
        z0 = -z0;
    }
    let x0 = dot(toCorner, xAxis);
    let y0 = dot(toCorner, yAxis);
    let x1 = x0 + 2.0 * halfWidth;
    let y1 = y0 + 2.0 * halfHeight;
    let z0sq = z0 * z0;

    let v00 = vec3<f32>(x0, y0, z0);
    let v01 = vec3<f32>(x0, y1, z0);
    let v10 = vec3<f32>(x1, y0, z0);
    let v11 = vec3<f32>(x1, y1, z0);
    let n0 = normalize(cross(v00, v10));
    let n1 = normalize(cross(v10, v11));
    let n2 = normalize(cross(v11, v01));
    let n3 = normalize(cross(v01, v00));
    let g0 = acos(clamp(-dot(n0, n1), -1.0, 1.0));
    let g1 = acos(clamp(-dot(n1, n2), -1.0, 1.0));
    let g2 = acos(clamp(-dot(n2, n3), -1.0, 1.0));
    let g3 = acos(clamp(-dot(n3, n0), -1.0, 1.0));
    let b0 = n0.z;
    let b1 = n2.z;
    let k = SCENE_RAY_TAU - g2 - g3;
    let solidAngle = g0 + g1 - k;
    // A receiver almost in the plane of the quad subtends a vanishing solid
    // angle and the parametrisation loses all precision. Area sampling is
    // well behaved there because the emitter cosine is what goes to zero.
    if (!(solidAngle > 1.0e-6) || abs(z0) <= 1.0e-6) { return areaSample; }

    let au = u0 * solidAngle + k;
    let sinAu = sin(au);
    if (abs(sinAu) <= 1.0e-9) { return areaSample; }
    let fu = (cos(au) * b0 - b1) / sinAu;
    let cuMagnitude = 1.0 / sqrt(fu * fu + b0 * b0);
    var cu = clamp(select(-cuMagnitude, cuMagnitude, fu >= 0.0), -1.0, 1.0);
    let cuComplement = sqrt(max(1.0 - cu * cu, 0.0));
    if (cuComplement <= 1.0e-9) { return areaSample; }
    let xu = clamp(-(cu * z0) / cuComplement, x0, x1);

    let planeDistance = sqrt(xu * xu + z0sq);
    let h0 = y0 / sqrt(planeDistance * planeDistance + y0 * y0);
    let h1 = y1 / sqrt(planeDistance * planeDistance + y1 * y1);
    let hv = h0 + u1 * (h1 - h0);
    let hvSq = hv * hv;
    let yv = select(
        y1,
        (hv * planeDistance) / sqrt(max(1.0 - hvSq, 1.0e-12)),
        hvSq < 1.0 - 1.0e-6);

    return SceneEmitterSample(
        receiver + xAxis * xu + yAxis * yv + zAxis * z0,
        0.0, 1.0 / solidAngle);
}

fn scene_invalid_candidate(
    sourcePixel : vec2<u32>, candidateSlot : u32) -> SparseVoxelSceneRay {
    return SparseVoxelSceneRay(
        vec4<f32>(0.0), vec4<f32>(0.0),
        vec4<u32>(sourcePixel.x, sourcePixel.y, candidateSlot, 0u),
        vec4<u32>(0u), vec4<f32>(0.0));
}

fn scene_generate_candidate(
    sourcePixel : vec2<u32>, surfacePosition : vec3<f32>,
    receiverNormal : vec3<f32>, candidateSlot : u32,
    candidateIndex : u32, skyLightIndex : u32, regionKey : u32,
    rayOrigin : vec3<f32>, analyticReceiverVisibility : bool,
    fallbackLightDirection : vec3<f32>)
    -> SparseVoxelSceneRay {
    let seed = (candidateIndex * 747796405u +
        settings.dimensionsGeneration.w) ^
        (settings.sampling.x * 2891336453u);
    // With more than one visibility lane, slot zero is a dedicated sky
    // stratum. The remaining lanes sample only non-sky emitters. Letting the
    // sky compete with a bright sun or local emitter in a single reservoir
    // gave many receivers no hemispherical visibility observation at all;
    // their light-field history then converged to flat, unoccluded ambient.
    // Keeping the strata separate guarantees one AO sample while preserving
    // unbiased light selection inside the non-sky stratum.
    let dedicatedSky = settings.rotation.w > 1u && candidateSlot == 0u;
    let receiverFlags =
        SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE |
        SPARSE_VOXEL_SCENE_RAY_FLAG_SHADOW |
        sparse_voxel_receiver_face_flags(receiverNormal);
    // Sky mode emits one fresh cosine-sampled occlusion ray per pixel every
    // frame. The sky is a hemispheric integral: pure per-frame Monte Carlo
    // plus the downstream visibility mean is its correct estimator, while
    // reservoir reuse would hold a single direction for many frames and
    // converge whole regions to that direction's binary occlusion.
    if ((settings.sampling.y & SCENE_RAY_MODE_SKY) != 0u || dedicatedSky) {
        if (skyLightIndex == 0xffffffffu) {
            return scene_invalid_candidate(sourcePixel, candidateSlot);
        }
        let skySample = scene_ray_sample_guided_sky(
            receiverNormal, sourcePixel,
            candidateIndex * 747796405u + settings.dimensionsGeneration.w,
            settings.sampling.x);
        return SparseVoxelSceneRay(
            vec4<f32>(rayOrigin, 0.0),
            vec4<f32>(skySample.direction, select(
                settings.lightDirectionMaximumDistance.w, 0.0,
                analyticReceiverVisibility)),
            vec4<u32>(sourcePixel.x, sourcePixel.y, candidateSlot, skyLightIndex),
            vec4<u32>(candidateIndex * 747796405u + candidateSlot,
                receiverFlags, settings.dimensionsGeneration.w, regionKey),
            vec4<f32>(1.0, skySample.pdf, 1.0, 0.0));
    }
    let selection = scene_select_light_bvh(
        surfacePosition, receiverNormal, regionKey, seed,
        settings.rotation.w > 1u);
    let lightIndex = selection.lightIndex;
    if (lightIndex == 0xffffffffu || lightIndex >= arrayLength(&lights)) {
        return scene_invalid_candidate(sourcePixel, candidateSlot);
    }
    let light = lights[lightIndex];
    var lightDirection = fallbackLightDirection;
    var maximumDistance = settings.lightDirectionMaximumDistance.w;
    // Area-measure density of the emitter sample, or a solid-angle density
    // that is converted to area measure below once the final ray geometry is
    // known. Infinite lights carry an implicit density of one.
    var emitterAreaPdf = 1.0;
    var emitterSolidAnglePdf = 0.0;
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        let directionalLength = length(light.directionType.xyz);
        if (directionalLength > 1.0e-6) {
            lightDirection = scene_sample_directional_disk(
                light, vec2<f32>(
                    scene_ray_hash(seed), scene_ray_hash(seed + 1u)));
        }
    } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
        let skySample = scene_ray_sample_guided_sky(
            receiverNormal, sourcePixel,
            candidateIndex * 747796405u + settings.dimensionsGeneration.w,
            settings.sampling.x);
        lightDirection = skySample.direction;
        emitterAreaPdf = skySample.pdf;
    } else {
        var emitter = SceneEmitterSample(light.positionRange.xyz, 1.0, 0.0);
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
            emitter = scene_sample_sphere_light(
                light, surfacePosition,
                scene_ray_hash(seed), scene_ray_hash(seed + 1u));
        } else if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
            emitter = scene_sample_planar_light(
                light, surfacePosition,
                scene_ray_hash(seed), scene_ray_hash(seed + 1u));
        }
        emitterAreaPdf = emitter.areaPdf;
        emitterSolidAnglePdf = emitter.solidAnglePdf;
        let toLight = emitter.position - surfacePosition;
        let distanceToLight = length(toLight);
        if (distanceToLight <= 1.0e-6) {
            return scene_invalid_candidate(sourcePixel, candidateSlot);
        }
        lightDirection = toLight / distanceToLight;
        // Preserve the actual sampled emitter point. Screen tracing may clamp
        // its own march distance, while Tree64/merge need the full segment to
        // classify scene and coverage escapes and reuse needs to reconnect to
        // the original emitter sample.
        maximumDistance = distanceToLight;
    }
    // Keep the ray on the reconstructed receiver. Direction- and LOD-dependent
    // origin motion changes every stochastic sample and was visible as
    // flickering terrain seams at close range.
    // Far receivers are lit analytically. Past a couple of kilometres a shadow
    // ray costs a full clipmap traversal to decide something no longer
    // resolvable on screen, so the ray keeps its direction, its selected light
    // and its weight - everything the estimator needs - and loses only its
    // span. Nothing traces a zero-length ray, the merge reports a miss, and a
    // miss is precisely unshadowed sky and sun. It therefore blends into the
    // traced result through the ordinary accumulation rather than as a second
    // lighting model with its own exposure.
    let analyticDistance = settings.stabilization.w;
    if (analyticDistance > 0.0 &&
        distance(surfacePosition, raster.cameraWorldPositionNear.xyz) >
            analyticDistance) {
        maximumDistance = 0.0;
    }
    // Reserved for receivers that explicitly opt out of traced visibility.
    // Generated terrain no longer uses this escape hatch: the Tree64 terrain
    // instances carry TREE64_DYNAMIC_SKIP_RECEIVER_CELL, so the tracer can skip
    // the authored receiver cell while still testing mountains and other
    // distant terrain as hard-shadow occluders.
    if (analyticReceiverVisibility) {
        maximumDistance = 0.0;
    }
    var ray = SparseVoxelSceneRay(
        vec4<f32>(rayOrigin, 0.0),
        vec4<f32>(lightDirection, maximumDistance),
        vec4<u32>(sourcePixel.x, sourcePixel.y, candidateSlot, lightIndex),
        vec4<u32>(candidateIndex * 747796405u + candidateSlot,
            receiverFlags, settings.dimensionsGeneration.w, regionKey),
        vec4<f32>(0.0));
    // Evaluate the target through the shared helper, on the ray exactly as it
    // will be stored. The temporal and spatial passes recompute the target
    // from the stored ray, so deriving it here from any other intermediate
    // would let the three disagree.
    let targetProxy = sparse_voxel_di_target(light, ray, receiverNormal);
    var emitterPdf = emitterAreaPdf;
    if (emitterSolidAnglePdf > 0.0) {
        // The reservoir resamples in area measure, so a solid-angle proposal
        // has to be converted: p_A = p_omega * cos_emitter / d^2. Both factors
        // are computed from the same floored cosine and the same squared
        // distance the target uses, so they cancel exactly and the candidate
        // weight reduces to power * cos_receiver / p_omega - bounded, with no
        // inverse-square term left.
        let sampledPoint = ray.originMinimumDistance.xyz +
            lightDirection * maximumDistance;
        emitterPdf = emitterSolidAnglePdf *
            sparse_voxel_di_emitter_cosine_floored(
                light, lightDirection, sampledPoint) /
            max(maximumDistance * maximumDistance, 1.0e-4);
    }
    ray.lightSamplePdfWeight =
        vec4<f32>(selection.selectionPdf, emitterPdf, targetProxy, 0.0);
    return ray;
}

// One invocation owns a raster pixel and emits all of its visibility lanes.
// Receiver reconstruction, history reprojection, and scheduling therefore
// stay O(pixels), while only actual proposal generation scales with the
// Raster pixel a ray-domain slot represents.
//
// This legacy helper can map a coarse ray slot to one raster receiver. The
// production per-pixel ReSTIR path keeps both domains at native resolution:
// rotating a coarse slot cannot supply visibility to the rest of its block,
// because a completed shadow ray is valid only at the selected receiver.
fn scene_ray_source_pixel(
    pixel : vec2<u32>, rayDimensions : vec2<u32>,
    sourceDimensions : vec2<u32>) -> vec2<u32> {
    // Stable pixel centres also cover fractional scales such as 0.75.
    return min(vec2<u32>((vec2<f32>(pixel) + vec2<f32>(0.5)) *
        vec2<f32>(sourceDimensions) / vec2<f32>(max(rayDimensions, vec2<u32>(1u)))),
        sourceDimensions - vec2<u32>(1u));
}

// configured proposal budget.
@compute @workgroup_size(8, 8, 1)
fn createSparseVoxelSceneRays(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    let dimensions = settings.dimensionsGeneration.xy;
    if (any(pixel >= dimensions)) { return; }
    let raysPerPixel = max(settings.rotation.w, 1u);
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    let sourceDimensions = max(
        settings.sampling.zw, vec2<u32>(1u));
    let sourcePixel = scene_ray_source_pixel(
        pixel, dimensions, sourceDimensions);
    let sourceIndex =
        sourcePixel.y * sourceDimensions.x + sourcePixel.x;
    let covered =
        sourceIndex < arrayLength(&coverage) &&
        sourceIndex < arrayLength(&linearDepth) &&
        coverage[sourceIndex] != 0u;
    var shouldGenerate = covered;
    var demand = 0.0;
    if (covered) {
        demand = scene_ray_pixel_demand(sourcePixel);
        if (!scene_ray_pixel_is_scheduled(pixel, settings.sampling.x)) {
            if ((settings.sampling.y &
                    SCENE_RAY_MODE_REPROJECT_DEMAND) == 0u) {
                shouldGenerate = false;
            } else if (demand < 1.0) {
                let extraProbability =
                    (demand * demand * demand) /
                    f32(scene_ray_schedule_phase_count());
                let extra = scene_ray_hash(
                    (pixelIndex * 2246822519u) ^
                    (settings.sampling.x * 0x9e3779b9u));
                shouldGenerate = extra < extraProbability;
            }
            // else: demand >= 1.0 (zero history) with demand mode active;
            // bypass scheduling to give disoccluded pixels an immediate ray.
        }
        let refreshInterval = settings.originBiasReserved.w;
        if (shouldGenerate && demand <= 0.0) {
            shouldGenerate = refreshInterval > 0.0 &&
                (scene_ray_is_periodic_refresh(
                     pixel, settings.sampling.x) ||
                 scene_ray_is_region_sentinel(
                     pixel, settings.sampling.x));
        }
        if (shouldGenerate &&
            !scene_ray_is_voxel_tile_representative(pixel)) {
            shouldGenerate = false;
        }
    }

    var surfacePosition = vec3<f32>(0.0);
    var receiverNormal = vec3<f32>(0.0, 1.0, 0.0);
    var analyticReceiverVisibility = false;
    if (shouldGenerate) {
        let cameraDirection = vec3<f32>(
            (f32(sourcePixel.x) + 0.5 - raster.focalPrincipal.z) /
                raster.focalPrincipal.x,
            (f32(sourcePixel.y) + 0.5 - raster.focalPrincipal.w) /
                raster.focalPrincipal.y,
            1.0);
        let worldDirection = normalize(
            scene_ray_transform_vector(raster.cameraToWorld, cameraDirection));
        surfacePosition = raster.cameraWorldPositionNear.xyz +
            worldDirection * linearDepth[sourceIndex];
        let geometryNormalSample = textureLoad(
            geometryNormalTexture, vec2<i32>(sourcePixel), 0);
        receiverNormal = sparse_voxel_safe_normalize(
            geometryNormalSample.xyz,
            vec3<f32>(0.0, 1.0, 0.0));
        // The normal-buffer alpha is material/normal sidecar data (currently
        // roughness on the shipped G-buffer paths), not a visibility policy.
        // Treating it as an analytic-terrain sentinel made otherwise valid
        // low-alpha receivers emit zero-length rays, erasing hard terrain
        // shadows before Tree64 ever saw them.
        analyticReceiverVisibility = false;
    }

    let candidateCount = max(settings.dimensionsGeneration.z, 1u);
    let baseOutputIndex = pixelIndex * raysPerPixel;
    var reservoirSelected = scene_invalid_candidate(sourcePixel, 0u);
    var reservoirSelectedSlot = 0xffffffffu;
    var reservoirWeightSum = 0.0;
    var reservoirM = 0u;
    // One bit per emitted lane, set only when that lane carries a ray a
    // downstream pass can actually trace. Reserving the full raysPerPixel span
    // instead made every ray-domain pass dispatch over scheduledPixels *
    // raysPerPixel and discard the dead slots via the active-flag test, so the
    // "exact" indirect count was really an upper bound.
    var activeLaneMask = 0u;

    // Hoist the sky light search out of the per-candidate loop. The scan
    // over all lights to find the first enabled sky is O(lights) and its
    // result is the same for every candidate a pixel emits.
    var skyLightIndex = 0xffffffffu;
    if (shouldGenerate &&
        ((settings.sampling.y & SCENE_RAY_MODE_SKY) != 0u ||
         settings.rotation.w > 1u)) {
        let skyLightCount = min(lightBVHInfo.lightCount, arrayLength(&lights));
        for (var i = 0u; i < skyLightCount; i = i + 1u) {
            if (lights[i].flags.y == WORLD_GI_LIGHT_TYPE_SKY &&
                scene_light_is_enabled(lights[i])) {
                skyLightIndex = i;
                break;
            }
        }
    }

    if (shouldGenerate) {
        // These values are receiver invariants. With eight ReSTIR proposals,
        // computing them in scene_generate_candidate duplicated the voxel-exit
        // loop, reachability hash, and fallback normalization eight times.
        let regionKey = sparse_voxel_reachability_key(
            surfacePosition, receiverNormal, settings.reachability.x);
        let rayOrigin = surfacePosition;
        let fallbackLightDirection = normalize(
            settings.lightDirectionMaximumDistance.xyz);
        // Evaluate the proposal budget once. The first raysPerPixel-1
        // candidates become independent visibility lanes; the remainder
        // stream into lane zero's reusable reservoir.
        for (var candidateSlot = 0u;
             candidateSlot < candidateCount;
             candidateSlot = candidateSlot + 1u) {
            let candidateIndex = pixelIndex * candidateCount + candidateSlot;
            var candidate = scene_generate_candidate(
                sourcePixel, surfacePosition, receiverNormal,
                candidateSlot, candidateIndex, skyLightIndex, regionKey,
                rayOrigin, analyticReceiverVisibility,
                fallbackLightDirection);
            let proposal = max(candidate.lightSamplePdfWeight.x, 0.0) *
                max(candidate.lightSamplePdfWeight.y, 0.0);
            let targetProxy = max(candidate.lightSamplePdfWeight.z, 0.0);
            let candidateValid =
                (candidate.seedFlags.y &
                    SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
                proposal > 0.0 && targetProxy > 0.0;

            if (candidateSlot < raysPerPixel - 1u) {
                let freshLane = candidateSlot + 1u;
                let freshOutputIndex = baseOutputIndex + freshLane;
                if (candidateValid) {
                    let weight = targetProxy / proposal;
                    candidate.pixelRayLight.z = candidateSlot;
                    candidate.lightSamplePdfWeight.w = weight;
                    if (freshOutputIndex < arrayLength(&finalRays)) {
                        finalRays[freshOutputIndex] = candidate;
                        activeLaneMask = activeLaneMask | (1u << freshLane);
                    }
                } else if (freshOutputIndex < arrayLength(&finalRays)) {
                    finalRays[freshOutputIndex].seedFlags.y = 0u;
                }
                continue;
            }

            if (!candidateValid) { continue; }
            let weight = targetProxy / proposal;
            reservoirM = reservoirM + 1u;
            reservoirWeightSum = reservoirWeightSum + weight;
            let replace = scene_ray_hash(
                candidate.seedFlags.x ^
                (baseOutputIndex * 1664525u) ^
                (settings.sampling.x * 0x85ebca6bu)) <
                weight / max(reservoirWeightSum, 1.0e-20);
            if (replace) {
                reservoirSelected = candidate;
                reservoirSelectedSlot = candidateSlot;
            }
        }
    } else {
        for (var freshLane = 1u;
             freshLane < raysPerPixel;
             freshLane = freshLane + 1u) {
            finalRays[baseOutputIndex + freshLane].seedFlags.y = 0u;
        }
    }

    if (reservoirM != 0u && reservoirSelectedSlot != 0xffffffffu) {
        reservoirSelected.pixelRayLight.z =
            (min(reservoirM, 0xffffu) << 16u) |
            (reservoirSelectedSlot & 0xffffu);
        reservoirSelected.lightSamplePdfWeight.w = reservoirWeightSum;
        rays[baseOutputIndex] = reservoirSelected;
    } else {
        rays[baseOutputIndex].seedFlags.y = 0u;
        finalRays[baseOutputIndex].seedFlags.y = 0u;
        if (pixelIndex < arrayLength(&finalReservoirs)) {
            finalReservoirs[pixelIndex].state.z = 0u;
        }
    }

    if (shouldGenerate) {
        // Lane zero stays unconditional. Its record is authored later by the DI
        // reservoir pass, which runs the dense pixel domain and can resurrect a
        // ray from temporal history for a pixel that produced no candidate
        // here - dropping it from the worklist would leave reused samples
        // untraced. Lanes one and up are fully decided at creation, so only
        // those that actually carry a ray are reserved.
        activeLaneMask = activeLaneMask | 1u;
        let activeLaneCount = countOneBits(activeLaneMask);
        let compactBase = atomicAdd(&activeRayState[3], activeLaneCount);
        var emitted = 0u;
        for (var lane = 0u; lane < raysPerPixel; lane = lane + 1u) {
            if ((activeLaneMask & (1u << lane)) == 0u) { continue; }
            let compactIndex = compactBase + emitted;
            if (compactIndex < arrayLength(&activeRayIndices)) {
                activeRayIndices[compactIndex] = baseOutputIndex + lane;
            }
            emitted = emitted + 1u;
        }
    }

    // ReSTIR now dispatches the dense pixel domain, so no reservoir worklist
    // or counter update is needed here.
}

// Candidate-parallel production path ----------------------------------------
//
// A cheap full-screen kernel first builds an exact list of receivers that
// need new proposals. The expensive kernel then assigns four lanes to each of
// 16 scheduled receivers in a 64-lane workgroup, so light-BVH proposals are
// generated in parallel without launching work for sleeping pixels. Keeping
// multiple receivers in each group amortizes the threadgroup scratch and the
// indirect-dispatch overhead on integrated GPUs.
const PARALLEL_RECEIVERS_PER_GROUP = 16u;
const PARALLEL_CANDIDATE_LANES = 4u;
const PARALLEL_MAX_CANDIDATES = 16u;
const PARALLEL_CANDIDATE_SCRATCH =
    PARALLEL_RECEIVERS_PER_GROUP * PARALLEL_CANDIDATE_LANES;
var<workgroup> parallelActiveOffsets : array<u32, 16>;
var<workgroup> parallelActiveBase : u32;
var<workgroup> parallelSurfacePosition : array<vec4<f32>, 16>;
var<workgroup> parallelReceiverNormal : array<vec4<f32>, 16>;
var<workgroup> parallelRayOrigin : array<vec4<f32>, 16>;
var<workgroup> parallelFallbackDirection : array<vec4<f32>, 16>;
var<workgroup> parallelRegionKey : array<u32, 16>;
var<workgroup> parallelSkyLightIndex : array<u32, 16>;
var<workgroup> parallelCandidateOrigin : array<vec4<f32>, PARALLEL_CANDIDATE_SCRATCH>;
var<workgroup> parallelCandidateDirection : array<vec4<f32>, PARALLEL_CANDIDATE_SCRATCH>;
var<workgroup> parallelCandidatePixelLight : array<vec4<u32>, PARALLEL_CANDIDATE_SCRATCH>;
var<workgroup> parallelCandidateSeedFlags : array<vec4<u32>, PARALLEL_CANDIDATE_SCRATCH>;
var<workgroup> parallelCandidatePdfWeight : array<vec4<f32>, PARALLEL_CANDIDATE_SCRATCH>;

// Whether a receiver may compete for a ray this frame, together with the
// classified demand the tile budget ranks it by. Eligibility is exactly the
// legacy admission test: coverage, the sparse-schedule phase with its
// high-demand escape, the sentinel heartbeat, and the visible-voxel
// representative filter. The budget only chooses among receivers that would
// already have been allowed to trace.
struct SceneRayEligibility {
    covered : bool,
    eligible : bool,
    sentinel : bool,
    forced : bool,
    demand : SceneRayDemandState,
}

fn scene_ray_eligibility(
    pixel : vec2<u32>, pixelIndex : u32,
    sourcePixel : vec2<u32>, sourceIndex : u32) -> SceneRayEligibility {
    let covered =
        sourceIndex < arrayLength(&coverage) &&
        sourceIndex < arrayLength(&linearDepth) &&
        coverage[sourceIndex] != 0u;
    if (!covered) {
        return SceneRayEligibility(
            false, false, false, false, scene_ray_empty_demand_state());
    }

    let state = scene_ray_pixel_demand_state(sourcePixel);
    let demand = state.demand;
    let fullRateEdge = scene_ray_pixel_requires_full_rate(pixel);
    var shouldGenerate = true;
    if (!scene_ray_pixel_is_scheduled(pixel, settings.sampling.x)) {
        if ((settings.sampling.y &
                SCENE_RAY_MODE_REPROJECT_DEMAND) == 0u) {
            shouldGenerate = false;
        } else if (demand < 1.0) {
            let extraProbability =
                (demand * demand * demand) /
                f32(scene_ray_schedule_phase_count());
            let extra = scene_ray_hash(
                (pixelIndex * 2246822519u) ^
                (settings.sampling.x * 0x9e3779b9u));
            shouldGenerate = extra < extraProbability;
        }
    }
    // Zero interval disables the heartbeat inside the election itself.
    let sentinel = scene_ray_is_region_sentinel(
        pixel, settings.sampling.x);
    if (shouldGenerate && demand <= 0.0 && !fullRateEdge) {
        shouldGenerate = sentinel || scene_ray_is_periodic_refresh(
            pixel, settings.sampling.x);
    }
    if (shouldGenerate && !fullRateEdge &&
        !scene_ray_is_voxel_tile_representative(pixel)) {
        shouldGenerate = false;
    }
    var classified = state;
    if (classified.priorityClass == SPARSE_VOXEL_RAY_BUDGET_CLASS_NONE &&
        shouldGenerate) {
        classified.priorityClass = SPARSE_VOXEL_RAY_BUDGET_CLASS_HEARTBEAT;
    }
    return SceneRayEligibility(
        true, shouldGenerate, sentinel, fullRateEdge, classified);
}

fn scene_ray_should_generate(
    pixel : vec2<u32>, pixelIndex : u32,
    sourcePixel : vec2<u32>, sourceIndex : u32) -> bool {
    return scene_ray_eligibility(
        pixel, pixelIndex, sourcePixel, sourceIndex).eligible;
}

// Adaptive tile-budget scheduler.
//
// One workgroup owns one screen tile. Every lane evaluates a handful of the
// tile's receivers, the workgroup reduces that into a single tile record, and
// the same workgroup then elects exactly as many receivers as the tile's quota
// allows. Selection is a 64-bucket workgroup histogram scanned from the highest
// priority downwards, so ranking costs O(pixels + 64) with no global sort and
// no second full-screen pass. Redistribution reads the previous frame's totals,
// which the double-buffered header makes free of any extra barrier.
const TILE_BUDGET_GROUP_SIZE : u32 = 256u;
// 256 lanes times 16 receivers covers the largest supported 64x64 tile.
const TILE_BUDGET_MAX_PIXELS_PER_LANE : u32 = 16u;
const TILE_BUDGET_RECORD_INVALID : u32 = 0xffffffffu;
// Selection groups: demand classes share one quota, the heartbeat class keeps
// its own small reserve so a settled tile never stops validating itself.
const TILE_BUDGET_GROUP_DEMAND : u32 = 0u;
const TILE_BUDGET_GROUP_HEARTBEAT : u32 = 1u;

var<workgroup> tileHistogram :
    array<atomic<u32>, SPARSE_VOXEL_RAY_BUDGET_BUCKETS>;
var<workgroup> tileCoveredPixels : atomic<u32>;
var<workgroup> tileDisoccludedPixels : atomic<u32>;
var<workgroup> tileMovingPixels : atomic<u32>;
var<workgroup> tileNoisyPixels : atomic<u32>;
var<workgroup> tileDemandSum : atomic<u32>;
var<workgroup> tileMaximumDemand : atomic<u32>;
var<workgroup> tileWakeMask : atomic<u32>;
var<workgroup> tileEligiblePixels : atomic<u32>;
var<workgroup> tileCutoffBucket : array<u32, 2>;
var<workgroup> tileCutoffSlots : array<u32, 2>;
var<workgroup> tileCutoffClaims : array<atomic<u32>, 2>;
var<workgroup> tileQuota : u32;
// Partition the production schedule counters so 256 covered receivers do
// not serialize on one workgroup atomic. No subgroup capability is required.
const DIRECT_SCHEDULE_BANKS : u32 = 8u;
var<workgroup> directCovered : array<atomic<u32>, DIRECT_SCHEDULE_BANKS>;
var<workgroup> directScheduled : array<atomic<u32>, DIRECT_SCHEDULE_BANKS>;
var<workgroup> directBase : array<u32, DIRECT_SCHEDULE_BANKS>;
var<workgroup> tileScheduledCount : atomic<u32>;
var<workgroup> tileScheduledBase : u32;

fn scene_ray_budget_tile_base(tileIndex : u32) -> u32 {
    return SPARSE_VOXEL_RAY_BUDGET_HEADER_WORDS +
        tileIndex * SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE;
}

fn scene_ray_budget_bank_base(frame : u32) -> u32 {
    return (frame & 1u) * SPARSE_VOXEL_RAY_BUDGET_BANK_WORDS;
}

fn scene_ray_budget_bound() -> bool {
    return settings.tileBudget.w != 0.0 &&
        arrayLength(&rayTileBudget) >
            SPARSE_VOXEL_RAY_BUDGET_HEADER_WORDS;
}

fn scene_ray_budget_add_stat(frame : u32, stat : u32, value : u32) {
    if (value == 0u) { return; }
    atomicAdd(&rayTileBudget[scene_ray_budget_bank_base(frame) + stat], value);
}

// Priority key: the class in the high bits, a jittered quantisation of the
// scalar demand in the low bits. The per-frame jitter rotates which receivers
// of an equally urgent tile win the quota, so a starved tile does not keep
// re-tracing the same fixed subset.
fn scene_ray_budget_bucket(
    demand : f32, priorityClass : u32, pixelIndex : u32,
    frame : u32) -> u32 {
    let jitter = scene_ray_hash(
        (pixelIndex * 0x27220a95u) ^ (frame * 0x9e3779b9u));
    let step = min(u32(floor(
        clamp(demand, 0.0, 1.0) *
            f32(SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS - 1u) + jitter)),
        SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS - 1u);
    return priorityClass * SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS + step;
}

fn scene_ray_budget_clear_pixel(pixelIndex : u32, raysPerPixel : u32) {
    let baseOutputIndex = pixelIndex * raysPerPixel;
    if (baseOutputIndex < arrayLength(&rays)) {
        rays[baseOutputIndex].seedFlags.y = 0u;
    }
    for (var lane = 0u; lane < raysPerPixel; lane += 1u) {
        let outputIndex = baseOutputIndex + lane;
        if (outputIndex < arrayLength(&finalRays)) {
            finalRays[outputIndex].seedFlags.y = 0u;
        }
    }
    if (pixelIndex < arrayLength(&finalReservoirs)) {
        finalReservoirs[pixelIndex].state.z = 0u;
    }
}

// Descending scan over one contiguous bucket range. Returns the lowest bucket
// any slot was granted to; every pixel above it is admitted outright and the
// pixels exactly in it compete for the leftover slots.
fn scene_ray_budget_cutoff(
    highBucket : i32, lowBucket : i32, budget : u32,
    group : u32) -> u32 {
    var remaining = budget;
    var cutoff = TILE_BUDGET_RECORD_INVALID;
    var slots = 0u;
    for (var bucket = highBucket; bucket >= lowBucket; bucket -= 1) {
        if (remaining == 0u) { break; }
        let count = atomicLoad(&tileHistogram[u32(bucket)]);
        if (count == 0u) { continue; }
        cutoff = u32(bucket);
        slots = min(count, remaining);
        remaining -= slots;
    }
    tileCutoffBucket[group] = cutoff;
    tileCutoffSlots[group] = slots;
    return budget - remaining;
}

@compute @workgroup_size(256, 1, 1)
fn scheduleSparseVoxelSceneRayPixels(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let frame = settings.sampling.x;
    let dimensions = settings.dimensionsGeneration.xy;
    let sourceDimensions = max(settings.sampling.zw, vec2<u32>(1u));
    // The budget grid is the regional stability grid: the wake state the demand
    // classifier reads is indexed with exactly these tiles. The extent takes
    // the larger of the two domains so that a ray grid wider than the source
    // still has every one of its slots visited and cleared.
    let tileSize = max(settings.tileGrid.x, 1u);
    let tileDimensions =
        (max(sourceDimensions, dimensions) + vec2<u32>(tileSize - 1u)) /
        tileSize;
    let tileCount = tileDimensions.x * tileDimensions.y;
    let tileIndex = workgroupId.x;
    let raysPerPixel = max(settings.rotation.w, 1u);
    let budgetBound = scene_ray_budget_bound();

    if (localIndex < DIRECT_SCHEDULE_BANKS) {
        atomicStore(&directCovered[localIndex], 0u);
        atomicStore(&directScheduled[localIndex], 0u);
    }
    if (localIndex < SPARSE_VOXEL_RAY_BUDGET_BUCKETS) {
        atomicStore(&tileHistogram[localIndex], 0u);
    }
    if (localIndex < 2u) {
        tileCutoffBucket[localIndex] = TILE_BUDGET_RECORD_INVALID;
        tileCutoffSlots[localIndex] = 0u;
        atomicStore(&tileCutoffClaims[localIndex], 0u);
    }
    if (localIndex == 0u) {
        atomicStore(&tileCoveredPixels, 0u);
        atomicStore(&tileDisoccludedPixels, 0u);
        atomicStore(&tileMovingPixels, 0u);
        atomicStore(&tileNoisyPixels, 0u);
        atomicStore(&tileDemandSum, 0u);
        atomicStore(&tileMaximumDemand, 0u);
        atomicStore(&tileWakeMask, 0u);
        atomicStore(&tileEligiblePixels, 0u);
        atomicStore(&tileScheduledCount, 0u);
        tileScheduledBase = 0u;
        tileQuota = 0u;
    }
    workgroupBarrier();
    if (tileIndex >= tileCount) { return; }

    let tileOrigin = vec2<u32>(
        tileIndex % tileDimensions.x,
        tileIndex / tileDimensions.x) * tileSize;
    let pixelsInTile = tileSize * tileSize;
    let laneIterations = max(
        (pixelsInTile + TILE_BUDGET_GROUP_SIZE - 1u) /
            TILE_BUDGET_GROUP_SIZE,
        1u);

    // The production sparse-window baseline has no adaptive tile budget. Do
    // not send its already-exact schedule through a second histogram/quota
    // election: at one receiver per 16x16 tile that election could reduce a
    // valid singleton to zero. Reserve one compact range per tile instead of
    // contending on a global atomic for every selected receiver.
    if (settings.tileBudget.x <= 0.0 &&
        (settings.sampling.y & SCENE_RAY_MODE_REPROJECT_DEMAND) == 0u) {
        let bank = localIndex / (TILE_BUDGET_GROUP_SIZE / DIRECT_SCHEDULE_BANKS);
        var directSlots : array<u32, TILE_BUDGET_MAX_PIXELS_PER_LANE>;
        for (var slot = 0u; slot < laneIterations;
             slot += 1u) {
            directSlots[slot] = TILE_BUDGET_RECORD_INVALID;
            let local = localIndex + slot * TILE_BUDGET_GROUP_SIZE;
            if (local >= pixelsInTile) { continue; }
            let pixel = tileOrigin +
                vec2<u32>(local % tileSize, local / tileSize);
            if (any(pixel >= dimensions)) { continue; }
            let pixelIndex = pixel.y * dimensions.x + pixel.x;
            let sourcePixel = scene_ray_source_pixel(
                pixel, dimensions, sourceDimensions);
            let sourceIndex =
                sourcePixel.y * sourceDimensions.x + sourcePixel.x;
            let covered = sourceIndex < arrayLength(&coverage) &&
                sourceIndex < arrayLength(&linearDepth) &&
                coverage[sourceIndex] != 0u;
            if (covered) {
                atomicAdd(&directCovered[bank], 1u);
            }
            let scheduled = covered && scene_ray_pixel_is_scheduled(
                pixel, settings.sampling.x);
            if (!scheduled) {
                scene_ray_budget_clear_pixel(pixelIndex, raysPerPixel);
                continue;
            }
            directSlots[slot] = atomicAdd(&directScheduled[bank], 1u);
        }
        workgroupBarrier();
        if (localIndex == 0u) {
            var covered = 0u;
            var scheduled = 0u;
            for (var counter = 0u; counter < DIRECT_SCHEDULE_BANKS; counter += 1u) {
                directBase[counter] = scheduled;
                covered += atomicLoad(&directCovered[counter]);
                scheduled += atomicLoad(&directScheduled[counter]);
            }
            atomicStore(&tileCoveredPixels, covered);
            atomicStore(&tileScheduledCount, scheduled);
            tileScheduledBase = atomicAdd(
                &reservoirRayState[3], atomicLoad(&tileScheduledCount));
        }
        workgroupBarrier();
        for (var slot = 0u; slot < laneIterations; slot += 1u) {
            if (directSlots[slot] == TILE_BUDGET_RECORD_INVALID) { continue; }
            let scheduledIndex = tileScheduledBase + directBase[bank] + directSlots[slot];
            let local = localIndex + slot * TILE_BUDGET_GROUP_SIZE;
            let pixel = tileOrigin + vec2<u32>(local % tileSize, local / tileSize);
            if (scheduledIndex < arrayLength(&reservoirRayIndices)) {
                reservoirRayIndices[scheduledIndex] = pixel.y * dimensions.x + pixel.x;
            }
        }
        // Keep the optional budget telemetry truthful even though no budget
        // election was performed. In particular, a zero here used to make a
        // healthy direct schedule indistinguishable from the singleton-rounds-
        // to-zero failure that this path exists to prevent.
        if (localIndex == 0u && budgetBound) {
            let covered = atomicLoad(&tileCoveredPixels);
            let scheduled = atomicLoad(&tileScheduledCount);
            let base = scene_ray_budget_tile_base(tileIndex);
            if (base + SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE <=
                arrayLength(&rayTileBudget)) {
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_COVERED], covered);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_DISOCCLUDED], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_MOVING], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_NOISY], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_DEMAND], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_CONFIDENCE], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_QUOTA], scheduled);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_SCHEDULED], scheduled);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_RESOLVED], 0u);
                atomicStore(
                    &rayTileBudget[base +
                        SPARSE_VOXEL_RAY_BUDGET_TILE_VISIBILITY_CHANGES], 0u);
            }
            if (covered != 0u) {
                scene_ray_budget_add_stat(
                    frame, SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_TILES, 1u);
                scene_ray_budget_add_stat(
                    frame, SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_PIXELS,
                    covered);
                scene_ray_budget_add_stat(
                    frame, SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_BUDGET,
                    scheduled);
                scene_ray_budget_add_stat(
                    frame, SPARSE_VOXEL_RAY_BUDGET_STAT_RAYS_SCHEDULED,
                    scheduled);
            }
        }
        return;
    }

    // Stage one: classify every receiver in the tile and reduce it into the
    // workgroup counters and the priority histogram.
    var laneRecord : array<u32, 16>;
    for (var slot = 0u; slot < laneIterations;
         slot += 1u) {
        laneRecord[slot] = TILE_BUDGET_RECORD_INVALID;
        let local = localIndex + slot * TILE_BUDGET_GROUP_SIZE;
        if (local >= pixelsInTile) { continue; }
        let pixel = tileOrigin +
            vec2<u32>(local % tileSize, local / tileSize);
        if (any(pixel >= dimensions)) { continue; }
        let pixelIndex = pixel.y * dimensions.x + pixel.x;
        let sourcePixel = scene_ray_source_pixel(
        pixel, dimensions, sourceDimensions);
        let sourceIndex =
            sourcePixel.y * sourceDimensions.x + sourcePixel.x;
        let admission = scene_ray_eligibility(
            pixel, pixelIndex, sourcePixel, sourceIndex);
        if (!admission.covered) {
            scene_ray_budget_clear_pixel(pixelIndex, raysPerPixel);
            continue;
        }
        let state = admission.demand;
        atomicAdd(&tileCoveredPixels, 1u);
        if (state.disoccluded) { atomicAdd(&tileDisoccludedPixels, 1u); }
        if (state.moving) { atomicAdd(&tileMovingPixels, 1u); }
        if (state.noisy || state.unconverged) {
            atomicAdd(&tileNoisyPixels, 1u);
        }
        let fixedDemand = u32(
            clamp(state.demand, 0.0, 1.0) *
            SPARSE_VOXEL_RAY_BUDGET_DEMAND_SCALE);
        atomicAdd(&tileDemandSum, fixedDemand);
        atomicMax(&tileMaximumDemand, fixedDemand);
        var wake = 0u;
        if (state.disoccluded) {
            wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_DISOCCLUSION;
        }
        if (state.moving) { wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_MOTION; }
        if (state.regionAwake) {
            wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_LIGHTING;
        }
        if (state.noisy) { wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_NOISE; }
        if (state.unconverged) {
            wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_UNCONVERGED;
        }
        if (admission.sentinel) {
            wake |= SPARSE_VOXEL_RAY_BUDGET_WAKE_HEARTBEAT;
        }
        atomicOr(&tileWakeMask, wake);
        if (!admission.eligible) {
            scene_ray_budget_clear_pixel(pixelIndex, raysPerPixel);
            continue;
        }
        let bucket = scene_ray_budget_bucket(
            state.demand, state.priorityClass, pixelIndex, frame);
        atomicAdd(&tileHistogram[bucket], 1u);
        atomicAdd(&tileEligiblePixels, 1u);
        laneRecord[slot] = (bucket << 2u) |
            select(0u, 1u, admission.sentinel) |
            select(0u, 2u, admission.forced);
    }
    workgroupBarrier();

    // Stage two: turn the tile's need into a quota. The reference is the
    // previous frame's mean tile demand, so a tile that is noisier than the
    // frame average is granted more than its equal share and a settled tile
    // falls back to its heartbeat reserve, with the frame total staying inside
    // the configured fraction of the eligible receivers.
    let eligible = atomicLoad(&tileEligiblePixels);
    if (localIndex == 0u) {
        let demandSum = atomicLoad(&tileDemandSum);
        let coveredPixels = atomicLoad(&tileCoveredPixels);
        var weight = 1.0;
        if (budgetBound && coveredPixels != 0u) {
            let previousBank = scene_ray_budget_bank_base(frame + 1u);
            let stamp = atomicLoad(
                &rayTileBudget[previousBank +
                    SPARSE_VOXEL_RAY_BUDGET_STAT_FRAME]);
            // The stamp is biased by one, so the bank written last frame
            // reads back as exactly this frame's index.
            if (stamp == frame && frame != 0u) {
                let previousPixels = atomicLoad(
                    &rayTileBudget[previousBank +
                        SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_PIXELS]);
                let previousDemand = atomicLoad(
                    &rayTileBudget[previousBank +
                        SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_DEMAND]);
                if (previousPixels != 0u && previousDemand != 0u) {
                    // Both sides are mean demand per covered receiver, so a
                    // tile at the frame average scores exactly one however
                    // much of it the geometry covers. A frame-wide rise in
                    // demand lifts the reference too, which is what keeps the
                    // loop from spending its way out of a noisy frame.
                    let frameMean =
                        f32(previousDemand) / f32(previousPixels);
                    let tileMean = f32(demandSum) / f32(coveredPixels);
                    weight = tileMean / max(frameMean, 1.0e-4);
                }
            }
        }
        let fraction = clamp(settings.tileBudget.x, 0.0, 1.0);
        let boost = max(settings.tileBudget.y, 1.0);
        // A zero fraction disables redistribution and admits every eligible
        // receiver, which is exactly the legacy schedule. The tile record and
        // the statistics bank are still produced, so the budget a given scene
        // would have used is measurable before it is enforced.
        var demandQuota = eligible;
        var heartbeatReserve = eligible;
        if (fraction > 0.0) {
            // The pool is sized from covered receivers and the sparse
            // schedule rate, both independent of how noisy the frame is. That
            // is what makes the total a budget rather than a demand-driven
            // ceiling: a tile can only exceed its equal share by taking from
            // tiles that scored below the frame average.
            let baseQuota = f32(coveredPixels) * fraction /
                f32(scene_ray_schedule_phase_count());
            demandQuota = u32(round(
                baseQuota * clamp(weight, 0.0, boost)));
            heartbeatReserve = u32(max(settings.tileBudget.z, 0.0));
        }
        tileQuota = scene_ray_budget_cutoff(
            i32(SPARSE_VOXEL_RAY_BUDGET_CLASS_COUNT *
                SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS) - 1,
            i32(SPARSE_VOXEL_RAY_BUDGET_CLASS_UNCONVERGED *
                SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS),
            min(demandQuota, eligible), TILE_BUDGET_GROUP_DEMAND) +
            scene_ray_budget_cutoff(
                i32(SPARSE_VOXEL_RAY_BUDGET_CLASS_UNCONVERGED *
                    SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS) - 1,
                i32(SPARSE_VOXEL_RAY_BUDGET_CLASS_HEARTBEAT *
                    SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS),
                heartbeatReserve, TILE_BUDGET_GROUP_HEARTBEAT);
    }
    workgroupBarrier();

    // Stage three: elect receivers up to the quota. Buckets above the cutoff
    // are admitted outright; the cutoff bucket itself is oversubscribed and
    // resolved by a workgroup atomic, which is where the per-frame jitter in
    // the bucket key keeps the choice rotating.
    var laneSlot : array<u32, 16>;
    for (var slot = 0u; slot < laneIterations;
         slot += 1u) {
        laneSlot[slot] = TILE_BUDGET_RECORD_INVALID;
        let record = laneRecord[slot];
        if (record == TILE_BUDGET_RECORD_INVALID) { continue; }
        let local = localIndex + slot * TILE_BUDGET_GROUP_SIZE;
        let pixel = tileOrigin +
            vec2<u32>(local % tileSize, local / tileSize);
        let pixelIndex = pixel.y * dimensions.x + pixel.x;
        let bucket = record >> 2u;
        let forced = (record & 2u) != 0u;
        let group = select(
            TILE_BUDGET_GROUP_DEMAND, TILE_BUDGET_GROUP_HEARTBEAT,
            bucket < SPARSE_VOXEL_RAY_BUDGET_CLASS_UNCONVERGED *
                SPARSE_VOXEL_RAY_BUDGET_DEMAND_STEPS);
        let cutoff = tileCutoffBucket[group];
        var selected = forced ||
            (cutoff != TILE_BUDGET_RECORD_INVALID &&
             bucket > cutoff);
        if (!selected && bucket == cutoff) {
            // The cutoff bucket is oversubscribed by construction. Claiming a
            // slot with a monotonically rising counter keeps the comparison
            // free of the underflow a countdown would risk.
            selected = atomicAdd(&tileCutoffClaims[group], 1u) <
                tileCutoffSlots[group];
        }
        if (!selected) {
            scene_ray_budget_clear_pixel(pixelIndex, raysPerPixel);
            continue;
        }
        laneSlot[slot] = atomicAdd(&tileScheduledCount, 1u);
    }
    workgroupBarrier();

    if (localIndex == 0u) {
        tileScheduledBase = atomicAdd(
            &reservoirRayState[3], atomicLoad(&tileScheduledCount));
    }
    workgroupBarrier();

    for (var slot = 0u; slot < laneIterations;
         slot += 1u) {
        if (laneSlot[slot] == TILE_BUDGET_RECORD_INVALID) { continue; }
        let scheduledIndex = tileScheduledBase + laneSlot[slot];
        if (scheduledIndex >= arrayLength(&reservoirRayIndices)) { continue; }
        let local = localIndex + slot * TILE_BUDGET_GROUP_SIZE;
        let pixel = tileOrigin +
            vec2<u32>(local % tileSize, local / tileSize);
        reservoirRayIndices[scheduledIndex] =
            (pixel.y * dimensions.x + pixel.x) |
            select(0u, 0x80000000u, (laneRecord[slot] & 1u) != 0u);
    }

    // Stage four: publish the tile record and fold it into this frame's
    // statistics bank. One lane, a handful of atomics per tile.
    if (localIndex != 0u || !budgetBound) { return; }
    let covered = atomicLoad(&tileCoveredPixels);
    let scheduled = atomicLoad(&tileScheduledCount);
    let wakeMask = atomicLoad(&tileWakeMask);
    let base = scene_ray_budget_tile_base(tileIndex);
    if (base + SPARSE_VOXEL_RAY_BUDGET_WORDS_PER_TILE >
        arrayLength(&rayTileBudget)) { return; }
    let disoccluded = atomicLoad(&tileDisoccludedPixels);
    let moving = atomicLoad(&tileMovingPixels);
    let noisy = atomicLoad(&tileNoisyPixels);
    let demandSum = atomicLoad(&tileDemandSum);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_COVERED], covered);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_DISOCCLUDED],
        disoccluded);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_MOVING], moving);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_NOISY], noisy);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_DEMAND], demandSum);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_CONFIDENCE],
        (atomicLoad(&tileMaximumDemand) & 0x00ffffffu) | (wakeMask << 24u));
    let effectiveQuota = max(tileQuota, scheduled);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_QUOTA],
        effectiveQuota);
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_SCHEDULED],
        scheduled);
    // The light-field resolve accumulates into these after every pass that
    // consumes this frame's rays, so the scheduler owns clearing them.
    atomicStore(
        &rayTileBudget[base + SPARSE_VOXEL_RAY_BUDGET_TILE_RESOLVED], 0u);
    atomicStore(
        &rayTileBudget[base +
            SPARSE_VOXEL_RAY_BUDGET_TILE_VISIBILITY_CHANGES], 0u);

    if (covered == 0u) { return; }
    scene_ray_budget_add_stat(
        frame, SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_TILES, 1u);
    scene_ray_budget_add_stat(
        frame, SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_DEMAND, demandSum);
    scene_ray_budget_add_stat(
        frame, SPARSE_VOXEL_RAY_BUDGET_STAT_COVERED_PIXELS, covered);
    scene_ray_budget_add_stat(
        frame, SPARSE_VOXEL_RAY_BUDGET_STAT_TOTAL_BUDGET, effectiveQuota);
    scene_ray_budget_add_stat(
        frame, SPARSE_VOXEL_RAY_BUDGET_STAT_RAYS_SCHEDULED, scheduled);
    // A tile counts as awake when a meaningful share of it still wants work.
    // A strict "any pixel" rule is useless at this granularity: one noisy
    // receiver out of 256 would keep every tile permanently awake and the
    // statistic would never distinguish a settled region from a busy one.
    let unstableMask = SPARSE_VOXEL_RAY_BUDGET_WAKE_DISOCCLUSION |
        SPARSE_VOXEL_RAY_BUDGET_WAKE_MOTION |
        SPARSE_VOXEL_RAY_BUDGET_WAKE_LIGHTING |
        SPARSE_VOXEL_RAY_BUDGET_WAKE_NOISE |
        SPARSE_VOXEL_RAY_BUDGET_WAKE_UNCONVERGED;
    let meanDemand = f32(demandSum) /
        (f32(covered) * SPARSE_VOXEL_RAY_BUDGET_DEMAND_SCALE);
    if ((wakeMask & unstableMask) != 0u &&
        meanDemand > SPARSE_VOXEL_RAY_BUDGET_AWAKE_DEMAND) {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_TILES_AWAKE, 1u);
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_UNSTABLE_RAYS, scheduled);
    } else {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_TILES_CONVERGED, 1u);
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_HEARTBEAT_RAYS, scheduled);
    }
    // Attribute the tile's rays to its dominant wake reason. Exact per-ray
    // attribution would need a second reduction; the dominant reason is what a
    // budget dashboard actually reads.
    if ((wakeMask & SPARSE_VOXEL_RAY_BUDGET_WAKE_DISOCCLUSION) != 0u) {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_DISOCCLUSION_RAYS, scheduled);
    } else if ((wakeMask & SPARSE_VOXEL_RAY_BUDGET_WAKE_MOTION) != 0u) {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_MOTION_RAYS, scheduled);
    } else if ((wakeMask & SPARSE_VOXEL_RAY_BUDGET_WAKE_LIGHTING) != 0u) {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_LIGHTING_RAYS, scheduled);
    } else if ((wakeMask & SPARSE_VOXEL_RAY_BUDGET_WAKE_NOISE) != 0u) {
        scene_ray_budget_add_stat(
            frame, SPARSE_VOXEL_RAY_BUDGET_STAT_NOISE_RAYS, scheduled);
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeSparseVoxelSceneRayPixels(
    @builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x != 0u) { return; }
    atomicStore(
        &reservoirRayState[0],
        (atomicLoad(&reservoirRayState[3]) +
            PARALLEL_RECEIVERS_PER_GROUP - 1u) /
            PARALLEL_RECEIVERS_PER_GROUP);
    atomicStore(&reservoirRayState[1], 1u);
    atomicStore(&reservoirRayState[2], 1u);
    if (!scene_ray_budget_bound()) { return; }
    let frame = settings.sampling.x;
    // Stamp the bank this frame just wrote, then clear the bank the next frame
    // will accumulate into. That bank is the one this frame read from, and the
    // schedule pass ahead of us has already consumed it.
    let currentBank = scene_ray_budget_bank_base(frame);
    atomicStore(
        &rayTileBudget[currentBank + SPARSE_VOXEL_RAY_BUDGET_STAT_FRAME],
        frame + 1u);
    let nextBank = scene_ray_budget_bank_base(frame + 1u);
    for (var word = 0u; word < SPARSE_VOXEL_RAY_BUDGET_BANK_WORDS;
         word += 1u) {
        atomicStore(&rayTileBudget[nextBank + word], 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn createSparseVoxelSceneRaysParallel(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let receiverLane = localIndex / PARALLEL_CANDIDATE_LANES;
    let candidateLane = localIndex & (PARALLEL_CANDIDATE_LANES - 1u);
    let scheduledIndex =
        workgroupId.x * PARALLEL_RECEIVERS_PER_GROUP + receiverLane;
    let scheduledCount = atomicLoad(&reservoirRayState[3]);
    let receiverValid =
        scheduledIndex < scheduledCount &&
        scheduledIndex < arrayLength(&reservoirRayIndices);
    var encodedPixelIndex = 0u;
    if (receiverValid) {
        encodedPixelIndex = reservoirRayIndices[scheduledIndex];
    }
    let regionSentinel = (encodedPixelIndex & 0x80000000u) != 0u;
    var pixelIndex = encodedPixelIndex & 0x7fffffffu;
    let dimensions = settings.dimensionsGeneration.xy;
    let pixelCount = dimensions.x * dimensions.y;
    let pixelValid = receiverValid && pixelIndex < pixelCount;
    pixelIndex = select(0u, pixelIndex, pixelValid);
    let pixel = vec2<u32>(
        pixelIndex % dimensions.x, pixelIndex / dimensions.x);
    let sourceDimensions = max(settings.sampling.zw, vec2<u32>(1u));
    let sourcePixel = scene_ray_source_pixel(
        pixel, dimensions, sourceDimensions);
    let sourceIndex =
        sourcePixel.y * sourceDimensions.x + sourcePixel.x;

    if (candidateLane == 0u && pixelValid) {
        let cameraDirection = vec3<f32>(
            (f32(sourcePixel.x) + 0.5 - raster.focalPrincipal.z) /
                raster.focalPrincipal.x,
            (f32(sourcePixel.y) + 0.5 - raster.focalPrincipal.w) /
                raster.focalPrincipal.y,
            1.0);
        let worldDirection = normalize(
            scene_ray_transform_vector(
                raster.cameraToWorld, cameraDirection));
        parallelSurfacePosition[receiverLane] = vec4<f32>(
            raster.cameraWorldPositionNear.xyz +
                worldDirection * linearDepth[sourceIndex], 0.0);
        parallelReceiverNormal[receiverLane] = vec4<f32>(
            sparse_voxel_safe_normalize(
            textureLoad(
                geometryNormalTexture, vec2<i32>(sourcePixel), 0).xyz,
            vec3<f32>(0.0, 1.0, 0.0)), 0.0);
        parallelRegionKey[receiverLane] = sparse_voxel_reachability_key(
            parallelSurfacePosition[receiverLane].xyz,
            parallelReceiverNormal[receiverLane].xyz,
            settings.reachability.x);
        parallelRayOrigin[receiverLane] = vec4<f32>(
            parallelSurfacePosition[receiverLane].xyz,
            textureLoad(
                geometryNormalTexture, vec2<i32>(sourcePixel), 0).w);
        parallelFallbackDirection[receiverLane] = vec4<f32>(
            normalize(settings.lightDirectionMaximumDistance.xyz), 0.0);
        parallelSkyLightIndex[receiverLane] = 0xffffffffu;
        if ((settings.sampling.y & SCENE_RAY_MODE_SKY) != 0u ||
            settings.rotation.w > 1u) {
            let skyLightCount = min(
                lightBVHInfo.lightCount, arrayLength(&lights));
            for (var i = 0u; i < skyLightCount; i += 1u) {
                if (lights[i].flags.y == WORLD_GI_LIGHT_TYPE_SKY &&
                    scene_light_is_enabled(lights[i])) {
                    parallelSkyLightIndex[receiverLane] = i;
                    break;
                }
            }
        }
    }
    workgroupBarrier();

    let candidateCount = min(max(
        settings.dimensionsGeneration.z, 1u), PARALLEL_MAX_CANDIDATES);
    let raysPerPixel = max(settings.rotation.w, 1u);
    let baseOutputIndex = pixelIndex * raysPerPixel;
    var activeLaneMask = 0u;
    var reservoirSelected = scene_invalid_candidate(sourcePixel, 0u);
    var reservoirSelectedSlot = 0xffffffffu;
    var reservoirWeightSum = 0.0;
    var reservoirM = 0u;
    // Stream four proposals at a time through 5 KiB of shared scratch. The
    // former 20 KiB allocation paid for all sixteen proposals even when the
    // default four were requested, limiting concurrent workgroups on Apple.
    // Owner lanes merge in the same candidate order and preserve RIS draws.
    for (var candidateBase = 0u; candidateBase < candidateCount;
         candidateBase += PARALLEL_CANDIDATE_LANES) {
        let candidateSlot = candidateBase + candidateLane;
        var generated = scene_invalid_candidate(
            sourcePixel, candidateSlot);
        if (pixelValid && candidateSlot < candidateCount) {
            let candidateIndex =
                pixelIndex * candidateCount + candidateSlot;
            generated = scene_generate_candidate(
                sourcePixel,
                parallelSurfacePosition[receiverLane].xyz,
                parallelReceiverNormal[receiverLane].xyz,
                candidateSlot, candidateIndex,
                parallelSkyLightIndex[receiverLane],
                parallelRegionKey[receiverLane],
                parallelRayOrigin[receiverLane].xyz,
                parallelRayOrigin[receiverLane].w < 0.5,
                parallelFallbackDirection[receiverLane].xyz);
            if (regionSentinel) {
                generated.seedFlags.y |=
                    SPARSE_VOXEL_SCENE_RAY_FLAG_REGION_SENTINEL;
            }
        }
        let scratchIndex =
            receiverLane * PARALLEL_CANDIDATE_LANES + candidateLane;
        parallelCandidateOrigin[scratchIndex] =
            generated.originMinimumDistance;
        parallelCandidateDirection[scratchIndex] =
            generated.directionMaximumDistance;
        parallelCandidatePixelLight[scratchIndex] = generated.pixelRayLight;
        parallelCandidateSeedFlags[scratchIndex] = generated.seedFlags;
        parallelCandidatePdfWeight[scratchIndex] =
            generated.lightSamplePdfWeight;
        workgroupBarrier();
        if (candidateLane == 0u && pixelValid) {
            for (var candidateSlot = candidateBase;
                 candidateSlot < min(candidateBase + PARALLEL_CANDIDATE_LANES, candidateCount);
                 candidateSlot += 1u) {
                let scratchIndex =
                    receiverLane * PARALLEL_CANDIDATE_LANES + candidateSlot - candidateBase;
                var candidate = SparseVoxelSceneRay(
                    parallelCandidateOrigin[scratchIndex],
                    parallelCandidateDirection[scratchIndex],
                    parallelCandidatePixelLight[scratchIndex],
                    parallelCandidateSeedFlags[scratchIndex],
                    parallelCandidatePdfWeight[scratchIndex]);
                let proposal = max(candidate.lightSamplePdfWeight.x, 0.0) *
                    max(candidate.lightSamplePdfWeight.y, 0.0);
                let targetProxy = max(candidate.lightSamplePdfWeight.z, 0.0);
                let candidateValid =
                    (candidate.seedFlags.y &
                        SPARSE_VOXEL_SCENE_RAY_FLAG_ACTIVE) != 0u &&
                    proposal > 0.0 && targetProxy > 0.0;
                if (candidateSlot < raysPerPixel - 1u) {
                    let freshLane = candidateSlot + 1u;
                    let freshOutputIndex = baseOutputIndex + freshLane;
                    if (candidateValid) {
                        let weight = targetProxy / proposal;
                        candidate.pixelRayLight.z = candidateSlot;
                        candidate.lightSamplePdfWeight.w = weight;
                        if (freshOutputIndex < arrayLength(&finalRays)) {
                            finalRays[freshOutputIndex] = candidate;
                            activeLaneMask |= 1u << freshLane;
                        }
                    } else if (freshOutputIndex < arrayLength(&finalRays)) {
                        finalRays[freshOutputIndex].seedFlags.y = 0u;
                    }
                    continue;
                }
                if (!candidateValid) { continue; }
                let weight = targetProxy / proposal;
                reservoirM += 1u;
                reservoirWeightSum += weight;
                let replace = scene_ray_hash(
                    candidate.seedFlags.x ^
                    (baseOutputIndex * 1664525u) ^
                    (settings.sampling.x * 0x85ebca6bu)) <
                    weight / max(reservoirWeightSum, 1.0e-20);
                if (replace) {
                    reservoirSelected = candidate;
                    reservoirSelectedSlot = candidateSlot;
                }
            }

        }
        // Readers must finish before the next four proposals overwrite them.
        workgroupBarrier();
    }
    if (candidateLane == 0u && pixelValid) {
        if (reservoirM != 0u && reservoirSelectedSlot != 0xffffffffu) {
            reservoirSelected.pixelRayLight.z =
                (min(reservoirM, 0xffffu) << 16u) |
                (reservoirSelectedSlot & 0xffffu);
            reservoirSelected.lightSamplePdfWeight.w = reservoirWeightSum;
            rays[baseOutputIndex] = reservoirSelected;
        } else {
            rays[baseOutputIndex].seedFlags.y = 0u;
            finalRays[baseOutputIndex].seedFlags.y = 0u;
            if (pixelIndex < arrayLength(&finalReservoirs)) {
                finalReservoirs[pixelIndex].state.z = 0u;
            }
        }

        activeLaneMask |= 1u;
    }
    if (candidateLane == 0u) {
        parallelActiveOffsets[receiverLane] = countOneBits(activeLaneMask);
    }
    workgroupBarrier();
    if (localIndex == 0u) {
        var total = 0u;
        for (var receiver = 0u; receiver < PARALLEL_RECEIVERS_PER_GROUP; receiver += 1u) {
            let count = parallelActiveOffsets[receiver];
            parallelActiveOffsets[receiver] = total;
            total += count;
        }
        parallelActiveBase = atomicAdd(&activeRayState[3], total);
    }
    workgroupBarrier();
    if (candidateLane != 0u || !pixelValid) { return; }
    let compactBase = parallelActiveBase + parallelActiveOffsets[receiverLane];
    var emitted = 0u;
    for (var lane = 0u; lane < raysPerPixel; lane += 1u) {
        if ((activeLaneMask & (1u << lane)) == 0u) { continue; }
        let compactIndex = compactBase + emitted;
        if (compactIndex < arrayLength(&activeRayIndices)) {
            activeRayIndices[compactIndex] = baseOutputIndex + lane;
        }
        emitted += 1u;
    }
}
