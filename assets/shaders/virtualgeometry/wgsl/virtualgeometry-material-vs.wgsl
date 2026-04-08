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

struct TileMaterialDrawEntry {
    tileX : u32,
    tileY : u32,
    materialId : u32,
    _padding0 : u32,
};

@group(0) @binding(0) var<uniform> tileUniforms : TileUniforms;
@group(0) @binding(8) var<storage, read> tileDrawEntries : array<TileMaterialDrawEntry>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) @interpolate(flat) materialId : u32,
};

/// @brief Encodes a material ID as a normalized depth value in (0, 1) for use as the clip-space Z of the tile quad.
/// @param materialId The zero-based material identifier.
/// @param materialCount The total number of materials in the scene.
/// @returns A normalized depth value in (0, 1] that uniquely encodes the material ID.
fn encodeMaterialDepth(materialId: u32, materialCount: u32) -> f32 {
    return f32(materialId + 1u) / f32(max(materialCount, 1u) + 1u);
}

/// @brief Converts a horizontal pixel coordinate to an NDC X coordinate in [-1, 1].
/// @param pixel The horizontal pixel position within the viewport.
/// @returns The corresponding NDC X coordinate.
fn pixelToNdcX(pixel: f32) -> f32 {
    return (pixel / f32(max(tileUniforms.viewportWidth, 1u))) * 2.0 - 1.0;
}

/// @brief Converts a vertical pixel coordinate to an NDC Y coordinate in [-1, 1].
/// @param pixel The vertical pixel position within the viewport.
/// @returns The corresponding NDC Y coordinate.
fn pixelToNdcY(pixel: f32) -> f32 {
    return (pixel / f32(max(tileUniforms.viewportHeight, 1u))) * 2.0 - 1.0;
}

/// @brief Vertex shader that emits a screen-aligned quad for each tile-material draw entry, setting clip depth to the encoded material depth.
@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32) -> VertexOutput {
    let cornerIndices = array<u32, 6>(0u, 2u, 1u, 1u, 2u, 3u);
    let drawIndex = vertexIndex / 6u;
    let localVertexIndex = vertexIndex % 6u;
    let entry = tileDrawEntries[drawIndex];

    let minPixel = vec2<f32>(f32(entry.tileX * tileUniforms.tileSize), f32(entry.tileY * tileUniforms.tileSize));
    let maxPixel = vec2<f32>(
        f32(min((entry.tileX + 1u) * tileUniforms.tileSize, tileUniforms.viewportWidth)),
        f32(min((entry.tileY + 1u) * tileUniforms.tileSize, tileUniforms.viewportHeight))
    );

    let ndcCorners = array<vec2<f32>, 4>(
        vec2<f32>(pixelToNdcX(minPixel.x), pixelToNdcY(minPixel.y)),
        vec2<f32>(pixelToNdcX(maxPixel.x), pixelToNdcY(minPixel.y)),
        vec2<f32>(pixelToNdcX(minPixel.x), pixelToNdcY(maxPixel.y)),
        vec2<f32>(pixelToNdcX(maxPixel.x), pixelToNdcY(maxPixel.y))
    );

    var out : VertexOutput;
    out.clipPos = vec4<f32>(ndcCorners[cornerIndices[localVertexIndex]], encodeMaterialDepth(entry.materialId, tileUniforms.materialCount), 1.0);
    out.materialId = entry.materialId;
    return out;
}
