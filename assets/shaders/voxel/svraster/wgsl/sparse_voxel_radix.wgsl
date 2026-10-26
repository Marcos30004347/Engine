#include "sparse_voxel_types.wgsl"

const HELPER_WORKGROUP_SIZE : u32 = 256u;
const RADIX_BLOCK_SIZE : u32 = 256u * 4u;
const RADIX_BIN_COUNT : u32 = 16u;

struct RadixDispatchArgs {
    x : u32,
    y : u32,
    z : u32,
}

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRadixUniforms;
@group(0) @binding(1) var<storage, read> candidateCount : array<u32>;
@group(0) @binding(2) var<storage, read_write> keys : array<u32>;
@group(0) @binding(3) var<storage, read_write> indices : array<u32>;
@group(0) @binding(4) var<storage, read> unsortedCandidates : array<TileCandidate>;
@group(0) @binding(5) var<storage, read_write> sortedCandidates : array<TileCandidate>;
@group(0) @binding(6) var<storage, read_write> dispatchArgs : array<RadixDispatchArgs>;

fn divCeil(value : u32, divisor : u32) -> u32 {
    return (value + divisor - 1u) / divisor;
}

fn itemCount() -> u32 {
    return min(candidateCount[0], uniforms.candidateCapacity);
}

@compute @workgroup_size(1, 1, 1)
fn prepareRadixDispatch() {
    let count = itemCount();
    let radixWorkgroups = divCeil(count, RADIX_BLOCK_SIZE);
    let countEntries = radixWorkgroups * RADIX_BIN_COUNT;
    let reduceWorkgroups = RADIX_BIN_COUNT * divCeil(radixWorkgroups, RADIX_BLOCK_SIZE);
    dispatchArgs[0] = RadixDispatchArgs(divCeil(countEntries, HELPER_WORKGROUP_SIZE), 1u, 1u);
    dispatchArgs[1] = RadixDispatchArgs(radixWorkgroups, 1u, 1u);
    dispatchArgs[2] = RadixDispatchArgs(reduceWorkgroups, 1u, 1u);
    dispatchArgs[3] = RadixDispatchArgs(select(0u, 1u, count != 0u), 1u, 1u);
    dispatchArgs[4] = RadixDispatchArgs(reduceWorkgroups, 1u, 1u);
    dispatchArgs[5] = RadixDispatchArgs(radixWorkgroups, 1u, 1u);
    dispatchArgs[6] = RadixDispatchArgs(divCeil(count, HELPER_WORKGROUP_SIZE), 1u, 1u);
}

@compute @workgroup_size(HELPER_WORKGROUP_SIZE, 1, 1)
fn extractLowKeys(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < itemCount()) {
        keys[index] = unsortedCandidates[index].keyLo;
        indices[index] = index;
    }
}

@compute @workgroup_size(HELPER_WORKGROUP_SIZE, 1, 1)
fn extractHighKeys(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < itemCount()) {
        keys[index] = unsortedCandidates[indices[index]].keyHi;
    }
}

// High-word-only ordering. extractHighKeys reads through the permutation left
// behind by a preceding low-word sort; this variant seeds the identity
// permutation itself, so the high word can be sorted on its own when the
// caller does not need the low word ordered.
@compute @workgroup_size(HELPER_WORKGROUP_SIZE, 1, 1)
fn extractHighKeysSeeded(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < itemCount()) {
        keys[index] = unsortedCandidates[index].keyHi;
        indices[index] = index;
    }
}

@compute @workgroup_size(HELPER_WORKGROUP_SIZE, 1, 1)
fn gatherSortedCandidates(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < itemCount()) {
        sortedCandidates[index] = unsortedCandidates[indices[index]];
    }
}
