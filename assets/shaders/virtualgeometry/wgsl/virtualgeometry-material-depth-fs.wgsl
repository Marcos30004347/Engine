struct TileUniforms {
    viewportWidth : u32,
    viewportHeight : u32,
    tileSize : u32,
    tilesX : u32,
    tilesY : u32,
    materialCount : u32,
    maxDrawEntries : u32,
    _padding0 : u32,
};

@group(0) @binding(1) var<uniform> tileUniforms : TileUniforms;
@group(0) @binding(2) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(3) var materialIdTexture : texture_2d<u32>;

struct DepthOutput {
    @builtin(frag_depth) depth : f32,
};

/// @brief Encodes a material ID as a normalized depth value in (0, 1) suitable for writing to the material depth target.
/// @param materialId The zero-based material identifier.
/// @param materialCount The total number of materials in the scene.
/// @returns A normalized depth value in (0, 1] that uniquely encodes the material ID.
fn encodeMaterialDepth(materialId: u32, materialCount: u32) -> f32 {
    return f32(materialId + 1u) / f32(max(materialCount, 1u) + 1u);
}

/// @brief Fragment shader that writes an encoded material depth value for each visible pixel, enabling per-material tile processing in subsequent passes.
@fragment
fn fs_main(@builtin(position) clipPos : vec4<f32>) -> DepthOutput {
    let pixel = vec2<i32>(clipPos.xy);
    let sceneDepth = textureLoad(sceneDepthTexture, pixel, 0);

    var out : DepthOutput;
    if (sceneDepth <= 0.0) {
        out.depth = 0.0;
        return out;
    }

    let materialId = textureLoad(materialIdTexture, pixel, 0).x;
    if (materialId >= tileUniforms.materialCount) {
        out.depth = 0.0;
        return out;
    }

    out.depth = encodeMaterialDepth(materialId, tileUniforms.materialCount);
    return out;
}
