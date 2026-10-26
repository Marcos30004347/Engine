fn sparseVoxelTransformPoint(matrix : mat4x4<f32>, point : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(point, 1.0)).xyz;
}

// Conservative five-plane test in the voxel raster camera convention (+Z
// forward, pixel Y down). An AABB intersecting the near plane remains visible.
fn sparseVoxelFrustumVisibleWorldAABB(
    worldMinimum : vec3<f32>,
    worldMaximum : vec3<f32>,
    raster : SparseVoxelRasterUniforms
) -> bool {
    var allOutsideLeft = true;
    var allOutsideRight = true;
    var allOutsideTop = true;
    var allOutsideBottom = true;
    var allOutsideNear = true;
    let width = f32(raster.viewportTiles.x);
    let height = f32(raster.viewportTiles.y);
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let world = select(worldMinimum, worldMaximum, vec3<bool>((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
        let view = sparseVoxelTransformPoint(raster.worldToCamera, world);
        let left = raster.focalPrincipal.x * view.x + raster.focalPrincipal.z * view.z;
        let right = raster.focalPrincipal.x * view.x + (raster.focalPrincipal.z - width) * view.z;
        let top = raster.focalPrincipal.y * view.y + raster.focalPrincipal.w * view.z;
        let bottom = raster.focalPrincipal.y * view.y + (raster.focalPrincipal.w - height) * view.z;
        if (left >= 0.0) { allOutsideLeft = false; }
        if (right <= 0.0) { allOutsideRight = false; }
        if (top >= 0.0) { allOutsideTop = false; }
        if (bottom <= 0.0) { allOutsideBottom = false; }
        if (view.z >= raster.cameraWorldPositionNear.w) { allOutsideNear = false; }
    }
    return !(allOutsideLeft || allOutsideRight || allOutsideTop || allOutsideBottom || allOutsideNear);
}

fn sparseVoxelFrustumVisibleLocalAABB(
    localMinimum : vec3<f32>,
    localMaximum : vec3<f32>,
    modelToWorld : mat4x4<f32>,
    raster : SparseVoxelRasterUniforms
) -> bool {
    var worldMinimum = vec3<f32>(1.0e30);
    var worldMaximum = vec3<f32>(-1.0e30);
    for (var corner = 0u; corner < 8u; corner += 1u) {
        let local = select(localMinimum, localMaximum, vec3<bool>((corner & 1u) != 0u, (corner & 2u) != 0u, (corner & 4u) != 0u));
        let world = sparseVoxelTransformPoint(modelToWorld, local);
        worldMinimum = min(worldMinimum, world);
        worldMaximum = max(worldMaximum, world);
    }
    return sparseVoxelFrustumVisibleWorldAABB(worldMinimum, worldMaximum, raster);
}

#include "sparse_voxel_hzb_common.wgsl"
