const THREAD_GROUP_SIZE : u32 = 512u;
const SEGMENT_SIZE : u32 = THREAD_GROUP_SIZE * 2u;
const STEP_COUNT : u32 = 10u;

struct PrefixUniforms {
    gridEntryCount : u32,
    segmentCount : u32,
    _padding0 : vec2<u32>,
};

@group(0) @binding(0) var<uniform> uniforms : PrefixUniforms;
@group(0) @binding(1) var<storage, read_write> surfelGrid : array<u32>;
@group(0) @binding(2) var<storage, read_write> segments : array<u32>;

var<workgroup> sharedData : array<u32, SEGMENT_SIZE>;

@compute @workgroup_size(THREAD_GROUP_SIZE, 1, 1)
fn cs_main(@builtin(local_invocation_id) localId : vec3<u32>, @builtin(workgroup_id) workgroupId : vec3<u32>) {
    let lane = localId.x;
    let segment = workgroupId.x;
    if (segment >= uniforms.segmentCount) {
        return;
    }

    let baseIndex = segment * SEGMENT_SIZE;
    let index0 = baseIndex + lane * 2u;
    let index1 = index0 + 1u;
    sharedData[lane * 2u + 0u] = select(0u, surfelGrid[index0], index0 < uniforms.gridEntryCount);
    sharedData[lane * 2u + 1u] = select(0u, surfelGrid[index1], index1 < uniforms.gridEntryCount);
    workgroupBarrier();

    for (var step = 0u; step < STEP_COUNT; step += 1u) {
        let mask = (1u << step) - 1u;
        let rd = ((lane >> step) << (step + 1u)) + mask;
        let wr = rd + 1u + (lane & mask);
        sharedData[wr] += sharedData[rd];
        workgroupBarrier();
    }

    if (index0 < uniforms.gridEntryCount) {
        surfelGrid[index0] = sharedData[lane * 2u + 0u];
    }
    if (index1 < uniforms.gridEntryCount) {
        surfelGrid[index1] = sharedData[lane * 2u + 1u];
    }
    if (lane == THREAD_GROUP_SIZE - 1u) {
        segments[segment] = sharedData[SEGMENT_SIZE - 1u];
    }
}
