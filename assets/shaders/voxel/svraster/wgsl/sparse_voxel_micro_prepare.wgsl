// Phase0 MicroPrepare. Transform emits disjoint residual occupancy and compact
// exact-parent records; the optional diagnostic compilation also scans the
// immutable source workload and publishes sizing counters.
#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_brick_common.wgsl"
// R3: the shared staircase column quantizer, so the cache this pass emits
// is bit-identical to the raster fragment's live evaluation.
#include "sparse_voxel_micro_columns.wgsl"

const MICRO_PREPARE_WORKGROUP_SIZE : u32 = 64u;
const MICRO_PREPARE_RECORD_BYTES : u32 = 32u;

const MICRO_PROFILE_CONSTANT : u32 = 1u;
const MICRO_PROFILE_HASH_COLUMNS : u32 = 2u;
const MICRO_PROFILE_VOLUME_DENSITY : u32 = 3u;
const MICRO_PROFILE_CLUMPED_COLUMNS : u32 = 4u;
const MICRO_PROFILE_STAIRCASE : u32 = 5u;
const MICRO_MAX_SUBDIVISION : u32 = 8u;

const MICRO_CONTEXT_INTERIOR : u32 = 0u;
const MICRO_CONTEXT_FLAT_SURFACE : u32 = 1u;
const MICRO_CONTEXT_WALL_NEGATIVE_X : u32 = 10u;
const MICRO_CONTEXT_WALL_POSITIVE_Z : u32 = 13u;
const MICRO_CONTEXT_SURFACE_FLAG : u32 = 0x80u;

const MICRO_FADE_START_PIXELS : f32 = 0.50;
const MICRO_FADE_END_PIXELS : f32 = 1.00;

const GEOMETRY_PROGRAM_ENABLED : u32 = 1u;
const GEOMETRY_PROGRAM_DOMAIN_VOLUME : u32 = 1u << 1u;
const GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK : u32 = 0x0f00u;

fn microPrepareTerrainCorner(surfaceData : vec2<u32>, corner : u32) -> u32 {
    if (corner == 0u) { return surfaceData.x & 0x3fffu; }
    if (corner == 1u) { return (surfaceData.x >> 14u) & 0x3fffu; }
    if (corner == 2u) {
        return ((surfaceData.x >> 28u) |
            ((surfaceData.y & 0x03ffu) << 4u)) & 0x3fffu;
    }
    return (surfaceData.y >> 10u) & 0x3fffu;
}

fn microPrepareLegacySurfaceData(surfaceData : vec2<u32>) -> u32 {
    var packed = 0u;
    for (var corner = 0u; corner < 4u; corner += 1u) {
        let quantized = u32(round(
            f32(microPrepareTerrainCorner(surfaceData, corner)) *
                (63.0 / 16383.0)));
        packed |= (quantized & 0x3fu) << (corner * 6u);
    }
    return packed;
}

// Only the immutable metadata word is needed here. This layout mirrors
// voxel::material::VoxelGeometryProgramGPU without pulling the evaluator and
// its unrelated instruction/field bindings into this diagnostic pass.
struct MicroPrepareGeometryProgram {
    instructionOffset : u32,
    instructionCount : u32,
    executionFlags : u32,
    builtInProfile : u32,
}

#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
// All counters are unique-parent counts, unlike SparseVoxelMicroCounters,
// whose fragment-side figures count repeated ray encounters.
struct SparseVoxelMicroPrepareCounters {
    visibleBricks : atomic<u32>,
    occupiedParents : atomic<u32>,
    microParents : atomic<u32>,
    validDescriptors : atomic<u32>,
    invalidDescriptors : atomic<u32>,
    contextInterior : atomic<u32>,
    contextFlat : atomic<u32>,
    contextSlope : atomic<u32>,
    contextWall : atomic<u32>,
    contextOther : atomic<u32>,
    projectedLtHalfPixel : atomic<u32>,
    projectedHalfToOnePixel : atomic<u32>,
    projectedOneToTwoPixels : atomic<u32>,
    projectedGeTwoPixels : atomic<u32>,
    handoffEligible : atomic<u32>,
    exactNearEligible : atomic<u32>,
    heightfieldRecords : atomic<u32>,
    heightfieldMicrocells : atomic<u32>,
    nonHeightfieldExact : atomic<u32>,
    heightfieldQuadsLow : atomic<u32>,
    heightfieldQuadsHigh : atomic<u32>,
    heightfieldBytesLow : atomic<u32>,
    heightfieldBytesHigh : atomic<u32>,
    reserved0 : atomic<u32>,
}
#endif

struct SparseVoxelMicroBrickRecord {
    nodeIndex : u32,
    // Low 9 bits: parent index. Bits 9..24: descriptor ID. Bit 25 records
    // that the descriptor has an enabled geometry-IR program. Bits 26..31 are
    // source-occupied neighbours in descriptor face order (-X,+X,-Y,+Y,-Z,+Z).
    // MicroPrepare has already rejected volume and material-field programs, so
    // the specialized shader needs no other execution flags.
    parentDescriptorFlags : u32,
    paletteAddress : u32,
    // Normalized context in the high byte and the four-corner surface patch in
    // the low 24 bits. Carrying this removes the material-prefix and surface
    // metadata lookups from every prepared fragment while retaining a compact
    // four-word record.
    microContextData : u32,
    // R3: quantized depth of every surface column, four bits each. Generated
    // here once per extracted parent so the raster fragment stops re-deriving
    // the same bilinear patch, quantization and staircase classification for
    // every column it visits. Bit 31 of .w marks it usable.
    columns : vec4<u32>,
}

struct SparseVoxelMicroResidual {
    bits : array<u32, SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS>,
    coarseLow : u32,
    coarseHigh : u32,
    // Nonzero when this brick extracted nothing and its residual mask is
    // therefore byte-for-byte the source occupancy. The fields above are then
    // left unwritten and the draw reads the immutable source instead.
    passthrough : u32,
    padding : u32,
}

#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
struct SparseVoxelMicroPrepareState {
    recordCount : atomic<u32>,
    overflowBricks : atomic<u32>,
    extractedParents : atomic<u32>,
    residualParents : atomic<u32>,
    outputBytesLow : atomic<u32>,
    outputBytesHigh : atomic<u32>,
    // R3: extracted parents whose compact column cache was generated. A zero
    // here against a nonzero record count means every fragment is still taking
    // the live evaluation and the cache is inert.
    cachedRecords : atomic<u32>,
    reserved1 : atomic<u32>,
}
#endif

struct SparseVoxelMicroPrepareDispatch {
    x : u32,
    y : u32,
    z : u32,
}

struct SparseVoxelMicroDrawIndirect {
    vertexCount : u32,
    instanceCount : atomic<u32>,
    firstVertex : u32,
    firstInstance : u32,
}

struct SparseVoxelMicroPrepareCandidate {
    valid : u32,
    record : SparseVoxelMicroBrickRecord,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBrickDrawUniforms;
@group(0) @binding(1) var<storage, read> visibleBricks : array<u32>;
@group(0) @binding(2) var<storage, read> drawArgs : SparseVoxelBrickDrawIndirect;
@group(0) @binding(3) var<storage, read> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(4) var<storage, read> occupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(5) var<storage, read> brickMaterialIndices : array<u32>;
@group(0) @binding(6) var<storage, read> brickMicroSurfaceData : array<vec2<u32>>;
@group(0) @binding(7) var<storage, read> brickPalette : array<SparseVoxelBrickMaterial>;
@group(0) @binding(8) var<storage, read> brickPaletteDescriptorIds : array<u32>;
@group(0) @binding(9) var<storage, read> microMaterials : array<SparseVoxelMicroMaterial>;
@group(0) @binding(10) var<storage, read> geometryPrograms : array<MicroPrepareGeometryProgram>;
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
@group(0) @binding(11) var<storage, read_write> output : SparseVoxelMicroPrepareCounters;
#endif
@group(0) @binding(12) var<storage, read_write> records : array<SparseVoxelMicroBrickRecord>;
@group(0) @binding(13) var<storage, read_write> residuals : array<SparseVoxelMicroResidual>;
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
@group(0) @binding(14) var<storage, read_write> prepareState : SparseVoxelMicroPrepareState;
#endif
@group(0) @binding(15) var<storage, read_write> prepareDispatch : SparseVoxelMicroPrepareDispatch;
@group(0) @binding(16) var<storage, read_write> microDrawArgs : SparseVoxelMicroDrawIndirect;

var<workgroup> transformResidual :
    array<atomic<u32>, SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS>;
var<workgroup> transformRecordCount : atomic<u32>;
var<workgroup> transformRecordBase : u32;
var<workgroup> transformAccepted : u32;
var<workgroup> transformCoarseLow : atomic<u32>;
var<workgroup> transformCoarseHigh : atomic<u32>;
var<workgroup> transformResidualSnapshot :
    array<u32, SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS>;
var<workgroup> transformRecordNode : array<u32, SPARSE_VOXEL_BRICK_VOXEL_COUNT>;
var<workgroup> transformRecordParentDescriptorFlags :
    array<u32, SPARSE_VOXEL_BRICK_VOXEL_COUNT>;
var<workgroup> transformRecordPalette : array<u32, SPARSE_VOXEL_BRICK_VOXEL_COUNT>;
var<workgroup> transformRecordMicroContextData :
    array<u32, SPARSE_VOXEL_BRICK_VOXEL_COUNT>;

// Quantize every surface column of one extracted parent, or return an
// unusable cache when this parent is not a plain staircase surface. The raster
// fragment falls back to live evaluation whenever the valid bit is clear, so a
// conservative refusal here is always safe.
fn microPrepareColumnCache(
    descriptorId : u32, geometryIREnabled : bool,
    microContextData : u32) -> vec4<u32> {
    if (geometryIREnabled || descriptorId == 0u ||
        descriptorId >= arrayLength(&microMaterials)) {
        return vec4<u32>(0u);
    }
    let material = microMaterials[descriptorId];
    let subdivision = material.geometry & 0x0fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    let context = microContextData >> 24u;
    if (profile != MICRO_PROFILE_STAIRCASE ||
        (context & MICRO_CONTEXT_SURFACE_FLAG) == 0u ||
        !sparseVoxelExactMicroColumnCacheFits(subdivision)) {
        return vec4<u32>(0u);
    }
    let surfaceData = microContextData & 0x00ffffffu;
    var cache = vec4<u32>(0u);
    for (var v = 0u; v < subdivision; v += 1u) {
        for (var u = 0u; u < subdivision; u += 1u) {
            let faceCell = vec2<u32>(u, v);
            cache = sparseVoxelExactMicroColumnCacheStore(
                cache,
                sparseVoxelExactMicroColumnCacheIndex(subdivision, faceCell),
                sparseVoxelExactMicroStaircaseSurfaceDepth(
                    subdivision, surfaceData, faceCell));
        }
    }
    cache.w |= SPARSE_VOXEL_MICRO_COLUMN_CACHE_VALID;
    return cache;
}

fn microPrepareDescriptorValid(material : SparseVoxelMicroMaterial) -> bool {
    let subdivision = material.geometry & 0x0fu;
    let maximumDepthFixed = (material.geometry >> 4u) & 0xffu;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let profile = (material.geometry >> 18u) & 0x07u;
    return subdivision >= 2u && subdivision <= MICRO_MAX_SUBDIVISION &&
        maximumDepthFixed != 0u &&
        maximumDepthFixed <= subdivision * 16u && faceMask != 0u &&
        (profile == MICRO_PROFILE_CONSTANT ||
         profile == MICRO_PROFILE_HASH_COLUMNS ||
         profile == MICRO_PROFILE_VOLUME_DENSITY ||
         profile == MICRO_PROFILE_CLUMPED_COLUMNS ||
         profile == MICRO_PROFILE_STAIRCASE);
}

fn microPrepareHandoffWeight(
    material : SparseVoxelMicroMaterial,
    projectedMicroPixels : f32) -> f32 {
    let encodedScale = (material.shading >> 24u) & 0xffu;
    let handoffScale = select(
        1.0, clamp(f32(encodedScale) / 128.0, 0.25, 1.0),
        encodedScale != 0u);
    return smoothstep(
        MICRO_FADE_START_PIXELS * handoffScale,
        MICRO_FADE_END_PIXELS * handoffScale,
        projectedMicroPixels);
}

fn microPrepareGeometryExecutionFlags(descriptorId : u32) -> u32 {
    if (descriptorId < arrayLength(&geometryPrograms)) {
        return geometryPrograms[descriptorId].executionFlags;
    }
    return 0u;
}

fn microPrepareHeightfield(
    executionFlags : u32, material : SparseVoxelMicroMaterial,
    context : u32) -> bool {
    let profile = (material.geometry >> 18u) & 0x07u;
    let faceMask = (material.geometry >> 12u) & 0x3fu;
    let terrainSurface = profile == MICRO_PROFILE_STAIRCASE &&
        (context == MICRO_CONTEXT_FLAT_SURFACE ||
         (context & MICRO_CONTEXT_SURFACE_FLAG) != 0u);
    let legacyHeightfield =
        (countOneBits(faceMask) == 1u || terrainSurface) &&
        profile != MICRO_PROFILE_VOLUME_DENSITY;
    if (!legacyHeightfield) {
        return false;
    }
    if ((executionFlags & GEOMETRY_PROGRAM_ENABLED) == 0u) {
        return true;
    }
    return (executionFlags & GEOMETRY_PROGRAM_DOMAIN_VOLUME) == 0u &&
        (executionFlags & GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) == 0u;
}

#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
fn microPrepareClassifyContext(context : u32) {
    if (context == MICRO_CONTEXT_INTERIOR) {
        atomicAdd(&output.contextInterior, 1u);
    } else if (context == MICRO_CONTEXT_FLAT_SURFACE) {
        atomicAdd(&output.contextFlat, 1u);
    } else if ((context & MICRO_CONTEXT_SURFACE_FLAG) != 0u) {
        atomicAdd(&output.contextSlope, 1u);
    } else if (context >= MICRO_CONTEXT_WALL_NEGATIVE_X &&
               context <= MICRO_CONTEXT_WALL_POSITIVE_Z) {
        atomicAdd(&output.contextWall, 1u);
    } else {
        atomicAdd(&output.contextOther, 1u);
    }
}

fn microPrepareClassifyProjection(projectedMicroPixels : f32) {
    if (projectedMicroPixels < 0.5) {
        atomicAdd(&output.projectedLtHalfPixel, 1u);
    } else if (projectedMicroPixels < 1.0) {
        atomicAdd(&output.projectedHalfToOnePixel, 1u);
    } else if (projectedMicroPixels < 2.0) {
        atomicAdd(&output.projectedOneToTwoPixels, 1u);
    } else {
        atomicAdd(&output.projectedGeTwoPixels, 1u);
    }
}
#endif

fn microPrepareTransformCandidate(
    nodeIndex : u32, node : SparseVoxelBrickNode,
    parentIndex : u32, materialOffset : u32)
    -> SparseVoxelMicroPrepareCandidate {
    let invalid = SparseVoxelMicroPrepareCandidate(
        0u, SparseVoxelMicroBrickRecord(0u, 0u, 0u, 0u, vec4<u32>(0u)));
    let materialSlot = node.materialBase + materialOffset;
    if (materialSlot >= arrayLength(&brickMicroSurfaceData) ||
        (materialSlot >> 2u) >= arrayLength(&brickMaterialIndices)) {
        return invalid;
    }
    let paletteIndex =
        (brickMaterialIndices[materialSlot >> 2u] >>
         ((materialSlot & 3u) * 8u)) & 0xffu;
    let paletteAddress = node.paletteBase + paletteIndex;
    if (paletteAddress >= arrayLength(&brickPalette) ||
        (brickPalette[paletteAddress].packed.y &
         SPARSE_VOXEL_BRICK_MICRO_ENABLED) == 0u) {
        return invalid;
    }
    let descriptorWordAddress = paletteAddress >> 1u;
    if (descriptorWordAddress >= arrayLength(&brickPaletteDescriptorIds)) {
        return invalid;
    }
    let descriptorWord = brickPaletteDescriptorIds[descriptorWordAddress];
    let descriptorId = (descriptorWord >>
        ((paletteAddress & 1u) * 16u)) & 0xffffu;
    if (descriptorId == 0u || descriptorId >= arrayLength(&microMaterials)) {
        return invalid;
    }
    let descriptor = microMaterials[descriptorId];
    if (!microPrepareDescriptorValid(descriptor)) {
        return invalid;
    }
    let packedSurfaceData = brickMicroSurfaceData[materialSlot];
    let context = packedSurfaceData.y >> 24u;
    let terrainSurfaceData = vec2<u32>(
        packedSurfaceData.x, packedSurfaceData.y & 0x00ffffffu);
    let microContextData = (context << 24u) |
        microPrepareLegacySurfaceData(terrainSurfaceData);
    let geometryExecutionFlags =
        microPrepareGeometryExecutionFlags(descriptorId);
    if (!microPrepareHeightfield(
            geometryExecutionFlags, descriptor, context)) {
        return invalid;
    }

    let subdivision = descriptor.geometry & 0x0fu;
    let cell = vec3<u32>(
        parentIndex & 7u,
        (parentIndex >> 3u) & 7u,
        (parentIndex >> 6u) & 7u);
    let parentMinimum = node.minimumVoxelSize.xyz +
        vec3<f32>(cell) * node.minimumVoxelSize.w;
    let parentMaximum = parentMinimum + vec3<f32>(node.minimumVoxelSize.w);
    let cameraPosition = uniforms.cameraWorldPosition.xyz;
    // Keep side-entered parents in the residual brick. Their exact result is
    // still legacy-owned, and extracting them would pay for both a direct
    // proxy and an incumbent replay. The centre ray is stable for the tiny
    // parent projection; the replay remains the conservative edge fallback.
    let parentCentre = 0.5 * (parentMinimum + parentMaximum);
    let cameraDelta = cameraPosition - parentCentre;
    let cameraOffset = abs(cameraDelta);
    var centreEntryAxis = 0u;
    if (cameraOffset.y > cameraOffset.x) {
        centreEntryAxis = 1u;
    }
    if (cameraOffset.z > max(cameraOffset.x, cameraOffset.y)) {
        centreEntryAxis = 2u;
    }
    let profile = (descriptor.geometry >> 18u) & 0x07u;
    let terrainSurface = profile == MICRO_PROFILE_STAIRCASE &&
        (context == MICRO_CONTEXT_FLAT_SURFACE ||
         (context & MICRO_CONTEXT_SURFACE_FLAG) != 0u);
    let carveFace = select(
        firstTrailingBit((descriptor.geometry >> 12u) & 0x3fu),
        3u, terrainSurface);
    let centreEntryFace = centreEntryAxis * 2u +
        select(0u, 1u, cameraDelta[centreEntryAxis] > 0.0);
    if (centreEntryFace != carveFace) {
        return invalid;
    }
    let farthestOffset = max(
        abs(cameraPosition - parentMinimum),
        abs(cameraPosition - parentMaximum));
    let farthestDistance = max(length(farthestOffset), 1.0e-4);
    let outputPixelScale = max(
        f32(uniforms.debugFlags.w) / 65536.0, 1.0);
    let projectedNumerator =
        (node.minimumVoxelSize.w / f32(subdivision)) *
        min(abs(uniforms.focalPrincipal.x),
            abs(uniforms.focalPrincipal.y)) * outputPixelScale;
    let minimumProjectedMicroPixels =
        projectedNumerator /
        (farthestDistance *
         sparseVoxelMicroHandoffScale(uniforms.microTuning.x));
    let cutoff = bitcast<f32>(uniforms.debugFlags.y);
    if ((cutoff > 0.0 && farthestDistance > cutoff) ||
        microPrepareHandoffWeight(
            descriptor, minimumProjectedMicroPixels) < 1.0) {
        return invalid;
    }
    let geometryIREnabled =
        (geometryExecutionFlags & GEOMETRY_PROGRAM_ENABLED) != 0u;
    var occupiedNeighbourMask = 0u;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let stride = select(select(1u, 8u, axis == 1u), 64u, axis == 2u);
        if (cell[axis] != 0u) {
            let neighbourIndex = parentIndex - stride;
            let neighbourEntry = occupancy[node.occupancyIndex].cells[
                neighbourIndex >> 5u];
            if ((neighbourEntry.bits &
                 (1u << (neighbourIndex & 31u))) != 0u) {
                occupiedNeighbourMask |= 1u << (axis * 2u);
            }
        }
        if (cell[axis] + 1u < SPARSE_VOXEL_BRICK_SIDE) {
            let neighbourIndex = parentIndex + stride;
            let neighbourEntry = occupancy[node.occupancyIndex].cells[
                neighbourIndex >> 5u];
            if ((neighbourEntry.bits &
                 (1u << (neighbourIndex & 31u))) != 0u) {
                occupiedNeighbourMask |= 1u << (axis * 2u + 1u);
            }
        }
    }
    let parentDescriptorFlags = (parentIndex & 0x1ffu) |
        ((descriptorId & 0xffffu) << 9u) |
        select(0u, 1u << 25u, geometryIREnabled) |
        ((occupiedNeighbourMask & 0x3fu) << 26u);
    return SparseVoxelMicroPrepareCandidate(
        1u, SparseVoxelMicroBrickRecord(
            nodeIndex, parentDescriptorFlags, paletteAddress,
            microContextData, vec4<u32>(0u)));
}

@compute @workgroup_size(1, 1, 1)
fn resetMicroPrepareCounters() {
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
    atomicStore(&output.visibleBricks, 0u);
    atomicStore(&output.occupiedParents, 0u);
    atomicStore(&output.microParents, 0u);
    atomicStore(&output.validDescriptors, 0u);
    atomicStore(&output.invalidDescriptors, 0u);
    atomicStore(&output.contextInterior, 0u);
    atomicStore(&output.contextFlat, 0u);
    atomicStore(&output.contextSlope, 0u);
    atomicStore(&output.contextWall, 0u);
    atomicStore(&output.contextOther, 0u);
    atomicStore(&output.projectedLtHalfPixel, 0u);
    atomicStore(&output.projectedHalfToOnePixel, 0u);
    atomicStore(&output.projectedOneToTwoPixels, 0u);
    atomicStore(&output.projectedGeTwoPixels, 0u);
    atomicStore(&output.handoffEligible, 0u);
    atomicStore(&output.exactNearEligible, 0u);
    atomicStore(&output.heightfieldRecords, 0u);
    atomicStore(&output.heightfieldMicrocells, 0u);
    atomicStore(&output.nonHeightfieldExact, 0u);
    atomicStore(&output.heightfieldQuadsLow, 0u);
    atomicStore(&output.heightfieldQuadsHigh, 0u);
    atomicStore(&output.heightfieldBytesLow, 0u);
    atomicStore(&output.heightfieldBytesHigh, 0u);
    atomicStore(&output.reserved0, 0u);
#endif
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
    atomicStore(&prepareState.recordCount, 0u);
    atomicStore(&prepareState.overflowBricks, 0u);
    atomicStore(&prepareState.extractedParents, 0u);
    atomicStore(&prepareState.residualParents, 0u);
    atomicStore(&prepareState.outputBytesLow, 0u);
    atomicStore(&prepareState.outputBytesHigh, 0u);
    atomicStore(&prepareState.cachedRecords, 0u);
    atomicStore(&prepareState.reserved1, 0u);
#endif
    prepareDispatch.x = min(
        drawArgs.instanceCount, arrayLength(&visibleBricks));
    prepareDispatch.y = 1u;
    prepareDispatch.z = 1u;
    microDrawArgs.vertexCount = 18u;
    atomicStore(&microDrawArgs.instanceCount, 0u);
    microDrawArgs.firstVertex = 0u;
    microDrawArgs.firstInstance = 0u;
}

// Convert a visible brick into two disjoint products: compact exact-near
// heightfield parent records and the occupancy that remains for the legacy
// brick DDA. A whole workgroup owns a brick. Reservation is all-or-nothing, so
// capacity pressure can never clear a parent that was not emitted.
@compute @workgroup_size(MICRO_PREPARE_WORKGROUP_SIZE, 1, 1)
fn transformMicroPrepareParents(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_index) lane : u32) {
    let visibleIndex = workgroupId.x;
    let visibleCount = min(drawArgs.instanceCount, arrayLength(&visibleBricks));
    if (visibleIndex >= visibleCount || visibleIndex >= arrayLength(&residuals)) {
        return;
    }
    let nodeIndex = visibleBricks[visibleIndex];
    if (nodeIndex >= arrayLength(&nodes)) {
        return;
    }
    let node = nodes[nodeIndex];
    if (node.occupancyIndex >= arrayLength(&occupancy)) {
        return;
    }

    if (lane < SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS) {
        atomicStore(&transformResidual[lane],
            occupancy[node.occupancyIndex].cells[lane].bits);
    }
    if (lane == 0u) {
        atomicStore(&transformRecordCount, 0u);
        transformRecordBase = 0u;
        transformAccepted = 0u;
        atomicStore(&transformCoarseLow, 0u);
        atomicStore(&transformCoarseHigh, 0u);
    }
    workgroupBarrier();

    // The incumbent DDA suppresses the complete source-occupied run when the
    // camera starts inside a brick. Clearing one member of that run would make
    // residual traversal expose an internal boundary. Keep such a brick wholly
    // legacy until the specialized path carries an equivalent inside-run rule.
    let occupiedCells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let occupiedMinimum = node.minimumVoxelSize.xyz +
        occupiedCells.minimumCell * node.minimumVoxelSize.w;
    let occupiedMaximum = node.minimumVoxelSize.xyz +
        occupiedCells.maximumCell * node.minimumVoxelSize.w;
    let camera = uniforms.cameraWorldPosition.xyz;
    let cameraInside = all(camera >= occupiedMinimum) &&
        all(camera <= occupiedMaximum);

    if (!cameraInside) {
        for (var parentIndex = lane;
             parentIndex < SPARSE_VOXEL_BRICK_VOXEL_COUNT;
             parentIndex += MICRO_PREPARE_WORKGROUP_SIZE) {
            let entry = occupancy[node.occupancyIndex].cells[parentIndex >> 5u];
            let bit = 1u << (parentIndex & 31u);
            if ((entry.bits & bit) == 0u) {
                continue;
            }
            let materialOffset = entry.prefix + countOneBits(
                entry.bits & (bit - 1u));
            let candidate = microPrepareTransformCandidate(
                nodeIndex, node, parentIndex, materialOffset);
            if (candidate.valid == 0u) {
                continue;
            }
            let slot = atomicAdd(&transformRecordCount, 1u);
            transformRecordNode[slot] = candidate.record.nodeIndex;
            transformRecordParentDescriptorFlags[slot] =
                candidate.record.parentDescriptorFlags;
            transformRecordPalette[slot] = candidate.record.paletteAddress;
            transformRecordMicroContextData[slot] =
                candidate.record.microContextData;
        }
    }
    workgroupBarrier();

    let localCount = atomicLoad(&transformRecordCount);
    if (lane == 0u && localCount != 0u) {
        // The indirect instance count is also the compact allocator. Shipping
        // therefore needs one CAS per candidate-bearing brick and no separate
        // diagnostic state buffer or mirrored atomic counter.
        var current = atomicLoad(&microDrawArgs.instanceCount);
        loop {
            if (current > arrayLength(&records) ||
                localCount > arrayLength(&records) - current) {
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
                atomicAdd(&prepareState.overflowBricks, 1u);
#endif
                break;
            }
            let exchanged = atomicCompareExchangeWeak(
                &microDrawArgs.instanceCount,
                current, current + localCount);
            if (exchanged.exchanged) {
                transformRecordBase = current;
                transformAccepted = 1u;
                break;
            }
            current = exchanged.old_value;
        }
    }
    workgroupBarrier();

    // No-record and whole-brick overflow cases are byte-for-byte the source
    // occupancy. Copy its already-built coarse mask instead of reducing all
    // 512 fine bits again. This is the common path after camera-facing
    // eligibility, and overflow must remain an exact legacy fallback.
    if (transformAccepted == 0u) {
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
        if (lane < SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS) {
            atomicAdd(
                &prepareState.residualParents,
                countOneBits(atomicLoad(&transformResidual[lane])));
        }
#endif
        // Publish one word instead of eighteen. The mask and both coarse words
        // are already exactly the source occupancy here, so restating them only
        // moves bytes the draw can read from the immutable node.
        if (lane == 0u) {
            residuals[visibleIndex].passthrough = 1u;
        }
    }

    if (transformAccepted != 0u) {
        for (var slot = lane; slot < localCount;
             slot += MICRO_PREPARE_WORKGROUP_SIZE) {
            let parentDescriptorFlags =
                transformRecordParentDescriptorFlags[slot];
            let parentIndex = parentDescriptorFlags & 0x1ffu;
            // Built here rather than carried through workgroup memory: a
            // per-parent 16-byte cache for all 512 slots would add 8 KiB of
            // shared storage to a kernel that already holds 8 KiB.
            let recordContextData = transformRecordMicroContextData[slot];
            let recordColumns = microPrepareColumnCache(
                (parentDescriptorFlags >> 9u) & 0xffffu,
                (parentDescriptorFlags & (1u << 25u)) != 0u,
                recordContextData);
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
            if (sparseVoxelExactMicroColumnCacheValid(recordColumns)) {
                atomicAdd(&prepareState.cachedRecords, 1u);
            }
#endif
            records[transformRecordBase + slot] =
                SparseVoxelMicroBrickRecord(
                    transformRecordNode[slot], parentDescriptorFlags,
                    transformRecordPalette[slot],
                    recordContextData,
                    recordColumns);
            atomicAnd(&transformResidual[parentIndex >> 5u],
                ~(1u << (parentIndex & 31u)));
        }
        if (lane == 0u) {
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
            atomicAdd(&prepareState.recordCount, localCount);
            atomicAdd(&prepareState.extractedParents, localCount);
            let outputBytes = localCount * MICRO_PREPARE_RECORD_BYTES;
            let previousBytes = atomicAdd(
                &prepareState.outputBytesLow, outputBytes);
            if (previousBytes > 0xffffffffu - outputBytes) {
                atomicAdd(&prepareState.outputBytesHigh, 1u);
            }
#endif
        }
    }
    workgroupBarrier();

    if (transformAccepted != 0u &&
        lane < SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS) {
        transformResidualSnapshot[lane] =
            atomicLoad(&transformResidual[lane]);
    }
    workgroupBarrier();

    // Rebuild the register-held 4^3 mask from the residual fine bits. Each
    // lane owns one coarse cell and tests its eight children from workgroup
    // memory; no global occupancy fetch is needed by this reduction.
    if (transformAccepted != 0u && lane < 64u) {
        let coarseCell = vec3<u32>(
            lane & 3u, (lane >> 2u) & 3u, (lane >> 4u) & 3u);
        let base = coarseCell * 2u;
        var occupied = false;
        for (var child = 0u; child < 8u; child += 1u) {
            let cell = base + vec3<u32>(
                child & 1u, (child >> 1u) & 1u, (child >> 2u) & 1u);
            let parentIndex = brickCellIndex(cell);
            occupied = occupied ||
                (transformResidualSnapshot[parentIndex >> 5u] &
                 (1u << (parentIndex & 31u))) != 0u;
        }
        if (occupied) {
            if (lane < 32u) {
                atomicOr(&transformCoarseLow, 1u << lane);
            } else {
                atomicOr(&transformCoarseHigh, 1u << (lane - 32u));
            }
        }
    }
    workgroupBarrier();
    if (transformAccepted != 0u &&
        lane < SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS) {
        let residualWord = transformResidualSnapshot[lane];
        residuals[visibleIndex].bits[lane] = residualWord;
#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
        atomicAdd(
            &prepareState.residualParents, countOneBits(residualWord));
#endif
    }
    if (transformAccepted != 0u && lane == 0u) {
        residuals[visibleIndex].coarseLow =
            atomicLoad(&transformCoarseLow);
        residuals[visibleIndex].coarseHigh =
            atomicLoad(&transformCoarseHigh);
        residuals[visibleIndex].passthrough = 0u;
    }
}

#ifdef SPARSE_VOXEL_MICRO_PREPARE_DIAGNOSTICS
@compute @workgroup_size(MICRO_PREPARE_WORKGROUP_SIZE, 1, 1)
fn scanMicroPrepareParents(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let visibleIndex = globalId.x;
    let visibleCount = min(drawArgs.instanceCount, arrayLength(&visibleBricks));
    if (visibleIndex >= visibleCount) {
        return;
    }
    atomicAdd(&output.visibleBricks, 1u);

    let nodeIndex = visibleBricks[visibleIndex];
    if (nodeIndex >= arrayLength(&nodes)) {
        return;
    }
    let node = nodes[nodeIndex];
    if (node.occupancyIndex >= arrayLength(&occupancy)) {
        return;
    }

    for (var wordIndex = 0u;
         wordIndex < SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS;
         wordIndex += 1u) {
        let entry = occupancy[node.occupancyIndex].cells[wordIndex];
        var remaining = entry.bits;
        while (remaining != 0u) {
            let bitInWord = firstTrailingBit(remaining);
            remaining &= remaining - 1u;
            let parentIndex = wordIndex * 32u + bitInWord;
            atomicAdd(&output.occupiedParents, 1u);

            let materialOffset = entry.prefix + countOneBits(
                entry.bits & ((1u << bitInWord) - 1u));
            let materialSlot = node.materialBase + materialOffset;
            if (materialSlot >= arrayLength(&brickMicroSurfaceData) ||
                (materialSlot >> 2u) >=
                    arrayLength(&brickMaterialIndices)) {
                continue;
            }
            let paletteIndex =
                (brickMaterialIndices[materialSlot >> 2u] >>
                 ((materialSlot & 3u) * 8u)) & 0xffu;
            let paletteAddress = node.paletteBase + paletteIndex;
            if (paletteAddress >= arrayLength(&brickPalette)) {
                continue;
            }
            let packedMaterial = brickPalette[paletteAddress];
            if ((packedMaterial.packed.y &
                 SPARSE_VOXEL_BRICK_MICRO_ENABLED) == 0u) {
                continue;
            }
            atomicAdd(&output.microParents, 1u);

            let descriptorWordAddress = paletteAddress >> 1u;
            if (descriptorWordAddress >=
                arrayLength(&brickPaletteDescriptorIds)) {
                atomicAdd(&output.invalidDescriptors, 1u);
                continue;
            }
            let descriptorWord =
                brickPaletteDescriptorIds[descriptorWordAddress];
            let descriptorId = (descriptorWord >>
                ((paletteAddress & 1u) * 16u)) & 0xffffu;
            if (descriptorId == 0u ||
                descriptorId >= arrayLength(&microMaterials)) {
                atomicAdd(&output.invalidDescriptors, 1u);
                continue;
            }
            let descriptor = microMaterials[descriptorId];
            if (!microPrepareDescriptorValid(descriptor)) {
                atomicAdd(&output.invalidDescriptors, 1u);
                continue;
            }
            atomicAdd(&output.validDescriptors, 1u);

            let context = brickMicroSurfaceData[materialSlot].y >> 24u;
            microPrepareClassifyContext(context);

            let subdivision = descriptor.geometry & 0x0fu;
            let cell = vec3<u32>(
                parentIndex & 7u,
                (parentIndex >> 3u) & 7u,
                (parentIndex >> 6u) & 7u);
            let parentMinimum = node.minimumVoxelSize.xyz +
                vec3<f32>(cell) * node.minimumVoxelSize.w;
            let parentMaximum = parentMinimum +
                vec3<f32>(node.minimumVoxelSize.w);
            let cameraPosition = uniforms.cameraWorldPosition.xyz;
            // The fragment path uses per-ray fineT. A unique-parent scan has no
            // ray, so bracket every possible entry distance with the nearest
            // and farthest points on the AABB. Nearest gives maximum projected
            // size (potential handoff); farthest gives minimum projected size
            // and is the only safe basis for guaranteed-exact extraction.
            let nearest = clamp(cameraPosition, parentMinimum, parentMaximum);
            let nearestDistance = max(
                length(cameraPosition - nearest), 1.0e-4);
            let farthestOffset = max(
                abs(cameraPosition - parentMinimum),
                abs(cameraPosition - parentMaximum));
            let farthestDistance = max(length(farthestOffset), 1.0e-4);
            let outputPixelScale = max(
                f32(uniforms.debugFlags.w) / 65536.0, 1.0);
            let projectedNumerator =
                (node.minimumVoxelSize.w / f32(subdivision)) *
                min(abs(uniforms.focalPrincipal.x),
                    abs(uniforms.focalPrincipal.y)) *
                outputPixelScale;
            let handoffScale =
                sparseVoxelMicroHandoffScale(uniforms.microTuning.x);
            let maximumProjectedMicroPixels =
                projectedNumerator / (nearestDistance * handoffScale);
            let minimumProjectedMicroPixels =
                projectedNumerator / (farthestDistance * handoffScale);
            microPrepareClassifyProjection(maximumProjectedMicroPixels);

            let maximumExactWeight = microPrepareHandoffWeight(
                descriptor, maximumProjectedMicroPixels);
            let minimumExactWeight = microPrepareHandoffWeight(
                descriptor, minimumProjectedMicroPixels);
            let cutoff = bitcast<f32>(uniforms.debugFlags.y);
            let potentiallyWithinDistance =
                cutoff <= 0.0 || nearestDistance <= cutoff;
            let guaranteedWithinDistance =
                cutoff <= 0.0 || farthestDistance <= cutoff;
            if (potentiallyWithinDistance && maximumExactWeight > 0.0) {
                atomicAdd(&output.handoffEligible, 1u);
            }
            // Count only parents whose minimum projected size is fully exact.
            // The stochastic transition band is deliberately excluded.
            if (!guaranteedWithinDistance || minimumExactWeight < 1.0) {
                continue;
            }
            atomicAdd(&output.exactNearEligible, 1u);

            let geometryExecutionFlags =
                microPrepareGeometryExecutionFlags(descriptorId);
            if (!microPrepareHeightfield(
                    geometryExecutionFlags, descriptor, context)) {
                atomicAdd(&output.nonHeightfieldExact, 1u);
                continue;
            }
            atomicAdd(&output.heightfieldRecords, 1u);
            let microcellCount = subdivision * subdivision;
            atomicAdd(&output.heightfieldMicrocells, microcellCount);
            // One top quad per column, at most one riser per internal edge,
            // and one boundary side per edge column: 3*N*N + 2*N. This is a
            // conservative allocation bound, not an emitted-geometry count.
            let quadUpperBound = 3u * microcellCount + 2u * subdivision;
            let previousQuads = atomicAdd(
                &output.heightfieldQuadsLow, quadUpperBound);
            if (previousQuads > 0xffffffffu - quadUpperBound) {
                atomicAdd(&output.heightfieldQuadsHigh, 1u);
            }
            let previousBytes = atomicAdd(
                &output.heightfieldBytesLow, MICRO_PREPARE_RECORD_BYTES);
            if (previousBytes >
                0xffffffffu - MICRO_PREPARE_RECORD_BYTES) {
                atomicAdd(&output.heightfieldBytesHigh, 1u);
            }
        }
    }
}
#endif
