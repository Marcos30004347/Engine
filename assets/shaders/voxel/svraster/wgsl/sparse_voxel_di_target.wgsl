// Shared ReSTIR DI target function.
//
// Include AFTER worldgi-types.wgsl, sparse_voxel_sky.wgsl and
// sparse_voxel_scene_ray_types.wgsl: these helpers use WorldGILight, the
// WORLD_GI_LIGHT_TYPE_* constants, sparse_voxel_sky_radiance and
// SparseVoxelSceneRay, and the WGSL preprocessor has no include guards.
//
// The reservoir resamples in AREA measure on the emitter surface. A stored
// sample is a point on the light, which is a receiver-independent quantity, so
// temporal and spatial reconnection may re-point the ray at a new receiver
// without applying a shift Jacobian. The target must therefore be
// proportional to the area-measure integrand
//
//     f = L_e * cos_receiver * cos_emitter / d^2 * V
//
// with V omitted, since visibility is only known after the ray is traced.
// Infinite lights (directional, sky) have no emitter surface and are resampled
// in solid-angle measure instead; a reservoir may legitimately hold candidates
// of both families, because RIS is unbiased over the disjoint union of the two
// sample spaces as long as each component uses its own measure consistently.
//
// The candidate pass, the temporal pass and the spatial pass all evaluate this
// one function. They exchange RIS weights directly, so any divergence between
// their targets makes the estimator inconsistent - which is why this lives in
// a shared file rather than in three hand-synchronised copies.

const SPARSE_VOXEL_DI_RECEIVER_COSINE_FLOOR : f32 = 0.05;
const SPARSE_VOXEL_DI_EMITTER_COSINE_FLOOR : f32 = 1.0e-3;
const SPARSE_VOXEL_DI_LUMINANCE : vec3<f32> =
    vec3<f32>(0.2126, 0.7152, 0.0722);

fn sparse_voxel_di_light_is_infinite(light : WorldGILight) -> bool {
    return light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL ||
        light.flags.y == WORLD_GI_LIGHT_TYPE_SKY;
}

fn sparse_voxel_di_light_power(light : WorldGILight) -> f32 {
    return dot(max(light.colorIntensity.rgb, vec3<f32>(0.0)),
        SPARSE_VOXEL_DI_LUMINANCE) * max(light.colorIntensity.w, 0.0);
}

// Emitter-side cosine of the sampled point. Returns exactly 1 for light types
// that have no emitter surface, and exactly 0 when the sampled point faces
// away from the receiver: both sphere and quad emitters are one-sided in the
// shading pass, so a back-facing sample shades to black.
fn sparse_voxel_di_emitter_cosine(
    light : WorldGILight, direction : vec3<f32>,
    sampledPoint : vec3<f32>) -> f32 {
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE) {
        if (max(light.shapeParams.x, 0.0) <= 1.0e-6) { return 1.0; }
        let emitterNormal = sparse_voxel_safe_normalize(
            sampledPoint - light.positionRange.xyz, -direction);
        return max(dot(emitterNormal, -direction), 0.0);
    }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let emitterNormal = sparse_voxel_safe_normalize(
            light.directionType.xyz, vec3<f32>(0.0, -1.0, 0.0));
        return max(dot(emitterNormal, -direction), 0.0);
    }
    return 1.0;
}

// The emitter cosine as it enters the target. The candidate pass divides by
// this same floored value when it converts a solid-angle proposal pdf into an
// area-measure pdf, so the two cancel exactly in target / pdf rather than
// dividing one denormal by another near the emitter silhouette.
fn sparse_voxel_di_emitter_cosine_floored(
    light : WorldGILight, direction : vec3<f32>,
    sampledPoint : vec3<f32>) -> f32 {
    return max(
        sparse_voxel_di_emitter_cosine(light, direction, sampledPoint),
        SPARSE_VOXEL_DI_EMITTER_COSINE_FLOOR);
}

fn sparse_voxel_di_target(
    light : WorldGILight, ray : SparseVoxelSceneRay,
    normal : vec3<f32>) -> f32 {
    let direction = ray.directionMaximumDistance.xyz;
    let rawCosine = max(dot(normal, direction), 0.0);
    if (rawCosine <= 0.0) { return 0.0; }
    // Grazing receiver directions are proposed as often as any other - the
    // cosine-hemisphere proposal used for the sky cancels the cosine outright -
    // while their raw target approaches zero, and W = weightSum / (m * target)
    // then divides by that near-zero target and sprays fireflies. RIS stays
    // unbiased for any positive target, so this floor only trades a little
    // extra noise at grazing angles for bounded weights.
    let receiverCosine = max(rawCosine, SPARSE_VOXEL_DI_RECEIVER_COSINE_FLOOR);
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY) {
        return dot(sparse_voxel_sky_radiance(light, direction),
            SPARSE_VOXEL_DI_LUMINANCE) * receiverCosine;
    }
    let power = sparse_voxel_di_light_power(light);
    if (sparse_voxel_di_light_is_infinite(light)) {
        return power * receiverCosine;
    }
    // Finite lights are in area measure, so the emitter cosine and the inverse
    // square term belong to the integrand. Dropping the emitter cosine (as
    // this target originally did) gave full weight to samples on the far side
    // of a sphere and to every sample of a quad seen edge-on, all of which
    // shade to exactly zero. Those dead samples still counted in m and still
    // added their weight to weightSum, so a pixel that happened to select a
    // live sample received a correspondingly inflated W - alternating black
    // and over-bright pixels, which is exactly the salt-and-pepper firefly
    // pattern area lights produced.
    let sampledPoint = ray.originMinimumDistance.xyz +
        direction * ray.directionMaximumDistance.w;
    if (sparse_voxel_di_emitter_cosine(light, direction, sampledPoint) <= 0.0) {
        return 0.0;
    }
    let emitterCosine = sparse_voxel_di_emitter_cosine_floored(
        light, direction, sampledPoint);
    return power * receiverCosine * emitterCosine /
        max(ray.directionMaximumDistance.w * ray.directionMaximumDistance.w,
            1.0e-4);
}
