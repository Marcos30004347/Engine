fn trace_direct_ray(rayId: u32, traceEnabled: bool) {
    if rayId >= uniforms.rayCount {
        return;
    }

    if !traceEnabled {
        if debug_mode_heatmap() {
            hitOutput[rayId] = TraceHit(DEBUG_HIT_INSTANCE_MARKER, DEBUG_HIT_TRACE_DISABLED, 0.0, 0.0, 0.0, INVALID_INDEX, 0u, 0u);
        } else {
            hitOutput[rayId] = invalid_trace_hit();
        }
        return;
    }

    trace_ray(rayId);
}

fn ray_sort_key(rayId: u32) -> u32 {
    if rayId >= uniforms.rayCount {
        return 0u;
    }

    let ray = rayInput[rayId];
    let origin = ray.origin_tmin.xyz;
    let direction = ray.direction_tmax.xyz;
    let cell = vec3<i32>(floor(origin * 0.125));
    let cellHash =
        (bitcast<u32>(cell.x) * 73856093u) ^
        (bitcast<u32>(cell.y) * 19349663u) ^
        (bitcast<u32>(cell.z) * 83492791u);
    let dirOctant =
        select(0u, 4u, direction.x < 0.0) |
        select(0u, 2u, direction.y < 0.0) |
        select(0u, 1u, direction.z < 0.0);
    return ((cellHash & 31u) << 3u) | dirOctant;
}

@compute @workgroup_size(64, 1, 1)
fn clear_ray_sort_bins(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let bin = globalId.x;
    if bin >= RAY_SORT_BIN_COUNT {
        return;
    }
    atomicStore(&raySortBinCounts[bin], 0u);
    raySortBinOffsets[bin] = 0u;
}

@compute @workgroup_size(64, 1, 1)
fn count_ray_sort_bins(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let rayId = globalId.x;
    if rayId >= uniforms.rayCount {
        return;
    }
    atomicAdd(&raySortBinCounts[ray_sort_key(rayId)], 1u);
}

@compute @workgroup_size(1, 1, 1)
fn prefix_ray_sort_bins(@builtin(global_invocation_id) globalId: vec3<u32>) {
    if globalId.x != 0u {
        return;
    }

    var offset = 0u;
    for (var bin = 0u; bin < RAY_SORT_BIN_COUNT; bin += 1u) {
        let count = atomicLoad(&raySortBinCounts[bin]);
        raySortBinOffsets[bin] = offset;
        atomicStore(&raySortBinCounts[bin], 0u);
        offset += count;
    }
}

@compute @workgroup_size(64, 1, 1)
fn scatter_ray_sort_ids(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let rayId = globalId.x;
    if rayId >= uniforms.rayCount {
        return;
    }

    let bin = ray_sort_key(rayId);
    let slot = atomicAdd(&raySortBinCounts[bin], 1u);
    raySortIds[raySortBinOffsets[bin] + slot] = rayId;
}

@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn main(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    let queueIndex = globalId.y * uniforms.rayDispatchWidth + globalId.x;
    var rayId = queueIndex;
    if uniforms.traceControl.w > 0.5 && queueIndex < uniforms.rayCount {
        rayId = raySortIds[queueIndex];
    }

    trace_direct_ray(rayId, traceEnabled);
}

@compute @workgroup_size(64, 1, 1)
#ifdef SWRT_WORLD_CWBVH_SUBGROUP
fn main_external(
    @builtin(global_invocation_id) globalId: vec3<u32>,
    @builtin(subgroup_invocation_id) subgroupInvocationId: u32,
    @builtin(subgroup_size) subgroupSize: u32) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    if uniforms.traceControl.z <= 0.5 {
        trace_direct_ray(globalId.x, traceEnabled);
        return;
    }

    loop {
        var subgroupRayBase = 0u;
        if subgroupInvocationId == 0u {
            subgroupRayBase = atomicAdd(&traceStats.dynamicRayFetchCursor, subgroupSize);
        }
        subgroupRayBase = swrt_subgroup_broadcast_first_u32(subgroupRayBase);

        let queueIndex = subgroupRayBase + subgroupInvocationId;
        let hasRay = queueIndex < uniforms.rayCount;
        if swrt_subgroup_active_lane_count(hasRay) == 0u {
            break;
        }

        if hasRay {
            var rayId = queueIndex;
            if uniforms.traceControl.w > 0.5 {
                rayId = raySortIds[queueIndex];
            }
            trace_direct_ray(rayId, traceEnabled);
        }
    }
}
#else
fn main_external(@builtin(global_invocation_id) globalId: vec3<u32>) {
    let traceEnabled = (uniforms.overlayFlags & OVERLAY_FLAG_TRACE_ENABLED) != 0u;
    var rayId = globalId.x;
    if uniforms.traceControl.w > 0.5 && globalId.x < uniforms.rayCount {
        rayId = raySortIds[globalId.x];
    }
    trace_direct_ray(rayId, traceEnabled);
}
#endif
