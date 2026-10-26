#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelBootstrapUniforms;
@group(0) @binding(1) var<storage, read_write> counters : SparseVoxelFrameCounters;
@group(0) @binding(2) var<storage, read_write> visibleInstances : array<VisibleInstance>;
@group(0) @binding(3) var<storage, read_write> indirectArgs : array<IndirectDispatchArgs>;

fn zeroDispatchArgs() -> IndirectDispatchArgs {
    return IndirectDispatchArgs(0u, 0u, 0u, 0u);
}

@compute @workgroup_size(64, 1, 1)
fn clearFrame(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < uniforms.visibleInstanceCapacity) {
        visibleInstances[index] = VisibleInstance(SPARSE_VOXEL_INVALID_INDEX, 0u, 0u, 0u);
    }
    if (index < SPARSE_VOXEL_INDIRECT_SLOT_COUNT) {
        indirectArgs[index] = zeroDispatchArgs();
    }
    if (index == 0u) {
        atomicStore(&counters.visibleInstanceCount, 0u);
        atomicStore(&counters.visibleVoxelCount, 0u);
        atomicStore(&counters.candidateCount, 0u);
        atomicStore(&counters.duplicateCount, 0u);
        atomicStore(&counters.overflowStage, SPARSE_VOXEL_OVERFLOW_NONE);
        atomicStore(&counters.requiredCount, 0u);
        atomicStore(&counters.capacity, 0u);
        atomicStore(&counters.occlusionCulledVoxelCount, 0u);
    }
}

fn recordOverflow(stage : u32, requiredCount : u32, capacity : u32) {
    atomicStore(&counters.visibleInstanceCount, 0u);
    atomicStore(&counters.visibleVoxelCount, 0u);
    atomicStore(&counters.overflowStage, stage);
    atomicStore(&counters.requiredCount, requiredCount);
    atomicStore(&counters.capacity, capacity);
    indirectArgs[SPARSE_VOXEL_INDIRECT_SLOT_PREPROCESS] = zeroDispatchArgs();
}

@compute @workgroup_size(1, 1, 1)
fn emitBootstrapVisibleInstance(@builtin(global_invocation_id) globalId : vec3<u32>) {
    if (globalId.x != 0u || uniforms.voxelCount == 0u) {
        return;
    }
    if (uniforms.visibleInstanceCapacity < 1u) {
        recordOverflow(SPARSE_VOXEL_OVERFLOW_VISIBLE_INSTANCES, 1u, uniforms.visibleInstanceCapacity);
        return;
    }
    if (uniforms.voxelCount > uniforms.visibleVoxelCapacity) {
        recordOverflow(SPARSE_VOXEL_OVERFLOW_VISIBLE_VOXELS, uniforms.voxelCount, uniforms.visibleVoxelCapacity);
        return;
    }

    let workgroupSize = max(uniforms.preprocessWorkgroupSize, 1u);
    visibleInstances[0] = VisibleInstance(0u, 0u, uniforms.voxelCount, 0u);
    atomicStore(&counters.visibleInstanceCount, 1u);
    atomicStore(&counters.visibleVoxelCount, uniforms.voxelCount);
    indirectArgs[SPARSE_VOXEL_INDIRECT_SLOT_PREPROCESS] = IndirectDispatchArgs(
        (uniforms.voxelCount + workgroupSize - 1u) / workgroupSize,
        1u,
        1u,
        0u);
}
