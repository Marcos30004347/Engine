struct OverviewCoverageUniforms {
    dimensions : vec4<u32>,
}

@group(0) @binding(0) var<uniform> uniforms : OverviewCoverageUniforms;
@group(0) @binding(1) var overviewColor : texture_2d<f32>;
@group(0) @binding(2) var<storage, read_write> coverage : array<u32>;

@compute @workgroup_size(8, 8, 1)
fn measurePlanetOverviewCoverage(@builtin(global_invocation_id) invocation : vec3<u32>) {
    if (invocation.x >= uniforms.dimensions.x ||
        invocation.y >= uniforms.dimensions.y) {
        return;
    }
    let pixel = vec2<i32>(invocation.xy);
    let index = invocation.y * uniforms.dimensions.x + invocation.x;
    let color = textureLoad(overviewColor, pixel, 0);
    let rgb = vec3<u32>(round(clamp(color.rgb, vec3<f32>(0.0),
        vec3<f32>(1.0)) * 255.0));
    let packed = 1u | (rgb.r << 8u) | (rgb.g << 16u) | (rgb.b << 24u);
    coverage[index] = select(0u, packed, color.a > 0.5);
}
