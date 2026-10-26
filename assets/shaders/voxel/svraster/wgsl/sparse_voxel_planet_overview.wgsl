#include "sparse_voxel_planet_cbt_contract.wgsl"

struct OverviewUniforms {
    // xy viewport, z vertical focal scale, w near plane.
    viewportFocal : vec4<f32>,
    cameraDirectionDistance : vec4<f32>,
    cameraRightRelief : vec4<f32>,
    cameraUpSourceRadius : vec4<f32>,
    sunDirectionIntensity : vec4<f32>,
    sunColorAtmosphere : vec4<f32>,
    featureStrengths : vec4<f32>,
    markerDirectionEnabled : vec4<f32>,
    markerColorRadius : vec4<f32>,
    // xy material-atlas dimensions; zw detail bias/range in planet radii.
    surfaceMapInfo : vec4<f32>,
    // x geometry (0 static, 1 adaptive), y diagnostic (0 shaded, 1 wireframe,
    // 2 depth heat map), z maximum diagnostic depth, w maximum render-only
    // subdivision depth below each adaptive CBT patch.
    renderControls : vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : OverviewUniforms;
@group(0) @binding(1) var surfaceAlbedoRoughness : texture_2d<f32>;
@group(0) @binding(2) var surfaceTerrainFeatures : texture_2d<f32>;
@group(0) @binding(3) var<storage, read> cbtBisectors : array<PlanetCbtBisectorGPU>;
@group(0) @binding(4) var<storage, read> cbtPointers : array<PlanetCbtPointerGPU>;
@group(0) @binding(5) var<storage, read> cbtRoots : array<PlanetCbtRootTriangleGPU>;
@group(0) @binding(6) var surfaceHeight : texture_2d<f32>;
@group(0) @binding(7) var surfaceLinearSampler : sampler;

fn surfaceAtlasTexel(direction : vec3<f32>, mip : u32) -> vec2<i32> {
    return planetCbtCubeAtlasTexel(direction,
        max(u32(uniforms.surfaceMapInfo.x), 1u), mip);
}

struct OverviewVertexOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) planetPosition : vec3<f32>,
    @location(1) radialNormal : vec3<f32>,
    @location(2) baseColorForest : vec4<f32>,
    @location(3) soilColorMountain : vec4<f32>,
    @location(4) climateRiverWater : vec4<f32>,
    @location(5) elevationNormalized : f32,
    @location(6) barycentric : vec3<f32>,
    @location(7) subdivisionDepth : f32,
    @location(8) @interpolate(flat) adaptiveGeometry : u32,
}

fn overviewOutput(
    directionElevation : vec4<f32>,
    baseColorForest : vec4<f32>,
    soilColorMountain : vec4<f32>,
    climateRiverWater : vec4<f32>,
    barycentric : vec3<f32>,
    subdivisionDepth : f32,
    adaptiveGeometry : u32) -> OverviewVertexOutput {
    let direction = normalize(directionElevation.xyz);
    let sourceRadius = max(uniforms.cameraUpSourceRadius.w, 1.0);
    let elevationNormalized = directionElevation.w / sourceRadius;
    let radius = 1.0 + elevationNormalized * uniforms.cameraRightRelief.w;
    let planetPosition = direction * radius;
    let cameraDirection = normalize(uniforms.cameraDirectionDistance.xyz);
    let cameraPosition = cameraDirection * uniforms.cameraDirectionDistance.w;
    let cameraForward = -cameraDirection;
    let cameraRight = normalize(uniforms.cameraRightRelief.xyz);
    let cameraUp = normalize(uniforms.cameraUpSourceRadius.xyz);
    let relative = planetPosition - cameraPosition;
    let view = vec3<f32>(
        dot(relative, cameraRight), -dot(relative, cameraUp),
        dot(relative, cameraForward));
    let aspect = uniforms.viewportFocal.x / max(uniforms.viewportFocal.y, 1.0);
    var output : OverviewVertexOutput;
    output.position = vec4<f32>(
        view.x * uniforms.viewportFocal.z / aspect,
        view.y * uniforms.viewportFocal.z,
        uniforms.viewportFocal.w,
        view.z);
    output.planetPosition = planetPosition;
    output.radialNormal = direction;
    output.baseColorForest = baseColorForest;
    output.soilColorMountain = soilColorMountain;
    output.climateRiverWater = climateRiverWater;
    output.elevationNormalized = elevationNormalized;
    output.barycentric = barycentric;
    output.subdivisionDepth = subdivisionDepth;
    output.adaptiveGeometry = adaptiveGeometry;
    return output;
}

@vertex
fn drawOverviewVertex(
    @location(0) directionElevation : vec4<f32>,
    @location(1) baseColorForest : vec4<f32>,
    @location(2) soilColorMountain : vec4<f32>,
    @location(3) climateRiverWater : vec4<f32>) -> OverviewVertexOutput {
    var output = overviewOutput(directionElevation, baseColorForest,
        soilColorMountain, climateRiverWater, vec3<f32>(0.0), 0.0, 0u);
    if (uniforms.renderControls.x != 0u) {
        output.position = vec4<f32>(2.0, 2.0, 0.0, 1.0);
    }
    return output;
}

fn adaptiveDirection(
    slot : u32, corner : u32, renderChild : u32,
    renderDepth : u32) -> vec3<f32> {
    let bisector = cbtBisectors[slot];
    let root = cbtRoots[min(bisector.rootHalfedge,
        arrayLength(&cbtRoots) - 1u)];
    var triangle = array<vec3<f32>, 3>(
        root.vertex0.xyz, root.vertex1.xyz, root.vertex2.xyz);
    for (var level = 0u; level < bisector.depth; level += 1u) {
        let child = planetCbtPathBit(bisector.pathLow, bisector.pathHigh,
            bisector.depth - level - 1u);
        let current = triangle;
        triangle = array<vec3<f32>, 3>(
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 0u),
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 1u),
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 2u));
    }
    // Instance IDs enumerate every binary descendant of the selected CBT
    // patch. This is render-only tessellation: topology selection and memory
    // stay compact while curvature and height displacement reach pixel scale.
    for (var level = 0u; level < renderDepth; level += 1u) {
        let child = (renderChild >> (renderDepth - level - 1u)) & 1u;
        let current = triangle;
        triangle = array<vec3<f32>, 3>(
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 0u),
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 1u),
            planetCbtSubdivideVertex(current[0], current[1], current[2], child, 2u));
    }
    return normalize(triangle[corner]);
}

fn overview_water_material(materialId : f32) -> bool {
    return abs(materialId - 0.0) < 0.5 ||
        abs(materialId - 1.0) < 0.5 ||
        abs(materialId - 6.0) < 0.5 ||
        abs(materialId - 8.0) < 0.5;
}

fn overview_material_traits(materialId : f32) -> vec3<f32> {
    let vegetation = select(0.0, 1.0,
        abs(materialId - 3.0) < 0.5 ||
        (materialId >= 7.0 && materialId <= 9.5) ||
        (materialId >= 12.0 && materialId <= 19.5 &&
            abs(materialId - 14.0) > 0.5));
    let sand = select(0.0, 1.0,
        abs(materialId - 2.0) < 0.5 ||
        abs(materialId - 14.0) < 0.5 ||
        abs(materialId - 20.0) < 0.5);
    let geological = select(0.0, 1.0,
        abs(materialId - 4.0) < 0.5 ||
        abs(materialId - 10.0) < 0.5 ||
        abs(materialId - 11.0) < 0.5);
    return vec3<f32>(vegetation, sand, geological);
}

fn overview_cell_hash(cell : vec3<i32>) -> f32 {
    var value = bitcast<u32>(cell.x) * 0x8da6b343u;
    value = value ^ (bitcast<u32>(cell.y) * 0xd8163841u);
    value = value ^ (bitcast<u32>(cell.z) * 0xcb1ab31fu);
    value = value ^ (value >> 13u);
    value = value * 0x85ebca6bu;
    value = value ^ (value >> 16u);
    return f32(value & 0x00ffffffu) / 16777215.0;
}

fn overview_value_noise(position : vec3<f32>) -> f32 {
    let base = vec3<i32>(floor(position));
    let fraction = fract(position);
    let blend = fraction * fraction * (vec3<f32>(3.0) - 2.0 * fraction);
    let x00 = mix(overview_cell_hash(base),
        overview_cell_hash(base + vec3<i32>(1, 0, 0)), blend.x);
    let x10 = mix(overview_cell_hash(base + vec3<i32>(0, 1, 0)),
        overview_cell_hash(base + vec3<i32>(1, 1, 0)), blend.x);
    let x01 = mix(overview_cell_hash(base + vec3<i32>(0, 0, 1)),
        overview_cell_hash(base + vec3<i32>(1, 0, 1)), blend.x);
    let x11 = mix(overview_cell_hash(base + vec3<i32>(0, 1, 1)),
        overview_cell_hash(base + vec3<i32>(1, 1, 1)), blend.x);
    return mix(mix(x00, x10, blend.y), mix(x01, x11, blend.y), blend.z);
}

// Select adjacent power-of-two wavelengths and cross-fade between them. This
// preserves planet-space detail without shimmer as the globe zoom changes and
// avoids baking metre-scale voxel noise into a kilometre-scale atlas texel.
fn overview_filtered_noise(
    surfaceMetres : vec3<f32>, targetWavelength : f32,
    seedOffset : vec3<f32>) -> f32 {
    let logarithmic = clamp(log2(max(targetWavelength, 4.0)), 2.0, 23.0);
    let wavelength = exp2(floor(logarithmic));
    let position = surfaceMetres + seedOffset;
    let fine = overview_value_noise(position / wavelength);
    let coarse = overview_value_noise(position / (wavelength * 2.0));
    let blend = smoothstep(0.0, 1.0, fract(logarithmic));
    return mix(fine, coarse, blend);
}

// Convert a sphere direction to a normalized coordinate inside the packed
// 3x2 cube atlas. Clamping to the centre of each face's edge texels lets the
// linear sampler smooth within a face without bleeding from an unrelated
// neighbouring tile in the packed texture.
fn overview_surface_atlas_uv(
    directionInput : vec3<f32>, mip : u32) -> vec2<f32> {
    let direction = normalize(directionInput);
    let absolute = abs(direction);
    var face = 0u;
    var u = 0.0;
    var v = 0.0;
    if (absolute.x >= absolute.y && absolute.x >= absolute.z) {
        if (direction.x >= 0.0) {
            face = 0u;
            u = -direction.z / absolute.x;
        } else {
            face = 1u;
            u = direction.z / absolute.x;
        }
        v = -direction.y / absolute.x;
    } else if (absolute.y >= absolute.z) {
        if (direction.y >= 0.0) {
            face = 2u;
            u = direction.x / absolute.y;
            v = direction.z / absolute.y;
        } else {
            face = 3u;
            u = direction.x / absolute.y;
            v = -direction.z / absolute.y;
        }
    } else {
        if (direction.z >= 0.0) {
            face = 4u;
            u = direction.x / absolute.z;
        } else {
            face = 5u;
            u = -direction.x / absolute.z;
        }
        v = -direction.y / absolute.z;
    }
    let side = max(u32(uniforms.surfaceMapInfo.x) >> mip, 1u);
    let halfTexel = 0.5 / f32(side);
    let localUV = clamp(
        vec2<f32>(u, v) * 0.5 + vec2<f32>(0.5),
        vec2<f32>(halfTexel), vec2<f32>(1.0 - halfTexel));
    let faceOffset = vec2<f32>(f32(face % 3u), f32(face / 3u));
    return (faceOffset + localUV) / vec2<f32>(3.0, 2.0);
}

fn overview_linear_albedo(
    direction : vec3<f32>, mip : u32) -> vec4<f32> {
    return textureSampleLevel(surfaceAlbedoRoughness,
        surfaceLinearSampler, overview_surface_atlas_uv(direction, mip),
        f32(mip));
}

fn overview_linear_height(direction : vec3<f32>, mip : u32) -> f32 {
    return textureSampleLevel(surfaceHeight,
        surfaceLinearSampler, overview_surface_atlas_uv(direction, mip),
        f32(mip)).x;
}

fn overview_linear_features(
    direction : vec3<f32>, mip : u32) -> vec4<f32> {
    return textureSampleLevel(surfaceTerrainFeatures,
        surfaceLinearSampler, overview_surface_atlas_uv(direction, mip),
        f32(mip));
}

// Reconstruct the normal from the actual package height field in a spherical
// tangent frame. Unlike screen derivatives this remains stable while adaptive
// triangles split/merge and continues to reveal ranges whose displacement is
// sub-pixel in a whole-globe silhouette.
fn overview_height_normal(directionInput : vec3<f32>, mip : u32) -> vec3<f32> {
    let direction = normalize(directionInput);
    let reference = select(vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(1.0, 0.0, 0.0), abs(direction.y) > 0.92);
    let tangent = normalize(cross(reference, direction));
    let bitangent = cross(direction, tangent);
    let faceSide = f32(max(u32(uniforms.surfaceMapInfo.x) >> mip, 1u));
    let angularStep = 1.15 / faceSide;
    let tangentLow = normalize(direction - tangent * angularStep);
    let tangentHigh = normalize(direction + tangent * angularStep);
    let bitangentLow = normalize(direction - bitangent * angularStep);
    let bitangentHigh = normalize(direction + bitangent * angularStep);
    let heightScale = uniforms.cameraRightRelief.w;
    let tangentGradient = (
        overview_linear_height(tangentHigh, mip) -
        overview_linear_height(tangentLow, mip)) *
        uniforms.surfaceMapInfo.w * heightScale / (2.0 * angularStep);
    let bitangentGradient = (
        overview_linear_height(bitangentHigh, mip) -
        overview_linear_height(bitangentLow, mip)) *
        uniforms.surfaceMapInfo.w * heightScale / (2.0 * angularStep);
    return normalize(direction - tangent * tangentGradient -
        bitangent * bitangentGradient);
}

struct OverviewBiomeSample {
    albedo : vec4<f32>,
    traits : vec3<f32>,
}

// The material ID stays nearest-sampled for exact land/water ownership, while
// albedo and visual material traits form a broad, spherical ecotone. The
// radius grows with the projected atlas footprint: a full globe has natural
// regional transitions, but a close orbit converges toward local voxel colors.
fn overview_blended_biome(
    direction : vec3<f32>, mip : u32, centreFeatures : vec4<f32>,
    radiusTexels : f32) -> OverviewBiomeSample {
    let centre = overview_linear_albedo(direction, mip);
    let centreMaterial = round(centreFeatures.x * 255.0);
    let centreWater = overview_water_material(centreMaterial);
    let reference = select(vec3<f32>(0.0, 1.0, 0.0),
        vec3<f32>(1.0, 0.0, 0.0), abs(direction.y) > 0.92);
    let tangent = normalize(cross(reference, direction));
    let bitangent = cross(direction, tangent);
    let faceSide = f32(max(
        u32(uniforms.surfaceMapInfo.x) >> mip, 1u));
    // Rotate and stretch the ecotone kernel with a continuous planet-space
    // field. A categorical source block therefore dissolves into an irregular
    // transition region instead of retaining its square raster silhouette.
    let kernelNoise = overview_value_noise(
        direction * 23.0 + vec3<f32>(7.3, -13.7, 29.1));
    let kernelAngle = kernelNoise * 6.28318530718;
    let kernelCos = cos(kernelAngle);
    let kernelSin = sin(kernelAngle);
    let angularStep = 1.30 / faceSide * radiusTexels *
        mix(0.76, 1.34, overview_value_noise(
            direction * 41.0 + vec3<f32>(-19.0, 31.0, 11.0)));
    let offsets = array<vec2<f32>, 12>(
        vec2<f32>(0.25, 0.0), vec2<f32>(0.0, 0.25),
        vec2<f32>(-0.25, 0.0), vec2<f32>(0.0, -0.25),
        vec2<f32>(0.4243, 0.4243), vec2<f32>(-0.4243, 0.4243),
        vec2<f32>(-0.4243, -0.4243), vec2<f32>(0.4243, -0.4243),
        vec2<f32>(0.9239, 0.3827), vec2<f32>(-0.3827, 0.9239),
        vec2<f32>(-0.9239, -0.3827), vec2<f32>(0.3827, -0.9239));
    let weights = array<f32, 12>(
        0.42, 0.42, 0.42, 0.42,
        0.25, 0.25, 0.25, 0.25,
        0.12, 0.12, 0.12, 0.12);
    var albedo = centre;
    var traits = overview_material_traits(centreMaterial);
    var totalWeight = 1.0;
    for (var index = 0u; index < 12u; index += 1u) {
        let sourceOffset = offsets[index];
        let offset = vec2<f32>(
            sourceOffset.x * kernelCos - sourceOffset.y * kernelSin,
            sourceOffset.x * kernelSin + sourceOffset.y * kernelCos);
        let tapDirection = normalize(direction +
            (tangent * offset.x + bitangent * offset.y) * angularStep);
        let tapTexel = surfaceAtlasTexel(tapDirection, mip);
        let tapFeatures = textureLoad(surfaceTerrainFeatures,
            tapTexel, i32(mip));
        let tapMaterial = round(tapFeatures.x * 255.0);
        let sameDomain = overview_water_material(tapMaterial) == centreWater;
        let weight = select(0.0, weights[index], sameDomain);
        albedo += overview_linear_albedo(tapDirection, mip) * weight;
        traits += overview_material_traits(tapMaterial) * weight;
        totalWeight += weight;
    }
    return OverviewBiomeSample(albedo / totalWeight, traits / totalWeight);
}

@vertex
fn drawAdaptiveOverviewVertex(
    @builtin(vertex_index) vertexIndex : u32,
    @builtin(instance_index) instanceIndex : u32) -> OverviewVertexOutput {
    let triangleIndex = vertexIndex / 3u;
    let corner = vertexIndex % 3u;
    let pointerCount = arrayLength(&cbtPointers);
    let poolCount = arrayLength(&cbtBisectors);
    let rootCount = arrayLength(&cbtRoots);
    let pointerInRange = triangleIndex < pointerCount;
    let pointerIndex = min(triangleIndex, max(pointerCount, 1u) - 1u);
    let requestedSlot = cbtPointers[pointerIndex].activePoolSlot;
    let slotInRange = pointerInRange &&
        requestedSlot != PLANET_CBT_NULL_SLOT && requestedSlot < poolCount;
    let slot = select(0u, requestedSlot, slotInRange);
    let bisector = cbtBisectors[slot];
    let validBisector = slotInRange &&
        bisector.rootHalfedge != PLANET_CBT_NULL_SLOT &&
        bisector.rootHalfedge < rootCount &&
        bisector.depth <= PLANET_CBT_RENDER_TARGET_DEPTH;
    let patchDepth = min(bisector.depth, PLANET_CBT_RENDER_TARGET_DEPTH);
    let requestedRenderDepth = min(uniforms.renderControls.w,
        PLANET_CBT_MAXIMUM_RENDER_SUBDIVISION_DEPTH);
    let remainingTargetDepth = select(0u,
        PLANET_CBT_RENDER_TARGET_DEPTH - patchDepth,
        patchDepth < PLANET_CBT_RENDER_TARGET_DEPTH);
    let renderDepth = min(requestedRenderDepth, remainingTargetDepth);
    let renderChildCount = 1u << renderDepth;
    let validRenderChild = instanceIndex < renderChildCount;
    var barycentric = vec3<f32>(0.0);
    barycentric[corner] = 1.0;
    let direction = adaptiveDirection(
        slot, corner, min(instanceIndex, renderChildCount - 1u),
        renderDepth);
    let effectiveDepth = patchDepth + renderDepth;
    let mip = planetCbtTextureMip(effectiveDepth,
        max(u32(uniforms.surfaceMapInfo.y), 1u));
    let heightNormalized = overview_linear_height(direction, mip);
    let heightMetres = (uniforms.surfaceMapInfo.z +
        heightNormalized * uniforms.surfaceMapInfo.w) *
        max(uniforms.cameraUpSourceRadius.w, 1.0);
    var output = overviewOutput(vec4<f32>(direction, heightMetres),
        vec4<f32>(0.36, 0.48, 0.28, 0.0),
        vec4<f32>(0.48, 0.45, 0.40, 0.0),
        vec4<f32>(0.5, 0.5, 0.0, 0.0), barycentric,
        f32(effectiveDepth), 1u);
    if (uniforms.renderControls.x != 1u || !validRenderChild ||
        !validBisector) {
        output.position = vec4<f32>(2.0, 2.0, 0.0, 1.0);
    }
    return output;
}

@fragment
fn drawOverviewFragment(input : OverviewVertexOutput) -> @location(0) vec4<f32> {
    if (input.adaptiveGeometry != uniforms.renderControls.x) {
        discard;
    }
    let radial = normalize(input.radialNormal);
    let sourceRadius = max(uniforms.cameraUpSourceRadius.w, 1.0);
    let surfaceMetres = radial * sourceRadius;
    let metresPerPixel = max(length(dpdx(surfaceMetres)),
        length(dpdy(surfaceMetres)));
    let materialMip = select(0u,
        planetCbtTextureMip(u32(round(input.subdivisionDepth)),
            max(u32(uniforms.surfaceMapInfo.y), 1u)),
        input.adaptiveGeometry == 1u);
    var surfaceAlbedo = vec4<f32>(input.baseColorForest.rgb, 0.82);
    var surfaceFeatures = vec4<f32>(0.0);
    var smoothSurfaceFeatures = vec4<f32>(0.0);
    var biomeTraits = vec3<f32>(0.0);
    if (uniforms.surfaceMapInfo.x > 0.5) {
        let atlasTexel = surfaceAtlasTexel(radial, materialMip);
        surfaceFeatures = textureLoad(surfaceTerrainFeatures, atlasTexel,
            i32(materialMip));
        smoothSurfaceFeatures = overview_linear_features(radial, materialMip);
        let faceSide = max(
            u32(uniforms.surfaceMapInfo.x) >> materialMip, 1u);
        let atlasTexelMetres = sourceRadius * 1.30 / f32(faceSide);
        let projectedAtlasFootprint = metresPerPixel /
            max(atlasTexelMetres, 1.0);
        let biomeRadius = mix(2.75, 9.5, smoothstep(
            0.12, 1.35, projectedAtlasFootprint));
        let biome = overview_blended_biome(
            radial, materialMip, surfaceFeatures, biomeRadius);
        surfaceAlbedo = biome.albedo;
        biomeTraits = biome.traits;
    }
    let materialId = round(surfaceFeatures.x * 255.0);
    // Mesh elevation owns the silhouette; explicit spherical height samples
    // own the lighting normal so a CBT topology update cannot create a
    // triangle-shaped normal discontinuity on the following settled frame.
    let mappedRelief = overview_height_normal(radial, materialMip);
    let relief = select(radial,
        select(mappedRelief, radial, overview_water_material(materialId)),
        uniforms.surfaceMapInfo.x > 0.5);
    // Authored masks are often smooth at whole-globe mesh density. Shape them
    // into readable regional features without introducing procedural detail
    // that would disagree with the package.
    let mountainSource = clamp(select(input.soilColorMountain.w,
        max(smoothSurfaceFeatures.y, input.soilColorMountain.w * 0.42),
        uniforms.surfaceMapInfo.x > 0.5), 0.0, 1.0);
    let mountain = smoothstep(0.08, 0.82, mountainSource);
    let heightSlope = clamp(select(surfaceFeatures.w,
        smoothSurfaceFeatures.w, uniforms.surfaceMapInfo.x > 0.5), 0.0, 1.0);
    let readableSlope = smoothstep(0.012, 0.30, heightSlope);
    let normal = normalize(mix(radial, relief,
        0.46 + mountain * 0.38 + readableSlope * 0.12));

    let textured = uniforms.surfaceMapInfo.x > 0.5;
    let textureTemperature = clamp(
        0.70 - abs(radial.y) * 0.55 - mountain * 0.16, 0.0, 1.0);
    let textureRainfall = clamp(
        biomeTraits.x * 0.72 + (1.0 - biomeTraits.y) * 0.18,
        0.0, 1.0);
    let temperature = clamp(select(input.climateRiverWater.x,
        textureTemperature, textured), 0.0, 1.0);
    let rainfall = clamp(select(input.climateRiverWater.y,
        textureRainfall, textured), 0.0, 1.0);
    let dryWarm = smoothstep(0.42, 0.82, temperature) *
        (1.0 - smoothstep(0.18, 0.62, rainfall));
    let cold = 1.0 - smoothstep(0.12, 0.46, temperature);
    var climateTint = mix(
        vec3<f32>(0.82, 0.91, 1.06),
        vec3<f32>(1.10, 0.94, 0.72), dryWarm);
    climateTint = mix(climateTint, vec3<f32>(0.78, 0.88, 1.08), cold);
    let forestSource = select(input.baseColorForest.w,
        biomeTraits.x, textured);
    let forest = smoothstep(0.10, 0.78,
        clamp(forestSource * uniforms.featureStrengths.x, 0.0, 1.0));
    // When the atlas exists it is the material source, not a tint over vertex
    // colors. This removes triangle-shaped climate/biome patches from both the
    // static and adaptive overview meshes.
    let authoredBase = select(input.baseColorForest.rgb,
        surfaceAlbedo.rgb, textured);
    var color = mix(
        authoredBase * mix(vec3<f32>(1.0), climateTint, 0.16),
        authoredBase * vec3<f32>(0.38, 0.68, 0.42), forest);
    let desert = biomeTraits.y;
    let geological = biomeTraits.z;
    let duneLight = 0.96 + 0.08 * smoothstep(-0.35, 0.45,
        dot(normal, normalize(-uniforms.sunDirectionIntensity.xyz)));
    color *= mix(1.0, duneLight, desert * 0.55);
    color *= mix(1.0, 0.94 + 0.09 * sin(
        dot(radial, vec3<f32>(71.0, 113.0, 157.0))),
        geological * 0.32);
    let fallbackRock = mix(input.soilColorMountain.rgb,
        vec3<f32>(0.55, 0.54, 0.52), 0.32);
    let texturedRock = mix(surfaceAlbedo.rgb,
        vec3<f32>(0.48, 0.47, 0.45), 0.58);
    let rock = select(fallbackRock, texturedRock, textured);
    color = mix(color, rock,
        clamp(mountain * uniforms.featureStrengths.y * 0.72, 0.0, 1.0));
    let snow = smoothstep(0.68, 0.94, mountain) *
        smoothstep(0.5, 0.0, temperature);
    color = mix(color, vec3<f32>(0.78, 0.84, 0.86), snow * 0.80);
    // Band-limited spherical noise supplies high-frequency material variation
    // at the scale the current view can resolve. It stays continuous across
    // cube faces and replaces the old hard cell hash that read as a low-res
    // grid when the planet stopped moving.
    let valueNoise = overview_filtered_noise(surfaceMetres,
        max(metresPerPixel * 2.2, 160.0),
        vec3<f32>(17341.0, 8191.0, 29567.0)) - 0.5;
    let hueNoise = overview_filtered_noise(surfaceMetres,
        max(metresPerPixel * 5.5, 720.0),
        vec3<f32>(-43721.0, 23549.0, 11939.0)) - 0.5;
    let grainCellSize = exp2(floor(log2(max(metresPerPixel * 1.15, 4.0))));
    let grainCell = vec3<i32>(floor(surfaceMetres / grainCellSize));
    let grain = overview_cell_hash(grainCell) - 0.5;
    let vegetation = biomeTraits.x;
    let sand = biomeTraits.y;
    var voxelTint = vec3<f32>(1.0 + valueNoise * 0.14 + grain * 0.025);
    voxelTint *= mix(vec3<f32>(1.0), vec3<f32>(
        1.0 + hueNoise * 0.03,
        1.0 + hueNoise * 0.09,
        1.0 - hueNoise * 0.04), vegetation);
    voxelTint *= mix(vec3<f32>(1.0), vec3<f32>(
        1.0 + hueNoise * 0.07,
        1.0 + hueNoise * 0.025,
        1.0 - hueNoise * 0.06), sand);
    voxelTint *= mix(vec3<f32>(1.0), vec3<f32>(
        1.0 + hueNoise * 0.04,
        1.0 + hueNoise * 0.01,
        1.0 - hueNoise * 0.03), geological);
    let materialIsWater = overview_water_material(materialId);
    color *= mix(vec3<f32>(1.0), voxelTint,
        select(0.72, 0.12, materialIsWater));
    let ridgeNoise = pow(clamp(1.0 - abs(
        overview_filtered_noise(surfaceMetres,
            max(metresPerPixel * 7.0, 1800.0),
            vec3<f32>(6113.0, -3571.0, 49727.0)) * 2.0 - 1.0),
        0.0, 1.0), 3.0);
    color = mix(color, color * 0.74 + rock * 0.26,
        ridgeNoise * mountain * 0.24);

    // Prefer the raster drainage mask over interpolated graph vertices. A
    // high threshold contracts the bilinear footprint to a narrow centreline;
    // minor tributaries become sub-pixel instead of continent-scale blue
    // strokes, while high-discharge rivers remain visible.
    let riverSource = select(input.climateRiverWater.z,
        smoothSurfaceFeatures.z, uniforms.surfaceMapInfo.x > 0.5);
    let river = smoothstep(0.46, 0.84,
        clamp(riverSource * uniforms.featureStrengths.z, 0.0, 1.0));
    color = mix(color, vec3<f32>(0.014, 0.16, 0.31), river * 0.64);

    let cameraPosition = normalize(uniforms.cameraDirectionDistance.xyz) *
        uniforms.cameraDirectionDistance.w;
    let view = normalize(cameraPosition - input.planetPosition);
    let sun = normalize(-uniforms.sunDirectionIntensity.xyz);
    let diffuse = max(dot(normal, sun), 0.0);
    let hemisphere = 0.38 + 0.18 * max(normal.y, 0.0);
    // Height-map slope acts as a restrained ambient-occlusion proxy. It is
    // deliberately applied before water shading and strongest in mountains,
    // approximating the shadow density of the voxel terrain without baking a
    // fixed sun direction into the package.
    color *= mix(1.0, 0.87,
        readableSlope * mix(0.50, 1.0, mountain));
    color *= hemisphere + diffuse * uniforms.sunDirectionIntensity.w *
        uniforms.sunColorAtmosphere.rgb * 0.72;

    // The texture material ID, rather than interpolated graph vertices, owns
    // the coastline. This keeps the orbit view and destination sampler on the
    // same land/water classification even when the topology mesh is coarse.
    let materialWater = select(0.0, 1.0,
        overview_water_material(materialId));
    let water = clamp(select(input.climateRiverWater.w, materialWater,
        uniforms.surfaceMapInfo.x > 0.5), 0.0, 1.0);
    if (water > 0.001) {
        let halfway = normalize(sun + view);
        let roughness = clamp(surfaceAlbedo.a, 0.04, 1.0);
        let specularPower = mix(180.0, 10.0, roughness * roughness);
        let specular = pow(max(dot(radial, halfway), 0.0), specularPower) *
            mix(0.95, 0.18, roughness);
        let fresnel = pow(1.0 - max(dot(radial, view), 0.0), 3.0);
        let ocean = vec3<f32>(0.018, 0.11, 0.24) * (0.72 + diffuse * 0.34) +
            vec3<f32>(0.22, 0.45, 0.68) * (specular * 0.55 + fresnel * 0.22);
        color = mix(color, ocean, water * uniforms.featureStrengths.w);
    }

    if (uniforms.markerDirectionEnabled.w > 0.5) {
        let markerDirection = normalize(uniforms.markerDirectionEnabled.xyz);
        let chordDistance = sqrt(max(0.0, 2.0 * (1.0 - dot(radial, markerDirection))));
        let markerRadius = max(uniforms.markerColorRadius.w, 0.001);
        let outer = 1.0 - smoothstep(markerRadius, markerRadius * 1.28, chordDistance);
        let inner = 1.0 - smoothstep(markerRadius * 0.52, markerRadius * 0.76, chordDistance);
        color = mix(color, vec3<f32>(0.025), outer * 0.84);
        color = mix(color, uniforms.markerColorRadius.rgb, inner);
    }
    if (input.adaptiveGeometry == 1u && uniforms.renderControls.y == 1u) {
        let edge = min(input.barycentric.x,
            min(input.barycentric.y, input.barycentric.z));
        let line = 1.0 - smoothstep(0.0, max(fwidth(edge) * 1.35, 0.0001), edge);
        color = mix(color * 0.32, vec3<f32>(0.20, 0.92, 1.0), line);
    } else if (input.adaptiveGeometry == 1u && uniforms.renderControls.y == 2u) {
        let depthRange = max(f32(uniforms.renderControls.z), 1.0);
        let t = clamp(input.subdivisionDepth / depthRange, 0.0, 1.0);
        color = mix(vec3<f32>(0.05, 0.18, 0.85),
            vec3<f32>(1.0, 0.12, 0.03), t);
    }
    return vec4<f32>(max(color, vec3<f32>(0.0)), 1.0);
}
