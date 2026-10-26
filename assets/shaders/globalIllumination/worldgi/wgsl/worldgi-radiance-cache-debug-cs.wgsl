#include "globalIllumination/worldgi/wgsl/worldgi-radiance-cache.wgsl"

struct WorldGIRadianceCacheDebugUniforms {
    inverseViewProjection : mat4x4<f32>,
    viewport : vec2<u32>,
    frameIndex : u32,
    enabled : u32,
    reverseZ : u32,
    cacheCapacity : u32,
    probeCount : u32,
    ttlFrames : u32,
    cellSizePadding : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGIRadianceCacheDebugUniforms;
@group(0) @binding(1) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(2) var<storage, read> cacheEntries : array<GIWorldRadianceCacheEntry>;
@group(0) @binding(3) var outputTexture : texture_storage_2d<rgba16float, write>;

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

fn key_matches(entryIndex : u32, key : vec3<u32>) -> bool {
    return atomicLoad(&cacheEntries[entryIndex].keyX) == key.x &&
        atomicLoad(&cacheEntries[entryIndex].keyY) == key.y &&
        atomicLoad(&cacheEntries[entryIndex].keyZ) == key.z;
}

fn query_cache_radiance(position : vec3<f32>) -> vec4<f32> {
    let capacity = min(max(uniforms.cacheCapacity, 1u), arrayLength(&cacheEntries));
    let probeCount = min(max(uniforms.probeCount, 1u), WORLD_GI_RADIANCE_CACHE_MAX_PROBE_COUNT);
    let key = worldgi_radiance_cache_cell_key(position, uniforms.cellSizePadding.x);
    let hash = worldgi_hash_cell_key(key);

    for (var probe = 0u; probe < probeCount; probe = probe + 1u) {
        let entryIndex = (hash + probe) % capacity;
        let flags = atomicLoad(&cacheEntries[entryIndex].flags);
        if (worldgi_radiance_cache_entry_alive(flags, uniforms.frameIndex, cacheEntries[entryIndex].metadata.x, uniforms.ttlFrames) && key_matches(entryIndex, key)) {
            let radiance =
                max(cacheEntries[entryIndex].emissiveRadiance.rgb, vec3<f32>(0.0)) +
                max(cacheEntries[entryIndex].directDiffuseRadiance.rgb, vec3<f32>(0.0)) +
                max(cacheEntries[entryIndex].indirectDiffuseIrradianceSH0.rgb, vec3<f32>(0.0));
            let confidence = max(cacheEntries[entryIndex].directDiffuseRadiance.w, cacheEntries[entryIndex].emissiveRadiance.w);
            return vec4<f32>(radiance, confidence);
        }
    }

    return vec4<f32>(0.0);
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
            outColor = query_cache_radiance(reconstruct_world_position(pixel, depthValue));
        }
    }

    textureStore(outputTexture, vec2<i32>(pixel), outColor);
}
