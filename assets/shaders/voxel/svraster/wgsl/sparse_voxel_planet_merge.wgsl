struct MergeUniforms {
    viewport : vec4<u32>,
    transition : vec4<f32>,
    cameraToWorld : mat4x4<f32>,
    focalPrincipal : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : MergeUniforms;
@group(0) @binding(1) var voxelShadedColor : texture_2d<f32>;
@group(0) @binding(2) var voxelGeometryNormal : texture_2d<f32>;
@group(0) @binding(3) var voxelMotion : texture_2d<f32>;
@group(0) @binding(4) var voxelInstanceId : texture_2d<u32>;
@group(0) @binding(5) var voxelTemporalId : texture_2d<u32>;
@group(0) @binding(6) var voxelDepth : texture_depth_2d;
@group(0) @binding(7) var planetColor : texture_2d<f32>;
@group(0) @binding(8) var planetNormalDepth : texture_2d<f32>;
@group(0) @binding(9) var planetDepth : texture_depth_2d;
@group(0) @binding(10) var<storage, read_write> coverage : array<u32>;
@group(0) @binding(11) var<storage, read_write> linearDepth : array<f32>;

@vertex
fn drawMergeVertex(
    @builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    let x = f32((vertexIndex << 1u) & 2u);
    let y = f32(vertexIndex & 2u);
    return vec4<f32>(x * 2.0 - 1.0, y * 2.0 - 1.0, 0.0, 1.0);
}

struct MergeOutput {
    @location(0) color : vec4<f32>,
    @location(1) geometryNormal : vec4<f32>,
    @location(2) motion : vec2<f32>,
    @location(3) instanceId : u32,
    @location(4) temporalId : u32,
    @builtin(frag_depth) depth : f32,
}

fn safe_normal(value : vec3<f32>) -> vec3<f32> {
    let squareLength = dot(value, value);
    if (squareLength <= 1.0e-8) {
        return vec3<f32>(0.0, 1.0, 0.0);
    }
    return value * inverseSqrt(squareLength);
}

@fragment
fn drawMergeFragment(@builtin(position) position : vec4<f32>) -> MergeOutput {
    let pixel = vec2<i32>(position.xy);
    let pixelU = vec2<u32>(position.xy);
    let pixelIndex = pixelU.y * uniforms.viewport.x + pixelU.x;
    let sourceColor = textureLoad(voxelShadedColor, pixel, 0);
    let hasVoxel = coverage[pixelIndex] != 0u;
    let planet = textureLoad(planetColor, pixel, 0);
    let hasPlanet = planet.a > 0.5 && uniforms.transition.z > 0.5;

    // Keep empty pixels as the already-composed sky. Explicit branches avoid
    // multiplying cleared/undefined G-buffer values by zero; 0 * NaN is still
    // NaN and was the source of black speckles through the transition band.
    if (!hasVoxel && !hasPlanet) {
        return MergeOutput(
            sourceColor, vec4<f32>(0.0), vec2<f32>(0.0),
            0xffffffffu, 0xffffffffu, 0.0);
    }

    if (hasVoxel && !hasPlanet) {
        return MergeOutput(
            sourceColor,
            textureLoad(voxelGeometryNormal, pixel, 0),
            textureLoad(voxelMotion, pixel, 0).xy,
            textureLoad(voxelInstanceId, pixel, 0).x,
            textureLoad(voxelTemporalId, pixel, 0).x,
            textureLoad(voxelDepth, pixel, 0));
    }

    let planetSurface = textureLoad(planetNormalDepth, pixel, 0);
    let planetN = safe_normal(planetSurface.xyz);
    let planetDeviceDepth = textureLoad(planetDepth, pixel, 0);
    // Change the coarse representation's identity every frame. Its raster
    // colour is stable, but it has no voxel motion/trace identity; rejecting
    // temporal reuse is preferable to dragging black voxel history over it.
    let planetIdentity = 0xffff0000u | (uniforms.viewport.z & 0xffffu);

    if (!hasVoxel) {
        coverage[pixelIndex] = 1u;
        linearDepth[pixelIndex] = planetSurface.w;
        return MergeOutput(
            planet, vec4<f32>(planetN, 1.0), vec2<f32>(0.0),
            planetIdentity, planetIdentity, planetDeviceDepth);
    }

    let voxelDistance = linearDepth[pixelIndex];
    // The CPU transition range is the horizontal distance to the edge of the
    // toroidal terrain window. Comparing it with full ray distance made merely
    // gaining altitude fade out the voxel directly below the camera. Recover
    // the same primary ray used by brick traversal and measure only its XZ
    // travel, so altitude changes LOD projection but not representation
    // ownership.
    let focal = max(abs(uniforms.focalPrincipal.xy), vec2<f32>(1.0e-6));
    let focalSign = select(
        vec2<f32>(-1.0), vec2<f32>(1.0),
        uniforms.focalPrincipal.xy >= vec2<f32>(0.0));
    let signedFocal = focal * focalSign;
    let viewX =
        (position.x - uniforms.focalPrincipal.z) / signedFocal.x;
    let viewY =
        (position.y - uniforms.focalPrincipal.w) / signedFocal.y;
    let voxelRay = safe_normal(
        uniforms.cameraToWorld[0].xyz * viewX +
        uniforms.cameraToWorld[1].xyz * viewY +
        uniforms.cameraToWorld[2].xyz);
    let horizontalDistance =
        voxelDistance * length(voxelRay.xz);
    let voxelWeight = 1.0 - smoothstep(
        uniforms.transition.x, uniforms.transition.y, horizontalDistance);
    let voxelN = safe_normal(textureLoad(voxelGeometryNormal, pixel, 0).xyz);
    let voxelDeviceDepth = textureLoad(voxelDepth, pixel, 0);
    let useVoxelIdentity = voxelWeight >= 0.5;
    // Colour can cross-fade, but geometry cannot occupy a linear interpolation
    // between two independently tessellated heightfields. Use one coherent
    // surface for depth, normal and temporal identity. The old mixed depth
    // produced false contour lines wherever the coarse planet and voxel
    // heightfields differed by even a fraction of a cell.
    let mergedNormal = select(planetN, voxelN, useVoxelIdentity);
    let mergedLinearDepth = select(
        planetSurface.w, voxelDistance, useVoxelIdentity);
    let mergedDeviceDepth = select(
        planetDeviceDepth, voxelDeviceDepth, useVoxelIdentity);

    coverage[pixelIndex] = 1u;
    linearDepth[pixelIndex] = mergedLinearDepth;
    return MergeOutput(
        mix(planet, sourceColor, voxelWeight),
        vec4<f32>(mergedNormal, 1.0),
        select(vec2<f32>(0.0),
            textureLoad(voxelMotion, pixel, 0).xy, useVoxelIdentity),
        select(planetIdentity,
            textureLoad(voxelInstanceId, pixel, 0).x, useVoxelIdentity),
        select(planetIdentity,
            textureLoad(voxelTemporalId, pixel, 0).x, useVoxelIdentity),
        mergedDeviceDepth);
}
