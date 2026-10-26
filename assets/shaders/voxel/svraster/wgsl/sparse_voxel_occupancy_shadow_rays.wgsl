#include "sparse_voxel_types.wgsl"

struct SparseVoxelOccupancyDDARay {
    originMaximumDistance : vec4<f32>,
    direction : vec4<f32>,
}

struct SparseVoxelOccupancyShadowSettings {
    lightDirectionMaximumDistance : vec4<f32>,
    dimensionsEnabled : vec4<u32>,
    biasReserved : vec4<f32>,
}

@group(0) @binding(0) var<uniform> raster : SparseVoxelRasterUniforms;
@group(0) @binding(1) var<uniform> shadow : SparseVoxelOccupancyShadowSettings;
@group(0) @binding(2) var<storage, read> coverage : array<u32>;
@group(0) @binding(3) var<storage, read> linearDepth : array<f32>;
@group(0) @binding(4) var<storage, read_write> rays : array<SparseVoxelOccupancyDDARay>;

const OCCUPANCY_RAY_DISABLED : u32 = 0u;
const OCCUPANCY_RAY_SURFACE_TO_LIGHT : u32 = 1u;
const OCCUPANCY_RAY_CAMERA_PRIMARY : u32 = 2u;

fn shadowTransformVector(
    matrix : mat4x4<f32>,
    value : vec3<f32>) -> vec3<f32> {
    return (matrix * vec4<f32>(value, 0.0)).xyz;
}

@compute @workgroup_size(8, 8, 1)
fn generateOccupancyShadowRays(
    @builtin(global_invocation_id) globalId : vec3<u32>) {
    let outputPixel = globalId.xy;
    let outputSize = shadow.dimensionsEnabled.xy;
    if (any(outputPixel >= outputSize)) {
        return;
    }
    let outputIndex = outputPixel.y * outputSize.x + outputPixel.x;
    let sourcePixel = min(
        outputPixel * 2u + vec2<u32>(1u),
        raster.viewportTiles.xy - vec2<u32>(1u));
    let sourceIndex =
        sourcePixel.y * shadow.dimensionsEnabled.z + sourcePixel.x;
    let rayMode = shadow.dimensionsEnabled.w;
    if (rayMode == OCCUPANCY_RAY_DISABLED ||
        (rayMode == OCCUPANCY_RAY_SURFACE_TO_LIGHT &&
         coverage[sourceIndex] == 0u)) {
        rays[outputIndex] = SparseVoxelOccupancyDDARay(
            vec4<f32>(0.0, 0.0, 0.0, -1.0),
            vec4<f32>(0.0));
        return;
    }
    let cameraDirection = vec3<f32>(
        (f32(sourcePixel.x) + 0.5 - raster.focalPrincipal.z) /
            raster.focalPrincipal.x,
        (f32(sourcePixel.y) + 0.5 - raster.focalPrincipal.w) /
            raster.focalPrincipal.y,
        1.0);
    let worldDirection = normalize(
        shadowTransformVector(raster.cameraToWorld, cameraDirection));
    if (rayMode == OCCUPANCY_RAY_CAMERA_PRIMARY) {
        rays[outputIndex] = SparseVoxelOccupancyDDARay(
            vec4<f32>(
                raster.cameraWorldPositionNear.xyz,
                shadow.lightDirectionMaximumDistance.w),
            vec4<f32>(worldDirection, 0.0));
        return;
    }
    let surfacePosition =
        raster.cameraWorldPositionNear.xyz +
        worldDirection * linearDepth[sourceIndex];
    let lightDirection =
        normalize(shadow.lightDirectionMaximumDistance.xyz);
    let origin =
        surfacePosition + lightDirection * shadow.biasReserved.x;
    rays[outputIndex] = SparseVoxelOccupancyDDARay(
        vec4<f32>(
            origin,
            shadow.lightDirectionMaximumDistance.w),
        vec4<f32>(lightDirection, 0.0));
}
