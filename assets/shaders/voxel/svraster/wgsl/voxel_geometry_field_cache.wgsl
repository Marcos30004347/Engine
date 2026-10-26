struct SparseVoxelGeometryFieldCacheRecord {
    canonicalParent : vec3<i32>,
    materialAndFlags : u32,
    occupancy : array<u32, 16>,
}

const VOXEL_GEOMETRY_FIELD_CACHE_VALID : u32 = 0x80000000u;
const VOXEL_GEOMETRY_FIELD_CACHE_CONSERVATIVE : u32 = 0x40000000u;
const VOXEL_GEOMETRY_FIELD_CACHE_MAXIMUM_PROBES : u32 = 8u;

fn voxelGeometryFieldCacheMix(input : u32) -> u32 {
    var value = input;
    value ^= value >> 16u;
    value *= 0x7feb352du;
    value ^= value >> 15u;
    value *= 0x846ca68bu;
    return value ^ (value >> 16u);
}

fn voxelGeometryFieldCacheHash(
    parent : vec3<i32>, materialHandle : u32) -> u32 {
    var value = voxelGeometryFieldCacheMix(
        materialHandle ^ 0x85ebca6bu);
    value = voxelGeometryFieldCacheMix(
        value ^ bitcast<u32>(parent.x) * 0x8da6b343u);
    value = voxelGeometryFieldCacheMix(
        value ^ bitcast<u32>(parent.y) * 0xd8163841u);
    return voxelGeometryFieldCacheMix(
        value ^ bitcast<u32>(parent.z) * 0xcb1ab31fu);
}

fn findVoxelGeometryFieldCacheRecord(
    parent : vec3<i32>, materialHandle : u32) -> u32 {
    let capacity = arrayLength(&voxelGeometryFieldCache);
    if (capacity == 0u) {
        return 0xffffffffu;
    }
    let start = voxelGeometryFieldCacheHash(parent, materialHandle) % capacity;
    for (var probe = 0u;
        probe < VOXEL_GEOMETRY_FIELD_CACHE_MAXIMUM_PROBES;
        probe += 1u) {
        let index = (start + probe) % capacity;
        // Read members directly from storage. Copying the complete record also
        // copies its fixed array, which is rejected by the Metal translation
        // path even though the WGSL/SPIR-V front end accepts it.
        let materialAndFlags =
            voxelGeometryFieldCache[index].materialAndFlags;
        if ((materialAndFlags &
             VOXEL_GEOMETRY_FIELD_CACHE_VALID) == 0u) {
            return 0xffffffffu;
        }
        if ((materialAndFlags & 0xffffu) == materialHandle &&
            all(voxelGeometryFieldCache[index].canonicalParent == parent)) {
            return index;
        }
    }
    return 0xffffffffu;
}

fn voxelGeometryFieldCacheOccupied(
    parent : vec3<i32>, materialHandle : u32,
    cell : vec3<u32>, subdivision : u32) -> bool {
    let index = findVoxelGeometryFieldCacheRecord(parent, materialHandle);
    if (index == 0xffffffffu) {
        return true;
    }
    let materialAndFlags = voxelGeometryFieldCache[index].materialAndFlags;
    let cachedSubdivision = (materialAndFlags >> 16u) & 0x0fu;
    if ((materialAndFlags &
         VOXEL_GEOMETRY_FIELD_CACHE_CONSERVATIVE) != 0u ||
        cachedSubdivision != subdivision ||
        any(cell >= vec3<u32>(subdivision))) {
        return true;
    }
    let linear =
        (cell.z * subdivision + cell.y) * subdivision + cell.x;
    return (voxelGeometryFieldCache[index].occupancy[linear >> 5u] &
            (1u << (linear & 31u))) != 0u;
}

fn voxelGeometryFieldCacheCoverage(
    parent : vec3<i32>, materialHandle : u32,
    subdivision : u32) -> f32 {
    let index = findVoxelGeometryFieldCacheRecord(parent, materialHandle);
    if (index == 0xffffffffu) {
        return 1.0;
    }
    let materialAndFlags = voxelGeometryFieldCache[index].materialAndFlags;
    let cachedSubdivision = (materialAndFlags >> 16u) & 0x0fu;
    if ((materialAndFlags &
         VOXEL_GEOMETRY_FIELD_CACHE_CONSERVATIVE) != 0u ||
        cachedSubdivision != subdivision) {
        return 1.0;
    }
    let cellCount = subdivision * subdivision * subdivision;
    var occupied = 0u;
    for (var word = 0u; word < 16u; word += 1u) {
        let firstBit = word * 32u;
        if (firstBit >= cellCount) {
            break;
        }
        var bits = voxelGeometryFieldCache[index].occupancy[word];
        let remaining = cellCount - firstBit;
        if (remaining < 32u) {
            bits &= (1u << remaining) - 1u;
        }
        occupied += countOneBits(bits);
    }
    return f32(occupied) / f32(max(cellCount, 1u));
}
