struct WorldGILightGridDebugUniforms {
    inverseViewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    enabled : u32,
    reverseZ : u32,
    gridResolution : u32,
    cascadeCount : u32,
    selectedCascade : i32,
    lineWidth : f32,
    gridCenterBaseExtent : vec4<f32>,
    cascadeScalePadding : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGILightGridDebugUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var outputTexture : texture_storage_2d<rgba16float, write>;

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, uniforms.reverseZ != 0u);
}

fn reconstruct_world_position(pixelCoord : vec2<u32>, depthValue : f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixelCoord) + vec2<f32>(0.5)) / vec2<f32>(uniforms.viewport);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = uniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn cascade_color(cascadeIndex : u32) -> vec3<f32> {
    switch (cascadeIndex % 6u) {
        case 0u: { return vec3<f32>(0.10, 0.85, 1.00); }
        case 1u: { return vec3<f32>(0.25, 1.00, 0.30); }
        case 2u: { return vec3<f32>(1.00, 0.85, 0.12); }
        case 3u: { return vec3<f32>(1.00, 0.35, 0.15); }
        case 4u: { return vec3<f32>(0.75, 0.35, 1.00); }
        default: { return vec3<f32>(1.00, 0.25, 0.75); }
    }
}

fn hash_u32(value : u32) -> u32 {
    var x = value;
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}

fn cell_color(cascadeIndex : u32, cellCoord : vec3<u32>) -> vec3<f32> {
    let seed = hash_u32(cascadeIndex * 1664525u + cellCoord.x * 73856093u + cellCoord.y * 19349663u + cellCoord.z * 83492791u);
    let randomColor = vec3<f32>(
        f32((seed >> 0u) & 255u) / 255.0,
        f32((seed >> 8u) & 255u) / 255.0,
        f32((seed >> 16u) & 255u) / 255.0);
    return mix(cascade_color(cascadeIndex), randomColor, 0.35);
}

fn grid_overlay_for_position(position : vec3<f32>) -> vec4<f32> {
    let resolution = max(uniforms.gridResolution, 1u);
    let cascadeCount = max(uniforms.cascadeCount, 1u);
    let lineWidth = clamp(uniforms.lineWidth, 0.001, 0.20);

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex = cascadeIndex + 1u) {
        if (uniforms.selectedCascade >= 0 && cascadeIndex != u32(uniforms.selectedCascade)) {
            continue;
        }

        let extent = max(uniforms.gridCenterBaseExtent.w, 1.0e-3) *
            pow(max(uniforms.cascadeScalePadding.x, 1.0), f32(cascadeIndex));
        let cascadeMin = uniforms.gridCenterBaseExtent.xyz - vec3<f32>(extent * 0.5);
        let local = position - cascadeMin;
        if (!all(local >= vec3<f32>(0.0)) || !all(local < vec3<f32>(extent))) {
            continue;
        }

        let cellCoordFloat = local / (extent / f32(resolution));
        let cellCoord = vec3<u32>(clamp(floor(cellCoordFloat), vec3<f32>(0.0), vec3<f32>(f32(resolution - 1u))));
        let cellFrac = fract(cellCoordFloat);
        let edgeDistance = min(min(cellFrac.x, 1.0 - cellFrac.x), min(min(cellFrac.y, 1.0 - cellFrac.y), min(cellFrac.z, 1.0 - cellFrac.z)));
        let line = 1.0 - smoothstep(lineWidth, lineWidth * 1.75, edgeDistance);
        let baseColor = cell_color(cascadeIndex, cellCoord);
        let edgeColor = mix(baseColor, vec3<f32>(1.0), 0.65);
        return vec4<f32>(mix(baseColor * 0.35, edgeColor, line), 1.0);
    }

    return vec4<f32>(1.0, 0.38, 0.05, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.viewport.x || gid.y >= uniforms.viewport.y) {
        return;
    }

    let pixel = vec2<u32>(gid.xy);
    var outColor = vec4<f32>(0.0);
    if (uniforms.enabled != 0u) {
        let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(pixel), 0);
        if (valid_depth(depthValue)) {
            let worldPosition = reconstruct_world_position(pixel, depthValue);
            outColor = grid_overlay_for_position(worldPosition);
        } else {
            outColor = vec4<f32>(0.85, 0.05, 1.0, 1.0);
        }
    }

    textureStore(outputTexture, vec2<i32>(pixel), outColor);
}
