#include "globalIllumination/worldgi/wgsl/worldgi-types.wgsl"

struct WorldGIRayGenUniforms {
    viewProjection : mat4x4<f32>,
    inverseViewProjection : mat4x4<f32>,
    cameraPosition : vec4<f32>,
    viewport : vec2<u32>,
    rayGrid : vec2<u32>,
    frameIndex : u32,
    raysPerPixel : u32,
    enabled : u32,
    reverseZ : u32,
    diffuseEnabled : u32,
    glossyEnabled : u32,
    minTraceDistance : f32,
    maxTraceDistance : f32,
    normalBias : f32,
    glossyMaxRoughness : f32,
    _padding0 : vec2<u32>,
};

struct SdfTraceRay {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
};

const PI : f32 = 3.14159265359;
const INV_PI : f32 = 0.31830988618;
const TRACE_RAY_FLAG_ACTIVE : u32 = 1u << 0u;

@group(0) @binding(0) var<uniform> uniforms : WorldGIRayGenUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(3) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(4) var materialPropertiesTexture : texture_2d<f32>;
@group(0) @binding(5) var<storage, read_write> worldGIRays : array<WorldGIRay>;
@group(0) @binding(6) var<storage, read_write> sdfTraceRays : array<SdfTraceRay>;

fn valid_depth(depthValue : f32) -> bool {
    return select(depthValue < 0.999999, depthValue > 1.0e-6, uniforms.reverseZ != 0u);
}

fn screen_pixel_for_ray_pixel(rayPixel : vec2<u32>) -> vec2<u32> {
    let uv = (vec2<f32>(rayPixel) + vec2<f32>(0.5)) / vec2<f32>(max(uniforms.rayGrid, vec2<u32>(1u)));
    let pixel = vec2<u32>(floor(uv * vec2<f32>(uniforms.viewport)));
    return min(pixel, uniforms.viewport - vec2<u32>(1u));
}

fn reconstruct_world_position(pixelCoord : vec2<u32>, depthValue : f32) -> vec3<f32> {
    let uv = (vec2<f32>(pixelCoord) + vec2<f32>(0.5)) / vec2<f32>(uniforms.viewport);
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = uniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
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

fn tangent_frame_direction(axis : vec3<f32>, local : vec3<f32>) -> vec3<f32> {
    let helper = select(vec3<f32>(0.0, 0.0, 1.0), vec3<f32>(0.0, 1.0, 0.0), abs(axis.z) > 0.9);
    let tangent = safe_normalize(cross(helper, axis), vec3<f32>(1.0, 0.0, 0.0));
    let bitangent = cross(axis, tangent);
    return safe_normalize(tangent * local.x + bitangent * local.y + axis * local.z, axis);
}

fn cosine_sample(axis : vec3<f32>, seed : ptr<function, u32>) -> vec4<f32> {
    let xi0 = random01(seed);
    let xi1 = random01(seed);
    let phi = xi0 * 2.0 * PI;
    let r = sqrt(xi1);
    let z = sqrt(max(0.0, 1.0 - xi1));
    let local = vec3<f32>(cos(phi) * r, sin(phi) * r, z);
    let direction = tangent_frame_direction(axis, local);
    return vec4<f32>(direction, max(z * INV_PI, 1.0e-6));
}

fn glossy_sample(reflectionDirection : vec3<f32>, roughness : f32, seed : ptr<function, u32>) -> vec4<f32> {
    let xi0 = random01(seed);
    let xi1 = random01(seed);
    let alpha = max(roughness * roughness, 0.02);
    let exponent = max(1.0, 2.0 / max(alpha * alpha, 1.0e-4) - 2.0);
    let phi = xi0 * 2.0 * PI;
    let cosTheta = pow(max(1.0 - xi1, 0.0), 1.0 / (exponent + 1.0));
    let sinTheta = sqrt(max(0.0, 1.0 - cosTheta * cosTheta));
    let local = vec3<f32>(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
    let direction = tangent_frame_direction(reflectionDirection, local);
    let pdf = max((exponent + 1.0) * pow(max(cosTheta, 0.0), exponent) / (2.0 * PI), 1.0e-6);
    return vec4<f32>(direction, pdf);
}

fn ray_type_for_sample(sampleIndex : u32) -> u32 {
    let diffuse = uniforms.diffuseEnabled != 0u;
    let glossy = uniforms.glossyEnabled != 0u;
    if (diffuse && glossy) {
        return select(WORLD_GI_RAY_TYPE_DIFFUSE, WORLD_GI_RAY_TYPE_GLOSSY, (sampleIndex & 1u) != 0u);
    }
    if (glossy) {
        return WORLD_GI_RAY_TYPE_GLOSSY;
    }
    if (diffuse) {
        return WORLD_GI_RAY_TYPE_DIFFUSE;
    }
    return WORLD_GI_RAY_TYPE_INVALID;
}

fn active_strategy_count() -> f32 {
    return f32(select(0u, 1u, uniforms.diffuseEnabled != 0u) + select(0u, 1u, uniforms.glossyEnabled != 0u));
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    if (gid.x >= uniforms.rayGrid.x || gid.y >= uniforms.rayGrid.y) {
        return;
    }

    let rayPixel = gid.xy;
    let screenPixel = screen_pixel_for_ray_pixel(rayPixel);
    let rayPixelIndex = rayPixel.y * uniforms.rayGrid.x + rayPixel.x;
    let baseRayIndex = rayPixelIndex * uniforms.raysPerPixel;
    let rayCount = arrayLength(&worldGIRays);
    let sdfRayCount = arrayLength(&sdfTraceRays);

    let depthValue = textureLoad(sceneDepthTexture, vec2<i32>(screenPixel), 0);
    let validReceiver = uniforms.enabled != 0u && valid_depth(depthValue) && active_strategy_count() > 0.0;
    let worldPosition = reconstruct_world_position(screenPixel, depthValue);
    let geometryNormal = safe_normalize(textureLoad(geometryNormalTexture, vec2<i32>(screenPixel), 0).xyz, vec3<f32>(0.0, 1.0, 0.0));
    let baseColor = max(textureLoad(baseColorTexture, vec2<i32>(screenPixel), 0).rgb, vec3<f32>(0.0));
    let materialProperties = textureLoad(materialPropertiesTexture, vec2<i32>(screenPixel), 0);
    let roughness = clamp(materialProperties.x, 0.02, 1.0);
    let metallic = clamp(materialProperties.y, 0.0, 1.0);
    let viewDirection = safe_normalize(uniforms.cameraPosition.xyz - worldPosition, geometryNormal);
    let reflectionDirection = safe_normalize(reflect(-viewDirection, geometryNormal), geometryNormal);
    let strategyCount = max(active_strategy_count(), 1.0);
    let techniquePdf = 1.0 / strategyCount;

    for (var sampleIndex = 0u; sampleIndex < uniforms.raysPerPixel; sampleIndex = sampleIndex + 1u) {
        let rayIndex = baseRayIndex + sampleIndex;
        if (rayIndex >= rayCount || rayIndex >= sdfRayCount) {
            continue;
        }

        var worldRay = invalid_world_gi_ray();
        var sdfRay = SdfTraceRay(vec4<f32>(0.0), vec4<f32>(0.0), vec4<u32>(0u));
        let rayType = ray_type_for_sample(sampleIndex);
        if (validReceiver && rayType != WORLD_GI_RAY_TYPE_INVALID) {
            var seed = hash_u32(screenPixel.x ^ (screenPixel.y * 0x9e3779b9u) ^ (uniforms.frameIndex * 0x85ebca6bu) ^ (sampleIndex * 0xc2b2ae35u));
            let diffuseSample = cosine_sample(geometryNormal, &seed);
            let glossySample = glossy_sample(reflectionDirection, roughness, &seed);
            let selected = select(diffuseSample, glossySample, rayType == WORLD_GI_RAY_TYPE_GLOSSY);
            let direction = safe_normalize(selected.xyz, geometryNormal);
            let origin = worldPosition + geometryNormal * uniforms.normalBias;
            let rayPdf = selected.w;
            let cosTheta = max(dot(geometryNormal, direction), 0.0);
            let diffusePdf = max(cosTheta * INV_PI, 1.0e-6);
            let bsdfPdf = select(diffusePdf, rayPdf, rayType == WORLD_GI_RAY_TYPE_GLOSSY);
            let misWeight = rayPdf * techniquePdf / max(rayPdf * techniquePdf + bsdfPdf * techniquePdf, 1.0e-6);

            worldRay = WorldGIRay(
                vec4<f32>(origin, uniforms.minTraceDistance),
                vec4<f32>(direction, uniforms.maxTraceDistance),
                vec4<f32>(worldPosition, rayPdf),
                vec4<f32>(geometryNormal, roughness),
                vec4<f32>(baseColor, metallic),
                vec4<f32>(rayPdf, bsdfPdf, techniquePdf, misWeight),
                vec4<u32>(screenPixel.x, screenPixel.y, sampleIndex, uniforms.frameIndex),
                vec4<u32>(rayType, WORLD_GI_RAY_BACKEND_SDF, WORLD_GI_RAY_FLAG_VALID, 0u));
            sdfRay = SdfTraceRay(
                vec4<f32>(origin, uniforms.minTraceDistance),
                vec4<f32>(direction, uniforms.maxTraceDistance),
                vec4<u32>(screenPixel.x, screenPixel.y, TRACE_RAY_FLAG_ACTIVE, rayIndex));
        }

        worldGIRays[rayIndex] = worldRay;
        sdfTraceRays[rayIndex] = sdfRay;
    }
}
