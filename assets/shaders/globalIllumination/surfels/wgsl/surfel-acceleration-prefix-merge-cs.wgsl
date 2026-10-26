const THREAD_GROUP_SIZE : u32 = 512u;
const SEGMENT_SIZE : u32 = THREAD_GROUP_SIZE * 2u;

struct PrefixUniforms {
    gridEntryCount : u32,
    segmentCount : u32,
    _padding0 : vec2<u32>,
};

@group(0) @binding(0) var<uniform> uniforms : PrefixUniforms;
@group(0) @binding(1) var<storage, read_write> surfelGrid : array<u32>;
@group(0) @binding(2) var<storage, read_write> surfelGridWriteCursor : array<u32>;
@group(0) @binding(3) var<storage, read> segments : array<u32>;

@compute @workgroup_size(THREAD_GROUP_SIZE, 1, 1)
fn cs_main(@builtin(local_invocation_id) localId : vec3<u32>, @builtin(workgroup_id) workgroupId : vec3<u32>) {
    let segment = workgroupId.x;
    if (segment >= uniforms.segmentCount) {
        return;
    }

    let prevSegmentSum = select(0u, segments[segment - 1u], segment != 0u);
    let baseIndex = segment * SEGMENT_SIZE;
    let index0 = baseIndex + localId.x * 2u;
    let index1 = index0 + 1u;

    if (index0 < uniforms.gridEntryCount) {
        let value0 = surfelGrid[index0] + prevSegmentSum;
        surfelGrid[index0] = value0;
        surfelGridWriteCursor[index0] = value0;
    }
    if (index1 < uniforms.gridEntryCount) {
        let value1 = surfelGrid[index1] + prevSegmentSum;
        surfelGrid[index1] = value1;
        surfelGridWriteCursor[index1] = value1;
    }
}
