// Compact ReGIR-style validation key for a receiver.  Only enabled lights
// whose influence volume contains the receiver contribute, so editing a
// finite light wakes nearby pixels without discarding settled lighting in an
// unrelated screen region. Infinite sky/directional lights intentionally
// contribute everywhere. The renderer binds at most eight light records.

fn sparse_voxel_local_light_hash_mix(value : u32) -> u32 {
    var result = value;
    result = (result ^ (result >> 16u)) * 0x7feb352du;
    result = (result ^ (result >> 15u)) * 0x846ca68bu;
    return result ^ (result >> 16u);
}

fn sparse_voxel_local_light_enabled(light : WorldGILight) -> bool {
    return (light.flags.x & WORLD_GI_LIGHT_FLAG_ENABLED) != 0u &&
        light.flags.y != WORLD_GI_LIGHT_TYPE_INVALID &&
        light.colorIntensity.w > 0.0;
}

fn sparse_voxel_local_light_affects(
    light : WorldGILight, position : vec3<f32>) -> bool {
    if (!sparse_voxel_local_light_enabled(light)) { return false; }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_SKY ||
        light.flags.y == WORLD_GI_LIGHT_TYPE_DIRECTIONAL) {
        return true;
    }
    if (light.flags.y == WORLD_GI_LIGHT_TYPE_PLANAR) {
        let closest = worldgi_planar_light_closest_point(light, position);
        return distance(closest, position) <= light.positionRange.w;
    }
    let emitterRadius = select(
        0.0, max(light.shapeParams.x, 0.0),
        light.flags.y == WORLD_GI_LIGHT_TYPE_SPHERE);
    return distance(light.positionRange.xyz, position) <=
        max(light.positionRange.w, 0.0) + emitterRadius;
}

fn sparse_voxel_local_light_hash(
    position : vec3<f32>, lightCapacity : u32) -> u32 {
    let count = min(min(lightCapacity, 8u), arrayLength(&lights));
    var hash = 0x9e3779b9u;
    var relevantCount = 0u;
    for (var index = 0u; index < count; index = index + 1u) {
        let light = lights[index];
        if (!sparse_voxel_local_light_affects(light, position)) { continue; }
        // flags.z is the stable light handle and flags.w is a content revision
        // for this light alone (not the global lighting revision).
        let signature = sparse_voxel_local_light_hash_mix(
            light.flags.z ^ (light.flags.y * 0x85ebca6bu) ^
            (light.flags.w * 0xc2b2ae35u));
        hash = sparse_voxel_local_light_hash_mix(hash ^ signature);
        relevantCount = relevantCount + 1u;
    }
    hash = sparse_voxel_local_light_hash_mix(hash ^ relevantCount);
    // Zero denotes an uninitialised history texture.
    return select(hash, 1u, hash == 0u);
}
