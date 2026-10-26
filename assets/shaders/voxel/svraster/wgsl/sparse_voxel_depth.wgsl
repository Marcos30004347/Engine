#include "sparse_voxel_types.wgsl"
#include "sparse_voxel_projection.wgsl"

@group(0) @binding(0) var<uniform> uniforms : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<storage, read> instances : array<SparseVoxelInstance>;
@group(0) @binding(2) var<storage, read> resolvedSurfaces : array<ResolvedVoxelSurface>;

fn depthCameraRayDirection(pixel : vec2<u32>) -> vec3<f32> {
    let cameraDirection = vec3<f32>(
        (f32(pixel.x) + 0.5 - uniforms.focalPrincipal.z) / uniforms.focalPrincipal.x,
        (f32(pixel.y) + 0.5 - uniforms.focalPrincipal.w) / uniforms.focalPrincipal.y,
        1.0);
    return normalize(transformVector(uniforms.cameraToWorld, cameraDirection));
}

@vertex
fn writeVoxelDepthVertex(@builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    let corners = array<vec2<f32>, 6>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, -1.0),
        vec2<f32>(1.0, -1.0), vec2<f32>(-1.0, 1.0), vec2<f32>(1.0, 1.0));
    return vec4<f32>(corners[vertexIndex], 0.0, 1.0);
}

@fragment
fn writeVoxelDepth(@builtin(position) fragmentPosition : vec4<f32>) -> @builtin(frag_depth) f32 {
    let pixel = vec2<u32>(fragmentPosition.xy);
    let pixelIndex = pixel.y * uniforms.viewportTiles.x + pixel.x;
    let surface = resolvedSurfaces[pixelIndex];
    if ((surface.hitFlags & SPARSE_VOXEL_SURFACE_HIT) == 0u) {
        discard;
    }

    let worldOrigin = uniforms.cameraWorldPositionNear.xyz;
    let worldPosition = worldOrigin + depthCameraRayDirection(pixel) * surface.surfaceT;
    let currentClip = uniforms.worldToClip * vec4<f32>(worldPosition, 1.0);
    return currentClip.z / currentClip.w;
}
