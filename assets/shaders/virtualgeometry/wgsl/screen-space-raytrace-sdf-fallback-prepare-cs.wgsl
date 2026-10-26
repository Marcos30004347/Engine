struct Uniforms {
    budget : u32,
    maxFailedRays : u32,
    enabled : u32,
    _padding : u32,
    tMin : f32,
    tMax : f32,
    _padding1 : vec2<u32>,
};

struct FailedRay {
    origin : vec4<f32>,
    direction : vec4<f32>,
    pixelAndRay : vec4<u32>,
};

struct FailedRayBuffer {
    count : atomic<u32>,
    overflow : atomic<u32>,
    capacity : u32,
    totalRays : atomic<u32>,
    offscreenRays : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
    rays : array<FailedRay>,
};

struct SdfTraceRay {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
};

struct DispatchIndirectArgs {
    groupCountX : u32,
    groupCountY : u32,
    groupCountZ : u32,
};

const TRACE_RAY_FLAG_ACTIVE : u32 = 1u << 0u;

@group(0) @binding(0) var<uniform> u : Uniforms;
@group(0) @binding(1) var<storage, read> failedRays : FailedRayBuffer;
@group(0) @binding(2) var<storage, read_write> sdfRayInput : array<SdfTraceRay>;
@group(0) @binding(3) var<storage, read_write> dispatchArgs : DispatchIndirectArgs;

fn invalid_trace_ray() -> SdfTraceRay {
    return SdfTraceRay(vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u));
}

fn active_failed_ray_count() -> u32 {
    let failedCapacity = min(failedRays.capacity, u.maxFailedRays);
    let failedCount = min(atomicLoad(&failedRays.count), failedCapacity);
    return select(0u, min(failedCount, u.budget), u.enabled != 0u);
}

@compute @workgroup_size(1, 1, 1)
fn prepare_dispatch() {
    let activeCount = active_failed_ray_count();
    dispatchArgs.groupCountX = (activeCount + 63u) / 64u;
    dispatchArgs.groupCountY = 1u;
    dispatchArgs.groupCountZ = 1u;
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let rayIndex = globalId.x;
    if (rayIndex >= u.budget) {
        return;
    }

    let activeCount = active_failed_ray_count();
    if (rayIndex >= activeCount) {
        sdfRayInput[rayIndex] = invalid_trace_ray();
        return;
    }

    let failedRay = failedRays.rays[rayIndex];
    sdfRayInput[rayIndex] = SdfTraceRay(
        vec4<f32>(failedRay.origin.xyz, u.tMin),
        vec4<f32>(failedRay.direction.xyz, u.tMax),
        vec4<u32>(failedRay.pixelAndRay.x, failedRay.pixelAndRay.y, TRACE_RAY_FLAG_ACTIVE, rayIndex));
}
