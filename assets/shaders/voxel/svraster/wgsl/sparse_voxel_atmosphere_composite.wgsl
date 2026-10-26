#include "sparse_voxel_types.wgsl"
#include "atmosphere/wgsl/atmosphere_common.wgsl"

struct SparseVoxelAtmosphereCompositeUniforms {
    atmosphere : AtmosphereParameters,
    // xy viewport, z enabled, w draw sun disk.
    viewportFlags : vec4<u32>,
    // xyz local planet up in world space, w camera radius km.
    localUpCameraRadius : vec4<f32>,
    // x disk luminance scale.
    display : vec4<f32>,
    // x aerial maximum distance km, y distribution scale km.
    aerial : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelAtmosphereCompositeUniforms;
@group(0) @binding(1) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(2) var surfaceColor : texture_2d<f32>;
@group(0) @binding(3) var transmittanceLut : texture_2d<f32>;
@group(0) @binding(4) var skyViewLut : texture_2d<f32>;
@group(0) @binding(5) var aerialPerspectiveLut : texture_3d<f32>;
@group(0) @binding(6) var aerialSampler : sampler;
@group(0) @binding(7) var<storage, read> coverage : array<u32>;
@group(0) @binding(8) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(9) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(10) var reactiveMask : texture_storage_2d<r16float, read_write>;
@group(0) @binding(11) var skyViewSampler : sampler;

fn atmosphere_star_hash(value : vec2<f32>) -> vec3<f32> {
    return fract(sin(vec3<f32>(
        dot(value, vec2<f32>(127.1, 311.7)),
        dot(value, vec2<f32>(269.5, 183.3)),
        dot(value, vec2<f32>(419.2, 371.9)))) * 43758.5453);
}

fn atmosphere_star_layer(direction : vec3<f32>, resolution : f32,
    threshold : f32) -> vec3<f32> {
    let uv = vec2<f32>(
        atan2(direction.z, direction.x) / (2.0 * ATMOSPHERE_PI) + 0.5,
        asin(clamp(direction.y, -1.0, 1.0)) / ATMOSPHERE_PI + 0.5);
    let grid = vec2<f32>(resolution, resolution * 0.5);
    let coordinate = uv * grid;
    let cell = floor(coordinate);
    let random = atmosphere_star_hash(cell);
    let distanceToStar = length(fract(coordinate) - random.xy);
    let point = 1.0 - smoothstep(0.015, 0.055, distanceToStar);
    let exists = step(threshold, random.z);
    let temperature = atmosphere_star_hash(cell + vec2<f32>(19.0, 71.0));
    let color = mix(vec3<f32>(0.62, 0.75, 1.0),
        vec3<f32>(1.0, 0.78, 0.55), temperature.x);
    return color * point * exists * mix(0.4, 1.0, temperature.y);
}

fn atmosphere_stars(direction : vec3<f32>) -> vec3<f32> {
    return atmosphere_star_layer(direction, 512.0, 0.995) +
        atmosphere_star_layer(direction, 1024.0, 0.998) * 0.65;
}

fn atmosphere_write_reactive(pixel : vec2<i32>, direction : vec3<f32>,
    up : vec3<f32>, directionTowardSun : vec3<f32>) {
    let previous = textureLoad(reactiveMask, pixel).r;
    let motion = clamp(uniforms.display.z, 0.0, 1.0);
    let sunOuterRadius = uniforms.atmosphere.sunColorAngularRadius.w * 6.0 +
        motion * 0.05;
    let sunHalo = smoothstep(cos(sunOuterRadius),
        cos(uniforms.atmosphere.sunColorAngularRadius.w),
        dot(direction, directionTowardSun));
    let sunAtHorizon = 1.0 - smoothstep(0.04, 0.20,
        abs(dot(up, directionTowardSun)));
    let viewAtHorizon = 1.0 - smoothstep(0.02, 0.15,
        abs(dot(up, direction)));
    let sunsetBand = sunAtHorizon * viewAtHorizon;
    let reactive = max(previous, max(uniforms.display.w,
        motion * max(sunHalo, sunsetBand)));
    textureStore(reactiveMask, pixel,
        vec4<f32>(clamp(reactive, 0.0, 1.0), 0.0, 0.0, 0.0));
}

fn atmosphere_composite_view_direction(pixel : vec2<u32>) -> vec3<f32> {
    // Use the stable principal point. Geometry has already been rasterized;
    // jittering the background here creates shimmer instead of antialiasing.
    let principal = vec2<f32>(uniforms.viewportFlags.xy) * 0.5;
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - principal.x) /
            max(abs(raster.focalPrincipal.x), 1.0e-6),
        (f32(pixel.y) + 0.5 - principal.y) /
            max(abs(raster.focalPrincipal.y), 1.0e-6),
        1.0);
    return normalize((raster.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
}

fn atmosphere_sample_aerial(pixel : vec2<u32>, distanceKm : f32) -> vec4<f32> {
    let dimensions = textureDimensions(aerialPerspectiveLut);
    let normalizedDepth = atmosphere_aerial_depth(distanceKm,
        uniforms.aerial.x, uniforms.aerial.y);
    let endpoint = normalizedDepth * f32(dimensions.z);
    let lowerEndpoint = min(u32(floor(endpoint)), dimensions.z);
    let upperEndpoint = min(lowerEndpoint + 1u, dimensions.z);
    let blend = fract(endpoint);
    let xy = (vec2<f32>(pixel) + vec2<f32>(0.5)) /
        vec2<f32>(uniforms.viewportFlags.xy);
    var lower = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    if (lowerEndpoint > 0u) {
        lower = textureSampleLevel(aerialPerspectiveLut, aerialSampler,
            vec3<f32>(xy, (f32(lowerEndpoint) - 0.5) / f32(dimensions.z)), 0.0);
    }
    let upper = textureSampleLevel(aerialPerspectiveLut, aerialSampler,
        vec3<f32>(xy, (f32(max(upperEndpoint, 1u)) - 0.5) /
            f32(dimensions.z)), 0.0);
    let result = mix(lower, upper, blend);
    // Exact zero is not a physically valid cumulative endpoint: even a fully
    // opaque volume contains scattering, while clear air has transmittance one.
    // Treat unwritten/corrupt froxels as identity so a partial volume update can
    // never black out a rectangular part of the screen.
    let finiteValue = all(result == result) &&
        all(abs(result) < vec4<f32>(65504.0));
    let unwritten = all(abs(result) < vec4<f32>(1.0e-7));
    return select(vec4<f32>(0.0, 0.0, 0.0, 1.0),
        vec4<f32>(max(result.rgb, vec3<f32>(0.0)),
            clamp(result.a, 0.0, 1.0)), finiteValue && !unwritten);
}

@compute @workgroup_size(16, 16, 1)
fn compositeSparseVoxelAtmosphere(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportFlags.xy)) { return; }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * uniforms.viewportFlags.x + pixel.x;
    let source = textureLoad(surfaceColor, pixelI, 0);
    if (uniforms.viewportFlags.z == 0u ||
        pixelIndex >= arrayLength(&coverage)) {
        textureStore(outputTexture, pixelI, source);
        return;
    }

    let up = atmosphere_safe_normalize(uniforms.localUpCameraRadius.xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    let direction = atmosphere_composite_view_direction(pixel);
    let directionTowardSun = atmosphere_safe_normalize(
        -uniforms.atmosphere.sunDirectionIntensity.xyz, up);
    atmosphere_write_reactive(pixelI, direction, up, directionTowardSun);

    if (coverage[pixelIndex] != 0u) {
        if (pixelIndex >= arrayLength(&linearDepth)) {
            textureStore(outputTexture, pixelI, source);
            return;
        }
        let distanceKm = max(linearDepth[pixelIndex], 0.0) * 0.001;
        let aerial = atmosphere_sample_aerial(pixel, distanceKm);
        textureStore(outputTexture, pixelI, vec4<f32>(
            source.rgb * clamp(aerial.a, 0.0, 1.0) + max(aerial.rgb, vec3<f32>(0.0)),
            source.a));
        return;
    }

    let skyUv = atmosphere_sky_view_uv(direction, up, directionTowardSun);
    // The deliberately low-frequency Sky-View LUT must be filtered when it is
    // expanded to the display. Nearest texel loads expose the nonlinear
    // latitude rows as broad staircase/compression bands at the horizon.
    var radiance = max(textureSampleLevel(
        skyViewLut, skyViewSampler, skyUv, 0.0).rgb, vec3<f32>(0.0));

    let planetOrigin = up * uniforms.localUpCameraRadius.w;
    let groundOccluded = atmosphere_nearest_positive(atmosphere_ray_sphere(
        planetOrigin, direction, uniforms.atmosphere.radiiMieG.x)) > 0.0;
    let night = 1.0 - smoothstep(-0.12, 0.0,
        dot(up, directionTowardSun));
    if (uniforms.display.y > 0.0 && night > 0.0 && !groundOccluded) {
        let starTransmittance = atmosphere_sample_transmittance(
            transmittanceLut, uniforms.atmosphere,
            uniforms.localUpCameraRadius.w, dot(up, direction));
        radiance += atmosphere_stars(direction) * uniforms.display.y *
            night * starTransmittance;
    }

    if (uniforms.viewportFlags.w != 0u &&
        uniforms.atmosphere.sunDirectionIntensity.w > 0.0 &&
        dot(direction, directionTowardSun) >=
            cos(uniforms.atmosphere.sunColorAngularRadius.w)) {
        if (!groundOccluded) {
            let sunTransmittance = atmosphere_sample_transmittance(
                transmittanceLut, uniforms.atmosphere,
                uniforms.localUpCameraRadius.w, dot(up, directionTowardSun));
            radiance += uniforms.atmosphere.sunColorAngularRadius.xyz *
                uniforms.atmosphere.sunDirectionIntensity.w *
                uniforms.atmosphere.groundAlbedoRadianceScale.w *
                uniforms.display.x * sunTransmittance;
        }
    }
    textureStore(outputTexture, pixelI, vec4<f32>(radiance, 1.0));
}
