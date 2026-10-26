// ============================================================================
// Virtual Geometry — visibility resolve (virtualgeometry-visibility-resolve)
//
// One fullscreen pass that turns the merged visibility key of every pixel
// into the G-buffer the rest of the frame consumes: depth, geometry ids,
// material id and UV, geometry normal and motion vectors. The key names a
// triangle; its attributes are reconstructed here from the decoded cluster
// data with the same transform and edge functions the rasterizers used, so a
// pixel written by the hardware draw and one written by the software
// rasterizer come out of this pass identically.
//
// Rigid geometry: skinning is not applied (the software path does not route
// skinned clusters, and the terrain has none).
// ============================================================================

#include "virtualgeometrydata.wgsl"
#include "virtualgeometry-visibility.wgsl"

struct VisibilityResolveUniforms {
    viewport : vec2<u32>,
    _padding0 : u32,
    _padding1 : u32,
}

@group(0) @binding(0) var<uniform>       uniforms                    : CullingUniforms;
@group(0) @binding(1) var<uniform>       temporalUniforms            : TemporalUniforms;
@group(0) @binding(2) var<uniform>       resolve                     : VisibilityResolveUniforms;
@group(0) @binding(3) var<storage, read> instances                   : array<InstanceData>;
@group(0) @binding(4) var<storage, read> instanceSparseToDense       : array<u32>;
@group(0) @binding(5) var<storage, read> decodedClusterGeometryInfos : array<DecodedClusterGeometryInfo>;
@group(0) @binding(6) var<storage, read> decodedVertexBuffer         : array<DecodedVertexData>;
@group(0) @binding(7) var<storage, read> decodedIndexBuffer          : array<u32>;
@group(0) @binding(8) var<storage, read> visibilityKeys              : array<VisibilityKey>;

@group(0) @binding(9) var<storage, read> pagesBuffer : array<u32>;
#include "virtualgeometry-geometry-fetch.wgsl"

struct ResolveOutput {
    @location(0) packedGeometryIdsLo : u32,
    @location(1) packedGeometryIdsHi : u32,
    @location(2) materialId : u32,
    @location(3) materialUV : vec2<f32>,
    @location(4) geometryNormal : vec4<f32>,
    @location(5) motionVectors : vec2<f32>,
    @builtin(frag_depth) depth : f32,
}

fn applyClipSpaceConvention(clipPos : vec4<f32>) -> vec4<f32> {
#ifdef USE_REVERSE_Z
    return vec4<f32>(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
#else
    return clipPos;
#endif
}

fn clipToUv(clipPos : vec4<f32>) -> vec2<f32> {
    let ndc = clipPos.xy / clipPos.w;
    return vec2<f32>(ndc.x * 0.5 + 0.5, ndc.y * 0.5 + 0.5);
}

@vertex
fn resolveVertex(@builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, -1.0),
        vec2<f32>(1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 1.0));
    return vec4<f32>(corners[vertexIndex], 0.0, 1.0);
}

fn emptyOutput() -> ResolveOutput {
    var out : ResolveOutput;
    out.packedGeometryIdsLo = 0u;
    out.packedGeometryIdsHi = 0u;
    out.materialId = 0u;
    out.materialUV = vec2<f32>(0.0);
    out.geometryNormal = vec4<f32>(0.0);
    out.motionVectors = vec2<f32>(0.0);
    out.depth = 0.0;
    return out;
}

@fragment
fn resolveFragment(@builtin(position) fragmentPosition : vec4<f32>) -> ResolveOutput {
    let pixel = vec2<u32>(fragmentPosition.xy);
    if (any(pixel >= resolve.viewport)) {
        return emptyOutput();
    }
    let index = pixel.y * resolve.viewport.x + pixel.x;
    let key = visibilityKeys[index];
    if (visibilityKeyIsEmpty(key)) {
        return emptyOutput();
    }

    let globalCluster = visibilityPayloadCluster(key.payload);
    let triangle = visibilityPayloadTriangle(key.payload);
    let geometry = decodedClusterGeometryInfos[globalCluster];
    if (triangle >= geometry.triangleCount) {
        return emptyOutput();
    }
    let denseInstance = instanceSparseToDense[geometry._padding1];
    let instance = instances[denseInstance];
    let viewProj = uniforms.proj * uniforms.view;
    let viewport = vec2<f32>(f32(resolve.viewport.x), f32(resolve.viewport.y));

    var screen : array<vec2<f32>, 3>;
    var invW : array<f32, 3>;
    var localPosition : array<vec3<f32>, 3>;
    var localNormal : array<vec3<f32>, 3>;
    var uv : array<vec2<f32>, 3>;
    for (var corner = 0u; corner < 3u; corner++) {
        let localIndex = vgLoadIndex(geometry, triangle * 3u + corner);
        let vertex = vgLoadVertex(geometry, min(localIndex, geometry.vertexCount - 1u));
        localPosition[corner] = vec3<f32>(vertex.positionX, vertex.positionY, vertex.positionZ);
        localNormal[corner] = vec3<f32>(vertex.normalX, vertex.normalY, vertex.normalZ);
        uv[corner] = vec2<f32>(vertex.uvX, vertex.uvY);
        let worldPosition = (instance.modelMatrix * vec4<f32>(localPosition[corner], 1.0)).xyz;
        let clip = applyClipSpaceConvention(viewProj * vec4<f32>(worldPosition, 1.0));
        let w = max(clip.w, 1.0e-6);
        screen[corner] = vec2<f32>(
            (clip.x / w * 0.5 + 0.5) * viewport.x,
            (clip.y / w * 0.5 + 0.5) * viewport.y);
        invW[corner] = 1.0 / w;
    }

    // Screen-space barycentrics at the pixel centre, then perspective
    // correction through 1/w: the same interpolation the hardware applied.
    let p = vec2<f32>(f32(pixel.x) + 0.5, f32(pixel.y) + 0.5);
    let area = visibilityEdge(screen[0], screen[1], screen[2]);
    var b = vec3<f32>(1.0 / 3.0);
    if (abs(area) > 1.0e-12) {
        let w0 = visibilityEdge(screen[1], screen[2], p) / area;
        let w1 = visibilityEdge(screen[2], screen[0], p) / area;
        let w2 = 1.0 - w0 - w1;
        let persp = vec3<f32>(w0 * invW[0], w1 * invW[1], w2 * invW[2]);
        let total = persp.x + persp.y + persp.z;
        if (abs(total) > 1.0e-20) {
            b = persp / total;
        }
    }

    let localPos = localPosition[0] * b.x + localPosition[1] * b.y + localPosition[2] * b.z;
    let localNorm = localNormal[0] * b.x + localNormal[1] * b.y + localNormal[2] * b.z;
    let worldPos = (instance.modelMatrix * vec4<f32>(localPos, 1.0)).xyz;
    let worldNormal = (instance.modelMatrix * vec4<f32>(localNorm, 0.0)).xyz;
    let worldNormalLength = length(worldNormal);
    let normal = select(vec3<f32>(0.0, 0.0, 1.0), worldNormal / worldNormalLength, worldNormalLength > 1e-6);

    var out : ResolveOutput;
    out.packedGeometryIdsLo = denseInstance;
    out.packedGeometryIdsHi = key.payload;
    out.materialId = instance.materialIndex;
    out.materialUV = uv[0] * b.x + uv[1] * b.y + uv[2] * b.z;
    out.geometryNormal = vec4<f32>(normal, 1.0);
    let currentClip = applyClipSpaceConvention(viewProj * vec4<f32>(worldPos, 1.0));
    let previousWorldPos = (instance.previousModelMatrix * vec4<f32>(localPos, 1.0)).xyz;
    let previousClip = applyClipSpaceConvention(
        (temporalUniforms.prevProj * temporalUniforms.prevView) * vec4<f32>(previousWorldPos, 1.0));
    if (temporalUniforms.hasPreviousFrame == 0u || abs(previousClip.w) <= 1e-6 || abs(currentClip.w) <= 1e-6) {
        out.motionVectors = vec2<f32>(0.0, 0.0);
    } else {
        out.motionVectors = clipToUv(currentClip) - clipToUv(previousClip);
    }
    out.depth = visibilityDepthFromBits(key.depthBits);
    return out;
}
