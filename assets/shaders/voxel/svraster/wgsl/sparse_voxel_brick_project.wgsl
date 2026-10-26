// Brick box projection, shared by the culler and the R9 tile-binning census.
//
// Extracted so the two cannot drift: a binning pass that projects a brick even
// slightly differently from the culler would report a tile footprint the raster
// never actually produces, and the whole point of the census is to size the
// compute path's per-tile candidate lists.
//
// `worldToClip` is an explicit parameter rather than a read of a global
// uniform, because the includers bind different uniform structs.
#include "sparse_voxel_types.wgsl"

struct BrickBounds {
    minimum : vec3<f32>,
    maximum : vec3<f32>,
}

fn brickBounds(node : SparseVoxelBrickNode) -> BrickBounds {
    let minimum = node.minimumVoxelSize.xyz;
    let extent = node.minimumVoxelSize.w * f32(SPARSE_VOXEL_BRICK_SIDE);
    return BrickBounds(minimum, minimum + vec3<f32>(extent));
}

// Bounds of the cells the brick rasterizer can actually hit.
//
// A brick is allocated as an 8-cubed block, but surface bricks usually occupy
// only a slab or an edge of that block. The draw shader already uses this
// packed range to avoid rasterizing empty space. The culler must use the same
// box: projecting the allocation produces a larger screen rectangle and a
// nearer box depth, both of which can only turn a valid HZB rejection into a
// costly visible result. The range encloses every occupied voxel, while the
// existing one-voxel dilation still covers the conservative coarse-LOD
// representation, so this makes the test stricter without making it unsafe.
fn brickOccupiedBounds(node : SparseVoxelBrickNode) -> BrickBounds {
    let cells = brickOccupiedCellRange(node.packedOccupiedBounds);
    let minimum = node.minimumVoxelSize.xyz + cells.minimumCell * node.minimumVoxelSize.w;
    let maximum = node.minimumVoxelSize.xyz + cells.maximumCell * node.minimumVoxelSize.w;
    return BrickBounds(minimum, maximum);
}

// Camera-relative form shared by culling and compute-tile routing. Generated
// terrain nodes carry an exact integer lattice address; subtracting the split
// camera cell before converting to f32 preserves shared page boundaries at
// planetary coordinates. Authored and bootstrap nodes retain their ordinary
// world-space bounds.
fn brickBoundaryCameraRelative(
    node : SparseVoxelBrickNode, cell : vec3<i32>,
    canonicalWorldGrid : vec4<f32>, canonicalCameraCell : vec4<i32>,
    canonicalCameraOffset : vec4<f32>,
    cameraWorldPosition : vec4<f32>) -> vec3<f32> {
    let cameraMagnitude = max(
        max(abs(cameraWorldPosition.x), abs(cameraWorldPosition.y)),
        abs(cameraWorldPosition.z));
    if (node.regionMappingId != 0u && canonicalWorldGrid.w > 0.0 &&
        cameraMagnitude > canonicalWorldGrid.w * 65536.0) {
        let canonicalMinimum = canonicalWorldGrid.xyz +
            vec3<f32>(node.canonicalGridOriginScale.xyz) *
                canonicalWorldGrid.w;
        let coordinateTolerance = max(
            vec3<f32>(node.minimumVoxelSize.w * 0.25),
            abs(node.minimumVoxelSize.xyz) * 1.0e-6);
        if (all(abs(canonicalMinimum - node.minimumVoxelSize.xyz) <=
                coordinateTolerance)) {
            let canonicalCell = node.canonicalGridOriginScale.xyz +
                cell * node.canonicalGridOriginScale.w;
            return vec3<f32>(canonicalCell - canonicalCameraCell.xyz) *
                canonicalWorldGrid.w - canonicalCameraOffset.xyz;
        }
    }
    return node.minimumVoxelSize.xyz + vec3<f32>(cell) *
        node.minimumVoxelSize.w - cameraWorldPosition.xyz;
}

fn brickBoundsCameraRelative(
    node : SparseVoxelBrickNode, cells : SparseVoxelBrickCellRange,
    canonicalWorldGrid : vec4<f32>, canonicalCameraCell : vec4<i32>,
    canonicalCameraOffset : vec4<f32>,
    cameraWorldPosition : vec4<f32>) -> BrickBounds {
    return BrickBounds(
        brickBoundaryCameraRelative(
            node, vec3<i32>(cells.minimumCell), canonicalWorldGrid,
            canonicalCameraCell, canonicalCameraOffset, cameraWorldPosition),
        brickBoundaryCameraRelative(
            node, vec3<i32>(cells.maximumCell), canonicalWorldGrid,
            canonicalCameraCell, canonicalCameraOffset, cameraWorldPosition));
}

fn brickFullBoundsCameraRelative(
    node : SparseVoxelBrickNode, canonicalWorldGrid : vec4<f32>,
    canonicalCameraCell : vec4<i32>, canonicalCameraOffset : vec4<f32>,
    cameraWorldPosition : vec4<f32>) -> BrickBounds {
    return brickBoundsCameraRelative(
        node,
        SparseVoxelBrickCellRange(
            vec3<f32>(0.0), vec3<f32>(f32(SPARSE_VOXEL_BRICK_SIDE))),
        canonicalWorldGrid, canonicalCameraCell, canonicalCameraOffset,
        cameraWorldPosition);
}

fn brickOccupiedBoundsCameraRelative(
    node : SparseVoxelBrickNode, canonicalWorldGrid : vec4<f32>,
    canonicalCameraCell : vec4<i32>, canonicalCameraOffset : vec4<f32>,
    cameraWorldPosition : vec4<f32>) -> BrickBounds {
    return brickBoundsCameraRelative(
        node, brickOccupiedCellRange(node.packedOccupiedBounds),
        canonicalWorldGrid, canonicalCameraCell, canonicalCameraOffset,
        cameraWorldPosition);
}

// Clip-space projection of the box, reduced to what the frustum and occlusion
// tests need. Both used to walk the eight corners separately; this walks them
// once.
struct BrickProjection {
    // Set when a corner is at or behind the eye. Nothing downstream can trust
    // a screen rectangle in that case.
    crossesNearPlane : bool,
    screenMinimum : vec2<f32>,
    screenMaximum : vec2<f32>,
    // Smallest ndc.z (forward projection) and largest (reverse-Z), which is
    // the box's nearest point either way.
    nearestDepth : f32,
    nearestReverseDepth : f32,
    // True when every corner falls outside the same lateral clip plane, which
    // is the conservative rejection a perspective frustum admits.
    outsideFrustum : bool,
}

fn brickProjectClipEdges(
    base : vec4<f32>, edgeX : vec4<f32>, edgeY : vec4<f32>,
    edgeZ : vec4<f32>) -> BrickProjection {
    var result : BrickProjection;
    result.crossesNearPlane = false;
    result.screenMinimum = vec2<f32>(1.0e30);
    result.screenMaximum = vec2<f32>(-1.0e30);
    result.nearestDepth = 1.0e30;
    result.nearestReverseDepth = -1.0e30;

    var outsideLeft = true;
    var outsideRight = true;
    var outsideBottom = true;
    var outsideTop = true;
    var outsideNear = true;

    for (var corner = 0u; corner < 8u; corner += 1u) {
        var clip = base;
        if ((corner & 1u) != 0u) { clip += edgeX; }
        if ((corner & 2u) != 0u) { clip += edgeY; }
        if ((corner & 4u) != 0u) { clip += edgeZ; }

        outsideLeft = outsideLeft && clip.x < -clip.w;
        outsideRight = outsideRight && clip.x > clip.w;
        outsideBottom = outsideBottom && clip.y < -clip.w;
        outsideTop = outsideTop && clip.y > clip.w;
        outsideNear = outsideNear && clip.w <= 0.0;

        if (clip.w <= 0.0) {
            result.crossesNearPlane = true;
            continue;
        }
        let ndc = clip.xyz / clip.w;
        // Image space is top-left origin here: the renderer treats clip +Y as
        // the top of the image, which is why the draw pass negates clip Y on
        // its way to the rasterizer.
        let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        result.screenMinimum = min(result.screenMinimum, uv);
        result.screenMaximum = max(result.screenMaximum, uv);
        result.nearestDepth = min(result.nearestDepth, ndc.z);
        result.nearestReverseDepth = max(result.nearestReverseDepth, ndc.z);
    }

    // The ordinary "all corners outside one plane" test is valid only when
    // every corner is in front of the eye.  For a box that straddles the eye
    // plane, w changes sign and the lateral inequalities reverse for its
    // behind-eye corners.  Combining those corners can therefore report the
    // whole box outside left/right/top/bottom even when it surrounds the
    // camera and covers the viewport.  Keep mixed-w boxes conservatively;
    // downstream projection/HZB code already treats their rectangle as
    // unusable.  A box wholly behind the eye is still rejected by
    // outsideNear.
    let outsideLateral =
        outsideLeft || outsideRight || outsideBottom || outsideTop;
    result.outsideFrustum =
        outsideNear || (!result.crossesNearPlane && outsideLateral);
    return result;
}

fn brickProject(bounds : BrickBounds,
    worldToClip : mat4x4<f32>) -> BrickProjection {
    // The eight corners are the minimum corner plus a subset of three
    // clip-space edge vectors. Transform the base and three edges once rather
    // than doing eight full matrix products.
    let extent = bounds.maximum - bounds.minimum;
    let base = worldToClip * vec4<f32>(bounds.minimum, 1.0);
    let edgeX = worldToClip * vec4<f32>(extent.x, 0.0, 0.0, 0.0);
    let edgeY = worldToClip * vec4<f32>(0.0, extent.y, 0.0, 0.0);
    let edgeZ = worldToClip * vec4<f32>(0.0, 0.0, extent.z, 0.0);
    return brickProjectClipEdges(base, edgeX, edgeY, edgeZ);
}

// Project bounds that have already been reconstructed relative to the camera.
// Re-adding the large planetary camera position here would throw away the
// precision recovered by the canonical integer lattice. This is the same
// finite reverse-Z projection used by the brick vertex shader.
fn brickProjectCameraRelative(
    bounds : BrickBounds, worldToClip : mat4x4<f32>, nearPlane : f32)
    -> BrickProjection {
    let rowX = vec3<f32>(
        worldToClip[0].x, worldToClip[1].x, worldToClip[2].x);
    let rowY = vec3<f32>(
        worldToClip[0].y, worldToClip[1].y, worldToClip[2].y);
    let rowZ = vec3<f32>(
        worldToClip[0].z, worldToClip[1].z, worldToClip[2].z);
    let rowW = vec3<f32>(
        worldToClip[0].w, worldToClip[1].w, worldToClip[2].w);
    let depthSlope = length(rowZ);
    let extent = bounds.maximum - bounds.minimum;
    let baseW = dot(rowW, bounds.minimum);
    let base = vec4<f32>(
        dot(rowX, bounds.minimum),
        dot(rowY, bounds.minimum),
        max(nearPlane, 1.0e-6) * (1.0 + depthSlope) -
            depthSlope * baseW,
        baseW);
    let edgeX = extent.x * vec4<f32>(
        rowX.x, rowY.x, -depthSlope * rowW.x, rowW.x);
    let edgeY = extent.y * vec4<f32>(
        rowX.y, rowY.y, -depthSlope * rowW.y, rowW.y);
    let edgeZ = extent.z * vec4<f32>(
        rowX.z, rowY.z, -depthSlope * rowW.z, rowW.z);
    return brickProjectClipEdges(base, edgeX, edgeY, edgeZ);
}

// Depth-only form for a camera-relative AABB. Finite reverse-Z depth is
// `near * (1 + slope) / clipW - slope`, and clipW is linear in world
// position, so its extrema are supporting corners of the box. The culler uses
// this for its dilated safety bound: projecting another eight corners merely
// to replace the two depth extrema doubles the hot phase-one projection work.
struct BrickCameraRelativeDepthBounds {
    crossesNearPlane : bool,
    nearestDepth : f32,
    nearestReverseDepth : f32,
}

fn brickProjectCameraRelativeDepthBounds(
    bounds : BrickBounds, worldToClip : mat4x4<f32>, nearPlane : f32)
    -> BrickCameraRelativeDepthBounds {
    let rowZ = vec3<f32>(
        worldToClip[0].z, worldToClip[1].z, worldToClip[2].z);
    let rowW = vec3<f32>(
        worldToClip[0].w, worldToClip[1].w, worldToClip[2].w);
    let minimumWCorner = select(bounds.maximum, bounds.minimum,
        rowW >= vec3<f32>(0.0));
    let maximumWCorner = select(bounds.minimum, bounds.maximum,
        rowW >= vec3<f32>(0.0));
    let minimumW = dot(rowW, minimumWCorner);
    let maximumW = dot(rowW, maximumWCorner);
    let depthSlope = length(rowZ);
    let depthNumerator = max(nearPlane, 1.0e-6) *
        (1.0 + depthSlope);
    // The values are ignored when the box crosses the eye plane. Keep their
    // arithmetic finite so validation layers and diagnostic captures remain
    // useful even for a camera-inside brick.
    let safeMinimumW = max(minimumW, 1.0e-30);
    let safeMaximumW = max(maximumW, 1.0e-30);
    return BrickCameraRelativeDepthBounds(
        minimumW <= 0.0,
        depthNumerator / safeMaximumW - depthSlope,
        depthNumerator / safeMinimumW - depthSlope);
}
