#include "sparse_voxel_occupancy_common.wgsl"

struct SparseVoxelOccupancyProbeUniforms {
    state : SparseVoxelOccupancyVolume,
    probeCount : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

@group(0) @binding(0) var<uniform> probe : SparseVoxelOccupancyProbeUniforms;
@group(0) @binding(1) var occupancyTextureRead : texture_3d<u32>;
@group(0) @binding(2) var<storage, read> probeWorldPositions : array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> probeResults : array<u32>;

@compute @workgroup_size(64, 1, 1)
fn probeOccupancyAtWorldPositions(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x >= probe.probeCount) {
        return;
    }
    let worldPosition = probeWorldPositions[globalId.x].xyz;
    let globalCell = vec3<i32>(floor(worldPosition / probe.state.cellSize));
    let logical = globalCell - probe.state.originCell;
    if (probe.state.valid == 0u ||
        any(logical < vec3<i32>(0)) ||
        any(logical >= vec3<i32>(probe.state.logicalResolution))) {
        probeResults[globalId.x] = 0u;
        return;
    }
    let address = sparseVoxelOccupancyAddress(
        vec3<u32>(logical),
        probe.state);
    let wordCoord = sparseVoxelOccupancyWordCoord(
        address.wordIndex,
        probe.state);
    let word = textureLoad(
        occupancyTextureRead,
        vec3<i32>(wordCoord),
        0).r;
    probeResults[globalId.x] = select(
        0u,
        1u,
        (word & address.bitMask) != 0u);
}
