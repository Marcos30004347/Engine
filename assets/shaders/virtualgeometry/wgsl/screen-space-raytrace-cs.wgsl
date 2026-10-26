struct ScreenSpaceRayTraceUniforms {
    viewProjection : mat4x4<f32>,
    inverseViewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    hzbSize : vec2<u32>,
    hzbMipCount : u32,
    tileSize : u32,
    raysPerTile : u32,
    maxSteps : u32,
    maxFailedRays : u32,
    frameIndex : u32,
    reverseZ : u32,
    enabled : u32,
    minTraceDistance : f32,
    maxTraceDistance : f32,
    surfaceThickness : f32,
    originBias : f32,
    forceWorldFallback : u32,
    screenTraceEnabled : u32,
    _padding0 : vec2<u32>,
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

struct ScreenHit {
    origin : vec4<f32>,
    direction : vec4<f32>,
    hitPositionDistance : vec4<f32>,
    pixelsAndRay : vec4<u32>,
};

struct ScreenHitBuffer {
    count : atomic<u32>,
    overflow : atomic<u32>,
    capacity : u32,
    totalHits : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
    _padding3 : u32,
    hits : array<ScreenHit>,
};

struct ProjectedPoint {
    pixel : vec2<i32>,
    depth : f32,
    uv : vec2<f32>,
    valid : bool,
};

@group(0) @binding(0) var<uniform> u : ScreenSpaceRayTraceUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(3) var hzbTexture : texture_2d<f32>;
@group(0) @binding(4) var<storage, read_write> failedRays : FailedRayBuffer;
@group(0) @binding(5) var stepCountTexture : texture_storage_2d<r32uint, write>;
@group(0) @binding(6) var<storage, read_write> screenHits : ScreenHitBuffer;

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, u.reverseZ != 0u);
}

fn reconstruct_world_position(pixelCoord : vec2<u32>, depthValue : f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixelCoord) + vec2<f32>(0.5, 0.5)) / vec2<f32>(u.viewport);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = u.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn project_world_position(worldPosition : vec3<f32>) -> ProjectedPoint {
    let clip = u.viewProjection * vec4<f32>(worldPosition, 1.0);
    let invW = select(1.0, 1.0 / clip.w, abs(clip.w) > 1.0e-6);
    let ndc = clip.xyz * invW;
    let uv = vec2<f32>(ndc.x * 0.5 + 0.5, 1.0 - (ndc.y * 0.5 + 0.5));
    var result : ProjectedPoint;
    result.uv = uv;
    result.depth = ndc.z;
    result.pixel = vec2<i32>(floor(uv * vec2<f32>(u.viewport)));
    result.valid = clip.w > 0.0 && uv.x >= 0.0 && uv.y >= 0.0 && uv.x < 1.0 && uv.y < 1.0 && ndc.z >= 0.0 && ndc.z <= 1.0;
    return result;
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

fn random01(seed : ptr<function, u32>) -> f32 {
    (*seed) = hash_u32((*seed) + 0x9e3779b9u);
    return f32((*seed) & 0x00ffffffu) / 16777216.0;
}

fn safe_normalize(value : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(value);
    return select(fallback, value / len, len > 1.0e-6);
}

fn hemisphere_direction(normal : vec3<f32>, seed : ptr<function, u32>) -> vec3<f32> {
    let xi0 = random01(seed);
    let xi1 = random01(seed);
    let phi = xi0 * 6.28318530718;
    let cosTheta = xi1;
    let sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    let local = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);

    let helper = select(vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(0.0, 1.0, 0.0), abs(normal.z) > 0.9);
    let tangent = safe_normalize(cross(helper, normal), vec3<f32>(1.0, 0.0, 0.0));
    let bitangent = cross(normal, tangent);
    return safe_normalize(tangent * local.x + bitangent * local.y + normal * local.z, normal);
}

fn depth_hit(rayDepth : f32, sceneDepth : f32) -> bool {
    if (!valid_depth(sceneDepth)) {
        return false;
    }

    let delta = select(rayDepth - sceneDepth, sceneDepth - rayDepth, u.reverseZ != 0u);
    return delta >= 0.0 && delta <= u.surfaceThickness;
}

fn choose_mip(stepIndex : u32) -> u32 {
    let coarse = stepIndex / 4u;
    return min(max(u.hzbMipCount, 1u) - 1u, coarse);
}

fn sample_hzb(projected : ProjectedPoint, mipLevel : u32) -> f32 {
    let mipSize = max(vec2<u32>(1u, 1u), u.hzbSize >> vec2<u32>(mipLevel, mipLevel));
    let coord = clamp(vec2<i32>(floor(projected.uv * vec2<f32>(mipSize))), vec2<i32>(0, 0), vec2<i32>(mipSize) - vec2<i32>(1, 1));
    return textureLoad(hzbTexture, coord, i32(mipLevel)).r;
}

fn append_failed_ray(origin : vec3<f32>, direction : vec3<f32>, pixel : vec2<u32>, rayIndex : u32, steps : u32) {
    atomicAdd(&failedRays.offscreenRays, 1u);

    let index = atomicAdd(&failedRays.count, 1u);
    if (index >= failedRays.capacity) {
        atomicStore(&failedRays.overflow, 1u);
        return;
    }

    failedRays.rays[index].origin = vec4<f32>(origin, 1.0);
    failedRays.rays[index].direction = vec4<f32>(direction, 0.0);
    failedRays.rays[index].pixelAndRay = vec4<u32>(pixel.x, pixel.y, steps, rayIndex);
}

fn append_screen_hit(
    origin : vec3<f32>,
    direction : vec3<f32>,
    hitPosition : vec3<f32>,
    rayDistance : f32,
    receiverPixel : vec2<u32>,
    hitPixel : vec2<u32>,
    rayIndex : u32) {
    atomicAdd(&screenHits.totalHits, 1u);

    let index = atomicAdd(&screenHits.count, 1u);
    if (index >= screenHits.capacity) {
        atomicStore(&screenHits.overflow, 1u);
        return;
    }

    screenHits.hits[index].origin = vec4<f32>(origin, 1.0);
    screenHits.hits[index].direction = vec4<f32>(direction, 0.0);
    screenHits.hits[index].hitPositionDistance = vec4<f32>(hitPosition, rayDistance);
    screenHits.hits[index].pixelsAndRay = vec4<u32>(receiverPixel.x, receiverPixel.y, hitPixel.x, hitPixel.y);
}

fn trace_ray(origin : vec3<f32>, direction : vec3<f32>, pixel : vec2<u32>, rayIndex : u32) -> u32 {
    var stepsTaken = 0u;
    let stepLength = (u.maxTraceDistance - u.minTraceDistance) / f32(max(u.maxSteps, 1u));

    for (var stepIndex = 0u; stepIndex < u.maxSteps; stepIndex++) {
        stepsTaken = stepIndex + 1u;
        let rayDistance = u.minTraceDistance + stepLength * f32(stepIndex + 1u);
        let projected = project_world_position(origin + direction * rayDistance);
        if (!projected.valid) {
            append_failed_ray(origin, direction, pixel, rayIndex, stepsTaken);
            return stepsTaken;
        }

        let mipLevel = choose_mip(stepIndex);
        let hzbDepth = sample_hzb(projected, mipLevel);
        if (!depth_hit(projected.depth, hzbDepth)) {
            continue;
        }

        let fullResDepth = textureLoad(sceneDepthTexture, projected.pixel, 0);
        if (depth_hit(projected.depth, fullResDepth)) {
            let hitPixel = vec2<u32>(u32(projected.pixel.x), u32(projected.pixel.y));
            let hitPosition = reconstruct_world_position(hitPixel, fullResDepth);
            append_screen_hit(origin, direction, hitPosition, rayDistance, pixel, hitPixel, rayIndex);
            return stepsTaken;
        }
    }

    append_failed_ray(origin, direction, pixel, rayIndex, stepsTaken);
    return stepsTaken;
}

fn write_tile_steps(tileBase : vec2<u32>, totalSteps : u32) {
    for (var y = 0u; y < u.tileSize; y++) {
        for (var x = 0u; x < u.tileSize; x++) {
            let pixel = tileBase + vec2<u32>(x, y);
            if (pixel.x < u.viewport.x && pixel.y < u.viewport.y) {
                textureStore(stepCountTexture, vec2<i32>(pixel), vec4<u32>(totalSteps, 0u, 0u, 1u));
            }
        }
    }
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalId : vec3<u32>) {
    let tileCount = (u.viewport + vec2<u32>(u.tileSize - 1u)) / vec2<u32>(u.tileSize);
    if (globalId.x >= tileCount.x || globalId.y >= tileCount.y) {
        return;
    }

    let tileBase = globalId.xy * vec2<u32>(u.tileSize);
    let sourcePixel = min(tileBase + vec2<u32>(u.tileSize / 2u), u.viewport - vec2<u32>(1u, 1u));

    if (u.enabled == 0u) {
        write_tile_steps(tileBase, 0u);
        return;
    }

    let sceneDepth = textureLoad(sceneDepthTexture, vec2<i32>(sourcePixel), 0);
    if (!valid_depth(sceneDepth)) {
        write_tile_steps(tileBase, 0u);
        return;
    }

    let normal = safe_normalize(textureLoad(geometryNormalTexture, vec2<i32>(sourcePixel), 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let worldPosition = reconstruct_world_position(sourcePixel, sceneDepth);
    let origin = worldPosition + normal * u.originBias;
    var totalSteps = 0u;

    atomicAdd(&failedRays.totalRays, u.raysPerTile);
    for (var rayIndex = 0u; rayIndex < u.raysPerTile; rayIndex++) {
        var seed = hash_u32(
            sourcePixel.x * 1973u ^
            sourcePixel.y * 9277u ^
            u.frameIndex * 26699u ^
            rayIndex * 911u);
        let direction = hemisphere_direction(normal, &seed);
        if (u.forceWorldFallback != 0u || u.screenTraceEnabled == 0u) {
            append_failed_ray(origin, direction, sourcePixel, rayIndex, 0u);
        } else {
            totalSteps += trace_ray(origin, direction, sourcePixel, rayIndex);
        }
    }

    write_tile_steps(tileBase, totalSteps);
}
