#include "vsm-ssss-common.wgsl"

const SSSS_DEBUG_HARD_SHADOW: u32 = 0u;
const SSSS_DEBUG_PENUMBRA: u32 = 1u;
const SSSS_DEBUG_TRANSLUCENCY: u32 = 2u;
const SSSS_DEBUG_HORIZONTAL_BLUR: u32 = 3u;
const SSSS_DEBUG_BLURRED_VISIBILITY: u32 = 4u;
const SSSS_DEBUG_LAYER_OCCUPANCY: u32 = 5u;

@group(0) @binding(0) var<uniform> uniforms: ScreenSpaceSoftShadowUniforms;
@group(0) @binding(1) var hardShadowTexture: texture_2d_array<f32>;
@group(0) @binding(2) var penumbraTexture: texture_2d_array<f32>;
@group(0) @binding(3) var translucencyTexture: texture_2d_array<f32>;
@group(0) @binding(4) var horizontalBlurTexture: texture_2d_array<f32>;
@group(0) @binding(5) var blurredVisibilityTexture: texture_2d_array<f32>;
@group(0) @binding(6) var outputTexture: texture_storage_2d<rgba16float, write>;

fn ssss_debug_heatmap(value: f32) -> vec3<f32> {
    let x = clamp(value, 0.0, 1.0);
    if (x < 0.25) {
        return mix(vec3<f32>(0.02, 0.03, 0.08), vec3<f32>(0.08, 0.30, 0.75), x / 0.25);
    }
    if (x < 0.5) {
        return mix(vec3<f32>(0.08, 0.30, 0.75), vec3<f32>(0.10, 0.72, 0.50), (x - 0.25) / 0.25);
    }
    if (x < 0.75) {
        return mix(vec3<f32>(0.10, 0.72, 0.50), vec3<f32>(0.98, 0.80, 0.18), (x - 0.5) / 0.25);
    }
    return mix(vec3<f32>(0.98, 0.80, 0.18), vec3<f32>(0.92, 0.26, 0.12), (x - 0.75) / 0.25);
}

fn ssss_debug_layer_color(layer: u32) -> vec3<f32> {
    let x = f32(layer + 1u);
    return 0.2 + 0.8 * vec3<f32>(
        fract(sin(x * 12.9898 + 0.1234) * 43758.5453),
        fract(sin(x * 78.2330 + 1.2345) * 24634.6345),
        fract(sin(x * 39.4250 + 2.3456) * 56445.2344));
}

fn ssss_debug_visibility(value: f32) -> vec4<f32> {
    let v = clamp(value, 0.0, 1.0);
    return vec4<f32>(vec3<f32>(v), 1.0);
}

fn ssss_debug_source_pixel(output_pixel: vec2<u32>, output_dims: vec2<u32>, source_dims: vec2<u32>) -> vec2<i32> {
    let uv = (vec2<f32>(output_pixel) + vec2<f32>(0.5, 0.5)) / max(vec2<f32>(output_dims), vec2<f32>(1.0, 1.0));
    let source_pixel = clamp(
        uv * vec2<f32>(source_dims),
        vec2<f32>(0.0, 0.0),
        max(vec2<f32>(source_dims) - vec2<f32>(1.0, 1.0), vec2<f32>(0.0, 0.0)));
    return vec2<i32>(source_pixel);
}

@compute @workgroup_size(8, 8, 1)
fn main(@builtin(global_invocation_id) global_id: vec3<u32>) {
    let dims = textureDimensions(outputTexture);
    if (global_id.x >= dims.x || global_id.y >= dims.y) {
        return;
    }

    let output_pixel = global_id.xy;
    let layer_count = max(uniforms.layerCount, 1u);
    let layer_u32 = min(uniforms.debugLayer, layer_count - 1u);
    let layer = i32(layer_u32);
    let scale = max(uniforms.debugValueScale, 0.0);

    var color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
    switch (uniforms.debugMode) {
        case SSSS_DEBUG_HARD_SHADOW: {
            let pixel = ssss_debug_source_pixel(output_pixel, dims, textureDimensions(hardShadowTexture).xy);
            color = ssss_debug_visibility(textureLoad(hardShadowTexture, pixel, layer, 0).r);
        }
        case SSSS_DEBUG_PENUMBRA: {
            let pixel = ssss_debug_source_pixel(output_pixel, dims, textureDimensions(penumbraTexture).xy);
            let penumbra = textureLoad(penumbraTexture, pixel, layer, 0).r * scale;
            color = vec4<f32>(ssss_debug_heatmap(penumbra), 1.0);
        }
        case SSSS_DEBUG_TRANSLUCENCY: {
            let pixel = ssss_debug_source_pixel(output_pixel, dims, textureDimensions(translucencyTexture).xy);
            color = vec4<f32>(textureLoad(translucencyTexture, pixel, layer, 0).rgb, 1.0);
        }
        case SSSS_DEBUG_HORIZONTAL_BLUR: {
            let pixel = ssss_debug_source_pixel(output_pixel, dims, textureDimensions(horizontalBlurTexture).xy);
            color = ssss_debug_visibility(textureLoad(horizontalBlurTexture, pixel, layer, 0).r);
        }
        case SSSS_DEBUG_LAYER_OCCUPANCY: {
            let in_layer_budget = select(0.0, 1.0, uniforms.debugLayer < layer_count);
            let color_for_layer = ssss_debug_layer_color(layer_u32) * in_layer_budget;
            color = vec4<f32>(color_for_layer, 1.0);
        }
        default: {
            let pixel = ssss_debug_source_pixel(output_pixel, dims, textureDimensions(blurredVisibilityTexture).xy);
            color = ssss_debug_visibility(textureLoad(blurredVisibilityTexture, pixel, layer, 0).r);
        }
    }

    textureStore(outputTexture, vec2<i32>(output_pixel), color);
}
