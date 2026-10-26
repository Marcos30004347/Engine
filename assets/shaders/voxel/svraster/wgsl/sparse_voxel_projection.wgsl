fn transformPoint(matrix : mat4x4<f32>, point : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(point, 1.0)).xyz;
}

fn transformVector(matrix : mat4x4<f32>, vector : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(vector, 0.0)).xyz;
}

fn cameraQuadrant(corner : vec3<f32>, camera : vec3<f32>) -> u32 {
    return select(0u, 4u, corner.x < camera.x) |
           select(0u, 2u, corner.y < camera.y) |
           select(0u, 1u, corner.z < camera.z);
}

fn packPixel(pixel : vec2<u32>) -> u32 {
    return ((pixel.x & 0xffffu) << 16u) | (pixel.y & 0xffffu);
}

fn unpackPixel(packed : u32) -> vec2<u32> {
    return vec2<u32>(packed >> 16u, packed & 0xffffu);
}

fn clampedFloorPixel(value : vec2<f32>, extent : vec2<u32>) -> vec2<u32> {
    let bounded = clamp(value, vec2<f32>(0.0), vec2<f32>(extent));
    return vec2<u32>(floor(bounded));
}

fn clampedCeilPixel(value : vec2<f32>, extent : vec2<u32>) -> vec2<u32> {
    let bounded = clamp(value, vec2<f32>(0.0), vec2<f32>(extent));
    return vec2<u32>(ceil(bounded));
}

fn tileRectangle(bounds : vec2<u32>, tileCount : vec2<u32>) -> vec4<u32> {
    let minimum = unpackPixel(bounds.x) / SPARSE_VOXEL_TILE_SIZE;
    let maximum = unpackPixel(bounds.y) / SPARSE_VOXEL_TILE_SIZE;
    let lastTile = max(tileCount, vec2<u32>(1u)) - vec2<u32>(1u);
    return vec4<u32>(min(minimum, lastTile), min(maximum, lastTile));
}
