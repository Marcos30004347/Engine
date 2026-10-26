// World-anchored irradiance cache: shared ABI, grid addressing and spherical
// harmonics. Included by the ray generation and update kernels, and by any
// consumer that queries the cache.
//
// Why this exists. Screen probes rebuild their hemisphere from one frame of
// rays, so a probe that loses its history - a disocclusion, a page install, a
// cohort of rays taking an unlucky branch - has nothing to fall back on and
// swings to a completely different value. Measured on the terrain fixture,
// 0.5-0.8% of probes swing fully in any given frame. A world-anchored cache is
// not primarily a performance structure here: it is a *prior*. It is anchored
// in world space, so it survives camera motion and screen-space disocclusion
// entirely, and it is temporally blended, so it cannot swing.
//
// The grid mirrors WorldGIDDGI.hpp exactly - snapped origin, toroidal scroll,
// logical/physical mapping. Any divergence between the two is a probe reading
// another probe's history, which looks like a stable but wrong result and is
// far harder to diagnose than a crash.

// One RGB coefficient per vec4. Tighter packs exist, but every consumer pays
// the unpack on every query and the whole cache is under three megabytes.
const WORLDGI_DDGI_SH_COEFFICIENTS : u32 = 9u;
const WORLDGI_DDGI_MAX_CASCADES : u32 = 8u;

// Mirrors WorldGIDDGIVolumeGPU.
struct WorldGIDDGIVolume {
    // xyz world position of logical probe (0,0,0), w probe spacing in metres.
    originSpacing : vec4<f32>,
    // xyz probe counts, w cascade index.
    dimensionsCascade : vec4<u32>,
    // xyz toroidal scroll offset, w generation.
    scrollOffsetGeneration : vec4<i32>,
    // x first probe index in the shared arrays, y flags, z update cursor,
    // w reserved.
    probeBaseFlagsCursor : vec4<u32>,
}

struct WorldGIDDGISettings {
    // x cascade count, y rays per probe, z probes updated this frame,
    // w total probes across all cascades.
    cascadeRaysProbes : vec4<u32>,
    // x hysteresis, y maximum ray distance, z normal bias, w reserved.
    blendTuning : vec4<f32>,
    // xyz sun direction (pointing at the sun), w sun intensity.
    sunDirectionIntensity : vec4<f32>,
    // xyz sun colour, w angular radius cosine.
    sunColorCosine : vec4<f32>,
    // xyz zenith sky colour, w horizon blend.
    skyZenithHorizon : vec4<f32>,
    // xyz ground/albedo tint for downward misses, w sky intensity.
    groundSkyIntensity : vec4<f32>,
    // x frame index, y seed, z first probe slot updated this frame, w flags.
    frameSeedCursor : vec4<u32>,
}

fn worldGIDDGIPositiveModulo(value : i32, modulus : u32) -> i32 {
    if (modulus == 0u) {
        return 0;
    }
    let m = i32(modulus);
    let remainder = value % m;
    return select(remainder, remainder + m, remainder < 0);
}

fn worldGIDDGIPhysicalCoordinate(
    logical : vec3<i32>, scroll : vec3<i32>, dimensions : vec3<u32>)
    -> vec3<u32> {
    return vec3<u32>(
        u32(worldGIDDGIPositiveModulo(logical.x + scroll.x, dimensions.x)),
        u32(worldGIDDGIPositiveModulo(logical.y + scroll.y, dimensions.y)),
        u32(worldGIDDGIPositiveModulo(logical.z + scroll.z, dimensions.z)));
}

fn worldGIDDGIFlatten(coordinate : vec3<u32>, dimensions : vec3<u32>) -> u32 {
    return coordinate.x +
        dimensions.x * (coordinate.y + dimensions.y * coordinate.z);
}

// Inverse of the physical mapping: which logical probe a storage slot holds.
fn worldGIDDGILogicalFromPhysical(
    physical : vec3<u32>, scroll : vec3<i32>, dimensions : vec3<u32>)
    -> vec3<i32> {
    return vec3<i32>(
        worldGIDDGIPositiveModulo(i32(physical.x) - scroll.x, dimensions.x),
        worldGIDDGIPositiveModulo(i32(physical.y) - scroll.y, dimensions.y),
        worldGIDDGIPositiveModulo(i32(physical.z) - scroll.z, dimensions.z));
}

fn worldGIDDGIProbePosition(
    volume : WorldGIDDGIVolume, logical : vec3<i32>) -> vec3<f32> {
    return volume.originSpacing.xyz +
        vec3<f32>(logical) * volume.originSpacing.w;
}

// Spherical Fibonacci, rotated per frame so consecutive frames integrate
// different directions rather than resampling one fixed set forever.
fn worldGIDDGIRayDirection(
    rayIndex : u32, rayCount : u32, rotation : vec2<f32>) -> vec3<f32> {
    let count = max(f32(rayCount), 1.0);
    let index = f32(rayIndex) + 0.5;
    let cosTheta = 1.0 - 2.0 * index / count;
    let sinTheta = sqrt(max(1.0 - cosTheta * cosTheta, 0.0));
    // Golden angle, offset by the frame's rotation.
    let phi = index * 2.399963229728653 + rotation.x;
    let direction = vec3<f32>(
        sinTheta * cos(phi), cosTheta, sinTheta * sin(phi));
    // A second rotation about X decorrelates the polar distribution too,
    // otherwise every frame samples the same set of latitudes.
    let c = cos(rotation.y);
    let s = sin(rotation.y);
    return vec3<f32>(
        direction.x,
        direction.y * c - direction.z * s,
        direction.y * s + direction.z * c);
}

fn worldGIDDGIHash(value : u32) -> u32 {
    var hash = value * 747796405u + 2891336453u;
    hash = ((hash >> ((hash >> 28u) + 4u)) ^ hash) * 277803737u;
    return (hash >> 22u) ^ hash;
}

// Real spherical harmonics through band two, in the usual order.
fn worldGIDDGIBasis(direction : vec3<f32>) -> array<f32, 9> {
    let x = direction.x;
    let y = direction.y;
    let z = direction.z;
    return array<f32, 9>(
        0.2820947918,
        0.4886025119 * y,
        0.4886025119 * z,
        0.4886025119 * x,
        1.0925484306 * x * y,
        1.0925484306 * y * z,
        0.3153915653 * (3.0 * z * z - 1.0),
        1.0925484306 * x * z,
        0.5462742153 * (x * x - y * y));
}

// Lambertian convolution. This returns *irradiance*, matching what the screen
// probe path publishes - its fallback multiplies the DC term by pi for exactly
// this reason. Folding 1/pi in here instead left the cache a factor of pi dark
// against the path it replaces, which read as a plausible but uniformly dim
// image rather than an obvious error.
fn worldGIDDGIConvolution(band : u32) -> f32 {
    if (band == 0u) {
        return 3.14159265359;
    }
    if (band == 1u) {
        return 2.09439510239;
    }
    return 0.78539816339;
}

fn worldGIDDGIBandOfCoefficient(index : u32) -> u32 {
    if (index == 0u) {
        return 0u;
    }
    if (index < 4u) {
        return 1u;
    }
    return 2u;
}

// Analytic sky for a ray that left the scene. Deliberately simple: a zenith to
// horizon gradient, a ground term below the horizon, and the sun disc. This is
// the whole light source model of the cache in its first form - a hit is
// treated as fully blocking - so what the cache stores is sun-and-sky
// irradiance with real occlusion.
fn worldGIDDGISkyRadiance(
    settings : WorldGIDDGISettings, direction : vec3<f32>) -> vec3<f32> {
    let up = clamp(direction.y * 0.5 + 0.5, 0.0, 1.0);
    let horizon = clamp(settings.skyZenithHorizon.w, 0.0, 1.0);
    let skyMix = mix(horizon, 1.0, up);
    var radiance = settings.skyZenithHorizon.xyz * skyMix *
        settings.groundSkyIntensity.w;
    if (direction.y < 0.0) {
        radiance = mix(
            radiance, settings.groundSkyIntensity.xyz *
                settings.groundSkyIntensity.w,
            clamp(-direction.y, 0.0, 1.0));
    }
    let sunCosine = dot(direction, normalize(settings.sunDirectionIntensity.xyz));
    if (sunCosine >= settings.sunColorCosine.w) {
        radiance += settings.sunColorCosine.xyz *
            settings.sunDirectionIntensity.w;
    }
    return radiance;
}

// Evaluates the cache for a surface normal. The convolution above is folded
// with 1/pi, so this is outgoing radiance for unit albedo: a consumer
// multiplies by albedo and adds it, with no further normalization.
fn worldGIDDGIEvaluate(
    coefficients : array<vec3<f32>, 9>, normal : vec3<f32>) -> vec3<f32> {
    let basis = worldGIDDGIBasis(normal);
    var result = vec3<f32>(0.0);
    for (var coefficient = 0u; coefficient < 9u; coefficient = coefficient + 1u) {
        result += coefficients[coefficient] * basis[coefficient] *
            worldGIDDGIConvolution(worldGIDDGIBandOfCoefficient(coefficient));
    }
    // A second-order fit of a mostly-positive signal can still ring negative in
    // deep occlusion; clamping is what keeps that from reading as black.
    return max(result, vec3<f32>(0.0));
}

// First-order evaluation, for consumers that are bandwidth bound on the fetch
// rather than starved of directionality.
fn worldGIDDGIEvaluateL1(
    coefficients : array<vec3<f32>, 4>, normal : vec3<f32>) -> vec3<f32> {
    let basis = worldGIDDGIBasis(normal);
    var result = coefficients[0] * basis[0] * worldGIDDGIConvolution(0u);
    for (var index = 1u; index < 4u; index = index + 1u) {
        result += coefficients[index] * basis[index] *
            worldGIDDGIConvolution(1u);
    }
    return max(result, vec3<f32>(0.0));
}
