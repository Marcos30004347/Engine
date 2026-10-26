const THREAD_GROUP_SIZE : u32 = 512u;
const SEGMENT_SIZE : u32 = THREAD_GROUP_SIZE * 2u;
const STEP_COUNT : u32 = 10u;

struct PrefixUniforms {
    segmentCount : u32,
    _padding0 : vec3<u32>,
};

@group(0) @binding(0) var<uniform> uniforms : PrefixUniforms;
@group(0) @binding(1) var<storage, read_write> segments : array<u32>;

var<workgroup> sharedData : array<u32, SEGMENT_SIZE>;

@compute @workgroup_size(THREAD_GROUP_SIZE, 1, 1)
fn cs_main(@builtin(local_invocation_id) localId : vec3<u32>, @builtin(global_invocation_id) globalId : vec3<u32>) {
    let lane = localId.x;
    let index0 = globalId.x * 2u;
    let index1 = index0 + 1u;
    sharedData[lane * 2u + 0u] = select(0u, segments[index0], index0 < uniforms.segmentCount);
    sharedData[lane * 2u + 1u] = select(0u, segments[index1], index1 < uniforms.segmentCount);
    workgroupBarrier();

    for (var step = 0u; step < STEP_COUNT; step += 1u) {
        let mask = (1u << step) - 1u;
        let rd = ((lane >> step) << (step + 1u)) + mask;
        let wr = rd + 1u + (lane & mask);
        sharedData[wr] += sharedData[rd];
        workgroupBarrier();
    }

    if (index0 < uniforms.segmentCount) {
        segments[index0] = sharedData[lane * 2u + 0u];
    }
    if (index1 < uniforms.segmentCount) {
        segments[index1] = sharedData[lane * 2u + 1u];
    }
}
