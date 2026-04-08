#include "virtualgeometrydata.wgsl"

// Entry packing sync marker: keep this file touched when
// virtualgeometrydata.wgsl changes because shader include dependency tracking
// is not reliable in the current build.

@group(0) @binding(0) var<uniform>           uniforms           : CullingUniforms;
@group(0) @binding(1) var<storage, read>      instances          : array<InstanceData>;
@group(0) @binding(2) var<storage, read>      pageTable          : array<PageTableEntry>;
@group(0) @binding(3) var<storage, read>      pagesBuffer        : array<u32>;
@group(0) @binding(4) var<storage, read>      visibleClusterInfos: array<VisibleClusterInfo>;
#ifndef DRAW_INDIRECT_COUNT_DISABLED
@group(0) @binding(5) var<storage, read>      cullingCounters    : array<u32>;
#endif
@group(0) @binding(6) var<storage, read>      meshPartTransforms : array<mat4x4<f32>>;

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) @interpolate(flat) instanceIndex : u32,
    @location(1) @interpolate(flat) pageIndex     : u32,
    @location(2) @interpolate(flat) groupIndex    : u32,
    @location(3) @interpolate(flat) clusterIndex  : u32,
    @location(4) @interpolate(flat) triangleId    : u32,
    @location(5) @interpolate(flat) materialIndex : u32,
    @location(6) uv : vec2<f32>,
}

/// @brief Reads a single descriptor field for a given page-local meshlet from the pages buffer.
/// @param pageBase The word base of the page in the pages buffer.
/// @param localIdx The page-local meshlet index.
/// @param fieldOff The field offset within the meshlet descriptor (one of the DESC_* constants).
/// @returns The raw u32 value of the requested descriptor field.
fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return pagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

// ── Bit extraction from the global pagesBuffer ────────────────────────────────

/// @brief Extracts a run of bits from the global pages buffer bit-stream, spanning at most two consecutive u32 words.
/// @param wordBase The starting word index in the pages buffer for the bit-stream.
/// @param bitOffset The bit index within the stream at which to begin extraction.
/// @param numBits The number of consecutive bits to extract (0 returns 0).
/// @returns The extracted value zero-extended to u32.
fn extractBits(wordBase: u32, bitOffset: u32, numBits: u32) -> u32 {
    if (numBits == 0u) { return 0u; }
    let wi  = wordBase + (bitOffset / 32u);
    let bi  = bitOffset % 32u;
    var val = pagesBuffer[wi] >> bi;
    if (bi + numBits > 32u) {
        let overflow = (bi + numBits) - 32u;
        val |= pagesBuffer[wi + 1u] << (numBits - overflow);
    }
    let mask = select((1u << numBits) - 1u, 0xFFFFFFFFu, numBits == 32u);
    return val & mask;
}


struct BoneWeight {
    weight   : f32,
    boneIndex: u32,
}

// Read a single bone influence for a given vertex and influence slot.
// pageBase          — pageWordBase(entry)
// numMeshlets       — pagesBuffer[pageBase + 0]
// posDataSize       — pagesBuffer[pageBase + 1]
// normDataSize      — pagesBuffer[pageBase + 2]
// uvDataSize        — pagesBuffer[pageBase + 3]
// idxDataSize       — pagesBuffer[pageBase + 4]
// bwDataSize        — pagesBuffer[pageBase + 5]  (not needed here, but shown for clarity)
//
// The bone-weight block starts after:
//   header (PAGE_HEADER_WORDS) + descriptor table (numMeshlets * MESHLET_DESC_WORDS)
//   + posDataSize + normDataSize + uvDataSize + idxDataSize words.
/// @brief Decodes a single bone influence (weight and bone index) for a given vertex and influence slot from the page's bone-weight block.
/// @param pageBase The word base of the page in the pages buffer.
/// @param numMeshlets The number of meshlets in the page, used to locate the vertex data base.
/// @param posDataSize Word count of the position data block.
/// @param normDataSize Word count of the normal data block.
/// @param uvDataSize Word count of the UV data block.
/// @param idxDataSize Word count of the index data block.
/// @param meshletLocalIdx The page-local meshlet index whose bone-weight offset is read.
/// @param vertexIdx The local vertex index within the meshlet.
/// @param influenceSlot The influence slot to read (0 to boneWeightsPerVertex-1).
/// @returns A BoneWeight containing the skinning weight and bone index for the requested influence.
fn decodeBoneWeight(
    pageBase       : u32,
    numMeshlets    : u32,
    posDataSize    : u32,
    normDataSize   : u32,
    uvDataSize     : u32,
    idxDataSize    : u32,
    meshletLocalIdx: u32,
    vertexIdx      : u32,
    influenceSlot  : u32,   // 0 .. boneWeightsPerVertex-1
) -> BoneWeight {
    let bwBlockBase = vertexDataBase(pageBase, numMeshlets)
                    + posDataSize
                    + normDataSize
                    + uvDataSize
                    + idxDataSize;

    let meshletBwOffset = readDesc(pageBase, meshletLocalIdx, DESC_BW_OFF);
    let bwPerVertex     = readDesc(pageBase, meshletLocalIdx, DESC_BW_PER_V);

    let influenceIdx = vertexIdx * bwPerVertex + influenceSlot;
    let wordBase     = bwBlockBase + meshletBwOffset + influenceIdx * 2u;

    var bw: BoneWeight;
    bw.weight    = bitcast<f32>(pagesBuffer[wordBase + 0u]);
    bw.boneIndex = pagesBuffer[wordBase + 1u];
    return bw;
}

const MAX_BONE_WEIGHTS: u32 = 4u;

// ── Vertex attribute decoders ─────────────────────────────────────────────────

/// @brief Decodes a quantized meshlet vertex position from the bit-stream and reconstructs it in local object space.
/// @param posDataBase Absolute word offset of the position block in the pages buffer.
/// @param posOff Word offset of this meshlet's position data within the position block.
/// @param bitsX Number of bits used to encode the X component of each vertex.
/// @param bitsY Number of bits used to encode the Y component of each vertex.
/// @param bitsZ Number of bits used to encode the Z component of each vertex.
/// @param qFactor Quantization exponent; the dequantization scale is 2^qFactor * unitScale.
/// @param minQX Quantization minimum for X, stored as a bitcast f32.
/// @param minQY Quantization minimum for Y, stored as a bitcast f32.
/// @param minQZ Quantization minimum for Z, stored as a bitcast f32.
/// @param unitScale Per-instance unit scale applied during dequantization.
/// @param vertexIdx The local vertex index within the meshlet to decode.
/// @returns The reconstructed local-space position as a vec3<f32>.
fn decodePosition(
    posDataBase : u32,   // absolute word offset of the position block in pagesBuffer
    posOff      : u32,   // word offset of this meshlet within that block
    bitsX       : u32,
    bitsY       : u32,
    bitsZ       : u32,
    qFactor     : u32,
    minQX       : f32,   // quantised-space integer stored as f32
    minQY       : f32,
    minQZ       : f32,
    unitScale   : f32,
    vertexIdx   : u32
) -> vec3<f32> {
    let bitsPerVert = bitsX + bitsY + bitsZ;
    let startBit    = vertexIdx * bitsPerVert;
    let base        = posDataBase + posOff;

    let qx = extractBits(base, startBit,           bitsX);
    let qy = extractBits(base, startBit + bitsX,   bitsY);
    let qz = extractBits(base, startBit + bitsX + bitsY, bitsZ);

    let dequantScale = f32(1u << qFactor) * unitScale;
    return vec3<f32>(
        (minQX + f32(qx)) / dequantScale,
        (minQY + f32(qy)) / dequantScale,
        (minQZ + f32(qz)) / dequantScale,
    );
}

/// @brief Decodes a meshlet vertex normal from the packed oct-encoded format stored in the pages buffer.
/// @param normDataBase Absolute word offset of the normal block in the pages buffer.
/// @param normOff Word offset of this meshlet's normals within the normal block.
/// @param vertexIdx The local vertex index within the meshlet to decode.
/// @returns The decoded and normalized surface normal as a vec3<f32>.
fn decodeNormal(normDataBase: u32, normOff: u32, vertexIdx: u32) -> vec3<f32> {
    let packed = pagesBuffer[normDataBase + normOff + vertexIdx];

    let rawX = i32(packed & 0xFFFFu);
    let rawY = i32((packed >> 16u) & 0xFFFFu);
    let sX   = select(rawX, rawX - 65536, rawX >= 32768);
    let sY   = select(rawY, rawY - 65536, rawY >= 32768);

    var ox = clamp(f32(sX) / 32767.0, -1.0, 1.0);
    var oy = clamp(f32(sY) / 32767.0, -1.0, 1.0);

    var n = vec3<f32>(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        let ox2 = n.x; let oy2 = n.y;
        n.x = (1.0 - abs(oy2)) * select(-1.0, 1.0, ox2 >= 0.0);
        n.y = (1.0 - abs(ox2)) * select(-1.0, 1.0, oy2 >= 0.0);
    }
    let len = length(n);
    if (len > 1e-6) { n /= len; }
    return n;
}

/// @brief Decodes a meshlet vertex UV coordinate pair stored as two consecutive f32 words in the pages buffer.
/// @param uvDataBase Absolute word offset of the UV block in the pages buffer.
/// @param uvOff Word offset of this meshlet's UV data within the UV block.
/// @param vertexIdx The local vertex index within the meshlet to decode.
/// @returns The decoded UV coordinate as a vec2<f32>.
fn decodeUV(uvDataBase: u32, uvOff: u32, vertexIdx: u32) -> vec2<f32> {
    let base = uvDataBase + uvOff + vertexIdx * 2u;
    return vec2<f32>(
        bitcast<f32>(pagesBuffer[base + 0u]),
        bitcast<f32>(pagesBuffer[base + 1u]),
    );
}

/// @brief Decodes a packed 8-bit vertex index from the meshlet's index buffer stored as 4 indices per u32 word.
/// @param idxDataBase Absolute word offset of the index block in the pages buffer.
/// @param idxOff Word offset of this meshlet's index data within the index block.
/// @param indexIdx The flat index (triangle corner) to decode.
/// @returns The local vertex index referenced by the given triangle corner.
fn decodeIndex(idxDataBase: u32, idxOff: u32, indexIdx: u32) -> u32 {
    let word  = pagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
    let shift = (indexIdx % 4u) * 8u;
    return (word >> shift) & 0xFFu;
}

// ── Main vertex shader ────────────────────────────────────────────────────────

/// @brief Main vertex shader that decodes meshlet vertex attributes from the page buffer, applies skinning if present, and transforms vertices to clip space.
@vertex
fn vs_main(
    @builtin(vertex_index)   vertexIndex  : u32,
    @builtin(instance_index) instanceIndex: u32,
) -> VertexOutput {
    var out: VertexOutput;

#ifdef DRAW_INDIRECT_COUNT_DISABLED
    // Flat-vertex-stream variant: one draw call covers all visible clusters.
    // Each cluster occupies exactly CLUSTER_SIZE * 3 vertex indices in sequence.
    // clusterSlot indexes into visibleClusterInfos; localCorner is the
    // per-cluster corner index (0 .. CLUSTER_SIZE*3-1).
    let clusterSlot     = vertexIndex / (CLUSTER_SIZE * 3u);
    let localCorner     = vertexIndex % (CLUSTER_SIZE * 3u);
    let clusterInfo     = visibleClusterInfos[clusterSlot];
#else
    let visibleClusterCount = cullingCounters[HW_VISIBLE_CLUSTER_COUNT_INDEX];
    if (instanceIndex >= visibleClusterCount) {
        out.clipPos = vec4<f32>(0.0, 0.0, -1.0, 1.0);
        return out;
    }
    let clusterInfo     = visibleClusterInfos[instanceIndex];
#endif
    let pageIdx         = clusterInfo.pageIndex;
    let localMeshletIdx = clusterInfo.pageLocalClusterIndex;
    let instIdx         = clusterInfo.instanceIndex;
    let instance        = instances[instIdx];
    let unitScale       = bitcast<f32>(instance.unit_scale_bits);

    let entry    = pageTable[pageIdx];
    let pageBase = pageWordBase(entry);

    // ── Page header ───────────────────────────────────────────────────────────
    // Word layout:  [0] num_meshlets  [1] posDataSize  [2] normDataSize
    //               [3] uvDataSize    [4] idxDataSize   [5] bwDataSize
    //               [6] depCount
    let numMeshlets  = pagesBuffer[pageBase + 0u];
    let posDataSize  = pagesBuffer[pageBase + 1u];
    let normDataSize = pagesBuffer[pageBase + 2u];
    let uvDataSize   = pagesBuffer[pageBase + 3u];
    let idxDataSize  = pagesBuffer[pageBase + 4u];
    // bwDataSize at [5] and depCount at [6] not needed in the VS

    // ── Descriptor base for this meshlet ─────────────────────────────────────
    let db = pageBase + PAGE_HEADER_WORDS + localMeshletIdx * MESHLET_DESC_WORDS;

    let posOff   = pagesBuffer[db + DESC_POS_OFF];
    let normOff  = pagesBuffer[db + DESC_NORM_OFF];
    let uvOff    = pagesBuffer[db + DESC_UV_OFF];
    let idxOff   = pagesBuffer[db + DESC_IDX_OFF];
    let vtxCount = pagesBuffer[db + DESC_VERT_COUNT];
    let triCount = pagesBuffer[db + DESC_TRI_COUNT];
    let bitsX    = quantizedSpanToBitCount(pagesBuffer[db + DESC_POS_SPAN_X]);
    let bitsY    = quantizedSpanToBitCount(pagesBuffer[db + DESC_POS_SPAN_Y]);
    let bitsZ    = quantizedSpanToBitCount(pagesBuffer[db + DESC_POS_SPAN_Z]);
    let qFactor  = pagesBuffer[db + DESC_QFACTOR];
    let minQX    = bitcast<f32>(pagesBuffer[db + DESC_MIN_X]);
    let minQY    = bitcast<f32>(pagesBuffer[db + DESC_MIN_Y]);
    let minQZ    = bitcast<f32>(pagesBuffer[db + DESC_MIN_Z]);
    let packedGroupCluster = pagesBuffer[db + DESC_GROUP_CLUSTER];
    let groupIndex = packedGroupCluster & 0x3Fu;
    let clusterIndex = (packedGroupCluster >> 6u) & 0x7u;
    // self/parent bounds and bone-weight fields are at db+17..db+28
    // — not needed for vertex attribute decode, but accessible if required.

    // ── Absolute word bases for each vertex data block ───────────────────────
    let vDataBase    = vertexDataBase(pageBase, numMeshlets);
    let posDataBase  = vDataBase;
    let normDataBase = vDataBase + posDataSize;
    let uvDataBase   = normDataBase + normDataSize;
    let idxDataBase  = uvDataBase + uvDataSize;

    // ── Triangle / vertex decode ──────────────────────────────────────────────
    let maxCorners  = triCount * 3u;
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    // localCorner may exceed maxCorners for the degenerate padding region.
    // Clamp the index read so decodeIndex stays in-bounds, then discard.
    let cornerIdx   = min(localCorner, maxCorners - 1u);
    let localVertID = decodeIndex(idxDataBase, idxOff, cornerIdx);

    if (localCorner >= maxCorners || localVertID >= vtxCount) {
        out.clipPos = vec4<f32>(0.0, 0.0, -1.0, 1.0);
        return out;
    }
#else
    let cornerIdx   = min(vertexIndex, maxCorners - 1u);
    let localVertID = decodeIndex(idxDataBase, idxOff, cornerIdx);

    if (vertexIndex >= maxCorners || localVertID >= vtxCount) {
        out.clipPos = vec4<f32>(0.0);
        return out;
    }
#endif

    var localPos  = decodePosition(
        posDataBase, posOff,
        bitsX, bitsY, bitsZ,
        qFactor, minQX, minQY, minQZ,
        unitScale, localVertID
    );
    var localNorm = decodeNormal(normDataBase, normOff, localVertID);
    let uv = decodeUV(uvDataBase, uvOff, localVertID);
    let boneWeightsPerVertex = readDesc(pageBase, localMeshletIdx, DESC_BW_PER_V);
    if (boneWeightsPerVertex > 0u) {
        let influenceCount = min(boneWeightsPerVertex, MAX_BONE_WEIGHTS);
        var skinnedPos = vec3<f32>(0.0);
        var skinnedNorm = vec3<f32>(0.0);
        for (var influenceIndex = 0u; influenceIndex < influenceCount; influenceIndex++) {
            let bw = decodeBoneWeight(
                pageBase, numMeshlets,
                posDataSize, normDataSize, uvDataSize, idxDataSize,
                localMeshletIdx, localVertID, influenceIndex
            );
            let boneMatrix = meshPartTransforms[instance.meshPartTransformsOffset + bw.boneIndex];
            skinnedPos += bw.weight * (boneMatrix * vec4<f32>(localPos, 1.0)).xyz;
            skinnedNorm += bw.weight * (boneMatrix * vec4<f32>(localNorm, 0.0)).xyz;
        }
        localPos = skinnedPos;
        localNorm = normalize(skinnedNorm);
    }

    let worldPos  = (instance.modelMatrix * vec4<f32>(localPos,  1.0)).xyz;
    let viewProj = uniforms.proj * uniforms.view;
    let clipPos  = viewProj * vec4<f32>(worldPos, 1.0);

    #ifdef USE_REVERSE_Z
        out.clipPos = vec4<f32>(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
    #else
        out.clipPos = clipPos;
    #endif

    out.instanceIndex = instIdx;
    out.pageIndex     = pageIdx;
    out.groupIndex    = groupIndex;
    out.clusterIndex  = clusterIndex;
    out.triangleId    = cornerIdx / 3u;
    out.materialIndex = instance.materialIndex;
    out.uv            = uv;

    return out;
}
