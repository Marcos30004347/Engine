#include "sparse_voxel_types.wgsl"

// Final material composition.
//
// Everything upstream of this pass carries a material-independent diffuse
// light multiplier: visibility, light colour, attenuation and cosine terms,
// but no albedo, emissive or environment. That separation is what lets the
// denoiser accumulate lighting without smearing material detail. This pass is
// where the light multiplier becomes a shaded image, and it is therefore the
// first point in the frame where a temporal upscaler may legitimately run:
// TSR's neighbourhood clipping, luma reactivity and lock heuristics are built
// for shaded colour, not for a Monte Carlo light estimate.

struct SparseVoxelComposeUniforms {
    width : u32,
    height : u32,
    flags : u32,
    padding : u32,
    skyUpExponent : vec4<f32>,
    skyHorizonIntensity : vec4<f32>,
    skyZenith : vec4<f32>,
    skyGround : vec4<f32>,
    // x start distance, y fully fogged distance, z response exponent.
    fogDistances : vec4<f32>,
    // rgb custom fog colour, w maximum opacity.
    fogColorOpacity : vec4<f32>,
}

const COMPOSE_FLAG_APPLY_ALBEDO : u32 = 1u << 0u;
const COMPOSE_FLAG_APPLY_EMISSIVE : u32 = 1u << 1u;
const COMPOSE_FLAG_APPLY_SKY_BACKGROUND : u32 = 1u << 2u;
const COMPOSE_FLAG_APPLY_DISTANCE_FOG : u32 = 1u << 3u;
const COMPOSE_FLAG_FOG_USES_SKY_COLOR : u32 = 1u << 4u;

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelComposeUniforms;
@group(0) @binding(1) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(2) var lightingTexture : texture_2d<f32>;
@group(0) @binding(3) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(4) var emissiveTexture : texture_2d<f32>;
@group(0) @binding(5) var<storage, read> coverage : array<u32>;
@group(0) @binding(6) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(7) var outputTexture : texture_storage_2d<rgba16float, write>;

fn compose_sky_radiance(pixel : vec2<u32>) -> vec3<f32> {
    // Deliberately unjittered. raster.focalPrincipal.zw carries the temporal
    // jitter baked into the principal point, which is what antialiases
    // geometry edges. The background has no geometry to antialias, and the
    // upscaler force-disoccludes background pixels every frame (they have no
    // surface to reproject), so a jittered sky direction never accumulates and
    // instead shows up as per-frame shimmer. Sampling the analytic sky through
    // the stable principal point makes the background identical frame to frame
    // for a static camera.
    let principal = vec2<f32>(f32(uniforms.width), f32(uniforms.height)) * 0.5;
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - principal.x) /
            max(raster.focalPrincipal.x, 1.0e-6),
        (f32(pixel.y) + 0.5 - principal.y) /
            max(raster.focalPrincipal.y, 1.0e-6),
        1.0);
    // Only the elevation is needed. skyUpExponent.xyz is transformed into
    // camera space once by the CPU instead of repeating three matrix dot
    // products for every background pixel.
    let cameraSpaceUp = uniforms.skyUpExponent.xyz;
    let inverseDirectionLength = inverseSqrt(max(
        dot(cameraDirection, cameraDirection), 1.0e-12));
    let elevation = clamp(
        dot(cameraDirection, cameraSpaceUp) * inverseDirectionLength,
        -1.0, 1.0);
    let destinationColor = select(
        uniforms.skyGround.xyz, uniforms.skyZenith.xyz, elevation >= 0.0);
    let curve = pow(abs(elevation), uniforms.skyUpExponent.w);
    return mix(uniforms.skyHorizonIntensity.xyz, destinationColor, curve) *
        uniforms.skyHorizonIntensity.w;
}

@compute @workgroup_size(16, 16, 1)
fn composeSparseVoxelShading(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (pixel.x >= uniforms.width || pixel.y >= uniforms.height) {
        return;
    }
    let pixelIndex = pixel.y * uniforms.width + pixel.x;
    let pixelI = vec2<i32>(pixel);

    if (coverage[pixelIndex] == 0u) {
        // No receiver: show the environment. Compositing the background here
        // rather than in the light buffer keeps the denoiser operating on
        // lighting alone.
        var background = vec3<f32>(0.0);
        if ((uniforms.flags & COMPOSE_FLAG_APPLY_SKY_BACKGROUND) != 0u) {
            background = compose_sky_radiance(pixel);
        }
        textureStore(outputTexture, pixelI, vec4<f32>(background, 1.0));
        return;
    }

    let lighting = max(
        textureLoad(lightingTexture, pixelI, 0).rgb, vec3<f32>(0.0));
    var albedo = vec3<f32>(1.0);
    if ((uniforms.flags & COMPOSE_FLAG_APPLY_ALBEDO) != 0u) {
        albedo = max(
            textureLoad(baseColorTexture, pixelI, 0).rgb, vec3<f32>(0.0));
    }
    var emissive = vec3<f32>(0.0);
    if ((uniforms.flags & COMPOSE_FLAG_APPLY_EMISSIVE) != 0u) {
        emissive = max(
            textureLoad(emissiveTexture, pixelI, 0).rgb, vec3<f32>(0.0));
    }
    var shaded = albedo * lighting + emissive;
    if ((uniforms.flags & COMPOSE_FLAG_APPLY_DISTANCE_FOG) != 0u &&
        pixelIndex < arrayLength(&linearDepth)) {
        let fogRange = max(
            uniforms.fogDistances.y - uniforms.fogDistances.x, 1.0e-3);
        let fogLinear = clamp(
            (linearDepth[pixelIndex] - uniforms.fogDistances.x) / fogRange,
            0.0, 1.0);
        // Sky radiance is evaluated from the same stable direction as the
        // background, so the terrain reaches the horizon without a colour
        // seam and does not shimmer with projection jitter.
        let fogAmount = pow(fogLinear, uniforms.fogDistances.z) *
            clamp(uniforms.fogColorOpacity.w, 0.0, 1.0);
        let fogColor = select(
            max(uniforms.fogColorOpacity.xyz, vec3<f32>(0.0)),
            compose_sky_radiance(pixel),
            (uniforms.flags & COMPOSE_FLAG_FOG_USES_SKY_COLOR) != 0u);
        shaded = mix(shaded, fogColor, fogAmount);
    }
    textureStore(outputTexture, pixelI, vec4<f32>(shaded, 1.0));
}
