// Combines the raster motion field with coarse-to-fine optical flow, computes
// independent current/previous validity, and leaves low-confidence pixels as
// holes (alpha < 1) for the color-pyramid inpainting stages.

struct InterpolationUniforms {
    outputViewport : vec4<u32>, // xy output, zw game-motion extent
    controls : vec4<f32>, // t, depth threshold, max motion px, clamp gamma
    flags : vec4<u32>, // history valid, debug mode
};

struct Candidate {
    color : vec4<f32>,
    confidence : f32,
    motion : vec2<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : InterpolationUniforms;
@group(0) @binding(1) var previousColorTexture : texture_2d<f32>;
@group(0) @binding(2) var currentColorTexture : texture_2d<f32>;
@group(0) @binding(3) var previousDepthTexture : texture_2d<f32>;
@group(0) @binding(4) var currentDepthTexture : texture_2d<f32>;
@group(0) @binding(5) var currentMotionTexture : texture_2d<f32>;
@group(0) @binding(6) var disocclusionTexture : texture_2d<f32>;
@group(0) @binding(7) var gameMotion0Texture : texture_2d<f32>;
@group(0) @binding(8) var gameMotion1Texture : texture_2d<f32>;
@group(0) @binding(9) var gameMotion2Texture : texture_2d<f32>;
@group(0) @binding(10) var gameMotion3Texture : texture_2d<f32>;
@group(0) @binding(11) var opticalFlowTexture : texture_2d<f32>;
@group(0) @binding(12) var outputTexture : texture_storage_2d<rgba16float, write>;

fn clamp_pixel(pixel : vec2<i32>, extent : vec2<u32>) -> vec2<i32> {
    return clamp(pixel, vec2<i32>(0), vec2<i32>(extent) - vec2<i32>(1));
}

fn sample_bilinear(
    inputTexture : texture_2d<f32>, uv : vec2<f32>, extent : vec2<u32>) -> vec4<f32> {
    let position = clamp(uv, vec2<f32>(0.0), vec2<f32>(1.0)) *
        vec2<f32>(extent) - vec2<f32>(0.5);
    let base = vec2<i32>(floor(position));
    let fraction = fract(position);
    let c00 = textureLoad(inputTexture, clamp_pixel(base, extent), 0);
    let c10 = textureLoad(inputTexture, clamp_pixel(base + vec2<i32>(1, 0), extent), 0);
    let c01 = textureLoad(inputTexture, clamp_pixel(base + vec2<i32>(0, 1), extent), 0);
    let c11 = textureLoad(inputTexture, clamp_pixel(base + vec2<i32>(1, 1), extent), 0);
    return mix(mix(c00, c10, fraction.x), mix(c01, c11, fraction.x), fraction.y);
}

fn sample_nearest(
    inputTexture : texture_2d<f32>, uv : vec2<f32>, extent : vec2<u32>) -> vec4<f32> {
    return textureLoad(
        inputTexture,
        clamp_pixel(vec2<i32>(uv * vec2<f32>(extent)), extent), 0);
}

fn luma(color : vec3<f32>) -> f32 {
    return dot(color, vec3<f32>(0.2126, 0.7152, 0.0722));
}

fn color_error(a : vec3<f32>, b : vec3<f32>) -> f32 {
    let scale = max(max(luma(a), luma(b)), 0.05);
    return abs(luma(a) - luma(b)) / scale +
        0.2 * length((a - vec3<f32>(luma(a))) -
                     (b - vec3<f32>(luma(b)))) / scale;
}

fn clamp_history(color : vec3<f32>, uv : vec2<f32>, extent : vec2<u32>) -> vec3<f32> {
    let center = clamp_pixel(vec2<i32>(uv * vec2<f32>(extent)), extent);
    var low = vec3<f32>(1.0e20);
    var high = vec3<f32>(-1.0e20);
    for (var y = -1; y <= 1; y++) {
        for (var x = -1; x <= 1; x++) {
            let value = textureLoad(
                currentColorTexture,
                clamp_pixel(center + vec2<i32>(x, y), extent), 0).rgb;
            low = min(low, value);
            high = max(high, value);
        }
    }
    let midpoint = 0.5 * (low + high);
    let radius = 0.5 * (high - low) * max(uniforms.controls.w, 0.0);
    return clamp(color, midpoint - radius, midpoint + radius);
}

fn game_motion(uv : vec2<f32>, motionExtent : vec2<u32>) -> vec3<f32> {
    let raw = sample_nearest(currentMotionTexture, uv, motionExtent).xy;
    let disocclusion = clamp(
        sample_nearest(disocclusionTexture, uv, uniforms.outputViewport.xy).x,
        0.0, 1.0);
    let maximumMotion = max(uniforms.controls.z, 1.0);
    let plausible = all(abs(raw) < vec2<f32>(1.0e3)) &&
        length(raw * vec2<f32>(uniforms.outputViewport.xy)) <= maximumMotion;
    var selected = vec4<f32>(raw, 0.0, select(0.0, 1.0 - disocclusion, plausible));
    let level0 = sample_nearest(
        gameMotion0Texture, uv, textureDimensions(gameMotion0Texture));
    let level1 = sample_nearest(
        gameMotion1Texture, uv, textureDimensions(gameMotion1Texture));
    let level2 = sample_nearest(
        gameMotion2Texture, uv, textureDimensions(gameMotion2Texture));
    let level3 = sample_nearest(
        gameMotion3Texture, uv, textureDimensions(gameMotion3Texture));
    if (selected.a < 0.25 && level0.a > selected.a) { selected = level0; }
    if (selected.a < 0.25 && level1.a > selected.a) { selected = level1; }
    if (selected.a < 0.25 && level2.a > selected.a) { selected = level2; }
    if (selected.a < 0.25 && level3.a > selected.a) { selected = level3; }
    return vec3<f32>(selected.xy, clamp(selected.a, 0.0, 1.0));
}

fn evaluate_candidate(
    midpointUv : vec2<f32>, inputMotion : vec2<f32>,
    sourceConfidence : f32) -> Candidate {
    let outputExtent = uniforms.outputViewport.xy;
    var motion = inputMotion;
    let motionPixels = motion * vec2<f32>(outputExtent);
    let motionLength = length(motionPixels);
    let maximumMotion = max(uniforms.controls.z, 1.0);
    if (motionLength > maximumMotion) {
        motion *= maximumMotion / motionLength;
    }
    let t = clamp(uniforms.controls.x, 0.0, 1.0);
    let currentUv = midpointUv + motion * t;
    let previousUv = midpointUv - motion * (1.0 - t);
    let inBounds = all(currentUv >= vec2<f32>(0.0)) &&
        all(currentUv <= vec2<f32>(1.0)) &&
        all(previousUv >= vec2<f32>(0.0)) &&
        all(previousUv <= vec2<f32>(1.0));
    let current = sample_bilinear(currentColorTexture, currentUv, outputExtent);
    var previous = sample_bilinear(previousColorTexture, previousUv, outputExtent);
    previous = vec4<f32>(
        clamp_history(previous.rgb, currentUv, outputExtent), previous.a);
    let currentDepth = sample_nearest(currentDepthTexture, currentUv, outputExtent).x;
    let previousDepth = sample_nearest(previousDepthTexture, previousUv, outputExtent).x;
    let relativeDepth = abs(currentDepth - previousDepth) /
        max(max(abs(currentDepth), abs(previousDepth)), 1.0e-4);
    let depthThreshold = max(uniforms.controls.y, 1.0e-5);
    let depthConfidence = 1.0 - smoothstep(
        depthThreshold, depthThreshold * 4.0, relativeDepth);
    let currentDisocclusion = clamp(
        sample_nearest(disocclusionTexture, currentUv, outputExtent).x,
        0.0, 1.0);
    let matchConfidence = 1.0 - smoothstep(
        0.08, 0.65, color_error(current.rgb, previous.rgb));
    let confidence = select(0.0, 1.0, inBounds) * depthConfidence *
        (1.0 - currentDisocclusion) * matchConfidence * sourceConfidence;
    return Candidate(
        mix(current, previous, 1.0 - t), confidence, motion);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) invocation : vec3<u32>) {
    let outputExtent = uniforms.outputViewport.xy;
    let motionExtent = uniforms.outputViewport.zw;
    if (any(invocation.xy >= outputExtent)) {
        return;
    }
    let pixel = vec2<i32>(invocation.xy);
    let uv = (vec2<f32>(invocation.xy) + vec2<f32>(0.5)) /
        vec2<f32>(outputExtent);
    let currentUnwarped = textureLoad(currentColorTexture, pixel, 0);
    if (uniforms.flags.x == 0u) {
        textureStore(outputTexture, pixel, vec4<f32>(currentUnwarped.rgb, 1.0));
        return;
    }

    let filledGameMotion = game_motion(uv, motionExtent);
    let flowExtent = textureDimensions(opticalFlowTexture);
    let optical = sample_nearest(opticalFlowTexture, uv, flowExtent);
    let game = evaluate_candidate(
        uv, filledGameMotion.xy, filledGameMotion.z);
    let flow = evaluate_candidate(uv, optical.xy, clamp(optical.z, 0.0, 1.0));
    let totalConfidence = game.confidence + flow.confidence;
    var resultColor = currentUnwarped.rgb;
    if (totalConfidence > 1.0e-4) {
        resultColor = (game.color.rgb * game.confidence +
            flow.color.rgb * flow.confidence) / totalConfidence;
    }
    let validity = clamp(max(game.confidence, flow.confidence), 0.0, 1.0);
    var result = vec4<f32>(resultColor, validity);
    if (uniforms.flags.y == 1u) {
        result = vec4<f32>(0.5 + game.motion * 20.0, 0.5, 1.0);
    } else if (uniforms.flags.y == 2u) {
        result = vec4<f32>(vec3<f32>(validity), 1.0);
    } else if (uniforms.flags.y == 3u && invocation.x < outputExtent.x / 2u) {
        result = vec4<f32>(currentUnwarped.rgb, 1.0);
    } else if (uniforms.flags.y == 4u) {
        result = vec4<f32>(0.5 + optical.xy * 20.0, optical.z, 1.0);
    } else if (uniforms.flags.y == 5u) {
        result = vec4<f32>(1.0 - validity, validity, 0.0, 1.0);
    }
    textureStore(outputTexture, pixel, result);
}
