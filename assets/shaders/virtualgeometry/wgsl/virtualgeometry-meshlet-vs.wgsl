#include "virtualgeometrydata.wgsl"

// Entry packing sync marker: keep this file touched when
// virtualgeometrydata.wgsl changes because shader include dependency tracking
// is not reliable in the current build.

@group(0) @binding(0) var<uniform>           uniforms           : CullingUniforms;
@group(0) @binding(1) var<storage, read>      instances          : array<InstanceData>;
@group(0) @binding(2) var<storage, read>      pageTable          : array<PageTableEntry>;
@group(0) @binding(3) var<storage, read>      pagesBuffer        : array<u32>;
@group(0) @binding(4) var<storage, read>      visibleClusterInfos: array<VisibleClusterInfo>;
@group(0) @binding(5) var<storage, read>      cullingCounters    : array<u32>;
@group(0) @binding(6) var<storage, read>      meshPartTransforms : array<mat4x4<f32>>;
#ifdef OUTPUT_MOTION_VECTORS
@group(0) @binding(7) var<uniform>            temporalUniforms   : TemporalUniforms;
#endif
@group(0) @binding(9) var<storage, read>      decodedClusterGeometryInfos : array<DecodedClusterGeometryInfo>;
@group(0) @binding(10) var<storage, read>     decodedVertexBuffer : array<DecodedVertexData>;
@group(0) @binding(11) var<storage, read>     decodedIndexBuffer : array<u32>;
@group(0) @binding(12) var<storage, read>     decodedBoneWeightBuffer : array<DecodedBoneWeightData>;

#include "virtualgeometry-geometry-fetch.wgsl"

struct VertexOutput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) @interpolate(flat) instanceIndex : u32,
    @location(1) @interpolate(flat) globalCluster : u32,
    @location(2) @interpolate(flat) triangleId    : u32,
    @location(3) @interpolate(flat) materialIndex : u32,
    @location(4) uv : vec2<f32>,
    @location(5) worldNormal : vec3<f32>,
#ifdef OUTPUT_MOTION_VECTORS
    @location(6) currentClip : vec4<f32>,
    @location(7) previousClip : vec4<f32>,
#endif
}

fn applyClipSpaceConvention(clipPos: vec4<f32>) -> vec4<f32> {
#ifdef USE_REVERSE_Z
    return vec4<f32>(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
#else
    return clipPos;
#endif
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
    let visibleClusterCount = cullingCounters[HW_VISIBLE_CLUSTER_COUNT_INDEX];
    if (clusterSlot >= visibleClusterCount) {
        out.clipPos = vec4<f32>(0.0, 0.0, -1.0, 1.0);
#ifdef OUTPUT_MOTION_VECTORS
        out.currentClip = out.clipPos;
        out.previousClip = out.clipPos;
#endif
        return out;
    }
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

    let entry    = pageTable[pageIdx];
    let globalCluster = globalClusterIndex(entry, localMeshletIdx);
    let clusterGeometry = decodedClusterGeometryInfos[globalCluster];
    let vtxCount     = clusterGeometry.vertexCount;
    let triCount     = clusterGeometry.triangleCount;

    // ── Triangle / vertex decode ──────────────────────────────────────────────
    let maxCorners  = triCount * 3u;
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    // localCorner may exceed maxCorners for the degenerate padding region.
    // Clamp the index read so decodeIndex stays in-bounds, then discard.
    let cornerIdx   = min(localCorner, maxCorners - 1u);
    let localVertID = vgLoadIndex(clusterGeometry, cornerIdx);

    if (localCorner >= maxCorners || localVertID >= vtxCount) {
        out.clipPos = vec4<f32>(0.0, 0.0, -1.0, 1.0);
#ifdef OUTPUT_MOTION_VECTORS
        out.currentClip = out.clipPos;
        out.previousClip = out.clipPos;
#endif
        return out;
    }
#else
    let cornerIdx   = min(vertexIndex, maxCorners - 1u);
    let localVertID = vgLoadIndex(clusterGeometry, cornerIdx);

    if (vertexIndex >= maxCorners || localVertID >= vtxCount) {
        out.clipPos = vec4<f32>(0.0, 0.0, -1.0, 1.0);
#ifdef OUTPUT_MOTION_VECTORS
        out.currentClip = out.clipPos;
        out.previousClip = out.clipPos;
#endif
        return out;
    }
#endif

    let vertexData = vgLoadVertex(clusterGeometry, localVertID);
    let bindPoseLocalPos = vec3<f32>(vertexData.positionX, vertexData.positionY, vertexData.positionZ);
    var localPos = bindPoseLocalPos;
    var localNorm = vec3<f32>(vertexData.normalX, vertexData.normalY, vertexData.normalZ);
    let uv = vec2<f32>(vertexData.uvX, vertexData.uvY);
#ifdef OUTPUT_MOTION_VECTORS
    var previousLocalPos = bindPoseLocalPos;
#endif
    let boneWeightsPerVertex = vertexData.boneWeightCount;
    if (boneWeightsPerVertex > 0u) {
        let influenceCount = min(boneWeightsPerVertex, MAX_BONE_WEIGHTS_PER_VERTEX);
        var skinnedPos = vec3<f32>(0.0);
        var skinnedNorm = vec3<f32>(0.0);
#ifdef OUTPUT_MOTION_VECTORS
        var previousSkinnedPos = vec3<f32>(0.0);
#endif
        for (var influenceIndex = 0u; influenceIndex < influenceCount; influenceIndex++) {
            var bw: DecodedBoneWeightData;
            if (clusterGeometry._padding0 != 0u) {
                let at = vertexData.boneWeightOffset + influenceIndex*2u;
                bw.weight = bitcast<f32>(pagesBuffer[at]);
                bw.boneIndex = pagesBuffer[at+1u];
            } else { bw = decodedBoneWeightBuffer[vertexData.boneWeightOffset + influenceIndex]; }
            let boneMatrix = meshPartTransforms[instance.meshPartTransformsOffset + bw.boneIndex];
            skinnedPos += bw.weight * (boneMatrix * vec4<f32>(bindPoseLocalPos, 1.0)).xyz;
            skinnedNorm += bw.weight * (boneMatrix * vec4<f32>(localNorm, 0.0)).xyz;
#ifdef OUTPUT_MOTION_VECTORS
            let previousBoneMatrix = meshPartTransforms[instance.previousMeshPartTransformsOffset + bw.boneIndex];
            previousSkinnedPos += bw.weight * (previousBoneMatrix * vec4<f32>(bindPoseLocalPos, 1.0)).xyz;
#endif
        }
        localPos = skinnedPos;
        localNorm = normalize(skinnedNorm);
#ifdef OUTPUT_MOTION_VECTORS
        previousLocalPos = previousSkinnedPos;
#endif
    }

    let worldPos  = (instance.modelMatrix * vec4<f32>(localPos,  1.0)).xyz;
    let worldNormal = (instance.modelMatrix * vec4<f32>(localNorm, 0.0)).xyz;
    let clipPos  = applyClipSpaceConvention((uniforms.proj * uniforms.view) * vec4<f32>(worldPos, 1.0));
    out.clipPos = clipPos;
    let worldNormalLength = length(worldNormal);
    out.worldNormal = select(vec3<f32>(0.0, 0.0, 1.0), worldNormal / worldNormalLength, worldNormalLength > 1e-6);
#ifdef OUTPUT_MOTION_VECTORS
    let previousWorldPos = (instance.previousModelMatrix * vec4<f32>(previousLocalPos, 1.0)).xyz;
    out.currentClip = clipPos;
    out.previousClip = applyClipSpaceConvention((temporalUniforms.prevProj * temporalUniforms.prevView) * vec4<f32>(previousWorldPos, 1.0));
#endif

    out.instanceIndex = instIdx;
    out.globalCluster = globalCluster;
    out.triangleId    = cornerIdx / 3u;
    out.materialIndex = instance.materialIndex;
    out.uv            = uv;

    return out;
}
