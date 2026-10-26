struct TraceRay {
    origin_tmin: vec4<f32>,
    direction_tmax: vec4<f32>,
};

struct TraceHit {
    instanceId: u32,
    triangleId: u32,
    hitT: f32,
    baryU: f32,
    baryV: f32,
    tlasInstanceIndex: u32,
    flags: u32,
    _padding0: u32,
};

fn invalid_trace_hit() -> TraceHit {
    return TraceHit(INVALID_INDEX, INVALID_INDEX, 0.0, 0.0, 0.0, INVALID_INDEX, 0u, 0u);
}

fn trace_ray_origin(ray: TraceRay) -> vec3<f32> {
    return ray.origin_tmin.xyz;
}

fn trace_ray_direction(ray: TraceRay) -> vec3<f32> {
    return ray.direction_tmax.xyz;
}

fn trace_ray_tmin(ray: TraceRay) -> f32 {
    return ray.origin_tmin.w;
}

fn trace_ray_tmax(ray: TraceRay) -> f32 {
    return ray.direction_tmax.w;
}

fn trace_ray_is_valid(ray: TraceRay) -> bool {
    let direction = trace_ray_direction(ray);
    return !any(direction != direction) &&
        !any(abs(direction) > vec3<f32>(1.0e20)) &&
        dot(direction, direction) > 1.0e-12 &&
        trace_ray_tmin(ray) < trace_ray_tmax(ray);
}

fn camera_trace_ray(
    pixel: vec2<u32>,
    resolution: vec2<u32>,
    cameraPosition: vec3<f32>,
    cameraForward: vec3<f32>,
    cameraRight: vec3<f32>,
    cameraUp: vec3<f32>,
    aspectRatio: f32,
    tanHalfFovY: f32,
    tMin: f32,
    tMax: f32) -> TraceRay {
    let safeResolution = max(vec2<f32>(resolution), vec2<f32>(1.0));
    let ndc = ((vec2<f32>(pixel) + vec2<f32>(0.5)) / safeResolution) * 2.0 - vec2<f32>(1.0);
    let direction = normalize(
        cameraForward +
        cameraRight * (ndc.x * aspectRatio * tanHalfFovY) -
        cameraUp * (ndc.y * tanHalfFovY));
    return TraceRay(vec4<f32>(cameraPosition, tMin), vec4<f32>(direction, tMax));
}

fn hash_u32(x: u32) -> u32 {
    var h = x;
    h ^= h >> 16u;
    h *= 0x7feb352du;
    h ^= h >> 15u;
    h *= 0x846ca68bu;
    h ^= h >> 16u;
    return h;
}

fn hash_color(key: u32) -> vec3<f32> {
    let hx = hash_u32(key);
    let hy = hash_u32(key ^ 0x68bc21ebu);
    let hz = hash_u32(key ^ 0x02e5be93u);
    let c = vec3<f32>(f32(hx & 0xffu), f32(hy & 0xffu), f32(hz & 0xffu)) / 255.0;
    return 0.2 + 0.8 * c;
}

fn trace_hit_output_color(hit: TraceHit) -> vec4<f32> {
    if uniforms.debugMode == DEBUG_MODE_HEATMAP {
        if hit.instanceId == DEBUG_HIT_INSTANCE_RAY_GENERATED {
            return vec4<f32>(0.02, 0.18, 0.28, 1.0);
        }
        if hit.instanceId == DEBUG_HIT_INSTANCE_INVALID_RAY {
            return vec4<f32>(0.95, 0.05, 0.02, 1.0);
        }
        if hit.instanceId == DEBUG_HIT_INSTANCE_MARKER && hit.triangleId == DEBUG_HIT_TRACE_DISABLED {
            return vec4<f32>(1.0, 0.82, 0.05, 1.0);
        }
        if hit.instanceId == DEBUG_HIT_INSTANCE_HEATMAP {
            let normalized = clamp(f32(hit.triangleId) / f32(max(uniforms.heatmapMaxIterations, 1u)), 0.0, 1.0);
            let cold = vec3<f32>(0.04, 0.08, 0.22);
            let mid = vec3<f32>(0.05, 0.65, 0.95);
            let hot = vec3<f32>(1.0, 0.15, 0.02);
            let color = select(
                mix(cold, mid, normalized * 2.0),
                mix(mid, hot, (normalized - 0.5) * 2.0),
                normalized > 0.5);
            return vec4<f32>(color, 1.0);
        }
    }

    if uniforms.debugMode == DEBUG_MODE_RBVH_CARD_AABB &&
        hit.instanceId == DEBUG_HIT_INSTANCE_MARKER &&
        hit.triangleId == DEBUG_HIT_RBVH_CARD_AABB {
        return vec4<f32>(1.0, 0.05, 0.85, 1.0);
    }

    if hit.instanceId == INVALID_INDEX || hit.triangleId == INVALID_INDEX {
        return uniforms.backgroundColor;
    }

    let colorKey = hit.triangleId ^ (hit.instanceId * 0x9e3779b9u);
    return vec4<f32>(hash_color(colorKey), 1.0);
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn generate_camera_rays(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x >= uniforms.width || globalId.y >= uniforms.height {
        return;
    }

    let rayId = globalId.y * uniforms.width + globalId.x;
    if rayId >= uniforms.rayCount {
        return;
    }

    rayInput[rayId] = camera_trace_ray(
        globalId.xy,
        vec2<u32>(uniforms.width, uniforms.height),
        uniforms.cameraPosition,
        uniforms.cameraForward,
        uniforms.cameraRight,
        uniforms.cameraUp,
        uniforms.aspectRatio,
        uniforms.tanHalfFovY,
        uniforms.tMin,
        uniforms.tMax);
    if uniforms.debugMode == DEBUG_MODE_HEATMAP {
        hitOutput[rayId] = TraceHit(DEBUG_HIT_INSTANCE_RAY_GENERATED, 0u, 0.0, 0.0, 0.0, INVALID_INDEX, 0u, 0u);
    } else {
        hitOutput[rayId] = invalid_trace_hit();
    }
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn resolve_camera_hit_output(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x >= uniforms.width || globalId.y >= uniforms.height {
        return;
    }

    let rayId = globalId.y * uniforms.width + globalId.x;
    if rayId >= uniforms.rayCount {
        textureStore(outputTexture, vec2<i32>(globalId.xy), uniforms.backgroundColor);
        return;
    }

    textureStore(outputTexture, vec2<i32>(globalId.xy), trace_hit_output_color(hitOutput[rayId]));
}
