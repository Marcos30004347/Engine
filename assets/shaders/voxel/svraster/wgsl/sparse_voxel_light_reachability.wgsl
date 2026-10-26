// Persistent, world-space light reachability cache shared by candidate
// generation and the light-field feedback path.
//
// Each entry is:
//   generation, region key, packed light statistics[light capacity]
// where a packed statistic contains total observations in the low 16 bits and
// visible observations in the high 16 bits. The dominant receiver-normal face
// is part of the key so opposite sides of a thin wall never share visibility.

const SPARSE_VOXEL_LIGHT_REACHABILITY_PROBES : u32 = 4u;
const SPARSE_VOXEL_LIGHT_REACHABILITY_LOCKED : u32 = 0xffffffffu;

fn sparse_voxel_reachability_generation(value : u32) -> u32 {
    if (value == 0u) { return 1u; }
    if (value == SPARSE_VOXEL_LIGHT_REACHABILITY_LOCKED) {
        return SPARSE_VOXEL_LIGHT_REACHABILITY_LOCKED - 1u;
    }
    return value;
}

fn sparse_voxel_reachability_mix(value : u32) -> u32 {
    var result = value;
    result = (result ^ (result >> 16u)) * 0x7feb352du;
    result = (result ^ (result >> 15u)) * 0x846ca68bu;
    return result ^ (result >> 16u);
}

fn sparse_voxel_reachability_face(normal : vec3<f32>) -> u32 {
    let absoluteNormal = abs(normal);
    if (absoluteNormal.x >= absoluteNormal.y &&
        absoluteNormal.x >= absoluteNormal.z) {
        return select(0u, 1u, normal.x >= 0.0);
    }
    if (absoluteNormal.y >= absoluteNormal.z) {
        return select(2u, 3u, normal.y >= 0.0);
    }
    return select(4u, 5u, normal.z >= 0.0);
}

fn sparse_voxel_reachability_key(
    position : vec3<f32>, normal : vec3<f32>, regionSize : f32) -> u32 {
    let cell = vec3<i32>(floor(position / max(regionSize, 1.0e-4)));
    var hash = sparse_voxel_reachability_mix(bitcast<u32>(cell.x));
    hash = sparse_voxel_reachability_mix(
        hash ^ bitcast<u32>(cell.y) * 0x9e3779b9u);
    hash = sparse_voxel_reachability_mix(
        hash ^ bitcast<u32>(cell.z) * 0x85ebca6bu);
    hash = sparse_voxel_reachability_mix(
        hash ^ sparse_voxel_reachability_face(normal) * 0xc2b2ae35u);
    // Zero is reserved for uninitialised memory in debugging/readback tools.
    return select(hash, 1u, hash == 0u);
}

fn sparse_voxel_reachability_probe(
    key : u32, probe : u32, entryCount : u32) -> u32 {
    let step = (sparse_voxel_reachability_mix(key ^ 0x27d4eb2du) | 1u);
    return (key + probe * step) % max(entryCount, 1u);
}

fn sparse_voxel_reachability_visibility(
    packed : u32, explorationFloor : f32) -> f32 {
    let total = f32(packed & 0xffffu);
    let visible = f32(packed >> 16u);
    if (total <= 0.0) { return 1.0; }
    // A uniform Beta prior remains optimistic enough to recover from one
    // unlucky observation without spending many frames selecting a light
    // that is consistently blocked. The previous fully-visible prior used
    // (visible + 2) / (total + 2), so even ten blocked samples retained 1/6
    // of the selection probability and made sky-only regions alternate
    // between black finite-light samples and strongly weighted sky samples.
    let estimate = (visible + 1.0) / (total + 2.0);
    return clamp(max(estimate, explorationFloor), 0.0, 1.0);
}
