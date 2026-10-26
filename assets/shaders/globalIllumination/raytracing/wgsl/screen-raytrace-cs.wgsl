struct ScreenRayTraceUniforms {
    viewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    hzbSize : vec2<u32>,
    controls : vec4<u32>,
    dispatchParams : vec4<u32>,
    modeParams : vec4<u32>,
    traceParams : vec4<f32>,
};

struct TraceRay {
    originTMin : vec4<f32>,
    directionTMax : vec4<f32>,
};

struct ScreenTraceHit {
    metadata : vec4<u32>,
    hit : vec4<f32>,
};

struct ProjectedPoint {
    pixel : vec2<i32>,
    depth : f32,
    uv : vec2<f32>,
    valid : bool,
};

const SCREEN_TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;

@group(0) @binding(0) var<uniform> uniforms : ScreenRayTraceUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var hzbTexture : texture_2d<f32>;
@group(0) @binding(3) var<storage, read_write> rays : array<TraceRay>;
@group(0) @binding(4) var<storage, read_write> screenHits : array<ScreenTraceHit>;

fn invalid_trace_ray() -> TraceRay {
    return TraceRay(vec4<f32>(0.0), vec4<f32>(0.0));
}

fn invalid_screen_hit() -> ScreenTraceHit {
    return ScreenTraceHit(vec4<u32>(0u), vec4<f32>(0.0));
}

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, uniforms.controls.y != 0u);
}

fn depth_hit(rayDepth : f32, sceneDepth : f32) -> bool {
    if (!valid_depth(sceneDepth)) {
        return false;
    }

    let delta = select(rayDepth - sceneDepth, sceneDepth - rayDepth, uniforms.controls.y != 0u);
    return delta >= 0.0 && delta <= uniforms.traceParams.x;
}

fn project_world_position(worldPosition : vec3<f32>) -> ProjectedPoint {
    let clip = uniforms.viewProjection * vec4<f32>(worldPosition, 1.0);
    let invW = select(1.0, 1.0 / clip.w, abs(clip.w) > 1.0e-6);
    let ndc = clip.xyz * invW;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    var result : ProjectedPoint;
    result.uv = uv;
    result.depth = ndc.z;
    result.pixel = vec2<i32>(floor(uv * vec2<f32>(uniforms.viewport)));
    result.valid = clip.w > 0.0 && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0 && ndc.z >= 0.0 && ndc.z <= 1.0;
    return result;
}

fn choose_mip(stepIndex : u32) -> u32 {
    let coarse = stepIndex / 4u;
    return min(max(uniforms.controls.z, 1u) - 1u, coarse);
}

fn sample_hzb(projected : ProjectedPoint, mipLevel : u32) -> f32 {
    let mipSize = max(vec2<u32>(1u, 1u), uniforms.hzbSize >> vec2<u32>(mipLevel, mipLevel));
    let coord = clamp(vec2<i32>(floor(projected.uv * vec2<f32>(mipSize))), vec2<i32>(0, 0), vec2<i32>(mipSize) - vec2<i32>(1, 1));
    return textureLoad(hzbTexture, coord, i32(mipLevel)).r;
}

fn trace_ray_is_active(ray : TraceRay) -> bool {
    let direction = ray.directionTMax.xyz;
    return !any(direction != direction) &&
        !any(abs(direction) > vec3<f32>(1.0e20)) &&
        dot(direction, direction) > 1.0e-12 &&
        ray.originTMin.w < ray.directionTMax.w;
}

fn trace_screen_ray(ray : TraceRay) -> ScreenTraceHit {
    let origin = ray.originTMin.xyz;
    let direction = normalize(ray.directionTMax.xyz);
    let traceMin = max(ray.originTMin.w, 0.0);
    let traceMax = min(ray.directionTMax.w, uniforms.traceParams.y);
    if (traceMax <= traceMin) {
        return invalid_screen_hit();
    }

    let maxSteps = max(uniforms.controls.w, 1u);
    let stepLength = (traceMax - traceMin) / f32(maxSteps);
    for (var stepIndex = 0u; stepIndex < maxSteps; stepIndex++) {
        let rayDistance = traceMin + stepLength * f32(stepIndex + 1u);
        let projected = project_world_position(origin + direction * rayDistance);
        if (!projected.valid) {
            return invalid_screen_hit();
        }

        let mipLevel = choose_mip(stepIndex);
        let hzbDepth = sample_hzb(projected, mipLevel);
        if (!depth_hit(projected.depth, hzbDepth)) {
            continue;
        }

        let fullResDepth = textureLoad(sceneDepthTexture, projected.pixel, 0);
        if (depth_hit(projected.depth, fullResDepth)) {
            return ScreenTraceHit(
                vec4<u32>(
                    SCREEN_TRACE_HIT_FLAG_HIT,
                    u32(projected.pixel.x),
                    u32(projected.pixel.y),
                    stepIndex + 1u),
                vec4<f32>(rayDistance, fullResDepth, 0.0, 0.0));
        }
    }

    return invalid_screen_hit();
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let rayIndex = gid.x;
    if (rayIndex >= uniforms.dispatchParams.x || rayIndex >= arrayLength(&rays) || rayIndex >= arrayLength(&screenHits)) {
        return;
    }

    screenHits[rayIndex] = invalid_screen_hit();
    if (uniforms.controls.x == 0u) {
        return;
    }

    let ray = rays[rayIndex];
    if (!trace_ray_is_active(ray)) {
        return;
    }

    let hit = trace_screen_ray(ray);
    screenHits[rayIndex] = hit;
    if ((hit.metadata.x & SCREEN_TRACE_HIT_FLAG_HIT) != 0u && uniforms.modeParams.y != 0u) {
        rays[rayIndex] = invalid_trace_ray();
    }
}
