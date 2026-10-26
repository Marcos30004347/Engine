// Tree64 primary-ray G-buffer resolver.
//
// An alternative to the SVRaster project/duplicate/bin/resolve chain: march one
// primary ray per pixel through the camera-centred Tree64 clipmap and write the
// same ResolvedVoxelSurface contract the tile resolver writes, so every
// downstream pass - G-buffer, ReSTIR DI, light field, compose - is unchanged.
//
// The traversal itself is not reimplemented here. The trace pass derives each
// ray from its invocation index and a camera uniform (TREE64_PRIMARY_RAYS), so
// no ray array exists; this shader only interprets the results it leaves
// behind.
//
// Tree64 stores occupancy, not appearance: a hit yields a cell, and nothing maps
// a cell back to the octree voxel index the material array is keyed by. Hits are
// therefore flagged SPARSE_VOXEL_SURFACE_NO_MATERIAL and the G-buffer shades
// them with a neutral material. Geometry - depth, normal, motion - is exact.
#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_projection.wgsl"
#include "sparse_voxel_tree64_common.wgsl"

struct SparseVoxelTree64Result {
    globalCell : vec3<i32>,
    status : u32,
    distance : f32,
    skippedDistance : f32,
    queryCount : u32,
    baseCellSteps : u32,
    highestLevel : u32,
    contentGeneration : u32,
    reserved0 : u32,
    reserved1 : u32,
}

const TREE64_PRIMARY_STATUS_HIT : u32 = 2u;

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read> treeState : SparseVoxelTree64State;
@group(0) @binding(2) var<storage, read> results : array<SparseVoxelTree64Result>;
@group(0) @binding(3) var<storage, read_write> resolvedSurfaces : array<ResolvedVoxelSurface>;
@group(0) @binding(4) var<storage, read_write> coverage : array<u32>;
@group(0) @binding(5) var<storage, read_write> linearDepth : array<f32>;
@group(0) @binding(6) var<storage, read_write> materialSampleAddresses :
    array<VoxelMaterialSampleAddress>;

fn primaryPixel(globalId : vec3<u32>) -> vec2<u32> {
    return globalId.xy;
}

fn primaryRayDirection(pixel : vec2<u32>) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) / uniforms.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) / uniforms.focalPrincipal.y,
        1.0);
    return normalize(transformVector(uniforms.cameraToWorld, cameraDirection));
}

// World-space box of a Tree64 cell. The clipmap is axis aligned and uniformly
// spaced, and globalCell is an absolute cell coordinate - originCell only bounds
// which cells are resident - so the box follows from the cell size alone. Same
// convention as tree64BaseCellAt().
fn primaryCellMinimum(cell : vec3<i32>) -> vec3<f32> {
    return vec3<f32>(cell) * treeState.cellSize;
}

// Recover the entry face from the ray and the hit cell.
//
// The trace reports a cell and a distance but no face, and the G-buffer needs a
// normal. One slab test against the single reported cell is cheaper than
// threading a face through the traversal, and gives the same answer: the
// entered face is the axis whose near-plane distance is the largest.
fn primaryEntryFace(
    origin : vec3<f32>,
    direction : vec3<f32>,
    minimum : vec3<f32>,
    maximum : vec3<f32>) -> u32 {
    var bestAxis = 0u;
    var bestEntry = -3.402823466e+38;
    for (var axis = 0u; axis < 3u; axis += 1u) {
        let rayDirection = direction[axis];
        if (abs(rayDirection) <= 1.0e-20) {
            continue;
        }
        let t0 = (minimum[axis] - origin[axis]) / rayDirection;
        let t1 = (maximum[axis] - origin[axis]) / rayDirection;
        let nearT = min(t0, t1);
        if (nearT > bestEntry) {
            bestEntry = nearT;
            bestAxis = axis;
        }
    }
    // Same packing the tile resolver uses: low bits axis, bit 2 the sign of the
    // face the ray came from.
    return bestAxis | select(0u, 4u, direction[bestAxis] < 0.0);
}

@compute @workgroup_size(8, 8, 1)
fn resolveTree64PrimarySurfaces(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixel = primaryPixel(globalId);
    let viewport = uniforms.viewportTiles.xy;
    if (any(pixel >= viewport)) {
        return;
    }
    let pixelIndex = pixel.y * viewport.x + pixel.x;
    resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
        0.0, SPARSE_VOXEL_INVALID_INDEX, SPARSE_VOXEL_INVALID_INDEX,
        0u, 0u, 0u, 0u, 0u);
    coverage[pixelIndex] = 0u;
    linearDepth[pixelIndex] = 0.0;
    // Tree64 occupancy-only primary hits do not yet expose a material payload;
    // keep the companion address explicitly invalid instead of leaking a
    // physical traversal cell as a material mapping.
    materialSampleAddresses[pixelIndex] =
        VoxelMaterialSampleAddress(vec3<i32>(0), 0u);

    let result = results[pixelIndex];
    if (result.status != TREE64_PRIMARY_STATUS_HIT || !(result.distance >= 0.0)) {
        return;
    }

    let origin = uniforms.cameraWorldPositionNear.xyz;
    let direction = primaryRayDirection(pixel);
    let minimum = primaryCellMinimum(result.globalCell);
    let face = primaryEntryFace(
        origin, direction, minimum, minimum + treeState.cellSize);

    // The whole clipmap is one implicit instance in world space, so instance 0
    // is the identity transform the G-buffer already applies. Voxel index zero
    // is a placeholder: NO_MATERIAL tells the G-buffer not to read the material
    // array with it.
    resolvedSurfaces[pixelIndex] = ResolvedVoxelSurface(
        result.distance, 0u, 0u, face,
        SPARSE_VOXEL_SURFACE_HIT | SPARSE_VOXEL_SURFACE_NO_MATERIAL,
        0u, 0u, 0u);
    coverage[pixelIndex] = 1u;
    linearDepth[pixelIndex] = result.distance;
}
