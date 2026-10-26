#ifndef MAX_MIP_LEVELS_PER_PASS
#define MAX_MIP_LEVELS_PER_PASS 5
#endif

struct SPDConstants {
    readSize: vec2<u32>,
    dispatchSize: vec2<u32>,
    levelsToWrite: u32,
    copySourceToFirstMip: u32,
    numWorkgroups: u32,
}

struct AtomicCounter {
    value: atomic<u32>,
}

@group(0) @binding(0) var<uniform> constants: SPDConstants;
@group(0) @binding(1) var<storage, read_write> globalCounter: AtomicCounter;
@group(0) @binding(2) var srcTexture: texture_2d<f32>;

@group(0) @binding(3) var mipOut0: texture_storage_2d<r32float, read_write>;
#if MAX_MIP_LEVELS_PER_PASS >= 2
@group(0) @binding(4) var mipOut1: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 3
@group(0) @binding(5) var mipOut2: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 4
@group(0) @binding(6) var mipOut3: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 5
@group(0) @binding(7) var mipOut4: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 6
@group(0) @binding(8) var mipOut5: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 7
@group(0) @binding(9) var mipOut6: texture_storage_2d<r32float, read_write>;
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 8
@group(0) @binding(10) var mipOut7: texture_storage_2d<r32float, read_write>;
#endif

/// @brief Reduces a 2x2 block of depth values to a single representative value.
/// @param a Top-left sample.
/// @param b Top-right sample.
/// @param c Bottom-left sample.
/// @param d Bottom-right sample.
/// @returns The minimum (reverse-Z) or maximum (forward-Z) of the four samples.
fn spdReduce2x2(a: f32, b: f32, c: f32, d: f32) -> f32 {
#ifdef USE_REVERSE_Z
    return min(min(a, b), min(c, d));
#else
    return max(max(a, b), max(c, d));
#endif
}

/// @brief Returns whether this workgroup should process its slice (always true for the non-array variant).
/// @param workgroupId The 3-D workgroup ID (unused in this variant).
/// @returns True, indicating every workgroup is an active slice.
fn spdIsActiveSlice(workgroupId: vec3<u32>) -> bool {
    _ = workgroupId;
    return true;
}

/// @brief Returns the global atomic counter index for the given workgroup (always 0 for the non-array variant).
/// @param workgroupId The 3-D workgroup ID (unused in this variant).
/// @returns The counter index, always 0.
fn spdCounterIndex(workgroupId: vec3<u32>) -> u32 {
    _ = workgroupId;
    return 0u;
}

/// @brief Atomically increments the global workgroup completion counter and returns the new value.
/// @param counterIndex Index of the counter to increment (unused; always operates on globalCounter).
/// @returns The updated counter value after the increment.
fn spdCounterAdd(counterIndex: u32) -> u32 {
    _ = counterIndex;
    return atomicAdd(&globalCounter.value, 1u) + 1u;
}

/// @brief Resets the global workgroup completion counter to zero.
/// @param counterIndex Index of the counter to reset (unused; always resets globalCounter).
fn spdCounterReset(counterIndex: u32) {
    _ = counterIndex;
    atomicStore(&globalCounter.value, 0u);
}

/// @brief Loads a single texel from the source color/depth texture at the given coordinate.
/// @param coord The texel coordinate to sample (integer pixel position).
/// @param workgroupId The workgroup ID (unused in this variant).
/// @returns The red channel value of the loaded texel.
fn spdLoadSource(coord: vec2<u32>, workgroupId: vec3<u32>) -> f32 {
    _ = workgroupId;
    return textureLoad(srcTexture, vec2<i32>(coord), 0).r;
}

/// @brief Writes a depth value to the specified output mip level texture at the given coordinate.
/// @param mipIndex Zero-based index of the destination mip level (0..MAX_MIP_LEVELS_PER_PASS-1).
/// @param coord The texel coordinate to write.
/// @param workgroupId The workgroup ID (unused in this variant).
/// @param depth The depth value to store in the red channel.
fn spdWriteMip(mipIndex: u32, coord: vec2<u32>, workgroupId: vec3<u32>, depth: f32) {
    _ = workgroupId;
    let color = vec4<f32>(depth, 0.0, 0.0, 1.0);
    switch (mipIndex) {
        case 0u: { textureStore(mipOut0, vec2<i32>(coord), color); }
#if MAX_MIP_LEVELS_PER_PASS >= 2
        case 1u: { textureStore(mipOut1, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 3
        case 2u: { textureStore(mipOut2, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 4
        case 3u: { textureStore(mipOut3, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 5
        case 4u: { textureStore(mipOut4, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 6
        case 5u: { textureStore(mipOut5, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 7
        case 6u: { textureStore(mipOut6, vec2<i32>(coord), color); }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 8
        case 7u: { textureStore(mipOut7, vec2<i32>(coord), color); }
#endif
        default: {}
    }
}

/// @brief Reads a single depth value from the specified output mip level texture at the given coordinate.
/// @param mipIndex Zero-based index of the mip level to read from (0..MAX_MIP_LEVELS_PER_PASS-1).
/// @param coord The texel coordinate to read.
/// @param workgroupId The workgroup ID (unused in this variant).
/// @returns The depth value stored in the red channel, or 0.0 for out-of-range mip indices.
fn spdReadMip(mipIndex: u32, coord: vec2<u32>, workgroupId: vec3<u32>) -> f32 {
    _ = workgroupId;
    switch (mipIndex) {
        case 0u: { return textureLoad(mipOut0, vec2<i32>(coord)).r; }
#if MAX_MIP_LEVELS_PER_PASS >= 2
        case 1u: { return textureLoad(mipOut1, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 3
        case 2u: { return textureLoad(mipOut2, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 4
        case 3u: { return textureLoad(mipOut3, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 5
        case 4u: { return textureLoad(mipOut4, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 6
        case 5u: { return textureLoad(mipOut5, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 7
        case 6u: { return textureLoad(mipOut6, vec2<i32>(coord)).r; }
#endif
#if MAX_MIP_LEVELS_PER_PASS >= 8
        case 7u: { return textureLoad(mipOut7, vec2<i32>(coord)).r; }
#endif
        default: { return 0.0; }
    }
}

#include "../../common/wgsl/spd_core.wgsl"

/// @brief Compute entry point that runs the SPD depth pyramid reduction over a single pass using chunked workgroups.
@compute @workgroup_size(SPD_WORKGROUP_SIZE_X, SPD_WORKGROUP_SIZE_Y, 1)
fn depth_pyramid_spd_chunked(
    @builtin(local_invocation_id) localId: vec3<u32>,
    @builtin(workgroup_id) workgroupId: vec3<u32>,
    @builtin(local_invocation_index) localIndex: u32
) {
    spdRun(localId, workgroupId, localIndex);
}
