@group(0) @binding(0) var<storage, read_write> statistics : array<atomic<u32>>;

@compute @workgroup_size(1, 1, 1)
fn resetSparseVoxelSceneRayStatistics() {
    for (var index = 0u; index < min(arrayLength(&statistics), 28u); index = index + 1u) {
        atomicStore(&statistics[index], 0u);
    }
}
