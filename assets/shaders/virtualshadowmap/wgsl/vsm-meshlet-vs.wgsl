#ifdef VSM_ENABLE_CLIP_DISTANCES
enable clip_distances;
#endif

#include "virtualgeometry/wgsl/virtualgeometrydata.wgsl"
#include "vsm-common.wgsl"

struct DrawUniforms {
    pageTableResolution: u32,
    physicalPageSize: u32,
    activeLayers: u32,
    currentLayer: u32,
    scratchResolution: u32,
    physicalAtlasResolution: u32,
    reverseZ: u32,
    maxVisibleClusterDrawsPerLayer: u32,
    _padding0: vec4<u32>,
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
    pageTableResolution: u32,
    lightType: u32,
    faceIndex: u32,
    mipLevel: u32,
}

struct ShadowVisibleClusterInfo {
    pageIndex: u32,
    pageLocalClusterIndex: u32,
    instanceIndex: u32,
    layer: u32,
    meshPartIndex: u32,
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    pageCoordsPacked: u32,
    linearPageCoordsPacked: u32,
#else
    _padding: u32,
#endif
}

@group(0) @binding(0) var<uniform> uniforms: DrawUniforms;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read> scenePageTable: array<PageTableEntry>;
@group(0) @binding(3) var<storage, read> pagesBuffer: array<u32>;
@group(0) @binding(4) var<storage, read> shadowVisibleClusterInfos: array<ShadowVisibleClusterInfo>;
@group(0) @binding(5) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(6) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(7) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(8) var<storage, read> meshPartTransforms: array<mat4x4<f32>>;
@group(0) @binding(10) var<storage, read> decodedClusterGeometryInfos: array<DecodedClusterGeometryInfo>;
@group(0) @binding(11) var<storage, read> decodedVertexBuffer: array<DecodedVertexData>;
@group(0) @binding(12) var<storage, read> decodedIndexBuffer: array<u32>;

#include "virtualgeometry/wgsl/virtualgeometry-geometry-fetch.wgsl"

struct VertexOutput {
    @builtin(position) clipPos: vec4<f32>,
#ifdef VSM_ENABLE_CLIP_DISTANCES
    @builtin(clip_distances) clipDistances: array<f32, 4>,
#endif
}

/// @brief Reads a single word from a meshlet descriptor field in the scene pages buffer.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param fieldOff The word offset of the desired field within the meshlet descriptor.
/// @returns The descriptor word at the specified field.
fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return pagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
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
    var val = pagesBuffer[wi] >> bi;
    if (bi + numBits > 32u) {
        let overflow = (bi + numBits) - 32u;
        val |= pagesBuffer[wi + 1u] << (numBits - overflow);
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
    let word = pagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
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

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
fn unpackPageCoords(packed: u32) -> vec2<u32> {
    return vec2<u32>(packed & 0xFFFFu, (packed >> 16u) & 0xFFFFu);
}

fn unpackLinearPageCoords(packed: u32) -> vec2<i32> {
    let packedX = packed & 0xFFFFu;
    let packedY = (packed >> 16u) & 0xFFFFu;
    let x = select(i32(packedX), i32(packedX | 0xFFFF0000u), (packedX & 0x8000u) != 0u);
    let y = select(i32(packedY), i32(packedY | 0xFFFF0000u), (packedY & 0x8000u) != 0u);
    return vec2<i32>(x, y);
}
#endif

/// @brief Shadow meshlet vertex shader that decodes and transforms cluster geometry into scratch-buffer clip space.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    var out: VertexOutput;
    out.clipPos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
#ifdef VSM_ENABLE_CLIP_DISTANCES
    out.clipDistances = array<f32, 4>(1.0, 1.0, 1.0, 1.0);
#endif
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let clusterSlotLocal = instance_index;
    let localCorner = vertex_index;
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    let clusterSlot = clusterSlotLocal;
#else
    let clusterSlot = uniforms.currentLayer * uniforms.maxVisibleClusterDrawsPerLayer + clusterSlotLocal;
#endif
    let clusterInfo = shadowVisibleClusterInfos[clusterSlot];
#else
    let clusterInfo = shadowVisibleClusterInfos[instance_index];
#endif
    let layer = clusterInfo.layer;
    if (layer >= uniforms.activeLayers
#ifndef VSM_CLIP_SPACE_ATLAS_DEPTH
        || layer != uniforms.currentLayer
#endif
    ) {
        return out;
    }

    let instance = instances[clusterInfo.instanceIndex];
    let pageEntry = scenePageTable[clusterInfo.pageIndex];
    let localMeshletIdx = clusterInfo.pageLocalClusterIndex;
    let clusterGeometry = decodedClusterGeometryInfos[globalClusterIndex(pageEntry, localMeshletIdx)];
    let vtxCount = clusterGeometry.vertexCount;
    let triCount = clusterGeometry.triangleCount;

    let maxCorners = triCount * 3u;
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let cornerIdx = min(localCorner, maxCorners - 1u);
#else
    let cornerIdx = min(vertex_index, maxCorners - 1u);
#endif
    let localVertID = vgLoadIndex(clusterGeometry, cornerIdx);
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    if (localCorner >= maxCorners || localVertID >= vtxCount) {
        return out;
    }
#else
    if (vertex_index >= maxCorners || localVertID >= vtxCount) {
        return out;
    }
#endif

    let vertexData = vgLoadVertex(clusterGeometry, localVertID);
    let localPos = vec3<f32>(vertexData.positionX, vertexData.positionY, vertexData.positionZ);
    let nodeModelMatrix = resolveMeshPartModelMatrix(instance, clusterInfo.meshPartIndex);
    let worldPos = (nodeModelMatrix * vec4<f32>(localPos, 1.0)).xyz;

    let lightClip = cascadeMatrices[layer].viewProj * vec4<f32>(worldPos, 1.0);
    let invW = select(1.0, 1.0 / lightClip.w, abs(lightClip.w) > 1e-6);
    let ndc = lightClip.xyz * invW;
    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let homogeneous_w = lightClip.w;
    let logical_page_table_resolution = f32(layer_page_table_resolution(layer));
    let storage_page_table_resolution = f32(max(uniforms.pageTableResolution, 1u));
    let layer_uv_scale = logical_page_table_resolution / storage_page_table_resolution;
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    let wrappedPageCoord = unpackPageCoords(clusterInfo.pageCoordsPacked);
    let linearPageCoord = unpackLinearPageCoords(clusterInfo.linearPageCoordsPacked);
    let pageSpace = uv * logical_page_table_resolution;
    let localPageUV = pageSpace - vec2<f32>(linearPageCoord);
    let physicalPageSize = f32(max(uniforms.physicalPageSize, 1u));
    let vptEntry = virtualPageTable[vsm_vpt_index(layer, wrappedPageCoord)];
    let atlasPageOrigin = vec2<u32>(get_page_x(vptEntry), get_page_y(vptEntry)) * max(uniforms.physicalPageSize, 1u);
    let atlasPixel = vec2<f32>(atlasPageOrigin) + localPageUV * physicalPageSize;
    let atlasUV = atlasPixel / f32(max(uniforms.physicalAtlasResolution, 1u));
    out.clipPos = vec4<f32>(
        (atlasUV * 2.0 - vec2<f32>(1.0, 1.0)) * homogeneous_w,
        lightClip.z,
        homogeneous_w);
#ifdef VSM_ENABLE_CLIP_DISTANCES
    let pageTexelMargin = 0.5 / physicalPageSize;
    let homogeneousPageSpace = (lightClip.xy * 0.5 + vec2<f32>(homogeneous_w * 0.5)) * logical_page_table_resolution;
    let pageMin = vec2<f32>(linearPageCoord) - vec2<f32>(pageTexelMargin);
    let pageMax = vec2<f32>(linearPageCoord) + vec2<f32>(1.0 + pageTexelMargin);
    out.clipDistances = array<f32, 4>(
        homogeneousPageSpace.x - pageMin.x * homogeneous_w,
        homogeneousPageSpace.y - pageMin.y * homogeneous_w,
        pageMax.x * homogeneous_w - homogeneousPageSpace.x,
        pageMax.y * homogeneous_w - homogeneousPageSpace.y);
#endif
#else
    out.clipPos = vec4<f32>(((uv * layer_uv_scale) * 2.0 - vec2<f32>(1.0, 1.0)) * homogeneous_w, lightClip.z, homogeneous_w);
#endif
    return out;
}
