// ============================================================================
// Virtual Geometry — software rasterizer (virtualgeometry-software-raster-cs)
//
// Rasterizes the clusters the culler routed to software - small triangles,
// where a hardware draw pays a whole pixel quad per triangle - into the same
// visibility key the hardware path produces: reverse-Z depth bits in one
// word, packVisibilityPayload(globalCluster, triangle) in the other.
//
// WGSL on this stack has no 64-bit atomics, so nearest-wins is two passes:
//   rasterizeDepth   atomicMax(depthBits) per covered pixel;
//   rasterizePayload atomicMax(payload) on the pixels whose depth equals the
//                    winning depth.
// Each pass is one workgroup per routed cluster and one thread per triangle.
// The vertex transform, viewport mapping, edge functions and fill rule are
// the shared ones in virtualgeometry-visibility.wgsl, so a pixel's key can be
// resolved back to the triangle that wrote it.
//
// Rigid geometry only: clusters of skinned instances and clusters crossing
// the near plane stay on the hardware path (the culler routes them there).
// ============================================================================

#include "virtualgeometrydata.wgsl"
#include "virtualgeometry-visibility.wgsl"

struct SoftwareRasterUniforms {
    viewport  : vec2<u32>,
    _padding0 : u32,
    _padding1 : u32,
}

@group(0) @binding(0)  var<uniform>            uniforms                   : CullingUniforms;
@group(0) @binding(1)  var<uniform>            raster                     : SoftwareRasterUniforms;
@group(0) @binding(2)  var<storage, read>       instances                  : array<InstanceData>;
@group(0) @binding(3)  var<storage, read>       pageTable                  : array<PageTableEntry>;
@group(0) @binding(4)  var<storage, read>       swVisibleClusterInfos      : array<VisibleClusterInfo>;
@group(0) @binding(5)  var<storage, read>       cullingCounters            : array<u32>;
@group(0) @binding(6)  var<storage, read>       decodedClusterGeometryInfos: array<DecodedClusterGeometryInfo>;
@group(0) @binding(7)  var<storage, read>       decodedVertexBuffer        : array<DecodedVertexData>;
@group(0) @binding(8)  var<storage, read>       decodedIndexBuffer         : array<u32>;
@group(0) @binding(9)  var<storage, read_write> visibilityDepth            : array<atomic<u32>>;
@group(0) @binding(10) var<storage, read_write> visibilityPayload          : array<atomic<u32>>;

const RASTER_WORKGROUP_SIZE : u32 = 128u;
const CLEAR_WORKGROUP_SIZE  : u32 = 256u;

@group(0) @binding(13) var<storage, read> pagesBuffer : array<u32>;
#include "virtualgeometry-geometry-fetch.wgsl"

@compute @workgroup_size(CLEAR_WORKGROUP_SIZE)
fn clearVisibility(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let pixelCount = raster.viewport.x * raster.viewport.y;
    let index = globalId.x;
    if (index >= pixelCount) {
        return;
    }
    atomicStore(&visibilityDepth[index], 0u);
    atomicStore(&visibilityPayload[index], 0u);
}

struct ScreenTriangle {
    valid   : bool,
    p0      : vec2<f32>,
    p1      : vec2<f32>,
    p2      : vec2<f32>,
    z0      : f32,
    z1      : f32,
    z2      : f32,
    payload : u32,
}

fn invalidTriangle() -> ScreenTriangle {
    return ScreenTriangle(false, vec2<f32>(0.0), vec2<f32>(0.0), vec2<f32>(0.0), 0.0, 0.0, 0.0, 0u);
}

fn applyClipSpaceConvention(clipPos : vec4<f32>) -> vec4<f32> {
#ifdef USE_REVERSE_Z
    return vec4<f32>(clipPos.x, -clipPos.y, clipPos.z, clipPos.w);
#else
    return clipPos;
#endif
}

fn setupTriangle(clusterSlot : u32, triangle : u32) -> ScreenTriangle {
    let clusterInfo = swVisibleClusterInfos[clusterSlot];
    let entry = pageTable[clusterInfo.pageIndex];
    let globalCluster = globalClusterIndex(entry, clusterInfo.pageLocalClusterIndex);
    let geometry = decodedClusterGeometryInfos[globalCluster];
    if (triangle >= geometry.triangleCount) {
        return invalidTriangle();
    }
    let instance = instances[clusterInfo.instanceIndex];
    let viewProj = uniforms.proj * uniforms.view;
    let viewport = vec2<f32>(f32(raster.viewport.x), f32(raster.viewport.y));

    var screen : array<vec3<f32>, 3>;
    for (var corner = 0u; corner < 3u; corner++) {
        let localIndex = vgLoadIndex(geometry, triangle * 3u + corner);
        if (localIndex >= geometry.vertexCount) {
            return invalidTriangle();
        }
        let vertex = vgLoadVertex(geometry, localIndex);
        let localPosition = vec3<f32>(vertex.positionX, vertex.positionY, vertex.positionZ);
        let worldPosition = (instance.modelMatrix * vec4<f32>(localPosition, 1.0)).xyz;
        let clip = applyClipSpaceConvention(viewProj * vec4<f32>(worldPosition, 1.0));
        // A vertex behind the eye: the culler keeps such clusters on the
        // hardware path, which clips them; here there is nothing sensible to
        // draw.
        if (clip.w <= 1.0e-6) {
            return invalidTriangle();
        }
        screen[corner] = vec3<f32>(
            (clip.x / clip.w * 0.5 + 0.5) * viewport.x,
#ifdef USE_REVERSE_Z
            (clip.y / clip.w * 0.5 + 0.5) * viewport.y,
#else
            (clip.y / clip.w * 0.5 + 0.5) * viewport.y,
#endif
            clip.z / clip.w);
    }
    return ScreenTriangle(
        true,
        screen[0].xy, screen[1].xy, screen[2].xy,
        screen[0].z, screen[1].z, screen[2].z,
        packVisibilityPayload(globalCluster, triangle));
}

// Walks the triangle's pixels; writes depth in the depth pass and, in the
// payload pass, the payload wherever this triangle's depth is the winner.
fn rasterize(tri : ScreenTriangle, payloadPass : bool) {
    var p0 = tri.p0;
    var p1 = tri.p1;
    var p2 = tri.p2;
    var z0 = tri.z0;
    var z1 = tri.z1;
    var z2 = tri.z2;
    var area = visibilityEdge(p0, p1, p2);
    if (area == 0.0) {
        return;
    }
    // No face culling (the hardware pipeline draws both faces too): make
    // every triangle positive-area so one fill rule serves both windings.
    if (area < 0.0) {
        let tp = p1; p1 = p2; p2 = tp;
        let tz = z1; z1 = z2; z2 = tz;
        area = -area;
    }
    let viewportMax = vec2<f32>(f32(raster.viewport.x), f32(raster.viewport.y));
    let boxMin = max(floor(min(min(p0, p1), p2)), vec2<f32>(0.0));
    let boxMax = min(ceil(max(max(p0, p1), p2)), viewportMax);
    if (any(boxMax <= boxMin)) {
        return;
    }
    let topLeft0 = visibilityEdgeIsTopLeft(p0, p1);
    let topLeft1 = visibilityEdgeIsTopLeft(p1, p2);
    let topLeft2 = visibilityEdgeIsTopLeft(p2, p0);
    let invArea = 1.0 / area;

    // The walk covers the whole bounding box, clipped to the viewport. A
    // per-triangle size cap was tried and truncated the large flat quads a
    // small-average cluster can still contain, which read as the surface
    // behind them showing through; the routing threshold, not a truncation,
    // is what keeps software triangles small.
    let xStart = u32(boxMin.x);
    let xEnd = u32(boxMax.x);
    let yStart = u32(boxMin.y);
    let yEnd = u32(boxMax.y);
    for (var y = yStart; y < yEnd; y++) {
        for (var x = xStart; x < xEnd; x++) {
            let p = vec2<f32>(f32(x) + 0.5, f32(y) + 0.5);
            // w0 is the weight of p0: the edge opposite it.
            let w0 = visibilityEdge(p1, p2, p);
            let w1 = visibilityEdge(p2, p0, p);
            let w2 = visibilityEdge(p0, p1, p);
            let inside =
                (w0 > 0.0 || (w0 == 0.0 && topLeft1)) &&
                (w1 > 0.0 || (w1 == 0.0 && topLeft2)) &&
                (w2 > 0.0 || (w2 == 0.0 && topLeft0));
            if (!inside) {
                continue;
            }
            // Device depth is affine in screen space: no perspective term.
            let z = (w0 * z0 + w1 * z1 + w2 * z2) * invArea;
            if (z < 0.0 || z > 1.0) {
                continue;
            }
            let depthBits = visibilityDepthBits(z);
            let pixel = y * raster.viewport.x + x;
            if (payloadPass) {
                if (atomicLoad(&visibilityDepth[pixel]) == depthBits) {
                    atomicMax(&visibilityPayload[pixel], tri.payload);
                }
            } else {
                atomicMax(&visibilityDepth[pixel], depthBits);
            }
        }
    }
}

@compute @workgroup_size(RASTER_WORKGROUP_SIZE)
fn rasterizeDepth(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>
) {
    let clusterSlot = workgroupId.x;
    if (clusterSlot >= cullingCounters[SW_VISIBLE_CLUSTER_COUNT_INDEX]) {
        return;
    }
    let tri = setupTriangle(clusterSlot, localId.x);
    if (!tri.valid) {
        return;
    }
    rasterize(tri, false);
}

@compute @workgroup_size(RASTER_WORKGROUP_SIZE)
fn rasterizePayload(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localId : vec3<u32>
) {
    let clusterSlot = workgroupId.x;
    if (clusterSlot >= cullingCounters[SW_VISIBLE_CLUSTER_COUNT_INDEX]) {
        return;
    }
    let tri = setupTriangle(clusterSlot, localId.x);
    if (!tri.valid) {
        return;
    }
    rasterize(tri, true);
}
