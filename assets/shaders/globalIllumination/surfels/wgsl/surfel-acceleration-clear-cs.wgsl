struct ClearUniforms {
    gridEntryCount : u32,
    _padding0 : vec3<u32>,
};

@group(0) @binding(0) var<uniform> uniforms : ClearUniforms;
@group(0) @binding(1) var<storage, read_write> surfelGrid : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> surfelGridWriteCursor : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read_write> surfelOwnerCell : array<atomic<u32>>;

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let index = globalInvocationId.x;
    if (index >= uniforms.gridEntryCount) {
        return;
    }

    atomicStore(&surfelGrid[index], 0u);
    atomicStore(&surfelGridWriteCursor[index], 0u);
    atomicStore(&surfelOwnerCell[index], 0xffffffffu);
}
