const SPARSE_VOXEL_SKY_GUIDE_TILE_SIZE : u32 = 16u;
const SPARSE_VOXEL_SKY_GUIDE_RADIAL_BINS : u32 = 4u;
const SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS : u32 = 4u;
const SPARSE_VOXEL_SKY_GUIDE_BIN_COUNT : u32 = 16u;
const SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_STRIDE : u32 = 2u;
const SPARSE_VOXEL_SKY_GUIDE_FEEDBACK_RESET_BIT : u32 = 1u << 31u;
const SPARSE_VOXEL_SKY_GUIDE_RADIANCE_SCALE : f32 = 1024.0;

fn sparse_voxel_cosine_hemisphere_basis(
    normalValue : vec3<f32>) -> mat3x3<f32> {
    let normal = sparse_voxel_safe_normalize(
        normalValue, vec3<f32>(0.0, 1.0, 0.0));
    let reference = select(
        vec3<f32>(0.0, 1.0, 0.0), vec3<f32>(1.0, 0.0, 0.0),
        abs(normal.y) > 0.999);
    let tangent = normalize(cross(reference, normal));
    let bitangent = cross(normal, tangent);
    return mat3x3<f32>(tangent, normal, bitangent);
}

fn sparse_voxel_sample_cosine_hemisphere_parameter(
    normalValue : vec3<f32>, parameter : vec2<f32>)
    -> SparseVoxelSkyDirectionSample {
    let normal = sparse_voxel_safe_normalize(
        normalValue, vec3<f32>(0.0, 1.0, 0.0));
    let sample = clamp(parameter, vec2<f32>(0.0), vec2<f32>(0.99999994));
    let radius = sqrt(sample.x);
    let phi = 6.28318530718 * sample.y;
    let local = vec3<f32>(
        radius * cos(phi), sqrt(max(1.0 - sample.x, 0.0)),
        radius * sin(phi));
    let direction = normalize(
        sparse_voxel_cosine_hemisphere_basis(normal) * local);
    return SparseVoxelSkyDirectionSample(
        direction, max(dot(normal, direction), 0.0) / 3.14159265359);
}

// Inverse of sparse_voxel_sample_cosine_hemisphere_parameter(). Keeping the
// guide in this parameter domain makes a bin's PDF an exact constant multiple
// of cos(theta)/pi; no octahedral solid-angle approximation is required.
fn sparse_voxel_cosine_hemisphere_parameter(
    normalValue : vec3<f32>, directionValue : vec3<f32>) -> vec2<f32> {
    let normal = sparse_voxel_safe_normalize(
        normalValue, vec3<f32>(0.0, 1.0, 0.0));
    let direction = sparse_voxel_safe_normalize(directionValue, normal);
    let basis = sparse_voxel_cosine_hemisphere_basis(normal);
    let local = vec3<f32>(
        dot(direction, basis[0]), dot(direction, basis[1]),
        dot(direction, basis[2]));
    let cosine = clamp(local.y, 0.0, 1.0);
    var azimuth = atan2(local.z, local.x) / 6.28318530718;
    if (azimuth < 0.0) { azimuth = azimuth + 1.0; }
    return vec2<f32>(
        clamp(1.0 - cosine * cosine, 0.0, 0.99999994),
        clamp(azimuth, 0.0, 0.99999994));
}

fn sparse_voxel_sky_guide_bin(parameter : vec2<f32>) -> u32 {
    let sample = clamp(parameter, vec2<f32>(0.0), vec2<f32>(0.99999994));
    let radial = min(
        u32(sample.x * f32(SPARSE_VOXEL_SKY_GUIDE_RADIAL_BINS)),
        SPARSE_VOXEL_SKY_GUIDE_RADIAL_BINS - 1u);
    let azimuth = min(
        u32(sample.y * f32(SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS)),
        SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS - 1u);
    return radial * SPARSE_VOXEL_SKY_GUIDE_AZIMUTH_BINS + azimuth;
}
