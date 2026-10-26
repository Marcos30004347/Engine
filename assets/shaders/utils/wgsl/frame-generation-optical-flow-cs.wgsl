// Coarse-to-fine block optical flow. Motion follows the renderer convention:
// currentUV - previousUV. Each refinement level consumes normalized motion
// from the next coarser level and searches a small SAD window around it.

struct FlowUniforms {
    extent : vec4<u32>, // xy luma input, zw flow output
    controls : vec4<f32>, // x search radius, y error scale
    flags : vec4<u32>, // x history valid, y coarsest level
};

@group(0) @binding(0) var<uniform> uniforms : FlowUniforms;
@group(0) @binding(1) var previousLumaTexture : texture_2d<f32>;
@group(0) @binding(2) var currentLumaTexture : texture_2d<f32>;
@group(0) @binding(3) var previousFlowTexture : texture_2d<f32>;
@group(0) @binding(4) var outputFlowTexture : texture_storage_2d<rgba16float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

fn sample_flow(uv : vec2<f32>, extent : vec2<u32>) -> vec2<f32> {
    let pixel = clamp_pixel(vec2<i32>(uv * vec2<f32>(extent)), extent);
    return textureLoad(previousFlowTexture, pixel, 0).xy;
}

fn block_error(
    currentCenter : vec2<i32>, motion : vec2<f32>, extent : vec2<u32>) -> f32 {
    let previousCenter = vec2<f32>(currentCenter) - motion * vec2<f32>(extent);
    var error = 0.0;
    var weight = 0.0;
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let offset = vec2<i32>(x, y);
            let currentValue = textureLoad(
                currentLumaTexture,
                clamp_pixel(currentCenter + offset, extent), 0).x;
            let previousValue = textureLoad(
                previousLumaTexture,
                clamp_pixel(vec2<i32>(round(previousCenter)) + offset, extent), 0).x;
            error += abs(currentValue - previousValue);
            weight += 1.0;
        }
    }
    return error / max(weight, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let lumaExtent = uniforms.extent.xy;
    let flowExtent = uniforms.extent.zw;
    if (any(invocation.xy >= flowExtent)) {
        return;
    }
    if (uniforms.flags.x == 0u) {
        textureStore(outputFlowTexture, vec2<i32>(invocation.xy), vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    let uv = (vec2<f32>(invocation.xy) + vec2<f32>(0.5)) /
        vec2<f32>(flowExtent);
    var predicted = vec2<f32>(0.0);
    if (uniforms.flags.y == 0u) {
        predicted = sample_flow(uv, textureDimensions(previousFlowTexture));
    }
    let currentCenter = clamp_pixel(
        vec2<i32>(uv * vec2<f32>(lumaExtent)), lumaExtent);
    var bestMotion = predicted;
    var bestError = block_error(currentCenter, predicted, lumaExtent);
    let radius = i32(clamp(round(uniforms.controls.x), 1.0, 4.0));
    for (var y = -4; y <= 4; y++) {
        for (var x = -4; x <= 4; x++) {
            if (abs(x) <= radius && abs(y) <= radius) {
                let candidate = predicted +
                    vec2<f32>(f32(x), f32(y)) / vec2<f32>(lumaExtent);
                let error = block_error(currentCenter, candidate, lumaExtent);
                if (error < bestError) {
                    bestError = error;
                    bestMotion = candidate;
                }
            }
        }
    }
    let confidence = exp(-bestError * max(uniforms.controls.y, 0.1));
    textureStore(
        outputFlowTexture, vec2<i32>(invocation.xy),
        vec4<f32>(bestMotion, confidence, bestError));
}
