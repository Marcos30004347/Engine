#include "virtualgeometrydata.wgsl"

const PAGE_DECODE_WORKGROUP_SIZE : u32 = 64u;

struct PageDecodeUniforms {
    requestCount : u32,
    _padding0    : u32,
    _padding1    : u32,
    _padding2    : u32,
}

@group(0) @binding(0) var<uniform> uniforms : PageDecodeUniforms;
@group(0) @binding(1) var<storage, read> pageDecodeRequests : array<u32>;
@group(0) @binding(2) var<storage, read> pageTable : array<PageTableEntry>;
@group(0) @binding(3) var<storage, read> pagesBuffer : array<u32>;
@group(0) @binding(4) var<storage, read_write> decodedClusterCullBuffer : array<DecodedClusterCullData>;
@group(0) @binding(5) var<storage, read_write> decodedClusterGeometryBuffer : array<DecodedClusterGeometryInfo>;
@group(0) @binding(6) var<storage, read_write> decodedVertexBuffer : array<DecodedVertexData>;
@group(0) @binding(7) var<storage, read_write> decodedIndexBuffer : array<u32>;
@group(0) @binding(8) var<storage, read_write> decodedBoneWeightBuffer : array<DecodedBoneWeightData>;

var<workgroup> clusterVertexOffsets : array<u32, MAX_CLUSTERS_PER_PAGE>;
var<workgroup> clusterIndexOffsets : array<u32, MAX_CLUSTERS_PER_PAGE>;
var<workgroup> clusterBoneWeightOffsets : array<u32, MAX_CLUSTERS_PER_PAGE>;

fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return pagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

fn extractBits(wordBase: u32, bitOffset: u32, numBits: u32) -> u32 {
    if (numBits == 0u) {
        return 0u;
    }

    let wi = wordBase + (bitOffset / 32u);
    let bi = bitOffset % 32u;
    var value = pagesBuffer[wi] >> bi;
    if (bi + numBits > 32u) {
        let overflow = (bi + numBits) - 32u;
        value |= pagesBuffer[wi + 1u] << (numBits - overflow);
    }

    let mask = select((1u << numBits) - 1u, 0xFFFFFFFFu, numBits == 32u);
    return value & mask;
}

fn decodePosition(
    posDataBase : u32,
    posOff      : u32,
    bitsX       : u32,
    bitsY       : u32,
    bitsZ       : u32,
    qFactor     : u32,
    minQX       : f32,
    minQY       : f32,
    minQZ       : f32,
    unitScale   : f32,
    vertexIdx   : u32
) -> vec3<f32> {
    let bitsPerVertex = bitsX + bitsY + bitsZ;
    let startBit = vertexIdx * bitsPerVertex;
    let base = posDataBase + posOff;

    let qx = extractBits(base, startBit, bitsX);
    let qy = extractBits(base, startBit + bitsX, bitsY);
    let qz = extractBits(base, startBit + bitsX + bitsY, bitsZ);

    let dequantScale = f32(1u << qFactor) * unitScale;
    return vec3<f32>(
        (minQX + f32(qx)) / dequantScale,
        (minQY + f32(qy)) / dequantScale,
        (minQZ + f32(qz)) / dequantScale,
    );
}

fn decodeNormal(normDataBase: u32, normOff: u32, vertexIdx: u32) -> vec3<f32> {
    let packed = pagesBuffer[normDataBase + normOff + vertexIdx];

    let rawX = i32(packed & 0xFFFFu);
    let rawY = i32((packed >> 16u) & 0xFFFFu);
    let sX = select(rawX, rawX - 65536, rawX >= 32768);
    let sY = select(rawY, rawY - 65536, rawY >= 32768);

    var ox = clamp(f32(sX) / 32767.0, -1.0, 1.0);
    var oy = clamp(f32(sY) / 32767.0, -1.0, 1.0);

    var n = vec3<f32>(ox, oy, 1.0 - abs(ox) - abs(oy));
    if (n.z < 0.0) {
        let ox2 = n.x;
        let oy2 = n.y;
        n.x = (1.0 - abs(oy2)) * select(-1.0, 1.0, ox2 >= 0.0);
        n.y = (1.0 - abs(ox2)) * select(-1.0, 1.0, oy2 >= 0.0);
    }

    let len = length(n);
    if (len > 1e-6) {
        return n / len;
    }
    return vec3<f32>(0.0, 0.0, 1.0);
}

fn decodeUV(uvDataBase: u32, uvOff: u32, vertexIdx: u32) -> vec2<f32> {
    let base = uvDataBase + uvOff + vertexIdx * 2u;
    return vec2<f32>(
        bitcast<f32>(pagesBuffer[base + 0u]),
        bitcast<f32>(pagesBuffer[base + 1u]),
    );
}

fn decodeIndex(idxDataBase: u32, idxOff: u32, indexIdx: u32) -> u32 {
    let word = pagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
    let shift = (indexIdx % 4u) * 8u;
    return (word >> shift) & 0xFFu;
}

@compute @workgroup_size(PAGE_DECODE_WORKGROUP_SIZE)
fn decodePages(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>
) {
    let requestIndex = workgroupId.x;
    if (requestIndex >= uniforms.requestCount) {
        return;
    }

    let globalPageIdx = pageDecodeRequests[requestIndex];
    if (globalPageIdx >= arrayLength(&pageTable)) {
        return;
    }

    let entry = pageTable[globalPageIdx];
    if (entry.isInstalled == 0u || entry.clusterCount == 0u || entry.clusterCount > MAX_CLUSTERS_PER_PAGE) {
        return;
    }

    let pageBase = pageWordBase(entry);
    let numMeshlets = pagesBuffer[pageBase + 0u];
    let posDataSize = pagesBuffer[pageBase + 1u];
    let normDataSize = pagesBuffer[pageBase + 2u];
    let uvDataSize = pagesBuffer[pageBase + 3u];
    let idxDataSize = pagesBuffer[pageBase + 4u];

    let vDataBase = vertexDataBase(pageBase, numMeshlets);
    let posDataBase = vDataBase;
    let normDataBase = vDataBase + posDataSize;
    let uvDataBase = normDataBase + normDataSize;
    let idxDataBase = uvDataBase + uvDataSize;
    let boneWeightDataBase = idxDataBase + idxDataSize;
    let unitScale = bitcast<f32>(entry.unitScaleBits);

    if (localId.x == 0u) {
        var vertexOffset = 0u;
        var indexOffset = 0u;
        var boneWeightOffset = 0u;
        for (var clusterLocalIdx = 0u; clusterLocalIdx < entry.clusterCount; clusterLocalIdx++) {
            clusterVertexOffsets[clusterLocalIdx] = vertexOffset;
            clusterIndexOffsets[clusterLocalIdx] = indexOffset;
            clusterBoneWeightOffsets[clusterLocalIdx] = boneWeightOffset;

            let vertexCount = readDesc(pageBase, clusterLocalIdx, DESC_VERT_COUNT);
            let triangleCount = readDesc(pageBase, clusterLocalIdx, DESC_TRI_COUNT);
            let boneWeightsPerVertex = readDesc(pageBase, clusterLocalIdx, DESC_BW_PER_V);

            vertexOffset += vertexCount;
            indexOffset += triangleCount * 3u;
            boneWeightOffset += vertexCount * boneWeightsPerVertex;
        }
    }
    workgroupBarrier();

    for (var localIdx = localId.x; localIdx < entry.clusterCount; localIdx += PAGE_DECODE_WORKGROUP_SIZE) {
        let clusterIdx = globalClusterIndex(entry, localIdx);
        let clusterVertexBase = entry.decodedVertexOffset + clusterVertexOffsets[localIdx];
        let clusterIndexBase = entry.decodedIndexOffset + clusterIndexOffsets[localIdx];
        let clusterBoneWeightBase = entry.decodedBoneWeightOffset + clusterBoneWeightOffsets[localIdx];

        let vertexCount = readDesc(pageBase, localIdx, DESC_VERT_COUNT);
        let triangleCount = readDesc(pageBase, localIdx, DESC_TRI_COUNT);
        let boneWeightsPerVertex = readDesc(pageBase, localIdx, DESC_BW_PER_V);
        let positionOffset = readDesc(pageBase, localIdx, DESC_POS_OFF);
        let normalOffset = readDesc(pageBase, localIdx, DESC_NORM_OFF);
        let uvOffset = readDesc(pageBase, localIdx, DESC_UV_OFF);
        let indexOffset = readDesc(pageBase, localIdx, DESC_IDX_OFF);

        let minQX = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_X));
        let minQY = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_Y));
        let minQZ = bitcast<f32>(readDesc(pageBase, localIdx, DESC_MIN_Z));
        let spanX = readDesc(pageBase, localIdx, DESC_POS_SPAN_X);
        let spanY = readDesc(pageBase, localIdx, DESC_POS_SPAN_Y);
        let spanZ = readDesc(pageBase, localIdx, DESC_POS_SPAN_Z);
        let qFactor = readDesc(pageBase, localIdx, DESC_QFACTOR);
        let dq = f32(1u << qFactor) * unitScale;
        let packedGroupCluster = readDesc(pageBase, localIdx, DESC_GROUP_CLUSTER);
        let bitsX = quantizedSpanToBitCount(spanX);
        let bitsY = quantizedSpanToBitCount(spanY);
        let bitsZ = quantizedSpanToBitCount(spanZ);

        decodedClusterCullBuffer[clusterIdx].localMinX = minQX / dq;
        decodedClusterCullBuffer[clusterIdx].localMinY = minQY / dq;
        decodedClusterCullBuffer[clusterIdx].localMinZ = minQZ / dq;
        decodedClusterCullBuffer[clusterIdx].triangleCount = triangleCount;
        decodedClusterCullBuffer[clusterIdx].localMaxX = (minQX + f32(spanX)) / dq;
        decodedClusterCullBuffer[clusterIdx].localMaxY = (minQY + f32(spanY)) / dq;
        decodedClusterCullBuffer[clusterIdx].localMaxZ = (minQZ + f32(spanZ)) / dq;
        decodedClusterCullBuffer[clusterIdx]._padding0 = 0u;
        decodedClusterCullBuffer[clusterIdx].selfCenterX = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CX));
        decodedClusterCullBuffer[clusterIdx].selfCenterY = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CY));
        decodedClusterCullBuffer[clusterIdx].selfCenterZ = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_CZ));
        decodedClusterCullBuffer[clusterIdx].selfRadius = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_R));
        decodedClusterCullBuffer[clusterIdx].parentCenterX = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CX));
        decodedClusterCullBuffer[clusterIdx].parentCenterY = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CY));
        decodedClusterCullBuffer[clusterIdx].parentCenterZ = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_CZ));
        decodedClusterCullBuffer[clusterIdx].parentRadius = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_R));
        decodedClusterCullBuffer[clusterIdx].coneAxisX = bitcast<f32>(readDesc(pageBase, localIdx, DESC_CONE_AXIS_X));
        decodedClusterCullBuffer[clusterIdx].coneAxisY = bitcast<f32>(readDesc(pageBase, localIdx, DESC_CONE_AXIS_Y));
        decodedClusterCullBuffer[clusterIdx].coneAxisZ = bitcast<f32>(readDesc(pageBase, localIdx, DESC_CONE_AXIS_Z));
        decodedClusterCullBuffer[clusterIdx].coneCutoff = bitcast<f32>(readDesc(pageBase, localIdx, DESC_CONE_CUTOFF));
        decodedClusterCullBuffer[clusterIdx].selfError = bitcast<f32>(readDesc(pageBase, localIdx, DESC_SELF_ERR));
        decodedClusterCullBuffer[clusterIdx].parentError = bitcast<f32>(readDesc(pageBase, localIdx, DESC_PAR_ERR));
        decodedClusterCullBuffer[clusterIdx]._padding1 = 0u;
        decodedClusterCullBuffer[clusterIdx]._padding2 = 0u;

        decodedClusterGeometryBuffer[clusterIdx].vertexOffset = clusterVertexBase;
        decodedClusterGeometryBuffer[clusterIdx].indexOffset = clusterIndexBase;
        decodedClusterGeometryBuffer[clusterIdx].vertexCount = vertexCount;
        decodedClusterGeometryBuffer[clusterIdx].triangleCount = triangleCount;
        decodedClusterGeometryBuffer[clusterIdx].pageLocalGroupIndex = packedGroupCluster & 0x3Fu;
        decodedClusterGeometryBuffer[clusterIdx].clusterIndexInGroup = (packedGroupCluster >> 6u) & 0x7u;
        decodedClusterGeometryBuffer[clusterIdx]._padding0 = 0u;
        decodedClusterGeometryBuffer[clusterIdx]._padding1 = 0u;

        for (var vertexIdx = 0u; vertexIdx < vertexCount; vertexIdx++) {
            let localPosition = decodePosition(
                posDataBase,
                positionOffset,
                bitsX,
                bitsY,
                bitsZ,
                qFactor,
                minQX,
                minQY,
                minQZ,
                unitScale,
                vertexIdx
            );
            let localNormal = decodeNormal(normDataBase, normalOffset, vertexIdx);
            let uv = decodeUV(uvDataBase, uvOffset, vertexIdx);
            let vertexBoneWeightOffset = clusterBoneWeightBase + vertexIdx * boneWeightsPerVertex;
            let decodedVertexIndex = clusterVertexBase + vertexIdx;

            decodedVertexBuffer[decodedVertexIndex].positionX = localPosition.x;
            decodedVertexBuffer[decodedVertexIndex].positionY = localPosition.y;
            decodedVertexBuffer[decodedVertexIndex].positionZ = localPosition.z;
            decodedVertexBuffer[decodedVertexIndex].boneWeightOffset = vertexBoneWeightOffset;
            decodedVertexBuffer[decodedVertexIndex].normalX = localNormal.x;
            decodedVertexBuffer[decodedVertexIndex].normalY = localNormal.y;
            decodedVertexBuffer[decodedVertexIndex].normalZ = localNormal.z;
            decodedVertexBuffer[decodedVertexIndex].boneWeightCount = boneWeightsPerVertex;
            decodedVertexBuffer[decodedVertexIndex].uvX = uv.x;
            decodedVertexBuffer[decodedVertexIndex].uvY = uv.y;
            decodedVertexBuffer[decodedVertexIndex]._padding0 = 0u;
            decodedVertexBuffer[decodedVertexIndex]._padding1 = 0u;
        }

        let cornerCount = triangleCount * 3u;
        for (var cornerIdx = 0u; cornerIdx < cornerCount; cornerIdx++) {
            decodedIndexBuffer[clusterIndexBase + cornerIdx] = decodeIndex(idxDataBase, indexOffset, cornerIdx);
        }

        let totalBoneWeights = vertexCount * boneWeightsPerVertex;
        for (var boneWeightIdx = 0u; boneWeightIdx < totalBoneWeights; boneWeightIdx++) {
            let sourceWordBase = boneWeightDataBase + readDesc(pageBase, localIdx, DESC_BW_OFF) + boneWeightIdx * 2u;
            decodedBoneWeightBuffer[clusterBoneWeightBase + boneWeightIdx].weight = bitcast<f32>(pagesBuffer[sourceWordBase + 0u]);
            decodedBoneWeightBuffer[clusterBoneWeightBase + boneWeightIdx].boneIndex = pagesBuffer[sourceWordBase + 1u];
        }
    }
}
