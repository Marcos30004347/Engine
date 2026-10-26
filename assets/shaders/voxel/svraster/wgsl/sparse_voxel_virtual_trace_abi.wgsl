#include "sparse_voxel_virtual_trace_common.wgsl"

@compute @workgroup_size(1, 1, 1)
fn main() {
    let page = VoxelTracePage(
        7u,
        3u,
        0x89abcdefu,
        0x01234567u,
        VOXEL_TRACE_FORMAT_CUBIC64,
        11u,
        13u,
        17u,
        19u,
        23u,
        29u,
        31u,
        1.0,
        2.0,
        3.0,
        64.0,
        0.25,
        8.25,
        VOXEL_TRACE_INVALID_OFFSET,
        VOXEL_TRACE_PAGE_VALID | VOXEL_TRACE_PAGE_CONSERVATIVE_FALLBACK);
    let node = VoxelTraceNode64(
        42u << 1u,
        0x80000001u,
        0x00000003u,
        99u);
    let child = voxelTraceNode64ChildIndex(3u, 3u, 3u);
    let contains0 = voxelTraceNode64PopulationContains(node, 0u);
    let contains63 = voxelTraceNode64PopulationContains(node, child);
    let before33 = voxelTraceNode64PopulationCountBefore(node, 33u);
    let packed = voxelTracePackQ8Error4(1u, 2u, 3u, 255u);
    let unpacked = voxelTraceUnpackQ8Error(packed, 3u);
    let encoded = voxelTraceEncodeQ8Error(4.25, page.errorMin, page.errorMax);
    let decoded = voxelTraceDecodeQ8Error(encoded, page.errorMin, page.errorMax);
    let childOrigin = voxelTraceNode64ChildOrigin(
        vec3<f32>(page.localOriginX, page.localOriginY, page.localOriginZ),
        page.rootSide,
        child);
    let childOctant = voxelTraceNode64ChildOctant(child);
    let octantOrigin = voxelTraceNode64OctantOrigin(
        vec3<f32>(page.localOriginX, page.localOriginY, page.localOriginZ),
        page.rootSide,
        childOctant);
    let octantMaskLo = voxelTraceNode64OctantMaskLo(childOctant);
    let octantMaskHi = voxelTraceNode64OctantMaskHi(childOctant);
    let ray = VoxelTraceRay(
        vec3<f32>(-1.0, 0.5, 0.5),
        0.0,
        vec3<f32>(1.0, 0.0, 0.0),
        100.0);
    let boxHit = voxelTraceRayAabb(ray, vec3<f32>(0.0), vec3<f32>(1.0));
    let projectedThreshold = voxelTraceProjectedErrorDistanceThreshold(
        vec3<f32>(100.0, 100.0, 100.0),
        childOrigin,
        childOrigin + vec3<f32>(page.rootSide / f32(VOXEL_TRACE_BRANCHING)),
        0.25,
        0.001);
    let cullSkip = voxelTraceCullExpansionDecisionQ8(0u, 0.0, 10.0, 0.1);
    let cullExpand = voxelTraceCullExpansionDecisionQ8(255u, 0.0, 10.0, 9.8);
    let childBase = voxelTraceNode64ChildBase(node);
    let isTerminal = voxelTraceNode64Terminal(node);
    let boundsExpX = voxelTraceEncodeSignedExponent8(8.0);
    let boundsScale = voxelTraceDecodeExponentScale(boundsExpX);
    let boundsPackedScale = voxelTraceBoundsPackScaleExponents(boundsExpX, boundsExpX, boundsExpX, 8u);
    let boundsChildCount = voxelTraceBoundsChildCount(boundsPackedScale);
    let boundsQLo = voxelTraceBoundsQuantizeLower(1.25, 0.0, boundsScale);
    let boundsQHi = voxelTraceBoundsQuantizeUpper(2.25, 0.0, boundsScale);
    let boundsNode8 = VoxelTraceBounds8Node(
        vec3<f32>(0.0),
        boundsPackedScale,
        4u,
        100u,
        0x00000003u,
        0x00000002u,
        array<u32, 2>(voxelTracePackQ8Error4(boundsQLo, 0u, 0u, 0u), 0u),
        array<u32, 2>(voxelTracePackQ8Error4(boundsQLo, 0u, 0u, 0u), 0u),
        array<u32, 2>(voxelTracePackQ8Error4(boundsQLo, 0u, 0u, 0u), 0u),
        array<u32, 2>(voxelTracePackQ8Error4(boundsQHi, 0u, 0u, 0u), 0u),
        array<u32, 2>(voxelTracePackQ8Error4(boundsQHi, 0u, 0u, 0u), 0u),
        array<u32, 2>(voxelTracePackQ8Error4(boundsQHi, 0u, 0u, 0u), 0u));
    let boundsMin8 = voxelTraceBoundsDecodeChildMinimum(
        boundsNode8.origin,
        boundsNode8.scaleExponentsAndChildCount,
        boundsNode8.qLoX[0],
        boundsNode8.qLoY[0],
        boundsNode8.qLoZ[0],
        0u);
    let boundsMax8 = voxelTraceBoundsDecodeChildMaximum(
        boundsNode8.origin,
        boundsNode8.scaleExponentsAndChildCount,
        boundsNode8.qHiX[0],
        boundsNode8.qHiY[0],
        boundsNode8.qHiZ[0],
        0u);
    let boundsNode16 = VoxelTraceBounds16Node(
        vec3<f32>(0.0),
        voxelTraceBoundsPackScaleExponents(boundsExpX, boundsExpX, boundsExpX, 16u),
        4u,
        100u,
        0x0000ffffu,
        0x00008000u,
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQLo)),
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQLo)),
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQLo)),
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQHi)),
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQHi)),
        array<u32, 4>(0u, 0u, 0u, voxelTracePackQ8Error4(0u, 0u, 0u, boundsQHi)));
    let boundsMin16 = voxelTraceBoundsDecodeChildMinimum(
        boundsNode16.origin,
        boundsNode16.scaleExponentsAndChildCount,
        boundsNode16.qLoX[3],
        boundsNode16.qLoY[3],
        boundsNode16.qLoZ[3],
        3u);
    _ = contains0;
    _ = contains63;
    _ = before33;
    _ = unpacked;
    _ = decoded;
    _ = childOrigin;
    _ = childOctant;
    _ = octantOrigin;
    _ = octantMaskLo;
    _ = octantMaskHi;
    _ = boxHit;
    _ = projectedThreshold;
    _ = cullSkip;
    _ = cullExpand;
    _ = childBase;
    _ = isTerminal;
    _ = boundsChildCount;
    _ = boundsQLo;
    _ = boundsQHi;
    _ = boundsMin8;
    _ = boundsMax8;
    _ = boundsMin16;
}
