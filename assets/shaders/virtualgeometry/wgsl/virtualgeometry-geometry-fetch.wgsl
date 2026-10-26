// Encoded geometry is addressed by descriptor word, page word, and unit scale.
// A zero unit scale marks legacy/direct-generated decoded geometry.
fn vgExtractBits(wordBase: u32, bitOffset: u32, numBits: u32) -> u32 {
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

fn vgDecodePosition(
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

    let qx = vgExtractBits(base, startBit, bitsX);
    let qy = vgExtractBits(base, startBit + bitsX, bitsY);
    let qz = vgExtractBits(base, startBit + bitsX + bitsY, bitsZ);

    let dequantScale = f32(1u << qFactor) * unitScale;
    return vec3<f32>(
        (minQX + f32(qx)) / dequantScale,
        (minQY + f32(qy)) / dequantScale,
        (minQZ + f32(qz)) / dequantScale,
    );
}

fn vgDecodeNormal(normDataBase: u32, normOff: u32, vertexIdx: u32) -> vec3<f32> {
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

fn vgDecodeUV(uvDataBase: u32, uvOff: u32, vertexIdx: u32) -> vec2<f32> {
    let base = uvDataBase + uvOff + vertexIdx * 2u;
    return vec2<f32>(
        bitcast<f32>(pagesBuffer[base + 0u]),
        bitcast<f32>(pagesBuffer[base + 1u]),
    );
}

fn vgDecodeIndex(idxDataBase: u32, idxOff: u32, indexIdx: u32) -> u32 {
    let word = pagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
    let shift = (indexIdx % 4u) * 8u;
    return (word >> shift) & 0xFFu;
}


fn vgLoadIndex(g: DecodedClusterGeometryInfo, corner: u32) -> u32 {
    if (g._padding0 == 0u) { return decodedIndexBuffer[g.indexOffset + corner]; }
    let p = g.indexOffset;
    let base = vertexDataBase(p, pagesBuffer[p]) + pagesBuffer[p+1u] + pagesBuffer[p+2u] + pagesBuffer[p+3u];
    return vgDecodeIndex(base, pagesBuffer[g.vertexOffset + DESC_IDX_OFF], corner);
}
fn vgLoadVertex(g: DecodedClusterGeometryInfo, index: u32) -> DecodedVertexData {
    if (g._padding0 == 0u) { return decodedVertexBuffer[g.vertexOffset + index]; }
    let d = g.vertexOffset;
    let p = g.indexOffset;
    let posBase = vertexDataBase(p, pagesBuffer[p]);
    let normBase = posBase + pagesBuffer[p+1u];
    let uvBase = normBase + pagesBuffer[p+2u];
    let boneBase = uvBase + pagesBuffer[p+3u] + pagesBuffer[p+4u];
    let pos = vgDecodePosition(posBase, pagesBuffer[d+DESC_POS_OFF],
        quantizedSpanToBitCount(pagesBuffer[d+DESC_POS_SPAN_X]),
        quantizedSpanToBitCount(pagesBuffer[d+DESC_POS_SPAN_Y]),
        quantizedSpanToBitCount(pagesBuffer[d+DESC_POS_SPAN_Z]),
        pagesBuffer[d+DESC_QFACTOR], bitcast<f32>(pagesBuffer[d+DESC_MIN_X]),
        bitcast<f32>(pagesBuffer[d+DESC_MIN_Y]), bitcast<f32>(pagesBuffer[d+DESC_MIN_Z]),
        bitcast<f32>(g._padding0), index);
    let normal = vgDecodeNormal(normBase, pagesBuffer[d+DESC_NORM_OFF], index);
    let uv = vgDecodeUV(uvBase, pagesBuffer[d+DESC_UV_OFF], index);
    var out: DecodedVertexData;
    out.positionX=pos.x; out.positionY=pos.y; out.positionZ=pos.z;
    out.normalX=normal.x; out.normalY=normal.y; out.normalZ=normal.z;
    out.uvX=uv.x; out.uvY=uv.y;
    out.boneWeightCount=pagesBuffer[d+DESC_BW_PER_V];
    out.boneWeightOffset=boneBase + pagesBuffer[d+DESC_BW_OFF] + index*out.boneWeightCount*2u;
    return out;
}
