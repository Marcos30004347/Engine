#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_probe_common.wgsl"
#include "worldgi_ddgi_common.wgsl"

// Probe interpolation: SH probes become the full-resolution lighting field.
//
// This is a drop-in replacement for the per-pixel light field resolve. It
// writes the same texture set, so SVGF, compose and the temporal upscaler are
// untouched by the mode switch and the graph simply stops building the passes
// that used to produce these.
//
// Each pixel gathers the four probes whose tiles surround it and blends their
// irradiance with a plane-distance weight. Weighting by distance to the probe's
// *plane* rather than to the probe point is what stops a probe leaking across a
// depth discontinuity: a probe on a wall behind this pixel may be centimetres
// away in screen space but is far off the pixel's surface plane.
//
// The acceptance radius is deliberately not the probe's perpendicular tile
// footprint. A tile on a surface seen at a grazing angle covers a world-space
// span many times that footprint, so a radius derived from depth and focal
// length alone rejects every probe on any floor viewed down its length - and
// rejecting all four is what produced hard tile-shaped blocks. The radius is
// widened by the surface's obliquity, so a probe stays acceptable across
// exactly the region it actually represents.
//
// When even that rejects everything, the fallback takes the best of the four
// candidates rather than the one that happens to sit at the floor of the
// bilinear footprint. That corner probe is frequently the invalid one, and
// taking it unconditionally turned a weighting failure into a black pixel.

struct SparseVoxelProbeInterpolateSettings {
    // xy raster dimensions, z probe tile size, w frame index.
    dimensionsTile : vec4<u32>,
    // x probes across, y probes down, z diagnostics enabled,
    // w normal-coherence SH damping enabled.
    probeCounts : vec4<u32>,
    // x normal threshold, y relative depth threshold, z plane distance scale,
    // w ambient fallback scale.
    weights : vec4<f32>,
    // x normal damping strength, y coherence response threshold,
    // z fallback ambient strength, w fallback ambient radius in probe tiles.
    geometry : vec4<f32>,
    // xy distance-density transitions, z traced-occlusion fade start,
    // w analytic-only cutoff. Only z/w are consumed by interpolation.
    distances : vec4<f32>,
    // x world irradiance cache cascade count, zero when the cache is absent.
    // y replaces the screen-probe gather entirely with a cache query.
    worldCache : vec4<u32>,
}

// Per-pixel gather outcomes, counted globally and over a 4x4 screen grid.
//
// Probe metrics elsewhere are per-*probe* means, and this failure is a
// per-*pixel* hole in the gather: a pixel whose whole neighbourhood is invalid
// can emit black while every probe in the scene reads perfectly healthy.
// No existing number can see it, so the 3x3 fallback widening that was supposed
// to close it shipped unverified. This counts the pixels on that path directly.
//
//   covered  - pixels with geometry, the denominator
//   black    - final gathered irradiance is zero or numerically near zero
//   fallback - the weighted gather failed and a fallback candidate was used
//   widened  - the bounded rescue search ran because it found no valid probe
const PROBE_DIAGNOSTIC_COVERED : u32 = 0u;
const PROBE_DIAGNOSTIC_BLACK : u32 = 1u;
const PROBE_DIAGNOSTIC_FALLBACK : u32 = 2u;
const PROBE_DIAGNOSTIC_WIDENED : u32 = 3u;
const PROBE_DIAGNOSTIC_REGION_AXIS : u32 = 4u;
// Which acceptance test threw a candidate away. The gather has six ways to
// reject a probe and the outcome counters above only say that it did - which
// cannot distinguish "there is no probe here" from "there are probes here and
// every one of them was refused", and those want opposite fixes.
const PROBE_DIAGNOSTIC_REJECT_ORIENTATION : u32 = 4u;
const PROBE_DIAGNOSTIC_REJECT_BILINEAR : u32 = 5u;
const PROBE_DIAGNOSTIC_REJECT_NORMAL : u32 = 6u;
const PROBE_DIAGNOSTIC_REJECT_PLANE : u32 = 7u;
const PROBE_DIAGNOSTIC_REJECT_DEPTH : u32 = 8u;
const PROBE_DIAGNOSTIC_REJECT_VISIBILITY : u32 = 9u;
// Pixels whose G-buffer geometry normal is degenerate, so `probe_normalize`
// substituted straight up. Every probe on a vertical face then has a zero dot
// with it and the orientation test refuses all of them, whatever the search
// radius - which looks identical to "no compatible probe exists" and is not.
const PROBE_DIAGNOSTIC_DEGENERATE_NORMAL : u32 = 10u;
// Mean linear depth over the region, in fixed point. If the gather's every test
// rejects far more in one screen region, either the probes there are wrong or
// the *pixel* is - and a region whose depth is nothing like its neighbours' is
// the cheapest way to tell those apart.
const PROBE_DIAGNOSTIC_DEPTH : u32 = 11u;
// The two skips that run before any acceptance test: the probe index falling
// outside the buffer, and the probe's own VALID bit being clear. Both were
// uninstrumented, so a candidate discarded here was invisible to the rejection
// accounting and looked exactly like "no candidate existed".
const PROBE_DIAGNOSTIC_PROBE_RANGE : u32 = 12u;
const PROBE_DIAGNOSTIC_PROBE_INVALID : u32 = 13u;
const PROBE_DIAGNOSTIC_REGION_WORDS : u32 = 14u;
const PROBE_DIAGNOSTIC_REGIONS : u32 = 14u;
const PROBE_DIAGNOSTIC_WORDS : u32 = 238u;

fn probe_diagnostic_region(pixel : vec2<u32>, dimensions : vec2<u32>) -> u32 {
    let cell = min(
        pixel * PROBE_DIAGNOSTIC_REGION_AXIS / max(dimensions, vec2<u32>(1u)),
        vec2<u32>(PROBE_DIAGNOSTIC_REGION_AXIS - 1u));
    return (cell.y * PROBE_DIAGNOSTIC_REGION_AXIS + cell.x) *
        PROBE_DIAGNOSTIC_REGION_WORDS + PROBE_DIAGNOSTIC_REGIONS;
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> settings : SparseVoxelProbeInterpolateSettings;
@group(0) @binding(2) var<storage, read> coverage : array<u32>;
@group(0) @binding(3) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(4) var<storage, read> probes : array<SparseVoxelProbe>;
@group(0) @binding(5) var<storage, read> probeSH : array<vec4<f32>>;
@group(0) @binding(6) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(7) var shadingNormalTexture : texture_2d<f32>;
@group(0) @binding(8) var instanceIdTexture : texture_2d<u32>;
@group(0) @binding(9) var voxelIdTexture : texture_2d<u32>;
@group(0) @binding(10) var outputFieldTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(11) var outputIdTexture : texture_storage_2d<rg32uint, write>;
@group(0) @binding(12) var reactiveOutput : texture_storage_2d<r16float, write>;
@group(0) @binding(13) var outputMomentsTexture : texture_storage_2d<rg16float, write>;
@group(0) @binding(14) var outputLightHashTexture : texture_storage_2d<r32uint, write>;
@group(0) @binding(15) var<storage, read> adaptiveTiles : array<u32>;
@group(0) @binding(16) var<storage, read> probeRadiance : array<vec4<f32>>;
@group(0) @binding(17) var<storage, read_write> diagnostics : array<atomic<u32>>;
// World irradiance cache. Empty (cascade count zero) unless --ddgi is active.
@group(0) @binding(18) var<storage, read> ddgiVolumes : array<WorldGIDDGIVolume>;
@group(0) @binding(19) var<storage, read> ddgiProbeSH : array<vec4<f32>>;

// Trilinear lookup over the finest cascade that contains the point. This is a
// fixed eight-probe gather with no search and no validity test, which is the
// whole reason it belongs on the failed-gather path: the screen-space fallback
// it replaces walks an 81-tile neighbourhood and its answer changes as probe
// validity churns, which is what makes those pixels alternate frame to frame.
fn worldGICacheIrradiance(
    position : vec3<f32>, normal : vec3<f32>, cascadeCount : u32,
    result : ptr<function, vec3<f32>>) -> bool {
    if (cascadeCount == 0u) {
        return false;
    }
    for (var cascade = 0u; cascade < cascadeCount; cascade = cascade + 1u) {
        if (cascade >= arrayLength(&ddgiVolumes)) {
            return false;
        }
        let volume = ddgiVolumes[cascade];
        let dimensions = volume.dimensionsCascade.xyz;
        let spacing = volume.originSpacing.w;
        if (spacing <= 0.0 || any(dimensions == vec3<u32>(0u))) {
            continue;
        }
        // Offset along the normal so a receiver sitting exactly on a surface
        // does not gather the probe buried behind it.
        let biased = position + normal * spacing * 0.25;
        let local = (biased - volume.originSpacing.xyz) / spacing;
        if (any(local < vec3<f32>(0.0)) ||
            any(local >= vec3<f32>(dimensions) - vec3<f32>(1.0))) {
            continue;
        }
        let corner = vec3<i32>(floor(local));
        let fraction = local - floor(local);
        var accumulated = vec3<f32>(0.0);
        var accumulatedWeight = 0.0;
        for (var offset = 0u; offset < 8u; offset = offset + 1u) {
            let step = vec3<i32>(
                i32(offset & 1u), i32((offset >> 1u) & 1u),
                i32((offset >> 2u) & 1u));
            let logical = corner + step;
            let trilinear = abs(
                vec3<f32>(1.0) - abs(vec3<f32>(step) - fraction));
            var weight = trilinear.x * trilinear.y * trilinear.z;
            if (weight <= 0.0) {
                continue;
            }
            // Backface weighting. Half of a ground receiver's eight probes sit
            // *below* the surface, and a buried probe sees only hits, so it
            // stores zero. Averaging those in turned the near field black while
            // distance - where the query falls through to the screen probes -
            // stayed lit. Softly down-weighting probes behind the receiver's
            // plane is the standard remedy and costs one dot product.
            let probeWorld = worldGIDDGIProbePosition(volume, logical);
            let toProbe = probeWorld - biased;
            let toProbeLength = length(toProbe);
            if (toProbeLength > 1.0e-4) {
                let facing = dot(toProbe / toProbeLength, normal);
                weight *= (facing * 0.5 + 0.5) * (facing * 0.5 + 0.5);
            }
            if (weight <= 1.0e-6) {
                continue;
            }
            let physical = worldGIDDGIPhysicalCoordinate(
                logical, volume.scrollOffsetGeneration.xyz, dimensions);
            let slot = volume.probeBaseFlagsCursor.x +
                worldGIDDGIFlatten(physical, dimensions);
            let coefficientBase = slot * WORLDGI_DDGI_SH_COEFFICIENTS;
            if (coefficientBase + WORLDGI_DDGI_SH_COEFFICIENTS >
                arrayLength(&ddgiProbeSH)) {
                continue;
            }
            // First order only. The query is bandwidth bound - eight probes
            // times nine vec4 is 1,152 bytes per pixel - and the second-order
            // bands carry little of a sky-dominated cache's energy. Loading
            // four instead of nine is the single largest lever on this path.
            var coefficients : array<vec3<f32>, 4>;
            for (var index = 0u; index < 4u; index = index + 1u) {
                coefficients[index] = ddgiProbeSH[coefficientBase + index].xyz;
            }
            // A probe with no ambient energy at all is either buried or has
            // never been refreshed. Either way it carries no information, and
            // averaging its zero in is indistinguishable from real occlusion.
            let ambient = dot(coefficients[0], vec3<f32>(0.2126, 0.7152, 0.0722));
            if (ambient <= 1.0e-5) {
                continue;
            }
            accumulated += worldGIDDGIEvaluateL1(coefficients, normal) * weight;
            accumulatedWeight += weight;
        }
        if (accumulatedWeight > 1.0e-4) {
            *result = accumulated / accumulatedWeight;
            return true;
        }
    }
    return false;
}

fn probe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let length2 = dot(value, value);
    return select(
        fallback, value * inverseSqrt(max(length2, 1.0e-12)),
        length2 > 1.0e-12);
}

fn probe_identity(pixel : vec2<i32>, normal : vec3<f32>) -> vec2<u32> {
    let magnitude = abs(normal);
    var axis = 0u;
    if (magnitude.y > magnitude.x) { axis = 1u; }
    if (magnitude.z > max(magnitude.x, magnitude.y)) { axis = 2u; }
    let faceClass = axis | select(0u, 4u, normal[axis] < 0.0);
    let instance = textureLoad(instanceIdTexture, pixel, 0).x;
    return vec2<u32>(
        (instance & ((1u << 29u) - 1u)) | (faceClass << 29u),
        textureLoad(voxelIdTexture, pixel, 0).x);
}

// Can this probe actually see the point it is being asked to light?
//
// The probe already answered that question when it traced: the cell pointing
// from the probe toward this pixel holds the distance its ray travelled before
// hitting something. If that distance is shorter than the distance to the
// pixel, there is geometry between the two and the probe's irradiance belongs
// to the near side of it.
//
// Plane weighting alone cannot catch this. Two points can sit on the same plane
// at almost the same depth with a wall between them - a doorway, a pillar, the
// two sides of a thin partition - and the plane test accepts both. This is the
// test that stops light crossing those, and it costs one buffer read because
// the hit distances were already stored for ray sharing.
fn probe_visibility(
    probeIndex : u32, probeNormal : vec3<f32>, probePosition : vec3<f32>,
    position : vec3<f32>) -> f32 {
    let offset = position - probePosition;
    let separation = length(offset);
    if (separation <= 1.0e-5) { return 1.0; }
    let cell = sparse_voxel_probe_cell_from_direction(
        probeNormal, offset / separation);
    if (cell >= SPARSE_VOXEL_PROBE_DIRECTION_COUNT) { return 1.0; }
    let index = probeIndex * SPARSE_VOXEL_PROBE_DIRECTION_COUNT + cell;
    if (index >= arrayLength(&probeRadiance)) { return 1.0; }
    let stored = probeRadiance[index];
    // No sample, or the ray reached open space: nothing was in the way.
    if (stored.w < 0.0 || stored.w >= SPARSE_VOXEL_PROBE_SKY_DISTANCE) {
        return 1.0;
    }
    // One cell spans a finite solid angle, so the recorded hit is only an
    // estimate of what lies along this exact direction. Fade rather than cut,
    // and leave a margin of one cell's worth of angular spread before fading at
    // all, or every probe rejects its own neighbourhood on curved geometry.
    let margin = max(separation * 0.25, 1.0e-4);
    let tracedVisibility =
        clamp((stored.w - separation + margin) / margin, 0.0, 1.0);
    // The radiance map fades a Tree64 occlusion into analytic sky over the
    // distant-lighting interval. Its auxiliary hit distance must obey the
    // same contract: otherwise a mostly analytic probe is still rejected as
    // fully occluded when it is shared with another receiver, recreating the
    // dark block through the interpolation weights instead of the SH value.
    let cameraDistance = distance(
        probePosition, raster.cameraWorldPositionNear.xyz);
    let traceWeight = 1.0 - smoothstep(
        settings.distances.z, settings.distances.w, cameraDistance);
    return mix(1.0, tracedVisibility, traceWeight);
}

fn probe_load_sh(
    probeIndex : u32,
    coefficient0 : vec4<f32>) -> array<vec4<f32>, 9> {
    var coefficients : array<vec4<f32>, 9>;
    // Radiance is non-negative, so every RGB SH coefficient has a strict
    // bound relative to L0: |c_lm| <= max(|Y_lm|) / Y_00 * c_00. The largest
    // ratio in this second-order basis is below four. Enforcing that physical
    // bound at the consumer prevents a corrupt/stale directional coefficient
    // from reconstructing as a saturated purple or green screen tile, while
    // leaving every coefficient produced by a valid non-negative environment
    // unchanged. It also contains non-finite history locally instead of
    // allowing one probe to poison a spatial gather and the SVGF history.
    let finite0 = all(coefficient0.xyz == coefficient0.xyz) &&
        all(abs(coefficient0.xyz) < vec3<f32>(1.0e20));
    let safe0 = select(vec3<f32>(0.0), max(coefficient0.xyz, vec3<f32>(0.0)),
        finite0);
    coefficients[0] = vec4<f32>(safe0, coefficient0.w);
    let directionalLimit = safe0 * 4.0 + vec3<f32>(1.0e-6);
    for (var i = 1u; i < SPARSE_VOXEL_PROBE_SH_COUNT; i += 1u) {
        let index = probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT + i;
        let loaded = select(
            vec4<f32>(0.0), probeSH[index],
            index < arrayLength(&probeSH));
        let finite = all(loaded.xyz == loaded.xyz) &&
            all(abs(loaded.xyz) < vec3<f32>(1.0e20));
        coefficients[i] = vec4<f32>(select(
            vec3<f32>(0.0),
            clamp(loaded.xyz, -directionalLimit, directionalLimit),
            finite), loaded.w);
    }
    return coefficients;
}

// Placement validity and lighting validity are separate contracts. A probe
// becomes a valid surface record before any of its directional cells has
// necessarily produced a usable radiance sample. Treating that newly placed
// record as an ordinary interpolation candidate gives it non-zero geometric
// weight but zero SH, so one whole part of its screen tile becomes a black
// spot until the first ray update lands. Search past it instead: a slightly
// farther probe with lighting history is a much better estimate than an exact
// surface match carrying no light at all.
fn probe_has_lighting(coefficient : vec4<f32>) -> bool {
    if (coefficient.w <= 0.0 ||
        coefficient.w != coefficient.w || abs(coefficient.w) >= 1.0e20 ||
        !all(coefficient.xyz == coefficient.xyz) ||
        any(abs(coefficient.xyz) >= vec3<f32>(1.0e20))) { return false; }
    // The outdoor coherence path must not promote a fully black conservative
    // Tree64 sample into the continuity reference. Such a record means every
    // hemispherical ray immediately struck the coarse receiver terrain, not
    // that the open landscape truly has no ambient illumination.
    if (settings.probeCounts.w != 0u &&
        dot(max(coefficient.xyz, vec3<f32>(0.0)),
            vec3<f32>(0.2126, 0.7152, 0.0722)) <= 1.0e-6) {
        return false;
    }
    return true;
}

@compute @workgroup_size(8, 8, 1)
fn interpolateSparseVoxelProbes(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let dimensions = max(settings.dimensionsTile.xy, vec2<u32>(1u));
    let pixel = globalId.xy;
    if (any(pixel >= dimensions)) { return; }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * dimensions.x + pixel.x;
    let rawGeometryNormal =
        textureLoad(geometryNormalTexture, pixelI, 0).xyz;
    let identity = probe_identity(pixelI, rawGeometryNormal);

    if (pixelIndex >= arrayLength(&coverage) || coverage[pixelIndex] == 0u) {
        textureStore(outputFieldTexture, pixelI, vec4<f32>(0.0));
        textureStore(outputIdTexture, pixelI, vec4<u32>(identity, 0u, 0u));
        textureStore(reactiveOutput, pixelI, vec4<f32>(0.0));
        textureStore(outputMomentsTexture, pixelI, vec4<f32>(0.0));
        textureStore(outputLightHashTexture, pixelI, vec4<u32>(0u));
        return;
    }

    let tileSize = max(settings.dimensionsTile.z, 1u);
    let probeCounts = settings.probeCounts.xy;

    let depth = linearDepth[pixelIndex];
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - raster.focalPrincipal.z) / raster.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - raster.focalPrincipal.w) / raster.focalPrincipal.y,
        1.0);
    let worldDirection = normalize(
        (raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    let position = raster.cameraWorldPositionNear.xyz + worldDirection * depth;
    let geometryNormal = probe_normalize(
        rawGeometryNormal,
        vec3<f32>(0.0, 1.0, 0.0));
    let shadingNormal = probe_normalize(
        textureLoad(shadingNormalTexture, pixelI, 0).xyz, geometryNormal);
    // Terrain's material G-buffer reconstructs a continuous height-field
    // normal once voxel risers become sub-pixel. Probe selection must use the
    // same normal it later shades with: selecting by the exact vertical face
    // makes every finer streamed page switch to a darker family of riser
    // probes even though the macro surface did not change. Near the camera the
    // filtered normal is the exact face normal, so this remains lossless for
    // close voxels. Ordinary models always retain geometry-normal selection.
    let receiverNormal = select(
        geometryNormal, shadingNormal, settings.probeCounts.w != 0u);

    // How much this pixel's normal can be trusted to be *this* pixel's normal.
    //
    // Voxel faces are axis aligned, so two adjacent faces are perpendicular.
    // Where the geometry is finer than a pixel - distant detail, thin structure
    // - which face a pixel resolves on changes from frame to frame under the
    // camera jitter, and the resolved normal swings by 90 degrees. The probe
    // irradiance is then evaluated along a direction that flips, so the pixel
    // switches brightness while nothing in the scene has moved. That is the
    // reported "tiny spots that keep changing".
    //
    // Comparing against the right and lower neighbours measures exactly that:
    // on a flat face they agree and this is 1, and in sub-pixel detail they
    // disagree and it falls toward zero. Two texture loads, no filter kernel,
    // and it costs nothing on the flat majority of the scene because the result
    // there is 1 and the evaluation below is unchanged.
    let neighbourRight = probe_normalize(
        textureLoad(
            geometryNormalTexture,
            min(pixelI + vec2<i32>(1, 0), vec2<i32>(dimensions) - vec2<i32>(1)),
            0).xyz,
        geometryNormal);
    let neighbourDown = probe_normalize(
        textureLoad(
            geometryNormalTexture,
            min(pixelI + vec2<i32>(0, 1), vec2<i32>(dimensions) - vec2<i32>(1)),
            0).xyz,
        geometryNormal);
    let coherence = min(
        dot(geometryNormal, neighbourRight), dot(geometryNormal, neighbourDown));
    // Perpendicular neighbours give 0 and coplanar ones give 1. The knee sits
    // above zero so a single perpendicular neighbour already starts damping,
    // rather than waiting for the normal to be actively inverted.
    // Generic scenes keep this optional. Terrain enables it because distant
    // top/riser alternation is precisely the high-frequency normal field it is
    // meant to stabilize; strength and response threshold remain tunable.
    let coherenceSharpness = smoothstep(
        0.0, max(settings.geometry.y, 0.01), coherence);
    let dampingStrength = select(
        0.0, clamp(settings.geometry.x, 0.0, 1.0),
        settings.probeCounts.w != 0u);
    let normalSharpness = mix(1.0, coherenceSharpness, dampingStrength);

    // Probes sit at their tile's spawn pixel, but the interpolation grid is
    // anchored at tile centres: that is what makes the four-probe footprint
    // symmetric around the pixel instead of biased toward one corner.
    let gridPosition =
        (vec2<f32>(pixel) + vec2<f32>(0.5)) / f32(tileSize) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(gridPosition));
    let fraction = gridPosition - vec2<f32>(base);

    // Cache-primary mode. The screen-probe gather below is a *search*: a 4x4
    // tile footprint with validity, normal and plane-distance weighting, plus
    // each tile's refined probes, and two more search paths when it fails. The
    // world cache answers the same question with eight fixed fetches and one
    // spherical-harmonic evaluation - no search, no per-probe validity, and no
    // dependence on what is currently on screen.
    //
    // This is a real quality change, not just a cost one: the cache's finest
    // spacing is metres where screen probes are tile-sized, so it cannot carry
    // contact-scale detail. It is offered as a mode rather than a replacement
    // for that reason.
    if (settings.worldCache.y != 0u) {
        var primaryIrradiance = vec3<f32>(0.0);
        if (worldGICacheIrradiance(
                position, receiverNormal, settings.worldCache.x,
                &primaryIrradiance)) {
            // No fallback scale here: that factor exists to make a
            // last-resort estimate conservative, and this is the primary
            // estimate.
            let primaryValue = max(primaryIrradiance, vec3<f32>(0.0));
            let primaryLuminance =
                dot(primaryValue, vec3<f32>(0.2126, 0.7152, 0.0722));
            if (settings.probeCounts.z != 0u) {
                atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_COVERED], 1u);
                if (primaryLuminance <= 1.0e-5) {
                    atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_BLACK], 1u);
                }
            }
            textureStore(
                outputFieldTexture, pixelI, vec4<f32>(primaryValue, 1.0));
            textureStore(outputIdTexture, pixelI, vec4<u32>(identity, 0u, 0u));
            textureStore(reactiveOutput, pixelI, vec4<f32>(0.0));
            textureStore(
                outputMomentsTexture, pixelI,
                vec4<f32>(primaryLuminance * primaryLuminance, 255.0, 0.0, 0.0));
            textureStore(outputLightHashTexture, pixelI, vec4<u32>(0u));
            return;
        }
    }

    // How obliquely this surface is seen. A tile's world span along the surface
    // grows as the reciprocal of this, and so must the radius a probe serves.
    let obliquity = clamp(abs(dot(receiverNormal, worldDirection)), 0.15, 1.0);

    let counting = settings.probeCounts.z != 0u;
    let diagnosticRegion = probe_diagnostic_region(pixel, dimensions);

    var irradiance = vec3<f32>(0.0);
    var ambientIrradiance = vec3<f32>(0.0);
    var totalWeight = 0.0;
    var fallbackIndex = 0xffffffffu;
    var fallbackCoefficient0 = vec4<f32>(0.0);
    var fallbackWeight = 0.0;
    // Search radius around the bilinear footprint. Zero is the footprint
    // itself; it only grows when the footprint produced nothing.
    //
    // A tile's probe sits on whichever voxel face its seed pixel landed on, and
    // voxel faces are axis aligned, so a probe one tile away is routinely
    // perpendicular to the surface a pixel is on. A probe traces only the
    // hemisphere above its *own* normal, so a perpendicular one holds no data
    // for this pixel's normal and the orientation test is right to refuse it -
    // but refusing all four leaves the pixel on the fallback, which is a single
    // probe that failed every test, scaled down. Measured on the interior
    // camera at 1280x720 that was 80% of the rightmost quarter of the screen.
    //
    // Searching further for a probe that *does* pass is the answer, and it is
    // safe in a way that relaxing the orientation test would not be: every
    // probe accepted here still passes orientation, plane, depth and
    // visibility, so nothing is lit by a probe that cannot see it. Same shape
    // as the fallback's own 3x3 widening and the accumulator's 3x3 identity
    // search: when a test can only answer "no", ask it over a wider set rather
    // than weakening it.
    var searchRadius = 0;
    loop {
    for (var y = -searchRadius; y <= 1 + searchRadius; y += 1) {
        for (var x = -searchRadius; x <= 1 + searchRadius; x += 1) {
            let coord = base + vec2<i32>(x, y);
            if (any(coord < vec2<i32>(0)) ||
                any(coord >= vec2<i32>(probeCounts))) { continue; }
            // Inside the footprint this is the bilinear weight. Outside it,
            // distance from the footprint centre, which keeps nearer probes
            // dominant and is continuous with the bilinear term at the border.
            let inFootprint = x >= 0 && x <= 1 && y >= 0 && y <= 1;
            let offsetFromCentre = vec2<f32>(f32(x) - 0.5, f32(y) - 0.5);
            let bilinear = select(
                0.5 / (1.0 + dot(offsetFromCentre, offsetFromCentre)),
                select(1.0 - fraction.x, fraction.x, x == 1) *
                    select(1.0 - fraction.y, fraction.y, y == 1),
                inFootprint);
            let tileIndex = sparse_voxel_probe_index(
                vec2<u32>(coord), probeCounts);

            // The tile's own probe, then any probe refinement added because
            // this tile could not be described by one. A refined probe is
            // weighted exactly like the uniform one - it is an ordinary probe
            // that happens to sit on the surface the uniform probe missed, and
            // for the pixels on that surface it is the only probe that will
            // pass the plane test at all.
            var slot = 0u;
            let registryBase = tileIndex * SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;
            var refinedCount = 0u;
            if (registryBase < arrayLength(&adaptiveTiles)) {
                refinedCount = min(
                    adaptiveTiles[registryBase],
                    SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
            }
            loop {
                if (slot > refinedCount) { break; }
                var probeIndex = tileIndex;
                if (slot > 0u) {
                    let entry = registryBase + slot;
                    if (entry >= arrayLength(&adaptiveTiles)) { break; }
                    probeIndex = adaptiveTiles[entry];
                }
                slot += 1u;
                if (probeIndex >= arrayLength(&probes)) {
                    if (counting) {
                        atomicAdd(
                            &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_PROBE_RANGE],
                            1u);
                    }
                    continue;
                }
                let probe = probes[probeIndex];
                if ((probe.identityState.z &
                    SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) {
                    if (counting) {
                        atomicAdd(
                            &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_PROBE_INVALID],
                            1u);
                    }
                    continue;
                }
                let coefficientIndex =
                    probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT;
                if (coefficientIndex >= arrayLength(&probeSH)) {
                    if (counting) {
                        atomicAdd(
                            &diagnostics[diagnosticRegion +
                                PROBE_DIAGNOSTIC_PROBE_INVALID],
                            1u);
                    }
                    continue;
                }
                let coefficient0 = probeSH[coefficientIndex];
                if (!probe_has_lighting(coefficient0)) {
                    if (counting) {
                        atomicAdd(
                            &diagnostics[diagnosticRegion +
                                PROBE_DIAGNOSTIC_PROBE_INVALID],
                            1u);
                    }
                    continue;
                }
                let orientation = dot(probe.normalRadius.xyz, receiverNormal);
                // Every valid probe in the footprint is a fallback candidate,
                // whatever its orientation, ranked so that better-oriented and
                // nearer ones still win.
                //
                // Requiring a positive orientation here is what turned small
                // voxel faces black. A face pointing +X surrounded by probes on
                // +Y and -Z faces has a zero or negative dot with every one of
                // them, so no candidate qualified, and the pixel emitted zero.
                // Whether the tile's jittered seed happened to land on that face
                // then decided between lit and black from frame to frame, which
                // is the flicker. A perpendicular probe is a poor estimate of
                // the ambient at that point; it is a far better one than black.
                //
                // Only the index is kept: refinement can offer this loop dozens
                // of candidates per pixel, and evaluating nine coefficients for
                // each before knowing whether its weights survive is the
                // difference between a cheap gather and an unaffordable one.
                let candidateWeight =
                    max(bilinear, 1.0e-3) * (orientation * 0.5 + 0.5);
                if (candidateWeight > fallbackWeight) {
                    fallbackWeight = candidateWeight;
                    fallbackIndex = probeIndex;
                    fallbackCoefficient0 = coefficient0;
                }
                // The weighted gather below still demands a compatible
                // orientation - the fallback is a floor, not a licence to blend
                // probes from surfaces facing the other way.
                if (orientation <= 0.0) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_ORIENTATION], 1u);
                }
                continue; }
                if (bilinear <= 1.0e-4) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_BILINEAR], 1u);
                }
                continue; }

                let normalWeight = smoothstep(
                    settings.weights.x, 1.0, orientation);
                if (normalWeight <= 0.0) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_NORMAL], 1u);
                }
                continue; }
                // Plane distance: how far this pixel sits off the probe's
                // surface plane, relative to the world region it represents.
                let radius = max(
                    probe.normalRadius.w * settings.weights.z / obliquity,
                    1.0e-4);
                let offset = position - probe.positionDepth.xyz;
                let planeDistance = abs(dot(offset, probe.normalRadius.xyz));
                let planeWeight = 1.0 - smoothstep(0.0, radius, planeDistance);
                if (planeWeight <= 0.0) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_PLANE], 1u);
                }
                continue; }
                let depthScale = max(
                    max(abs(depth), abs(probe.positionDepth.w)), 1.0);
                if (abs(depth - probe.positionDepth.w) / depthScale >
                    settings.weights.y) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_DEPTH], 1u);
                }
                continue; }
                let visibility = probe_visibility(
                    probeIndex, probe.normalRadius.xyz,
                    probe.positionDepth.xyz, position);
                if (visibility <= 0.0) { if (counting) {
                    atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_REJECT_VISIBILITY], 1u);
                }
                continue; }
                let weight = bilinear * normalWeight * planeWeight * visibility;
                let coefficients = probe_load_sh(probeIndex, coefficient0);
                irradiance += sparse_voxel_probe_sh_irradiance_damped(
                    coefficients, shadingNormal,
                    normalSharpness) * weight;
                ambientIrradiance += max(
                    coefficients[0].xyz *
                        (SPARSE_VOXEL_PROBE_SH_C0 * 3.14159265359),
                    vec3<f32>(0.0)) * weight;
                totalWeight += weight;
            }
        }
    }
    // Widen twice. Terrain LOD boundaries can span more than one probe tile in
    // depth even when the visible surface is continuous, and stopping after
    // one ring left 10-19% of static-scene pixels alternating onto the
    // single-probe fallback. Nothing accumulated when totalWeight is zero, so
    // repeating the inner footprint cannot double count. The extra work is
    // paid only by pixels whose smaller footprint found no compatible probe.
    if (totalWeight > 1.0e-5 || searchRadius >= 2) { break; }
    searchRadius += 1;
    }

    var value = vec3<f32>(0.0);
    // The reactive mask tells the upscaler to drop history for a pixel.
    //
    // This path used to raise it precisely when the gather failed, which is
    // backwards: a pixel whose estimate this frame is least trustworthy is the
    // one that most needs its history kept. Raising it there asked the upscaler
    // to pass every alternation straight through, so a face that flickered
    // between two probes flickered in the final image at full amplitude.
    //
    // Probe mode never raises it. The temporal accumulation that matters lives
    // in the probe radiance cells, and a probe pixel has no per-pixel event
    // that would justify discarding history - a disocclusion changes which
    // probe is gathered, not whether the gather is valid.
    // A pixel whose entire local tile neighbourhood is invalid found no
    // fallback at all and emitted black - which is the exact failure the
    // fallback exists to prevent, it simply could not reach far enough to
    // prevent it.
    //
    // That hole is not static. Probe validity churns frame to frame (measured
    // on a static camera: the refined set turns over ~36% per frame, and the
    // count of valid probes swings 17-26% with jitter), so which pixels land in
    // it changes every frame and a region alternates between lit and black.
    // The temporal upscaler normally averages that away, which is why it shows
    // up as solid black regions the moment TAA/TSR is switched off.
    //
    // The wider sweep runs only when the footprint yielded nothing, so the
    // common path pays for none of it, and it can only ever replace black with
    // a poor estimate - never darken a pixel that already had one. Same
    // reasoning as the accumulator's 3x3 identity search: when a test can only
    // answer "no", ask the weaker question it can answer.
    if (counting) {
        if (dot(rawGeometryNormal, rawGeometryNormal) <= 1.0e-12) {
            atomicAdd(
                &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_DEGENERATE_NORMAL],
                1u);
        }
        atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_COVERED], 1u);
        atomicAdd(
            &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_DEPTH],
            u32(clamp(depth, 0.0, 1.0e7)));
        atomicAdd(
            &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_COVERED], 1u);
    }

    // The world cache answers first. When it can, the 81-tile search below is
    // skipped entirely - this replaces the most expensive branch in the pass
    // rather than adding to it.
    var worldCacheIrradiance = vec3<f32>(0.0);
    var worldCacheValid = false;
    if (fallbackIndex == 0xffffffffu && totalWeight <= 1.0e-5) {
        worldCacheValid = worldGICacheIrradiance(
            position, receiverNormal,
            settings.worldCache.x, &worldCacheIrradiance);
    }
    if (fallbackIndex == 0xffffffffu && !worldCacheValid) {
        if (counting) {
            atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_WIDENED], 1u);
            atomicAdd(
                &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_WIDENED], 1u);
        }
        // Probe placement/refinement is intentionally stochastic, so even a
        // static camera can temporarily have no valid uniform probe in the
        // immediate 4x4 neighbourhood. The old last-resort search ignored the
        // refined probes and emitted literal black in that case. Which tiles
        // had the hole changed as the refined set turned over, producing the
        // characteristic stationary-camera dark spots that never converged.
        //
        // This is a failure-only path. Search a bounded 9x9 neighbourhood and
        // include each tile's refined probes. Strict interpolation above is
        // unchanged; this only chooses a non-black estimate after every
        // surface-compatible candidate has already failed.
        for (var y = -4; y <= 4; y += 1) {
            for (var x = -4; x <= 4; x += 1) {
                let coord = base + vec2<i32>(x, y);
                if (any(coord < vec2<i32>(0)) ||
                    any(coord >= vec2<i32>(probeCounts))) { continue; }
                let tileIndex = sparse_voxel_probe_index(
                    vec2<u32>(coord), probeCounts);
                let registryBase =
                    tileIndex * SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;
                var refinedCount = 0u;
                if (registryBase < arrayLength(&adaptiveTiles)) {
                    refinedCount = min(
                        adaptiveTiles[registryBase],
                        SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
                }
                var slot = 0u;
                loop {
                    if (slot > refinedCount) { break; }
                    var probeIndex = tileIndex;
                    if (slot > 0u) {
                        let entry = registryBase + slot;
                        if (entry >= arrayLength(&adaptiveTiles)) { break; }
                        probeIndex = adaptiveTiles[entry];
                    }
                    slot += 1u;
                    if (probeIndex >= arrayLength(&probes)) { continue; }
                    let probe = probes[probeIndex];
                    if ((probe.identityState.z &
                        SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) { continue; }
                    let coefficientIndex =
                        probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT;
                    if (coefficientIndex >= arrayLength(&probeSH)) {
                        continue;
                    }
                    let coefficient0 = probeSH[coefficientIndex];
                    if (!probe_has_lighting(coefficient0)) {
                        continue;
                    }
                    let orientation =
                        dot(probe.normalRadius.xyz, receiverNormal);
                    // An exactly opposite-facing probe is a weak estimate, but
                    // it is still preferable to a zero-energy hard shadow. A
                    // small floor also guarantees the first valid probe can win
                    // when every candidate happens to face the other way.
                    let orientationWeight = max(
                        orientation * 0.5 + 0.5, 0.025);
                    let candidateWeight = 1.0e-3 * orientationWeight /
                        (1.0 + f32(abs(x) + abs(y)));
                    if (candidateWeight > fallbackWeight) {
                        fallbackWeight = candidateWeight;
                        fallbackIndex = probeIndex;
                        fallbackCoefficient0 = coefficient0;
                    }
                }
            }
        }
    }

    // A single fallback probe avoids literal holes but can still expose its
    // screen tile as a flat patch. Only on the failed-gather path, blend the DC
    // term from the nearby valid probes. DC is orientation independent, and a
    // compact spatial blend gives adjacent fallback pixels overlapping support
    // instead of assigning each one to a different tile winner. Refined probes
    // participate because they are often the only records describing the tiny
    // riser faces that caused the fallback.
    var fallbackAmbient = vec3<f32>(0.0);
    var fallbackAmbientWeight = 0.0;
    if (totalWeight <= 1.0e-5 && fallbackIndex != 0xffffffffu) {
        let fallbackRadius = i32(clamp(settings.geometry.w, 0.0, 4.0));
        for (var y = -fallbackRadius; y <= 1 + fallbackRadius; y += 1) {
            for (var x = -fallbackRadius; x <= 1 + fallbackRadius; x += 1) {
                let coord = base + vec2<i32>(x, y);
                if (any(coord < vec2<i32>(0)) ||
                    any(coord >= vec2<i32>(probeCounts))) { continue; }
                let tileIndex = sparse_voxel_probe_index(
                    vec2<u32>(coord), probeCounts);
                let registryBase =
                    tileIndex * SPARSE_VOXEL_PROBE_ADAPTIVE_TILE_STRIDE;
                var refinedCount = 0u;
                if (registryBase < arrayLength(&adaptiveTiles)) {
                    refinedCount = min(
                        adaptiveTiles[registryBase],
                        SPARSE_VOXEL_PROBE_ADAPTIVE_PER_TILE);
                }
                var slot = 0u;
                loop {
                    if (slot > refinedCount) { break; }
                    var probeIndex = tileIndex;
                    if (slot > 0u) {
                        let entry = registryBase + slot;
                        if (entry >= arrayLength(&adaptiveTiles)) { break; }
                        probeIndex = adaptiveTiles[entry];
                    }
                    slot += 1u;
                    if (probeIndex >= arrayLength(&probes)) { continue; }
                    let probe = probes[probeIndex];
                    if ((probe.identityState.z &
                        SPARSE_VOXEL_PROBE_STATE_VALID) == 0u) { continue; }
                    let coefficientIndex =
                        probeIndex * SPARSE_VOXEL_PROBE_SH_COUNT;
                    if (coefficientIndex >= arrayLength(&probeSH)) { continue; }
                    let coefficient = probeSH[coefficientIndex];
                    if (coefficient.w <= 0.0) { continue; }
                    let orientation = clamp(
                        dot(probe.normalRadius.xyz, receiverNormal), -1.0, 1.0);
                    let orientationWeight =
                        0.25 + 0.75 * max(orientation, 0.0);
                    let spatialWeight = 1.0 /
                        (1.0 + f32(abs(x) + abs(y)));
                    let weight = orientationWeight * spatialWeight;
                    fallbackAmbient += max(
                        coefficient.xyz *
                            (SPARSE_VOXEL_PROBE_SH_C0 * 3.14159265359),
                        vec3<f32>(0.0)) * weight;
                    fallbackAmbientWeight += weight;
                }
            }
        }
    }

    let reactive = 0.0;
    if (totalWeight > 1.0e-5) {
        value = irradiance / totalWeight;
        // On incoherent terrain pixels a second-order directional lobe can
        // reconstruct slightly below zero and is clamped to black, even while
        // the contributing probes carry healthy positive ambient energy. The
        // same coherence setting that damps those unstable bands supplies a
        // conservative fraction of their DC term as a continuity floor. This
        // is disabled for ordinary scenes, so a deliberately black interior
        // is not lifted by the outdoor-terrain stabilization path.
        if (settings.probeCounts.w != 0u) {
            value = max(
                value,
                ambientIrradiance / totalWeight *
                    clamp(settings.geometry.x, 0.0, 1.0));
        }
    } else if (worldCacheValid) {
        // No screen probe passed the strict test, and the world cache has a
        // value for this point. It is world-anchored and temporally blended,
        // so unlike the screen-space fallback it does not change as probe
        // validity churns - which is what made these pixels alternate between
        // lit and black frame to frame.
        value = worldCacheIrradiance * max(settings.weights.w, 0.0);
        if (counting) {
            atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_FALLBACK], 1u);
            atomicAdd(
                &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_FALLBACK], 1u);
        }
    } else if (fallbackIndex != 0xffffffffu) {
        // No probe passed the strict test. Rather than emit black - which reads
        // as a hard shadow and is the worst possible failure for a soft-shadow
        // renderer - take the best candidate the neighbourhood offers.
        //
        // The candidate is deliberately allowed to belong to a perpendicular
        // voxel face: rejecting it would put us back on literal black. Its
        // directional SH bands are not meaningful for this pixel, however. A
        // probe above a +Y terrace evaluated on the neighbouring +X riser can
        // reconstruct zero even though its hemisphere contains plenty of sky;
        // one such result covers a whole screen tile and produces the reported
        // black blocks. The band-zero term is the probe's mean diffuse
        // irradiance. It is independent of the receiving normal and therefore
        // the conservative continuity estimate when surface compatibility has
        // already failed.
        let fallbackSH = probe_load_sh(
            fallbackIndex, fallbackCoefficient0);
        let directionalFallback = sparse_voxel_probe_sh_irradiance_damped(
            fallbackSH, shadingNormal, normalSharpness);
        let ambientFallback = max(
            fallbackSH[0].xyz *
                (SPARSE_VOXEL_PROBE_SH_C0 * 3.14159265359),
            vec3<f32>(0.0));
        let blendedAmbient = select(
            ambientFallback,
            fallbackAmbient / max(fallbackAmbientWeight, 1.0e-6),
            fallbackAmbientWeight > 0.0);
        let continuityAmbient = mix(
            vec3<f32>(0.0), blendedAmbient,
            clamp(settings.geometry.z, 0.0, 1.0));
        value = max(directionalFallback, continuityAmbient) *
            max(settings.weights.w, 0.0);
        if (counting) {
            atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_FALLBACK], 1u);
            atomicAdd(
                &diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_FALLBACK], 1u);
        }
    }

    // A valid second-order projection can still ring below zero in only one
    // colour channel. The ordinary non-negative clamp then changes hue (most
    // visibly to purple when green alone crosses zero) and exposes the probe's
    // tile as the camera or adaptive set changes. On the outdoor terrain path,
    // bound extreme chromaticity against the orientation-independent DC light
    // while retaining a wide four-to-one range for real blue sky and warm sun.
    // Luminance and directional contrast are unchanged unless a channel lies
    // outside that range.
    var stableValue = value;
    if (settings.probeCounts.w != 0u && totalWeight > 1.0e-5) {
        let dc = max(ambientIrradiance / totalWeight, vec3<f32>(0.0));
        let valueLuminance = dot(max(stableValue, vec3<f32>(0.0)),
            vec3<f32>(0.2126, 0.7152, 0.0722));
        let dcLuminance = dot(dc, vec3<f32>(0.2126, 0.7152, 0.0722));
        if (valueLuminance > 1.0e-6 && dcLuminance > 1.0e-6) {
            let matchedDC = dc * (valueLuminance / dcLuminance);
            stableValue = clamp(stableValue,
                matchedDC * 0.25, matchedDC * 4.0);
        }
    }
    let finiteValue = all(stableValue == stableValue) &&
        all(abs(stableValue) < vec3<f32>(1.0e20));
    stableValue = select(vec3<f32>(0.0), stableValue, finiteValue);

    let luminance = dot(stableValue, vec3<f32>(0.2126, 0.7152, 0.0722));
    if (counting && luminance <= 1.0e-5) {
        // Count the result, not merely whether an index was found. A valid but
        // incompatible probe can reconstruct zero and is visually identical to
        // having no probe at all; the previous diagnostic missed exactly those
        // screen-tile blackouts and reported a misleading zero.
        atomicAdd(&diagnostics[PROBE_DIAGNOSTIC_BLACK], 1u);
        atomicAdd(&diagnostics[diagnosticRegion + PROBE_DIAGNOSTIC_BLACK], 1u);
    }
    // The count channel is what downstream treats as confidence. A probe pixel
    // is backed by a full hemisphere every frame, so it is reported as fully
    // measured rather than accumulating like the per-pixel path did.
    textureStore(
        outputFieldTexture, pixelI,
        vec4<f32>(max(stableValue, vec3<f32>(0.0)), 1.0));
    textureStore(outputIdTexture, pixelI, vec4<u32>(identity, 0u, 0u));
    textureStore(reactiveOutput, pixelI, vec4<f32>(reactive));
    textureStore(
        outputMomentsTexture, pixelI,
        vec4<f32>(luminance * luminance, 255.0, 0.0, 0.0));
    textureStore(outputLightHashTexture, pixelI, vec4<u32>(0u));
}

@compute @workgroup_size(64, 1, 1)
fn resetSparseVoxelProbeDiagnostics(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= PROBE_DIAGNOSTIC_WORDS) { return; }
    atomicStore(&diagnostics[globalId.x], 0u);
}
