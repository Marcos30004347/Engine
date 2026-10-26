struct TerrainSyntheticWork {
    jobIdLow : u32,
    jobIdHigh : u32,
    stage : u32,
    cursor : u32,
    workUnits : u32,
    keyHash : u32,
    dispatchIdLow : u32,
    dispatchIdHigh : u32,
}

struct TerrainSyntheticResult {
    dispatchIdLow : u32,
    dispatchIdHigh : u32,
    checksum : u32,
    completed : u32,
}

struct TerrainSyntheticUniforms {
    stageIndex : u32,
    stageCapacity : u32,
    reserved0 : u32,
    reserved1 : u32,
}

@group(0) @binding(0) var<storage, read> work : array<TerrainSyntheticWork>;
@group(0) @binding(1) var<storage, read_write> results : array<TerrainSyntheticResult>;
@group(0) @binding(2) var<uniform> uniforms : TerrainSyntheticUniforms;

fn mix(value : u32) -> u32 {
    var mixed = value;
    mixed ^= mixed >> 16u;
    mixed *= 0x7feb352du;
    mixed ^= mixed >> 15u;
    mixed *= 0x846ca68bu;
    return mixed ^ (mixed >> 16u);
}

@compute @workgroup_size(64, 1, 1)
fn runTerrainSyntheticWork(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.stageCapacity) {
        return;
    }
    let index = uniforms.stageIndex * uniforms.stageCapacity + gid.x;
    let item = work[index];
    if (item.dispatchIdLow == 0u && item.dispatchIdHigh == 0u) {
        return;
    }
    var checksum = mix(item.keyHash ^ item.stage);
    checksum = mix(checksum ^ item.cursor);
    checksum = mix(checksum ^ item.workUnits);
    results[index] = TerrainSyntheticResult(
        item.dispatchIdLow, item.dispatchIdHigh, checksum, 1u);
}
