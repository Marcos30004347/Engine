const GPUSignedDistanceFields_InvalidId : u32 = 0x00ffffffu;
const GPUSignedDistanceFields_UninitializedId : u32 = 0xffffffffu;
const GPUSignedDistanceFields_InvalidBottomAabbNode : u32 = 0x00007fffu;
const GPUSignedDistanceFields_CascadeResolution : u32 = 64u;
const GPUSignedDistanceFields_CascadeDegree : u32 = 6u;
const GPUSignedDistanceFields_CascadeWrapMask : u32 = 63u;
const GPUSignedDistanceFields_CascadeVoxelCount : u32 = 64u * 64u * 64u;
const GPUSignedDistanceFields_BrickResolution : u32 = 8u;
const GPUSignedDistanceFields_BrickDegree : u32 = 3u;
const GPUSignedDistanceFields_BrickCellCount : u32 = 8u * 8u * 8u;
const GPUSignedDistanceFields_BrickAabbDistanceThreshold : f32 = (0.75 * 0.75) / 16.0;
const GPUSignedDistanceFields_VoxelizerGroupSize : u32 = 256u;
const GPUSignedDistanceFields_ScanGroupSize : u32 = 256u;
const GPUSignedDistanceFields_CompactReferencesGroupSize : u32 = 64u;
const GPUSignedDistanceFields_EmitSdfGroupSize : u32 = 32u;
const GPUSignedDistanceFields_EmitSdfRefsPerGroup : u32 = 32u;
const GPUSignedDistanceFields_SurfaceCacheFaceCount : u32 = 6u;
const GPUSignedDistanceFields_SurfaceCacheFace_PosX : u32 = 0u;
const GPUSignedDistanceFields_SurfaceCacheFace_NegX : u32 = 1u;
const GPUSignedDistanceFields_SurfaceCacheFace_PosY : u32 = 2u;
const GPUSignedDistanceFields_SurfaceCacheFace_NegY : u32 = 3u;
const GPUSignedDistanceFields_SurfaceCacheFace_PosZ : u32 = 4u;
const GPUSignedDistanceFields_SurfaceCacheFace_NegZ : u32 = 5u;

const GPUSignedDistanceFields_ContextCounter_BrickCount : u32 = 0u;
const GPUSignedDistanceFields_ContextCounter_DirtyBricks : u32 = 1u;
const GPUSignedDistanceFields_ContextCounter_FreeBricks : u32 = 2u;
const GPUSignedDistanceFields_ContextCounter_ClearBricks : u32 = 3u;
const GPUSignedDistanceFields_ContextCounter_MergeBricks : u32 = 4u;
const GPUSignedDistanceFields_ContextCounter_Count : u32 = 6u;

const GPUSignedDistanceFields_ScratchCounter_Triangles : u32 = 0u;
const GPUSignedDistanceFields_ScratchCounter_MaxTriangles : u32 = 1u;
const GPUSignedDistanceFields_ScratchCounter_References : u32 = 2u;
const GPUSignedDistanceFields_ScratchCounter_MaxReferences : u32 = 3u;
const GPUSignedDistanceFields_ScratchCounter_GroupIndex : u32 = 4u;
const GPUSignedDistanceFields_ScratchCounter_CompressionBricks : u32 = 5u;
const GPUSignedDistanceFields_ScratchCounter_StorageOffset : u32 = 6u;
const GPUSignedDistanceFields_ScratchCounter_StorageSize : u32 = 7u;
const GPUSignedDistanceFields_ScratchCounter_NumBricksAllocated : u32 = 8u;
const GPUSignedDistanceFields_ScratchCounter_ClearBricks : u32 = 9u;
const GPUSignedDistanceFields_ScratchCounter_Count : u32 = 10u;

const GPUSignedDistanceFields_IndirectOffset_InitializeCascade : u32 = 0u;
const GPUSignedDistanceFields_IndirectOffset_CompactReferences : u32 = 16u;
const GPUSignedDistanceFields_IndirectOffset_EmitSdf : u32 = 32u;
const GPUSignedDistanceFields_IndirectOffset_Voxelize : u32 = 48u;
const GPUSignedDistanceFields_IndirectOffset_CompressBricks : u32 = 64u;
const GPUSignedDistanceFields_IndirectOffset_Eikonal : u32 = 80u;
const GPUSignedDistanceFields_IndirectOffset_ClearBricks : u32 = 96u;
const GPUSignedDistanceFields_IndirectOffset_ClearScratchBricks : u32 = 96u;
const GPUSignedDistanceFields_IndirectOffset_MergeBricks : u32 = 112u;
const GPUSignedDistanceFields_IndirectOffset_ScrollCascade : u32 = 128u;
const GPUSignedDistanceFields_IndirectOffset_ClearRefCounters : u32 = 144u;
const GPUSignedDistanceFields_IndirectOffset_ClearJobCounter : u32 = 160u;
const GPUSignedDistanceFields_IndirectOffset_InvalidateJobAreas : u32 = 176u;
const GPUSignedDistanceFields_IndirectOffset_CoarseCulling : u32 = 192u;
const GPUSignedDistanceFields_IndirectOffset_ScanJobs : u32 = 208u;
const GPUSignedDistanceFields_IndirectOffset_FinalizeJobScan : u32 = 224u;
const GPUSignedDistanceFields_IndirectOffset_ScanReferences : u32 = 240u;
const GPUSignedDistanceFields_IndirectOffset_FinalizeReferenceScan : u32 = 256u;
const GPUSignedDistanceFields_IndirectOffset_Count : u32 = 272u;

const GPUSignedDistanceFields_BuildTreeBottomNodeCount : u32 = 16u * 16u * 16u;
const GPUSignedDistanceFields_BuildTreeMidNodeCount : u32 = 4u * 4u * 4u;
const GPUSignedDistanceFields_BuildTreeNodeCount : u32 = GPUSignedDistanceFields_BuildTreeBottomNodeCount + GPUSignedDistanceFields_BuildTreeMidNodeCount + 1u;
const GPUSignedDistanceFields_BuildTreeFloat4Count : u32 = GPUSignedDistanceFields_BuildTreeNodeCount * 2u;
const GPUSignedDistanceFields_TriangleSmallFlag : u32 = 0x80000000u;
const GPUSignedDistanceFields_TriangleOffsetMask : u32 = 0x7fffffffu;

struct MeshTableEntryGPU {
    vertexOffset : u32,
    vertexCount : u32,
    indexOffset : u32,
    triangleCount : u32,
    boneWeightsPerVertex : u32,
    flags : u32,
    surfaceMaterialIndex : u32,
    _padding1 : u32,
}

struct SurfaceMaterialGPU {
    baseColorOpacity : vec4<f32>,
    normalRoughness : vec4<f32>,
    emissiveMetallic : vec4<f32>,
    flags : vec4<u32>,
}

struct VertexGPU {
    position : vec3<f32>,
    boneWeightOffset : u32,
    uvMaterial : vec4<f32>,
    surfaceBaseColorOpacity : vec4<f32>,
    surfaceNormalRoughness : vec4<f32>,
    surfaceEmissiveMetallic : vec4<f32>,
    surfaceFlags : vec4<u32>,
}

struct BoneWeightGPU {
    weight : f32,
    boneIndex : u32,
    _padding0 : u32,
    _padding1 : u32,
}

struct InstanceGPU {
    modelMatrix : mat4x4<f32>,
    previousModelMatrix : mat4x4<f32>,
    aabbMin : vec3<f32>,
    meshIndex : u32,
    aabbMax : vec3<f32>,
    flags : u32,
    boneMatrixOffset : u32,
    previousBoneMatrixOffset : u32,
    boneCount : u32,
    _padding0 : u32,
}

struct JobGPU {
    aabbMin : vec3<u32>,
    instanceIndex : u32,
    aabbMax : vec3<u32>,
    flags : u32,
}

struct RawTriangleReferenceGPU {
    voxelIndex : u32,
    triangleId : u32,
    localReferenceIndex : u32,
    _padding0 : u32,
}

struct CascadeInfoGPU {
    gridMin : vec3<f32>,
    voxelSize : f32,
    gridMax : vec3<f32>,
    flags : u32,
    clipmapOffset : vec3<u32>,
    _padding0 : u32,
    clipmapInvalidationOffset : vec3<i32>,
    _padding1 : u32,
    ioffset : vec3<i32>,
    index : u32,
    gridMid : vec3<f32>,
    isEnabled : u32,
    relGridMinFP16 : vec2<u32>,
    relGridMaxFP16 : vec2<u32>,
    _padding2 : u32,
    _padding3 : u32,
    iVoxelSize : f32,
    isInitialized : u32,
}

struct CascadeBuildInfoGPU {
    numJobs : u32,
    numJobVoxels : u32,
    voxelizeDispatchX : u32,
    compactReferencesDispatchX : u32,
    clearBrickStorageDispatchX : u32,
    emitSdfDispatchX : u32,
    compressBricksDispatchX : u32,
    _padding0 : u32,
}

struct TriangleData {
    jobIndex : u32,
    triangleIndex : u32,
    surfaceMaterialIndex : u32,
    wp0 : vec3<f32>,
    wp1 : vec3<f32>,
    wp2 : vec3<f32>,
}

struct TriangleVoxelBounds {
    boundMin : vec3<f32>,
    boundMax : vec3<f32>,
    voxelMin : vec3<u32>,
    voxelMax : vec3<u32>,
}

struct CRItem {
    bounds_min : vec3<u32>,
    bounds_max : vec3<u32>,
}

struct TrianglePartial {
    wp0 : vec3<f32>,
    wp1 : vec3<f32>,
    wp2 : vec3<f32>,
    surfaceMaterialIndex : u32,
    jobIndex : u32,
    triangleIndex : u32,
}

struct TriangleCenter {
    center : vec3<f32>,
    surfaceMaterialIndex : u32,
    jobIndex : u32,
    triangleIndex : u32,
}

struct EdgeData {
    deXY : vec3<f32>,
    neXY0 : vec2<f32>,
    neXY1 : vec2<f32>,
    neXY2 : vec2<f32>,
    deXZ : vec3<f32>,
    neXZ0 : vec2<f32>,
    neXZ1 : vec2<f32>,
    neXZ2 : vec2<f32>,
    deYZ : vec3<f32>,
    neYZ0 : vec2<f32>,
    neYZ1 : vec2<f32>,
    neYZ2 : vec2<f32>,
    gn : vec3<f32>,
}

fn GPUSignedDistanceFieldsUnflatten3D(flatIndex: u32, dimension: u32) -> vec3<u32> {
    let plane = dimension * dimension;
    let z = flatIndex / plane;
    let rem = flatIndex - z * plane;
    let y = rem / dimension;
    let x = rem - y * dimension;
    return vec3<u32>(x, y, z);
}

fn GPUSignedDistanceFieldsUnflatten2D(flatIndex: u32, dimension: vec2<u32>) -> vec2<u32> {
    return vec2<u32>(flatIndex % dimension.x, (flatIndex / dimension.x) % dimension.y);
}

fn GPUSignedDistanceFieldsUnflatten(flatIndex: u32, dimension: vec3<u32>) -> vec3<u32> {
    return vec3<u32>(flatIndex % dimension.x, (flatIndex / dimension.x) % dimension.y, flatIndex / (dimension.x * dimension.y));
}

fn GPUSignedDistanceFieldsFlatten3D(coord: vec3<u32>, dimension: u32) -> u32 {
    return coord.x + coord.y * dimension + coord.z * dimension * dimension;
}

fn GPUSignedDistanceFieldsFlatten(coord: vec3<u32>, dimension: vec3<u32>) -> u32 {
    return coord.x + coord.y * dimension.x + coord.z * dimension.x * dimension.y;
}

fn GPUSignedDistanceFieldsSurfaceCachePageIndex(brickId: u32, faceIndex: u32) -> u32 {
    return brickId * GPUSignedDistanceFields_SurfaceCacheFaceCount + min(faceIndex, GPUSignedDistanceFields_SurfaceCacheFaceCount - 1u);
}

fn GPUSignedDistanceFieldsSurfaceCachePageOrigin(pageIndex: u32, pageResolution: u32, pagesPerRow: u32) -> vec2<u32> {
    let safePagesPerRow = max(pagesPerRow, 1u);
    let pageCoord = vec2<u32>(pageIndex % safePagesPerRow, pageIndex / safePagesPerRow);
    return pageCoord * pageResolution;
}

fn GPUSignedDistanceFieldsSurfaceCacheTexel(brickId: u32, faceIndex: u32, pageTexel: vec2<u32>, pageResolution: u32, pagesPerRow: u32) -> vec2<u32> {
    let pageIndex = GPUSignedDistanceFieldsSurfaceCachePageIndex(brickId, faceIndex);
    let clampedTexel = min(pageTexel, vec2<u32>(max(pageResolution, 1u) - 1u));
    return GPUSignedDistanceFieldsSurfaceCachePageOrigin(pageIndex, pageResolution, pagesPerRow) + clampedTexel;
}

fn GPUSignedDistanceFieldsSurfaceCacheFaceUv(faceIndex: u32, uvw: vec3<f32>) -> vec2<f32> {
    switch (faceIndex) {
        case GPUSignedDistanceFields_SurfaceCacheFace_PosX: { return uvw.zy; }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegX: { return vec2<f32>(1.0 - uvw.z, uvw.y); }
        case GPUSignedDistanceFields_SurfaceCacheFace_PosY: { return uvw.xz; }
        case GPUSignedDistanceFields_SurfaceCacheFace_NegY: { return vec2<f32>(uvw.x, 1.0 - uvw.z); }
        case GPUSignedDistanceFields_SurfaceCacheFace_PosZ: { return uvw.xy; }
        default: { return vec2<f32>(1.0 - uvw.x, uvw.y); }
    }
}

fn GPUSignedDistanceFieldsSurfaceCacheFaceFromNormal(normal: vec3<f32>) -> u32 {
    let absNormal = abs(normal);
    if (absNormal.x >= absNormal.y && absNormal.x >= absNormal.z) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegX, GPUSignedDistanceFields_SurfaceCacheFace_PosX, normal.x >= 0.0);
    }
    if (absNormal.y >= absNormal.z) {
        return select(GPUSignedDistanceFields_SurfaceCacheFace_NegY, GPUSignedDistanceFields_SurfaceCacheFace_PosY, normal.y >= 0.0);
    }
    return select(GPUSignedDistanceFields_SurfaceCacheFace_NegZ, GPUSignedDistanceFields_SurfaceCacheFace_PosZ, normal.z >= 0.0);
}

fn GPUSignedDistanceFieldsIsBrickValid(brickId: u32) -> bool {
    return brickId != GPUSignedDistanceFields_InvalidId && brickId != GPUSignedDistanceFields_UninitializedId;
}

fn GPUSignedDistanceFieldsIsInvalidId(id: u32) -> bool {
    return (id & GPUSignedDistanceFields_InvalidId) == GPUSignedDistanceFields_InvalidId;
}

fn GPUSignedDistanceFieldsHasClipmapInvalidation(cascade: CascadeInfoGPU) -> bool {
    return any(cascade.clipmapInvalidationOffset != vec3<i32>(0, 0, 0));
}

fn GPUSignedDistanceFieldsMakeVoxelId(cascadeIndex: u32, voxelIndex: u32) -> u32 {
    return (cascadeIndex << 24u) | (voxelIndex & 0x00ffffffu);
}

fn GPUSignedDistanceFieldsVoxelIdCascade(voxelId: u32) -> u32 {
    return voxelId >> 24u;
}

fn GPUSignedDistanceFieldsVoxelIdIndex(voxelId: u32) -> u32 {
    return voxelId & 0x00ffffffu;
}

fn GPUSignedDistanceFieldsTriangleIsSmall(triangleId: u32) -> bool {
    return (triangleId & GPUSignedDistanceFields_TriangleSmallFlag) != 0u;
}

fn GPUSignedDistanceFieldsTriangleIdGetOffset(triangleId: u32) -> u32 {
    return triangleId & GPUSignedDistanceFields_TriangleOffsetMask;
}

fn GPUSignedDistanceFieldsWrapCoords(cascade: CascadeInfoGPU, voxelCoord: vec3<u32>) -> vec3<u32> {
    return (voxelCoord + cascade.clipmapOffset) & vec3<u32>(
        GPUSignedDistanceFields_CascadeWrapMask,
        GPUSignedDistanceFields_CascadeWrapMask,
        GPUSignedDistanceFields_CascadeWrapMask);
}

fn GPUSignedDistanceFieldsWrapFlatCoords(cascade: CascadeInfoGPU, voxelIndex: u32) -> u32 {
    return GPUSignedDistanceFieldsFlatten3D(
        GPUSignedDistanceFieldsWrapCoords(cascade, GPUSignedDistanceFieldsUnflatten3D(voxelIndex, GPUSignedDistanceFields_CascadeResolution)),
        GPUSignedDistanceFields_CascadeResolution);
}

fn GPUSignedDistanceFieldsWrapVoxelCoord(cascade: CascadeInfoGPU, voxelCoord: vec3<u32>) -> vec3<u32> {
    return GPUSignedDistanceFieldsWrapCoords(cascade, voxelCoord);
}

fn GPUSignedDistanceFieldsWrapFlatVoxelIndex(cascade: CascadeInfoGPU, voxelIndex: u32) -> u32 {
    return GPUSignedDistanceFieldsWrapFlatCoords(cascade, voxelIndex);
}

fn GPUSignedDistanceFieldsAtlasBrickCoord(brickId: u32, atlasBricksPerAxis: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsUnflatten3D(brickId, atlasBricksPerAxis);
}

fn GPUSignedDistanceFieldsAtlasCellCoord(brickId: u32, localCellCoord: vec3<u32>, atlasBricksPerAxis: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsAtlasBrickCoord(brickId, atlasBricksPerAxis) * GPUSignedDistanceFields_BrickResolution + localCellCoord;
}

fn GPUSignedDistanceFieldsVoxelMin(cascade: CascadeInfoGPU, voxelIndex: u32) -> vec3<f32> {
    return cascade.gridMin + vec3<f32>(GPUSignedDistanceFieldsUnflatten3D(voxelIndex, GPUSignedDistanceFields_CascadeResolution)) * cascade.voxelSize;
}

fn GPUSignedDistanceFieldsPackDistance(distanceValue: f32) -> u32 {
    let uval = bitcast<u32>(distanceValue);
    return (uval << 1u) | (uval >> 31u);
}

fn GPUSignedDistanceFieldsUnpackDistance(uval: u32) -> f32 {
    return bitcast<f32>((uval >> 1u) | ((uval & 1u) << 31u));
}

fn GPUSignedDistanceFieldsUnsignedMin(a: f32, b: f32) -> f32 {
    return select(b, a, abs(a) < abs(b));
}

fn GPUSignedDistanceFieldsGetSign(v: f32) -> f32 {
    return select(1.0, -1.0, v < 0.0);
}

fn GPUSignedDistanceFieldsDot2Vec3(v: vec3<f32>) -> f32 {
    return dot(v, v);
}

fn GPUSignedDistanceFieldsDot2Vec2(v: vec2<f32>) -> f32 {
    return dot(v, v);
}

fn GPUSignedDistanceFieldsSaturate(value: f32) -> f32 {
    return clamp(value, 0.0, 1.0);
}

fn GPUSignedDistanceFieldsCalculateDistanceToTriangleSquared(
    ba: vec3<f32>,
    pa: vec3<f32>,
    cb: vec3<f32>,
    pb: vec3<f32>,
    ac: vec3<f32>,
    pc: vec3<f32>,
    nor: vec3<f32>,
    crossBaNor: vec3<f32>,
    crossCbNor: vec3<f32>,
    crossAcNor: vec3<f32>,
    dot2Ba: f32,
    dot2Cb: f32,
    dot2Ac: f32,
    dot2Nor: f32) -> f32 {
    let inside = sign(dot(crossBaNor, pa)) + sign(dot(crossCbNor, pb)) + sign(dot(crossAcNor, pc));
    if (inside < 2.0) {
        let d0 = GPUSignedDistanceFieldsDot2Vec3(ba * clamp(dot(ba, pa) / max(dot2Ba, 1.0e-20), 0.0, 1.0) - pa);
        let d1 = GPUSignedDistanceFieldsDot2Vec3(cb * clamp(dot(cb, pb) / max(dot2Cb, 1.0e-20), 0.0, 1.0) - pb);
        let d2 = GPUSignedDistanceFieldsDot2Vec3(ac * clamp(dot(ac, pc) / max(dot2Ac, 1.0e-20), 0.0, 1.0) - pc);
        return min(d0, min(d1, d2));
    }

    return dot(nor, pa) * dot(nor, pa) / max(dot2Nor, 1.0e-20);
}

fn GPUSignedDistanceFieldsPackF16x2(v: vec2<f32>) -> u32 {
    return GPUSignedDistanceFieldsF32ToF16(v.x) | (GPUSignedDistanceFieldsF32ToF16(v.y) << 16u);
}

fn GPUSignedDistanceFieldsUnpackF16x2(v: u32) -> vec2<f32> {
    return vec2<f32>(GPUSignedDistanceFieldsF16ToF32(v & 0xffffu), GPUSignedDistanceFieldsF16ToF32(v >> 16u));
}

fn GPUSignedDistanceFieldsF32ToF16(value: f32) -> u32 {
    let bits = bitcast<u32>(value);
    let sign = (bits >> 16u) & 0x8000u;
    var mantissa = bits & 0x007fffffu;
    let exponent32 = (bits >> 23u) & 0xffu;
    var exponent = i32(exponent32) - 127 + 15;

    if (exponent32 == 0xffu) {
        return sign | 0x7c00u | select(0u, 1u, mantissa != 0u);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return sign;
        }
        mantissa = mantissa | 0x00800000u;
        let shift = u32(14 - exponent);
        return sign | (mantissa >> shift);
    }
    if (exponent >= 31) {
        return sign | 0x7c00u;
    }

    return sign | (u32(exponent) << 10u) | (mantissa >> 13u);
}

fn GPUSignedDistanceFieldsF16ToF32(value: u32) -> f32 {
    let sign = (value & 0x8000u) << 16u;
    var exponent = (value >> 10u) & 0x1fu;
    var mantissa = value & 0x03ffu;

    if (exponent == 0u) {
        if (mantissa == 0u) {
            return bitcast<f32>(sign);
        }

        var exponent32 = 127u - 15u + 1u;
        loop {
            if ((mantissa & 0x0400u) != 0u) {
                break;
            }
            mantissa = mantissa << 1u;
            exponent32 = exponent32 - 1u;
        }
        mantissa = mantissa & 0x03ffu;
        return bitcast<f32>(sign | (exponent32 << 23u) | (mantissa << 13u));
    }

    if (exponent == 31u) {
        return bitcast<f32>(sign | 0x7f800000u | (mantissa << 13u));
    }

    let exponent32 = exponent - 15u + 127u;
    return bitcast<f32>(sign | (exponent32 << 23u) | (mantissa << 13u));
}

fn GPUSignedDistanceFieldsOffsetByMax(deIn: f32, ne: vec2<f32>, offset: f32) -> f32 {
    var de = deIn;
    de += max(0.0, ne.x) + abs(ne.x * offset);
    de += max(0.0, ne.y) + abs(ne.y * offset);
    return de;
}

fn GPUSignedDistanceFieldsOffsetByMin(deIn: f32, ne: vec2<f32>, offset: f32) -> f32 {
    var de = deIn;
    de += min(0.0, ne.x) - abs(ne.x * offset);
    de += min(0.0, ne.y) - abs(ne.y * offset);
    return de;
}

fn GPUSignedDistanceFieldsGet2DEdge(orientation: f32, edgeX: f32, edgeY: f32, vertexX: f32, vertexY: f32, offset: f32) -> vec3<f32> {
    let ne = vec2<f32>(-orientation * edgeY, orientation * edgeX);
    let de = GPUSignedDistanceFieldsOffsetByMax(-(ne.x * vertexX + ne.y * vertexY), ne, offset);
    return vec3<f32>(ne, de);
}

fn GPUSignedDistanceFieldsGet2DEdges(p0: vec3<f32>, p1: vec3<f32>, p2: vec3<f32>, offset: f32, invert: bool) -> EdgeData {
    var e0 = p1 - p0;
    var e1 = p2 - p1;
    var e2 = p0 - p2;
    if (invert) {
        e0 = -e0;
        e1 = -e1;
        e2 = -e2;
    }

    var result : EdgeData;
    result.gn = normalize(cross(e2, e0));

    let orientationXY = select(1.0, -1.0, result.gn.z < 0.0);
    let xy0 = GPUSignedDistanceFieldsGet2DEdge(orientationXY, e0.x, e0.y, p0.x, p0.y, offset);
    let xy1 = GPUSignedDistanceFieldsGet2DEdge(orientationXY, e1.x, e1.y, p1.x, p1.y, offset);
    let xy2 = GPUSignedDistanceFieldsGet2DEdge(orientationXY, e2.x, e2.y, p2.x, p2.y, offset);
    result.neXY0 = xy0.xy;
    result.neXY1 = xy1.xy;
    result.neXY2 = xy2.xy;
    result.deXY = vec3<f32>(xy0.z, xy1.z, xy2.z);

    let orientationXZ = select(1.0, -1.0, result.gn.y > 0.0);
    let xz0 = GPUSignedDistanceFieldsGet2DEdge(orientationXZ, e0.x, e0.z, p0.x, p0.z, offset);
    let xz1 = GPUSignedDistanceFieldsGet2DEdge(orientationXZ, e1.x, e1.z, p1.x, p1.z, offset);
    let xz2 = GPUSignedDistanceFieldsGet2DEdge(orientationXZ, e2.x, e2.z, p2.x, p2.z, offset);
    result.neXZ0 = xz0.xy;
    result.neXZ1 = xz1.xy;
    result.neXZ2 = xz2.xy;
    result.deXZ = vec3<f32>(xz0.z, xz1.z, xz2.z);

    let orientationYZ = select(1.0, -1.0, result.gn.x < 0.0);
    let yz0 = GPUSignedDistanceFieldsGet2DEdge(orientationYZ, e0.y, e0.z, p0.y, p0.z, offset);
    let yz1 = GPUSignedDistanceFieldsGet2DEdge(orientationYZ, e1.y, e1.z, p1.y, p1.z, offset);
    let yz2 = GPUSignedDistanceFieldsGet2DEdge(orientationYZ, e2.y, e2.z, p2.y, p2.z, offset);
    result.neYZ0 = yz0.xy;
    result.neYZ1 = yz1.xy;
    result.neYZ2 = yz2.xy;
    result.deYZ = vec3<f32>(yz0.z, yz1.z, yz2.z);
    return result;
}

fn GPUSignedDistanceFieldsEvalEdge(vertex: vec2<f32>, de: vec3<f32>, ne0: vec2<f32>, ne1: vec2<f32>, ne2: vec2<f32>) -> bool {
    return (dot(ne0, vertex) + de.x >= 0.0) &&
        (dot(ne1, vertex) + de.y >= 0.0) &&
        (dot(ne2, vertex) + de.z >= 0.0);
}

fn GPUSignedDistanceFieldsPackBrickAabb(aabbMin: vec3<u32>, aabbMax: vec3<u32>) -> u32 {
    return GPUSignedDistanceFieldsFlatten3D(min(vec3<u32>(7u, 7u, 7u), aabbMin), GPUSignedDistanceFields_BrickResolution) |
        (GPUSignedDistanceFieldsFlatten3D(min(vec3<u32>(7u, 7u, 7u), aabbMax), GPUSignedDistanceFields_BrickResolution) << 9u);
}

fn GPUSignedDistanceFieldsUnpackBrickAabbMin(packed: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsUnflatten3D(packed & 0x1ffu, GPUSignedDistanceFields_BrickResolution);
}

fn GPUSignedDistanceFieldsUnpackBrickAabbMax(packed: u32) -> vec3<u32> {
    return GPUSignedDistanceFieldsUnflatten3D((packed >> 9u) & 0x1ffu, GPUSignedDistanceFields_BrickResolution);
}
