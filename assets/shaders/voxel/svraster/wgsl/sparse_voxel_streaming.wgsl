#include "sparse_voxel_types.wgsl"

@group(0) @binding(0) var<uniform> streaming : SparseVoxelStreamingPassUniforms;
@group(0) @binding(1) var<storage, read_write> pageRequestBits : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> pageRequestPriorities : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> pageRequestInstances : array<atomic<u32>>;
@group(0) @binding(4) var<storage, read> rewrites : array<SparseVoxelPhysicalPageRewrite>;
@group(0) @binding(5) var<storage, read_write> nodes : array<SparseVoxelLODNode>;

@compute @workgroup_size(64, 1, 1)
fn clearPageRequests(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index < streaming.requestWordCount) {
        atomicStore(&pageRequestBits[index], 0u);
    }
    if (index < streaming.pageCount) {
        atomicStore(&pageRequestPriorities[index], 0u);
        atomicStore(&pageRequestInstances[index], SPARSE_VOXEL_INVALID_INDEX);
    }
}

// Uploads and barriers are recorded before this kernel for installs. For an
// uninstall this kernel first publishes childCount=0; the physical range may
// only be returned to the allocator after the dispatch completes.
@compute @workgroup_size(64, 1, 1)
fn applyPhysicalPageRewrites(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let index = globalId.x;
    if (index >= streaming.rewriteCount) {
        return;
    }
    let rewrite = rewrites[index];
    if (rewrite.nodeIndex >= streaming.physicalNodeCapacity) {
        return;
    }
    nodes[rewrite.nodeIndex].firstChild = rewrite.firstChild;
    nodes[rewrite.nodeIndex].childCount = rewrite.childCount;
}
