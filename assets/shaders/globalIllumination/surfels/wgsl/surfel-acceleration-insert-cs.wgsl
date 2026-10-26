struct InsertUniforms {
    cameraPosition : vec4<f32>,
    cameraRight : vec4<f32>,
    cameraUp : vec4<f32>,
    cameraForward : vec4<f32>,
    surfelCapacity : u32,
    gridCapacity : u32,
    listCapacity : u32,
    lookupCellResolution : u32,
    centralGridHalfExtent : f32,
    gridScale : f32,
    frustumFarDistance : f32,
    lookupRadiusScale : f32,
    spawnMinRadius : f32,
    spawnDepthRadiusScale : f32,
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

const REGION_CENTER : u32 = 0u;
const REGION_POS_X : u32 = 1u;
const REGION_NEG_X : u32 = 2u;
const REGION_POS_Y : u32 = 3u;
const REGION_NEG_Y : u32 = 4u;
const REGION_POS_Z : u32 = 5u;
const REGION_NEG_Z : u32 = 6u;
const OUTER_DEPTH_WARP_ALPHA : f32 = 7.0;

@group(0) @binding(0) var<uniform> uniforms : InsertUniforms;
@group(0) @binding(1) var<storage, read> surfelPosR : array<vec4<f32>>;
@group(0) @binding(2) var<storage, read_write> surfelOwnerCell : array<atomic<u32>>;
@group(0) @binding(3) var<storage, read> surfelGrid : array<atomic<u32>>;
@group(0) @binding(4) var<storage, read_write> surfelGridWriteCursor : array<atomic<u32>>;
@group(0) @binding(5) var<storage, read_write> surfelList : array<u32>;

fn surfel_desired_radius(pos : vec3<f32>, cameraPos : vec3<f32>) -> f32 {
    return max(uniforms.spawnMinRadius, distance(pos, cameraPos) * uniforms.spawnDepthRadiusScale);
}

fn surfel_stored_radius(posR : vec4<f32>) -> f32 {
    return max(sqrt(posR.w), 1.0e-6);
}

fn surfel_lookup_radius(drawRadius : f32) -> f32 {
    return max(drawRadius * uniforms.lookupRadiusScale, uniforms.spawnMinRadius);
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

fn lookup_cell(pos : vec3<f32>) -> LookupCell {
    let resolution = lookup_resolution();
    let cellSize = surfel_base_cell_size();
    let local = camera_local(pos, uniforms.cameraPosition.xyz);
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

fn insert_lookup_box(region : u32, minCoord : vec3<u32>, maxCoord : vec3<u32>, surfelId : u32) {
    for (var z = minCoord.z; z <= maxCoord.z; z += 1u) {
        for (var y = minCoord.y; y <= maxCoord.y; y += 1u) {
            for (var x = minCoord.x; x <= maxCoord.x; x += 1u) {
                let index = cell_index(region, vec3<u32>(x, y, z));
                let offset = atomicAdd(&surfelGridWriteCursor[index], 1u);
                if (offset < uniforms.listCapacity) {
                    surfelList[offset] = surfelId;
                }
            }
        }
    }
}

fn insert_surfel(pos : vec3<f32>, radius : f32, surfelId : u32) {
    let local = camera_local(pos, uniforms.cameraPosition.xyz);
    let baseCell = lookup_cell(pos);
    let overlapBox = overlap_box_for_cell(local, radius, baseCell);
    insert_lookup_box(baseCell.region, overlapBox.minCoord, overlapBox.maxCoord, surfelId);
    let centralBox = central_lookup_box(local, radius);
    if (baseCell.region != REGION_CENTER && centralBox.valid != 0u && region_face_overlap(local, radius, baseCell.region)) {
        insert_lookup_box(REGION_CENTER, centralBox.minCoord, centralBox.maxCoord, surfelId);
    }
    for (var region = REGION_POS_X; region <= REGION_NEG_Z; region += 1u) {
        if (region == baseCell.region || !region_face_overlap(local, radius, region)) {
            continue;
        }
        let regionBox = outer_lookup_box(local, radius, region);
        if (regionBox.valid != 0u) {
            insert_lookup_box(region, regionBox.minCoord, regionBox.maxCoord, surfelId);
        }
    }
}

@compute @workgroup_size(128, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let surfelId = globalInvocationId.x;
    if (surfelId >= uniforms.surfelCapacity) {
        return;
    }

    let posR = surfelPosR[surfelId];
    if (posR.w <= 0.0) {
        return;
    }

    let radius = surfel_lookup_radius(surfel_stored_radius(posR));
    insert_surfel(posR.xyz, radius, surfelId);
}
