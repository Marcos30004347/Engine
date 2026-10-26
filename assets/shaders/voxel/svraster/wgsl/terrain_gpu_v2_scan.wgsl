struct TerrainGPUV2ScanParameters {
    count : u32,
    level0Blocks : u32,
    level1Blocks : u32,
    outputCapacity : u32,
    compactEnabled : u32,
    indirectWorkgroupSize : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct IndirectDispatchArgs {
    x : u32,
    y : u32,
    z : u32,
    reserved : u32,
}

@group(0) @binding(0) var<storage, read> scanInput : array<u32>;
@group(0) @binding(1) var<storage, read_write> scanOutput : array<u32>;
@group(0) @binding(2) var<storage, read_write> level0Sums : array<u32>;
@group(0) @binding(3) var<storage, read_write> level0Offsets : array<u32>;
@group(0) @binding(4) var<storage, read_write> level1Sums : array<u32>;
@group(0) @binding(5) var<storage, read_write> level1Offsets : array<u32>;
@group(0) @binding(6) var<storage, read_write> status : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> compactedIndices : array<u32>;
@group(0) @binding(8) var<storage, read_write> indirectArgs : IndirectDispatchArgs;
@group(0) @binding(9) var<uniform> parameters : TerrainGPUV2ScanParameters;

var<workgroup> sharedValues : array<u32, 256>;

fn checkedAdd(left : u32, right : u32) -> u32 {
    let result = left + right;
    if (result < left) {
        atomicStore(&status[1], 1u);
    }
    return result;
}

fn levelCount(level : u32) -> u32 {
    if (level == 0u) { return parameters.count; }
    if (level == 1u) { return parameters.level0Blocks; }
    return parameters.level1Blocks;
}

fn loadLevel(level : u32, index : u32) -> u32 {
    if (level == 0u) { return scanInput[index]; }
    if (level == 1u) { return level0Sums[index]; }
    return level1Sums[index];
}

fn storeLevel(level : u32, index : u32, value : u32) {
    if (level == 0u) {
        scanOutput[index] = value;
    } else if (level == 1u) {
        level0Offsets[index] = value;
    } else {
        level1Offsets[index] = value;
    }
}

fn storeBlockSum(level : u32, index : u32, value : u32) {
    if (level == 0u) {
        level0Sums[index] = value;
    } else if (level == 1u) {
        level1Sums[index] = value;
    }
}

@compute @workgroup_size(1, 1, 1)
fn resetTerrainGPUV2Scan() {
    atomicStore(&status[0], 0u);
    atomicStore(&status[1], 0u);
    atomicStore(&status[2], parameters.outputCapacity);
    atomicStore(&status[3], 0u);
    indirectArgs = IndirectDispatchArgs(0u, 1u, 1u, 0u);
}

// The pass stage is supplied through reserved0: 0 scans input, 1 scans its
// block sums, and 2 scans the second-level sums.
@compute @workgroup_size(256, 1, 1)
fn scanTerrainGPUV2Blocks(
    @builtin(local_invocation_id) localId : vec3<u32>,
    @builtin(workgroup_id) workgroupId : vec3<u32>) {
    let level = parameters.reserved0;
    let count = levelCount(level);
    let index = workgroupId.x * 256u + localId.x;
    var inputValue = 0u;
    if (index < count) {
        inputValue = loadLevel(level, index);
    }
    sharedValues[localId.x] = inputValue;
    workgroupBarrier();

    var stride = 1u;
    while (stride < 256u) {
        let destination = (localId.x + 1u) * stride * 2u - 1u;
        if (destination < 256u) {
            sharedValues[destination] = checkedAdd(
                sharedValues[destination], sharedValues[destination - stride]);
        }
        stride *= 2u;
        workgroupBarrier();
    }

    if (localId.x == 255u) {
        storeBlockSum(level, workgroupId.x, sharedValues[255u]);
        sharedValues[255u] = 0u;
    }
    workgroupBarrier();

    stride = 128u;
    while (stride > 0u) {
        let destination = (localId.x + 1u) * stride * 2u - 1u;
        if (destination < 256u) {
            let left = sharedValues[destination - stride];
            sharedValues[destination - stride] = sharedValues[destination];
            sharedValues[destination] = checkedAdd(sharedValues[destination], left);
        }
        stride /= 2u;
        workgroupBarrier();
    }
    if (index < count) {
        storeLevel(level, index, sharedValues[localId.x]);
    }
}

// reserved0 == 3 adds level-1 prefixes to level-0 offsets. reserved0 == 4
// adds level-0 prefixes to the element output.
@compute @workgroup_size(256, 1, 1)
fn addTerrainGPUV2ScanBlocks(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (parameters.reserved0 == 3u) {
        if (gid.x < parameters.level0Blocks) {
            level0Offsets[gid.x] = checkedAdd(
                level0Offsets[gid.x], level1Offsets[gid.x / 256u]);
        }
    } else if (gid.x < parameters.count) {
        scanOutput[gid.x] = checkedAdd(
            scanOutput[gid.x], level0Offsets[gid.x / 256u]);
    }
}

@compute @workgroup_size(64, 1, 1)
fn validateTerrainGPUV2Compaction(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (parameters.compactEnabled != 0u && gid.x < parameters.count &&
        scanInput[gid.x] > 1u) {
        atomicStore(&status[1], 1u);
    }
}

@compute @workgroup_size(1, 1, 1)
fn finalizeTerrainGPUV2Scan() {
    var total = 0u;
    if (parameters.count != 0u) {
        let last = parameters.count - 1u;
        total = checkedAdd(scanOutput[last], scanInput[last]);
    }
    atomicStore(&status[0], total);
    let overflow = atomicLoad(&status[1]) != 0u ||
        (parameters.compactEnabled != 0u && total > parameters.outputCapacity);
    atomicStore(&status[1], select(0u, 1u, overflow));
    if (!overflow) {
        indirectArgs = IndirectDispatchArgs(
            (total + parameters.indirectWorkgroupSize - 1u) /
                parameters.indirectWorkgroupSize,
            1u, 1u, 0u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn emitTerrainGPUV2Compaction(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (parameters.compactEnabled == 0u || gid.x >= parameters.count ||
        atomicLoad(&status[1]) != 0u) {
        return;
    }
    let flag = scanInput[gid.x];
    if (flag == 1u) {
        let destination = scanOutput[gid.x];
        if (destination < parameters.outputCapacity) {
            compactedIndices[destination] = gid.x;
        }
    } else if (flag != 0u) {
        atomicStore(&status[1], 1u);
        indirectArgs.x = 0u;
    }
}
