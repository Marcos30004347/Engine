// Brick rasterization: three cube faces per visible brick, DDA in the fragment.
//
// Hardware rasterization does the binning the SVRaster chain does in compute,
// and the depth test does the nearest-surface resolution `hitPrecedes()` does
// per candidate. What the fragment shader adds is one DDA from the ray's entry
// point into the brick.
//
// The three faces drawn are the ones facing *toward* the camera. Terrain uses a
// full-brick proxy pinned to near depth: frustum/HZB select its bricks, then the
// exact DDA decides every covered pixel. A tighter occupied sub-box or
// AABB-wide proxy depth can become an angle-dependent rejection at close range,
// so the fragment DDA is deliberately the only per-pixel terrain visibility
// test after the brick list has been built.
//
// Front faces are clipped away when the camera is inside a brick, which is why
// back faces were used originally. That case now emits a full-screen triangle
// pinned to the near plane instead: it covers the brick conservatively and lets
// the fragment DDA reject every ray outside the brick.
//
// The march is two-level. A 4-cubed coarse mask - 64 bits, carried from the
// vertex shader in registers - is stepped first, and the 8-cubed occupancy
// words are only touched inside a coarse cell that has something in it. Empty
// space therefore costs no memory traffic at all, which is most of what a
// surface-fitted brick contains.
#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_brick_common.wgsl"
#define VOXEL_GEOMETRY_IR_ENABLED
#include "voxel_geometry_ir.wgsl"
#include "voxel_geometry_field_cache.wgsl"
#include "sparse_voxel_micro_geometry.wgsl"
#include "sparse_voxel_brick_micro_common.wgsl"

#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
#ifndef SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
#define SPARSE_VOXEL_BRICK_DIRECT_ONLY
#endif
#endif

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBrickDrawUniforms;
@group(0) @binding(1) var<storage, read> nodes : array<SparseVoxelBrickNode>;
@group(0) @binding(2) var<storage, read> occupancy : array<SparseVoxelBrickOccupancy>;
// One palette index per occupied voxel, four packed per word: WGSL storage
// buffers have no byte array type.
@group(0) @binding(3) var<storage, read> brickMaterialIndices : array<u32>;
@group(0) @binding(4) var<storage, read> visibleBricks : array<u32>;
// Fragment invocations per pixel, for the overdraw view. A 1-word stub unless
// that view is selected; `uniforms.debugFlags.x` gates every access.
@group(0) @binding(5) var<storage, read_write> overdraw : array<atomic<u32>>;
// Palette records, `paletteStride` per instance. A voxel stores the index; the
// record itself is looked up here. See SparseVoxelBrickHierarchy::palette for
// why the palette is per instance rather than per brick.
@group(0) @binding(6) var<storage, read> brickPalette : array<SparseVoxelBrickMaterial>;
// Stable material handles are parallel to brickPalette and packed two per
// word. Bit 31 of the palette material gates these buffers, so legacy pixels
// never load them. Exact traversal uses the descriptor to refine the parent.
@group(0) @binding(7) var<storage, read> brickPaletteDescriptorIds : array<u32>;
@group(0) @binding(8) var<storage, read> microMaterials : array<SparseVoxelMicroMaterial>;
@group(0) @binding(9) var<storage, read_write> microCounters : SparseVoxelMicroCounters;
// One normalized context word per occupied parent voxel: context in the upper
// byte and the four-corner terrain coverage patch in the lower 24 bits.
@group(0) @binding(10) var<storage, read> brickMicroSurfaceData : array<vec2<u32>>;
@group(0) @binding(11) var<storage, read> voxelGeometryPrograms : array<VoxelGeometryProgram>;
@group(0) @binding(12) var<storage, read> voxelGeometryInstructions : array<VoxelGeometryInstruction>;
@group(0) @binding(13) var<storage, read> voxelGeometryParameters : array<VoxelGeometryParameters>;
@group(0) @binding(14) var<storage, read> voxelMaterialFieldDefaults : array<VoxelMaterialFieldDefaults>;
@group(0) @binding(15) var<storage, read> voxelGeometryFieldCache :
    array<SparseVoxelGeometryFieldCacheRecord>;
// The exact-micro descriptor, denormalized per palette address. Reaching a
// descriptor by id is a two-step chain ending in a scattered dependent load,
// paid once per eligible parent hit - 15,718,061 per frame, 1:1 with eligible
// parents, no reuse across the walk. This array holds the same values keyed by
// the palette address the eligibility test has already computed, so the hot
// path does one cached load instead. xy is the descriptor, z is its id.
@group(0) @binding(17) var<storage, read> brickPaletteMicroMaterials :
    array<vec4<u32>>;

const SPARSE_VOXEL_BRICK_LOD_DEBUG_LOCAL_SHIFT : u32 = 16u;
const SPARSE_VOXEL_BRICK_LOD_DEBUG_CLIPMAP_SHIFT : u32 = 0u;
const SPARSE_VOXEL_BRICK_LOD_DEBUG_MASK : u32 = 0xffu;

fn brickLodDebugPayload(nodeLevel : u32) -> u32 {
    let localLevel = nodeLevel & SPARSE_VOXEL_BRICK_LOD_DEBUG_MASK;
    let clipmapLevel =
        (nodeLevel >> 8u) & SPARSE_VOXEL_BRICK_LOD_DEBUG_MASK;
    return (localLevel << SPARSE_VOXEL_BRICK_LOD_DEBUG_LOCAL_SHIFT) |
        (clipmapLevel << SPARSE_VOXEL_BRICK_LOD_DEBUG_CLIPMAP_SHIFT);
}

#ifdef SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
// MicroPrepare stores residual occupancy by compact visible-list slot. It
// deliberately does not use node.occupancyIndex: streamed node slots are
// reusable, while this buffer is a transient product of this exact draw list.
struct SparseVoxelMicroResidual {
    bits : array<u32, SPARSE_VOXEL_BRICK_OCCUPANCY_WORDS>,
    coarseLow : u32,
    coarseHigh : u32,
    // Nonzero when the brick extracted nothing, so the fields above were never
    // written and the immutable source occupancy is the residual mask.
    passthrough : u32,
    padding : u32,
}
@group(0) @binding(16) var<storage, read> microResiduals :
    array<SparseVoxelMicroResidual>;
#endif

#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
// Compact, unique-parent records emitted by MicroPrepare. The R2 direct variant
// starts at this parent; the sparse tie variant replays incumbent ownership only
// for boundary and proxy-edge samples.
struct SparseVoxelMicroBrickRecord {
    nodeIndex : u32,
    parentDescriptorFlags : u32,
    paletteAddress : u32,
    microContextData : u32,
    // R3: quantized depth of every surface column, four bits each, generated
    // once by MicroPrepare. Bit 31 of .w marks it usable.
    columns : vec4<u32>,
}
@group(0) @binding(16) var<storage, read> microBrickRecords :
    array<SparseVoxelMicroBrickRecord>;
#endif

struct BrickVertexOutput {
    @builtin(position) position : vec4<f32>,
    // xyz: the *full* brick's minimum corner, which is what cell indices are
    // measured from. w: voxel size.
    //
    // Everything the fragment needs travels here rather than being re-fetched.
    // The node is one storage load; doing it again in the fragment
    // costs a dependent load per pixel for values the vertex shader already
    // had in registers.
    @location(0) @interpolate(flat) brickMinimumVoxelSize : vec4<f32>,
    // x, y: the 4-cubed coarse occupancy mask. z: occupancy array index.
    // w: base of this brick's run in the material array.
    @location(1) @interpolate(flat) brickPacked : vec4<u32>,
    // Inclusive occupied cell range, six 3-bit fields, so the fragment marches
    // only the occupied span.
    @location(2) @interpolate(flat) brickBounds : u32,
    // First palette entry belonging to this brick's instance.
    @location(3) @interpolate(flat) paletteBase : u32,
    // Tagged local XZ rectangle supplied by finer terrain cascades.
    @location(4) @interpolate(flat) terrainClip : u32,
    // Low byte: hierarchy-local LOD; next byte: terrain clipmap LOD. Exact
    // microgeometry is canonical only when both are zero.
    @location(5) @interpolate(flat) nodeLevel : u32,
    // Signed address of this brick in the canonical finest-parent grid.
    @location(6) @interpolate(flat) canonicalGridOriginScale : vec4<i32>,
    @location(7) @interpolate(flat) regionMappingId : u32,
#ifdef SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    // x: visible-list slot for residual occupancy.
    @location(8) @interpolate(flat) microRasterData : vec4<u32>,
#endif
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
    // x: packed parent/descriptor/program flags. y: palette address.
    // z: normalized context byte plus the four-corner surface patch.
    @location(8) @interpolate(flat) microRasterData : vec4<u32>,
    // R3 compact column cache, carried in registers rather than refetched.
    @location(9) @interpolate(flat) microColumnCache : vec4<u32>,
#endif
}

// The main render target carries visibility and the primary material. A small
// R32Uint companion carries bottom RGB24 plus the normalized terrain-context
// byte; this preserves the exact 32-bit temporal identity without another
// attachment. A
// second R32Uint companion carries exact micro-surface identity when carving
// is active and remains zero for legacy hits.
//
//   x : packed material word 0 (albedo, roughness)
//   y : packed material word 1, with the hit face and surface flags folded into
//       the high half the material format leaves reserved
//   z : surface identity - see brickSurfaceIdentity()
//   w : distance along the primary ray, bit-cast
//
// Depth stays a float attachment because the hardware owns it; everything else
// is integer, so bit-casting the distance into the fourth channel costs nothing
// and removes a second target and a second full-screen texture read in the
// resolve. `textureLoad` with no filtering and no blending makes the round trip
// exact.
struct BrickFragmentOutput {
    @builtin(frag_depth) depth : f32,
    @location(0) surface : vec4<u32>,
    @location(1) bottomAlbedo : u32,
    @location(2) microPayload : u32,
    // Stable canonical material-grid XYZ plus logical region mapping ID.
    @location(3) materialSampleAddress : vec4<u32>,
    // Diagnostic-only logical terrain page identity. Unlike the material
    // address this is populated for every surviving parent or micro hit.
    @location(4) terrainOwnershipId : u32,
}

// A stable, per-voxel surface identity.
//
// Temporal reuse - SVGF history and the light field both - accepts a reprojected
// sample only when its `(instanceIndex, voxelIndex)` matches exactly. Bricks are
// built in world space as one implicit instance, so `instanceIndex` is 0 on
// every pixel and this value carries the entire test.
//
// It must therefore vary per voxel, not per appearance. Keying on the packed
// material made every pixel of a uniformly coloured surface identical, so
// history was accepted wholesale: walking the camera into the shell kept the
// exterior's lit history on the interior walls for as long as the accumulation
// took to decay.
//
// Physical occupancy slots are deliberately reusable by streamed terrain.
// Using the slot address as identity makes a newly installed page inherit the
// evicted page's lighting, and forced the CPU to invalidate the whole screen on
// every install. Hash the represented world-space brick and its LOD instead:
// retained pages keep their history even if the residency directory changes,
// while a different page or finer LOD gets a different identity before it is
// published. Bit patterns are used so negative coordinates remain stable.





// Projected screen area of the tight occupied box, in pixels. Deliberately the
// same quantity the compute binner measures, and deliberately cheap: an
// approximate solid angle from the box radius and its distance, not an eight
// corner projection.
fn brickProjectedFootprintPixels(node : SparseVoxelBrickNode) -> f32 {
    // Mirror sparse_voxel_brick_tile_bin.wgsl exactly. Generated terrain is
    // rasterized in a camera-relative canonical lattice; routing in absolute
    // f32 world space makes the hardware and compute paths disagree at
    // planetary coordinates, so routed bricks still hit the hardware proxy and
    // keep all of its overdraw.
    let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let minimum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.minimumCell));
    let maximum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.maximumCell));
    let closest = clamp(vec3<f32>(0.0), minimum, maximum);
    let distance = max(length(closest), 1.0e-4);
    let radius = 0.5 * length(maximum - minimum);
    let focal = min(
        abs(uniforms.focalPrincipal.x), abs(uniforms.focalPrincipal.y));
    let projected = radius * focal / distance;
    return projected * projected * 4.0;
}

fn brickCameraRayDirection(fragmentPosition : vec4<f32>) -> vec3<f32> {
    let viewX = (fragmentPosition.x - uniforms.focalPrincipal.z) /
        uniforms.focalPrincipal.x;
    let viewY = (fragmentPosition.y - uniforms.focalPrincipal.w) /
        uniforms.focalPrincipal.y;
    return normalize(
        uniforms.cameraToWorld[0].xyz * viewX +
        uniforms.cameraToWorld[1].xyz * viewY +
        uniforms.cameraToWorld[2].xyz);
}

// Geometry stays relative to the node bounds that the CPU/GPU publication
// pass evaluated in higher precision. Reconstructing an absolute position in
// the shader from canonicalWorldGrid + a large integer cell address loses the
// low bits when those terms nearly cancel. Adjacent pages then quantize a
// shared plane differently, producing view-dependent cracks and overlaps.
// Canonical integer coordinates remain useful for stable voxel identity, but
// must not be converted back into absolute f32 geometry here.
fn brickCellBoundaryWorld(
    minimumVoxelSize : vec4<f32>, canonicalGridOriginScale : vec4<i32>,
    regionMappingId : u32, cell : vec3<i32>) -> vec3<f32> {
    return minimumVoxelSize.xyz + vec3<f32>(cell) * minimumVoxelSize.w;
}

// Camera-relative geometry for traversal and proxy rasterization. Generated
// terrain nodes are independently published into stable arena ranges, so their
// f32 world minima can round opposite sides of a shared 10 cm plane at large
// coordinates. Their canonical integer addresses cannot. Subtract the split
// camera cell while still in integer space, then convert the small difference
// to f32. Ordinary/package bricks retain their authored world coordinates.
fn brickCellBoundaryCameraRelative(
    minimumVoxelSize : vec4<f32>, canonicalGridOriginScale : vec4<i32>,
    regionMappingId : u32, cell : vec3<i32>) -> vec3<f32> {
    let cameraMagnitude = max(
        max(abs(uniforms.cameraWorldPosition.x),
            abs(uniforms.cameraWorldPosition.y)),
        abs(uniforms.cameraWorldPosition.z));
    // Near renderer zero, the CPU's double-precision publication has already
    // reduced the global map lattice to precise local f32 node bounds. Keep
    // that contract for bootstrap and GPU pages alike. Once the camera itself
    // is far enough from zero to lose centimetres, switch every generated
    // page to the split integer lattice before doing any f32 subtraction.
    if (regionMappingId != 0u && uniforms.canonicalWorldGrid.w > 0.0 &&
        cameraMagnitude > uniforms.canonicalWorldGrid.w * 65536.0) {
        let canonicalMinimum = uniforms.canonicalWorldGrid.xyz +
            vec3<f32>(canonicalGridOriginScale.xyz) *
                uniforms.canonicalWorldGrid.w;
        let coordinateTolerance = max(
            vec3<f32>(minimumVoxelSize.w * 0.25),
            abs(minimumVoxelSize.xyz) * 1.0e-6);
        // Bootstrap/proxy roots predate the GPU page payload and may carry a
        // non-zero region mapping without a global canonical address. They
        // must stay on their authored world bounds until the jointly
        // published GPU node replaces them; treating their local address as
        // global creates an overlapping wall across the refinement window.
        if (all(abs(canonicalMinimum - minimumVoxelSize.xyz) <=
                coordinateTolerance)) {
            let canonicalCell = canonicalGridOriginScale.xyz +
                cell * canonicalGridOriginScale.w;
            return vec3<f32>(
                canonicalCell - uniforms.canonicalCameraCell.xyz) *
                uniforms.canonicalWorldGrid.w -
                uniforms.canonicalCameraOffset.xyz;
        }
    }
    return minimumVoxelSize.xyz + vec3<f32>(cell) * minimumVoxelSize.w -
        uniforms.cameraWorldPosition.xyz;
}

// Project a camera-relative point without first adding the planetary camera
// position back to it. M * (camera + relative) is mathematically equivalent,
// but forming that absolute f32 position loses centimetres hundreds of
// kilometres from zero, and the view translation then has to cancel the same
// large value. That cancellation made clip W cross zero at close range and
// presented as faces flipping or punching through depth while the eye turned.
//
// For this finite reverse-Z perspective matrix, row W is the exact relative
// view depth. Row Z's vector length is near/(far-near), and its camera-origin
// constant is near * (1 + slope). This reconstructs clip Z without either
// absolute positions or the imprecise view translation.
fn brickCameraRelativeClip(relativeWorld : vec3<f32>) -> vec4<f32> {
    let rowX = vec3<f32>(
        uniforms.worldToClip[0].x, uniforms.worldToClip[1].x,
        uniforms.worldToClip[2].x);
    let rowY = vec3<f32>(
        uniforms.worldToClip[0].y, uniforms.worldToClip[1].y,
        uniforms.worldToClip[2].y);
    let rowZ = vec3<f32>(
        uniforms.worldToClip[0].z, uniforms.worldToClip[1].z,
        uniforms.worldToClip[2].z);
    let rowW = vec3<f32>(
        uniforms.worldToClip[0].w, uniforms.worldToClip[1].w,
        uniforms.worldToClip[2].w);
    let clipW = dot(rowW, relativeWorld);
    let depthSlope = length(rowZ);
    let clipZ = uniforms.cameraWorldPosition.w * (1.0 + depthSlope) -
        depthSlope * clipW;
    return vec4<f32>(
        dot(rowX, relativeWorld), dot(rowY, relativeWorld), clipZ, clipW);
}

fn brickFinerTerrainClipContainsCameraRelative(
    minimumVoxelSize : vec4<f32>, canonicalGridOriginScale : vec4<i32>,
    regionMappingId : u32, packed : u32,
    relativeWorld : vec3<f32>) -> bool {
    if (!SPARSE_VOXEL_BRICK_ENABLE_TERRAIN_EXCLUSION_CLIP ||
        packed == SPARSE_VOXEL_BRICK_INVALID_INDEX ||
        (packed & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_FLAG) == 0u) {
        return false;
    }
    let brickMinimum = brickCellBoundaryCameraRelative(
        minimumVoxelSize, canonicalGridOriginScale, regionMappingId,
        vec3<i32>(0));
    let step = minimumVoxelSize.w /
        SPARSE_VOXEL_BRICK_TERRAIN_CLIP_QUANTIZATION;
    let finer = vec4<f32>(
        brickMinimum.x +
            f32(packed & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        brickMinimum.z +
            f32((packed >> 7u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        brickMinimum.x +
            f32((packed >> 14u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step,
        brickMinimum.z +
            f32((packed >> 21u) & SPARSE_VOXEL_BRICK_TERRAIN_CLIP_MASK) * step);
    // The former absolute-world epsilon grew to 2.6 cm around world-data's
    // 260 km coordinate. It expanded the exclusion rectangle beyond the finer
    // page and exposed a moving wall/gap at one-centimetre camera clearance.
    // All operands are local now, so only a tiny voxel-relative tie tolerance
    // is needed.
    let epsilon = minimumVoxelSize.w * 1.0e-4;
    return relativeWorld.x >= finer.x - epsilon &&
        relativeWorld.x <= finer.z + epsilon &&
        relativeWorld.z >= finer.y - epsilon &&
        relativeWorld.z <= finer.w + epsilon;
}

fn brickCanonicalVoxelSize(
    minimumVoxelSize : vec4<f32>, canonicalGridOriginScale : vec4<i32>,
    regionMappingId : u32) -> f32 {
    return minimumVoxelSize.w;
}




// Exact-micro tracing has one canonical parent interval in both the incumbent
// fine DDA and the R2 direct path. The legacy walk still selects the parent and
// owns all X/Y/Z ties; only the algebraically equivalent interval passed to the
// micro tracer is canonicalized here. This avoids a one-ULP drift caused by
// accumulated fine/coarse DDA boundary additions at oblique views.

// Keep the parent-local ray construction and exact heightfield call in one
// shared function. Besides reducing duplicated hot code, this gives the
// incumbent and direct shader variants the same floating-point expression
// tree for the returned world-ray distance.

#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
fn brickPreparedBoundaryTieCount(
    distances : vec3<f32>, boundary : f32, tolerance : f32) -> u32 {
    let matches = select(
        vec3<u32>(0u), vec3<u32>(1u),
        abs(distances - vec3<f32>(boundary)) <= vec3<f32>(tolerance));
    return matches.x + matches.y + matches.z;
}

fn brickPreparedBoundaryAmbiguous(
    nearT : vec3<f32>, farT : vec3<f32>, distance : f32) -> bool {
    let entry = max(nearT.x, max(nearT.y, nearT.z));
    let exit = min(farT.x, min(farT.y, farT.z));
    let tolerance = 2.0e-6 * max(1.0, max(abs(entry), abs(exit)));
    let tiedEntry = brickPreparedBoundaryTieCount(
        nearT, entry, tolerance) >= 2u &&
        abs(distance - entry) <= tolerance;
    let tiedExit = brickPreparedBoundaryTieCount(
        farT, exit, tolerance) >= 2u &&
        abs(distance - exit) <= tolerance;
    return tiedEntry || tiedExit;
}

fn brickPreparedDescriptorFace(
    axis : u32, negativeDirection : bool) -> u32 {
    return axis * 2u + select(0u, 1u, negativeDirection);
}

// The direct pass owns a fragment unless its exact hit lands on a boundary
// whose ownership only the source DDA can settle. The replay pass is defined as
// the exact complement of this predicate rather than a conservative superset of
// it, so the decision lives in one function that both pipelines call with the
// same arguments. A superset here is not merely wasteful: on connected terrain
// almost every fragment exits through an occupied neighbour, which is what made
// the "rare edge/corner" replay the most expensive pass in the frame.
fn brickPreparedBoundaryDeferred(
    nearT : vec3<f32>, farT : vec3<f32>,
    parentEnter : f32, parentExit : f32,
    parentAxis : u32, direction : vec3<f32>, neighbourMask : u32,
    distance : f32) -> bool {
    if (brickPreparedBoundaryAmbiguous(nearT, farT, distance)) {
        return true;
    }
    let tolerance = 2.0e-6 * max(
        1.0, max(abs(parentEnter), abs(parentExit)));
    var exitAxis = 0u;
    if (farT.y < farT.x) {
        exitAxis = 1u;
    }
    if (farT.z < farT[exitAxis]) {
        exitAxis = 2u;
    }
    let entryFace = brickPreparedDescriptorFace(
        parentAxis, direction[parentAxis] < 0.0);
    let exitFace = brickPreparedDescriptorFace(
        exitAxis, direction[exitAxis] >= 0.0);
    let sharedEntry =
        (neighbourMask & (1u << entryFace)) != 0u &&
        abs(distance - parentEnter) <= tolerance;
    let sharedExit =
        (neighbourMask & (1u << exitFace)) != 0u &&
        abs(distance - parentExit) <= tolerance;
    return sharedEntry || sharedExit;
}

#ifdef SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
// The direct records are unordered. Exact parent ties and exterior side-entry
// samples can require the source DDA's strict X/Y/Z owner. Only those rare
// proxy samples enter the replay pipeline; it restores that owner without
// putting brick traversal back into the R2 hot shader.
fn brickPreparedPotentialBoundaryAmbiguity(
    input : BrickVertexOutput) -> bool {
    let parentDescriptorFlags = input.microRasterData.x;
    let parentIndex = parentDescriptorFlags & 0x1ffu;
    let descriptorId = (parentDescriptorFlags >> 9u) & 0xffffu;
    if (descriptorId == 0u || descriptorId >= arrayLength(&microMaterials)) {
        return false;
    }
    let parentCell = vec3<u32>(
        parentIndex & 7u,
        (parentIndex >> 3u) & 7u,
        (parentIndex >> 6u) & 7u);
    let voxelSize = input.brickMinimumVoxelSize.w;
    let origin = uniforms.cameraWorldPosition.xyz;
    let direction = brickCameraRayDirection(input.position);
    let nearDistance = max(uniforms.cameraWorldPosition.w, 1.0e-4);
    let interval = brickExactMicroParentInterval(
        input.brickMinimumVoxelSize.xyz, parentCell, voxelSize,
        origin, direction, nearDistance);
    let nearT = interval.nearT;
    let farT = interval.farT;
    let entry = interval.entry;
    let exit = interval.exit;
    if (entry > exit || exit < nearDistance) {
        return false;
    }
    var entryAxis = 0u;
    if (nearT.y > nearT.x) {
        entryAxis = 1u;
    }
    if (nearT.z > max(nearT.x, nearT.y)) {
        entryAxis = 2u;
    }
    let descriptor = microMaterials[descriptorId];
    let profile = (descriptor.geometry >> 18u) & 0x07u;
    let microContext = input.microRasterData.z >> 24u;
    let staircaseSurface =
        sparseVoxelExactMicroIsTerrainStaircaseSurface(
            profile, microContext);
    let carveFace = select(
        firstTrailingBit((descriptor.geometry >> 12u) & 0x3fu),
        3u, staircaseSurface);
    let entryFace = brickPreparedDescriptorFace(
        entryAxis, direction[entryAxis] < 0.0);
    // A side-entered heightfield is delegated wholesale: evaluating it once in
    // the incumbent replay is cheaper than tracing it in both passes, and it
    // preserves the backend-sensitive boundary distance exactly.
    if (entryFace != carveFace) {
        return true;
    }
    let geometryIREnabled =
        (parentDescriptorFlags & (1u << 25u)) != 0u;
    let geometryExecutionFlags = select(
        0u, VOXEL_GEOMETRY_PROGRAM_ENABLED, geometryIREnabled);
    let solidStaircase = !geometryIREnabled &&
        profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE &&
        microContext == SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR;
    let parentFace = entryAxis |
        select(0u, 4u, direction[entryAxis] < 0.0);
    let parentKey = brickMicroCanonicalParentKey(
        input.canonicalGridOriginScale, vec3<i32>(parentCell));
    let micro = brickTraceExactMicroHeightfield(
        descriptor, descriptorId, geometryExecutionFlags, parentKey,
        input.brickMinimumVoxelSize.xyz, parentCell, voxelSize,
        origin, direction, nearDistance, parentFace, microContext,
        input.microRasterData.z & 0x00ffffffu, solidStaircase,
        input.microColumnCache);
    if (!micro.valid) {
        return false;
    }
    return brickPreparedBoundaryDeferred(
        nearT, farT, entry, exit, entryAxis, direction,
        parentDescriptorFlags >> 26u, micro.distance);
}
#endif

// R2's prepared fragment starts at one resolved parent. It deliberately has no
// outer brick slab, coarse mask, fine occupancy walk, material-prefix lookup,
// stochastic handoff, or general-volume branch. A carved miss simply discards;
// another prepared parent or the residual draw remains free to own the pixel.
fn drawPreparedMicroFragment(
    input : BrickVertexOutput,
    outputIn : BrickFragmentOutput) -> BrickFragmentOutput {
    var output = outputIn;
    let parentDescriptorFlags = input.microRasterData.x;
    let parentIndex = parentDescriptorFlags & 0x1ffu;
    let descriptorId = (parentDescriptorFlags >> 9u) & 0xffffu;
    let geometryIREnabled = (parentDescriptorFlags & (1u << 25u)) != 0u;
    let paletteAddress = input.microRasterData.y;
    let microContextData = input.microRasterData.z;
    if (descriptorId == 0u || descriptorId >= arrayLength(&microMaterials) ||
        paletteAddress >= arrayLength(&brickPalette)) {
        discard;
    }

    let voxelSize = input.brickMinimumVoxelSize.w;
    let brickMinimum = input.brickMinimumVoxelSize.xyz;
    let parentCell = vec3<u32>(
        parentIndex & 7u,
        (parentIndex >> 3u) & 7u,
        (parentIndex >> 6u) & 7u);

    // Reconstruct the exact G-buffer camera ray, matching the incumbent path.
    let origin = uniforms.cameraWorldPosition.xyz;
    let direction = brickCameraRayDirection(input.position);
    let nearDistance = max(uniforms.cameraWorldPosition.w, 1.0e-4);
    let interval = brickExactMicroParentInterval(
        brickMinimum, parentCell, voxelSize, origin, direction,
        nearDistance);
    let nearT = interval.nearT;
    let farT = interval.farT;
    let parentEnter = interval.entry;
    let parentExit = interval.exit;
    if (parentEnter > parentExit || parentExit < nearDistance) {
        discard;
    }
    var parentAxis = 0u;
    if (nearT.y > nearT.x) {
        parentAxis = 1u;
    }
    if (nearT.z > max(nearT.x, nearT.y)) {
        parentAxis = 2u;
    }
    let parentFace = parentAxis |
        select(0u, 4u, direction[parentAxis] < 0.0);

    let descriptor = microMaterials[descriptorId];
    let subdivision = descriptor.geometry & 0x0fu;
    let faceMask = (descriptor.geometry >> 12u) & 0x3fu;
    let profile = (descriptor.geometry >> 18u) & 0x07u;
    let microContext = microContextData >> 24u;
    let microSurfaceData = microContextData & 0x00ffffffu;
    let parentKey = brickMicroCanonicalParentKey(
        input.canonicalGridOriginScale, vec3<i32>(parentCell));
    let geometryExecutionFlags = select(
        0u, VOXEL_GEOMETRY_PROGRAM_ENABLED, geometryIREnabled);
    let terrainStaircase =
        profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE;
    let terrainStaircaseSurface =
        sparseVoxelExactMicroIsTerrainStaircaseSurface(
            profile, microContext);
    let carveFace = select(
        firstTrailingBit(faceMask), 3u, terrainStaircaseSurface);
    let staircaseInterior = terrainStaircase &&
        microContext == SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR;
    let staircaseFlat = terrainStaircase &&
        microContext == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE;
    let staircaseSlope = terrainStaircase &&
        (microContext & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
    let staircaseWall = terrainStaircase &&
        microContext >= SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X &&
        microContext <= SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z;
    let solidStaircase = !geometryIREnabled && staircaseInterior;

    // Side-entry samples are resolved once by the incumbent replay. Waiting
    // until after the exact trace would duplicate the most expensive work.
    let parentEntryFace = brickPreparedDescriptorFace(
        parentAxis, direction[parentAxis] < 0.0);
    if (parentEntryFace != carveFace) {
        discard;
    }

#ifdef SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    let collectMicroCounters = uniforms.debugFlags.z != 0u;
    if (collectMicroCounters) {
        atomicAdd(&microCounters.eligibleParentHits, 1u);
        atomicAdd(&microCounters.descriptorLoads, 1u);
        atomicAdd(&microCounters.exactTraceInvocations, 1u);
        if (staircaseInterior) {
            atomicAdd(&microCounters.traceContextInterior, 1u);
        } else if (staircaseFlat) {
            atomicAdd(&microCounters.traceContextFlat, 1u);
        } else if (staircaseSlope) {
            atomicAdd(&microCounters.traceContextSlope, 1u);
        } else if (staircaseWall) {
            atomicAdd(&microCounters.traceContextWall, 1u);
        } else {
            atomicAdd(&microCounters.traceContextOtherHeightfield, 1u);
        }
        if (solidStaircase) {
            atomicAdd(&microCounters.solidInteriorShortcuts, 1u);
        } else {
            atomicAdd(&microCounters.oneFaceInvocations, 1u);
        }
    }
#endif

    let micro = brickTraceExactMicroHeightfield(
        descriptor, descriptorId, geometryExecutionFlags, parentKey,
        brickMinimum, parentCell, voxelSize, origin, direction,
        nearDistance, parentFace, microContext,
        microSurfaceData, solidStaircase, input.microColumnCache);

    // Ambiguous boundary ownership is resolved by the sparse replay pass.
    // Discard here first so a residual parent remains untouched when the source
    // DDA says that it, rather than any compact record, owns the boundary.
    if (micro.valid && brickPreparedBoundaryDeferred(
        nearT, farT, parentEnter, parentExit, parentAxis, direction,
        parentDescriptorFlags >> 26u, micro.distance)) {
        discard;
    }

#ifdef SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    if (collectMicroCounters) {
        if (!solidStaircase) {
            atomicAdd(&microCounters.heightfieldSteps, micro.steps);
            if (micro.valid) {
                atomicAdd(&microCounters.oneFaceHits, 1u);
            } else {
                atomicAdd(&microCounters.oneFaceMisses, 1u);
            }
        }
        if (micro.valid) {
            atomicAdd(&microCounters.acceptedMicroHits, 1u);
            atomicAdd(&microCounters.firstHitExits, 1u);
        } else {
            atomicAdd(&microCounters.carvedParents, 1u);
            atomicAdd(&microCounters.parentContinuations, 1u);
            atomicAdd(&microCounters.finalDiscards, 1u);
            atomicAdd(&microCounters.traversalMisses, 1u);
        }
    }
#endif
    if (!micro.valid) {
        discard;
    }

    let surfaceT = max(micro.distance, nearDistance);
    let world = origin + direction * surfaceT;
    if (brickFinerTerrainClipContainsWorld(
            input.brickMinimumVoxelSize, input.terrainClip, world)) {
        discard;
    }
    let clipZ = dot(
        vec4<f32>(
            uniforms.worldToClip[0].z, uniforms.worldToClip[1].z,
            uniforms.worldToClip[2].z, uniforms.worldToClip[3].z),
        vec4<f32>(world, 1.0));
    let clipW = dot(
        vec4<f32>(
            uniforms.worldToClip[0].w, uniforms.worldToClip[1].w,
            uniforms.worldToClip[2].w, uniforms.worldToClip[3].w),
        vec4<f32>(world, 1.0));
    if (clipW <= 0.0) {
        discard;
    }
    let depth = clipZ / clipW;
    if (!(depth == depth)) {
        discard;
    }

    let material = brickPalette[paletteAddress];
    var bottomAlbedo = material.packed.x & 0x00ffffffu;
    if ((material.packed.y & SPARSE_VOXEL_BRICK_SECONDARY_ENABLED) != 0u) {
        let bottomIndex =
            (material.packed.y >>
                SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_SHIFT) &
            SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_MASK;
        let bottomAddress = input.paletteBase + bottomIndex;
        if (bottomAddress < arrayLength(&brickPalette)) {
            bottomAlbedo =
                brickPalette[bottomAddress].packed.x & 0x00ffffffu;
        }
    }

    let hitAxis = micro.face & 3u;
    let face = hitAxis | select(0u, 4u, direction[hitAxis] < 0.0);
    let terrainSlope = terrainStaircase &&
        (microContext & SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
    output.depth = clamp(depth, 0.0, 1.0);
    let terrainIdentity = input.regionMappingId != 0u;
    let parentIdentity = select(
        brickSurfaceIdentity(input.brickMinimumVoxelSize, parentIndex),
        brickTerrainSurfaceIdentity(
            input.brickMinimumVoxelSize,
            input.canonicalGridOriginScale, world),
        terrainIdentity);
    let surfaceIdentity = select(
        brickMicroSurfaceIdentity(
            parentIdentity, micro.cell, face, descriptorId),
        parentIdentity, terrainIdentity);
    output.surface = vec4<u32>(
        material.packed.x,
        // Bit 31 means that the source cell is eligible for micro traversal,
        // but in a resolved surface it means that this particular ray hit an
        // exact microcell.  Carrying eligibility through made an ordinary
        // hardware-raster terrain voxel skip the canonical per-voxel colour
        // path, while the compute raster shaded that same voxel normally.
        (material.packed.y & ~(
                SPARSE_VOXEL_BRICK_TRANSIENT_MASK |
                SPARSE_VOXEL_BRICK_MICRO_ENABLED)) |
            ((face | (SPARSE_VOXEL_SURFACE_HIT << 3u)) <<
                SPARSE_VOXEL_BRICK_SURFACE_SHIFT) |
            SPARSE_VOXEL_BRICK_MICRO_ENABLED,
        surfaceIdentity,
        bitcast<u32>(surfaceT));
    // GPU-owned generated terrain is occupancy-only.  Its column builder has
    // no micro-surface sidecar, so an unmodified zero context labels every
    // visible cube as generic interior and makes the G-buffer light shell
    // bottoms and page-closing faces as authored geology.  Preserve authored
    // micro contexts, but give occupancy terrain the stable flat-heightfield
    // context needed by the shared terrain-normal path.
    let resolvedContext = select(
        microContext, SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE,
        terrainIdentity);
    output.bottomAlbedo =
        (bottomAlbedo & SPARSE_VOXEL_RESOLVED_BOTTOM_ALBEDO_MASK) |
        (resolvedContext << SPARSE_VOXEL_RESOLVED_CONTEXT_SHIFT);
    output.microPayload = brickMicroPayload(
        descriptorId, micro.cell, carveFace, subdivision, true,
        terrainSlope);
    let canonicalScale = u32(max(input.canonicalGridOriginScale.w, 1));
    let canonicalMicro = parentKey * i32(subdivision) +
        vec3<i32>(micro.cell * canonicalScale);
    output.materialSampleAddress = vec4<u32>(
        bitcast<vec3<u32>>(canonicalMicro), input.regionMappingId);
    return output;
}
#endif

@vertex
fn drawBrickVertex(
    @builtin(vertex_index) vertexIndex : u32,
    @builtin(instance_index) instanceIndex : u32) -> BrickVertexOutput {
    var output : BrickVertexOutput;
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
    let prepared = microBrickRecords[instanceIndex];
    let brickIndex = prepared.nodeIndex;
    let node = nodes[brickIndex];
#else
    let brickIndex = visibleBricks[instanceIndex];
    let node = nodes[brickIndex];
#endif

    let voxelSize = brickCanonicalVoxelSize(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId);
    let brickMinimum = brickCellBoundaryWorld(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(0));
    output.brickMinimumVoxelSize = vec4<f32>(brickMinimum, voxelSize);
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
    output.brickPacked = vec4<u32>(
        node.coarseOccupancyLow, node.coarseOccupancyHigh,
        node.occupancyIndex, node.materialBase);
    // Direct records rasterize a tight parent proxy and trace that one parent.
    // The separate sparse replay variant uses these source bounds only for the
    // edge/side-entry samples whose incumbent X/Y/Z ownership must be retained.
    output.brickBounds = node.packedOccupiedBounds;
    output.microRasterData = vec4<u32>(
        prepared.parentDescriptorFlags, prepared.paletteAddress,
        prepared.microContextData, 0u);
    output.microColumnCache = prepared.columns;
#else
#ifdef SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    // A brick that extracted nothing keeps the node's own coarse mask, and its
    // fine mask stays in the immutable source occupancy. Only extracting bricks
    // read the rebuilt residual record.
    let residualPassthrough = microResiduals[instanceIndex].passthrough != 0u;
    output.brickPacked = vec4<u32>(
        select(microResiduals[instanceIndex].coarseLow,
            node.coarseOccupancyLow, residualPassthrough),
        select(microResiduals[instanceIndex].coarseHigh,
            node.coarseOccupancyHigh, residualPassthrough),
        node.occupancyIndex, node.materialBase);
#else
    output.brickPacked = vec4<u32>(
        node.coarseOccupancyLow, node.coarseOccupancyHigh, node.occupancyIndex, node.materialBase);
#endif
    output.brickBounds = node.packedOccupiedBounds;
#ifdef SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
    output.microRasterData = vec4<u32>(
        instanceIndex, select(0u, 1u, residualPassthrough), 0u, 0u);
#endif
#endif
    output.paletteBase = node.paletteBase;
    output.terrainClip = node.instanceIndex;
    output.nodeLevel = node.level;
    output.canonicalGridOriginScale = node.canonicalGridOriginScale;
    output.regionMappingId = node.regionMappingId;

    // Draw the occupied sub-box, not the whole 8-cubed brick. Bricks on a
    // surface are mostly empty, so this removes both the fragments that could
    // only ever miss and the leading empty span of every DDA that does hit.
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
    let parentCell = vec3<u32>(
        prepared.parentDescriptorFlags & 7u,
        (prepared.parentDescriptorFlags >> 3u) & 7u,
        (prepared.parentDescriptorFlags >> 6u) & 7u);
    let minimum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(parentCell));
    let maximum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(parentCell + vec3<u32>(1u)));
    let extent = maximum - minimum;
    let camera = vec3<f32>(0.0);

    let insideSlab = (camera >= minimum) & (camera <= maximum);
    let insideSlabCount = select(0u, 1u, insideSlab.x) +
        select(0u, 1u, insideSlab.y) +
        select(0u, 1u, insideSlab.z);
    // If the eye is inside two slabs, the remaining camera-facing quad wraps
    // across the camera plane. Clipping its negative-W corners is not a
    // conservative projection and leaves a band with no proxy coverage. This
    // happens only for the few bricks immediately around the eye; cover the
    // screen and let the exact fragment DDA reject rays outside the box.
    if (insideSlabCount >= 2u) {
        let screenCorner = vec2<f32>(
            select(-1.0, 3.0, vertexIndex == 1u),
            select(-1.0, 3.0, vertexIndex == 2u));
        output.position = vec4<f32>(screenCorner, 1.0, 1.0);
        return output;
    }

    // Eighteen vertices reserve one parent face per axis. An axis contributes
    // a visible face only when the eye is outside that axis' slab. Choosing a
    // side from the box centre while the eye is inside the slab emits a hidden
    // face that flips sides as the camera crosses the centre and produces
    // view-dependent holes.
    let axis = vertexIndex / 6u;
    let vertex = vertexIndex % 6u;
    if (camera[axis] >= minimum[axis] &&
        camera[axis] <= maximum[axis]) {
        output.position = vec4<f32>(0.0);
        return output;
    }
    let corner2 = vec2<f32>(
        select(0.0, 1.0, vertex == 1u || vertex == 4u || vertex == 5u),
        select(0.0, 1.0, vertex == 2u || vertex == 3u || vertex == 5u));
    let centre = minimum + 0.5 * extent;
    let fixedValue = select(0.0, 1.0, camera[axis] > centre[axis]);
    var corner = vec3<f32>(fixedValue, corner2.x, corner2.y);
    if (axis == 1u) {
        corner = vec3<f32>(corner2.x, fixedValue, corner2.y);
    } else if (axis == 2u) {
        corner = vec3<f32>(corner2.x, corner2.y, fixedValue);
    }

    // Select the exact precomputed endpoints rather than subtracting and
    // re-adding the extent; proxy planes then share the fragment interval's
    // parent-boundary bits.
    let relativeWorld = select(minimum, maximum, corner == vec3<f32>(1.0));
    let clip = brickCameraRelativeClip(relativeWorld);
    // Near-depth proxy samples leave x/y/w geometric while making the exact
    // fragment DDA responsible for the final surface depth. Negative-w vertices
    // remain outside the homogeneous depth volume and clip.
    var rasterClip = vec4<f32>(clip.x, -clip.y, clip.w, clip.w);
    let centreClip = brickCameraRelativeClip(centre);
    if (clip.w > 0.0 && centreClip.w > 0.0) {
        let ndc = vec2<f32>(clip.x, -clip.y) / clip.w;
        let centreNdc =
            vec2<f32>(centreClip.x, -centreClip.y) / centreClip.w;
        let outward = select(
            vec2<f32>(-1.0), vec2<f32>(1.0), ndc >= centreNdc);
        // One full output pixel absorbs parent/LOD projection rounding and
        // face T-junctions without changing the exact fragment result.
        let expansion = vec2<f32>(2.0) /
            vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
        rasterClip.x = (ndc.x + outward.x * expansion.x) * clip.w;
        rasterClip.y = (ndc.y + outward.y * expansion.y) * clip.w;
    }
    output.position = rasterClip;
    return output;
#else
#ifdef SPARSE_VOXEL_BRICK_NO_MICRO
    // Terrain cull/HZB already use the tight occupied proxy. Rasterizing the
    // whole 8^3 brick here makes the depth proxy and the hardware proxy
    // disagree and sends huge empty regions into the fragment DDA. Keep the
    // same occupied bounds and rely on the conservative pixel expansion plus
    // the camera-inside/full-screen fallback below for crack safety.
    let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let minimum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.minimumCell));
    let maximum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.maximumCell));
#else
    let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let minimum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.minimumCell));
    let maximum = brickCellBoundaryCameraRelative(
        node.minimumVoxelSize, node.canonicalGridOriginScale,
        node.regionMappingId, vec3<i32>(cells.maximumCell));
#endif
    let extent = maximum - minimum;
    let camera = vec3<f32>(0.0);

    // Inside the box, front faces have vertices behind the eye and clip away.
    // Cover the screen at the near plane instead - the fragment's own slab test
    // rejects anything the box does not actually cover, so this only has to be
    // conservative, not tight.
    let insideSlab = (camera >= minimum) & (camera <= maximum);
    let insideSlabCount = select(0u, 1u, insideSlab.x) +
        select(0u, 1u, insideSlab.y) +
        select(0u, 1u, insideSlab.z);
    if (insideSlabCount >= 2u) {
        // Only the first triangle covers; the other two collapse onto it so the
        // instance still consumes its fixed 18 vertices.
        let corner = vec2<f32>(
            select(-1.0, 3.0, vertexIndex == 1u),
            select(-1.0, 3.0, vertexIndex == 2u));
        output.position = vec4<f32>(corner, 1.0, 1.0);
        return output;
    }

#ifdef SPARSE_VOXEL_BRICK_TERRAIN_PROXY_STRADDLE_FALLBACK
    // If a terrain brick straddles the eye plane, its camera-facing quads are
    // not a conservative screen envelope: homogeneous clipping can slice the
    // proxy triangles differently from the ray-box interval used by the
    // fragment DDA. This only happens very close to the camera, so cover the
    // screen and let the DDA be the sole geometry decision for that brick.
    var anyBehindEye = false;
    var anyInFrontEye = false;
    for (var cornerIndex = 0u; cornerIndex < 8u; cornerIndex = cornerIndex + 1u) {
        let cornerPoint = vec3<f32>(
            select(minimum.x, maximum.x, (cornerIndex & 1u) != 0u),
            select(minimum.y, maximum.y, (cornerIndex & 2u) != 0u),
            select(minimum.z, maximum.z, (cornerIndex & 4u) != 0u));
        let cornerClip = brickCameraRelativeClip(cornerPoint);
        anyBehindEye = anyBehindEye || cornerClip.w <= 0.0;
        anyInFrontEye = anyInFrontEye || cornerClip.w > 0.0;
    }
    if (anyBehindEye && anyInFrontEye) {
        let corner = vec2<f32>(
            select(-1.0, 3.0, vertexIndex == 1u),
            select(-1.0, 3.0, vertexIndex == 2u));
        output.position = vec4<f32>(corner, 1.0, 1.0);
        return output;
    }
#endif

    // The fixed draw reserves one face per axis. An axis has a camera-facing
    // box face only while the eye is outside its slab. If the eye is inside
    // the slab, selecting either side from the centre is wrong: it is a hidden
    // proxy, its interpolated depth is not conservative for the DDA entry, and
    // it abruptly flips sides at the centre plane. Collapse that axis' six
    // vertices; the one or two genuinely facing axes cover the convex box.
    let axis = vertexIndex / 6u;
    let vertex = vertexIndex % 6u;
    if (camera[axis] >= minimum[axis] &&
        camera[axis] <= maximum[axis]) {
        output.position = vec4<f32>(0.0);
        return output;
    }
    // The corner is bit arithmetic on the vertex index rather than a lookup
    // table: the two triangles of a quad are (0,0) (1,0) (0,1) and
    // (0,1) (1,0) (1,1).
    let corner2 = vec2<f32>(
        select(0.0, 1.0, vertex == 1u || vertex == 4u || vertex == 5u),
        select(0.0, 1.0, vertex == 2u || vertex == 3u || vertex == 5u));
    let centre = minimum + 0.5 * extent;
    let fixedValue = select(0.0, 1.0, camera[axis] > centre[axis]);
    var corner = vec3<f32>(fixedValue, corner2.x, corner2.y);
    if (axis == 1u) {
        corner = vec3<f32>(corner2.x, fixedValue, corner2.y);
    } else if (axis == 2u) {
        corner = vec3<f32>(corner2.x, corner2.y, fixedValue);
    }

    let relativeWorld = select(minimum, maximum, corner == vec3<f32>(1.0));
    let clip = brickCameraRelativeClip(relativeWorld);
    // The sparse raster projection is not Vulkan-Y-flipped: the rest of the
    // pipeline treats clip +Y as the top of the image, while the rasterizer
    // takes NDC +Y as downward. Everything else in this renderer reaches the
    // screen through compute or a full-screen triangle and never notices; this
    // is the only pass that puts world-space geometry through the rasterizer,
    // so it is the only one that has to reconcile the two.
    //
    // Depth clipping has to be defeated for the same reason: the far plane here
    // is sized from the initial camera framing and never grows, so a brick
    // whose front surface is comfortably in view can still have face vertices
    // past it. Clipping them throws away the whole brick, which reads as the
    // scene being cut off a short distance from the camera.
    //
    // Clamping z into the accepted range keeps the triangle from being clipped
    // without moving a single pixel: screen position is x/w and y/w, which are
    // untouched, and the interpolated depth is discarded anyway because the
    // fragment writes frag_depth from the DDA hit. Vertices behind the eye
    // (w <= 0) are left alone, since those must still clip.
    let clippedZ = select(clip.z, clamp(clip.z, 0.0, clip.w), clip.w > 0.0);
    var rasterClip = vec4<f32>(clip.x, -clip.y, clippedZ, clip.w);
    // Expand each projected face by three quarters of a pixel. Adjacent brick
    // faces can form T-junctions (especially across LOD levels), and exact
    // triangle coverage may otherwise leave a one-sample crack as projection
    // jitter moves the edge across the pixel centre. The fragment DDA remains
    // exact and discards every sample outside the brick, so this only makes
    // raster coverage conservative; it cannot create geometry.
    // Occupancy-only terrain still needs conservative *coverage*: adjacent
    // proxy faces meet through independent triangles and ordinary top-left
    // raster rules can leave a one-pixel T-junction hole while the camera
    // rotates. Pin its proxy depth to the near plane before expanding X/Y and
    // leave the fragment DDA as the final visibility/depth decision.
#ifdef SPARSE_VOXEL_BRICK_NO_MICRO
    if (clip.w > 0.0) {
        // Do not add a second, raster-only cull after the brick survived
        // frustum/HZB selection. Near depth keeps coverage conservative and
        // leaves the fragment DDA as the sole per-pixel geometry decision.
        rasterClip.z = clip.w;
        let centreClip = brickCameraRelativeClip(centre);
        if (centreClip.w > 0.0) {
            let ndc = vec2<f32>(clip.x, -clip.y) / clip.w;
            let centreNdc =
                vec2<f32>(centreClip.x, -centreClip.y) / centreClip.w;
            let outward = select(
                vec2<f32>(-1.0), vec2<f32>(1.0), ndc >= centreNdc);
            let expansion = vec2<f32>(2.0) /
                vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
            rasterClip.x = (ndc.x + outward.x * expansion.x) * clip.w;
            rasterClip.y = (ndc.y + outward.y * expansion.y) * clip.w;
        }
    }
#else
    let centreClip = brickCameraRelativeClip(centre);
    if (clip.w > 0.0 && centreClip.w > 0.0) {
        let ndc = vec2<f32>(clip.x, -clip.y) / clip.w;
        let centreNdc =
            vec2<f32>(centreClip.x, -centreClip.y) / centreClip.w;
        let outward = select(
            vec2<f32>(-1.0), vec2<f32>(1.0), ndc >= centreNdc);
        let expansion = vec2<f32>(1.5) /
        vec2<f32>(max(uniforms.viewport.xy, vec2<u32>(1u)));
        rasterClip.x = (ndc.x + outward.x * expansion.x) * clip.w;
        rasterClip.y = (ndc.y + outward.y * expansion.y) * clip.w;
    }
#endif
    // H5: hand this brick to the tiled compute raster by collapsing its proxy.
    // A degenerate position produces no fragments, which is how a vertex stage
    // declines an instance without an indirect compaction. The compute path
    // owns exactly the complement of this predicate.
    //
    // Only safe below the exact-microgeometry handoff: the compute walk does
    // not carve, so a routed brick must be one whose microvoxels the handoff
    // has already retired. The threshold is chosen against that, not freely.
    if (sparseVoxelBrickRoutedToCompute(
            uniforms.microTuning.y, brickProjectedFootprintPixels(node))) {
        output.position = vec4<f32>(0.0);
        return output;
    }
    output.position = rasterClip;
    return output;
#endif
}

@fragment
fn drawBrickFragment(input : BrickVertexOutput) -> BrickFragmentOutput {
#ifdef SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    let collectMicroCounters = uniforms.debugFlags.z != 0u;
#else
    // Compile the shipping variants with a constant-false gate. The compiler
    // removes every traversal counter, local accumulator, and atomic so a
    // diagnostic capture cannot contaminate the performance shader.
    let collectMicroCounters = false;
#endif
    // Counted before any `discard`, and before the depth test: overdraw is how
    // much work the rasterizer was asked to do for this pixel, which is the
    // question the view exists to answer. Counting survivors instead would
    // report at most one per pixel and say nothing.
#ifdef SPARSE_VOXEL_BRICK_RASTER_DIAGNOSTICS
    if (uniforms.debugFlags.x != 0u) {
        let viewport = max(uniforms.viewport.xy, vec2<u32>(1u));
        let pixel = vec2<u32>(input.position.xy);
        if (all(pixel < viewport)) {
            let pixelIndex = pixel.y * viewport.x + pixel.x;
            if (pixelIndex < arrayLength(&overdraw)) {
                atomicAdd(&overdraw[pixelIndex], 1u);
            }
        }
    }
#endif
    var output : BrickFragmentOutput;
    // Reverse-Z: 0.0 is the far plane, so an unwritten fragment is farthest.
    output.depth = 0.0;
    output.surface = vec4<u32>(0u);
    output.bottomAlbedo = 0u;
    output.microPayload = brickLodDebugPayload(input.nodeLevel);
    output.materialSampleAddress = vec4<u32>(0u);
    output.terrainOwnershipId = input.regionMappingId;

#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
#ifndef SPARSE_VOXEL_BRICK_PREPARED_TIE_REPLAY
    return drawPreparedMicroFragment(input, output);
#else
    if (!brickPreparedPotentialBoundaryAmbiguity(input)) {
        discard;
    }
#endif
#endif

#ifndef SPARSE_VOXEL_BRICK_DIRECT_ONLY
    let voxelSize = brickCanonicalVoxelSize(
        input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
        input.regionMappingId);
    let brickMinimum = brickCellBoundaryCameraRelative(
        input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
        input.regionMappingId, vec3<i32>(0));
#ifdef SPARSE_VOXEL_BRICK_NO_MICRO
    let cells = SparseVoxelBrickCellRange(
        vec3<f32>(0.0), vec3<f32>(f32(SPARSE_VOXEL_BRICK_SIDE)));
#else
    let cells = brickOccupiedCellRange(input.brickBounds);
#endif
    let minimum = brickCellBoundaryCameraRelative(
        input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
        input.regionMappingId, vec3<i32>(cells.minimumCell));
    let maximum = brickCellBoundaryCameraRelative(
        input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
        input.regionMappingId, vec3<i32>(cells.maximumCell));

    // The ray has to be the one the G-buffer will reconstruct for this pixel,
    // because surfaceT is measured along it. That is the camera ray encoded by
    // focalPrincipal (including the current TAA jitter), not the ray through
    // this fragment's interpolated face point.
    //
    // Written as a sum of three matrix columns rather than a mat4 product: the
    // w component of the direction is zero, so a quarter of that product is
    // multiplying by zero.
    let origin = vec3<f32>(0.0);
    let direction = brickCameraRayDirection(input.position);

    // One reciprocal for the whole fragment: the slab test and both DDA levels
    // need it, and it was being recomputed per axis in each.
    //
    // An axis the ray is parallel to needs no special case. Substituting a tiny
    // direction makes both slab distances run off to the same infinity, so the
    // axis either does not constrain the interval (origin inside the slab) or
    // collapses it to empty (origin outside) - which is exactly the branch it
    // used to take. It also keeps sign() away from zero, so the DDA step is
    // never stuck.
    let safeDirection = brickSafeDirection(direction);
    let inverseDirection = brickSafeInverseDirection(direction);
    let slabA = (minimum - origin) * inverseDirection;
    let slabB = (maximum - origin) * inverseDirection;
    let nearT = min(slabA, slabB);
    let farT = max(slabA, slabB);

    // Start no closer than the near plane. Inside a scene the camera regularly
    // sits within a coarse brick, and without this the DDA reports a hit at
    // t = 0: the surface is the eye point itself, its clip w collapses to ~0,
    // and the depth it writes wins the test over the whole screen.
    let nearDistance = max(uniforms.cameraWorldPosition.w, 1.0e-4);
    let tEnter = max(nearDistance, max(nearT.x, max(nearT.y, nearT.z)));
    let tExit = min(farT.x, min(farT.y, farT.z));
    if (tEnter > tExit || tExit < nearDistance) {
        if (collectMicroCounters) {
            atomicAdd(&microCounters.traversalMisses, 1u);
        }
        discard;
    }

    // The entered face is the axis whose near distance is largest, which the
    // slab test above has already computed - it used to be a second full pass
    // over the three slabs.
    var hitAxis = 0u;
    if (nearT.y > nearT.x) {
        hitAxis = 1u;
    }
    if (nearT.z > max(nearT.x, nearT.y)) {
        hitAxis = 2u;
    }

    let step = vec3<i32>(sign(safeDirection));
    // Boundary offset: the +1 face for a positive step, the +0 face otherwise.
    let boundaryOffset = max(step, vec3<i32>(0));

    // Coarse level: 4-cubed cells of two voxels each, mask held in registers.
    let coarseSize = voxelSize * 2.0;
    let coarseDelta = abs(vec3<f32>(coarseSize) * inverseDirection);
    let fineDelta = abs(vec3<f32>(voxelSize) * inverseDirection);
    let coarseLow = input.brickPacked.x;
    let coarseHigh = input.brickPacked.y;
    let occupancyIndex = input.brickPacked.z;
    // A non-zero region mapping identifies generated terrain. Its complete
    // geometric contract is the 512-bit (8x8x8) occupancy record. Neither a
    // retained-height patch nor a material descriptor may move or carve the
    // occupied cube, otherwise raster visibility and Tree64 lighting trace
    // different surfaces.
    let terrainOccupancyOnly = input.regionMappingId != 0u;

    let coarseSide = i32(SPARSE_VOXEL_BRICK_COARSE_SIDE);
    var coarseCell = clamp(
        vec3<i32>(floor((origin + direction * tEnter - brickMinimum) / coarseSize)),
        vec3<i32>(0),
        vec3<i32>(coarseSide - 1));
    var coarseNext = (brickCellBoundaryCameraRelative(
        input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
        input.regionMappingId,
        (coarseCell + boundaryOffset) * 2) - origin) * inverseDirection;

    // The camera can sit inside solid geometry. Reporting that cell would put a
    // surface at the eye, so skip the run of occupied cells the ray starts
    // inside and report the next surface it actually enters. Only when the eye
    // is genuinely inside this box: a ray entering from outside lands on an
    // occupied cell constantly, and that is the front surface.
    var insideSolid = all(origin >= minimum) && all(origin <= maximum);

    var travelled = tEnter;
    var hit = false;
    var hitIndex = 0u;
    var hitMaterial = SparseVoxelBrickMaterial(vec2<u32>(0u));
    var exactMicroHit = false;
    var hitMicroCell = vec3<u32>(0u);
    var hitMicroDescriptorId = 0u;
    var hitMicroCarveFace = 0u;
    var hitMicroSubdivision = 0u;
    var hitMicroHeightfield = false;
    var hitMicroFallback = false;
    var hitMicroTerrainSlope = false;
    var hitMicroContext = 0u;
    var hitCanonicalParentKey = vec3<i32>(0);
    var sawMicroCandidate = false;
#ifndef SPARSE_VOXEL_BRICK_NO_MICRO
#ifdef SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
    // Constant for the whole brick: its page voxel size against the limit.
    let microMaxVoxelBrick = bitcast<f32>(uniforms.microTuning.z);
    let microCoarsePageBrick =
        microMaxVoxelBrick > 0.0 && voxelSize > microMaxVoxelBrick;
#endif
#endif
    var scale4GroupsTested = 0u;
    var scale4GroupsSkipped = 0u;
    var scale2CellsVisited = 0u;
    var fineOccupancyProbes = 0u;

    // A ray crosses at most one boundary per axis per cell of span, so three
    // times the coarse side bounds the walk.
    for (var coarseStep = 0; coarseStep < 3 * coarseSide; coarseStep += 1) {
        if (any(coarseCell < vec3<i32>(0)) || any(coarseCell >= vec3<i32>(coarseSide))) {
            break;
        }
        if (collectMicroCounters) {
            scale2CellsVisited += 1u;
        }
        let coarseIndex = brickCoarseCellIndex(vec3<u32>(coarseCell));
        let coarseWord = select(coarseLow, coarseHigh, coarseIndex >= 32u);
        let coarseOccupied = (coarseWord & (1u << (coarseIndex & 31u))) != 0u;
        let coarseExit = min(coarseNext.x, min(coarseNext.y, coarseNext.z));

        if (coarseOccupied) {
            // Fine level: the 2-cubed block under this coarse cell. At most
            // four cells are visited, so the loop is short enough to unroll and
            // the occupancy words it reads are all in one cache line.
            let fineBase = coarseCell * 2;
            var fineCell = clamp(
                vec3<i32>(floor((origin + direction * travelled - brickMinimum) / voxelSize)),
                fineBase,
                fineBase + vec3<i32>(1));
            var fineNext = (brickCellBoundaryCameraRelative(
                input.brickMinimumVoxelSize,
                input.canonicalGridOriginScale, input.regionMappingId,
                fineCell + boundaryOffset) - origin) * inverseDirection;
            var fineT = travelled;
            var fineAxis = hitAxis;
            for (var fineStep = 0; fineStep < 4; fineStep += 1) {
                if (collectMicroCounters) {
                    fineOccupancyProbes += 1u;
                }
                let index = brickCellIndex(vec3<u32>(fineCell));
                let entry = occupancy[occupancyIndex].cells[index >> 5u];
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
                let occupied =
                    (entry.bits & (1u << (index & 31u))) != 0u;
#else
#ifdef SPARSE_VOXEL_BRICK_RESIDUAL_RASTER
                // `entry` is already loaded for the material prefix, so a
                // passthrough brick reads no residual record at all.
                var occupied = false;
                if (input.microRasterData.y != 0u) {
                    occupied = (entry.bits & (1u << (index & 31u))) != 0u;
                } else {
                    occupied =
                        (microResiduals[input.microRasterData.x].bits[
                             index >> 5u] &
                         (1u << (index & 31u))) != 0u;
                }
#else
                let occupied =
                    (entry.bits & (1u << (index & 31u))) != 0u;
#endif
#endif
                if (occupied && !insideSolid) {
                    let offset = entry.prefix + countOneBits(
                        entry.bits & ((1u << (index & 31u)) - 1u));
                    let materialSlot = input.brickPacked.w + offset;
                    let microContextData = brickMicroSurfaceData[materialSlot];
                    let microContext = microContextData.y >> 24u;
                    let terrainSurfaceData = vec2<u32>(
                        microContextData.x,
                        microContextData.y & 0x00ffffffu);
                    let microSurfaceData =
                        brickTerrainPatchLegacyData(terrainSurfaceData);
                    let paletteIndex =
                        (brickMaterialIndices[materialSlot >> 2u] >>
                            ((materialSlot & 3u) * 8u)) & 0xffu;
                    let paletteAddress = input.paletteBase + paletteIndex;
                    let material = brickPalette[paletteAddress];
                    var candidateAccepted = true;
                    var candidateDistance = fineT;
                    var candidateAxis = fineAxis;
                    var candidateMicroCell = vec3<u32>(0u);
                    var candidateDescriptorId = 0u;
                    var candidateIsMicro = false;
                    var candidateCarveFace = 0u;
                    var candidateSubdivision = 0u;
                    var candidateHeightfield = false;
                    var candidateFallback = false;
                    var candidateTerrainSlope = false;
                    var candidateCanonicalParentKey = vec3<i32>(0);
                    // The generated-terrain variant is a literal parent-voxel
                    // renderer. Do not let the legacy four-corner surface
                    // sidecar move a hit inside a voxel: raster and Tree64
                    // must see the same axis-aligned cube surface.
#ifdef SPARSE_VOXEL_BRICK_NO_MICRO
                    let candidateTerrainPatch = false;
#else
                    let candidateTerrainPatch =
                        !terrainOccupancyOnly &&
                        !brickTerrainPatchIsEmpty(terrainSurfaceData) &&
                        (microContext == SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE ||
                         (microContext &
                              SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u);
#endif
                    if (candidateTerrainPatch) {
                        let terrainHit = brickTraceTerrainSurfacePatch(
                            brickMinimum, vec3<u32>(fineCell), voxelSize,
                            origin, direction, fineT,
                            min(coarseExit,
                                min(fineNext.x,
                                    min(fineNext.y, fineNext.z))),
                            terrainSurfaceData);
                        candidateAccepted = terrainHit.valid;
                        candidateDistance = terrainHit.distance;
                        candidateAxis = select(
                            1u, fineAxis,
                            abs(terrainHit.distance - fineT) <=
                                voxelSize * 2.0e-4);
                        candidateTerrainSlope = true;
                    }

#ifndef SPARSE_VOXEL_BRICK_NO_MICRO
#ifdef SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
                    // Hoisted above the descriptor load and the entire
                    // handoff. A page coarser than the limit cannot produce
                    // exact microgeometry, and the measured cost of this block
                    // is the per-entry ALU - handoff weight, smoothstep,
                    // selector hash - paid on every eligible entry, not the
                    // tracing it guards. Testing at the bottom skipped only
                    // the trace and left the ALU in place.
                    //
                    // What is given up with it is the sub-pixel fallback
                    // payload, so those pixels reach the material pass without
                    // a descriptor or slope flag. The per-voxel terrain
                    // context byte is read above this block and survives.
                    if (!terrainOccupancyOnly && !candidateTerrainPatch &&
                        (material.packed.y & SPARSE_VOXEL_BRICK_MICRO_ENABLED) != 0u &&
                        !microCoarsePageBrick) {
#else
                    if (!terrainOccupancyOnly && !candidateTerrainPatch &&
                        (material.packed.y & SPARSE_VOXEL_BRICK_MICRO_ENABLED) != 0u) {
#endif
                        sawMicroCandidate = true;
                        if (collectMicroCounters) {
                            atomicAdd(&microCounters.eligibleParentHits, 1u);
                        }
                        if (paletteAddress < arrayLength(&brickPaletteMicroMaterials)) {
                            let microRecord =
                                brickPaletteMicroMaterials[paletteAddress];
                            let descriptorId = microRecord.z;
                            if (descriptorId != 0u) {
                                if (collectMicroCounters) {
                                    atomicAdd(&microCounters.descriptorLoads, 1u);
                                }
                                let descriptor = SparseVoxelMicroMaterial(
                                    microRecord.x, microRecord.y);
                                let subdivision = descriptor.geometry & 0x0fu;
                                let maximumDepthFixed =
                                    (descriptor.geometry >> 4u) & 0xffu;
                                let faceMask =
                                    (descriptor.geometry >> 12u) & 0x3fu;
                                let profile =
                                    (descriptor.geometry >> 18u) & 0x07u;
                                let descriptorValid = subdivision >= 2u &&
                                    subdivision <=
                                        SPARSE_VOXEL_EXACT_MICRO_MAX_SUBDIVISION &&
                                    maximumDepthFixed != 0u &&
                                    maximumDepthFixed <= subdivision * 16u &&
                                    faceMask != 0u &&
                                    (profile == SPARSE_VOXEL_EXACT_MICRO_CONSTANT ||
                                     profile == SPARSE_VOXEL_EXACT_MICRO_HASH_COLUMNS ||
                                     profile == SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY ||
                                     profile == SPARSE_VOXEL_EXACT_MICRO_CLUMPED_COLUMNS ||
                                     profile == SPARSE_VOXEL_EXACT_MICRO_STAIRCASE);
                                if (descriptorValid) {
                                    candidateTerrainSlope =
                                        profile ==
                                            SPARSE_VOXEL_EXACT_MICRO_STAIRCASE &&
                                        (microContext &
                                             SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
                                    let parentKey = brickMicroCanonicalParentKey(
                                        input.canonicalGridOriginScale,
                                        fineCell);
                                    candidateCanonicalParentKey = parentKey;
                                    candidateMicroCell = vec3<u32>(subdivision / 2u);
                                    let outputPixelScale = max(
                                        f32(uniforms.debugFlags.w) / 65536.0,
                                        1.0);
                                    let projectedMicroPixels =
                                        (voxelSize / f32(subdivision)) *
                                        min(abs(uniforms.focalPrincipal.x),
                                            abs(uniforms.focalPrincipal.y)) *
                                        outputPixelScale /
                                        (max(abs(fineT), 1.0e-4) *
                                         sparseVoxelMicroHandoffScale(
                                             uniforms.microTuning.x));
                                    let exactWeight =
                                        sparseVoxelExactMicroHandoffWeight(
                                            descriptor,
                                            projectedMicroPixels);
                                    let exactSelector =
                                        sparseVoxelExactMicroSelector(
                                            descriptor, descriptorId,
                                            parentKey, 0x51ed270bu);
#ifdef SPARSE_VOXEL_BRICK_EXACT_MICRO_DISTANCE_CUTOFF
                                    // This comparison exists only in the
                                    // separately compiled distance-cutoff
                                    // variant. fineT is measured in world
                                    // metres because the camera ray is
                                    // normalized. Beyond the cutoff, reuse the
                                    // same stable terrain/porous fallback as
                                    // the projected-pixel handoff below.
                                    // Two independent retirements, both only
                                    // in this variant. Distance retires a fine
                                    // page seen from far away; the voxel-size
                                    // limit retires a coarse clipmap ring
                                    // outright, whose microgeometry subdivides
                                    // a voxel larger than its own source data
                                    // and is therefore invented detail that
                                    // the projected-size handoff nonetheless
                                    // keeps alive to the horizon.
                                    // The page-coarseness test is hoisted to
                                    // the top of this block; only the distance
                                    // retirement remains per entry.
                                    let distanceCutoff = bitcast<f32>(
                                        uniforms.debugFlags.y);
                                    let useExact =
                                        (distanceCutoff <= 0.0 ||
                                         fineT <= distanceCutoff) &&
                                        (exactWeight >= 1.0 ||
                                         (exactWeight > 0.0 &&
                                          exactSelector < exactWeight));
#else
                                    let useExact = exactWeight >= 1.0 ||
                                        (exactWeight > 0.0 &&
                                         exactSelector < exactWeight);
#endif
                                    let terrainStaircase = profile ==
                                        SPARSE_VOXEL_EXACT_MICRO_STAIRCASE;
                                    let terrainStaircaseSurface =
                                        sparseVoxelExactMicroIsTerrainStaircaseSurface(
                                            profile, microContext);
                                    let staircaseInterior = terrainStaircase &&
                                        microContext ==
                                            SPARSE_VOXEL_MICRO_CONTEXT_INTERIOR;
                                    let staircaseFlat = terrainStaircase &&
                                        microContext ==
                                            SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE;
                                    let staircaseSlope = terrainStaircase &&
                                        (microContext &
                                            SPARSE_VOXEL_MICRO_CONTEXT_SURFACE_FLAG) != 0u;
                                    let staircaseWall = terrainStaircase &&
                                        microContext >=
                                            SPARSE_VOXEL_MICRO_CONTEXT_WALL_NEGATIVE_X &&
                                        microContext <=
                                            SPARSE_VOXEL_MICRO_CONTEXT_WALL_POSITIVE_Z;
                                    if (collectMicroCounters && terrainStaircase) {
                                        if (staircaseInterior) {
                                            atomicAdd(
                                                &microCounters.staircaseInteriorCandidates,
                                                1u);
                                        } else if (staircaseFlat) {
                                            atomicAdd(
                                                &microCounters.staircaseFlatCandidates,
                                                1u);
                                        } else if (staircaseSlope) {
                                            atomicAdd(
                                                &microCounters.staircaseSlopeCandidates,
                                                1u);
                                        } else if (staircaseWall) {
                                            atomicAdd(
                                                &microCounters.staircaseWallCandidates,
                                                1u);
                                        } else {
                                            atomicAdd(
                                                &microCounters.staircaseOtherCandidates,
                                                1u);
                                        }
                                    }
                                    if (!useExact) {
                                        if (collectMicroCounters) {
                                            atomicAdd(
                                                &microCounters.fallbackCandidates,
                                                1u);
                                        }
                                        candidateDescriptorId = descriptorId;
                                        candidateSubdivision = subdivision;
                                        candidateFallback = true;
                                        // Exact tracing now follows projected
                                        // microcell size at every brick LOD.
                                        // Once sub-pixel, retain expected
                                        // coverage with a parent-stable
                                        // selector instead of exposing a
                                        // clipmap/node-level distance ring.
                                        // Terrain is an opaque heightfield. A
                                        // stochastic whole-parent coverage
                                        // estimate turns its regular parent
                                        // grid into dotted contour rows during
                                        // the sub-pixel handoff. Conservatively
                                        // retain the solid parent instead. The
                                        // expected-coverage selector remains
                                        // correct for genuinely porous/clumped
                                        // microgeometry.
                                        if (!terrainStaircase) {
                                            let geometryExecutionFlags =
                                                sparseVoxelExactMicroGeometryExecutionFlags(
                                                    descriptorId);
                                            let expectedCoverage =
                                                sparseVoxelExactMicroExpectedCoverage(
                                                    descriptor, descriptorId,
                                                    geometryExecutionFlags,
                                                    parentKey, microContext,
                                                    microSurfaceData);
                                            let fallbackCoverage = mix(
                                                1.0, expectedCoverage,
                                                exactWeight);
                                            let coverageSelector =
                                                sparseVoxelExactMicroSelector(
                                                    descriptor, descriptorId,
                                                    parentKey, 0xa511e9b3u);
                                            if (coverageSelector >=
                                                fallbackCoverage) {
                                                candidateAccepted = false;
                                                if (collectMicroCounters) {
                                                    atomicAdd(
                                                        &microCounters.fallbackRejected,
                                                        1u);
                                                    atomicAdd(
                                                        &microCounters.carvedParents,
                                                        1u);
                                                    atomicAdd(
                                                        &microCounters.parentContinuations,
                                                        1u);
                                                }
                                            }
                                        }
                                    } else {
                                        let parentMinimum =
                                            brickCellBoundaryCameraRelative(
                                                input.brickMinimumVoxelSize,
                                                input.canonicalGridOriginScale,
                                                input.regionMappingId,
                                                fineCell);
                                        let localOrigin =
                                            (origin - parentMinimum) / voxelSize;
                                        let localDirection = direction / voxelSize;
                                        let legacyParentExit = min(
                                            coarseExit,
                                            min(fineNext.x,
                                                min(fineNext.y, fineNext.z)));
                                        let parentInterval =
                                            brickExactMicroParentInterval(
                                                brickMinimum,
                                                vec3<u32>(fineCell), voxelSize,
                                                origin, direction,
                                                nearDistance);
                                        let parentEnter = parentInterval.entry;
                                        let parentExit = parentInterval.exit;
                                        let parentFace = fineAxis |
                                            select(0u, 4u,
                                                direction[fineAxis] < 0.0);
                                        let geometryExecutionFlags =
                                            sparseVoxelExactMicroGeometryExecutionFlags(
                                                descriptorId);
                                        let geometryIREnabled =
                                            (geometryExecutionFlags &
                                             VOXEL_GEOMETRY_PROGRAM_ENABLED) != 0u;
                                        let geometryIRVolume =
                                            geometryIREnabled &&
                                            (geometryExecutionFlags & 2u) != 0u;
                                        let geometryIRFieldCached =
                                            geometryIREnabled &&
                                            (geometryExecutionFlags &
                                             VOXEL_GEOMETRY_PROGRAM_MATERIAL_FIELD_MASK) != 0u;
                                        let legacyHeightfield =
                                            (countOneBits(faceMask) == 1u ||
                                             terrainStaircaseSurface) &&
                                            profile !=
                                                SPARSE_VOXEL_EXACT_MICRO_VOLUME_DENSITY;
                                        let heightfield = select(
                                            legacyHeightfield,
                                            !geometryIRVolume &&
                                                !geometryIRFieldCached,
                                            geometryIREnabled);
                                        let solidStaircase =
                                            !geometryIREnabled &&
                                            staircaseInterior;
                                        if (collectMicroCounters) {
                                            atomicAdd(
                                                &microCounters.exactTraceInvocations,
                                                1u);
                                            if (staircaseInterior) {
                                                atomicAdd(
                                                    &microCounters.traceContextInterior,
                                                    1u);
                                            } else if (staircaseFlat) {
                                                atomicAdd(
                                                    &microCounters.traceContextFlat,
                                                    1u);
                                            } else if (staircaseSlope) {
                                                atomicAdd(
                                                    &microCounters.traceContextSlope,
                                                    1u);
                                            } else if (staircaseWall) {
                                                atomicAdd(
                                                    &microCounters.traceContextWall,
                                                    1u);
                                            } else if (heightfield) {
                                                atomicAdd(
                                                    &microCounters.traceContextOtherHeightfield,
                                                    1u);
                                            } else {
                                                atomicAdd(
                                                    &microCounters.traceContextGeneralVolume,
                                                    1u);
                                            }
                                            if (solidStaircase) {
                                                atomicAdd(
                                                    &microCounters.solidInteriorShortcuts,
                                                    1u);
                                            }
                                            if (!geometryIREnabled &&
                                                staircaseWall) {
                                                atomicAdd(
                                                    &microCounters.wallOneFaceEligible,
                                                    1u);
                                            }
                                            if (!solidStaircase) {
                                                if (heightfield) {
                                                    atomicAdd(
                                                        &microCounters.oneFaceInvocations,
                                                        1u);
                                                } else {
                                                    atomicAdd(
                                                        &microCounters.volumeInvocations,
                                                        1u);
                                                }
                                            }
                                        }
                                        var micro = sparseVoxelExactMicroMiss(
                                            parentExit, descriptorId, 0u);
                                        if (solidStaircase || heightfield) {
                                            micro = brickTraceExactMicroHeightfield(
                                                descriptor, descriptorId,
                                                geometryExecutionFlags,
                                                parentKey, brickMinimum,
                                                vec3<u32>(fineCell), voxelSize,
                                                origin, direction,
                                                nearDistance,
                                                parentFace, microContext,
                                                microSurfaceData,
                                                solidStaircase,
                                                // The legacy walk resolves the
                                                // parent itself and has no
                                                // prepared record to cache.
                                                vec4<u32>(0u));
                                        } else {
                                            micro = traceSparseVoxelExactMicroVolume(
                                                descriptor, descriptorId,
                                                geometryExecutionFlags, parentKey,
                                                localOrigin, localDirection,
                                                fineT, legacyParentExit,
                                                parentFace,
                                                microContext,
                                                microSurfaceData, 0u);
                                        }
                                        candidateAccepted = micro.valid;
                                        if (collectMicroCounters) {
                                            if (!solidStaircase) {
                                                if (heightfield) {
                                                    atomicAdd(
                                                        &microCounters.heightfieldSteps,
                                                        micro.steps);
                                                    if (micro.valid) {
                                                        atomicAdd(
                                                            &microCounters.oneFaceHits,
                                                            1u);
                                                    } else {
                                                        atomicAdd(
                                                            &microCounters.oneFaceMisses,
                                                            1u);
                                                    }
                                                } else {
                                                    atomicAdd(
                                                        &microCounters.volumeSteps,
                                                        micro.steps);
                                                    if (micro.valid) {
                                                        atomicAdd(
                                                            &microCounters.volumeHits,
                                                            1u);
                                                    } else {
                                                        atomicAdd(
                                                            &microCounters.volumeMisses,
                                                            1u);
                                                    }
                                                }
                                            }
                                            if (micro.valid) {
                                                atomicAdd(
                                                    &microCounters.acceptedMicroHits,
                                                    1u);
                                            } else {
                                                atomicAdd(
                                                    &microCounters.carvedParents,
                                                    1u);
                                                atomicAdd(
                                                    &microCounters.parentContinuations,
                                                    1u);
                                            }
                                        }
                                        if (micro.valid) {
                                            candidateDistance = micro.distance;
                                            candidateAxis = micro.face & 3u;
                                            candidateMicroCell = micro.cell;
                                            candidateDescriptorId = descriptorId;
                                            candidateIsMicro = true;
                                            candidateCarveFace = select(
                                                firstTrailingBit(faceMask), 3u,
                                                terrainStaircaseSurface);
                                            candidateSubdivision = subdivision;
                                            candidateHeightfield = heightfield;
                                        }
                                    }
                                }
                            }
                        }
                    }
#endif

                    if (candidateAccepted) {
                        hit = true;
                        hitAxis = candidateAxis;
                        hitIndex = index;
                        hitMaterial = material;
                        travelled = candidateDistance;
                        exactMicroHit = candidateIsMicro;
                        hitMicroCell = candidateMicroCell;
                        hitMicroDescriptorId = candidateDescriptorId;
                        hitMicroCarveFace = candidateCarveFace;
                        hitMicroSubdivision = candidateSubdivision;
                        hitMicroHeightfield = candidateHeightfield;
                        hitMicroFallback = candidateFallback;
                        hitMicroTerrainSlope = candidateTerrainSlope;
                        hitMicroContext = microContext;
                        hitCanonicalParentKey = candidateCanonicalParentKey;
                        break;
                    }
                }
                insideSolid = insideSolid && occupied;
                if (fineNext.x <= fineNext.y && fineNext.x <= fineNext.z) {
                    fineT = fineNext.x;
                    fineNext.x += fineDelta.x;
                    fineCell.x += step.x;
                    fineAxis = 0u;
                } else if (fineNext.y <= fineNext.z) {
                    fineT = fineNext.y;
                    fineNext.y += fineDelta.y;
                    fineCell.y += step.y;
                    fineAxis = 1u;
                } else {
                    fineT = fineNext.z;
                    fineNext.z += fineDelta.z;
                    fineCell.z += step.z;
                    fineAxis = 2u;
                }
                if (fineT > coarseExit || any(fineCell < fineBase) || any(fineCell > fineBase + vec3<i32>(1))) {
                    break;
                }
            }
            if (hit) {
                break;
            }
        } else {
            // An empty coarse cell is eight empty voxels, so a run the eye
            // started inside has ended here.
            insideSolid = false;

            // Binary level above the 4-cubed coarse grid: each aligned 2x2x2
            // group of coarse cells is one 4-cubed-voxel brick octant. Its
            // eight bits are already present in one half of the register-held
            // coarse mask, so testing it needs no extra storage load or ABI.
            //
            // Only take this path after proving the current coarse cell empty.
            // Dense/hit-first regions therefore retain the shorter two-level
            // walk, while a completely empty octant advances directly to its
            // exit plane.
            let octantMinimum = (coarseCell / vec3<i32>(2)) * 2;
            let octantWord = select(
                coarseLow, coarseHigh, octantMinimum.z >= 2);
            let octantShift = u32(
                octantMinimum.x + 4 * octantMinimum.y);
            let octantMask = 0x00330033u << octantShift;
            if (collectMicroCounters) {
                scale4GroupsTested += 1u;
            }
            // Generated terrain is only an 8^3 occupancy brick. Keep its walk
            // on the ordinary coarse/fine DDA: the empty-octant shortcut has
            // different simultaneous-boundary ownership and can jump past a
            // corner-touching solid cell, which appears as a one-pixel crack
            // while the camera moves. Non-terrain assets retain the shortcut.
            if (!terrainOccupancyOnly &&
                (octantWord & octantMask) == 0u) {
                if (collectMicroCounters) {
                    scale4GroupsSkipped += 1u;
                }
                let octantMaximum = octantMinimum + vec3<i32>(1);
                let nextCell = coarseCell + step;
                let internal = vec3<bool>(
                    nextCell.x >= octantMinimum.x &&
                        nextCell.x <= octantMaximum.x,
                    nextCell.y >= octantMinimum.y &&
                        nextCell.y <= octantMaximum.y,
                    nextCell.z >= octantMinimum.z &&
                        nextCell.z <= octantMaximum.z);
                // Reuse the ordinary DDA's accumulated boundary values instead
                // of recomputing them from world coordinates. Besides being
                // cheaper, this preserves its exact floating-point boundary
                // ownership: an internal boundary needs one more coarse delta
                // to reach the octant exit, while an outer boundary is already
                // the exit.
                let octantNext = select(
                    coarseNext, coarseNext + coarseDelta, internal);
                let octantExit = min(
                    octantNext.x, min(octantNext.y, octantNext.z));

                // Match the ordinary DDA's X/Y/Z tie priority. Boundaries
                // strictly before the octant exit have already been skipped.
                // At an exact tie, advance only an internal boundary whose
                // axis the ordinary walk would process before the selected
                // octant-exit axis. Other tied axes stay pending for the next
                // iteration, so a corner-touching occupied neighbour cannot be
                // skipped.
                var octantAxis = 0u;
                if (octantNext.y < octantNext.x) {
                    octantAxis = 1u;
                }
                if (octantNext.z < octantNext[octantAxis]) {
                    octantAxis = 2u;
                }
                if (internal.x &&
                    (coarseNext.x < octantExit ||
                     (octantAxis > 0u && coarseNext.x <= octantExit))) {
                    coarseNext.x += coarseDelta.x;
                    coarseCell.x += step.x;
                }
                if (internal.y &&
                    (coarseNext.y < octantExit ||
                     (octantAxis > 1u && coarseNext.y <= octantExit))) {
                    coarseNext.y += coarseDelta.y;
                    coarseCell.y += step.y;
                }
                if (internal.z && coarseNext.z < octantExit) {
                    coarseNext.z += coarseDelta.z;
                    coarseCell.z += step.z;
                }

                travelled = octantExit;
                if (octantAxis == 0u) {
                    coarseNext.x += coarseDelta.x;
                    coarseCell.x += step.x;
                } else if (octantAxis == 1u) {
                    coarseNext.y += coarseDelta.y;
                    coarseCell.y += step.y;
                } else {
                    coarseNext.z += coarseDelta.z;
                    coarseCell.z += step.z;
                }
                hitAxis = octantAxis;
                if (travelled > tExit) {
                    break;
                }
                continue;
            }
        }

        travelled = coarseExit;
        if (coarseNext.x <= coarseNext.y && coarseNext.x <= coarseNext.z) {
            coarseNext.x += coarseDelta.x;
            coarseCell.x += step.x;
            hitAxis = 0u;
        } else if (coarseNext.y <= coarseNext.z) {
            coarseNext.y += coarseDelta.y;
            coarseCell.y += step.y;
            hitAxis = 1u;
        } else {
            coarseNext.z += coarseDelta.z;
            coarseCell.z += step.z;
            hitAxis = 2u;
        }
        if (travelled > tExit) {
            break;
        }
    }
    if (!hit) {
        if (collectMicroCounters) {
            if (sawMicroCandidate) {
                atomicAdd(&microCounters.finalDiscards, 1u);
            }
            atomicAdd(&microCounters.scale4GroupsTested, scale4GroupsTested);
            atomicAdd(&microCounters.scale4GroupsSkipped, scale4GroupsSkipped);
            atomicAdd(&microCounters.scale2CellsVisited, scale2CellsVisited);
            atomicAdd(&microCounters.fineOccupancyProbes, fineOccupancyProbes);
            atomicAdd(&microCounters.traversalMisses, 1u);
        }
        discard;
    }
#ifdef SPARSE_VOXEL_BRICK_PREPARED_RASTER
    // Multiple prepared-parent proxies can overlap at conservative raster and
    // voxel boundaries. Replaying the source walk above establishes the same
    // single owner as the incumbent brick draw; only that owner's record may
    // reach the depth buffer.
    if (hitIndex != (input.microRasterData.x & 0x1ffu)) {
        discard;
    }
#endif
    if (collectMicroCounters) {
        atomicAdd(&microCounters.scale4GroupsTested, scale4GroupsTested);
        atomicAdd(&microCounters.scale4GroupsSkipped, scale4GroupsSkipped);
        atomicAdd(&microCounters.scale2CellsVisited, scale2CellsVisited);
        atomicAdd(&microCounters.fineOccupancyProbes, fineOccupancyProbes);
        if (fineOccupancyProbes == 1u) {
            atomicAdd(&microCounters.firstHitExits, 1u);
        }
    }

    let surfaceT = max(travelled, nearDistance);
    let relativeWorld = origin + direction * surfaceT;
    let world = uniforms.cameraWorldPosition.xyz + relativeWorld;
    // Terrain clipmap rings overlap by whole pages so they remain resident
    // while the camera crosses a page boundary. Only the finest resident
    // cascade may shade that overlap. Letting both resolutions reach the
    // depth buffer produces moving colour patches and exposes their different
    // height quantisation as cracks. The rectangle travels with the node
    // upload, so the ownership change is atomic with publishing the pages.
    if (brickFinerTerrainClipContainsCameraRelative(
            input.brickMinimumVoxelSize, input.canonicalGridOriginScale,
            input.regionMappingId, input.terrainClip, relativeWorld)) {
        discard;
    }
    let hitClip = brickCameraRelativeClip(relativeWorld);
    let clipZ = hitClip.z;
    let clipW = hitClip.w;
    if (clipW <= 0.0) {
        discard;
    }
    // Clamp rather than reject. Discarding out-of-range depth throws away every
    // surface past the far plane, which reads as the far plane having moved in;
    // the octree G-buffer writes its clip depth unconditionally and this has to
    // match it. NaN still has to go, since it would poison the depth test.
    let depth = clipZ / clipW;
    if (!(depth == depth)) {
        discard;
    }

    // Material index within the brick: the popcount of every occupancy bit
    // before this voxel. The prefix stored beside the word covers the preceding
    // words, so the run of loads and popcounts this used to be is now one
    // masked popcount on a value already in a register.
    let index = hitIndex;
    let material = hitMaterial;

    var bottomAlbedo = material.packed.x & 0x00ffffffu;
    if ((material.packed.y & SPARSE_VOXEL_BRICK_SECONDARY_ENABLED) != 0u) {
        let bottomIndex =
            (material.packed.y >>
                SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_SHIFT) &
            SPARSE_VOXEL_BRICK_SECONDARY_PALETTE_MASK;
        let bottomAddress = input.paletteBase + bottomIndex;
        if (bottomAddress < arrayLength(&brickPalette)) {
            bottomAlbedo =
                brickPalette[bottomAddress].packed.x & 0x00ffffffu;
        }
    }

    // Same packing the tile resolver uses: axis in the low bits, bit 2 set when
    // the ray entered through the positive face.
    let face = hitAxis | select(0u, 4u, direction[hitAxis] < 0.0);
    output.depth = clamp(depth, 0.0, 1.0);
    let terrainIdentity = input.regionMappingId != 0u;
    let parentIdentity = select(
        brickSurfaceIdentity(input.brickMinimumVoxelSize, index),
        brickTerrainSurfaceIdentity(
            input.brickMinimumVoxelSize,
            input.canonicalGridOriginScale, world),
        terrainIdentity);
    let detailedSurfaceIdentity = select(
        parentIdentity,
        brickMicroSurfaceIdentity(
            parentIdentity, hitMicroCell, face, hitMicroDescriptorId),
        exactMicroHit);
    let surfaceIdentity = select(
        detailedSurfaceIdentity, parentIdentity, terrainIdentity);
    output.surface = vec4<u32>(
        material.packed.x,
        // Source bit 31 is traversal eligibility. The resolved ABI uses it
        // only when this ray actually stopped on an exact microcell; otherwise
        // the material pass must shade the canonical solid terrain voxel.
        (material.packed.y & ~(
                SPARSE_VOXEL_BRICK_TRANSIENT_MASK |
                SPARSE_VOXEL_BRICK_MICRO_ENABLED)) |
            ((face | (SPARSE_VOXEL_SURFACE_HIT << 3u)) <<
                SPARSE_VOXEL_BRICK_SURFACE_SHIFT) |
            select(0u, SPARSE_VOXEL_BRICK_MICRO_ENABLED, exactMicroHit),
        surfaceIdentity,
        bitcast<u32>(surfaceT));
    let resolvedContext = select(
        hitMicroContext, SPARSE_VOXEL_MICRO_CONTEXT_FLAT_SURFACE,
        terrainIdentity);
    output.bottomAlbedo =
        (bottomAlbedo & SPARSE_VOXEL_RESOLVED_BOTTOM_ALBEDO_MASK) |
        (resolvedContext << SPARSE_VOXEL_RESOLVED_CONTEXT_SHIFT);
    output.microPayload = brickLodDebugPayload(input.nodeLevel) |
        select(0u, SPARSE_VOXEL_MICRO_PAYLOAD_TERRAIN_SLOPE,
            hitMicroTerrainSlope);
    if (terrainIdentity) {
        let hitCell = vec3<i32>(
            i32(index & 7u),
            i32((index >> 3u) & 7u),
            i32((index >> 6u) & 7u));
        let canonicalParent = brickMicroCanonicalParentKey(
            input.canonicalGridOriginScale, hitCell);
        output.materialSampleAddress = vec4<u32>(
            bitcast<vec3<u32>>(canonicalParent), input.regionMappingId);
    }
    if (exactMicroHit) {
        output.microPayload = brickMicroPayload(
            hitMicroDescriptorId, hitMicroCell, hitMicroCarveFace,
            hitMicroSubdivision, hitMicroHeightfield,
            hitMicroTerrainSlope);
    } else if (hitMicroFallback) {
        output.microPayload = brickMicroFallbackPayload(
            hitMicroDescriptorId, hitMicroTerrainSlope);
    }
    if (exactMicroHit || hitMicroFallback) {
        let canonicalScale = u32(max(input.canonicalGridOriginScale.w, 1));
        let canonicalMicro = hitCanonicalParentKey *
                i32(hitMicroSubdivision) +
            vec3<i32>(hitMicroCell * canonicalScale);
        output.materialSampleAddress = vec4<u32>(
            bitcast<vec3<u32>>(canonicalMicro), input.regionMappingId);
    }
    return output;
#endif
}
