struct TerrainFeedbackPackUniforms {
    pageCount : u32,
    requestWordCount : u32,
    reserved0 : u32,
    reserved1 : u32,
}

struct TerrainFeedbackRecord {
    slot : u32,
    generation : u32,
    priority : u32,
    desiredLOD : u32,
    flags : u32,
    reserved0 : u32,
    reserved1 : u32,
    reserved2 : u32,
}

const TERRAIN_FEEDBACK_REQUESTED : u32 = 1u;

@group(0) @binding(0) var<uniform> uniforms : TerrainFeedbackPackUniforms;
@group(0) @binding(1) var<storage, read> requestBits : array<u32>;
@group(0) @binding(2) var<storage, read> requestPriorities : array<u32>;
@group(0) @binding(3) var<storage, read> requestGenerations : array<u32>;
@group(0) @binding(4) var<storage, read> requestDesiredLODs : array<u32>;
@group(0) @binding(5) var<storage, read_write> packedFeedback :
    array<TerrainFeedbackRecord>;

@compute @workgroup_size(64)
fn packTerrainFeedback(@builtin(global_invocation_id) gid : vec3<u32>) {
    let slot = gid.x;
    if (slot >= uniforms.pageCount) {
        return;
    }

    let word = select(0u, requestBits[slot >> 5u],
        (slot >> 5u) < uniforms.requestWordCount);
    let requested = (word & (1u << (slot & 31u))) != 0u;
    packedFeedback[slot] = TerrainFeedbackRecord(
        slot,
        requestGenerations[slot],
        requestPriorities[slot],
        requestDesiredLODs[slot],
        select(0u, TERRAIN_FEEDBACK_REQUESTED, requested),
        0u,
        0u,
        0u);
}
