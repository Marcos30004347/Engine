#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(2) var<storage, read> materials : array<SparseVoxelMaterial>;
@group(0) @binding(3) var<storage, read> resolvedSurfaces : array<ResolvedVoxelSurface>;
@group(0) @binding(4) var baseColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(5) var materialPropertiesTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(6) var emissiveTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var normalTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var<storage, read> materialSampleAddresses :
    array<VoxelMaterialSampleAddress>;

// Authoring overrides for the two scalars an OBJ almost never carries usefully.
//
// MTL has no reliable roughness or metallic: most exporters omit them entirely
// and the ones that write them disagree about the range. The imported values
// are therefore usually a constant guess, and being able to sweep them without
// re-voxelising is what makes the response of the lighting model inspectable at
// all.
//
// x roughness value, y roughness blend, z metallic value, w metallic blend. A
// blend rather than a flag so the control degrades continuously: zero keeps the
// authored value, one replaces it, and anything between shows how far the
// authored material is from the override.
struct SparseVoxelMaterialOverride {
    roughnessMetallic : vec4<f32>,
    // x filter strength, y depth sigma in projected pixel footprints,
    // z variance-to-roughness scale, w reserved.
    normalFilter : vec4<f32>,
    // x enabled, y start distance, z end distance, w normal strength.
    distantFilter : vec4<f32>,
    // x albedo strength, y maximum pixel radius, z plane sigma in projected
    // pixel footprints, w local-detail variance threshold.
    distantFilterDetail : vec4<f32>,
    // x enabled, y terrain/micro-voxel hue variation, z virtual-step normal strength,
    // w maximum inward recess in world metres.
    microDetail : vec4<f32>,
    // x finest world-space voxel size, y hard micro-face shadow strength. A finest
    // face is split 2 by 3; coarser raster LOD faces naturally expose more of
    // the same stable micro grid.
    microGrid : vec4<f32>,
}

@group(0) @binding(8) var<uniform> materialOverride : SparseVoxelMaterialOverride;
@group(0) @binding(9) var<storage, read> microMaterials : array<SparseVoxelMicroMaterial>;
@group(0) @binding(10) var<storage, read> microPayloads : array<u32>;

struct VoxelShadingProgram {
    instructionOffset : u32,
    instructionCount : u32,
    flags : u32,
    reserved : u32,
}

struct VoxelShadingInstruction {
    opcodeOperands : u32,
    immediate0 : f32,
    immediate1 : f32,
    reserved : u32,
}

@group(0) @binding(11) var<storage, read> voxelShadingPrograms : array<VoxelShadingProgram>;
@group(0) @binding(12) var<storage, read> voxelShadingInstructions : array<VoxelShadingInstruction>;
@group(0) @binding(13) var<storage, read> voxelShadingParameters : array<vec4<f32>>;

struct VoxelMaterialFieldDefaults {
    values : vec4<u32>,
}

@group(0) @binding(14) var<storage, read> voxelMaterialFieldDefaults : array<VoxelMaterialFieldDefaults>;

struct VoxelShadingPalette {
    additionalAlbedoRGBA8 : vec2<u32>,
    reserved : vec2<u32>,
}

@group(0) @binding(19) var<storage, read> voxelShadingPalettes :
    array<VoxelShadingPalette>;

#include "voxel_material_field_atlas.wgsl"

#include "sparse_voxel_micro_geometry.wgsl"

fn applyMaterialOverride(roughness : f32, metallic : f32) -> vec2<f32> {
    return vec2<f32>(
        mix(roughness, materialOverride.roughnessMetallic.x,
            clamp(materialOverride.roughnessMetallic.y, 0.0, 1.0)),
        mix(metallic, materialOverride.roughnessMetallic.z,
            clamp(materialOverride.roughnessMetallic.w, 0.0, 1.0)));
}

// Keep authored emissive data in the material ABI, but disable its contribution
// in the sparse voxel renderer until emissive lighting is intentionally enabled.
const EMISSIVE_MATERIAL_SCALE : f32 = 0.0;

fn localFaceNormal(packedFace : u32) -> vec3<f32> {
    let axis = packedFace & 3u;
    let sign = select(-1.0, 1.0, (packedFace & 4u) != 0u);
    var normal = vec3<f32>(0.0);
    normal[axis] = sign;
    return normal;
}

fn transformNormal(worldToModel : mat4x4<f32>, localNormal : vec3<f32>) -> vec3<f32> {
    let transformed = vec3<f32>(
        dot(worldToModel[0].xyz, localNormal),
        dot(worldToModel[1].xyz, localNormal),
        dot(worldToModel[2].xyz, localNormal));
    return normalize(transformed);
}

struct FilteredVoxelNormal {
    normal : vec3<f32>,
    roughness : f32,
}

fn safeVoxelNormal(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let length2 = dot(value, value);
    return select(
        fallback, value * inverseSqrt(max(length2, 1.0e-12)),
        length2 > 1.0e-12);
}

#include "sparse_voxel_terrain_normal.wgsl"

// Filter only the material shading normal. The G-buffer starts from the same
// exact axis-aligned face normal, then applies the world-stable micro-height
// normal separately so geometry consumers see that intentional detail too.
//
// The kernel is bilateral in instance and ray depth. It therefore averages the
// rapidly alternating faces of sub-pixel voxels without pulling a background
// normal across an occluding contour. The length lost before renormalization is
// normal variance; converting part of it to roughness is the Toksvig-style
// energy-preserving response that prevents a tiny specular face from flashing.
fn filterVoxelNormal(
    pixel : vec2<u32>, centerSurface : ResolvedVoxelSurface,
    centerNormal : vec3<f32>, roughness : f32) -> FilteredVoxelNormal {
    if (sparseVoxelIsGeneratedSolidTerrain(centerSurface)) {
        return FilteredVoxelNormal(centerNormal, roughness);
    }
    let terrainNormal = sparseVoxelTerrainMacroNormal(
        pixel, centerSurface, centerNormal);
    if (sparseVoxelIsExactTerrainSurface(centerSurface)) {
        let variance = 1.0 - clamp(
            abs(dot(centerNormal, terrainNormal)), 0.0, 1.0);
        return FilteredVoxelNormal(
            terrainNormal,
            clamp(max(roughness, roughness + variance *
                max(materialOverride.normalFilter.z, 0.0)), 0.0, 1.0));
    }
    let dimensions = max(uniforms.viewportTiles.xy, vec2<u32>(1u));
    let strength = clamp(materialOverride.normalFilter.x, 0.0, 1.0);
    if (strength <= 0.0) {
        return FilteredVoxelNormal(centerNormal, roughness);
    }
    let depthSigma = max(
        abs(centerSurface.surfaceT) /
            max(abs(uniforms.focalPrincipal.x), 1.0e-3) *
            max(materialOverride.normalFilter.y, 0.25),
        1.0e-4);
    var accumulated = centerNormal * 2.0;
    var totalWeight = 2.0;
    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            if (x == 0 && y == 0) { continue; }
            let coordinate = vec2<i32>(pixel) + vec2<i32>(x, y);
            if (any(coordinate < vec2<i32>(0)) ||
                any(coordinate >= vec2<i32>(dimensions))) { continue; }
            let candidateIndex = u32(coordinate.y) * dimensions.x +
                u32(coordinate.x);
            if (candidateIndex >= arrayLength(&resolvedSurfaces)) { continue; }
            let candidate = resolvedSurfaces[candidateIndex];
            if ((candidate.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u ||
                candidate.instanceIndex != centerSurface.instanceIndex) {
                continue;
            }
            let candidateNormal = transformNormal(
                instances[candidate.instanceIndex].worldToModel,
                localFaceNormal(candidate.packedLocalFace));
            // Never average an opposite side of a thin surface into this one.
            if (dot(centerNormal, candidateNormal) < -0.05) { continue; }
            let depthWeight = 1.0 - smoothstep(
                0.0, depthSigma,
                abs(candidate.surfaceT - centerSurface.surfaceT));
            let spatialWeight = select(0.5, 0.75, x == 0 || y == 0);
            let weight = depthWeight * spatialWeight;
            accumulated += candidateNormal * weight;
            totalWeight += weight;
        }
    }
    let mean = accumulated / max(totalWeight, 1.0e-4);
    let variance = 1.0 - clamp(length(mean), 0.0, 1.0);
    let meanNormal = safeVoxelNormal(mean, centerNormal);
    let filtered = safeVoxelNormal(
        mix(centerNormal, meanNormal, strength), centerNormal);
    let filteredRoughness = clamp(
        max(roughness, roughness + variance *
            max(materialOverride.normalFilter.z, 0.0)),
        0.0, 1.0);
    return FilteredVoxelNormal(filtered, filteredRoughness);
}

fn srgbChannelToLinear(value : f32) -> f32 {
    return select(value / 12.92, pow((value + 0.055) / 1.055, 2.4), value > 0.04045);
}

fn decodeAlbedo(packed : u32) -> vec4<f32> {
    let scale = 1.0 / 255.0;
    let srgb = vec3<f32>(
        f32(packed & 0xffu),
        f32((packed >> 8u) & 0xffu),
        f32((packed >> 16u) & 0xffu)) * scale;
    return vec4<f32>(
        srgbChannelToLinear(srgb.x),
        srgbChannelToLinear(srgb.y),
        srgbChannelToLinear(srgb.z),
        f32((packed >> 24u) & 0xffu) * scale);
}

fn halfToFloat(packed : u32) -> f32 {
    let bits = packed & 0xffffu;
    let sign = bits >> 15u;
    let exponent = (bits >> 10u) & 0x1fu;
    let mantissa = bits & 0x3ffu;
    if (exponent == 0u) {
        if (mantissa == 0u) {
            return bitcast<f32>(sign << 31u);
        }
        let value = f32(mantissa) * exp2(-24.0);
        return select(value, -value, sign != 0u);
    }
    let floatExponent = select(exponent + 112u, 0xffu, exponent == 0x1fu);
    return bitcast<f32>((sign << 31u) | (floatExponent << 23u) | (mantissa << 13u));
}

fn surfaceAlbedo(surface : ResolvedVoxelSurface) -> vec3<f32> {
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_NO_MATERIAL) != 0u) {
        return vec3<f32>(0.5);
    }
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) != 0u) {
        return decodeAlbedo(surface.reserved0 | 0xff000000u).rgb;
    }
    if (surface.voxelIndex >= arrayLength(&materials)) {
        return vec3<f32>(0.5);
    }
    return decodeAlbedo(materials[surface.voxelIndex].albedoRGBA8).rgb;
}

fn surfaceWorldPosition(pixel : vec2<i32>, distance : f32) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) /
            max(uniforms.focalPrincipal.x, 1.0e-4),
        (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) /
            max(uniforms.focalPrincipal.y, 1.0e-4),
        1.0);
    let worldDirection = normalize(
        (uniforms.cameraToWorld * vec4<f32>(cameraDirection, 0.0)).xyz);
    return uniforms.cameraWorldPositionNear.xyz + worldDirection * distance;
}

fn microVoxelHash(coordinate : vec3<i32>, face : u32) -> u32 {
    var value = bitcast<u32>(coordinate.x) * 0x8da6b343u;
    value ^= bitcast<u32>(coordinate.y) * 0xd8163841u;
    value ^= bitcast<u32>(coordinate.z) * 0xcb1ab31fu;
    value ^= face * 0x9e3779b9u;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn microVoxelHeight(
    cell : vec2<i32>, plane : i32, face : u32) -> f32 {
    let hash = microVoxelHash(vec3<i32>(cell, plane), face);
    return f32(hash & 0xffffu) * (2.0 / 65535.0) - 1.0;
}

fn voxelColourNoise(hash : u32) -> vec3<f32> {
    return vec3<f32>(
        f32(hash & 0xffu),
        f32((hash >> 8u) & 0xffu),
        f32((hash >> 16u) & 0xffu)) * (2.0 / 255.0) - vec3<f32>(1.0);
}

// Perturb chroma around the authored colour while keeping the average
// luminance nearly unchanged. Unlike a scalar multiplier this produces an
// actual hue shift, and even a neutral fallback becomes subtly warm or cool.
fn applyVoxelHueVariation(
    albedo : vec3<f32>, noise : vec3<f32>, strength : f32) -> vec3<f32> {
    let lumaWeights = vec3<f32>(0.2126, 0.7152, 0.0722);
    let noiseLuma = dot(noise, lumaWeights);
    let chroma = noise - vec3<f32>(noiseLuma);
    let luminance = max(dot(albedo, lumaWeights), 0.04);
    let brightness = noiseLuma * 0.30;
    return max(
        albedo * (1.0 + brightness * strength) +
            chroma * luminance * strength * 0.80,
        vec3<f32>(0.0));
}

// Stable hue variation for the actual resolved terrain voxel. The exact brick
// identity is mixed with its world-space cell, so adjacent voxels sharing one
// palette entry no longer produce a perfectly flat field.
fn applyTerrainVoxelColour(
    pixel : vec2<u32>, surface : ResolvedVoxelSurface,
    sampleAddress : VoxelMaterialSampleAddress,
    albedo : vec3<f32>) -> vec3<f32> {
    // Solid-terrain colour is ordinary per-voxel material variation, not
    // microgeometry. Keep it active when the microgeometry feature is off.
    let enabled = (sparseVoxelIsGeneratedSolidTerrain(surface) ||
        materialOverride.microDetail.x > 0.5) &&
        (surface.reserved1 & SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) == 0u;
    let variation = clamp(materialOverride.microDetail.y, 0.0, 0.35);
    let voxelSize = max(materialOverride.microGrid.x, 0.0);
    if (!enabled || variation <= 0.0 || voxelSize <= 1.0e-6) {
        return albedo;
    }
    let worldPosition = surfaceWorldPosition(
        vec2<i32>(pixel), surface.surfaceT);
    var cell = vec3<i32>(floor(worldPosition / voxelSize));
    var salt = surface.packedLocalFace & 7u;
    if (sampleAddress.regionMappingId != 0u) {
        // The canonical parent coordinate identifies the resolved terrain
        // voxel itself.  Region IDs are page identities and face is a view of
        // the same voxel; including either made adjacent pages/faces use
        // visibly different colour distributions and recoloured retained
        // cells after an ownership rewrite.
        cell = sampleAddress.canonicalMicroCoordinate;
        salt = 0u;
    } else {
        cell.x ^= bitcast<i32>(surface.voxelIndex);
        cell.y ^= bitcast<i32>(surface.instanceIndex * 0x9e3779b9u);
        cell.z ^= bitcast<i32>(surface.voxelIndex * 0x85ebca6bu);
    }
    let hash = microVoxelHash(cell, salt);
    return applyVoxelHueVariation(
        albedo, voxelColourNoise(hash), variation * 0.65);
}

// Apply scalar contrast after every authored/material-program colour decision,
// so even exact terrain programs remain visibly divided into individual cells.
// The canonical address is common to the raster and Tree64 resolvers and is
// independent of camera-relative rebasing.
fn applyPerVoxelBrightness(
    pixel : vec2<u32>, surface : ResolvedVoxelSurface,
    sampleAddress : VoxelMaterialSampleAddress,
    albedo : vec3<f32>) -> vec3<f32> {
    let variation = clamp(materialOverride.microGrid.z, 0.0, 0.35);
    let voxelSize = max(materialOverride.microGrid.x, 0.0);
    if (variation <= 0.0 || voxelSize <= 1.0e-6) {
        return albedo;
    }
    let exactMicro =
        (surface.reserved1 & SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) != 0u;
    let worldPosition = surfaceWorldPosition(
        vec2<i32>(pixel), surface.surfaceT);
    var cell = vec3<i32>(floor(worldPosition / voxelSize));
    var salt = surface.instanceIndex;
    if (sampleAddress.regionMappingId != 0u) {
        // Terrain surface identity is intentionally a finest-grid XZ column
        // so temporal lighting survives a page refinement.  It therefore
        // varies many times across one coarse voxel face and must not be part
        // of the albedo hash.  The canonical material address, in contrast,
        // is the lower corner of the actually resolved voxel and is constant
        // for every pixel and every raster path that sees that voxel.
        cell = sampleAddress.canonicalMicroCoordinate;
        salt = 0u;
    } else if (exactMicro) {
        cell = sampleAddress.canonicalMicroCoordinate;
        salt = sampleAddress.regionMappingId;
    }
    // Include the resolved identity because it represents the cell actually
    // visible at the selected LOD. A finest-grid world-position hash becomes
    // sub-pixel noise on a coarse face and reconstruction averages it away.
    // Four bands produce readable cell boundaries without becoming a checker.
    var identityCell = cell ^ vec3<i32>(
        bitcast<i32>(surface.voxelIndex),
        bitcast<i32>(surface.voxelIndex * 0x9e3779b9u),
        bitcast<i32>(surface.voxelIndex * 0x85ebca6bu));
    if (sampleAddress.regionMappingId != 0u) {
        identityCell = cell;
    }
    let hash = microVoxelHash(identityCell, salt);
    let brightnessBand = f32((hash >> 8u) & 3u) / 3.0;
    let brightnessNoise = brightnessBand * 2.0 - 1.0;
    return clamp(
        albedo * (1.0 + brightnessNoise * variation),
        vec3<f32>(0.0), vec3<f32>(1.0));
}

struct MicroVoxelSurface {
    albedo : vec3<f32>,
    normal : vec3<f32>,
}

struct VoxelColourLayers {
    bottom : vec3<f32>,
    faceModes : u32,
    enabled : bool,
}

const VOXEL_SHADING_MAX_INSTRUCTIONS : u32 = 16u;
const VOXEL_SHADING_PROGRAM_PALETTE_MASK : u32 = 0x30u;
const VOXEL_SHADING_PROGRAM_MATERIAL_FIELD_MASK : u32 = 0xf00u;

fn voxelShadingDefaultMaterialFields(materialHandle : u32) -> vec4<f32> {
    if (materialHandle >= arrayLength(&voxelShadingPrograms) ||
        materialHandle >= arrayLength(&voxelMaterialFieldDefaults) ||
        (voxelShadingPrograms[materialHandle].flags &
            VOXEL_SHADING_PROGRAM_MATERIAL_FIELD_MASK) == 0u) {
        return vec4<f32>(0.0);
    }
    return vec4<f32>(min(
        voxelMaterialFieldDefaults[materialHandle].values,
        vec4<u32>(65536u))) / 65536.0;
}

struct VoxelShadingResult {
    layerWeight : f32,
    hueVariation : f32,
    brightness : f32,
    roughness : f32,
    metallic : f32,
    paletteWeights : vec4<f32>,
}

fn evaluateVoxelShadingProgram(
    program : VoxelShadingProgram, normalizedDepth : f32,
    stableRandom : f32, terrainSlope : f32,
    terrainSlopeClassified : bool,
    sideOrRiser : bool, parameters : vec4<f32>,
    materialFields : vec4<f32>) -> VoxelShadingResult {
    var registers : array<f32, 8>;
    // Negative PBR values mean "retain the authored material scalar".
    var result = VoxelShadingResult(
        0.0, 0.0, 1.0, -1.0, -1.0,
        vec4<f32>(1.0, 0.0, 0.0, 0.0));
    for (var index = 0u; index < VOXEL_SHADING_MAX_INSTRUCTIONS;
         index += 1u) {
        if (index >= program.instructionCount) {
            break;
        }
        let instruction =
            voxelShadingInstructions[program.instructionOffset + index];
        let opcode = instruction.opcodeOperands & 0xffu;
        let destination = (instruction.opcodeOperands >> 8u) & 0xffu;
        let source0 = (instruction.opcodeOperands >> 16u) & 0xffu;
        let source1 = instruction.opcodeOperands >> 24u;
        if (opcode == 0u) {
            registers[destination] = normalizedDepth;
        } else if (opcode == 1u) {
            registers[destination] = stableRandom;
        } else if (opcode == 2u) {
            registers[destination] = clamp(terrainSlope, 0.0, 1.0);
        } else if (opcode == 3u) {
            registers[destination] = instruction.immediate0;
        } else if (opcode == 4u) {
            registers[destination] = registers[source0] + registers[source1];
        } else if (opcode == 5u) {
            registers[destination] = registers[source0] * registers[source1];
        } else if (opcode == 6u) {
            registers[destination] = 1.0 - registers[source0];
        } else if (opcode == 7u) {
            registers[destination] = clamp(registers[source0], 0.0, 1.0);
        } else if (opcode == 8u) {
            registers[destination] = smoothstep(
                instruction.immediate0, instruction.immediate1,
                registers[source0]);
        } else if (opcode == 9u) {
            result.layerWeight = clamp(registers[source0], 0.0, 1.0);
        } else if (opcode == 10u) {
            result.hueVariation = clamp(registers[source0], -1.0, 1.0);
        } else if (opcode == 11u) {
            result.brightness = clamp(registers[source0], 0.0, 2.0);
        } else if (opcode == 12u) {
            registers[destination] =
                registers[source0] * instruction.immediate0 +
                instruction.immediate1;
        } else if (opcode == 13u) {
            registers[destination] = select(0.0, 1.0, sideOrRiser);
        } else if (opcode == 14u) {
            registers[destination] = max(
                registers[source0], registers[source1]);
        } else if (opcode == 15u) {
            result.roughness = clamp(registers[source0], 0.0, 1.0);
        } else if (opcode == 16u) {
            result.metallic = clamp(registers[source0], 0.0, 1.0);
        } else if (opcode == 17u) {
            registers[destination] = select(
                0.0, 1.0, registers[source0] > instruction.immediate0);
        } else if (opcode == 18u) {
            registers[destination] = select(
                0.0, 1.0, terrainSlopeClassified);
        } else if (opcode == 19u) {
            registers[destination] = parameters[source0];
        } else if (opcode == 20u) {
            registers[destination] = clamp(materialFields[source0], 0.0, 1.0);
        } else if (opcode == 21u) {
            result.paletteWeights.z = clamp(registers[source0], 0.0, 1.0);
        } else if (opcode == 22u) {
            result.paletteWeights.w = clamp(registers[source0], 0.0, 1.0);
        }
    }
    result.paletteWeights.y = result.layerWeight;
    let nonPrimary = result.paletteWeights.y +
        result.paletteWeights.z + result.paletteWeights.w;
    if (nonPrimary > 1.0) {
        result.paletteWeights.y /= nonPrimary;
        result.paletteWeights.z /= nonPrimary;
        result.paletteWeights.w /= nonPrimary;
    }
    result.paletteWeights.x = max(1.0 - nonPrimary, 0.0);
    result.layerWeight = result.paletteWeights.y;
    return result;
}

fn voxelAdditionalPaletteColour(
    materialHandle : u32, index : u32, fallback : vec3<f32>) -> vec3<f32> {
    if (materialHandle >= arrayLength(&voxelShadingPalettes) || index >= 2u) {
        return fallback;
    }
    return decodeAlbedo(
        voxelShadingPalettes[materialHandle].additionalAlbedoRGBA8[index]).rgb;
}

fn colourLayerBlendsOnFace(
    layers : VoxelColourLayers, packedFace : u32) -> bool {
    let topOrBottomFace = (packedFace & 3u) == 1u;
    let mode = select(
        SPARSE_VOXEL_MATERIAL_BLEND_SIDE_FACES,
        SPARSE_VOXEL_MATERIAL_BLEND_TOP_BOTTOM_FACES,
        topOrBottomFace);
    return (layers.faceModes & mode) != 0u;
}

struct ExactMicroMaterial {
    albedo : vec3<f32>,
    roughness : f32,
    metallic : f32,
    layerWeight : f32,
}

fn exactMicroMaterial(
    surface : ResolvedVoxelSurface, payload : u32, top : vec3<f32>,
    layers : VoxelColourLayers, roughness : f32,
    metallic : f32,
    materialSampleAddress : VoxelMaterialSampleAddress) -> ExactMicroMaterial {
    if ((payload & SPARSE_VOXEL_MICRO_PAYLOAD_VALID) == 0u) {
        return ExactMicroMaterial(top, roughness, metallic, 0.0);
    }
    let descriptorId = payload & 0xffffu;
    if (descriptorId == 0u || descriptorId >= arrayLength(&microMaterials)) {
        return ExactMicroMaterial(top, roughness, metallic, 0.0);
    }
    let descriptor = microMaterials[descriptorId];
    let subdivision = descriptor.geometry & 0x0fu;
    if (subdivision < 2u) {
        return ExactMicroMaterial(top, roughness, metallic, 0.0);
    }
    // One-colour materials may still use colour variation or PBR outputs. For
    // them the colour ramp degenerates to top->top while every scalar output
    // remains active.
    let programBottom = select(top, layers.bottom, layers.enabled);
    let depthLayer = (payload >> 24u) & 0x0fu;
    let heightfield =
        (payload & SPARSE_VOXEL_MICRO_PAYLOAD_HEIGHTFIELD) != 0u;
    let fallback =
        (payload & SPARSE_VOXEL_MICRO_PAYLOAD_FALLBACK) != 0u;
    let resolvedContext = sparseVoxelResolvedContext(surface);
    let contextIsSurface =
        (resolvedContext & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
    let encodedTerrainSlope =
        resolvedContext & SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_MASK;
    let terrainSlopeClassified =
        (payload & SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE) != 0u ||
        contextIsSurface;
    // Code zero is an old binary slope context. New runtime pages use codes
    // 1..127 for a normalized 0..90 degree terrain slope angle.
    var terrainSlope = select(
        0.0, 1.0, terrainSlopeClassified);
    if (contextIsSurface && encodedTerrainSlope != 0u) {
        terrainSlope = f32(encodedTerrainSlope - 1u) /
            SPARSE_VOXEL_MICRO_TERRAIN_SLOPE_QUANTIZATION;
    }
    var normalizedDepth = clamp(
        f32(depthLayer) / f32(max(subdivision - 1u, 1u)), 0.0, 1.0);
    if (fallback) {
        let maximumDepth =
            f32((descriptor.geometry >> 4u) & 0xffu) / 16.0;
        let geometryProfile = (descriptor.geometry >> 18u) & 0x07u;
        // Constant columns all stop at the authored depth. Hash columns are
        // uniform over [0, maximumDepth], so half that value is their exact
        // expected recess. This retains the layer's low-frequency energy
        // without evaluating a new coarse micro pattern.
        var expectedDepth = select(
            maximumDepth * 0.5,
            maximumDepth,
            geometryProfile == SPARSE_VOXEL_EXACT_MICRO_CONSTANT);
        // A distant terrain staircase is still its visible surface, not a
        // random column sampled halfway through the material. Its top depth is
        // zero; the quantitative terrain slope remains responsible for any
        // grass-to-soil transition. This keeps the exact/fallback handoff from
        // turning flat green terrain brown toward the horizon.
        let terrainSurface =
            resolvedContext == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE ||
            contextIsSurface;
        if (geometryProfile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE &&
            terrainSurface) {
            expectedDepth = 0.0;
        }
        normalizedDepth = clamp(
            expectedDepth / f32(max(subdivision - 1u, 1u)), 0.0, 1.0);
    }
    let geometryProfile = (descriptor.geometry >> 18u) & 0x07u;
    let profile = (descriptor.geometry >> 21u) & 0x07u;
    let variation = f32(descriptor.shading & 0xffu) / 255.0;
    let parameter0 = f32((descriptor.shading >> 8u) & 0xffu) / 255.0;
    var hash = sparseVoxelExactMicroMix(surface.voxelIndex ^ payload);
    hash = sparseVoxelExactMicroMix(hash ^ (descriptor.geometry >> 24u));
    let random = f32(hash & 0xffffu) / 65535.0;
    let face = surface.packedLocalFace & 7u;
    let descriptorFace = (face & 3u) * 2u +
        select(0u, 1u, (face & 4u) != 0u);
    let descriptorFaceMask = (descriptor.geometry >> 12u) & 0x3fu;
    let carveFace = select(
        firstTrailingBit(descriptorFaceMask), 3u,
        geometryProfile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE);
    let sideOrRiser = descriptorFace != carveFace;
    if (descriptorId < arrayLength(&voxelShadingPrograms)) {
        let program = voxelShadingPrograms[descriptorId];
        let instructionCapacity = arrayLength(&voxelShadingInstructions);
        let programEnabled = (program.flags & 1u) != 0u;
        if (programEnabled && program.instructionCount > 0u &&
            program.instructionCount <= VOXEL_SHADING_MAX_INSTRUCTIONS &&
            program.instructionOffset <= instructionCapacity &&
            program.instructionCount <=
                instructionCapacity - program.instructionOffset) {
            let requiredFields =
                (program.flags & VOXEL_SHADING_PROGRAM_MATERIAL_FIELD_MASK) >>
                8u;
            let materialFields = resolveVoxelMaterialFields(
                descriptorId, requiredFields, subdivision,
                materialSampleAddress,
                voxelShadingDefaultMaterialFields(descriptorId));
            let programResult = evaluateVoxelShadingProgram(
                program, normalizedDepth, random, terrainSlope,
                terrainSlopeClassified, sideOrRiser,
                voxelShadingParameters[descriptorId],
                materialFields);
            var layered = mix(
                top, programBottom, programResult.layerWeight);
            if ((program.flags & VOXEL_SHADING_PROGRAM_PALETTE_MASK) != 0u) {
                let paletteWeights = programResult.paletteWeights;
                layered =
                    top * paletteWeights.x +
                    programBottom * paletteWeights.y +
                    voxelAdditionalPaletteColour(
                        descriptorId, 0u, top) * paletteWeights.z +
                    voxelAdditionalPaletteColour(
                        descriptorId, 1u, top) * paletteWeights.w;
            }
            let colourNoise = voxelColourNoise(
                sparseVoxelExactMicroMix(hash ^ 0x68bc21ebu));
            let albedo = max(
                applyVoxelHueVariation(
                    layered, colourNoise, programResult.hueVariation) *
                    programResult.brightness,
                vec3<f32>(0.0));
            return ExactMicroMaterial(
                albedo,
                select(
                    roughness, programResult.roughness,
                    programResult.roughness >= 0.0),
                select(
                    metallic, programResult.metallic,
                    programResult.metallic >= 0.0),
                programResult.layerWeight);
        }
        // A slot explicitly marked programmable must never execute arbitrary
        // bytes or silently change semantics. Corrupt bounds resolve to the
        // authored primary colour and PBR values; host-side verification
        // reports the actual compile error before upload.
        if (programEnabled) {
            return ExactMicroMaterial(top, roughness, metallic, 0.0);
        }
    }
    if (!layers.enabled) {
        return ExactMicroMaterial(top, roughness, metallic, 0.0);
    }
    if (profile == 1u) {
        let threshold = select(0.25, parameter0, parameter0 > 0.0);
        return ExactMicroMaterial(
            select(top, layers.bottom, normalizedDepth > threshold),
            roughness, metallic, select(0.0, 1.0, normalizedDepth > threshold));
    }
    if (profile == 2u) {
        let noisyDepth = clamp(
            normalizedDepth + (random * 2.0 - 1.0) * variation,
            0.0, 1.0);
        return ExactMicroMaterial(
            mix(top, layers.bottom, noisyDepth), roughness, metallic,
            noisyDepth);
    }
    if (profile == 3u) {
        // ExposedSides treats only the authored carve face as foliage. For
        // terrain staircases that is always +Y; risers and undersides remain
        // the solid bottom/soil colour.
        if (descriptorFace != carveFace) {
            return ExactMicroMaterial(layers.bottom, roughness, metallic, 1.0);
        }
        let dirtVariation = variation * (0.15 + random * 0.85);
        let flatTop = applyVoxelHueVariation(
            mix(top, layers.bottom, dirtVariation),
            voxelColourNoise(hash), variation * 0.35);
        if (fallback) {
            if (geometryProfile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE &&
                !terrainSlopeClassified) {
                return ExactMicroMaterial(flatTop, roughness, metallic, dirtVariation);
            }
            return ExactMicroMaterial(
                mix(top, layers.bottom, normalizedDepth), roughness, metallic,
                normalizedDepth);
        }
        // A general volume has no single authored top face: every resolved
        // boundary is exposed by subtraction, so the exposed-side profile
        // consistently selects the interior material.
        if (!heightfield &&
            geometryProfile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE) {
            // Flat terrain parents still resolve exact microcell identities,
            // even though their geometry remains a full plane. Use those
            // stable cells to introduce sparse dirt/foliage thickness without
            // perturbing the deterministic staircase transition itself.
            return ExactMicroMaterial(flatTop, roughness, metallic, dirtVariation);
        }
        if (!heightfield) {
            return ExactMicroMaterial(layers.bottom, roughness, metallic, 1.0);
        }
        let layerMix = select(
            clamp(
                normalizedDepth + dirtVariation * (1.0 - normalizedDepth),
                0.0, 1.0),
            normalizedDepth,
            terrainSlopeClassified);
        return ExactMicroMaterial(
            mix(top, layers.bottom, layerMix), roughness, metallic, layerMix);
    }
    if (profile == 4u) {
        // Keep variation inside the authored two-colour material range. This
        // reads as dirt showing through thick foliage instead of unrelated hue
        // noise, and remains stable because the resolved microcell is hashed.
        let layerMix = clamp(
            normalizedDepth * 0.65 + random * variation * 0.35,
            0.0, 1.0);
        return ExactMicroMaterial(
            mix(top, layers.bottom, layerMix), roughness, metallic, layerMix);
    }
    return ExactMicroMaterial(
        mix(top, layers.bottom, normalizedDepth), roughness, metallic,
        normalizedDepth);
}

// Six virtual cells per finest voxel face, evaluated entirely in the existing
// material pass. There is no atlas fetch and no new geometry. The pattern is
// keyed in world space, so temporal jitter and camera motion cannot reshuffle
// it. At sub-pixel scale it fades before the distant-detail filter, preventing
// the high-frequency pattern from becoming shimmer.
fn applyMicroVoxelDetailLegacy(
    pixel : vec2<u32>, surface : ResolvedVoxelSurface,
    albedo : vec3<f32>, faceNormal : vec3<f32>,
    shadingNormal : vec3<f32>) -> MicroVoxelSurface {
    let enabled = materialOverride.microDetail.x > 0.5;
    let baseVoxelSize = max(materialOverride.microGrid.x, 0.0);
    if (!enabled || baseVoxelSize <= 1.0e-6) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }
    let microWorldSize = baseVoxelSize / 3.0;
    let projectedMicroPixels = microWorldSize *
        min(abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y)) /
        max(surface.surfaceT, 1.0e-4);
    let detailFade = sparseVoxelMicroDetailFade(projectedMicroPixels);
    if (detailFade <= 0.0) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }

    let worldPosition = surfaceWorldPosition(vec2<i32>(pixel), surface.surfaceT);
    let face = surface.packedLocalFace & 7u;
    let axis = face & 3u;
    var tangentU = vec3<f32>(1.0, 0.0, 0.0);
    var tangentV = vec3<f32>(0.0, 1.0, 0.0);
    if (axis == 0u) {
        tangentU = vec3<f32>(0.0, 0.0, 1.0);
        tangentV = vec3<f32>(0.0, 1.0, 0.0);
    } else if (axis == 1u) {
        tangentU = vec3<f32>(1.0, 0.0, 0.0);
        tangentV = vec3<f32>(0.0, 0.0, 1.0);
    }

    // Bias toward the solid side before quantizing the face plane, so a hit
    // landing a few ulps on either side of the boundary retains one identity.
    let inside = worldPosition - faceNormal * baseVoxelSize * 0.01;
    let solidCell = vec3<i32>(floor(inside / baseVoxelSize));
    let plane = solidCell[axis];
    var microCoordinate = vec2<f32>(
        dot(worldPosition, tangentU) / baseVoxelSize * 2.0,
        dot(worldPosition, tangentV) / baseVoxelSize * 3.0);
    var microCell = vec2<i32>(floor(microCoordinate));
    let preliminaryHeight = microVoxelHeight(microCell, plane, face);

    // One physical parallax step is enough for piecewise-constant cells. Each
    // hash maps to a recess from the original plane down to the requested
    // maximum (1 cm by default). Convert metres independently to the 2-by-3
    // cell axes, then clamp below half a cell at grazing angles.
    let towardCamera = normalize(
        uniforms.cameraWorldPositionNear.xyz - worldPosition);
    let grazing = max(abs(dot(towardCamera, faceNormal)), 0.20);
    let parallaxDirection = vec2<f32>(
        dot(towardCamera, tangentU), dot(towardCamera, tangentV)) / grazing;
    let preliminaryRecess = (preliminaryHeight * 0.5 + 0.5) *
        max(materialOverride.microDetail.w, 0.0) * detailFade;
    let parallaxCells = clamp(
        parallaxDirection * preliminaryRecess / baseVoxelSize *
            vec2<f32>(2.0, 3.0),
        vec2<f32>(-0.40), vec2<f32>(0.40));
    microCoordinate -= parallaxCells;
    microCell = vec2<i32>(floor(microCoordinate));

    let height = microVoxelHeight(microCell, plane, face);
    let heightLeft = microVoxelHeight(
        microCell + vec2<i32>(-1, 0), plane, face);
    let heightRight = microVoxelHeight(
        microCell + vec2<i32>(1, 0), plane, face);
    let heightDown = microVoxelHeight(
        microCell + vec2<i32>(0, -1), plane, face);
    let heightUp = microVoxelHeight(
        microCell + vec2<i32>(0, 1), plane, face);
    let maximumRecess = max(materialOverride.microDetail.w, 0.0);
    let recessLeft = (heightLeft * 0.5 + 0.5) * maximumRecess;
    let recessRight = (heightRight * 0.5 + 0.5) * maximumRecess;
    let recessDown = (heightDown * 0.5 + 0.5) * maximumRecess;
    let recessUp = (heightUp * 0.5 + 0.5) * maximumRecess;
    // Central differences expressed as physical slopes. This makes a 1 cm
    // recess look equally deep when the underlying voxel scale changes.
    let gradient = vec2<f32>(
        (recessRight - recessLeft) / max(baseVoxelSize, 1.0e-6),
        (recessUp - recessDown) /
            max(baseVoxelSize * (2.0 / 3.0), 1.0e-6));
    let normalStrength = clamp(
        materialOverride.microDetail.z, 0.0, 1.0) * detailFade;
    let detailedNormal = safeVoxelNormal(
        shadingNormal - tangentU * gradient.x * normalStrength -
            tangentV * gradient.y * normalStrength,
        shadingNormal);

    // Multiplying linear RGB by one scalar preserves hue. Keep the range
    // deliberately small: this should read as extra voxels, not camouflage.
    let colourStrength = clamp(
        materialOverride.microDetail.y, 0.0, 0.35) * detailFade;
    // Give every recessed horizontal micro-face one constant shadow value.
    // Since height is piecewise constant per cell, transitions at cell edges
    // are deliberately hard: there is no feathered rim or soft border.
    // This is evaluated on every face axis, including upward/downward faces.
    let currentRecess = (height * 0.5 + 0.5) * maximumRecess;
    let inverseMaximumRecess = 1.0 / max(maximumRecess, 1.0e-6);
    let cavity = clamp(currentRecess * inverseMaximumRecess, 0.0, 1.0);
    let hasRecess = select(0.0, 1.0, maximumRecess > 1.0e-6);
    let shadowMask = cavity * detailFade * hasRecess;
    let shadowStrength = clamp(materialOverride.microGrid.y, 0.0, 0.85);
    let detailedAlbedo = max(
        albedo * (1.0 + height * colourStrength) *
            (1.0 - shadowMask * shadowStrength),
        vec3<f32>(0.0));
    return MicroVoxelSurface(detailedAlbedo, detailedNormal);
}

// Shade the same analytic micro-surface written to hardware depth. Horizontal
// cells receive one constant, hard shadow value; a ray that sees between two
// heights instead receives the connecting wall normal and a darker wall tint.
fn applyMicroVoxelDetail(
    pixel : vec2<u32>, surface : ResolvedVoxelSurface,
    albedo : vec3<f32>, layers : VoxelColourLayers,
    faceNormal : vec3<f32>,
    shadingNormal : vec3<f32>) -> MicroVoxelSurface {
    if ((surface.reserved1 &
         SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) != 0u) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }
    let layerBlends = colourLayerBlendsOnFace(
        layers, surface.packedLocalFace);
    // A clear category bit deliberately disables the top layer and its
    // parallax on these faces. Legacy one-colour materials remain unchanged.
    if (layers.enabled && !layerBlends) {
        return MicroVoxelSurface(layers.bottom, shadingNormal);
    }
    if (sparseVoxelHasTerrainSurfaceContext(surface)) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }
    let enabled = materialOverride.microDetail.x > 0.5;
    let baseVoxelSize = max(materialOverride.microGrid.x, 0.0);
    let maximumRecess = max(materialOverride.microDetail.w, 0.0);
    if (!enabled || baseVoxelSize <= 1.0e-6 || maximumRecess <= 1.0e-6) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }

    let worldPosition = surfaceWorldPosition(
        vec2<i32>(pixel), surface.surfaceT);
    let rayDirection = normalize(
        worldPosition - uniforms.cameraWorldPositionNear.xyz);
    let microHit = traceSparseVoxelMicroSurface(
        worldPosition, rayDirection, faceNormal, surface.surfaceT,
        surface.packedLocalFace, uniforms.focalPrincipal,
        baseVoxelSize, maximumRecess);
    if (!microHit.valid) {
        return MicroVoxelSurface(albedo, shadingNormal);
    }

    var detailedNormal = shadingNormal;
    if (microHit.isWall) {
        let wallNormalStrength = clamp(
            materialOverride.microDetail.z, 0.0, 1.0) *
            microHit.detailFade;
        detailedNormal = safeVoxelNormal(
            mix(shadingNormal, microHit.normal, wallNormalStrength),
            shadingNormal);
    }

    let colourStrength = clamp(
        materialOverride.microDetail.y, 0.0, 0.35) *
        microHit.detailFade;
    let visibleMaximumRecess = max(
        maximumRecess * microHit.detailFade, 1.0e-6);
    let cavity = clamp(
        microHit.cellRecess / visibleMaximumRecess, 0.0, 1.0);
    // Both terms are constant over their primitive. There is intentionally no
    // smoothstep based on distance to an edge: adjacent faces meet sharply.
    let hardShadowMask = select(
        cavity, max(cavity, 0.65), microHit.isWall) *
        microHit.detailFade;
    let shadowStrength = clamp(materialOverride.microGrid.y, 0.0, 0.85);
    // Horizontal recessed cells use their final plane depth; connecting walls
    // use the exact crossed depth. Both therefore meet with the same colour at
    // their shared edge instead of looking like a floating top plate.
    // Do not divide by the already-faded recess. That kept the layer contrast
    // at full strength until the geometric hit disappeared, producing a hard
    // distance ring. Normalize against the authored recess so colour, depth,
    // shadow, and normals all approach the base plane together.
    let layerDepth = clamp(
        microHit.surfaceDepth / max(maximumRecess, 1.0e-6), 0.0, 1.0);
    let layeredAlbedo = select(
        albedo, mix(albedo, layers.bottom, layerDepth), layers.enabled);
    let detailedAlbedo = max(
        layeredAlbedo * (1.0 + microHit.height * colourStrength) *
            (1.0 - hardShadowMask * shadowStrength),
        vec3<f32>(0.0));
    return MicroVoxelSurface(detailedAlbedo, detailedNormal);
}

// Once a 2-by-3 micro cell is smaller than a pixel, retaining its literal hash
// would alias and shimmer. Select a progressively coarser, world-space hash
// cell instead. Keep that cell several *output* pixels wide: a one-pixel signal
// is correctly integrated back to its mean by TAA/MetalFX and used to make the
// horizon look flat. The larger footprint remains visible after temporal
// reconstruction without becoming screen-anchored or requiring the analytic
// wall intersection for unresolved geometry.
fn applyDistantMicroVoxelColour(
    pixel : vec2<u32>, surface : ResolvedVoxelSurface,
    albedo : vec3<f32>, baseAlbedo : vec3<f32>,
    layers : VoxelColourLayers,
    faceNormal : vec3<f32>) -> vec3<f32> {
    // Generated terrain owns square, canonical voxel colours. Do not replace
    // them with the legacy analytic 2-by-3 material micro-grid at distance.
    if (sparseVoxelHasTerrainSurfaceContext(surface)) {
        return albedo;
    }
    if ((surface.reserved1 &
         SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) != 0u) {
        return albedo;
    }
    let enabled = materialOverride.microDetail.x > 0.5;
    let variation = clamp(materialOverride.microDetail.y, 0.0, 0.35);
    let baseVoxelSize = max(materialOverride.microGrid.x, 0.0);
    if (!enabled || variation <= 0.0 || baseVoxelSize <= 1.0e-6) {
        return albedo;
    }

    let microWorldSize = baseVoxelSize / 3.0;
    let projectedMicroPixels = microWorldSize *
        min(abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y)) /
        max(surface.surfaceT, 1.0e-4);
    let resolvedFade = sparseVoxelMicroDetailFade(projectedMicroPixels);
    let distantFade = 1.0 - resolvedFade;
    if (distantFade <= 0.0) {
        return albedo;
    }

    let worldPosition = surfaceWorldPosition(
        vec2<i32>(pixel), surface.surfaceT);
    let face = surface.packedLocalFace & 7u;
    let axis = face & 3u;
    var tangentU = vec3<f32>(1.0, 0.0, 0.0);
    var tangentV = vec3<f32>(0.0, 1.0, 0.0);
    if (axis == 0u) {
        tangentU = vec3<f32>(0.0, 0.0, 1.0);
        tangentV = vec3<f32>(0.0, 1.0, 0.0);
    } else if (axis == 1u) {
        tangentU = vec3<f32>(1.0, 0.0, 0.0);
        tangentV = vec3<f32>(0.0, 0.0, 1.0);
    }

    let inside = worldPosition - faceNormal * baseVoxelSize * 0.01;
    let solidCell = vec3<i32>(floor(inside / baseVoxelSize));
    let plane = solidCell[axis];
    let microCoordinate = vec2<f32>(
        dot(worldPosition, tangentU) / baseVoxelSize * 2.0,
        dot(worldPosition, tangentV) / baseVoxelSize * 3.0);

    // One LOD step doubles the world-space colour cell. About six pixels is
    // large enough to survive the quality MetalFX input ratio and the long
    // terrain history, while still reading as fine breakup at the horizon.
    const DISTANT_COLOUR_FOOTPRINT_PIXELS = 6.0;
    let continuousLod = clamp(
        log2(max(
            DISTANT_COLOUR_FOOTPRINT_PIXELS /
                max(projectedMicroPixels, 1.0e-6),
            1.0)),
        0.0, 20.0);
    let lowerLod = floor(continuousLod);
    let upperLod = min(lowerLod + 1.0, 20.0);
    let lowerScale = exp2(lowerLod);
    let upperScale = exp2(upperLod);
    let lowerCell = vec2<i32>(floor(microCoordinate / lowerScale));
    let upperCell = vec2<i32>(floor(microCoordinate / upperScale));
    let lowerPlane = i32(floor(f32(plane) / lowerScale));
    let upperPlane = i32(floor(f32(plane) / upperScale));
    let exactCell = vec2<i32>(floor(microCoordinate));
    let exactHash = microVoxelHash(vec3<i32>(exactCell, plane), face);
    let exactNoise = voxelColourNoise(exactHash);
    let lowerHash = microVoxelHash(
        vec3<i32>(lowerCell, lowerPlane), face);
    let upperHash = microVoxelHash(
        vec3<i32>(upperCell, upperPlane), face);
    let lowerNoise = voxelColourNoise(lowerHash);
    let upperNoise = voxelColourNoise(upperHash);
    let lodBlend = smoothstep(0.0, 1.0, fract(continuousLod));
    // The two hashes are uncorrelated. A plain mix loses half its variance at
    // the midpoint, producing visible flat distance bands. Renormalise the
    // blend so colour/brightness contrast remains constant through an LOD
    // transition, then clamp the rare amplified extrema.
    let variance = max(
        (1.0 - lodBlend) * (1.0 - lodBlend) + lodBlend * lodBlend,
        1.0e-4);
    let coarseNoise = clamp(
        mix(lowerNoise, upperNoise, lodBlend) * inverseSqrt(variance),
        vec3<f32>(-1.0), vec3<f32>(1.0));
    // Start the fallback with the exact resolved cell hash. Only coarsen it as
    // physical detail fades, so the handoff changes frequency gradually rather
    // than replacing the pattern at one projected size.
    let coarseBlend = smoothstep(0.0, 1.0, distantFade);
    let handoffVariance = max(
        (1.0 - coarseBlend) * (1.0 - coarseBlend) +
            coarseBlend * coarseBlend,
        1.0e-4);
    let noise = clamp(
        mix(exactNoise, coarseNoise, coarseBlend) *
            inverseSqrt(handoffVariance),
        vec3<f32>(-1.0), vec3<f32>(1.0));
    // Once physical recess depth is sub-pixel, retain its material meaning as
    // a band-limited colour LOD. This is deliberately driven by the same
    // world-space hash, not screen position, so green/dirt patches remain
    // stable through MetalFX and camera motion.
    let exactLayerMix = f32(exactHash & 0xffffu) / 65535.0;
    let coarseLayerMix = clamp(noise.x * 0.5 + 0.5, 0.0, 1.0);
    let distantLayerMix = mix(
        exactLayerMix, coarseLayerMix, coarseBlend);
    let useDistantLayers = layers.enabled &&
        colourLayerBlendsOnFace(layers, surface.packedLocalFace);
    // `albedo` already contains the resolved micro surface. Cross-fade from it
    // to one independently constructed distant layer; blending it toward dirt
    // a second time caused a contrast dip in the middle of the transition.
    let distantBaseAlbedo = mix(baseAlbedo, albedo, distantFade);
    let distantLayeredAlbedo = mix(
        distantBaseAlbedo, layers.bottom, distantLayerMix);
    let layeredAlbedo = select(
        albedo, mix(albedo, distantLayeredAlbedo, distantFade),
        useDistantLayers);
    let varied = applyVoxelHueVariation(
        layeredAlbedo, noise, variation * distantFade);
    // Near resolved cells already receive hard recess shading. Preserve an
    // equivalent (but restrained) luminance breakup as that geometric term
    // disappears, instead of leaving only a subtle chroma perturbation that
    // temporal reconstruction can average into flat green.
    let brightnessNoise = dot(noise, vec3<f32>(0.25, 0.50, 0.25));
    let brightnessContrast = brightnessNoise * variation * distantFade * 0.75;
    return max(varied * (1.0 + brightnessContrast), vec3<f32>(0.0));
}

struct DistantSurfaceSample {
    position : vec3<f32>,
    normal : vec3<f32>,
    albedo : vec3<f32>,
    valid : bool,
}

fn distantSurfaceSample(
    coordinate : vec2<i32>, dimensions : vec2<u32>,
    centerInstance : u32,
    allowTerrainNeighbour : bool) -> DistantSurfaceSample {
    if (any(coordinate < vec2<i32>(0)) ||
        any(coordinate >= vec2<i32>(dimensions))) {
        return DistantSurfaceSample(
            vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), false);
    }
    let index = u32(coordinate.y) * dimensions.x + u32(coordinate.x);
    if (index >= arrayLength(&resolvedSurfaces)) {
        return DistantSurfaceSample(
            vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), false);
    }
    let surface = resolvedSurfaces[index];
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u ||
        (!allowTerrainNeighbour && surface.instanceIndex != centerInstance) ||
        surface.instanceIndex >= arrayLength(&instances)) {
        return DistantSurfaceSample(
            vec3<f32>(0.0), vec3<f32>(0.0), vec3<f32>(0.0), false);
    }
    let normal = transformNormal(
        instances[surface.instanceIndex].worldToModel,
        localFaceNormal(surface.packedLocalFace));
    return DistantSurfaceSample(
        surfaceWorldPosition(coordinate, surface.surfaceT), normal,
        surfaceAlbedo(surface), true);
}

struct FilteredDistantSurface {
    normal : vec3<f32>,
    albedo : vec3<f32>,
    roughness : f32,
}

// Screen-space material LOD for far voxel microgeometry.
//
// The filter follows the apparent depth plane rather than raw depth, so a
// sloped surface may share samples while an occluding contour cannot. Normal
// coherence and albedo variance decide whether the neighbourhood is genuine
// sub-pixel detail; smooth regions pay the gathers but keep their exact value.
// Lost normal length is folded into roughness to conserve specular energy.
fn filterDistantVoxelSurface(
    pixel : vec2<u32>, centerSurface : ResolvedVoxelSurface,
    centerNormal : vec3<f32>, centerAlbedo : vec3<f32>,
    roughness : f32) -> FilteredDistantSurface {
    if (sparseVoxelHasTerrainSurfaceContext(centerSurface) &&
        (centerSurface.reserved1 &
             SPARSE_VOXEL_EXACT_MICRO_MATERIAL_FLAG) == 0u) {
        return FilteredDistantSurface(
            centerNormal, centerAlbedo, roughness);
    }
    let enabled = materialOverride.distantFilter.x > 0.5;
    // Terrain pages are independent toroidal instances, but together form one
    // continuous height field. Value 2 is the terrain-aware form of the
    // filter: it may reconstruct a macro slope across a page boundary. The
    // ordinary value 1 keeps model-instance isolation for non-terrain scenes.
    let allowTerrainNeighbour = materialOverride.distantFilter.x > 1.5;
    let startDistance = max(materialOverride.distantFilter.y, 0.0);
    let endDistance = max(
        materialOverride.distantFilter.z, startDistance + 1.0e-3);
    let distanceFade = smoothstep(
        startDistance, endDistance, max(centerSurface.surfaceT, 0.0));
    if (!enabled || distanceFade <= 0.0) {
        return FilteredDistantSurface(centerNormal, centerAlbedo, roughness);
    }

    let dimensions = max(uniforms.viewportTiles.xy, vec2<u32>(1u));
    let maximumRadius = u32(clamp(
        round(materialOverride.distantFilterDetail.y), 1.0,
        select(4.0, 8.0, allowTerrainNeighbour)));
    let radius = 1u + u32(round(distanceFade * f32(maximumRadius - 1u)));
    let stride = i32(radius);
    let centerPixel = vec2<i32>(pixel);
    let centerPosition = surfaceWorldPosition(
        centerPixel, centerSurface.surfaceT);

    let left = distantSurfaceSample(
        centerPixel + vec2<i32>(-stride, 0), dimensions,
        centerSurface.instanceIndex, allowTerrainNeighbour);
    let right = distantSurfaceSample(
        centerPixel + vec2<i32>(stride, 0), dimensions,
        centerSurface.instanceIndex, allowTerrainNeighbour);
    let up = distantSurfaceSample(
        centerPixel + vec2<i32>(0, -stride), dimensions,
        centerSurface.instanceIndex, allowTerrainNeighbour);
    let down = distantSurfaceSample(
        centerPixel + vec2<i32>(0, stride), dimensions,
        centerSurface.instanceIndex, allowTerrainNeighbour);

    var macroNormal = centerNormal;
    if (left.valid && right.valid && up.valid && down.valid) {
        let macroCross = cross(right.position - left.position,
                               down.position - up.position);
        macroNormal = safeVoxelNormal(macroCross, centerNormal);
        if (select(
                dot(macroNormal, centerNormal) < 0.0,
                macroNormal.y < 0.0,
                allowTerrainNeighbour)) {
            macroNormal = -macroNormal;
        }
    }

    let pixelFootprint = max(
        centerSurface.surfaceT /
            max(min(abs(uniforms.focalPrincipal.x),
                    abs(uniforms.focalPrincipal.y)), 1.0),
        1.0e-5);
    let planeSigma = pixelFootprint * f32(radius) *
        max(materialOverride.distantFilterDetail.z, 0.25);
    var normalSum = centerNormal * 2.0;
    var albedoSum = centerAlbedo * 2.0;
    var albedoSquareSum = centerAlbedo * centerAlbedo * 2.0;
    var weightSum = 2.0;

    for (var y = -1; y <= 1; y += 1) {
        for (var x = -1; x <= 1; x += 1) {
            if (x == 0 && y == 0) { continue; }
            let sample = distantSurfaceSample(
                centerPixel + vec2<i32>(x * stride, y * stride),
                dimensions, centerSurface.instanceIndex,
                allowTerrainNeighbour);
            if (!sample.valid) { continue; }
            let planeDistance = abs(dot(
                sample.position - centerPosition, macroNormal));
            let planeWeight = exp(-planeDistance * planeDistance /
                max(2.0 * planeSigma * planeSigma, 1.0e-8));
            let spatialWeight = select(0.55, 0.80, x == 0 || y == 0);
            let weight = planeWeight * spatialWeight;
            normalSum += sample.normal * weight;
            albedoSum += sample.albedo * weight;
            albedoSquareSum += sample.albedo * sample.albedo * weight;
            weightSum += weight;
        }
    }

    let meanNormalRaw = normalSum / max(weightSum, 1.0e-4);
    let normalVariance = 1.0 - clamp(length(meanNormalRaw), 0.0, 1.0);
    let meanNormal = safeVoxelNormal(meanNormalRaw, centerNormal);
    // The depth-derived normal describes the macro slope, while the averaged
    // G-buffer normal retains intentional broad curvature.
    // A terrain riser is not a separate vertical material surface: it is one
    // quantized sample of the continuous height-field slope. Once it is
    // sub-pixel, retaining its face normal makes every finer page acquire dark
    // contour bands. Terrain therefore shades with the depth-derived macro
    // slope; ordinary voxel models retain the conservative mixed normal.
    let targetNormal = select(
        safeVoxelNormal(mix(meanNormal, macroNormal, 0.55), centerNormal),
        macroNormal,
        allowTerrainNeighbour);
    let meanAlbedo = albedoSum / max(weightSum, 1.0e-4);
    let albedoVariance3 = max(
        albedoSquareSum / max(weightSum, 1.0e-4) -
            meanAlbedo * meanAlbedo,
        vec3<f32>(0.0));
    let albedoVariance = sqrt(max(
        albedoVariance3.x,
        max(albedoVariance3.y, albedoVariance3.z)));
    let detailVariance = max(normalVariance, albedoVariance);
    let threshold = max(
        materialOverride.distantFilterDetail.w, 1.0e-4);
    let detailActivation = smoothstep(
        threshold, threshold * 3.0, detailVariance);
    // Terrain's activation is distance-driven. Requiring local variance makes
    // the result depend on which internal LOD happens to be resident, which is
    // precisely the page-shaped temporal discontinuity this filter removes.
    let activation = distanceFade * select(
        detailActivation, 1.0, allowTerrainNeighbour);
    let normalBlend = activation * clamp(
        materialOverride.distantFilter.w, 0.0, 1.0);
    let albedoBlend = activation * clamp(
        materialOverride.distantFilterDetail.x, 0.0, 1.0);
    let filteredNormal = safeVoxelNormal(
        mix(centerNormal, targetNormal, normalBlend), centerNormal);
    let filteredAlbedo = mix(centerAlbedo, meanAlbedo, albedoBlend);
    let filteredRoughness = clamp(
        roughness + normalVariance *
            max(materialOverride.normalFilter.z, 0.0) * activation,
        0.0, 1.0);
    return FilteredDistantSurface(
        filteredNormal, filteredAlbedo, filteredRoughness);
}

@compute @workgroup_size(8, 8, 1)
fn writeVoxelMaterialGBuffer(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = globalId.xy;
    if (any(pixel >= uniforms.viewportTiles.xy)) {
        return;
    }
    let pixelI = vec2<i32>(pixel);
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    let surface = resolvedSurfaces[pixelIndex];
    let sampleAddress = materialSampleAddresses[pixelIndex];
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u) {
        textureStore(baseColorTexture, pixelI, vec4<f32>(0.0));
        textureStore(materialPropertiesTexture, pixelI, vec4<f32>(0.0));
        textureStore(emissiveTexture, pixelI, vec4<f32>(0.0));
        textureStore(normalTexture, pixelI, vec4<f32>(0.0));
        return;
    }

    let instance = instances[surface.instanceIndex];
    let surfacePosition = surfaceWorldPosition(pixelI, surface.surfaceT);
    let primaryRayDirection = safeVoxelNormal(
        surfacePosition - uniforms.cameraWorldPositionNear.xyz,
        vec3<f32>(0.0, 0.0, 1.0));
    let worldNormal = sparseVoxelSolidTerrainFaceNormal(
        surface,
        transformNormal(
            instance.worldToModel, localFaceNormal(surface.packedLocalFace)),
        primaryRayDirection);

    // An occupancy-only resolver hits real geometry but has no voxel index that
    // addresses the material array. Indexing it anyway lands on material 0 for
    // every pixel, which paints the whole scene with one arbitrary - typically
    // near-black - albedo. Shade a neutral surface instead, so geometry stays
    // readable and it is obvious that appearance is missing rather than wrong.
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_NO_MATERIAL) != 0u) {
        const NEUTRAL_ALBEDO = vec4<f32>(0.5, 0.5, 0.5, 1.0);
        const NEUTRAL_ROUGHNESS = 0.6;
        let filtered = filterVoxelNormal(
            pixel, surface, worldNormal, NEUTRAL_ROUGHNESS);
        let voxelAlbedo = applyTerrainVoxelColour(
            pixel, surface, sampleAddress, NEUTRAL_ALBEDO.rgb);
        let neutralLayers = VoxelColourLayers(
            vec3<f32>(0.0), 0u, false);
        let micro = applyMicroVoxelDetail(
            pixel, surface, voxelAlbedo, neutralLayers,
            worldNormal, filtered.normal);
        let distant = filterDistantVoxelSurface(
            pixel, surface, micro.normal, micro.albedo,
            filtered.roughness);
        textureStore(
            baseColorTexture, pixelI,
            vec4<f32>(
                applyPerVoxelBrightness(
                    pixel, surface, sampleAddress,
                    applyDistantMicroVoxelColour(
                        pixel, surface, distant.albedo, voxelAlbedo,
                        neutralLayers,
                        worldNormal)),
                NEUTRAL_ALBEDO.a));
        textureStore(
            materialPropertiesTexture,
            pixelI,
            vec4<f32>(distant.roughness, 0.0, 1.0, 0.0));
        textureStore(emissiveTexture, pixelI, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        textureStore(
            normalTexture, pixelI,
            vec4<f32>(distant.normal, distant.roughness));
        return;
    }

    // A resolver whose representation stores appearance inline hands it over in
    // the surface itself, so there is no material array to index. The brick
    // hierarchy does this: eight packed bytes per occupied voxel, which is
    // cheaper to reach than a 4-byte index plus a dependent 32-byte record.
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_PACKED_MATERIAL) != 0u) {
        let packedAlbedo = surface.reserved0;
        let packedScalars = surface.reserved1;
        // Packed bricks retain the authored sRGB8 channels. Feeding the bytes
        // directly to the linear lighting buffer makes mid-tone terrain much
        // too bright (green grass approaches white under the default sun).
        // Match the regular 32-byte material path exactly.
        let packedBaseColor = decodeAlbedo(packedAlbedo | 0xff000000u);
        let authoredRoughness =
            f32((packedAlbedo >> 24u) & 0xffu) / 255.0;
        let authoredMetallic =
            f32((packedScalars >> 8u) & 0xffu) / 255.0;
        let voxelAlbedo = applyTerrainVoxelColour(
            pixel, surface, sampleAddress, packedBaseColor.rgb);
        let packedLayerEnabled =
            (packedScalars & SPARSE_VOXEL_PACKED_SECONDARY_ENABLED) != 0u;
        var packedFaceModes = 0u;
        if ((packedScalars &
             SPARSE_VOXEL_PACKED_BLEND_TOP_BOTTOM_FACES) != 0u) {
            packedFaceModes |=
                SPARSE_VOXEL_MATERIAL_BLEND_TOP_BOTTOM_FACES;
        }
        if ((packedScalars &
             SPARSE_VOXEL_PACKED_BLEND_SIDE_FACES) != 0u) {
            packedFaceModes |= SPARSE_VOXEL_MATERIAL_BLEND_SIDE_FACES;
        }
        let packedBottomBase = decodeAlbedo(
            (surface.reserved2 & SPARSE_VOXEL_RESOLVED_BOTTOM_ALBEDO_MASK) |
            0xff000000u).rgb;
        let packedBottom = select(
            packedBottomBase,
            applyTerrainVoxelColour(
                pixel, surface, sampleAddress, packedBottomBase),
            packedLayerEnabled);
        let packedLayers = VoxelColourLayers(
            packedBottom, packedFaceModes, packedLayerEnabled);
        // The material program runs before global inspection overrides and
        // normal-variance filtering. Thus an authored soil roughness behaves
        // like an ordinary base material scalar, while the renderer can still
        // add anti-aliasing roughness afterward.
        let exact = exactMicroMaterial(
            surface, microPayloads[pixelIndex], voxelAlbedo, packedLayers,
            authoredRoughness, authoredMetallic,
            sampleAddress);
        let packedOverridden = applyMaterialOverride(
            exact.roughness, exact.metallic);
        let filtered = filterVoxelNormal(
            pixel, surface, worldNormal, packedOverridden.x);
        let micro = applyMicroVoxelDetail(
            pixel, surface, exact.albedo, packedLayers,
            worldNormal, filtered.normal);
        let distant = filterDistantVoxelSurface(
            pixel, surface, micro.normal, micro.albedo,
            filtered.roughness);
        // Emissive is one scalar tinting the surface's own albedo, which is how
        // the octree's three-half emissive was collapsed on the way in.
        let packedEmissive =
            f32(packedScalars & 0xffu) / 255.0 * SPARSE_VOXEL_PACKED_EMISSIVE_SCALE;
        textureStore(
            baseColorTexture, pixelI,
            vec4<f32>(
                applyPerVoxelBrightness(
                    pixel, surface, sampleAddress,
                    applyDistantMicroVoxelColour(
                        pixel, surface, distant.albedo, voxelAlbedo,
                        packedLayers,
                        worldNormal)),
                packedBaseColor.a));
        textureStore(
            materialPropertiesTexture,
            pixelI,
            vec4<f32>(
                distant.roughness, packedOverridden.y, exact.layerWeight,
                f32(sparseVoxelResolvedContext(surface))));
        textureStore(
            emissiveTexture,
            pixelI,
            vec4<f32>(packedBaseColor.rgb * packedEmissive, 1.0) * EMISSIVE_MATERIAL_SCALE);
        textureStore(
            normalTexture, pixelI,
            vec4<f32>(distant.normal, distant.roughness));
        return;
    }

    let material = materials[surface.voxelIndex];
    let albedo = decodeAlbedo(material.albedoRGBA8);
    let overridden = applyMaterialOverride(
        f32(material.roughnessMetallicUNorm8 & 0xffu) / 255.0,
        f32((material.roughnessMetallicUNorm8 >> 8u) & 0xffu) / 255.0);
    let roughness = overridden.x;
    let metallic = overridden.y;
    let filtered = filterVoxelNormal(pixel, surface, worldNormal, roughness);
    let voxelAlbedo = applyTerrainVoxelColour(
        pixel, surface, sampleAddress, albedo.rgb);
    let layerEnabled = (material.reserved0 >> 24u) != 0u;
    let bottomBase = decodeAlbedo(material.reserved0).rgb;
    let bottomAlbedo = select(
        bottomBase,
        applyTerrainVoxelColour(
            pixel, surface, sampleAddress, bottomBase),
        layerEnabled);
    let colourLayers = VoxelColourLayers(
        bottomAlbedo,
        material.reserved1 &
            (SPARSE_VOXEL_MATERIAL_BLEND_TOP_BOTTOM_FACES |
             SPARSE_VOXEL_MATERIAL_BLEND_SIDE_FACES),
        layerEnabled);
    let micro = applyMicroVoxelDetail(
        pixel, surface, voxelAlbedo, colourLayers,
        worldNormal, filtered.normal);
    let distant = filterDistantVoxelSurface(
        pixel, surface, micro.normal, micro.albedo,
        filtered.roughness);
    let emissiveRG = vec2<f32>(halfToFloat(material.emissiveRG16F), halfToFloat(material.emissiveRG16F >> 16u));
    let emissiveB = halfToFloat(material.emissiveB16F);

    textureStore(
        baseColorTexture, pixelI,
        vec4<f32>(
            applyPerVoxelBrightness(
                pixel, surface, sampleAddress,
                applyDistantMicroVoxelColour(
                    pixel, surface, distant.albedo, voxelAlbedo,
                    colourLayers,
                    worldNormal)),
            albedo.a));
    textureStore(materialPropertiesTexture, pixelI, vec4<f32>(distant.roughness, metallic, albedo.a, f32(material.flags)));
    textureStore(
        emissiveTexture,
        pixelI,
        vec4<f32>(emissiveRG, emissiveB, 1.0) * EMISSIVE_MATERIAL_SCALE);
    textureStore(
        normalTexture, pixelI,
        vec4<f32>(distant.normal, distant.roughness));
}
