// Physical atmosphere ambient shares the same packed sky light used by every
// voxel GI path. The analytic uniforms below remain the legacy fallback.
#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"
#include "sparse_voxel_sky.wgsl"
#include "atmosphere/wgsl/atmosphere_common.wgsl"
#include "atmosphere/wgsl/atmosphere_terrain_shadow.wgsl"

struct PlanetUniforms {
    tangentX : vec4<f32>,
    tangentUp : vec4<f32>,
    tangentZ : vec4<f32>,
    // xyz: current tangent surface point relative to camera, w: radius.
    tangentCameraRadius : vec4<f32>,
    // x: source/package radius, y: maximum retained visual cap angle.
    sourceRadiusCap : vec4<f32>,
    // xy focal length in pixels, zw viewport size.
    focalViewport : vec4<f32>,
    // xy principal point in pixels, z fully voxel-owned transition distance,
    // w camera near distance.
    principalViewport : vec4<f32>,
    // rgb constant base colour, w selects it instead of textured biomes.
    baseColor : vec4<f32>,
    // xyz world/tangent up, w sky intensity.
    skyUpIntensity : vec4<f32>,
    // rgb environment radiance, w enables the sky contribution.
    skyHorizonEnabled : vec4<f32>,
    // rgb zenith radiance, w the authored horizon response exponent.
    skyZenith : vec4<f32>,
    skyGround : vec4<f32>,
    // xyz is the direction light travels, w is radiance/intensity.
    directionalDirectionIntensity : vec4<f32>,
    // rgb radiance tint, w enables the directional contribution.
    directionalColorEnabled : vec4<f32>,
    // x voxel SH normal-coherence damping; y finest voxel size in metres.
    lightingMatch : vec4<f32>,
    terrainShadowControl : vec4<f32>,
    terrainShadowFade : vec4<f32>,
    // xy polar texture dimensions, zw reciprocal dimensions.
    surfaceMapInfo : vec4<f32>,
    worldToCamera : mat4x4<f32>,
    cameraToWorld : mat4x4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : PlanetUniforms;
@group(0) @binding(1) var surfaceAlbedoRoughness : texture_2d<f32>;
@group(0) @binding(2) var surfaceSoilBlend : texture_2d<f32>;
@group(0) @binding(3) var surfaceTerrainFeatures : texture_2d<f32>;
@group(0) @binding(4) var surfaceSampler : sampler;
@group(0) @binding(5) var<storage, read> sceneLights : array<WorldGILight>;
@group(0) @binding(6) var terrainShadowMap : texture_depth_2d;
@group(0) @binding(7) var<storage, read> voxelCoverage : array<u32>;
@group(0) @binding(8) var<storage, read> voxelLinearDepth : array<f32>;

struct PlanetVertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) normalDepth : vec4<f32>,
    @location(1) relativePosition : vec3<f32>,
    @location(2) displayAngle : f32,
    @location(3) surfaceCoordinate : vec2<f32>,
}

fn rotate_world_to_camera(value : vec3<f32>) -> vec3<f32> {
    return (uniforms.worldToCamera * vec4<f32>(value, 0.0)).xyz;
}

fn srgb_to_linear(value : vec3<f32>) -> vec3<f32> {
    let low = value / 12.92;
    let high = pow((value + 0.055) / 1.055, vec3<f32>(2.4));
    return select(low, high, value > vec3<f32>(0.04045));
}

fn planet_cell_hash(cell : vec3<i32>) -> f32 {
    var value = bitcast<u32>(cell.x) * 0x8da6b343u;
    value = value ^ (bitcast<u32>(cell.y) * 0xd8163841u);
    value = value ^ (bitcast<u32>(cell.z) * 0xcb1ab31fu);
    value = value ^ (value >> 13u);
    value = value * 0x85ebca6bu;
    value = value ^ (value >> 16u);
    return f32(value & 0x00ffffffu) / 16777215.0;
}

fn is_water_material(materialId : f32) -> bool {
    return abs(materialId - 0.0) < 0.5 ||
        abs(materialId - 1.0) < 0.5 ||
        abs(materialId - 6.0) < 0.5 ||
        abs(materialId - 8.0) < 0.5;
}

fn is_vegetation_material(materialId : f32) -> bool {
    return abs(materialId - 3.0) < 0.5 ||
        (materialId >= 7.0 && materialId <= 9.5) ||
        (materialId >= 12.0 && materialId <= 19.5 &&
            abs(materialId - 14.0) > 0.5);
}

fn is_desert_material(materialId : f32) -> bool {
    return abs(materialId - 14.0) < 0.5 ||
        abs(materialId - 20.0) < 0.5 ||
        abs(materialId - 2.0) < 0.5;
}

fn is_geological_material(materialId : f32) -> bool {
    return abs(materialId - 4.0) < 0.5 ||
        abs(materialId - 10.0) < 0.5 ||
        abs(materialId - 11.0) < 0.5;
}

fn planet_value_noise(position : vec2<f32>, seed : i32) -> f32 {
    let cell = vec2<i32>(floor(position));
    let fraction = fract(position);
    let weight = fraction * fraction * (vec2<f32>(3.0) - 2.0 * fraction);
    let n00 = planet_cell_hash(vec3<i32>(cell, seed));
    let n10 = planet_cell_hash(vec3<i32>(cell + vec2<i32>(1, 0), seed));
    let n01 = planet_cell_hash(vec3<i32>(cell + vec2<i32>(0, 1), seed));
    let n11 = planet_cell_hash(vec3<i32>(cell + vec2<i32>(1, 1), seed));
    return mix(mix(n00, n10, weight.x), mix(n01, n11, weight.x), weight.y) - 0.5;
}

fn planet_material_cell_tint(
    position : vec2<f32>, cellSize : f32, materialId : f32) -> vec3<f32> {
    let cell = vec2<i32>(floor(position / cellSize));
    let seed = i32(materialId);
    let valueNoise = planet_cell_hash(vec3<i32>(cell, seed)) - 0.5;
    let hueNoise = planet_cell_hash(vec3<i32>(
        cell.yx + vec2<i32>(17, 37), seed + 71)) - 0.5;
    var tint = vec3<f32>(1.0 + valueNoise * 0.16);
    if (is_vegetation_material(materialId)) {
        tint *= vec3<f32>(
            1.0 + hueNoise * 0.055,
            1.0 + hueNoise * 0.140,
            1.0 - hueNoise * 0.070);
    } else if (is_desert_material(materialId)) {
        tint *= vec3<f32>(
            1.0 + hueNoise * 0.110,
            1.0 + hueNoise * 0.045,
            1.0 - hueNoise * 0.090);
    } else if (is_geological_material(materialId)) {
        tint *= vec3<f32>(
            1.0 + hueNoise * 0.075,
            1.0 + hueNoise * 0.025,
            1.0 - hueNoise * 0.060);
    }
    return tint;
}

fn planet_terrain_detail(
    position : vec2<f32>, cellSize : f32, materialId : f32) -> f32 {
    let seed = i32(materialId) * 31 + 113;
    let broad = planet_value_noise(position / (cellSize * 5.0), seed);
    let grain = planet_value_noise(position / (cellSize * 1.5), seed + 47);
    return broad * 0.68 + grain * 0.32;
}

fn planet_filtered_detail(
    position : vec2<f32>, gridSize : f32, nextGridSize : f32,
    gridBlend : f32, materialId : f32) -> f32 {
    return mix(
        planet_terrain_detail(position, gridSize, materialId),
        planet_terrain_detail(position, nextGridSize, materialId),
        gridBlend);
}

@vertex
fn drawPlanetVertex(
    @location(0) directionElevation : vec4<f32>,
    @location(1) surfaceCoordinate : vec4<f32>) -> PlanetVertexOutput {
    let direction = normalize(directionElevation.xyz);
    let sourceTangentDirection = vec3<f32>(
        dot(direction, uniforms.tangentX.xyz),
        dot(direction, uniforms.tangentUp.xyz),
        dot(direction, uniforms.tangentZ.xyz));
    let sourceRadial = length(sourceTangentDirection.xz);
    let sourceAngle = atan2(sourceRadial, sourceTangentDirection.y);
    let surfaceDistance = sourceAngle * uniforms.sourceRadiusCap.x;
    let displayAngle = surfaceDistance / uniforms.tangentCameraRadius.w;
    let retainedAngle = min(displayAngle, uniforms.sourceRadiusCap.y);
    let radialDirection = sourceTangentDirection.xz /
        max(sourceRadial, 1.0e-8);
    let tangentDirection = vec3<f32>(
        radialDirection.x * sin(retainedAngle),
        cos(retainedAngle),
        radialDirection.y * sin(retainedAngle));
    let radius = uniforms.tangentCameraRadius.w + directionElevation.w;
    // Subtract the tangent radius before scaling into metres. This keeps the
    // visible cap camera-relative instead of subtracting two 6,000 km float
    // positions after the fact.
    let relativeWorld = uniforms.tangentCameraRadius.xyz + vec3<f32>(
        tangentDirection.x * radius,
        tangentDirection.y * radius - uniforms.tangentCameraRadius.w,
        tangentDirection.z * radius);
    let camera = rotate_world_to_camera(relativeWorld);
    let halfViewport = max(uniforms.focalViewport.zw * 0.5, vec2<f32>(1.0));
    // Reverse-Z with an effectively infinite far plane. The background globe
    // is not constrained by the finite voxel projection; x/w and y/w retain
    // the exact camera perspective while near/w supplies monotonic depth.
    let nearDepth = max(uniforms.principalViewport.w, 1.0e-6);
    var output : PlanetVertexOutput;
    output.position = vec4<f32>(
        camera.x * uniforms.focalViewport.x / halfViewport.x,
        camera.y * uniforms.focalViewport.y / halfViewport.y,
        nearDepth,
        camera.z);
    output.normalDepth = vec4<f32>(normalize(tangentDirection), length(relativeWorld));
    output.relativePosition = relativeWorld;
    output.displayAngle = displayAngle;
    output.surfaceCoordinate = surfaceCoordinate.xy;
    return output;
}

fn planet_surface_uv(coordinate : vec2<f32>) -> vec2<f32> {
    let radius = clamp(length(coordinate), 0.0, 1.0);
    let angle = atan2(coordinate.y, coordinate.x) / 6.28318530718;
    let u = fract(angle + 1.0 + uniforms.surfaceMapInfo.z * 0.5);
    let v = (radius * (uniforms.surfaceMapInfo.y - 1.0) + 0.5) *
        uniforms.surfaceMapInfo.w;
    return vec2<f32>(u, v);
}

fn planet_wrapped_uv(uv : vec2<f32>) -> vec2<f32> {
    let halfV = uniforms.surfaceMapInfo.w * 0.5;
    return vec2<f32>(fract(uv.x + 1.0),
        clamp(uv.y, halfV, 1.0 - halfV));
}

fn planet_material_at(uv : vec2<f32>) -> f32 {
    let wrapped = planet_wrapped_uv(uv);
    let pixel = vec2<i32>(
        i32(floor(wrapped.x * uniforms.surfaceMapInfo.x)),
        i32(floor(wrapped.y * uniforms.surfaceMapInfo.y)));
    return round(textureLoad(surfaceTerrainFeatures, pixel, 0).x * 255.0);
}

fn planet_material_traits(materialId : f32) -> vec3<f32> {
    return vec3<f32>(
        select(0.0, 1.0, is_vegetation_material(materialId)),
        select(0.0, 1.0, is_desert_material(materialId)),
        select(0.0, 1.0, is_geological_material(materialId)));
}

struct PlanetSurfaceSample {
    albedo : vec4<f32>,
    soil : vec4<f32>,
    traits : vec3<f32>,
}

// Filter only within the centre pixel's land/water domain. This makes broad,
// organic ecotones between land biomes without smearing coastlines into the
// ocean. The smoothly varying warp prevents the transition from following a
// square source-raster edge.
fn planet_blended_surface(coordinate : vec2<f32>) -> PlanetSurfaceSample {
    let uv = planet_surface_uv(coordinate);
    let centreMaterial = planet_material_at(uv);
    let centreWater = is_water_material(centreMaterial);
    let warp = vec2<f32>(
        planet_value_noise(coordinate * 19.0 + vec2<f32>(17.3, -8.1), 211),
        planet_value_noise(coordinate * 23.0 + vec2<f32>(-4.7, 31.9), 317));
    let radius = 2.4 + 1.8 *
        (planet_value_noise(coordinate * 11.0, 401) + 0.5);
    let texel = uniforms.surfaceMapInfo.zw;
    let offsets = array<vec2<f32>, 12>(
        vec2<f32>(0.0, 0.0),
        vec2<f32>(1.0, 0.0), vec2<f32>(-1.0, 0.0),
        vec2<f32>(0.0, 1.0), vec2<f32>(0.0, -1.0),
        vec2<f32>(0.707, 0.707), vec2<f32>(-0.707, 0.707),
        vec2<f32>(-0.707, -0.707), vec2<f32>(0.707, -0.707),
        vec2<f32>(1.85, 0.76), vec2<f32>(-0.76, 1.85),
        vec2<f32>(-1.85, -0.76));
    let weights = array<f32, 12>(
        1.0, 0.62, 0.62, 0.62, 0.62,
        0.48, 0.48, 0.48, 0.48, 0.20, 0.20, 0.20);
    var albedo = vec4<f32>(0.0);
    var soil = vec4<f32>(0.0);
    var traits = vec3<f32>(0.0);
    var total = 0.0;
    for (var index = 0u; index < 12u; index += 1u) {
        let offset = (offsets[index] * radius + warp * 1.35) * texel;
        let tapUV = planet_wrapped_uv(uv + offset);
        let material = planet_material_at(tapUV);
        let sameDomain = is_water_material(material) == centreWater;
        let weight = select(0.0, weights[index], sameDomain);
        albedo += textureSample(surfaceAlbedoRoughness,
            surfaceSampler, tapUV) * weight;
        soil += textureSample(surfaceSoilBlend,
            surfaceSampler, tapUV) * weight;
        traits += planet_material_traits(material) * weight;
        total += weight;
    }
    return PlanetSurfaceSample(
        albedo / max(total, 1.0e-5),
        soil / max(total, 1.0e-5),
        traits / max(total, 1.0e-5));
}

fn planet_sky_radiance(direction : vec3<f32>) -> vec3<f32> {
    for (var index = 0u; index < arrayLength(&sceneLights); index += 1u) {
        let light = sceneLights[index];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ATMOSPHERE_SH) != 0u) {
            return sparse_voxel_sky_radiance(light, direction);
        }
    }
    let up = normalize(uniforms.skyUpIntensity.xyz);
    let elevation = clamp(dot(normalize(direction), up), -1.0, 1.0);
    let destination = select(
        uniforms.skyGround.rgb, uniforms.skyZenith.rgb, elevation >= 0.0);
    let curve = pow(abs(elevation), max(uniforms.skyZenith.w, 0.01));
    return mix(uniforms.skyHorizonEnabled.rgb, destination, curve) *
        uniforms.skyUpIntensity.w * uniforms.skyHorizonEnabled.w;
}

// The voxel lighting buffer stores cosine-convolved irradiance and composition
// applies it directly to albedo. This helper returns the cosine-importance
// sampled mean radiance; drawPlanetFragment restores the hemisphere PI factor
// to follow that same renderer energy convention.
fn planet_sky_diffuse(normal : vec3<f32>) -> vec3<f32> {
    let reference = select(
        vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0),
        abs(normal.y) > 0.98);
    let tangent = normalize(cross(reference, normal));
    let bitangent = cross(normal, tangent);
    var result = vec3<f32>(0.0);
    for (var index = 0u; index < 8u; index += 1u) {
        let radial2 = (f32(index) + 0.5) / 8.0;
        let radial = sqrt(radial2);
        let angle = f32(index) * 2.39996323;
        let local = vec3<f32>(
            radial * cos(angle), sqrt(max(1.0 - radial2, 0.0)),
            radial * sin(angle));
        let direction = normalize(
            tangent * local.x + normal * local.y + bitangent * local.z);
        result += planet_sky_radiance(direction);
    }
    return result * 0.125;
}

// Exact spherical mean of the axisymmetric sky model above. This is the SH
// band-zero value the voxel probe retains when normal coherence damping removes
// all directional bands: integral(pow(mu, exponent), mu=0..1) is simply
// 1/(exponent+1).
fn planet_sky_mean_radiance() -> vec3<f32> {
    for (var index = 0u; index < arrayLength(&sceneLights); index += 1u) {
        let light = sceneLights[index];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ATMOSPHERE_SH) != 0u) {
            return max(light.positionRange.xyz * 0.2820947918,
                vec3<f32>(0.0)) * max(light.colorIntensity.w, 0.0);
        }
    }
    let exponent = max(uniforms.skyZenith.w, 0.01);
    let curveMean = 1.0 / (exponent + 1.0);
    let destinationMean =
        (uniforms.skyZenith.rgb + uniforms.skyGround.rgb) * 0.5;
    return mix(
        uniforms.skyHorizonEnabled.rgb, destinationMean, curveMean) *
        uniforms.skyUpIntensity.w * uniforms.skyHorizonEnabled.w;
}

// Irradiance reconstructed from a directional delta after projection into the
// same second-order SH basis used by sparse_voxel_probe_common.wgsl. At
// sharpness one this is the probe's ordinary clamped-cosine approximation; at
// zero it is its stable DC term. Using max(dot(n,l),0) here was the lighting
// mismatch: the raster surface stayed fully directional while distant voxel
// faces deliberately reduced their first and second bands.
fn planet_directional_sh_response(cosine : f32, sharpness : f32) -> f32 {
    if (cosine <= 0.0) { return 0.0; }
    let bandOne = clamp(sharpness, 0.0, 1.0);
    let bandTwo = bandOne * bandOne;
    return max(
        0.25 + 0.5 * cosine * bandOne +
            0.15625 * (3.0 * cosine * cosine - 1.0) * bandTwo,
        0.0);
}

struct PlanetDirectionalLight {
    directionTowardLight : vec3<f32>,
    radiance : vec3<f32>,
    enabled : f32,
}

// The shared light buffer already contains the direct solar transmittance
// applied on the CPU. Reading it here keeps the raster planet and voxel probes
// on the same atmospheric colour/intensity at sunrise and sunset.
fn planet_directional_light() -> PlanetDirectionalLight {
    for (var index = 0u; index < arrayLength(&sceneLights); index += 1u) {
        let light = sceneLights[index];
        if (light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL &&
            (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u) {
            return PlanetDirectionalLight(
                normalize(-light.directionType.xyz),
                max(light.colorIntensity.rgb, vec3<f32>(0.0)) *
                    max(light.colorIntensity.w, 0.0),
                1.0);
        }
    }
    return PlanetDirectionalLight(
        normalize(-uniforms.directionalDirectionIntensity.xyz),
        max(uniforms.directionalColorEnabled.rgb, vec3<f32>(0.0)) *
            max(uniforms.directionalDirectionIntensity.w, 0.0),
        uniforms.directionalColorEnabled.w);
}

// Match SparseVoxelPlanetMergePass exactly. A covered voxel inside the fully
// voxel-owned portion of the transition never samples the planet attachments,
// so avoid the background's material, procedural-detail and lighting work.
// Coverage holes and every cross-fade pixel continue through the full path.
fn planet_hidden_by_owned_voxel(position : vec4<f32>) -> bool {
    if (uniforms.principalViewport.z < 0.0) { return false; }
    let pixel = vec2<u32>(position.xy);
    let pixelIndex = pixel.y * u32(uniforms.focalViewport.z) + pixel.x;
    if (voxelCoverage[pixelIndex] == 0u) { return false; }
    let focal = max(abs(uniforms.focalViewport.xy), vec2<f32>(1.0e-6));
    let focalSign = select(
        vec2<f32>(-1.0), vec2<f32>(1.0),
        uniforms.focalViewport.xy >= vec2<f32>(0.0));
    let signedFocal = focal * focalSign;
    let viewX = (position.x - uniforms.principalViewport.x) / signedFocal.x;
    let viewY = (position.y - uniforms.principalViewport.y) / signedFocal.y;
    let voxelRay = normalize(
        uniforms.cameraToWorld[0].xyz * viewX +
        uniforms.cameraToWorld[1].xyz * viewY +
        uniforms.cameraToWorld[2].xyz);
    let horizontalDistance = voxelLinearDepth[pixelIndex] *
        length(voxelRay.xz);
    return horizontalDistance <= uniforms.principalViewport.z;
}

struct PlanetFragmentOutput {
    @location(0) color : vec4<f32>,
    @location(1) normalDepth : vec4<f32>,
}

@fragment
fn drawPlanetFragment(input : PlanetVertexOutput) -> PlanetFragmentOutput {
    if (input.displayAngle > uniforms.sourceRadiusCap.y) {
        discard;
    }
    if (planet_hidden_by_owned_voxel(input.position)) {
        discard;
    }
    // Match the planet preview's restrained derivative-normal relief without
    // exposing every coarse triangle as a hard facet.
    let radialNormal = normalize(input.normalDepth.xyz);
    var reliefNormal = normalize(cross(
        dpdx(input.relativePosition), dpdy(input.relativePosition)));
    reliefNormal = select(
        -reliefNormal, reliefNormal,
        dot(reliefNormal, radialNormal) >= 0.0);
    var normal = normalize(mix(radialNormal, reliefNormal, 0.58));

    let surfaceUV = planet_surface_uv(input.surfaceCoordinate);
    let texturedSurface = planet_blended_surface(input.surfaceCoordinate);
    let centreFeatures = textureSample(surfaceTerrainFeatures,
        surfaceSampler, surfaceUV);
    let materialId = planet_material_at(surfaceUV);
    let constantColor = srgb_to_linear(uniforms.baseColor.rgb);

    // Match the default voxel grass-over-soil graph. Its exposed top remains
    // green on flat terrain and blends to the packed secondary soil colour
    // between authored slopes of 18 and 42 degrees.
    let normalizedSlope = acos(clamp(
        dot(reliefNormal, radialNormal), 0.0, 1.0)) / 1.57079632679;
    let soilWeight = smoothstep(18.0 / 90.0, 42.0 / 90.0,
        normalizedSlope) * clamp(texturedSurface.soil.a, 0.0, 1.0);
    var surfaceColor = select(
        mix(texturedSurface.albedo.rgb, texturedSurface.soil.rgb, soilWeight),
        constantColor, uniforms.baseColor.w > 0.5);

    // A power-of-two world-space grid is stable while the camera moves and
    // naturally tracks the projected footprint. Each cell reads as a tiny
    // distant voxel, with biome-specific chroma rather than generic noise.
    let metresPerPixel = max(length(dpdx(input.relativePosition.xz)),
        length(dpdy(input.relativePosition.xz)));
    let gridLOD = clamp(log2(max(metresPerPixel * 1.75, 2.0)), 1.0, 13.0);
    let gridSize = exp2(floor(gridLOD));
    let nextGridSize = min(gridSize * 2.0, 16384.0);
    let gridBlend = smoothstep(0.0, 1.0, fract(gridLOD));
    let materialTint = mix(
        planet_material_cell_tint(
            input.relativePosition.xz, gridSize, materialId),
        planet_material_cell_tint(
            input.relativePosition.xz, nextGridSize, materialId),
        gridBlend);
    let gridStrength = select(0.82, 0.18, is_water_material(materialId)) *
        select(1.0, 0.0, uniforms.baseColor.w > 0.5);
    surfaceColor *= mix(vec3<f32>(1.0), materialTint, gridStrength);

    // Add band-limited height detail at roughly one projected pixel. Sampling
    // adjacent power-of-two levels avoids swimming and moire while the colour
    // and perturbed normal retain some of the voxel surface's local contrast.
    let detailStep = mix(gridSize, nextGridSize, gridBlend) * 0.65;
    let detailCenter = planet_filtered_detail(
        input.relativePosition.xz, gridSize, nextGridSize, gridBlend,
        materialId);
    let detailX = planet_filtered_detail(
        input.relativePosition.xz + vec2<f32>(detailStep, 0.0),
        gridSize, nextGridSize, gridBlend, materialId);
    let detailZ = planet_filtered_detail(
        input.relativePosition.xz + vec2<f32>(0.0, detailStep),
        gridSize, nextGridSize, gridBlend, materialId);
    let detailGradient = vec2<f32>(detailX, detailZ) - detailCenter;
    var detailNormalStrength = select(0.16, 0.23,
        is_geological_material(materialId));
    var detailColorStrength = select(0.11, 0.16,
        is_geological_material(materialId));
    if (is_water_material(materialId)) {
        detailNormalStrength = 0.015;
        detailColorStrength = 0.015;
    }
    let detailEnabled = select(1.0, 0.0, uniforms.baseColor.w > 0.5);
    normal = normalize(normal + vec3<f32>(
        -detailGradient.x, 0.0, -detailGradient.y) *
        detailNormalStrength * detailEnabled);
    surfaceColor *= 1.0 + detailCenter * detailColorStrength * detailEnabled;

    // Mountain/uplift fields are continuous textures. Their restrained
    // neutral-rock modulation helps the local cap match voxel slopes without
    // reintroducing a categorical biome edge.
    let mountain = smoothstep(0.18, 0.82, centreFeatures.y);
    let uplift = smoothstep(0.24, 0.88, centreFeatures.z);
    let geological = texturedSurface.traits.z;
    let rockTint = mix(surfaceColor, vec3<f32>(0.43, 0.42, 0.40),
        0.28 + geological * 0.16);
    surfaceColor = mix(surfaceColor, rockTint,
        mountain * (0.18 + uplift * 0.18) * detailEnabled);

    // Raster-only approximation of the scene lighting. The hemispherical
    // ambient picks up the authored sky/ground mood, while the same directional
    // light used by the voxel world gives both representations a common key.
    // Estimate how often the equivalent distant voxel footprint straddles a
    // top/riser edge. Flat terrain remains fully directional. Sloped terrain
    // only approaches the voxel coherence damping once a finest cell projects
    // below roughly a pixel, which keeps this continuous through the handoff.
    let projectedVoxelFootprint = metresPerPixel /
        max(uniforms.lightingMatch.y, 1.0e-4);
    let terraceIncoherence =
        smoothstep(0.025, 0.22, normalizedSlope) *
        smoothstep(0.35, 1.25, projectedVoxelFootprint);
    let lightingSharpness = 1.0 -
        clamp(uniforms.lightingMatch.x, 0.0, 1.0) * terraceIncoherence;
    let sharpSkyIrradiance =
        planet_sky_diffuse(normal) * 3.14159265359;
    let dcSkyIrradiance =
        planet_sky_mean_radiance() * 3.14159265359;
    let skyIrradiance = mix(
        dcSkyIrradiance, sharpSkyIrradiance, lightingSharpness);
    let directionalLight = planet_directional_light();
    let lightDirection = directionalLight.directionTowardLight;
    let terrainVisibility = atmosphere_terrain_shadow_visibility(
        terrainShadowMap, input.relativePosition * 0.001,
        uniforms.skyUpIntensity.xyz, lightDirection,
        uniforms.terrainShadowControl, uniforms.terrainShadowFade);
    // Retain the probe-compatible SH lobe at the representation seam, but
    // restore enough Lambert response to make large-scale relief readable.
    let lightCosine = dot(normal, lightDirection);
    let directResponse = mix(
        planet_directional_sh_response(lightCosine, lightingSharpness),
        max(lightCosine, 0.0), 0.55);
    let direct = directionalLight.radiance * directResponse *
        select(0.0, 1.0, dot(radialNormal, lightDirection) > 0.0) *
        terrainVisibility * directionalLight.enabled;
    let hasSceneLight = uniforms.skyHorizonEnabled.w > 0.0 ||
        uniforms.directionalColorEnabled.w > 0.0;
    // The voxel compose path multiplies albedo by the probe result directly.
    // Its SH convolution includes the cosine-hemisphere PI factor, so keep the
    // raster background in that same renderer energy convention.
    let irradiance = select(
        vec3<f32>(1.0), skyIrradiance + direct, hasSceneLight);
    // Approximate the missing short-range self-shadowing at the representation
    // handoff. Steeper height-map facets receive less open-sky energy, which
    // makes distant hills sit in the same value range as voxel slopes.
    let slopeOcclusion = mix(1.0, 0.90,
        smoothstep(0.05, 0.48, normalizedSlope));
    return PlanetFragmentOutput(
        vec4<f32>(max(surfaceColor * irradiance * slopeOcclusion,
            vec3<f32>(0.0)), 1.0),
        vec4<f32>(normal, input.normalDepth.w));
}
