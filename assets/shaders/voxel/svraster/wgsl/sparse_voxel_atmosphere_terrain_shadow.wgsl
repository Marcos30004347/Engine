struct AtmosphereTerrainShadowUniforms {
    tangentX : vec4<f32>,
    tangentUp : vec4<f32>,
    tangentZ : vec4<f32>,
    // xyz is the anchor surface point relative to the camera.
    tangentCamera : vec4<f32>,
    // x source planet radius, y half map extent, z half depth range (metres).
    sourceRadiusExtent : vec4<f32>,
    directionTowardSun : vec4<f32>,
    localUp : vec4<f32>,
}

@group(0) @binding(0) var<uniform> uniforms : AtmosphereTerrainShadowUniforms;

fn terrain_shadow_safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let lengthSquared = dot(value, value);
    return select(fallback, value * inverseSqrt(lengthSquared),
        lengthSquared > 1.0e-12);
}

@vertex
fn clearAtmosphereTerrainShadow(
    @builtin(vertex_index) vertexIndex : u32) -> @builtin(position) vec4<f32> {
    let corners = array<vec2<f32>, 3>(
        vec2<f32>(-1.0, -1.0), vec2<f32>(3.0, -1.0),
        vec2<f32>(-1.0, 3.0));
    return vec4<f32>(corners[vertexIndex], 0.0, 1.0);
}

@vertex
fn drawAtmosphereTerrainShadow(
    @location(0) directionElevation : vec4<f32>,
    @location(1) surfaceCoordinate : vec4<f32>) -> @builtin(position) vec4<f32> {
    let direction = terrain_shadow_safe_normalize(directionElevation.xyz,
        uniforms.tangentUp.xyz);
    // Convert the package's planet-space direction to the renderer's local
    // tangent frame without using the visually compressed background radius.
    let tangentDirection = vec3<f32>(
        dot(direction, uniforms.tangentX.xyz),
        dot(direction, uniforms.tangentUp.xyz),
        dot(direction, uniforms.tangentZ.xyz));
    let sourceRadius = max(uniforms.sourceRadiusExtent.x, 1.0);
    let radius = sourceRadius + directionElevation.w;
    let relativePosition = uniforms.tangentCamera.xyz + vec3<f32>(
        tangentDirection.x * radius,
        tangentDirection.y * radius - sourceRadius,
        tangentDirection.z * radius);

    let up = terrain_shadow_safe_normalize(uniforms.localUp.xyz,
        vec3<f32>(0.0, 1.0, 0.0));
    let towardSun = terrain_shadow_safe_normalize(
        uniforms.directionTowardSun.xyz, up);
    let fallbackAxis = select(vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 0.0, 1.0), abs(up.z) < 0.9);
    let shadowX = terrain_shadow_safe_normalize(cross(up, towardSun),
        terrain_shadow_safe_normalize(cross(up, fallbackAxis),
            vec3<f32>(1.0, 0.0, 0.0)));
    let shadowY = terrain_shadow_safe_normalize(cross(towardSun, shadowX), up);
    let extent = max(uniforms.sourceRadiusExtent.y, 1.0);
    let depthRange = max(uniforms.sourceRadiusExtent.z, 1.0);
    let projected = vec2<f32>(dot(relativePosition, shadowX),
        dot(relativePosition, shadowY)) / extent;
    // Reverse depth: the largest projection toward the sun is the nearest
    // terrain occluder for a ray travelling from the atmosphere to the sun.
    let reverseDepth = clamp(0.5 +
        dot(relativePosition, towardSun) / (2.0 * depthRange), 0.0, 1.0);
    return vec4<f32>(projected, reverseDepth, 1.0);
}

@fragment
fn writeAtmosphereTerrainShadow(
    @builtin(position) fragmentPosition : vec4<f32>) -> @builtin(frag_depth) f32 {
    return fragmentPosition.z;
}
