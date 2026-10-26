fn atmosphere_terrain_shadow_basis_x(
    up : vec3<f32>, directionTowardSun : vec3<f32>) -> vec3<f32> {
    let fallbackAxis = select(vec3<f32>(1.0, 0.0, 0.0),
        vec3<f32>(0.0, 0.0, 1.0), abs(up.z) < 0.9);
    return atmosphere_safe_normalize(cross(up, directionTowardSun),
        atmosphere_safe_normalize(cross(up, fallbackAxis),
            vec3<f32>(1.0, 0.0, 0.0)));
}

fn atmosphere_terrain_shadow_tap(shadowMap : texture_depth_2d,
    texel : vec2<i32>, receiverDepth : f32, bias : f32) -> f32 {
    let storedDepth = textureLoad(shadowMap, texel, 0);
    // Zero is the persistent map's clear value and means no local-cap
    // coverage. Missing coverage must fall back to PlanetOnly (fully lit here).
    if (storedDepth <= 0.0) { return 1.0; }
    return select(1.0, 0.0, storedDepth > receiverDepth + bias);
}

// control: x enabled, y half extent km, z half depth range km, w bias km.
// fade: x minimum sun cosine, y fully enabled cosine, z edge start, w strength.
fn atmosphere_terrain_shadow_visibility(shadowMap : texture_depth_2d,
    relativePosition : vec3<f32>, upValue : vec3<f32>,
    directionTowardSunValue : vec3<f32>, control : vec4<f32>,
    fade : vec4<f32>) -> f32 {
    if (control.x < 0.5) { return 1.0; }
    let up = atmosphere_safe_normalize(upValue, vec3<f32>(0.0, 1.0, 0.0));
    let directionTowardSun = atmosphere_safe_normalize(
        directionTowardSunValue, up);
    let extent = max(control.y, 1.0e-4);
    let depthRange = max(control.z, 1.0e-4);
    let shadowX = atmosphere_terrain_shadow_basis_x(up, directionTowardSun);
    let shadowY = atmosphere_safe_normalize(
        cross(directionTowardSun, shadowX), up);
    let projected = vec2<f32>(dot(relativePosition, shadowX),
        dot(relativePosition, shadowY)) / extent;
    if (any(abs(projected) >= vec2<f32>(1.0))) { return 1.0; }

    let sunCosine = dot(up, directionTowardSun);
    let sunWeight = smoothstep(clamp(fade.x, 0.0, 1.0),
        max(clamp(fade.y, 0.0, 1.0), fade.x + 1.0e-4), sunCosine);
    let edgeWeight = 1.0 - smoothstep(clamp(fade.z, 0.0, 0.999), 1.0,
        length(projected));
    let weight = clamp(sunWeight * edgeWeight * fade.w, 0.0, 1.0);
    if (weight <= 0.0) { return 1.0; }

    let dimensions = max(textureDimensions(shadowMap), vec2<u32>(1u));
    let pixel = (projected * 0.5 + 0.5) * vec2<f32>(dimensions) - 0.5;
    let base = vec2<i32>(floor(pixel));
    let maximum = vec2<i32>(dimensions) - vec2<i32>(1);
    let receiverDepth = 0.5 +
        dot(relativePosition, directionTowardSun) / (2.0 * depthRange);
    if (receiverDepth <= 0.0 || receiverDepth >= 1.0) { return 1.0; }
    let normalizedBias = max(control.w / (2.0 * depthRange), 0.0);
    // A continuously weighted four-texel-wide tent replaces the old one-cell
    // bilinear comparison. At a 50 km map extent one texel can cover hundreds
    // of metres; filtering only that square projected the depth-map grid into
    // aerial perspective as long rectangular shadow bands. This kernel is
    // still cheap because it runs in the low-resolution atmosphere LUTs, not
    // as a full-resolution per-pixel shadow pass.
    var visibility = 0.0;
    var totalWeight = 0.0;
    for (var y = -1; y <= 2; y += 1) {
        let wy = max(2.0 - abs(f32(base.y + y) - pixel.y), 0.0);
        if (wy <= 0.0) { continue; }
        for (var x = -1; x <= 2; x += 1) {
            let wx = max(2.0 - abs(f32(base.x + x) - pixel.x), 0.0);
            let tapWeight = wx * wy;
            if (tapWeight <= 0.0) { continue; }
            let texel = clamp(base + vec2<i32>(x, y),
                vec2<i32>(0), maximum);
            visibility += atmosphere_terrain_shadow_tap(
                shadowMap, texel, receiverDepth, normalizedBias) * tapWeight;
            totalWeight += tapWeight;
        }
    }
    visibility /= max(totalWeight, 1.0e-6);
    return mix(1.0, visibility, weight);
}
