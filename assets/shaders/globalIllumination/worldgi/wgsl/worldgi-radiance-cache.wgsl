#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

fn worldgi_hash_u32(value : u32) -> u32 {
    var x = value;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

fn worldgi_hash_cell_key(cellKey : vec3<u32>) -> u32 {
    return worldgi_hash_u32(cellKey.x * 73856093u ^ cellKey.y * 19349663u ^ cellKey.z * 83492791u);
}

fn worldgi_radiance_cache_cell_key(position : vec3<f32>, cellSize : f32) -> vec3<u32> {
    let safeCellSize = max(cellSize, 1.0e-4);
    return vec3<u32>(bitcast<u32>(vec3<i32>(floor(position / safeCellSize))));
}

fn worldgi_radiance_cache_frame_age(currentFrame : u32, touchedFrame : u32) -> u32 {
    return select(0xffffffffu - touchedFrame + currentFrame, currentFrame - touchedFrame, currentFrame >= touchedFrame);
}

fn worldgi_radiance_cache_entry_alive(flags : u32, currentFrame : u32, touchedFrame : u32, ttlFrames : u32) -> bool {
    return (flags & WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_VALID) != 0u &&
        worldgi_radiance_cache_frame_age(currentFrame, touchedFrame) <= ttlFrames;
}
