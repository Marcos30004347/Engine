struct CountUniforms {
    cameraPosition : vec4<f32>,
    previousCameraPosition : vec4<f32>,
    cameraRight : vec4<f32>,
    cameraUp : vec4<f32>,
    cameraForward : vec4<f32>,
    frameIndex : u32,
    surfelCapacity : u32,
    gridCapacity : u32,
    lookupCellResolution : u32,
    targetSurfelsPerTile : u32,
    maxSurfelsPerTile : u32,
    viewport : vec2<u32>,
    centralGridHalfExtent : f32,
    gridScale : f32,
    frustumFarDistance : f32,
    targetScreenRadiusPixels : f32,
    lookupRadiusScale : f32,
    worldUnitsPerPixelAtUnitDepth : f32,
    spawnMinRadius : f32,
    spawnDepthRadiusScale : f32,
    recycleCoverageThreshold : f32,
    recycleFreeListPressureBias : f32,
    visibleRecycleMaxAgeFrames : u32,
    hiddenRecycleMaxAgeFrames : u32,
    recycleMaxAgeFrames : u32,
    visibleUntouchedAgeStep : u32,
    hiddenUntouchedAgeStep : u32,
    denseSurfelsAgeStep : u32,
    hiddenDistanceThresholdScale : f32,
    hiddenDensityThresholdScale : f32,
    hiddenDensityAgeScale : f32,
    radiusUpdateRate : f32,
    undersizedAgeBias : f32,
    oversizedAgeBias : f32,
    ownerResolveOnly : u32,
    ownerSelectionMode : u32,
};

struct Attachment {
    instanceId : u32,
    triangleId : u32,
    attachmentKind : u32,
    flags : u32,
    localPosition : vec4<f32>,
    localNormal : vec4<f32>,
};

struct Lifecycle {
    ageFrames : u32,
    lastTouchedFrame : u32,
    lastCoverage : f32,
    flags : u32,
};

struct InstanceData {
    modelMatrix : mat4x4<f32>,
    hierarchyStartOffset : u32,
    quantization_factor : u32,
    unit_scale_bits : u32,
    pageTableOffset : u32,
    materialIndex : u32,
    meshPartTransformsOffset : u32,
    _padding0 : u32,
    _padding1 : u32,
    previousModelMatrix : mat4x4<f32>,
    previousMeshPartTransformsOffset : u32,
    _temporalFlags : u32,
    _padding2 : u32,
    _padding3 : u32,
};

struct RegionBox {
    minCoord : vec3<u32>,
    maxCoord : vec3<u32>,
    valid : u32,
};

struct LookupCell {
    region : u32,
    coord : vec3<u32>,
    cellId : u32,
};

const SURFEL_ATTACHMENT_NONE : u32 = 0u;
const SURFEL_ATTACHMENT_SKINNED : u32 = 2u;
const SURFEL_ATTACHMENT_INVALID_BONE : u32 = 0xffffu;
const REGION_CENTER : u32 = 0u;
const REGION_POS_X : u32 = 1u;
const REGION_NEG_X : u32 = 2u;
const REGION_POS_Y : u32 = 3u;
const REGION_NEG_Y : u32 = 4u;
const REGION_POS_Z : u32 = 5u;
const REGION_NEG_Z : u32 = 6u;
const SURFEL_OWNER_OLDEST : u32 = 0u;
const SURFEL_OWNER_NEWEST : u32 = 1u;
const OUTER_DEPTH_WARP_ALPHA : f32 = 7.0;
const ALLOCATION_GRACE_FRAMES : u32 = 6u;

@group(0) @binding(0) var<uniform> uniforms : CountUniforms;
@group(0) @binding(1) var<storage, read_write> surfelStack : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> surfelPosR : array<vec4<f32>>;
@group(0) @binding(3) var<storage, read_write> surfelNorW : array<vec4<f32>>;
@group(0) @binding(4) var<storage, read> surfelAttachment : array<Attachment>;
@group(0) @binding(5) var<storage, read_write> surfelLifecycle : array<Lifecycle>;
@group(0) @binding(6) var<storage, read_write> surfelLifecycleFeedback : array<atomic<u32>>;
@group(0) @binding(7) var<storage, read_write> surfelOwnerCell : array<atomic<u32>>;
@group(0) @binding(8) var<storage, read_write> surfelGrid : array<atomic<u32>>;
@group(0) @binding(9) var<storage, read> previousGrid : array<u32>;
@group(0) @binding(10) var<storage, read> instances : array<InstanceData>;
@group(0) @binding(11) var<storage, read> meshPartTransforms : array<mat4x4<f32>>;

fn identity_matrix() -> mat4x4<f32> {
    return mat4x4<f32>(
        vec4<f32>(1.0, 0.0, 0.0, 0.0),
        vec4<f32>(0.0, 1.0, 0.0, 0.0),
        vec4<f32>(0.0, 0.0, 1.0, 0.0),
        vec4<f32>(0.0, 0.0, 0.0, 1.0));
}

fn safe_normalize(vectorValue : vec3<f32>, fallbackValue : vec3<f32>) -> vec3<f32> {
    let vectorLength = length(vectorValue);
    return select(fallbackValue, vectorValue / vectorLength, vectorLength > 1.0e-6);
}

fn surfel_view_coordinates(pos : vec3<f32>, cameraPos : vec3<f32>, cameraRight : vec3<f32>, cameraUp : vec3<f32>, cameraForward : vec3<f32>) -> vec3<f32> {
    let offset = pos - cameraPos;
    return vec3<f32>(dot(offset, cameraRight), dot(offset, cameraUp), dot(offset, cameraForward));
}

fn surfel_stored_radius(posR : vec4<f32>) -> f32 {
    return max(sqrt(posR.w), 1.0e-6);
}

fn surfel_camera_distance(pos : vec3<f32>) -> f32 {
    return max(distance(pos, uniforms.cameraPosition.xyz), 1.0e-4);
}

fn surfel_previous_camera_distance(pos : vec3<f32>) -> f32 {
    return max(distance(pos, uniforms.previousCameraPosition.xyz), 1.0e-4);
}

fn surfel_desired_radius(pos : vec3<f32>) -> f32 {
    let cameraDistance = surfel_camera_distance(pos);
    let projectedRadius = uniforms.targetScreenRadiusPixels * uniforms.worldUnitsPerPixelAtUnitDepth * cameraDistance;
    return max(uniforms.spawnMinRadius, projectedRadius);
}

fn surfel_lookup_radius(drawRadius : f32) -> f32 {
    return max(drawRadius * uniforms.lookupRadiusScale, uniforms.spawnMinRadius);
}

fn surfel_projects_to_screen(pos : vec3<f32>, radius : f32) -> bool {
    let view = surfel_view_coordinates(pos, uniforms.cameraPosition.xyz, uniforms.cameraRight.xyz, uniforms.cameraUp.xyz, uniforms.cameraForward.xyz);
    if (view.z <= 1.0e-4) {
        return false;
    }
    let halfHeight = 0.5 * f32(max(uniforms.viewport.y, 1u)) * uniforms.worldUnitsPerPixelAtUnitDepth * view.z + radius;
    let halfWidth = 0.5 * f32(max(uniforms.viewport.x, 1u)) * uniforms.worldUnitsPerPixelAtUnitDepth * view.z + radius;
    return abs(view.x) <= halfWidth && abs(view.y) <= halfHeight;
}

fn lookup_resolution() -> u32 {
    return max(uniforms.lookupCellResolution, 1u);
}

fn cells_per_region() -> u32 {
    let resolution = lookup_resolution();
    return resolution * resolution * resolution;
}

fn surfel_base_cell_size() -> f32 {
    return (2.0 * max(uniforms.centralGridHalfExtent, 1.0e-4)) / f32(lookup_resolution());
}

fn central_half_extent() -> f32 {
    return max(uniforms.centralGridHalfExtent, 1.0e-4);
}

fn center_half_extent_vec3() -> vec3<f32> {
    return vec3<f32>(central_half_extent());
}

fn outer_depth_max() -> f32 {
    return max(uniforms.frustumFarDistance - central_half_extent(), surfel_base_cell_size());
}

fn clamp_index(index : i32, resolution : u32) -> u32 {
    return u32(clamp(index, 0, i32(max(resolution, 1u)) - 1));
}

fn camera_local(worldPosition : vec3<f32>, cameraPosition : vec3<f32>) -> vec3<f32> {
    return worldPosition - cameraPosition;
}

fn normalized_outer_overflow(local : vec3<f32>) -> vec3<f32> {
    let ext = center_half_extent_vec3();
    let overflow = max(abs(local) - ext, vec3<f32>(0.0));
    return overflow / max(ext, vec3<f32>(1.0e-4));
}

fn region_offset(region : u32) -> u32 {
    return region * cells_per_region();
}

fn cell_index(region : u32, coord : vec3<u32>) -> u32 {
    let resolution = lookup_resolution();
    let index = region_offset(region) + coord.x + coord.y * resolution + coord.z * resolution * resolution;
    return min(index, max(uniforms.gridCapacity, 1u) - 1u);
}

fn lookup_cell(pos : vec3<f32>, cameraPosition : vec3<f32>) -> LookupCell {
    let resolution = lookup_resolution();
    let cellSize = surfel_base_cell_size();
    let local = camera_local(pos, cameraPosition);
    let centerHalf = center_half_extent_vec3();
    let centralLocal = (local + centerHalf) / cellSize;
    let centralCoordI = vec3<i32>(floor(centralLocal));
    let insideCentral =
        abs(local.x) <= centerHalf.x &&
        abs(local.y) <= centerHalf.y &&
        abs(local.z) <= centerHalf.z;
    if (insideCentral) {
        let coord = vec3<u32>(
            clamp_index(centralCoordI.x, resolution),
            clamp_index(centralCoordI.y, resolution),
            clamp_index(centralCoordI.z, resolution));
        return LookupCell(REGION_CENTER, coord, cell_index(REGION_CENTER, coord));
    }

    let face = face_coordinates(local);
    let slice = depth_to_slice(face.distance);
    let faceHalf = face_half_extents_for_slice(face, slice);
    let uv01 = clamp(vec2<f32>(
            0.5 * (face.u / max(faceHalf.x, 1.0e-4) + 1.0),
            0.5 * (face.w / max(faceHalf.y, 1.0e-4) + 1.0)),
        vec2<f32>(0.0),
        vec2<f32>(0.999999));
    let coord = vec3<u32>(
        clamp_index(i32(floor(uv01.x * f32(resolution))), resolution),
        clamp_index(i32(floor(uv01.y * f32(resolution))), resolution),
        slice);
    return LookupCell(face.region, coord, cell_index(face.region, coord));
}

struct FaceCoordinates {
    region : u32,
    signedDistance : f32,
    distance : f32,
    u : f32,
    w : f32,
    baseDepth : f32,
    baseHalfU : f32,
    baseHalfW : f32,
};

fn face_coordinates(local : vec3<f32>) -> FaceCoordinates {
    let ext = center_half_extent_vec3();
    let overflow = normalized_outer_overflow(local);
    if (overflow.x >= overflow.y && overflow.x >= overflow.z) {
        if (local.x >= 0.0) {
            let signedDistance = local.x - ext.x;
            return FaceCoordinates(REGION_POS_X, signedDistance, max(signedDistance, 0.0), -local.z, local.y, ext.x, ext.z, ext.y);
        }
        let signedDistance = -local.x - ext.x;
        return FaceCoordinates(REGION_NEG_X, signedDistance, max(signedDistance, 0.0), local.z, local.y, ext.x, ext.z, ext.y);
    }
    if (overflow.y >= overflow.z) {
        if (local.y >= 0.0) {
            let signedDistance = local.y - ext.y;
            return FaceCoordinates(REGION_POS_Y, signedDistance, max(signedDistance, 0.0), local.x, -local.z, ext.y, ext.x, ext.z);
        }
        let signedDistance = -local.y - ext.y;
        return FaceCoordinates(REGION_NEG_Y, signedDistance, max(signedDistance, 0.0), local.x, local.z, ext.y, ext.x, ext.z);
    }
    if (local.z >= 0.0) {
        let signedDistance = local.z - ext.z;
        return FaceCoordinates(REGION_POS_Z, signedDistance, max(signedDistance, 0.0), local.x, local.y, ext.z, ext.x, ext.y);
    }
    let signedDistance = -local.z - ext.z;
    return FaceCoordinates(REGION_NEG_Z, signedDistance, max(signedDistance, 0.0), -local.x, local.y, ext.z, ext.x, ext.y);
}

fn face_coordinates_for_region(local : vec3<f32>, region : u32) -> FaceCoordinates {
    let ext = center_half_extent_vec3();
    switch (region) {
        case REGION_POS_X: {
            let signedDistance = local.x - ext.x;
            return FaceCoordinates(REGION_POS_X, signedDistance, max(signedDistance, 0.0), -local.z, local.y, ext.x, ext.z, ext.y);
        }
        case REGION_NEG_X: {
            let signedDistance = -local.x - ext.x;
            return FaceCoordinates(REGION_NEG_X, signedDistance, max(signedDistance, 0.0), local.z, local.y, ext.x, ext.z, ext.y);
        }
        case REGION_POS_Y: {
            let signedDistance = local.y - ext.y;
            return FaceCoordinates(REGION_POS_Y, signedDistance, max(signedDistance, 0.0), local.x, -local.z, ext.y, ext.x, ext.z);
        }
        case REGION_NEG_Y: {
            let signedDistance = -local.y - ext.y;
            return FaceCoordinates(REGION_NEG_Y, signedDistance, max(signedDistance, 0.0), local.x, local.z, ext.y, ext.x, ext.z);
        }
        case REGION_POS_Z: {
            let signedDistance = local.z - ext.z;
            return FaceCoordinates(REGION_POS_Z, signedDistance, max(signedDistance, 0.0), local.x, local.y, ext.z, ext.x, ext.y);
        }
        default: {
            let signedDistance = -local.z - ext.z;
            return FaceCoordinates(REGION_NEG_Z, signedDistance, max(signedDistance, 0.0), -local.x, local.y, ext.z, ext.x, ext.y);
        }
    }
}

fn face_half_extents(face : FaceCoordinates, distanceFromFace : f32) -> vec2<f32> {
    let depthScale = (face.baseDepth + max(distanceFromFace, 0.0)) / max(face.baseDepth, 1.0e-4);
    return vec2<f32>(face.baseHalfU * depthScale, face.baseHalfW * depthScale);
}

fn depth_to_slice(distanceFromFace : f32) -> u32 {
    let resolution = lookup_resolution();
    let t = clamp(max(distanceFromFace, 0.0) / outer_depth_max(), 0.0, 0.999999);
    let warped = clamp(log(1.0 + OUTER_DEPTH_WARP_ALPHA * t) / log(1.0 + OUTER_DEPTH_WARP_ALPHA), 0.0, 0.999999);
    return clamp_index(i32(floor(warped * f32(resolution))), resolution);
}

fn depth_from_slice_coord(sliceCoord : u32) -> f32 {
    let resolution = f32(lookup_resolution());
    let clampedCoord = min(sliceCoord, lookup_resolution());
    let warped = clamp(f32(clampedCoord) / resolution, 0.0, 1.0);
    let t = (pow(1.0 + OUTER_DEPTH_WARP_ALPHA, warped) - 1.0) / OUTER_DEPTH_WARP_ALPHA;
    return clamp(t, 0.0, 1.0) * outer_depth_max();
}

fn slice_depth_bounds(sliceCoord : u32) -> vec2<f32> {
    let depthMin = depth_from_slice_coord(sliceCoord);
    let depthMax = max(depthMin + 1.0e-5, depth_from_slice_coord(sliceCoord + 1u));
    return vec2<f32>(depthMin, depthMax);
}

fn face_half_extents_for_slice(face : FaceCoordinates, sliceCoord : u32) -> vec2<f32> {
    let depthBounds = slice_depth_bounds(sliceCoord);
    let depthCenter = 0.5 * (depthBounds.x + depthBounds.y);
    return face_half_extents(face, depthCenter);
}

fn blend_radius(currentRadius : f32, desiredRadius : f32) -> f32 {
    let clampedRate = clamp(uniforms.radiusUpdateRate, 0.0, 1.0);
    return max(uniforms.spawnMinRadius, mix(currentRadius, desiredRadius, clampedRate));
}

fn attachment_transform(attachment : Attachment) -> mat4x4<f32> {
    if (attachment.attachmentKind == SURFEL_ATTACHMENT_NONE || attachment.instanceId >= arrayLength(&instances)) {
        return identity_matrix();
    }

    let instance = instances[attachment.instanceId];
    if (attachment.attachmentKind == SURFEL_ATTACHMENT_SKINNED) {
        let boneIndex = attachment.flags & SURFEL_ATTACHMENT_INVALID_BONE;
        let boneTransformIndex = instance.meshPartTransformsOffset + boneIndex;
        if (boneIndex != SURFEL_ATTACHMENT_INVALID_BONE && boneTransformIndex < arrayLength(&meshPartTransforms)) {
            return instance.modelMatrix * meshPartTransforms[boneTransformIndex];
        }
    }

    return instance.modelMatrix;
}

fn central_lookup_box(local : vec3<f32>, radius : f32) -> RegionBox {
    let resolution = lookup_resolution();
    let cellSize = surfel_base_cell_size();
    let centerHalf = center_half_extent_vec3();

    let centralMinI = vec3<i32>(floor((local - vec3<f32>(radius) + centerHalf) / cellSize));
    let centralMaxI = vec3<i32>(floor((local + vec3<f32>(radius) + centerHalf) / cellSize));
    let centralMin = vec3<u32>(
        clamp_index(centralMinI.x, resolution),
        clamp_index(centralMinI.y, resolution),
        clamp_index(centralMinI.z, resolution));
    let centralMax = vec3<u32>(
        clamp_index(centralMaxI.x, resolution),
        clamp_index(centralMaxI.y, resolution),
        clamp_index(centralMaxI.z, resolution));
    let centralValid =
        select(0u, 1u,
            local.x + radius >= -centerHalf.x &&
            local.y + radius >= -centerHalf.y &&
            local.z + radius >= -centerHalf.z &&
            local.x - radius <= centerHalf.x &&
            local.y - radius <= centerHalf.y &&
            local.z - radius <= centerHalf.z);
    return RegionBox(centralMin, centralMax, centralValid);
}

fn outer_lookup_box(local : vec3<f32>, radius : f32, region : u32) -> RegionBox {
    let resolution = lookup_resolution();
    let face = face_coordinates_for_region(local, region);
    let frustumValid = select(0u, 1u, face.signedDistance + radius > 0.0);
    let depthMin = max(face.signedDistance - radius, 0.0);
    let depthMax = max(depthMin + 1.0e-5, face.signedDistance + radius);
    let faceHalfMin = face_half_extents(face, depthMin);
    let faceHalfMax = face_half_extents(face, depthMax);
    let nuMinNear = 0.5 * ((face.u - radius) / max(faceHalfMin.x, 1.0e-4) + 1.0);
    let nuMinFar = 0.5 * ((face.u - radius) / max(faceHalfMax.x, 1.0e-4) + 1.0);
    let nuMaxNear = 0.5 * ((face.u + radius) / max(faceHalfMin.x, 1.0e-4) + 1.0);
    let nuMaxFar = 0.5 * ((face.u + radius) / max(faceHalfMax.x, 1.0e-4) + 1.0);
    let nwMinNear = 0.5 * ((face.w - radius) / max(faceHalfMin.y, 1.0e-4) + 1.0);
    let nwMinFar = 0.5 * ((face.w - radius) / max(faceHalfMax.y, 1.0e-4) + 1.0);
    let nwMaxNear = 0.5 * ((face.w + radius) / max(faceHalfMin.y, 1.0e-4) + 1.0);
    let nwMaxFar = 0.5 * ((face.w + radius) / max(faceHalfMax.y, 1.0e-4) + 1.0);
    let nuMin = clamp(min(nuMinNear, nuMinFar), 0.0, 0.999999);
    let nuMax = clamp(max(nuMaxNear, nuMaxFar), 0.0, 0.999999);
    let nwMin = clamp(min(nwMinNear, nwMinFar), 0.0, 0.999999);
    let nwMax = clamp(max(nwMaxNear, nwMaxFar), 0.0, 0.999999);
    let frustumMin = vec3<u32>(
        clamp_index(i32(floor(nuMin * f32(resolution))), resolution),
        clamp_index(i32(floor(nwMin * f32(resolution))), resolution),
        depth_to_slice(depthMin));
    let frustumMax = vec3<u32>(
        clamp_index(i32(floor(nuMax * f32(resolution))), resolution),
        clamp_index(i32(floor(nwMax * f32(resolution))), resolution),
        depth_to_slice(depthMax));
    return RegionBox(frustumMin, frustumMax, frustumValid);
}

fn central_overlap_box_for_cell(local : vec3<f32>, radius : f32, coord : vec3<u32>) -> RegionBox {
    let resolution = lookup_resolution();
    let cellSize = surfel_base_cell_size();
    let centerHalf = center_half_extent_vec3();
    let cellMin = -centerHalf + vec3<f32>(coord) * cellSize;
    let cellMax = cellMin + vec3<f32>(cellSize);
    var minCoord = coord;
    var maxCoord = coord;
    if (coord.x > 0u && local.x - radius < cellMin.x) {
        minCoord.x -= 1u;
    }
    if (coord.y > 0u && local.y - radius < cellMin.y) {
        minCoord.y -= 1u;
    }
    if (coord.z > 0u && local.z - radius < cellMin.z) {
        minCoord.z -= 1u;
    }
    if (coord.x + 1u < resolution && local.x + radius > cellMax.x) {
        maxCoord.x += 1u;
    }
    if (coord.y + 1u < resolution && local.y + radius > cellMax.y) {
        maxCoord.y += 1u;
    }
    if (coord.z + 1u < resolution && local.z + radius > cellMax.z) {
        maxCoord.z += 1u;
    }
    return RegionBox(minCoord, maxCoord, 1u);
}

fn outer_overlap_box_for_cell(local : vec3<f32>, radius : f32, region : u32, coord : vec3<u32>) -> RegionBox {
    let resolution = lookup_resolution();
    let face = face_coordinates_for_region(local, region);
    let faceHalf = face_half_extents_for_slice(face, coord.z);
    let uvCellSize = (2.0 * faceHalf) / f32(resolution);
    let uvMin = -faceHalf + vec2<f32>(coord.xy) * uvCellSize;
    let uvMax = uvMin + uvCellSize;
    let depthBounds = slice_depth_bounds(coord.z);
    var minCoord = coord;
    var maxCoord = coord;
    if (coord.x > 0u && face.u - radius < uvMin.x) {
        minCoord.x -= 1u;
    }
    if (coord.y > 0u && face.w - radius < uvMin.y) {
        minCoord.y -= 1u;
    }
    if (coord.z > 0u && face.signedDistance - radius < depthBounds.x) {
        minCoord.z -= 1u;
    }
    if (coord.x + 1u < resolution && face.u + radius > uvMax.x) {
        maxCoord.x += 1u;
    }
    if (coord.y + 1u < resolution && face.w + radius > uvMax.y) {
        maxCoord.y += 1u;
    }
    if (coord.z + 1u < resolution && face.signedDistance + radius > depthBounds.y) {
        maxCoord.z += 1u;
    }
    return RegionBox(minCoord, maxCoord, 1u);
}

fn overlap_box_for_cell(local : vec3<f32>, radius : f32, baseCell : LookupCell) -> RegionBox {
    if (baseCell.region == REGION_CENTER) {
        return central_overlap_box_for_cell(local, radius, baseCell.coord);
    }
    return outer_overlap_box_for_cell(local, radius, baseCell.region, baseCell.coord);
}

fn cell_surface_span(local : vec3<f32>, baseCell : LookupCell) -> f32 {
    if (baseCell.region == REGION_CENTER) {
        return surfel_base_cell_size();
    }
    let face = face_coordinates_for_region(local, baseCell.region);
    let faceHalf = face_half_extents_for_slice(face, baseCell.coord.z);
    let uvCellSize = (2.0 * faceHalf) / f32(lookup_resolution());
    return sqrt(max(uvCellSize.x * uvCellSize.y, 1.0e-8));
}

fn desired_density_thresholds(local : vec3<f32>, baseCell : LookupCell, desiredRadius : f32) -> vec2<u32> {
    let cellSpan = cell_surface_span(local, baseCell);
    let radiusSpan = max(2.0 * desiredRadius, 1.0e-4);
    let densityScale = clamp((cellSpan / radiusSpan) * (cellSpan / radiusSpan), 0.25, 16.0) * max(uniforms.hiddenDensityThresholdScale, 0.1);
    let targetDensity = max(1u, u32(ceil(f32(max(uniforms.targetSurfelsPerTile, 1u)) * densityScale)));
    let maxDensity = max(targetDensity, u32(ceil(f32(max(uniforms.maxSurfelsPerTile, targetDensity)) * densityScale)));
    return vec2<u32>(targetDensity, maxDensity);
}

fn region_face_overlap(local : vec3<f32>, radius : f32, region : u32) -> bool {
    let centerHalf = central_half_extent();
    switch (region) {
        case REGION_POS_X: {
            return local.x + radius > centerHalf && local.x - radius < centerHalf;
        }
        case REGION_NEG_X: {
            return local.x - radius < -centerHalf && local.x + radius > -centerHalf;
        }
        case REGION_POS_Y: {
            return local.y + radius > centerHalf && local.y - radius < centerHalf;
        }
        case REGION_NEG_Y: {
            return local.y - radius < -centerHalf && local.y + radius > -centerHalf;
        }
        case REGION_POS_Z: {
            return local.z + radius > centerHalf && local.z - radius < centerHalf;
        }
        default: {
            return local.z - radius < -centerHalf && local.z + radius > -centerHalf;
        }
    }
}

fn count_lookup_box(region : u32, minCoord : vec3<u32>, maxCoord : vec3<u32>) {
    for (var z = minCoord.z; z <= maxCoord.z; z += 1u) {
        for (var y = minCoord.y; y <= maxCoord.y; y += 1u) {
            for (var x = minCoord.x; x <= maxCoord.x; x += 1u) {
                let index = cell_index(region, vec3<u32>(x, y, z));
                atomicAdd(&surfelGrid[index + 1u], 1u);
            }
        }
    }
}

fn count_surfel(pos : vec3<f32>, radius : f32) {
    let local = camera_local(pos, uniforms.cameraPosition.xyz);
    let baseCell = lookup_cell(pos, uniforms.cameraPosition.xyz);
    let overlapBox = overlap_box_for_cell(local, radius, baseCell);
    count_lookup_box(baseCell.region, overlapBox.minCoord, overlapBox.maxCoord);
    let centralBox = central_lookup_box(local, radius);
    if (baseCell.region != REGION_CENTER && centralBox.valid != 0u && region_face_overlap(local, radius, baseCell.region)) {
        count_lookup_box(REGION_CENTER, centralBox.minCoord, centralBox.maxCoord);
    }
    for (var region = REGION_POS_X; region <= REGION_NEG_Z; region += 1u) {
        if (region == baseCell.region || !region_face_overlap(local, radius, region)) {
            continue;
        }
        let regionBox = outer_lookup_box(local, radius, region);
        if (regionBox.valid != 0u) {
            count_lookup_box(region, regionBox.minCoord, regionBox.maxCoord);
        }
    }
}

fn previous_cell_count(cellId : u32) -> u32 {
    if (uniforms.gridCapacity == 0u) {
        return 0u;
    }
    let clampedCellId = min(cellId, uniforms.gridCapacity - 1u);
    let begin = previousGrid[clampedCellId];
    let end = previousGrid[clampedCellId + 1u];
    return max(end, begin) - min(end, begin);
}

fn surfel_cell_density(pos : vec3<f32>) -> u32 {
    let baseCell = lookup_cell(pos, uniforms.cameraPosition.xyz);
    return previous_cell_count(baseCell.cellId);
}

fn recycle_surfel(surfelId : u32) {
    surfelPosR[surfelId] = vec4<f32>(0.0);
    surfelNorW[surfelId] = vec4<f32>(0.0);
    surfelLifecycle[surfelId] = Lifecycle(0u, 0u, 0.0, 0u);
    let freeIndex = atomicAdd(&surfelStack[0], 1u);
    if (freeIndex < uniforms.surfelCapacity) {
        atomicStore(&surfelStack[1u + freeIndex], surfelId);
    } else {
        atomicStore(&surfelStack[0], uniforms.surfelCapacity);
    }
}

fn surfel_owner_better(candidateId : u32, incumbentId : u32) -> bool {
    if (candidateId == incumbentId) {
        return false;
    }
    if (candidateId >= uniforms.surfelCapacity) {
        return false;
    }
    if (incumbentId >= uniforms.surfelCapacity) {
        return true;
    }

    let candidateStamp = surfelLifecycle[candidateId].flags;
    let incumbentStamp = surfelLifecycle[incumbentId].flags;
    if (candidateStamp != incumbentStamp) {
        if (uniforms.ownerSelectionMode == SURFEL_OWNER_NEWEST) {
            return candidateStamp > incumbentStamp;
        }
        return candidateStamp < incumbentStamp;
    }

    return candidateId < incumbentId;
}

@compute @workgroup_size(128, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let surfelId = globalInvocationId.x;
    if (surfelId >= uniforms.surfelCapacity) {
        return;
    }

    var posR = surfelPosR[surfelId];
    if (posR.w <= 0.0) {
        return;
    }

    var norW = surfelNorW[surfelId];
    let attachment = surfelAttachment[surfelId];
    if (attachment.attachmentKind != SURFEL_ATTACHMENT_NONE && attachment.instanceId < arrayLength(&instances)) {
        let transform = attachment_transform(attachment);
        posR = vec4<f32>((transform * vec4<f32>(attachment.localPosition.xyz, 1.0)).xyz, posR.w);
        norW = vec4<f32>(safe_normalize((transform * vec4<f32>(attachment.localNormal.xyz, 0.0)).xyz, norW.xyz), norW.w);
        surfelPosR[surfelId] = posR;
        surfelNorW[surfelId] = norW;
    }

    if (uniforms.ownerResolveOnly != 0u) {
        return;
    }

    let lifecycle = surfelLifecycle[surfelId];
    let feedback = atomicExchange(&surfelLifecycleFeedback[surfelId], 0u);
    let touchedThisFrame = (feedback & 1u) != 0u;
    let denseFeedback = feedback >> 1u;
    let desiredRadius = surfel_desired_radius(posR.xyz);
    let currentRadius = blend_radius(surfel_stored_radius(posR), desiredRadius);
    posR.w = currentRadius * currentRadius;
    surfelPosR[surfelId] = posR;

    let freeCount = min(atomicLoad(&surfelStack[0]), uniforms.surfelCapacity);
    let allocatedCount = uniforms.surfelCapacity - freeCount;
    let pressure = f32(allocatedCount) / max(f32(uniforms.surfelCapacity), 1.0);
    let recyclePressure = clamp((pressure - 0.75) / 0.25, 0.0, 1.0);
    let pressureScale = 1.0 + recyclePressure * uniforms.recycleFreeListPressureBias;
    let visibleOnScreen = surfel_projects_to_screen(posR.xyz, currentRadius);
    let local = camera_local(posR.xyz, uniforms.cameraPosition.xyz);
    let baseCell = lookup_cell(posR.xyz, uniforms.cameraPosition.xyz);
    let cameraDistance = surfel_camera_distance(posR.xyz);
    let previousCameraDistance = surfel_previous_camera_distance(posR.xyz);
    let hiddenDistanceThreshold = max(0.25 * desiredRadius, 0.125 * surfel_base_cell_size()) * max(uniforms.hiddenDistanceThresholdScale, 0.0);
    let movingAwayWhileHidden = !visibleOnScreen && cameraDistance > previousCameraDistance + hiddenDistanceThreshold;
    let cellDensity = surfel_cell_density(posR.xyz);
    let densityThresholds = desired_density_thresholds(local, baseCell, desiredRadius);
    let targetCellDensity = densityThresholds.x;
    let maxCellDensity = densityThresholds.y;
    let softCellOverflow = max(cellDensity, targetCellDensity) - targetCellDensity;
    let hardCellOverflow = max(cellDensity, maxCellDensity) - maxCellDensity;
    let hiddenSoftCellOverflow = select(0u, softCellOverflow, movingAwayWhileHidden);
    let hiddenHardCellOverflow = select(0u, hardCellOverflow, movingAwayWhileHidden);
    let hiddenAgeStepBase = max(1u, u32(ceil(f32(max(uniforms.hiddenUntouchedAgeStep, 1u)) * max(uniforms.hiddenDensityAgeScale, 0.0))));
    let baseMaxAge = max(
        select(uniforms.hiddenRecycleMaxAgeFrames, uniforms.visibleRecycleMaxAgeFrames, visibleOnScreen),
        max(uniforms.recycleMaxAgeFrames, 1u));
    let effectiveMaxAge = max(1u, u32(f32(baseMaxAge) / pressureScale));
    let projectedRadiusPixels = currentRadius / max(uniforms.worldUnitsPerPixelAtUnitDepth * cameraDistance, 1.0e-6);
    let sizeRatio = projectedRadiusPixels / max(uniforms.targetScreenRadiusPixels, 1.0e-4);

    var ageStep = 0u;
    if (!touchedThisFrame) {
        if (visibleOnScreen) {
            ageStep += uniforms.visibleUntouchedAgeStep;
        } else if (movingAwayWhileHidden && (denseFeedback > 0u || recyclePressure > 0.0 || hiddenSoftCellOverflow > 0u || hiddenHardCellOverflow > 0u)) {
            ageStep += hiddenAgeStepBase;
        }
    }
    if (hiddenSoftCellOverflow > 0u) {
        ageStep += min(hiddenSoftCellOverflow, 2u) * hiddenAgeStepBase;
    }
    if (denseFeedback > 0u) {
        ageStep += denseFeedback * max(uniforms.denseSurfelsAgeStep, 1u);
    }
    if (hiddenHardCellOverflow > 0u) {
        ageStep += min(hiddenHardCellOverflow, 2u) * hiddenAgeStepBase;
    }
    if (visibleOnScreen && sizeRatio < uniforms.undersizedAgeBias) {
        let undersizedStep = max(1u, u32(ceil((uniforms.undersizedAgeBias - sizeRatio) * 8.0)));
        if (!touchedThisFrame || denseFeedback > 0u || visibleOnScreen || recyclePressure > 0.0) {
            ageStep += undersizedStep;
        }
    }
    if (visibleOnScreen && sizeRatio > uniforms.oversizedAgeBias) {
        ageStep += max(1u, u32(ceil((sizeRatio - uniforms.oversizedAgeBias) * 8.0)));
    }
    if (visibleOnScreen && recyclePressure > 0.0) {
        ageStep += max(1u, u32(ceil(recyclePressure * 4.0)));
    }

    let stableTouch = touchedThisFrame && denseFeedback == 0u && hiddenSoftCellOverflow == 0u && hiddenHardCellOverflow == 0u && ageStep == 0u;
    let nextAge = select(min(lifecycle.ageFrames + ageStep, 0xfffffff0u), 0u, stableTouch);
    let spawnFrame = min(lifecycle.flags, uniforms.frameIndex);
    let inAllocationGrace = uniforms.frameIndex - spawnFrame < ALLOCATION_GRACE_FRAMES;
    let clampedAge = select(nextAge, min(nextAge, effectiveMaxAge - 1u), inAllocationGrace && effectiveMaxAge > 1u);
    let expired = clampedAge >= effectiveMaxAge;
    if (expired) {
        recycle_surfel(surfelId);
        return;
    }

    let lastTouchedFrame = select(lifecycle.lastTouchedFrame, uniforms.frameIndex, touchedThisFrame);
    surfelLifecycle[surfelId] = Lifecycle(clampedAge, lastTouchedFrame, sizeRatio, lifecycle.flags);
    count_surfel(posR.xyz, surfel_lookup_radius(currentRadius));
}
