// Shared city material address and exact appearance decode. The six storage
// arrays are bound by the including pass; geometry/appearance indices live in
// InstanceData._padding0/_padding1. All byte reads are little endian.
struct CityGeometry { firstBrick: u32, brickCount: u32, pitchMicrometres: u32, reserved: u32 }
struct CityBrick { key: vec3<i32>, baseRank: u32, occupancy: array<u32, 16> }
struct CityAppearance { byteOffset: u32, byteCount: u32, codec: u32, occupiedCount: u32 }
struct CityClusterSource { origin: vec3<i32>, cellScale: u32 }
struct CityProfileSlot { profileId: u32, materialIndex: u32 }
struct CitySurfaceValue { valid: u32, rgba: u32, profileId: u32 }

fn cityReadByte(offset: u32) -> u32 {
    let wordIndex = offset >> 2u;
    if (wordIndex >= arrayLength(&cityAppearanceWords)) { return 0u; }
    return (cityAppearanceWords[wordIndex] >> ((offset & 3u) * 8u)) & 255u;
}
fn cityReadU32(offset: u32) -> u32 {
    return cityReadByte(offset) | (cityReadByte(offset + 1u) << 8u) |
           (cityReadByte(offset + 2u) << 16u) | (cityReadByte(offset + 3u) << 24u);
}
fn cityKeyLess(a: vec3<i32>, b: vec3<i32>) -> bool {
    if (a.x != b.x) { return a.x < b.x; }
    if (a.y != b.y) { return a.y < b.y; }
    return a.z < b.z;
}
// Rank of an occupied cell in its geometry's brick stream, or 0xffffffff.
fn cityCellRank(geometry: CityGeometry, cell: vec3<i32>) -> u32 {
    if (any(cell < vec3<i32>(0))) { return 0xffffffffu; }
    let key = cell / 8;
    var low = geometry.firstBrick;
    var high = geometry.firstBrick + geometry.brickCount;
    if (high > arrayLength(&cityBricks)) { return 0xffffffffu; }
    while (low < high) {
        let middle = low + (high - low) / 2u;
        if (cityKeyLess(cityBricks[middle].key, key)) { low = middle + 1u; }
        else { high = middle; }
    }
    if (low >= geometry.firstBrick + geometry.brickCount ||
        any(cityBricks[low].key != key)) { return 0xffffffffu; }
    let within = cell & vec3<i32>(7);
    let bit = u32(within.x + within.y * 8 + within.z * 64);
    let wordIndex = bit >> 5u;
    let mask = 1u << (bit & 31u);
    if ((cityBricks[low].occupancy[wordIndex] & mask) == 0u) { return 0xffffffffu; }
    var rank = cityBricks[low].baseRank;
    for (var i = 0u; i < wordIndex; i++) {
        rank += countOneBits(cityBricks[low].occupancy[i]);
    }
    return rank + countOneBits(cityBricks[low].occupancy[wordIndex] & (mask - 1u));
}

// Lattice clusters (welded-surface VG): cellScale carries this bit. The cell
// on the view's plane is the position over the pitch along the plane's axes -
// the meshes store no UVs - and a coarse surface may sit up to a few voxels
// off the voxels it stands for.
const CITY_LATTICE_CLUSTER: u32 = 0x80000000u;
const CITY_LATTICE_REACH: i32 = 8;

fn cityExactValue(geometryIndex: u32, appearanceIndex: u32,
                  globalCluster: u32, localPosition: vec3<f32>,
                  localNormal: vec3<f32>, uv: vec2<f32>) -> CitySurfaceValue {
    let empty = CitySurfaceValue(0u, 0u, 0u);
    if (geometryIndex >= arrayLength(&cityGeometries) ||
        appearanceIndex >= arrayLength(&cityAppearances) ||
        globalCluster >= arrayLength(&cityClusterSources)) { return empty; }
    let source = cityClusterSources[globalCluster];
    let geometry = cityGeometries[geometryIndex];
    let appearance = cityAppearances[appearanceIndex];
    if (source.cellScale == 0u || geometry.pitchMicrometres == 0u ||
        geometry.brickCount == 0u || appearance.occupiedCount == 0u) { return empty; }
    let normal = abs(localNormal);
    var axis = 0u;
    if (normal.y > normal.x) { axis = 1u; }
    if (normal.z > normal[axis]) { axis = 2u; }
    if (normal[axis] < 0.5) { return empty; }
    let positive = localNormal[axis] > 0.0;
    let direction = axis * 2u + select(0u, 1u, positive);
    var uAxis = 0u;
    var vAxis = 1u;
    if (direction < 2u) { uAxis = 1u; vAxis = 2u; }
    else if (direction < 4u) { uAxis = 2u; vAxis = 0u; }
    else { uAxis = 0u; vAxis = 1u; }
    let pitch = f32(geometry.pitchMicrometres) * 0.000001;
    var rank = 0xffffffffu;
    if ((source.cellScale & CITY_LATTICE_CLUSTER) != 0u) {
        // The cell just inside the surface; if that is empty the surface
        // floats above the voxels, so walk inward to the first solid cell;
        // if it is solid the surface is sunk, so walk outward to the last.
        var cell = vec3<i32>(0);
        cell[uAxis] = i32(floor(localPosition[uAxis] / pitch));
        cell[vAxis] = i32(floor(localPosition[vAxis] / pitch));
        let depth = localPosition[axis] / pitch;
        cell[axis] = i32(floor(depth + select(0.5, -0.5, positive)));
        let outward = select(-1, 1, positive);
        rank = cityCellRank(geometry, cell);
        if (rank == 0xffffffffu) {
            for (var k = 1; k <= CITY_LATTICE_REACH; k++) {
                cell[axis] -= outward;
                rank = cityCellRank(geometry, cell);
                if (rank != 0xffffffffu) { break; }
            }
        } else {
            for (var k = 1; k <= CITY_LATTICE_REACH; k++) {
                var next = cell;
                next[axis] += outward;
                let nextRank = cityCellRank(geometry, next);
                if (nextRank == 0xffffffffu) { break; }
                cell = next;
                rank = nextRank;
            }
        }
    } else {
        let scale = f32(source.cellScale);
        let plane = round((localPosition[axis] / pitch - f32(source.origin[axis])) / scale);
        if (plane < 0.0 || plane > 8.0) { return empty; }
        var local = vec3<i32>(0);
        local[uAxis] = i32(clamp(floor(uv.x), 0.0, 7.0));
        local[vAxis] = i32(clamp(floor(uv.y), 0.0, 7.0));
        local[axis] = i32(plane) - select(0, 1, positive);
        if (local[axis] < 0 || local[axis] >= 8) { return empty; }
        rank = cityCellRank(geometry, source.origin + local * i32(source.cellScale));
    }
    if (rank == 0xffffffffu || rank >= appearance.occupiedCount) { return empty; }
    let start = appearance.byteOffset;
    let end = start + appearance.byteCount;
    var valueAt = start;
    if (appearance.codec == 0u) {
        valueAt = start + rank * 8u;
    } else if (appearance.codec == 2u || appearance.codec == 3u) {
        let paletteCount = cityReadU32(start);
        let indices = start + 4u + paletteCount * 8u;
        let palette = select(cityReadByte(indices + rank),
                             cityReadByte(indices + rank * 2u) |
                             (cityReadByte(indices + rank * 2u + 1u) << 8u),
                             appearance.codec == 3u);
        if (palette >= paletteCount) { return empty; }
        valueAt = start + 4u + palette * 8u;
    } else if (appearance.codec == 4u) {
        let overrideCount = cityReadU32(start + 8u);
        var left = 0u;
        var right = overrideCount;
        while (left < right) {
            let middle = left + (right - left) / 2u;
            if (cityReadU32(start + 12u + middle * 12u) < rank) { left = middle + 1u; }
            else { right = middle; }
        }
        if (left < overrideCount && cityReadU32(start + 12u + left * 12u) == rank) {
            valueAt = start + 16u + left * 12u;
        }
    } else { return empty; }
    if (valueAt + 8u > end || end < start) { return empty; }
    return CitySurfaceValue(1u, cityReadU32(valueAt), cityReadU32(valueAt + 4u));
}

fn cityMaterialIndex(profileId: u32, fallbackIndex: u32) -> u32 {
    var left = 0u;
    var right = arrayLength(&cityProfileSlots);
    while (left < right) {
        let middle = left + (right - left) / 2u;
        if (cityProfileSlots[middle].profileId < profileId) { left = middle + 1u; }
        else { right = middle; }
    }
    if (left < arrayLength(&cityProfileSlots) && cityProfileSlots[left].profileId == profileId) {
        return cityProfileSlots[left].materialIndex;
    }
    return fallbackIndex;
}

fn cityRGBAFloat(rgba: u32) -> vec4<f32> {
    return vec4<f32>(f32(rgba & 255u), f32((rgba >> 8u) & 255u),
                     f32((rgba >> 16u) & 255u), f32((rgba >> 24u) & 255u)) / 255.0;
}
