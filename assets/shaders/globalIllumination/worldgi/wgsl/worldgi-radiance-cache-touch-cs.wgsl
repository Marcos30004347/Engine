#include "globalIllumination/worldgi/wgsl/worldgi-radiance-cache.wgsl"

struct WorldGIRadianceCacheTouchUniforms {
    surfaceCount : u32,
    frameIndex : u32,
    enabled : u32,
    screenHitsEnabled : u32,
    cacheCapacity : u32,
    probeCount : u32,
    ttlFrames : u32,
    _padding0 : u32,
    cellSizeMinConfidence : vec4<f32>,
};

@group(0) @binding(0) var<uniform> uniforms : WorldGIRadianceCacheTouchUniforms;
@group(0) @binding(1) var<storage, read> traceSurfaces : array<GITraceSurface>;
@group(0) @binding(2) var<storage, read_write> cacheEntries : array<GIWorldRadianceCacheEntry>;
@group(0) @binding(3) var<storage, read_write> activeList : array<GIWorldRadianceCacheActiveItem>;
@group(0) @binding(4) var<storage, read_write> counters : GIWorldRadianceCacheCounters;

fn surface_is_valid(surface : GITraceSurface) -> bool {
    return (surface.sourceFlags.y & GI_TRACE_SURFACE_FLAG_VALID) != 0u &&
        surface.sourceFlags.x != GI_TRACE_SURFACE_SOURCE_INVALID &&
        surface.geometryNormalConfidence.w >= uniforms.cellSizeMinConfidence.y &&
        surface.worldPositionHitT.w >= 0.0;
}

fn key_matches(entryIndex : u32, key : vec3<u32>) -> bool {
    return atomicLoad(&cacheEntries[entryIndex].keyX) == key.x &&
        atomicLoad(&cacheEntries[entryIndex].keyY) == key.y &&
        atomicLoad(&cacheEntries[entryIndex].keyZ) == key.z;
}

fn write_entry_from_surface(entryIndex : u32, surfaceIndex : u32, key : vec3<u32>, surface : GITraceSurface) {
    atomicStore(&cacheEntries[entryIndex].keyX, key.x);
    atomicStore(&cacheEntries[entryIndex].keyY, key.y);
    atomicStore(&cacheEntries[entryIndex].keyZ, key.z);
    cacheEntries[entryIndex].worldPositionCellSize = vec4<f32>(surface.worldPositionHitT.xyz, max(uniforms.cellSizeMinConfidence.x, 1.0e-4));
    cacheEntries[entryIndex].normalConfidence = surface.geometryNormalConfidence;
    cacheEntries[entryIndex].baseColorRoughness = vec4<f32>(surface.baseColorOpacity.rgb, surface.shadingNormalRoughness.w);
    cacheEntries[entryIndex].emissiveRadiance = surface.emissiveRadiance;
    cacheEntries[entryIndex].directDiffuseRadiance = vec4<f32>(0.0);
    cacheEntries[entryIndex].directSpecularRadiance = vec4<f32>(0.0);
    cacheEntries[entryIndex].indirectDiffuseIrradianceSH0 = vec4<f32>(0.0);
    cacheEntries[entryIndex].glossyRadiance = vec4<f32>(0.0);
    cacheEntries[entryIndex].metadata = vec4<u32>(uniforms.frameIndex, surface.ids.y, surface.ids.x, surfaceIndex);
}

fn append_active(entryIndex : u32, surfaceIndex : u32) {
    let activeIndex = atomicAdd(&counters.activeCount, 1u);
    if (activeIndex < arrayLength(&activeList)) {
        activeList[activeIndex].entrySurfaceProducerFlags = vec4<u32>(entryIndex, surfaceIndex, 0u, 0u);
    } else {
        _ = atomicAdd(&counters.overflowCount, 1u);
    }
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let surfaceIndex = gid.x;
    if (surfaceIndex >= uniforms.surfaceCount || uniforms.enabled == 0u) {
        return;
    }

    let surface = traceSurfaces[surfaceIndex];
    if (!surface_is_valid(surface)) {
        return;
    }
    if (surface.sourceFlags.x == GI_TRACE_SURFACE_SOURCE_PRIMARY_GBUFFER && uniforms.screenHitsEnabled == 0u) {
        return;
    }

    let capacity = min(max(uniforms.cacheCapacity, 1u), arrayLength(&cacheEntries));
    let probeCount = min(max(uniforms.probeCount, 1u), WORLD_GI_RADIANCE_CACHE_MAX_PROBE_COUNT);
    let key = worldgi_radiance_cache_cell_key(surface.worldPositionHitT.xyz, uniforms.cellSizeMinConfidence.x);
    let hash = worldgi_hash_cell_key(key);
    _ = atomicAdd(&counters.touchedCount, 1u);

    for (var probe = 0u; probe < probeCount; probe = probe + 1u) {
        let entryIndex = (hash + probe) % capacity;
        let flags = atomicLoad(&cacheEntries[entryIndex].flags);
        if ((flags & WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_LOCKED) != 0u) {
            continue;
        }

        if (worldgi_radiance_cache_entry_alive(flags, uniforms.frameIndex, cacheEntries[entryIndex].metadata.x, uniforms.ttlFrames) && key_matches(entryIndex, key)) {
            cacheEntries[entryIndex].metadata.x = uniforms.frameIndex;
            _ = atomicAdd(&counters.reusedCount, 1u);
            return;
        }

        let stale = (flags & WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_VALID) != 0u &&
            !worldgi_radiance_cache_entry_alive(flags, uniforms.frameIndex, cacheEntries[entryIndex].metadata.x, uniforms.ttlFrames);
        if (flags == WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_EMPTY || stale) {
            let claim = atomicCompareExchangeWeak(&cacheEntries[entryIndex].flags, flags, WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_LOCKED);
            if (claim.exchanged) {
                write_entry_from_surface(entryIndex, surfaceIndex, key, surface);
                append_active(entryIndex, surfaceIndex);
                atomicStore(&cacheEntries[entryIndex].flags, WORLD_GI_RADIANCE_CACHE_ENTRY_FLAG_VALID);
                return;
            }
        }
    }

    _ = atomicAdd(&counters.overflowCount, 1u);
}
