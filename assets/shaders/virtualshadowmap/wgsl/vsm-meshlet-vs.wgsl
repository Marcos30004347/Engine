#include "virtualgeometry/wgsl/virtualgeometrydata.wgsl"
#include "vsm-common.wgsl"

struct DrawUniforms {
    pageTableResolution: u32,
    physicalPageSize: u32,
    activeLayers: u32,
    currentLayer: u32,
    scratchResolution: u32,
    _padding0: vec3<u32>,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

struct CascadeMatrix {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    worldExtent: f32,
    pageWorldSize: f32,
    lightIndex: u32,
    cascadeIndex: u32,
}

struct ShadowVisibleClusterInfo {
    pageIndex: u32,
    pageLocalClusterIndex: u32,
    instanceIndex: u32,
    layer: u32,
    meshPartIndex: u32,
    _padding: u32,
}

@group(0) @binding(0) var<uniform> uniforms: DrawUniforms;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read> scenePageTable: array<PageTableEntry>;
@group(0) @binding(3) var<storage, read> scenePagesBuffer: array<u32>;
@group(0) @binding(4) var<storage, read> shadowVisibleClusterInfos: array<ShadowVisibleClusterInfo>;
@group(0) @binding(5) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(6) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(7) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(8) var<storage, read> meshPartTransforms: array<mat4x4<f32>>;

struct VertexOutput {
    @builtin(position) clipPos: vec4<f32>,
}

/// @brief Reads a single word from a meshlet descriptor field in the scene pages buffer.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param fieldOff The word offset of the desired field within the meshlet descriptor.
/// @returns The descriptor word at the specified field.
fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return scenePagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

/// @brief Extracts a bit-packed unsigned integer spanning up to 32 bits from the scene pages buffer.
/// @param wordBase The base word index in the scene pages buffer.
/// @param bitOffset The bit offset from the base word where the value starts.
/// @param numBits The number of bits to extract.
/// @returns The extracted unsigned integer value.
fn extractBits(wordBase: u32, bitOffset: u32, numBits: u32) -> u32 {
    if (numBits == 0u) { return 0u; }
    let wi = wordBase + (bitOffset / 32u);
    let bi = bitOffset % 32u;
    var val = scenePagesBuffer[wi] >> bi;
    if (bi + numBits > 32u) {
        let overflow = (bi + numBits) - 32u;
        val |= scenePagesBuffer[wi + 1u] << (numBits - overflow);
    }
    let mask = select((1u << numBits) - 1u, 0xFFFFFFFFu, numBits == 32u);
    return val & mask;
}

/// @brief Decodes the quantized local-space position of a vertex from the packed position bit-stream.
/// @param posDataBase Absolute word offset of the position block in the scene pages buffer.
/// @param posOff Word offset of the meshlet's position data within the position block.
/// @param bitsX Number of bits used for the X component.
/// @param bitsY Number of bits used for the Y component.
/// @param bitsZ Number of bits used for the Z component.
/// @param qFactor Quantization exponent used to scale back to mesh space.
/// @param minQX Quantized-space minimum X (stored as f32).
/// @param minQY Quantized-space minimum Y (stored as f32).
/// @param minQZ Quantized-space minimum Z (stored as f32).
/// @param unitScale Per-instance unit scale factor.
/// @param vertexIdx The local vertex index within the meshlet.
/// @returns The decoded local-space vertex position.
fn decodePosition(posDataBase: u32, posOff: u32, bitsX: u32, bitsY: u32, bitsZ: u32, qFactor: u32, minQX: f32, minQY: f32, minQZ: f32, unitScale: f32, vertexIdx: u32) -> vec3<f32> {
    let bitsPerVert = bitsX + bitsY + bitsZ;
    let startBit = vertexIdx * bitsPerVert;
    let base = posDataBase + posOff;
    let qx = extractBits(base, startBit, bitsX);
    let qy = extractBits(base, startBit + bitsX, bitsY);
    let qz = extractBits(base, startBit + bitsX + bitsY, bitsZ);
    let dequantScale = f32(1u << qFactor) * unitScale;
    return vec3<f32>(
        (minQX + f32(qx)) / dequantScale,
        (minQY + f32(qy)) / dequantScale,
        (minQZ + f32(qz)) / dequantScale);
}

/// @brief Decodes a single byte-packed triangle index from the index buffer.
/// @param idxDataBase Absolute word offset of the index block in the scene pages buffer.
/// @param idxOff Word offset of the meshlet's index data within the index block.
/// @param indexIdx The corner index (0-based) to decode.
/// @returns The local vertex index referenced by that corner.
fn decodeIndex(idxDataBase: u32, idxOff: u32, indexIdx: u32) -> u32 {
    let word = scenePagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
    let shift = (indexIdx % 4u) * 8u;
    return (word >> shift) & 0xFFu;
}

/// @brief Resolves the model matrix for a mesh part, combining the instance model matrix with an optional per-part transform.
/// @param instance The instance data containing the base model matrix and transform offset.
/// @param meshPartIndex The local mesh part index, or 0xFFFFFFFF to use the instance model matrix directly.
/// @returns The resolved world-space model matrix for the mesh part.
fn resolveMeshPartModelMatrix(instance: InstanceData, meshPartIndex: u32) -> mat4x4<f32> {
    if (meshPartIndex == 0xFFFFFFFFu) {
        return instance.modelMatrix;
    }
    return instance.modelMatrix * meshPartTransforms[instance.meshPartTransformsOffset + meshPartIndex];
}

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param pageCoord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vsm_vpt_index(layer: u32, pageCoord: vec2<u32>) -> u32 {
    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pagesPerLayer + pageCoord.y * uniforms.pageTableResolution + pageCoord.x;
}

/// @brief Shadow meshlet vertex shader that decodes and transforms cluster geometry into scratch-buffer clip space.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    var out: VertexOutput;
    out.clipPos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);

    let clusterInfo = shadowVisibleClusterInfos[instance_index];
    let layer = clusterInfo.layer;
    if (layer >= uniforms.activeLayers || layer != uniforms.currentLayer) {
        return out;
    }

    let instance = instances[clusterInfo.instanceIndex];
    let pageEntry = scenePageTable[clusterInfo.pageIndex];
    let pageBase = pageWordBase(pageEntry);
    let localMeshletIdx = clusterInfo.pageLocalClusterIndex;

    let numMeshlets = scenePagesBuffer[pageBase + 0u];
    let posDataSize = scenePagesBuffer[pageBase + 1u];
    let normDataSize = scenePagesBuffer[pageBase + 2u];
    let uvDataSize = scenePagesBuffer[pageBase + 3u];
    let idxOff = readDesc(pageBase, localMeshletIdx, DESC_IDX_OFF);
    let vtxCount = readDesc(pageBase, localMeshletIdx, DESC_VERT_COUNT);
    let triCount = readDesc(pageBase, localMeshletIdx, DESC_TRI_COUNT);
    let bitsX = quantizedSpanToBitCount(readDesc(pageBase, localMeshletIdx, DESC_POS_SPAN_X));
    let bitsY = quantizedSpanToBitCount(readDesc(pageBase, localMeshletIdx, DESC_POS_SPAN_Y));
    let bitsZ = quantizedSpanToBitCount(readDesc(pageBase, localMeshletIdx, DESC_POS_SPAN_Z));
    let qFactor = readDesc(pageBase, localMeshletIdx, DESC_QFACTOR);
    let minQX = bitcast<f32>(readDesc(pageBase, localMeshletIdx, DESC_MIN_X));
    let minQY = bitcast<f32>(readDesc(pageBase, localMeshletIdx, DESC_MIN_Y));
    let minQZ = bitcast<f32>(readDesc(pageBase, localMeshletIdx, DESC_MIN_Z));
    let posOff = readDesc(pageBase, localMeshletIdx, DESC_POS_OFF);

    let vDataBase = vertexDataBase(pageBase, numMeshlets);
    let posDataBase = vDataBase;
    let normDataBase = vDataBase + posDataSize;
    let uvDataBase = normDataBase + normDataSize;
    let idxDataBase = uvDataBase + uvDataSize;

    let maxCorners = triCount * 3u;
    let cornerIdx = min(vertex_index, maxCorners - 1u);
    let localVertID = decodeIndex(idxDataBase, idxOff, cornerIdx);
    if (vertex_index >= maxCorners || localVertID >= vtxCount) {
        return out;
    }

    let unitScale = bitcast<f32>(instance.unit_scale_bits);
    let localPos = decodePosition(posDataBase, posOff, bitsX, bitsY, bitsZ, qFactor, minQX, minQY, minQZ, unitScale, localVertID);
    let nodeModelMatrix = resolveMeshPartModelMatrix(instance, clusterInfo.meshPartIndex);
    let worldPos = (nodeModelMatrix * vec4<f32>(localPos, 1.0)).xyz;

    let lightClip = cascadeMatrices[layer].viewProj * vec4<f32>(worldPos, 1.0);
    let invW = select(1.0, 1.0 / lightClip.w, abs(lightClip.w) > 1e-6);
    let ndc = lightClip.xyz * invW;
    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    out.clipPos = vec4<f32>(uv * 2.0 - vec2<f32>(1.0, 1.0), clamp(ndc.z, 0.0, 1.0), 1.0);
    return out;
}
