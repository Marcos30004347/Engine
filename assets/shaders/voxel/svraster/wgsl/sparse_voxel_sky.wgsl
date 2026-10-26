fn sparse_voxel_safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let valueLength = length(value);
    return select(fallback, value / max(valueLength, 1.0e-6), valueLength > 1.0e-6);
}

fn sparse_voxel_sky_up(light : WorldGILight) -> vec3<f32> {
    return sparse_voxel_safe_normalize(light.directionType.xyz, vec3<f32>(0.0, 1.0, 0.0));
}

fn sparse_voxel_sky_radiance(light : WorldGILight, direction : vec3<f32>) -> vec3<f32> {
    // Bit 1 is WorldGILightFlag_AtmosphereSH. Use the payload bit directly so
    // this shared helper also compiles in the lightweight debug presenter,
    // which declares WorldGILight without importing every WorldGI constant.
    if ((light.flags.x & 2u) != 0u) {
        let value = light.positionRange.xyz * 0.2820947918 +
            light.directionType.xyz * (0.4886025119 * direction.x) +
            light.shapeParams.xyz * (0.4886025119 * direction.y) +
            light.colorIntensity.xyz * (0.4886025119 * direction.z);
        return max(value, vec3<f32>(0.0)) *
            max(light.colorIntensity.w, 0.0);
    }
    let up = sparse_voxel_sky_up(light);
    let elevation = clamp(dot(sparse_voxel_safe_normalize(direction, up), up), -1.0, 1.0);
    let exponent = max(light.shapeParams.w, 0.01);
    let horizon = max(light.positionRange.xyz, vec3<f32>(0.0));
    let zenith = max(light.colorIntensity.xyz, vec3<f32>(0.0));
    let ground = max(light.shapeParams.xyz, vec3<f32>(0.0));
    let destinationColor = select(ground, zenith, elevation >= 0.0);
    return mix(horizon, destinationColor, pow(abs(elevation), exponent)) *
        max(light.colorIntensity.w, 0.0);
}

fn sparse_voxel_sky_average_luminance(light : WorldGILight) -> f32 {
    if ((light.flags.x & 2u) != 0u) {
        let meanRadiance = max(light.positionRange.xyz * 0.2820947918,
            vec3<f32>(0.0));
        return dot(meanRadiance, vec3<f32>(0.2126, 0.7152, 0.0722)) *
            max(light.colorIntensity.w, 0.0);
    }
    let averageColor = (max(light.positionRange.xyz, vec3<f32>(0.0)) +
        max(light.colorIntensity.xyz, vec3<f32>(0.0)) +
        max(light.shapeParams.xyz, vec3<f32>(0.0))) / 3.0;
    return dot(averageColor, vec3<f32>(0.2126, 0.7152, 0.0722)) *
        max(light.colorIntensity.w, 0.0);
}

struct SparseVoxelSkyDirectionSample {
    direction : vec3<f32>,
    pdf : f32,
}

fn sparse_voxel_sample_cosine_hemisphere(
    normalValue : vec3<f32>, random0 : f32, random1 : f32) -> SparseVoxelSkyDirectionSample {
    let normal = sparse_voxel_safe_normalize(normalValue, vec3<f32>(0.0, 1.0, 0.0));
    let radius = sqrt(clamp(random0, 0.0, 1.0));
    let phi = 6.28318530718 * random1;
    let local = vec3<f32>(radius * cos(phi), sqrt(max(1.0 - random0, 0.0)), radius * sin(phi));
    let reference = select(vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0), abs(normal.y) > 0.999);
    let tangent = normalize(cross(reference, normal));
    let bitangent = cross(normal, tangent);
    let direction = normalize(tangent * local.x + normal * local.y + bitangent * local.z);
    return SparseVoxelSkyDirectionSample(direction, max(dot(normal, direction), 0.0) / 3.14159265359);
}
