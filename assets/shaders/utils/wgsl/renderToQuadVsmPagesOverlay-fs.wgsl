struct OverlayUniforms {
    ndcMinX : f32,
    ndcMinY : f32,
    ndcMaxX : f32,
    ndcMaxY : f32,
    uvMinX  : f32,
    uvMinY  : f32,
    uvMaxX  : f32,
    uvMaxY  : f32,
    enabled : u32,
    opacity : f32,
    _padding0 : vec2<u32>,
};

@group(0) @binding(0) var<uniform> u : OverlayUniforms;
@group(0) @binding(1) var texSampler : sampler;
@group(0) @binding(2) var vsmPagesTexture : texture_2d<f32>;

@fragment
fn fs_main(@location(0) uv : vec2<f32>) -> @location(0) vec4<f32> {
    if (u.enabled == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    let overlay = textureSample(vsmPagesTexture, texSampler, uv);
    let signal = max(max(overlay.r, overlay.g), overlay.b);
    if (signal <= 1e-4) {
        return vec4<f32>(0.0, 0.0, 0.0, 0.0);
    }

    return vec4<f32>(overlay.rgb, clamp(u.opacity, 0.0, 1.0));
}
