// R9 H1: tiled compute raster, shadow result only.
//
// One 16x16 workgroup per nonempty screen tile, one thread per pixel. Each
// thread walks its tile's candidate list from the binner and keeps its own
// nearest hit in a register - because a thread owns a pixel outright, nothing
// is shared and there is no atomic in the inner loop at all. The packed 32-bit
// depth|payload key that the H0 census sized is only needed by a
// primitive-parallel variant; pixel ownership does not need it.
//
// This writes to its own comparison buffers and is never presented. H1's whole
// purpose is to prove the compute path reproduces the hardware raster before
// H3 routes any real work to it, so the output exists to be diffed.
//
// Scope: the brick DDA including exact microgeometry carving, so parity is
// expected against the shipping configuration rather than only against one with
// terrain micro materials disabled.
//
// The exact-micro stack is reached through shared, binding-free includes: the
// geometry IR, the micro geometry math, and the brick-side helpers factored out
// of the raster fragment. Every input arrives as a parameter, so calling them
// here inherits none of the fragment's binding layout, and both paths evaluate
// one floating-point expression tree rather than two copies of the arithmetic.
#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_brick_common.wgsl"
#include "voxel_geometry_ir.wgsl"
#include "voxel_geometry_field_cache.wgsl"
#include "sparse_voxel_micro_geometry.wgsl"
#include "sparse_voxel_brick_micro_common.wgsl"

const TILE_SIZE : u32 = 16u;
// Chunked so the candidate loads are coalesced across the workgroup and each
// thread reads node data out of shared memory rather than re-fetching it.
const TILE_CHUNK : u32 = 64u;

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBrickDrawUniforms;
@group(0) @binding(1) var<storage, read> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read> occupancy : array<SparseVoxelBrickOccupancy>;
@group(0) @binding(3) var<storage, read> tileCounts : array<u32>;
// Holds each tile's exclusive END after the scatter; the range is
// [end - count, end).
@group(0) @binding(4) var<storage, read> tileOffsets : array<u32>;
@group(0) @binding(5) var<storage, read> tileCandidates : array<u32>;
// Linear hit distance per pixel, or a negative value where nothing was hit.
@group(0) @binding(6) var<storage, read_write> computeDistance : array<f32>;
@group(0) @binding(7) var<storage, read_write> computeCoverage : array<u32>;
// The hardware raster's resolved products, for the H1 diff. `coverage` is 0/1
// and `linearDepth` is the same linear ray distance this kernel produces.
// Read-only for the H1 comparison, but the H3 merge publishes into them: they
// are the resolver's shared coverage and linear depth, which is what every
// downstream consumer actually reads.
@group(0) @binding(8) var<storage, read_write> hardwareCoverage : array<u32>;
@group(0) @binding(9) var<storage, read_write> hardwareDistance : array<f32>;
@group(0) @binding(10) var<storage, read_write> parity : SparseVoxelBrickTileParity;
// The winning brick and cell per pixel, so the merge can rebuild the same
// resolved surface record the hardware resolver publishes. Without these a
// merged pixel would carry a depth with no material and shade as garbage.
@group(0) @binding(11) var<storage, read_write> computeNode : array<u32>;
@group(0) @binding(12) var<storage, read_write> computeCell : array<u32>;
@group(0) @binding(13) var<storage, read> brickMaterialIndices : array<u32>;
@group(0) @binding(14) var<storage, read> brickPalette : array<SparseVoxelBrickMaterial>;
@group(0) @binding(15) var<storage, read_write> resolvedSurfaces : array<ResolvedVoxelSurface>;
// Entry axis of the winning hit, packed with its sign at merge time.
@group(0) @binding(16) var<storage, read_write> computeAxis : array<u32>;
// Per-voxel terrain context, which rides in the bottom-albedo word's high byte.
@group(0) @binding(17) var<storage, read> brickMicroSurfaceData : array<vec2<u32>>;
// The two arrays the exact-micro path needs: the per-palette-entry descriptor
// id, and the descriptors themselves. Same contents the raster fragment binds.
@group(0) @binding(18) var<storage, read> brickPaletteDescriptorIds : array<u32>;
@group(0) @binding(19) var<storage, read> microMaterials : array<SparseVoxelMicroMaterial>;
// Per-pixel exact-micro record for the winning hit, so the merge can publish the
// same microPayload and materialSampleAddress the fragment does. Without these a
// routed micro pixel would carry the parent's material and lose its carving.
@group(0) @binding(20) var<storage, read_write> computeMicroPayload : array<u32>;
@group(0) @binding(21) var<storage, read_write> computeMicroAddress : array<vec4<u32>>;
// Surface identity of the winning hit. A micro hit's identity is the parent's
// hash mixed with the microcell, so it cannot be rebuilt from the cell alone.
@group(0) @binding(22) var<storage, read_write> computeIdentity : array<u32>;
// Terrain context byte of the winning hit, which rides in the bottom-albedo
// word. A carved hit reports the microcell's context, not the parent's.
@group(0) @binding(23) var<storage, read_write> computeContext : array<u32>;
// The resolver's own micro products. A merged pixel has to publish into these
// too: the material pass reads them, not the resolved surface record.
@group(0) @binding(24) var<storage, read_write> microPayloads : array<u32>;
@group(0) @binding(25) var<storage, read_write> materialSampleAddresses : array<VoxelMaterialSampleAddress>;
// Mirrors the resolver's flag for "this surface has an addressable microsurface".
const SPARSE_VOXEL_BRICK_EXACT_MICRO_MATERIAL : u32 = 1u << 31u;
// The geometry IR and field-cache includes are binding-free in the sense that
// every value reaches them as a parameter, but they do read these four arrays by
// name, so the includer has to declare them. Same contents and same order the
// raster fragment binds, which is what keeps a programmable-geometry material
// evaluating identically on both paths.
@group(0) @binding(26) var<storage, read> voxelGeometryPrograms : array<VoxelGeometryProgram>;
@group(0) @binding(27) var<storage, read> voxelGeometryInstructions : array<VoxelGeometryInstruction>;
@group(0) @binding(28) var<storage, read> voxelGeometryParameters : array<VoxelGeometryParameters>;
@group(0) @binding(29) var<storage, read> voxelMaterialFieldDefaults : array<VoxelMaterialFieldDefaults>;
@group(0) @binding(30) var<storage, read> voxelGeometryFieldCache :
    array<SparseVoxelGeometryFieldCacheRecord>;
// Bottom albedo mask and context shift, mirroring the raster fragment.
const RESOLVED_BOTTOM_ALBEDO_MASK : u32 = 0x00ffffffu;
const RESOLVED_CONTEXT_SHIFT : u32 = 24u;

struct SparseVoxelBrickTileParity {
    comparedPixels : atomic<u32>,
    // Both paths agree the pixel is covered.
    bothCovered : atomic<u32>,
    // Hardware found geometry the compute path missed, and the reverse. These
    // are the numbers H1 has to drive to zero.
    hardwareOnly : atomic<u32>,
    computeOnly : atomic<u32>,
    // Covered by both and agreeing on distance to within the tolerance.
    distanceMatches : atomic<u32>,
    // Largest absolute distance disagreement, in micrometres so it fits an
    // integer atomicMax.
    maximumDeltaMicrometres : atomic<u32>,
    // Direction of the disagreement. Compute finding a *nearer* surface means
    // it accepts geometry the hardware rejects; finding a *farther* one means
    // it misses a nearer hit. These separate two very different bugs.
    computeNearer : atomic<u32>,
    hardwareNearer : atomic<u32>,
    // Identity of the worst mismatching pixel, captured in a second pass.
    worstPixel : atomic<u32>,
    worstHardwareBits : atomic<u32>,
    worstComputeBits : atomic<u32>,
    // For the worst pixel: how many of its tile's candidates have an occupied
    // box the ray enters at or before the hardware's answer. Zero means the
    // winning brick was never binned, which indicts the binner. Nonzero means
    // it was there and traceBrick rejected it, which indicts the traversal.
    worstCandidatesSpanning : atomic<u32>,
    worstCandidateCount : atomic<u32>,
    worstComputeNode : atomic<u32>,
    // Sizing the last open H1 question: would exact microgeometry be cheaper in
    // compute than in the fragment? The fragment re-derives a parent's
    // microgeometry once per overdrawing fragment; the compute walk visits a
    // candidate at most once per pixel. These two counters are the compute
    // path's analogue of microCounters.eligibleParentHits, so the ratio between
    // them is the redundancy a compute micro path would actually remove.
    //
    // occupiedCellHits counts every accepted occupied cell across all
    // candidates; microEligibleCellHits counts the subset whose material has
    // MICRO_ENABLED. Counting is per-thread and flushed once per pixel, so the
    // instrument costs two atomics per pixel rather than two per candidate.
    occupiedCellHits : atomic<u32>,
    microEligibleCellHits : atomic<u32>,
    // Micro record parity, over the pixels where both paths already agree on
    // the surface. Distance agreement alone does not prove the carved record
    // matches: a pixel can stop at the right microsurface and still publish the
    // wrong payload, identity or canonical address, which shades as the parent.
    // Compare runs before Merge, so the hardware buffers still hold the
    // resolver's own values when this reads them.
    microComparedPixels : atomic<u32>,
    microPayloadMismatches : atomic<u32>,
    microAddressMismatches : atomic<u32>,
    microIdentityMismatches : atomic<u32>,
    // Direction of the payload disagreement. "Hardware only" means the compute
    // walk published no micro record where the fragment did, which is a missing
    // path; "compute only" is the reverse; "both differ" is a wrong record
    // rather than an absent one. These separate three different bugs.
    microPayloadHardwareOnly : atomic<u32>,
    microPayloadComputeOnly : atomic<u32>,
    microPayloadBothDiffer : atomic<u32>,
    // Exact traces the compute walk performs, against the fragment's
    // exactTraceInvocations. This is the ratio that actually predicts cost.
    computeExactTraces : atomic<u32>,
}

var<workgroup> chunkNodes : array<u32, TILE_CHUNK>;
var<workgroup> gapPixelCount : atomic<u32>;

fn tileGridWidth() -> u32 {
    return (max(uniforms.viewport.x, 1u) + TILE_SIZE - 1u) / TILE_SIZE;
}

fn hardwareSurroundsPixel(pixel : vec2<u32>) -> bool {
    if (pixel.x == 0u || pixel.y == 0u ||
        pixel.x + 1u >= uniforms.viewport.x ||
        pixel.y + 1u >= uniforms.viewport.y) {
        return false;
    }
    for (var offsetY = -1; offsetY <= 1; offsetY += 1) {
        for (var offsetX = -1; offsetX <= 1; offsetX += 1) {
            if (offsetX == 0 && offsetY == 0) {
                continue;
            }
            let neighbour = vec2<u32>(vec2<i32>(pixel) +
                vec2<i32>(offsetX, offsetY));
            let index = neighbour.y * uniforms.viewport.x + neighbour.x;
            if (index >= arrayLength(&hardwareCoverage) ||
                hardwareCoverage[index] == 0u) {
                return false;
            }
        }
    }
    return true;
}

fn cameraRayDirection(pixel : vec2<f32>) -> vec3<f32> {
    // Pixel centres, matching the rasterizer's fragment position convention.
    let viewX = (pixel.x + 0.5 - uniforms.focalPrincipal.z) /
        uniforms.focalPrincipal.x;
    let viewY = (pixel.y + 0.5 - uniforms.focalPrincipal.w) /
        uniforms.focalPrincipal.y;
    return normalize(
        uniforms.cameraToWorld[0].xyz * viewX +
        uniforms.cameraToWorld[1].xyz * viewY +
        uniforms.cameraToWorld[2].xyz);
}

// Never returns a component of exactly zero. The DDA derives its step direction
// from the sign of this, and sign(0.0) is 0 - an axis that never advances, so
// the walk stalls on that axis, burns its iteration budget without moving, and
// reports a miss. Axis-aligned view rays are common, which is exactly when it
// bites.
fn safeDirection(direction : vec3<f32>) -> vec3<f32> {
    let parallel = abs(direction) < vec3<f32>(1.0e-20);
    let parallelDirection = select(
        vec3<f32>(-1.0e-20), vec3<f32>(1.0e-20),
        direction >= vec3<f32>(0.0));
    return select(direction, parallelDirection, parallel);
}

fn safeInverseDirection(direction : vec3<f32>) -> vec3<f32> {
    return vec3<f32>(1.0) / safeDirection(direction);
}

struct SlabHit {
    entry : f32,
    exit : f32,
    hit : bool,
    // Axis whose near plane the ray crossed last, i.e. the face it entered
    // through. The hardware fragment packs this into the surface tag and the
    // G-buffer turns it into a geometric normal, so a routed pixel without it
    // shades with the wrong facing.
    axis : u32,
}

fn intersectBox(minimum : vec3<f32>, maximum : vec3<f32>, origin : vec3<f32>,
    inverseDirection : vec3<f32>) -> SlabHit {
    let slabA = (minimum - origin) * inverseDirection;
    let slabB = (maximum - origin) * inverseDirection;
    let nearT = min(slabA, slabB);
    let farT = max(slabA, slabB);
    let planar = max(max(nearT.x, nearT.y), nearT.z);
    let entry = max(planar, 0.0);
    let exit = min(min(farT.x, farT.y), farT.z);
    var axis = 0u;
    if (nearT.y >= nearT.x && nearT.y >= nearT.z) {
        axis = 1u;
    } else if (nearT.z >= nearT.x && nearT.z >= nearT.y) {
        axis = 2u;
    }
    return SlabHit(entry, exit, exit >= entry, axis);
}

fn occupancyBit(occupancyIndex : u32, cell : vec3<u32>) -> bool {
    let index = brickCellIndex(cell);
    let word = occupancy[occupancyIndex].cells[index >> 5u].bits;
    return (word & (1u << (index & 31u))) != 0u;
}

// First occupied cell along the ray inside one brick, as a linear distance.
// Returns a negative value when the brick contributes nothing.
struct BrickTraceResult {
    distance : f32,
    cellIndex : u32,
    axis : u32,
    // Exact-micro record of the hit, mirroring what the fragment carries out of
    // its walk. `microValid` distinguishes a carved hit from a plain parent hit;
    // `microFallback` is the sub-pixel handoff's retained parent, which still
    // publishes a payload so the material pass can shade it as micro.
    microValid : bool,
    microFallback : bool,
    microDescriptorId : u32,
    microCell : vec3<u32>,
    microCarveFace : u32,
    microSubdivision : u32,
    microHeightfield : bool,
    microTerrainSlope : bool,
    microContext : u32,
    microParentCell : vec3<u32>,
    usedExact : bool,
}

fn brickTraceMiss() -> BrickTraceResult {
    return BrickTraceResult(
        -1.0, 0u, 0u, false, false, 0u, vec3<u32>(0u), 0u, 0u, false, false,
        0u, vec3<u32>(0u), false);
}

// Outcome of testing one occupied cell against its exact microgeometry.
// `accepted == false` means the carve removed this cell and the DDA must keep
// walking - that continuation is the whole reason micro cannot be applied as a
// post-process on the winning cell.
struct BrickMicroDecision {
    accepted : bool,
    // Whether this cell took the expensive exact trace rather than the cheap
    // handoff fallback. Entry count alone overstates the redundancy a compute
    // walk removes, because the fragment's entries are mostly fallbacks.
    usedExact : bool,
    distance : f32,
    axis : u32,
    valid : bool,
    fallback : bool,
    descriptorId : u32,
    cell : vec3<u32>,
    carveFace : u32,
    subdivision : u32,
    heightfield : bool,
    terrainSlope : bool,
    context : u32,
}

// Mirrors the raster fragment's per-cell exact-micro decision. Kept in one
// function so the ported control flow can be read against the original: load
// descriptor, decide exact vs fallback by projected microvoxel size, then either
// carve or retain the parent.
fn brickDecideMicro(
    node : SparseVoxelBrickNode, parentCell : vec3<u32>, materialSlot : u32,
    paletteAddress : u32, material : SparseVoxelBrickMaterial,
    origin : vec3<f32>, direction : vec3<f32>,
    cellEntryT : f32, cellExitT : f32, entryAxis : u32) -> BrickMicroDecision {
    var decision = BrickMicroDecision(
        true, false, cellEntryT, entryAxis, false, false, 0u, vec3<u32>(0u), 0u,
        0u, false, false, 0u);
    var microContext = 0u;
    var microSurfaceData = 0u;
    var terrainSurfaceData = vec2<u32>(0u);
    if (materialSlot < arrayLength(&brickMicroSurfaceData)) {
        let packedContext = brickMicroSurfaceData[materialSlot];
        microContext = packedContext.y >> 24u;
        terrainSurfaceData = vec2<u32>(
            packedContext.x, packedContext.y & 0x00ffffffu);
        microSurfaceData = brickTerrainPatchLegacyData(terrainSurfaceData);
    }
    decision.context = microContext;
    let terrainPatch = !brickTerrainPatchIsEmpty(terrainSurfaceData) &&
        (microContext == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE ||
         (microContext & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u);
    if (terrainPatch) {
        let hit = brickTraceTerrainSurfacePatch(
            node.minimumVoxelSize.xyz, parentCell,
            node.minimumVoxelSize.w, origin, direction,
            cellEntryT, cellExitT, terrainSurfaceData);
        decision.accepted = hit.valid;
        decision.distance = hit.distance;
        decision.axis = select(
            1u, entryAxis,
            abs(hit.distance - cellEntryT) <=
                node.minimumVoxelSize.w * 2.0e-4);
        decision.terrainSlope = true;
        return decision;
    }
    if ((material.packed.y & SPARSE_VOXEL_BRICK_MICRO_ENABLED) == 0u) {
        return decision;
    }
    let descriptorWordAddress = paletteAddress >> 1u;
    if (descriptorWordAddress >= arrayLength(&brickPaletteDescriptorIds)) {
        return decision;
    }
    let descriptorWord = brickPaletteDescriptorIds[descriptorWordAddress];
    let descriptorId =
        (descriptorWord >> ((paletteAddress & 1u) * 16u)) & 0xffffu;
    if (descriptorId == 0u || descriptorId >= arrayLength(&microMaterials)) {
        return decision;
    }
    let descriptor = microMaterials[descriptorId];
    let subdivision = descriptor.geometry & 0x0fu;
    let maximumDepthFixed = (descriptor.geometry >> 4u) & 0xffu;
    let faceMask = (descriptor.geometry >> 12u) & 0x3fu;
    let profile = (descriptor.geometry >> 18u) & 0x07u;
    let descriptorValid = subdivision >= 2u &&
        subdivision <= SPARSE_VOXEL_EXACT_MICRO_MAX_SUBDIVISION &&
        maximumDepthFixed != 0u &&
        maximumDepthFixed <= subdivision * 16u &&
        faceMask != 0u &&
        (profile == SPARSE_VOXEL_EXACT_MICRO_CONSTANT ||
         profile == SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS ||
         profile == SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY ||
         profile == SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS ||
         profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE);
    if (!descriptorValid) {
        return decision;
    }
    let voxelSize = node.minimumVoxelSize.w;
    let brickMinimum = node.minimumVoxelSize.xyz;
    let terrainStaircase = profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE;
    let terrainSlope = terrainStaircase &&
        (microContext & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
    let parentKey = brickMicroCanonicalParentKey(
        node.canonicalGridOriginScale, vec3<i32>(parentCell));

    // Same projected-size handoff the fragment applies, including the output
    // pixel scale and the --micro-handoff-scale divisor, so a cell retires exact
    // carving at exactly the same distance on both paths.
    let outputPixelScale = max(f32(uniforms.debugFlags.w) / 65536.0, 1.0);
    let projectedMicroPixels =
        (voxelSize / f32(subdivision)) *
        min(abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y)) *
        outputPixelScale /
        (max(abs(cellEntryT), 1.0e-4) *
         sparseVoxelMicroHandoffScale(uniforms.microTuning.x));
    let exactWeight =
        sparseVoxelExactMicroHandoffWeight(descriptor, projectedMicroPixels);
    let exactSelector = sparseVoxelExactMicroSelector(
        descriptor, descriptorId, parentKey, 0x51ed270bu);
    let useExact = exactWeight >= 1.0 ||
        (exactWeight > 0.0 && exactSelector < exactWeight);

    decision.descriptorId = descriptorId;
    decision.subdivision = subdivision;
    decision.terrainSlope = terrainSlope;
    // The fragment seeds the microcell to the parent centre before deciding
    // exact vs fallback, and a retained-parent fallback keeps that seed. It
    // reaches the canonical sample address, so leaving it zero addresses the
    // wrong microcolumn on every faded-out parent.
    decision.cell = vec3<u32>(subdivision / 2u);

    if (!useExact) {
        decision.fallback = true;
        // Terrain is an opaque heightfield: a stochastic whole-parent coverage
        // estimate turns its regular parent grid into dotted contour rows during
        // the sub-pixel handoff, so the solid parent is retained instead. The
        // expected-coverage selector stays correct for porous/clumped profiles.
        if (!terrainStaircase) {
            let geometryExecutionFlags =
                sparseVoxelExactMicroGeometryExecutionFlags(descriptorId);
            let expectedCoverage = sparseVoxelExactMicroExpectedCoverage(
                descriptor, descriptorId, geometryExecutionFlags, parentKey,
                microContext, microSurfaceData);
            let fallbackCoverage = mix(1.0, expectedCoverage, exactWeight);
            let coverageSelector = sparseVoxelExactMicroSelector(
                descriptor, descriptorId, parentKey, 0xa511e9b3u);
            if (coverageSelector >= fallbackCoverage) {
                decision.accepted = false;
            }
        }
        return decision;
    }

    decision.usedExact = true;
    let staircaseSurface =
        sparseVoxelExactMicroIsTerrainStaircaseSurface(profile, microContext);
    let staircaseInterior = terrainStaircase &&
        microContext == SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR;
    let geometryExecutionFlags =
        sparseVoxelExactMicroGeometryExecutionFlags(descriptorId);
    let geometryIREnabled =
        (geometryExecutionFlags & VOXEL_GEOMETRY_PROGRAM_ENABLED) != 0u;
    let geometryIRVolume = geometryIREnabled &&
        (geometryExecutionFlags & 2u) != 0u;
    let geometryIRFieldCached = geometryIREnabled &&
        (geometryExecutionFlags &
         VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u;
    let legacyHeightfield =
        (countOneBits(faceMask) == 1u || staircaseSurface) &&
        profile != SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY;
    let heightfield = select(
        legacyHeightfield,
        !geometryIRVolume && !geometryIRFieldCached,
        geometryIREnabled);
    let solidStaircase = !geometryIREnabled && staircaseInterior;

    let parentFace = entryAxis | select(0u, 4u, direction[entryAxis] < 0.0);
    let nearDistance = max(uniforms.cameraWorldPosition.w, 1.0e-4);

    var micro = sparseVoxelExactMicroMiss(cellExitT, descriptorId, 0u);
    if (solidStaircase || heightfield) {
        micro = brickTraceExactMicroHeightfield(
            descriptor, descriptorId, geometryExecutionFlags, parentKey,
            brickMinimum, parentCell, voxelSize, origin, direction,
            nearDistance, parentFace, microContext, microSurfaceData,
            solidStaircase, vec4<u32>(0u));
    } else {
        let parentMinimum = brickMinimum + vec3<f32>(parentCell) * voxelSize;
        let localOrigin = (origin - parentMinimum) / voxelSize;
        let localDirection = direction / voxelSize;
        micro = traceSparseVoxelExactMicroVolume(
            descriptor, descriptorId, geometryExecutionFlags, parentKey,
            localOrigin, localDirection, cellEntryT, cellExitT, parentFace,
            microContext, microSurfaceData, 0u);
    }
    decision.accepted = micro.valid;
    decision.valid = micro.valid;
    decision.fallback = false;
    if (micro.valid) {
        decision.distance = micro.distance;
        decision.axis = micro.face & 3u;
        decision.cell = micro.cell;
        // The carve face is the descriptor's own carved face, not the face the
        // ray happened to enter through: brickMicroPayload permutes the cell
        // into that face's frame, so taking micro.face here rotates every
        // payload into the wrong axis while leaving the unpermuted cell - and
        // therefore the surface identity - looking correct.
        decision.carveFace = select(
            firstTrailingBit(faceMask), 3u, staircaseSurface);
        // Deliberately not `|| solidStaircase`. A solid staircase interior is
        // traced by the heightfield entry point but is not published as a
        // heightfield payload, and the flag also selects the cell permutation.
        decision.heightfield = heightfield;
    }
    return decision;
}

// Resolves the cell's material the same way the merge and the raster fragment
// do - popcount of the occupancy bits before the cell, added to the word's
// running prefix - then delegates the carve decision. A cell whose material
// carries no descriptor falls through as an ordinary accepted parent hit.
fn brickMicroForCell(
    node : SparseVoxelBrickNode, unsignedCell : vec3<u32>, cellIndex : u32,
    origin : vec3<f32>, direction : vec3<f32>,
    cellEntryT : f32, cellExitT : f32, entryAxis : u32) -> BrickMicroDecision {
    let plain = BrickMicroDecision(
        true, false, cellEntryT, entryAxis, false, false, 0u, vec3<u32>(0u), 0u,
        0u, false, false, 0u);
    let entry = occupancy[node.occupancyIndex].cells[cellIndex >> 5u];
    let offset = entry.prefix +
        countOneBits(entry.bits & ((1u << (cellIndex & 31u)) - 1u));
    let materialSlot = node.materialBase + offset;
    if ((materialSlot >> 2u) >= arrayLength(&brickMaterialIndices)) {
        return plain;
    }
    let paletteIndex = (brickMaterialIndices[materialSlot >> 2u] >>
        ((materialSlot & 3u) * 8u)) & 0xffu;
    let paletteAddress = node.paletteBase + paletteIndex;
    if (paletteAddress >= arrayLength(&brickPalette)) {
        return plain;
    }
    return brickDecideMicro(
        node, unsignedCell, materialSlot, paletteAddress,
        brickPalette[paletteAddress], origin, direction,
        cellEntryT, cellExitT, entryAxis);
}

fn traceBrick(node : SparseVoxelBrickNode, origin : vec3<f32>,
    direction : vec3<f32>, inverseDirection : vec3<f32>,
    limit : f32) -> BrickTraceResult {
    let voxelSize = node.minimumVoxelSize.w;
    let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let boxMinimum = node.minimumVoxelSize.xyz + cells.minimumCell * voxelSize;
    let boxMaximum = node.minimumVoxelSize.xyz + cells.maximumCell * voxelSize;
    let box = intersectBox(boxMinimum, boxMaximum, origin, inverseDirection);
    if (!box.hit || box.entry >= limit) {
        return brickTraceMiss();
    }

    // Step the fine 8-cubed grid directly. The coarse mask is an optimisation
    // the hardware path uses to skip empty octants; correctness does not depend
    // on it, and H1 is about matching the result, not the traversal order.
    let brickMinimum = node.minimumVoxelSize.xyz;
    let start = origin + direction * (box.entry + voxelSize * 1.0e-4);
    var cell = clamp(
        vec3<i32>(floor((start - brickMinimum) / voxelSize)),
        vec3<i32>(0),
        vec3<i32>(i32(SPARSE_VOXEL_BRICK_SIDE) - 1));

    // From the safe direction, so no component can be zero. See safeDirection.
    let step = vec3<i32>(sign(safeDirection(direction)));
    let nextBoundary = brickMinimum +
        (vec3<f32>(cell) + max(vec3<f32>(step), vec3<f32>(0.0))) * voxelSize;
    var sideT = (nextBoundary - origin) * inverseDirection;
    let deltaT = abs(vec3<f32>(voxelSize) * inverseDirection);
    // Distance at which the ray entered the cell currently under test. The
    // fragment reports this accumulated parameter rather than re-deriving the
    // cell's entry with a fresh slab intersection, and the two disagree in the
    // low bits - accumulated addition against one division. Carrying it here
    // both matches the published contract and removes a slab test per cell.
    var entryT = box.entry;
    var entryAxis = box.axis;

    for (var iteration = 0u;
         iteration < SPARSE_VOXEL_BRICK_SIDE * 3u; iteration += 1u) {
        if (any(cell < vec3<i32>(0)) ||
            any(cell >= vec3<i32>(i32(SPARSE_VOXEL_BRICK_SIDE)))) {
            return brickTraceMiss();
        }
        let unsignedCell = vec3<u32>(cell);
        if (all(unsignedCell >= vec3<u32>(cells.minimumCell)) &&
            all(unsignedCell < vec3<u32>(cells.maximumCell)) &&
            occupancyBit(node.occupancyIndex, unsignedCell)) {
            if (entryT < limit) {
                let cellIndex = brickCellIndex(unsignedCell);
                // Exact microgeometry can carve this cell away entirely, in
                // which case the walk has to continue rather than report a hit.
                // That is why the decision lives inside the loop and not as a
                // post-process on the winning cell.
                let cellExitT = min(sideT.x, min(sideT.y, sideT.z));
                let micro = brickMicroForCell(
                    node, unsignedCell, cellIndex, origin, direction,
                    entryT, cellExitT, entryAxis);
                if (micro.accepted && micro.distance < limit) {
                    return BrickTraceResult(
                        micro.distance, cellIndex, micro.axis,
                        micro.valid, micro.fallback, micro.descriptorId,
                        micro.cell, micro.carveFace, micro.subdivision,
                        micro.heightfield, micro.terrainSlope, micro.context,
                        unsignedCell, micro.usedExact);
                }
            }
        }
        // Advance across the nearest axis boundary.
        // The axis crossed is the face the next cell is entered through, and
        // the crossing distance becomes that cell's entry parameter.
        if (sideT.x < sideT.y && sideT.x < sideT.z) {
            entryT = sideT.x;
            entryAxis = 0u;
            cell.x += step.x;
            sideT.x += deltaT.x;
        } else if (sideT.y < sideT.z) {
            entryT = sideT.y;
            entryAxis = 1u;
            cell.y += step.y;
            sideT.y += deltaT.y;
        } else {
            entryT = sideT.z;
            entryAxis = 2u;
            cell.z += step.z;
            sideT.z += deltaT.z;
        }
        // Deliberately no early exit on box.exit here. Checking it *after*
        // stepping returns a miss without ever testing the cell just entered,
        // which silently drops the last cell along the ray - measured as 9,492
        // pixels where the hardware found a nearer surface than this walk. The
        // cell-range test at the top of the loop and the iteration cap are the
        // termination conditions; a few extra empty-cell tests are the price.
    }
    return brickTraceMiss();
}

// Does this cell's material opt into exact microgeometry? Same addressing the
// merge and the raster fragment use: popcount of the occupancy bits before the
// cell, added to the word's running prefix.
//
// Measurement only. The compute walk does not carve microgeometry; this asks
// how often it *would* have to, so the cost of building that path can be sized
// against the fragment's count before it is built.
fn brickCellMicroEligible(node : SparseVoxelBrickNode, cellIndex : u32) -> bool {
    let entry = occupancy[node.occupancyIndex].cells[cellIndex >> 5u];
    if ((entry.bits & (1u << (cellIndex & 31u))) == 0u) {
        return false;
    }
    let offset = entry.prefix +
        countOneBits(entry.bits & ((1u << (cellIndex & 31u)) - 1u));
    let materialSlot = node.materialBase + offset;
    if ((materialSlot >> 2u) >= arrayLength(&brickMaterialIndices)) {
        return false;
    }
    let paletteIndex = (brickMaterialIndices[materialSlot >> 2u] >>
        ((materialSlot & 3u) * 8u)) & 0xffu;
    let paletteAddress = node.paletteBase + paletteIndex;
    if (paletteAddress >= arrayLength(&brickPalette)) {
        return false;
    }
    return (brickPalette[paletteAddress].packed.y &
        SPARSE_VOXEL_BRICK_MICRO_ENABLED) != 0u;
}

@compute @workgroup_size(16, 16, 1)
fn rasterBrickTile(@builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>,
    @builtin(local_invocation_index) localIndex : u32) {
    let gridWidth = tileGridWidth();
    let tile = workgroupId.x;
    let tileCoord = vec2<u32>(tile % gridWidth, tile / gridWidth);
    let pixel = tileCoord * TILE_SIZE + localId.xy;
    let inside = all(pixel < uniforms.viewport.xy);
    let pixelIndex = pixel.y * uniforms.viewport.x + pixel.x;
    let gapFillOnly = uniforms.microTuning.w != 0u;
    let tracePixel = inside &&
        (!gapFillOnly ||
         (pixelIndex < arrayLength(&hardwareCoverage) &&
          hardwareCoverage[pixelIndex] == 0u));
    // A closed height field cannot contain a one-pixel opening surrounded on
    // all eight sides. Both raster paths can nevertheless reject the exact
    // centre ray at a shared analytic-patch boundary after world-scale f32
    // arithmetic. Retry only this proven crack shape at subpixel positions;
    // silhouettes and genuine empty regions never enter the repair path.
    let retryIsolatedGap = tracePixel && gapFillOnly &&
        hardwareSurroundsPixel(pixel);

    if (localIndex == 0u) {
        atomicStore(&gapPixelCount, 0u);
    }
    workgroupBarrier();
    if (tracePixel) {
        atomicAdd(&gapPixelCount, 1u);
    }
    workgroupBarrier();
    if (gapFillOnly && atomicLoad(&gapPixelCount) == 0u) {
        if (inside && pixelIndex < arrayLength(&computeCoverage)) {
            computeCoverage[pixelIndex] = 0u;
            computeDistance[pixelIndex] = -1.0;
        }
        return;
    }

    var best = -1.0;
    var bestNode = SPARSE_VOXEL_BRICK_INVALID_INDEX;
    var bestCell = 0u;
    var bestAxis = 0u;
    var bestHit = brickTraceMiss();
    // Accumulated per thread and flushed once below, so the instrument does not
    // put an atomic inside the candidate loop it is measuring.
    var occupiedHits = 0u;
    var microEligibleHits = 0u;
    var exactTraces = 0u;
    let collectMicroCounters = uniforms.debugFlags.z != 0u;
    if (tile < arrayLength(&tileOffsets) && tile < arrayLength(&tileCounts)) {
        let count = tileCounts[tile];
        let end = tileOffsets[tile];
        let begin = end - min(count, end);

        let origin = uniforms.cameraWorldPosition.xyz;
        let direction = cameraRayDirection(vec2<f32>(pixel));
        let inverseDirection = safeInverseDirection(direction);

        var chunkBase = begin;
        while (chunkBase < end) {
            let chunkCount = min(TILE_CHUNK, end - chunkBase);
            // The whole workgroup cooperates on the fetch, then everyone reads
            // it from shared memory.
            if (localIndex < chunkCount) {
                let source = chunkBase + localIndex;
                chunkNodes[localIndex] = select(
                    SPARSE_VOXEL_BRICK_INVALID_INDEX,
                    tileCandidates[source],
                    source < arrayLength(&tileCandidates));
            }
            workgroupBarrier();

            if (tracePixel) {
                for (var entry = 0u; entry < chunkCount; entry += 1u) {
                    let nodeIndex = chunkNodes[entry];
                    if (nodeIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
                        nodeIndex >= arrayLength(&nodes)) {
                        continue;
                    }
                    let limit = select(best, 1.0e30, best < 0.0);
                    var hit = traceBrick(
                        nodes[nodeIndex], origin, direction, inverseDirection,
                        limit);
                    if (hit.distance < 0.0 && retryIsolatedGap) {
                        let retryOffsets = array<vec2<f32>, 4>(
                            vec2<f32>(-0.35, -0.35),
                            vec2<f32>(0.35, -0.35),
                            vec2<f32>(-0.35, 0.35),
                            vec2<f32>(0.35, 0.35));
                        for (var retry = 0u; retry < 4u; retry += 1u) {
                            let retryDirection = cameraRayDirection(
                                vec2<f32>(pixel) + retryOffsets[retry]);
                            let retryHit = traceBrick(
                                nodes[nodeIndex], origin, retryDirection,
                                safeInverseDirection(retryDirection), limit);
                            if (retryHit.distance >= 0.0 &&
                                (hit.distance < 0.0 ||
                                 retryHit.distance < hit.distance)) {
                                hit = retryHit;
                            }
                        }
                    }
                    // Same switch that enables the fragment's
                    // microCounters.eligibleParentHits, so one run produces
                    // both halves of the comparison and the routed path pays
                    // nothing when it is off.
                    if (collectMicroCounters && hit.distance >= 0.0) {
                        // Every accepted occupied cell is a place a micro path
                        // would have to carve, whether or not this candidate
                        // ends up winning the pixel.
                        occupiedHits += 1u;
                        if (brickCellMicroEligible(
                                nodes[nodeIndex], hit.cellIndex)) {
                            microEligibleHits += 1u;
                        }
                        if (hit.usedExact) {
                            exactTraces += 1u;
                        }
                    }
                    if (hit.distance >= 0.0 &&
                        (best < 0.0 || hit.distance < best)) {
                        best = hit.distance;
                        bestNode = nodeIndex;
                        bestCell = hit.cellIndex;
                        bestAxis = hit.axis;
                        bestHit = hit;
                    }
                }
            }
            workgroupBarrier();
            chunkBase += chunkCount;
        }
    }

    // The fragment publishes max(travelled, nearDistance); a hit in front of
    // the near plane has to report the near plane, not its true distance.
    let nearDistance = max(uniforms.cameraWorldPosition.w, 1.0e-4);
    if (best >= 0.0) {
        best = max(best, nearDistance);
    }
    if (inside && occupiedHits != 0u) {
        atomicAdd(&parity.occupiedCellHits, occupiedHits);
        atomicAdd(&parity.microEligibleCellHits, microEligibleHits);
        atomicAdd(&parity.computeExactTraces, exactTraces);
    }
    if (inside && pixelIndex < arrayLength(&computeDistance)) {
        computeDistance[pixelIndex] = best;
        computeCoverage[pixelIndex] = select(0u, 1u, best >= 0.0);
        computeNode[pixelIndex] = bestNode;
        computeCell[pixelIndex] = bestCell;
        computeAxis[pixelIndex] = bestAxis;

        // Same record assembly as the raster fragment's output stage. A carved
        // hit takes the micro identity and canonical microcell address; a
        // retained-parent fallback still publishes a payload so the material
        // pass shades it as micro rather than as a bare parent voxel.
        var payload = 0u;
        var address = vec4<u32>(0u);
        var identity = 0u;
        if (best >= 0.0 && bestNode != SPARSE_VOXEL_BRICK_INVALID_INDEX) {
            let node = nodes[bestNode];
            let direction = cameraRayDirection(vec2<f32>(pixel));
            let face = bestAxis | select(0u, 4u, direction[bestAxis] < 0.0);
            let terrainIdentity = node.regionMappingId != 0u;
            let parentIdentity = select(
                brickSurfaceIdentity(node.minimumVoxelSize, bestCell),
                brickTerrainSurfaceIdentity(
                    node.minimumVoxelSize, node.canonicalGridOriginScale,
                    uniforms.cameraWorldPosition.xyz + direction * best),
                terrainIdentity);
            let detailedIdentity = select(
                parentIdentity,
                brickMicroSurfaceIdentity(
                    parentIdentity, bestHit.microCell, face,
                    bestHit.microDescriptorId),
                bestHit.microValid);
            identity = select(
                detailedIdentity, parentIdentity, terrainIdentity);
            // Ordinary generated-terrain hits need the same canonical material
            // address as the hardware fragment path.  Leaving this zero made
            // the material pass fall back to camera-derived worldPosition /
            // finestVoxelSize coordinates whenever the compute raster won a
            // pixel.  Hardware and compute could therefore shade the same
            // resolved voxel with different per-voxel hashes as the camera
            // moved, producing non-uniform patches inside otherwise solid
            // voxel faces.
            if (terrainIdentity) {
                let hitCell = vec3<i32>(
                    i32(bestCell & 7u),
                    i32((bestCell >> 3u) & 7u),
                    i32((bestCell >> 6u) & 7u));
                let canonicalParent = brickMicroCanonicalParentKey(
                    node.canonicalGridOriginScale, hitCell);
                address = vec4<u32>(
                    bitcast<vec3<u32>>(canonicalParent),
                    node.regionMappingId);
            }
            if (bestHit.microValid) {
                payload = brickMicroPayload(
                    bestHit.microDescriptorId, bestHit.microCell,
                    bestHit.microCarveFace, bestHit.microSubdivision,
                    bestHit.microHeightfield, bestHit.microTerrainSlope);
            } else if (bestHit.microFallback) {
                payload = brickMicroFallbackPayload(
                    bestHit.microDescriptorId, bestHit.microTerrainSlope);
            } else if (bestHit.microTerrainSlope) {
                payload = SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE;
            }
            if (bestHit.microValid || bestHit.microFallback) {
                let canonicalScale =
                    u32(max(node.canonicalGridOriginScale.w, 1));
                let parentKey = brickMicroCanonicalParentKey(
                    node.canonicalGridOriginScale,
                    vec3<i32>(bestHit.microParentCell));
                let canonicalMicro =
                    parentKey * i32(bestHit.microSubdivision) +
                    vec3<i32>(bestHit.microCell * canonicalScale);
                address = vec4<u32>(
                    bitcast<vec3<u32>>(canonicalMicro), node.regionMappingId);
            }
        }
        computeMicroPayload[pixelIndex] = payload;
        computeMicroAddress[pixelIndex] = address;
        computeIdentity[pixelIndex] = identity;
        computeContext[pixelIndex] = bestHit.microContext;
    }
}

// One thread per pixel. Diffs the compute result against the hardware raster's
// resolved coverage and linear depth.
@compute @workgroup_size(64, 1, 1)
fn compareBrickTileRaster(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixelIndex = globalId.x;
    let pixelCount = uniforms.viewport.x * uniforms.viewport.y;
    if (pixelIndex >= pixelCount ||
        pixelIndex >= arrayLength(&computeCoverage) ||
        pixelIndex >= arrayLength(&hardwareCoverage)) {
        return;
    }
    atomicAdd(&parity.comparedPixels, 1u);
    let hardwareHit = hardwareCoverage[pixelIndex] != 0u;
    let computeHit = computeCoverage[pixelIndex] != 0u;
    if (hardwareHit && !computeHit) {
        atomicAdd(&parity.hardwareOnly, 1u);
        return;
    }
    if (computeHit && !hardwareHit) {
        atomicAdd(&parity.computeOnly, 1u);
        return;
    }
    if (!hardwareHit) {
        return;
    }
    atomicAdd(&parity.bothCovered, 1u);
    let delta = abs(hardwareDistance[pixelIndex] - computeDistance[pixelIndex]);
    // One millimetre. The two paths intersect the same boxes but accumulate
    // their DDA differently, so exact bit equality is not the bar here;
    // agreeing on the same surface is.
    if (delta <= 1.0e-3) {
        atomicAdd(&parity.distanceMatches, 1u);
        // Same surface on both paths, so the carved record is comparable. The
        // hardware's resolved surface identity lives in voxelIndex; see the
        // resolver, which stores an identity there rather than an index.
        atomicAdd(&parity.microComparedPixels, 1u);
        let computePayload = computeMicroPayload[pixelIndex];
        let hardwarePayload = microPayloads[pixelIndex];
        if (computePayload != hardwarePayload) {
            atomicAdd(&parity.microPayloadMismatches, 1u);
            if (computePayload == 0u) {
                atomicAdd(&parity.microPayloadHardwareOnly, 1u);
            } else if (hardwarePayload == 0u) {
                atomicAdd(&parity.microPayloadComputeOnly, 1u);
            } else {
                atomicAdd(&parity.microPayloadBothDiffer, 1u);
            }
        }
        let hardwareAddress = materialSampleAddresses[pixelIndex];
        let computeAddress = computeMicroAddress[pixelIndex];
        if (any(bitcast<vec3<u32>>(hardwareAddress.canonicalMicroCoordinate) !=
                computeAddress.xyz) ||
            hardwareAddress.regionMappingId != computeAddress.w) {
            atomicAdd(&parity.microAddressMismatches, 1u);
        }
        if (computeIdentity[pixelIndex] !=
            resolvedSurfaces[pixelIndex].voxelIndex) {
            atomicAdd(&parity.microIdentityMismatches, 1u);
        }
        return;
    }
    if (computeDistance[pixelIndex] < hardwareDistance[pixelIndex]) {
        atomicAdd(&parity.computeNearer, 1u);
    } else {
        atomicAdd(&parity.hardwareNearer, 1u);
    }
    atomicMax(&parity.maximumDeltaMicrometres,
        u32(clamp(delta * 1.0e6, 0.0, 4.0e9)));
}

@compute @workgroup_size(1, 1, 1)
fn resetBrickTileParity() {
    atomicStore(&parity.comparedPixels, 0u);
    atomicStore(&parity.bothCovered, 0u);
    atomicStore(&parity.hardwareOnly, 0u);
    atomicStore(&parity.computeOnly, 0u);
    atomicStore(&parity.distanceMatches, 0u);
    atomicStore(&parity.maximumDeltaMicrometres, 0u);
    atomicStore(&parity.computeNearer, 0u);
    atomicStore(&parity.hardwareNearer, 0u);
    atomicStore(&parity.worstPixel, 0xffffffffu);
    atomicStore(&parity.worstHardwareBits, 0u);
    atomicStore(&parity.worstComputeBits, 0u);
    atomicStore(&parity.worstCandidatesSpanning, 0u);
    atomicStore(&parity.worstCandidateCount, 0u);
    atomicStore(&parity.worstComputeNode, 0u);
    atomicStore(&parity.occupiedCellHits, 0u);
    atomicStore(&parity.microEligibleCellHits, 0u);
    atomicStore(&parity.microComparedPixels, 0u);
    atomicStore(&parity.microPayloadMismatches, 0u);
    atomicStore(&parity.microAddressMismatches, 0u);
    atomicStore(&parity.microIdentityMismatches, 0u);
    atomicStore(&parity.microPayloadHardwareOnly, 0u);
    atomicStore(&parity.microPayloadComputeOnly, 0u);
    atomicStore(&parity.microPayloadBothDiffer, 0u);
    atomicStore(&parity.computeExactTraces, 0u);
}

// Second pass: the pixel whose disagreement equals the maximum publishes its
// identity. Selecting on equality rather than racing an atomicMax keeps this
// deterministic enough to inspect a single case by hand.
@compute @workgroup_size(64, 1, 1)
fn captureBrickTileMismatch(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixelIndex = globalId.x;
    let pixelCount = uniforms.viewport.x * uniforms.viewport.y;
    if (pixelIndex >= pixelCount ||
        pixelIndex >= arrayLength(&computeCoverage) ||
        pixelIndex >= arrayLength(&hardwareCoverage)) {
        return;
    }
    if (hardwareCoverage[pixelIndex] == 0u || computeCoverage[pixelIndex] == 0u) {
        return;
    }
    let hardware = hardwareDistance[pixelIndex];
    let compute = computeDistance[pixelIndex];
    let delta = u32(clamp(abs(hardware - compute) * 1.0e6, 0.0, 4.0e9));
    if (delta != atomicLoad(&parity.maximumDeltaMicrometres) || delta == 0u) {
        return;
    }
    // Lowest pixel index wins, so the report is stable across frames.
    if (atomicMin(&parity.worstPixel, pixelIndex) > pixelIndex) {
        atomicStore(&parity.worstHardwareBits, bitcast<u32>(hardware));
        atomicStore(&parity.worstComputeBits, bitcast<u32>(compute));
        atomicStore(&parity.worstComputeNode, computeNode[pixelIndex]);

        // Re-walk this pixel's tile and ask the discriminating question: was a
        // brick spanning the hardware's answer ever in the candidate list?
        let gridWidth = tileGridWidth();
        let pixel = vec2<u32>(
            pixelIndex % uniforms.viewport.x,
            pixelIndex / uniforms.viewport.x);
        let tile = (pixel.y / TILE_SIZE) * gridWidth + pixel.x / TILE_SIZE;
        if (tile < arrayLength(&tileOffsets) && tile < arrayLength(&tileCounts)) {
            let count = tileCounts[tile];
            let end = tileOffsets[tile];
            let begin = end - min(count, end);
            atomicStore(&parity.worstCandidateCount, count);
            let origin = uniforms.cameraWorldPosition.xyz;
            let direction = cameraRayDirection(vec2<f32>(pixel));
            let inverseDirection = safeInverseDirection(direction);
            var spanning = 0u;
            for (var slot = begin; slot < end; slot += 1u) {
                if (slot >= arrayLength(&tileCandidates)) {
                    break;
                }
                let nodeIndex = tileCandidates[slot];
                if (nodeIndex >= arrayLength(&nodes)) {
                    continue;
                }
                let node = nodes[nodeIndex];
                let voxelSize = node.minimumVoxelSize.w;
                let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
                let box = intersectBox(
                    node.minimumVoxelSize.xyz + cells.minimumCell * voxelSize,
                    node.minimumVoxelSize.xyz + cells.maximumCell * voxelSize,
                    origin, inverseDirection);
                if (box.hit && box.entry <= hardware && box.exit >= hardware) {
                    spanning += 1u;
                }
            }
            atomicStore(&parity.worstCandidatesSpanning, spanning);
        }
    }
}

// H3: hardware-first merge. The hardware raster's resolved surface is the
// starting answer and the compute path may only *improve* it - it wins a pixel
// only when it has a strictly nearer hit and can supply the whole record.
//
// That asymmetry is deliberate. The compute traversal is known to miss some
// nearer surfaces the hardware finds; a nearest-wins merge seeded by hardware
// is immune to a miss, because a missing compute hit simply leaves the hardware
// answer standing. The reverse arrangement would punch holes.
@compute @workgroup_size(64, 1, 1)
fn mergeBrickTileRaster(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixelIndex = globalId.x;
    let pixelCount = uniforms.viewport.x * uniforms.viewport.y;
    if (pixelIndex >= pixelCount ||
        pixelIndex >= arrayLength(&computeCoverage) ||
        pixelIndex >= arrayLength(&resolvedSurfaces)) {
        return;
    }
    if (computeCoverage[pixelIndex] == 0u) {
        return;
    }
    let computeT = computeDistance[pixelIndex];
    let hardwareHit = hardwareCoverage[pixelIndex] != 0u;
    if (hardwareHit && hardwareDistance[pixelIndex] <= computeT) {
        return;
    }
    let nodeIndex = computeNode[pixelIndex];
    if (nodeIndex == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        nodeIndex >= arrayLength(&nodes)) {
        return;
    }
    let node = nodes[nodeIndex];
    let cellIndex = computeCell[pixelIndex];
    // Same material addressing the raster fragment uses: the popcount of the
    // occupancy bits before this cell, added to the word's running prefix.
    let entry = occupancy[node.occupancyIndex].cells[cellIndex >> 5u];
    if ((entry.bits & (1u << (cellIndex & 31u))) == 0u) {
        return;
    }
    let offset = entry.prefix +
        countOneBits(entry.bits & ((1u << (cellIndex & 31u)) - 1u));
    let materialSlot = node.materialBase + offset;
    if ((materialSlot >> 2u) >= arrayLength(&brickMaterialIndices)) {
        return;
    }
    let paletteIndex = (brickMaterialIndices[materialSlot >> 2u] >>
        ((materialSlot & 3u) * 8u)) & 0xffu;
    let paletteAddress = node.paletteBase + paletteIndex;
    if (paletteAddress >= arrayLength(&brickPalette)) {
        return;
    }
    let material = brickPalette[paletteAddress];

    // H4: rebuild the whole record the hardware fragment would have published,
    // not just a depth and a colour. Face, bottom albedo and terrain context
    // all reach shading; leaving them zero gives a routed pixel a flat facing
    // and the wrong secondary colour.
    //
    // Same packing as the raster fragment: axis in the low bits, bit 2 set when
    // the ray entered through the positive face.
    let direction = cameraRayDirection(vec2<f32>(
        f32(pixelIndex % uniforms.viewport.x),
        f32(pixelIndex / uniforms.viewport.x)));
    let axis = min(computeAxis[pixelIndex], 2u);
    let face = axis | select(0u, 4u, direction[axis] < 0.0);

    var bottomAlbedo = material.packed.x & RESOLVED_BOTTOM_ALBEDO_MASK;
    if ((material.packed.y & SPARSE_VOXEL_BRICK_SECONDARY_ENABLED) != 0u) {
        let bottomIndex = (material.packed.y >>
            SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_SHIFT) &
            SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_MASK;
        let bottomAddress = node.paletteBase + bottomIndex;
        if (bottomAddress < arrayLength(&brickPalette)) {
            bottomAlbedo =
                brickPalette[bottomAddress].packed.x &
                RESOLVED_BOTTOM_ALBEDO_MASK;
        }
    }
    // The traversal already resolved the context for the cell it stopped in,
    // and for a carved hit that is the microcell's context rather than the
    // parent's, so it must come from the walk rather than be re-read here.
    let microContext = computeContext[pixelIndex];

    // Exactly the routing the hardware resolver uses: the payload and the
    // canonical sample address ride in their own buffers, and reserved1 carries
    // only the EXACT_MICRO_MATERIAL bit that tells the material pass a carved
    // microsurface is addressable. Folding the payload into the surface record
    // instead would leave the material pass reading a parent voxel.
    let microPayload = computeMicroPayload[pixelIndex];
    let exactMicroMaterial = select(
        0u, SPARSE_VOXEL_BRICK_EXACT_MICRO_MATERIAL,
        (microPayload & SPARSE_VOXEL_MICRO_PAYLOAD_VALID) != 0u);
    let analyticTerrain = select(
        0u, SPARSE_VOXEL_SURFACE_ANALYTIC_TERRAIN,
        (microPayload & SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE) != 0u &&
        (microPayload & SPARSE_VOXEL_MICRO_PAYLOAD_VALID) == 0u);
    resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
        computeT,
        0u,
        computeIdentity[pixelIndex],
        face,
        SPARSE_VOXEL_SURFACE_HIT | SPARSE_VOXEL_SURFACE_PACKED_MATERIAL |
            analyticTerrain,
        material.packed.x,
        // The transient face/hit nibble is republished below; everything else
        // in the metadata word is carried through untouched.
        // The source material's bit 31 is traversal eligibility, whereas the
        // resolved record uses it for an exact-micro *hit*. Do not let an
        // ordinary solid terrain hit inherit the source meaning.
        ((material.packed.y & ~(
                SPARSE_VOXEL_BRICK_TRANSIENT_MASK |
                SPARSE_VOXEL_BRICK_MICRO_ENABLED)) |
            ((face | (SPARSE_VOXEL_SURFACE_HIT << 3u)) <<
                SPARSE_VOXEL_BRICK_SURFACE_SHIFT)) | exactMicroMaterial,
        (bottomAlbedo & RESOLVED_BOTTOM_ALBEDO_MASK) |
            (microContext << RESOLVED_CONTEXT_SHIFT));
    microPayloads[pixelIndex] = microPayload;
    let sampleAddress = computeMicroAddress[pixelIndex];
    materialSampleAddresses[pixelIndex] = VoxelMaterialSampleAddress(
        bitcast<vec3<i32>>(sampleAddress.xyz), sampleAddress.w);
    // Publish into the shared products, not just the private ones. Writing the
    // resolved surface alone leaves coverage at zero and every downstream pass
    // treats the pixel as empty - which is exactly what routing every brick to
    // compute produced before this line existed.
    hardwareCoverage[pixelIndex] = 1u;
    hardwareDistance[pixelIndex] = computeT;
}
