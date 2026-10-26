#include "gpusigneddistancefields_common.wgsl"

struct AtlasDebugUniforms {
    outputWidth : u32,
    outputHeight : u32,
    atlasResolution : u32,
    atlasBricksPerAxis : u32,
    axis : u32,
    sliceIndex : u32,
    showBrickGrid : u32,
    enabled : u32,
}

@group(0) @binding(0) var<uniform> gUniforms : AtlasDebugUniforms;
@group(0) @binding(1) var sdfAtlasTexture : texture_3d<f32>;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

fn heatmap(t: f32) -> vec3<f32> {
    let x = clamp(t, 0.0, 1.0);
    if (x < 0.25) {
        return mix(vec3<f32>(0.02, 0.03, 0.08), vec3<f32>(0.08, 0.30, 0.75), x / 0.25);
    }
    if (x < 0.5) {
        return mix(vec3<f32>(0.08, 0.30, 0.75), vec3<f32>(0.10, 0.72, 0.50), (x - 0.25) / 0.25);
    }
    if (x < 0.75) {
        return mix(vec3<f32>(0.10, 0.72, 0.50), vec3<f32>(0.98, 0.80, 0.18), (x - 0.50) / 0.25);
    }
    return mix(vec3<f32>(0.98, 0.80, 0.18), vec3<f32>(0.92, 0.26, 0.12), (x - 0.75) / 0.25);
}

fn decode_atlas_magnitude(atlasValue: f32) -> f32 {
    // Atlas stores distance/voxelSize linearly; magnitude is just the normalized [0,1] value.
    return clamp(abs(atlasValue), 0.0, 1.0);
}

fn compute_atlas_coord(outputPixel: vec2<u32>) -> vec3<u32> {
    let atlasPixel = min(
        vec2<u32>(floor((vec2<f32>(outputPixel) + vec2<f32>(0.5)) / vec2<f32>(f32(gUniforms.outputWidth), f32(gUniforms.outputHeight)) * f32(gUniforms.atlasResolution))),
        vec2<u32>(max(gUniforms.atlasResolution, 1u) - 1u)
    );
    let slice = min(gUniforms.sliceIndex, gUniforms.atlasResolution - 1u);
    if (gUniforms.axis == 0u) {
        return vec3<u32>(slice, atlasPixel.x, atlasPixel.y);
    }
    if (gUniforms.axis == 1u) {
        return vec3<u32>(atlasPixel.x, slice, atlasPixel.y);
    }
    return vec3<u32>(atlasPixel.x, atlasPixel.y, slice);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= gUniforms.outputWidth || globalInvocationId.y >= gUniforms.outputHeight) {
        return;
    }

    let outputPixel = vec2<i32>(globalInvocationId.xy);
    if (gUniforms.enabled == 0u) {
        textureStore(outputTexture, outputPixel, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    let atlasCoord = compute_atlas_coord(globalInvocationId.xy);
    let atlasValue = textureLoad(sdfAtlasTexture, vec3<i32>(atlasCoord), 0).x;
    let magnitude = decode_atlas_magnitude(atlasValue);
    var color = select(vec3<f32>(0.0, 0.0, 0.0), heatmap(clamp(sqrt(magnitude), 0.0, 1.0)), magnitude > 1.0e-6);
    if (magnitude > 1.0e-6) {
        let signTint = select(vec3<f32>(0.22, 0.85, 0.95), vec3<f32>(0.95, 0.30, 0.22), atlasValue < 0.0);
        color = mix(color, signTint, 0.30);
    }

    if (gUniforms.showBrickGrid != 0u) {
        let gridX = (atlasCoord.x % GPUSignedDistanceFields_BrickResolution) == 0u;
        let gridY = (atlasCoord.y % GPUSignedDistanceFields_BrickResolution) == 0u;
        let gridZ = (atlasCoord.z % GPUSignedDistanceFields_BrickResolution) == 0u;
        if ((gUniforms.axis == 0u && (gridY || gridZ)) ||
            (gUniforms.axis == 1u && (gridX || gridZ)) ||
            (gUniforms.axis == 2u && (gridX || gridY))) {
            color *= 0.25;
        }
    }

    textureStore(outputTexture, outputPixel, vec4<f32>(color, 1.0));
}
