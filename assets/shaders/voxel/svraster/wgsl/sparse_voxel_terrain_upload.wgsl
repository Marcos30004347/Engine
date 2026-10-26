const TERRAIN_UPLOAD_MAX_SECTIONS = 1024u;

struct TerrainUploadSettings {
    uploadWords : u32,
    sectionCount : u32,
    generation : u32,
    atlasWidth : u32,
    publicationCount : u32,
    freezePublication : u32,
    nodesPerPage : u32,
    materialCellsPerPage : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
    reserved3 : u32,
    // x: staging word offset, y: destination word offset,
    // z: word count, w: destination id.
    sections : array<vec4<u32>, 1024>,
}

struct TerrainPageState {
    generation : u32,
    currentLOD : u32,
    publishedFrame : u32,
    flags : u32,
}

struct TerrainTree64Descriptor {
    terrainSlot : u32,
    terrainGeneration : u32,
    internalLOD : u32,
    flags : u32,
    nodePageSlot : u32,
    nodePageGeneration : u32,
    rootNodeOffset : u32,
    nodeCount : u32,
    materialPageSlot : u32,
    materialPageGeneration : u32,
    materialCellOffset : u32,
    materialCellCount : u32,
    geometryRevisionLo : u32,
    geometryRevisionHi : u32,
    modelSide : u32,
    modelCellSizeBits : u32,
}

struct TerrainTree64DynamicInstance {
    worldToModel : mat4x4<f32>,
    worldBoundsMinimum : vec4<f32>,
    worldBoundsMaximum : vec4<f32>,
    modelIndex : u32,
    stableInstanceId : u32,
    contentGeneration : u32,
    flags : u32,
    terrainSlot : u32,
    terrainGeneration : u32,
    regionMappingId : u32,
    receiverCellSizeBits : u32,
}

struct TerrainPublicationRecord {
    terrainSlot : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
    expectedPageState : TerrainPageState,
    replacementPageState : TerrainPageState,
    expectedTree64 : TerrainTree64Descriptor,
    replacementTree64 : TerrainTree64Descriptor,
    replacementDynamicInstance : TerrainTree64DynamicInstance,
}

struct TerrainPublicationStatus {
    accepted : atomic<u32>,
    rejectedRecord : u32,
    generation : u32,
    publishedRecordCount : atomic<u32>,
    rejectionReason : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

const TERRAIN_PUBLICATION_VALID = 1u;
const TERRAIN_PUBLICATION_DYNAMIC_INSTANCE = 2u;
const TERRAIN_PUBLICATION_DYNAMIC_FLAGS_ONLY = 4u;
const TERRAIN_TREE64_PAGE_VALID = 1u;
const TREE64_DYNAMIC_VALID = 1u;
const TREE64_DYNAMIC_INSTANCE_WORDS = 32u;
const TREE64_DYNAMIC_CONTENT_GENERATION_WORD = 26u;
const TREE64_DYNAMIC_FLAGS_WORD = 27u;
const TREE64_DYNAMIC_TERRAIN_GENERATION_WORD = 29u;

@group(0) @binding(0)
var<storage, read> staging : array<u32>;
@group(0) @binding(1)
var<storage, read_write> brickNodes : array<u32>;
@group(0) @binding(2)
var<storage, read_write> brickOccupancy : array<u32>;
@group(0) @binding(3)
var<storage, read_write> brickMaterials : array<u32>;
@group(0) @binding(4)
var<storage, read_write> brickPalette : array<u32>;
@group(0) @binding(5)
var<storage, read_write> brickPreviouslyVisible : array<u32>;
@group(0) @binding(6)
var<storage, read_write> tree64Memberships : array<u32>;
@group(0) @binding(7)
var<storage, read_write> tree64References : array<u32>;
@group(0) @binding(8)
var<storage, read_write> tree64Instances : array<u32>;
@group(0) @binding(9)
var<storage, read_write> tree64Models : array<u32>;
@group(0) @binding(10)
var<storage, read_write> tree64Nodes : array<u32>;
@group(0) @binding(11)
var<storage, read_write> tree64NodesPadded : array<u32>;
@group(0) @binding(12)
var<uniform> settings : TerrainUploadSettings;
@group(0) @binding(13)
var<storage, read_write> brickPaletteDescriptors : array<u32>;
@group(0) @binding(14)
var<storage, read_write> brickMicroSurfaces : array<u32>;
@group(0) @binding(15)
var<storage, read_write> materialFieldResolvers : array<u32>;
@group(0) @binding(16)
var<storage, read_write> materialFieldMappings : array<u32>;
@group(0) @binding(17)
var<storage, read_write> geometryFieldCache : array<u32>;
@group(0) @binding(18)
var<storage, read_write> tree64MaterialCells : array<u32>;
@group(0) @binding(19)
var<storage, read_write> terrainPageStates : array<TerrainPageState>;
@group(0) @binding(20)
var<storage, read_write> tree64PageNodes : array<u32>;
@group(0) @binding(21)
var<storage, read_write> tree64PageNodesPadded : array<u32>;
@group(0) @binding(22)
var<storage, read_write> tree64PageMaterialCells : array<u32>;
@group(0) @binding(23)
var<storage, read_write> tree64NodePageGenerations : array<u32>;
@group(0) @binding(24)
var<storage, read_write> tree64MaterialPageGenerations : array<u32>;
@group(0) @binding(25)
var<storage, read_write> brickChildIndices : array<u32>;
@group(0) @binding(26)
var<storage, read_write> brickRootIndices : array<u32>;
@group(0) @binding(27)
var materialFieldAtlas : texture_storage_2d<rgba8uint, write>;
@group(0) @binding(28)
var<storage, read_write> terrainTree64Descriptors :
    array<TerrainTree64Descriptor>;
@group(0) @binding(29)
var<storage, read> terrainPublicationRecords :
    array<TerrainPublicationRecord>;
@group(0) @binding(30)
var<storage, read_write> terrainPublicationStatus :
    TerrainPublicationStatus;

fn pageStateEqual(left : TerrainPageState, right : TerrainPageState) -> bool {
    return left.generation == right.generation &&
        left.currentLOD == right.currentLOD &&
        left.publishedFrame == right.publishedFrame &&
        left.flags == right.flags;
}

fn tree64DescriptorEqual(
    left : TerrainTree64Descriptor,
    right : TerrainTree64Descriptor) -> bool {
    return left.terrainSlot == right.terrainSlot &&
        left.terrainGeneration == right.terrainGeneration &&
        left.internalLOD == right.internalLOD &&
        left.flags == right.flags &&
        left.nodePageSlot == right.nodePageSlot &&
        left.nodePageGeneration == right.nodePageGeneration &&
        left.rootNodeOffset == right.rootNodeOffset &&
        left.nodeCount == right.nodeCount &&
        left.materialPageSlot == right.materialPageSlot &&
        left.materialPageGeneration == right.materialPageGeneration &&
        left.materialCellOffset == right.materialCellOffset &&
        left.materialCellCount == right.materialCellCount &&
        left.geometryRevisionLo == right.geometryRevisionLo &&
        left.geometryRevisionHi == right.geometryRevisionHi &&
        left.modelSide == right.modelSide &&
        left.modelCellSizeBits == right.modelCellSizeBits;
}

fn validReplacement(record : TerrainPublicationRecord) -> bool {
    let descriptor = record.replacementTree64;
    let state = record.replacementPageState;
    if ((descriptor.flags & TERRAIN_TREE64_PAGE_VALID) == 0u) {
        return descriptor.flags == 0u && state.generation == 0u;
    }
    if (descriptor.terrainSlot != record.terrainSlot ||
        descriptor.terrainGeneration == 0u ||
        descriptor.terrainGeneration != state.generation ||
        descriptor.internalLOD != state.currentLOD ||
        descriptor.nodePageSlot >= arrayLength(&tree64NodePageGenerations) ||
        descriptor.materialPageSlot >=
            arrayLength(&tree64MaterialPageGenerations) ||
        descriptor.nodePageGeneration == 0u ||
        descriptor.materialPageGeneration == 0u ||
        tree64NodePageGenerations[descriptor.nodePageSlot] !=
            descriptor.nodePageGeneration ||
        tree64MaterialPageGenerations[descriptor.materialPageSlot] !=
            descriptor.materialPageGeneration ||
        settings.nodesPerPage == 0u ||
        settings.materialCellsPerPage == 0u ||
        descriptor.nodeCount == 0u ||
        descriptor.nodeCount > settings.nodesPerPage ||
        descriptor.modelSide == 0u ||
        !(bitcast<f32>(descriptor.modelCellSizeBits) > 0.0) ||
        descriptor.rootNodeOffset >= descriptor.nodeCount ||
        descriptor.materialCellOffset > settings.materialCellsPerPage ||
        descriptor.materialCellCount >
            settings.materialCellsPerPage - descriptor.materialCellOffset) {
        return false;
    }
    return true;
}

@compute @workgroup_size(1, 1, 1)
fn validateTerrainPublication() {
    // Payload-only and idle frames do not own the publication result. Leaving
    // it untouched lets the CPU consume the accepted final batch after later
    // frames have already executed this pass.
    if (settings.publicationCount == 0u) {
        return;
    }
    atomicStore(&terrainPublicationStatus.accepted, 0u);
    atomicStore(&terrainPublicationStatus.publishedRecordCount, 0u);
    terrainPublicationStatus.rejectedRecord = 0xffffffffu;
    terrainPublicationStatus.generation = settings.generation;
    terrainPublicationStatus.rejectionReason = 0u;
    if (settings.freezePublication != 0u) {
        terrainPublicationStatus.rejectedRecord = 0xfffffffeu;
        terrainPublicationStatus.rejectionReason = 5u;
        return;
    }
    if (settings.publicationCount >
        arrayLength(&terrainPublicationRecords)) {
        terrainPublicationStatus.rejectedRecord = 0xfffffffdu;
        terrainPublicationStatus.rejectionReason = 6u;
        return;
    }
    var previousSlot = 0u;
    for (var index = 0u; index < settings.publicationCount; index += 1u) {
        let record = terrainPublicationRecords[index];
        if ((record.flags & TERRAIN_PUBLICATION_VALID) == 0u ||
            record.terrainSlot >= arrayLength(&terrainPageStates) ||
            record.terrainSlot >= arrayLength(&terrainTree64Descriptors) ||
            (index != 0u && record.terrainSlot <= previousSlot)) {
            terrainPublicationStatus.rejectedRecord = index;
            terrainPublicationStatus.rejectionReason = 1u;
            return;
        }
        if (!pageStateEqual(
                terrainPageStates[record.terrainSlot],
                record.expectedPageState)) {
            terrainPublicationStatus.rejectedRecord = index;
            terrainPublicationStatus.rejectionReason = 2u;
            return;
        }
        if (!tree64DescriptorEqual(
                terrainTree64Descriptors[record.terrainSlot],
                record.expectedTree64)) {
            terrainPublicationStatus.rejectedRecord = index;
            terrainPublicationStatus.rejectionReason = 3u;
            return;
        }
        if (!validReplacement(record)) {
            terrainPublicationStatus.rejectedRecord = index;
            terrainPublicationStatus.rejectionReason = 4u;
            return;
        }
        if ((record.flags & TERRAIN_PUBLICATION_DYNAMIC_INSTANCE) != 0u) {
            let flagsOnly = (record.flags &
                TERRAIN_PUBLICATION_DYNAMIC_FLAGS_ONLY) != 0u;
            let instanceBase = record.terrainSlot *
                TREE64_DYNAMIC_INSTANCE_WORDS;
            if (record.terrainSlot >=
                    arrayLength(&tree64Instances) /
                        TREE64_DYNAMIC_INSTANCE_WORDS ||
                (flagsOnly &&
                    (((record.replacementTree64.flags &
                            TERRAIN_TREE64_PAGE_VALID) == 0u) ||
                     record.reserved0 != 0u ||
                     (record.reserved1 & TREE64_DYNAMIC_VALID) == 0u ||
                     record.replacementDynamicInstance.flags != 0u ||
                     (tree64Instances[instanceBase +
                         TREE64_DYNAMIC_FLAGS_WORD] &
                         TREE64_DYNAMIC_VALID) == 0u)) ||
                (!flagsOnly &&
                (((record.replacementTree64.flags &
                        TERRAIN_TREE64_PAGE_VALID) != 0u) &&
                    (record.reserved0 == 0u ||
                     (record.reserved1 & TREE64_DYNAMIC_VALID) == 0u ||
                     record.replacementDynamicInstance.contentGeneration !=
                         record.reserved0 ||
                     record.replacementDynamicInstance.flags != record.reserved1 ||
                     record.replacementDynamicInstance.terrainSlot !=
                         record.terrainSlot ||
                     record.replacementDynamicInstance.terrainGeneration !=
                         record.replacementPageState.generation)) ||
                (((record.replacementTree64.flags &
                        TERRAIN_TREE64_PAGE_VALID) == 0u) &&
                    (record.reserved0 != 0u || record.reserved1 != 0u ||
                     record.replacementDynamicInstance.flags != 0u)))) {
                terrainPublicationStatus.rejectedRecord = index;
                terrainPublicationStatus.rejectionReason = 7u;
                return;
            }
        }
        previousSlot = record.terrainSlot;
    }
    atomicStore(&terrainPublicationStatus.accepted, 1u);
}

fn writeTerrainPageStateWord(destination : u32, value : u32) {
    let slot = destination / 4u;
    let member = destination % 4u;
    if (slot >= arrayLength(&terrainPageStates)) {
        return;
    }
    switch member {
        case 0u: { terrainPageStates[slot].generation = value; }
        case 1u: { terrainPageStates[slot].currentLOD = value; }
        case 2u: { terrainPageStates[slot].publishedFrame = value; }
        case 3u: { terrainPageStates[slot].flags = value; }
        default: {}
    }
}

fn writeTerrainUploadWord(destinationId : u32, destination : u32, value : u32) {
    switch destinationId {
        case 0u: { brickNodes[destination] = value; }
        case 1u: { brickOccupancy[destination] = value; }
        case 2u: { brickMaterials[destination] = value; }
        case 3u: { brickPalette[destination] = value; }
        case 4u: { brickPreviouslyVisible[destination] = value; }
        case 5u: { tree64Memberships[destination] = value; }
        case 6u: { tree64References[destination] = value; }
        case 7u: { tree64Instances[destination] = value; }
        case 8u: { tree64Models[destination] = value; }
        case 9u: { tree64Nodes[destination] = value; }
        case 10u: { tree64NodesPadded[destination] = value; }
        case 11u: { brickPaletteDescriptors[destination] = value; }
        case 12u: { brickMicroSurfaces[destination] = value; }
        case 13u: { materialFieldResolvers[destination] = value; }
        case 14u: { materialFieldMappings[destination] = value; }
        case 15u: { geometryFieldCache[destination] = value; }
        case 16u: {
            tree64MaterialCells[destination] = value;
        }
        case 17u: {
            writeTerrainPageStateWord(destination, value);
        }
        case 18u: {
            tree64PageNodes[destination] = value;
        }
        case 19u: {
            tree64PageNodesPadded[destination] = value;
        }
        case 20u: {
            tree64PageMaterialCells[destination] = value;
        }
        case 21u: {
            tree64NodePageGenerations[destination] = value;
        }
        case 22u: {
            tree64MaterialPageGenerations[destination] = value;
        }
        case 23u: {
            brickChildIndices[destination] = value;
        }
        case 24u: {
            brickRootIndices[destination] = value;
        }
        case 25u: {
            let width = max(settings.atlasWidth, 1u);
            let pixel = vec2<u32>(destination % width, destination / width);
            textureStore(materialFieldAtlas, pixel, vec4<u32>(
                value & 0xffu,
                (value >> 8u) & 0xffu,
                (value >> 16u) & 0xffu,
                (value >> 24u) & 0xffu));
        }
        default: {}
    }
}

@compute @workgroup_size(64, 1, 1)
fn uploadTerrainPages(
    @builtin(global_invocation_id) globalId : vec3<u32>,
    @builtin(num_workgroups) groupCount : vec3<u32>) {
    // Dispatches wider than Vulkan's X limit continue in Y. local_size_y is
    // one, so this is a dense linear index over both dimensions.
    let word = globalId.x + globalId.y * groupCount.x * 64u;
    if (settings.publicationCount != 0u &&
        atomicLoad(&terrainPublicationStatus.accepted) == 0u) {
        return;
    }
    if (word < settings.publicationCount) {
        let publication = terrainPublicationRecords[word];
        terrainPageStates[publication.terrainSlot] =
            publication.replacementPageState;
        terrainTree64Descriptors[publication.terrainSlot] =
            publication.replacementTree64;
        if ((publication.flags &
                TERRAIN_PUBLICATION_DYNAMIC_INSTANCE) != 0u) {
            let replacement = publication.replacementDynamicInstance;
            let base = publication.terrainSlot *
                TREE64_DYNAMIC_INSTANCE_WORDS;
            if ((publication.flags &
                    TERRAIN_PUBLICATION_DYNAMIC_FLAGS_ONLY) != 0u) {
                // This pass is graph-ordered after validation. Publishing the
                // flags word alone preserves the incumbent transform and all
                // stale-owner guards while changing lighting visibility.
                tree64Instances[base + TREE64_DYNAMIC_FLAGS_WORD] =
                    publication.reserved1;
            } else {
            tree64Instances[base + 0u] = bitcast<u32>(replacement.worldToModel[0].x);
            tree64Instances[base + 1u] = bitcast<u32>(replacement.worldToModel[0].y);
            tree64Instances[base + 2u] = bitcast<u32>(replacement.worldToModel[0].z);
            tree64Instances[base + 3u] = bitcast<u32>(replacement.worldToModel[0].w);
            tree64Instances[base + 4u] = bitcast<u32>(replacement.worldToModel[1].x);
            tree64Instances[base + 5u] = bitcast<u32>(replacement.worldToModel[1].y);
            tree64Instances[base + 6u] = bitcast<u32>(replacement.worldToModel[1].z);
            tree64Instances[base + 7u] = bitcast<u32>(replacement.worldToModel[1].w);
            tree64Instances[base + 8u] = bitcast<u32>(replacement.worldToModel[2].x);
            tree64Instances[base + 9u] = bitcast<u32>(replacement.worldToModel[2].y);
            tree64Instances[base + 10u] = bitcast<u32>(replacement.worldToModel[2].z);
            tree64Instances[base + 11u] = bitcast<u32>(replacement.worldToModel[2].w);
            tree64Instances[base + 12u] = bitcast<u32>(replacement.worldToModel[3].x);
            tree64Instances[base + 13u] = bitcast<u32>(replacement.worldToModel[3].y);
            tree64Instances[base + 14u] = bitcast<u32>(replacement.worldToModel[3].z);
            tree64Instances[base + 15u] = bitcast<u32>(replacement.worldToModel[3].w);
            tree64Instances[base + 16u] = bitcast<u32>(replacement.worldBoundsMinimum.x);
            tree64Instances[base + 17u] = bitcast<u32>(replacement.worldBoundsMinimum.y);
            tree64Instances[base + 18u] = bitcast<u32>(replacement.worldBoundsMinimum.z);
            tree64Instances[base + 19u] = bitcast<u32>(replacement.worldBoundsMinimum.w);
            tree64Instances[base + 20u] = bitcast<u32>(replacement.worldBoundsMaximum.x);
            tree64Instances[base + 21u] = bitcast<u32>(replacement.worldBoundsMaximum.y);
            tree64Instances[base + 22u] = bitcast<u32>(replacement.worldBoundsMaximum.z);
            tree64Instances[base + 23u] = bitcast<u32>(replacement.worldBoundsMaximum.w);
            tree64Instances[base + 24u] = replacement.modelIndex;
            tree64Instances[base + 25u] = replacement.stableInstanceId;
            tree64Instances[base + 26u] = replacement.contentGeneration;
            tree64Instances[base + 28u] = replacement.terrainSlot;
            tree64Instances[base + 29u] = replacement.terrainGeneration;
            tree64Instances[base + 30u] = replacement.regionMappingId;
            tree64Instances[base + 31u] = replacement.receiverCellSizeBits;
            // Publish VALID last. Readers execute after this pass's graph
            // barrier and can never observe a valid instance with stale
            // transform, bounds, or generation ownership.
            tree64Instances[base + TREE64_DYNAMIC_FLAGS_WORD] =
                replacement.flags;
            }
        }
        atomicAdd(
            &terrainPublicationStatus.publishedRecordCount, 1u);
    }
    if (word >= settings.uploadWords) {
        return;
    }
    // Section staging ranges are appended in ascending, non-overlapping order.
    // Binary search keeps a large page transaction O(log sections) per word;
    // linearly scanning 500+ small page ranges made the upload itself look like
    // a renderer hang even though all work was on the GPU.
    var low = 0u;
    var high = min(settings.sectionCount, TERRAIN_UPLOAD_MAX_SECTIONS);
    while (low < high) {
        let middle = low + (high - low) / 2u;
        let section = settings.sections[middle];
        if (word < section.x) {
            high = middle;
        } else if (word >= section.x + section.z) {
            low = middle + 1u;
        } else {
            writeTerrainUploadWord(
                section.w,
                section.y + word - section.x,
                staging[word]);
            return;
        }
    }
}
