// Material-field textures are resolved before the shading VM runs. The VM
// sees four ordinary scalar inputs and therefore has no texture opcode,
// descriptor indexing, or residency-dependent control flow.
struct VoxelMaterialFieldResolver {
    controls : vec4<u32>,
    scale : vec4<f32>,
    bias : vec4<f32>,
}

struct VoxelMaterialFieldMapping {
    materialAndFlags : u32,
    regionMappingId : u32,
    atlasOrigin : vec2<u32>,
    atlasExtent : vec2<u32>,
    canonicalOrigin : vec2<i32>,
    canonicalExtent : vec2<u32>,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(16) var<storage, read> voxelMaterialFieldResolvers :
    array<VoxelMaterialFieldResolver>;
@group(0) @binding(17) var<storage, read> voxelMaterialFieldMappings :
    array<VoxelMaterialFieldMapping>;
@group(0) @binding(18) var voxelMaterialFieldAtlas : texture_2d<u32>;

const VOXEL_MATERIAL_FIELD_MAPPING_VALID : u32 = 0x80000000u;
const VOXEL_MATERIAL_FIELD_MAPPING_CHANNEL_SHIFT : u32 = 16u;
const VOXEL_MATERIAL_FIELD_MAPPING_MAXIMUM_PROBES : u32 = 8u;
const VOXEL_MATERIAL_FIELD_SOURCE_CONSTANT : u32 = 0u;
const VOXEL_MATERIAL_FIELD_SOURCE_PLANET_REGION : u32 = 2u;
const VOXEL_MATERIAL_FIELD_MAPPING_PARENT_LOCAL : u32 = 2u;

fn voxelMaterialFieldMix(input : u32) -> u32 {
    var value = input;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn voxelMaterialFieldMappingHash(
    materialHandle : u32, regionMappingId : u32) -> u32 {
    return voxelMaterialFieldMix(
        materialHandle * 0x9e3779b9u ^
        voxelMaterialFieldMix(regionMappingId ^ 0x85ebca6bu));
}

fn findVoxelMaterialFieldMapping(
    materialHandle : u32, regionMappingId : u32) -> u32 {
    let capacity = arrayLength(&voxelMaterialFieldMappings);
    if (capacity == 0u) {
        return 0xffffffffu;
    }
    let start = voxelMaterialFieldMappingHash(
        materialHandle, regionMappingId) % capacity;
    for (var probe = 0u;
         probe < VOXEL_MATERIAL_FIELD_MAPPING_MAXIMUM_PROBES;
         probe += 1u) {
        let index = (start + probe) % capacity;
        let mapping = voxelMaterialFieldMappings[index];
        if ((mapping.materialAndFlags &
             VOXEL_MATERIAL_FIELD_MAPPING_VALID) == 0u) {
            return 0xffffffffu;
        }
        if ((mapping.materialAndFlags & 0xffffu) == materialHandle &&
            mapping.regionMappingId == regionMappingId) {
            return index;
        }
    }
    return 0xffffffffu;
}

fn voxelMaterialFieldPositiveModulo(value : i32, extent : i32) -> i32 {
    let remainder = value % extent;
    return select(remainder, remainder + extent, remainder < 0);
}

fn voxelMaterialFieldAddress(
    value : i32, extent : u32, repeat : bool) -> u32 {
    let signedExtent = i32(extent);
    if (repeat) {
        return u32(voxelMaterialFieldPositiveModulo(value, signedExtent));
    }
    return u32(clamp(value, 0, signedExtent - 1));
}

fn loadVoxelMaterialFieldAtlas(
    mapping : VoxelMaterialFieldMapping, slot : u32,
    coordinate : vec2<i32>, repeat : bool) -> f32 {
    let local = vec2<u32>(
        voxelMaterialFieldAddress(coordinate.x, mapping.atlasExtent.x, repeat),
        voxelMaterialFieldAddress(coordinate.y, mapping.atlasExtent.y, repeat));
    let atlasCoordinate = mapping.atlasOrigin + local;
    let dimensions = textureDimensions(voxelMaterialFieldAtlas);
    if (any(atlasCoordinate >= dimensions)) {
        return 0.0;
    }
    return f32(textureLoad(
        voxelMaterialFieldAtlas, vec2<i32>(atlasCoordinate), 0)[slot]) /
        255.0;
}

fn sampleVoxelMaterialField(
    mapping : VoxelMaterialFieldMapping, control : u32, slot : u32,
    subdivision : u32,
    address : VoxelMaterialSampleAddress) -> f32 {
    let mappingMode = (control >> 2u) & 0x3u;
    let linear = ((control >> 4u) & 1u) != 0u;
    let repeat = ((control >> 5u) & 1u) != 0u;
    var logicalOrigin = mapping.canonicalOrigin;
    var logicalExtent = mapping.canonicalExtent;
    if (mappingMode == VOXEL_MATERIAL_FIELD_MAPPING_PARENT_LOCAL) {
        logicalOrigin = vec2<i32>(0);
        logicalExtent = vec2<u32>(max(subdivision, 1u));
    }
    if (any(logicalExtent == vec2<u32>(0u)) ||
        any(mapping.atlasExtent == vec2<u32>(0u))) {
        return 0.0;
    }
    let canonical = address.canonicalMicroCoordinate.xz;
    var local = canonical - logicalOrigin;
    if (mappingMode == VOXEL_MATERIAL_FIELD_MAPPING_PARENT_LOCAL) {
        local = vec2<i32>(
            voxelMaterialFieldPositiveModulo(
                canonical.x, i32(logicalExtent.x)),
            voxelMaterialFieldPositiveModulo(
                canonical.y, i32(logicalExtent.y)));
    } else {
        local = vec2<i32>(
            i32(voxelMaterialFieldAddress(
                local.x, logicalExtent.x, repeat)),
            i32(voxelMaterialFieldAddress(
                local.y, logicalExtent.y, repeat)));
    }
    let texel =
        (vec2<f32>(local) + vec2<f32>(0.5)) *
            vec2<f32>(mapping.atlasExtent) /
            vec2<f32>(logicalExtent) -
        vec2<f32>(0.5);
    if (!linear) {
        return loadVoxelMaterialFieldAtlas(
            mapping, slot, vec2<i32>(floor(texel + vec2<f32>(0.5))),
            repeat);
    }
    let base = vec2<i32>(floor(texel));
    let fraction = fract(texel);
    return mix(
        mix(
            loadVoxelMaterialFieldAtlas(mapping, slot, base, repeat),
            loadVoxelMaterialFieldAtlas(
                mapping, slot, base + vec2<i32>(1, 0), repeat),
            fraction.x),
        mix(
            loadVoxelMaterialFieldAtlas(
                mapping, slot, base + vec2<i32>(0, 1), repeat),
            loadVoxelMaterialFieldAtlas(
                mapping, slot, base + vec2<i32>(1, 1), repeat),
            fraction.x),
        fraction.y);
}

fn resolveVoxelMaterialFields(
    materialHandle : u32, requiredMask : u32, subdivision : u32,
    address : VoxelMaterialSampleAddress,
    defaults : vec4<f32>) -> vec4<f32> {
    var result = defaults;
    if (materialHandle >= arrayLength(&voxelMaterialFieldResolvers)) {
        return result;
    }
    let resolver = voxelMaterialFieldResolvers[materialHandle];
    for (var slot = 0u; slot < 4u; slot += 1u) {
        if ((requiredMask & (1u << slot)) == 0u) {
            continue;
        }
        let control = resolver.controls[slot];
        let source = control & 0x3u;
        if (source == VOXEL_MATERIAL_FIELD_SOURCE_CONSTANT) {
            continue;
        }
        let regionMappingId = select(
            0u, address.regionMappingId,
            source == VOXEL_MATERIAL_FIELD_SOURCE_PLANET_REGION);
        let mappingIndex = findVoxelMaterialFieldMapping(
            materialHandle, regionMappingId);
        if (mappingIndex == 0xffffffffu) {
            continue;
        }
        let mapping = voxelMaterialFieldMappings[mappingIndex];
        let channelMask =
            (mapping.materialAndFlags >>
                VOXEL_MATERIAL_FIELD_MAPPING_CHANNEL_SHIFT) & 0x0fu;
        if ((channelMask & (1u << slot)) == 0u) {
            continue;
        }
        let sampled = sampleVoxelMaterialField(
            mapping, control, slot, subdivision, address);
        result[slot] = clamp(
            sampled * resolver.scale[slot] + resolver.bias[slot],
            0.0, 1.0);
    }
    return result;
}
