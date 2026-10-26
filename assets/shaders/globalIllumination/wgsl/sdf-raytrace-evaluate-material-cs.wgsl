#include "globalIllumination/wgsl/trace-shading-types.wgsl"

const INVALID_INDEX : u32 = 0xffffffffu;
const MATERIAL_INVALID_TEXTURE_ID : u32 = 0xffffffffu;

struct SDFRayTraceEvaluateMaterialUniforms {
    controls : vec4<u32>,
};

struct SdfTraceRay {
    originAndMinT : vec4<f32>,
    directionAndMaxT : vec4<f32>,
    metadata : vec4<u32>,
};

struct SdfTraceHit {
    worldPositionAndDistance : vec4<f32>,
    worldNormalAndStepCount : vec4<f32>,
    localUVWAndAux : vec4<f32>,
    metadata : vec4<u32>,
    traceInfo : vec4<u32>,
    surfaceMaterialReference : vec4<u32>,
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

struct MaterialRuntimeData {
    materialId : u32,
    tileListBase : u32,
    baseColorTextureId : u32,
    normalTextureId : u32,
    hasNormalTexture : u32,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
    baseColorFactor : vec4<f32>,
    pbrFactors : vec4<f32>,
    emissiveFactorAndStrength : vec4<f32>,
};

struct AtlasInfo {
    pageSize : u32,
    physicalPagesPerAxis : u32,
    atlasWidth : u32,
    atlasHeight : u32,
    totalPhysicalPages : u32,
    textureCount : u32,
    materialCount : u32,
    pageTableEntryCount : u32,
};

struct VirtualTextureMipInfo {
    width : u32,
    height : u32,
    pageTableOffset : u32,
    pagesX : u32,
    pagesY : u32,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
};

struct VirtualTextureEntry {
    mips : array<VirtualTextureMipInfo, 16>,
    mipCount : u32,
    pageSize : u32,
    totalPageCount : u32,
    flags : u32,
    addressModeU : u32,
    addressModeV : u32,
    filterMode : u32,
    mipBias : f32,
    minMip : u32,
    maxMip : u32,
    _padding0 : u32,
    _padding1 : u32,
};

struct VirtualTexturePageTableEntry {
    physicalPage : u32,
    flags : u32,
    _padding0 : u32,
    _padding1 : u32,
};

struct FeedbackRequest {
    textureIndex : u32,
    mipLevel : u32,
    pageX : u32,
    pageY : u32,
};

struct FeedbackBuffer {
    requestCount : atomic<u32>,
    overflowCount : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    requests : array<FeedbackRequest>,
};

const TRACE_HIT_FLAG_HIT : u32 = 1u << 0u;
const TRACE_HIT_FLAG_INACTIVE_RAY : u32 = 1u << 10u;
const TRACE_HIT_FLAG_INVALID_DIRECTION : u32 = 1u << 9u;

@group(0) @binding(0) var<uniform> uniforms : SDFRayTraceEvaluateMaterialUniforms;
@group(0) @binding(1) var<storage, read> sdfRayInput : array<SdfTraceRay>;
@group(0) @binding(2) var<storage, read> sdfHitOutput : array<SdfTraceHit>;
@group(0) @binding(3) var<storage, read> failedRays : FailedRayBuffer;
@group(0) @binding(4) var<storage, read_write> traceShading : array<TraceShadingParameters>;
@group(0) @binding(5) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(6) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(7) var<storage, read> materialRuntime : array<MaterialRuntimeData>;
@group(0) @binding(8) var<uniform> atlasInfo : AtlasInfo;
@group(0) @binding(9) var<storage, read> textureEntries : array<VirtualTextureEntry>;
@group(0) @binding(10) var<storage, read> vtPageTable : array<VirtualTexturePageTableEntry>;
@group(0) @binding(11) var<storage, read> vtPhysicalPages : array<u32>;
@group(0) @binding(12) var<storage, read_write> feedbackBuffer : FeedbackBuffer;

fn safe_normalize(v : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(v);
    return select(fallback, v / len, len > 1.0e-6);
}

fn extract_trace_flags(hit : SdfTraceHit) -> u32 {
    return hit.traceInfo.x >> 8u;
}

fn hit_is_valid(hit : SdfTraceHit) -> bool {
    let flags = extract_trace_flags(hit);
    return (flags & TRACE_HIT_FLAG_HIT) != 0u &&
        (flags & TRACE_HIT_FLAG_INACTIVE_RAY) == 0u &&
        (flags & TRACE_HIT_FLAG_INVALID_DIRECTION) == 0u &&
        hit.worldPositionAndDistance.w >= 0.0;
}

fn receiver_pixel(rayId : u32, ray : SdfTraceRay) -> vec2<i32> {
    if (ray.metadata.x != 0xffffffffu && ray.metadata.y != 0xffffffffu) {
        return vec2<i32>(ray.metadata.xy);
    }
    return vec2<i32>(failedRays.rays[rayId].pixelAndRay.xy);
}

fn applyAddressMode(coord: f32, mode: u32) -> f32 {
    if (mode == 1u) {
        return clamp(coord, 0.0, 1.0);
    }
    return coord - floor(coord);
}

fn sampleUV(textureIndex: u32, uv: vec2<f32>) -> vec2<f32> {
    let wrappedU = applyAddressMode(uv.x, textureEntries[textureIndex].addressModeU);
    let wrappedV = 1.0 - applyAddressMode(uv.y, textureEntries[textureIndex].addressModeV);
    return vec2<f32>(wrappedU, wrappedV);
}

fn textureMipPixel(textureIndex: u32, mipLevel: u32, uv: vec2<f32>) -> vec2<u32> {
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    let wrapped = sampleUV(textureIndex, uv);
    return vec2<u32>(
        min(u32(wrapped.x * f32(max(mipInfo.width, 1u))), max(mipInfo.width, 1u) - 1u),
        min(u32(wrapped.y * f32(max(mipInfo.height, 1u))), max(mipInfo.height, 1u) - 1u));
}

fn textureMipTexel(textureIndex: u32, mipLevel: u32, uv: vec2<f32>) -> vec2<f32> {
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    let wrapped = sampleUV(textureIndex, uv);
    return vec2<f32>(
        wrapped.x * f32(max(mipInfo.width, 1u)) - 0.5,
        wrapped.y * f32(max(mipInfo.height, 1u)) - 0.5);
}

fn decode_uv_window_mip(hit: SdfTraceHit, textureIndex: u32) -> u32 {
    if ((hit.surfaceMaterialReference.w & (1u << 31u)) == 0u || textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }
    let qx = hit.surfaceMaterialReference.w & 2047u;
    let qy = (hit.surfaceMaterialReference.w >> 11u) & 2047u;
    let uvWindow = vec2<f32>(f32(qx), f32(qy)) / 2047.0;
    let baseMip = textureEntries[textureIndex].mips[0];
    let footprint = max(uvWindow.x * f32(max(baseMip.width, 1u)), uvWindow.y * f32(max(baseMip.height, 1u)));
    let requested = max(0.0, floor(log2(max(footprint, 1.0))) + textureEntries[textureIndex].mipBias);
    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    return u32(clamp(requested, 0.0, f32(highestMip)));
}

fn finest_resident_start_mip(textureIndex: u32) -> u32 {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries) || textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }
    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    let minMip = min(textureEntries[textureIndex].minMip, highestMip);
    let maxMip = min(max(textureEntries[textureIndex].maxMip, minMip), highestMip);
    return min(minMip, maxMip);
}

fn requestVirtualPage(textureIndex: u32, uv: vec2<f32>, requestedMip: u32) {
    if (uniforms.controls.z == 0u || textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries)) {
        return;
    }
    if (textureEntries[textureIndex].mipCount == 0u || atlasInfo.pageSize == 0u) {
        return;
    }

    let mip = min(requestedMip, textureEntries[textureIndex].mipCount - 1u);
    let mipInfo = textureEntries[textureIndex].mips[mip];
    if (mipInfo.pagesX == 0u || mipInfo.pagesY == 0u) {
        return;
    }

    let mipPixel = textureMipPixel(textureIndex, mip, uv);
    let requestedPageX = min(mipPixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u);
    let requestedPageY = min(mipPixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u);
    let pageTableIndex = mipInfo.pageTableOffset + requestedPageY * mipInfo.pagesX + requestedPageX;
    if (pageTableIndex < arrayLength(&vtPageTable)) {
        let pageEntry = vtPageTable[pageTableIndex];
        if ((pageEntry.flags & 1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
            return;
        }
    }

    let requestIndex = atomicAdd(&feedbackBuffer.requestCount, 1u);
    if (requestIndex >= uniforms.controls.z) {
        atomicAdd(&feedbackBuffer.overflowCount, 1u);
        return;
    }
    feedbackBuffer.requests[requestIndex] = FeedbackRequest(textureIndex, mip, requestedPageX, requestedPageY);
}

struct ResolvedVirtualPage {
    valid : bool,
    physicalPage : u32,
    mipLevel : u32,
    pixelX : u32,
    pixelY : u32,
};

fn resolveVirtualPageFromMip(textureIndex: u32, uv: vec2<f32>, firstMip: u32) -> ResolvedVirtualPage {
    var resolved : ResolvedVirtualPage;
    resolved.valid = false;
    resolved.physicalPage = MATERIAL_INVALID_TEXTURE_ID;
    resolved.mipLevel = 0u;
    resolved.pixelX = 0u;
    resolved.pixelY = 0u;

    if (textureEntries[textureIndex].mipCount == 0u) {
        return resolved;
    }

    let requestedMip = min(firstMip, textureEntries[textureIndex].mipCount - 1u);
    for (var mipLevel = requestedMip; mipLevel < textureEntries[textureIndex].mipCount; mipLevel++) {
        let mipInfo = textureEntries[textureIndex].mips[mipLevel];
        if (mipInfo.pagesX == 0u || mipInfo.pagesY == 0u) {
            continue;
        }

        let mipPixel = textureMipPixel(textureIndex, mipLevel, uv);
        let pageX = min(mipPixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u);
        let pageY = min(mipPixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u);
        let pageTableIndex = mipInfo.pageTableOffset + pageY * mipInfo.pagesX + pageX;
        if (pageTableIndex < arrayLength(&vtPageTable)) {
            let pageEntry = vtPageTable[pageTableIndex];
            if ((pageEntry.flags & 1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
                resolved.valid = true;
                resolved.physicalPage = pageEntry.physicalPage;
                resolved.mipLevel = mipLevel;
                resolved.pixelX = mipPixel.x;
                resolved.pixelY = mipPixel.y;
                return resolved;
            }
        }
    }

    return resolved;
}

fn finest_resident_mip_at_uv(textureIndex: u32, uv: vec2<f32>) -> u32 {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries) || textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }

    let requestedMip = finest_resident_start_mip(textureIndex);
    if (atlasInfo.pageSize == 0u || atlasInfo.physicalPagesPerAxis == 0u) {
        return requestedMip;
    }

    let resolved = resolveVirtualPageFromMip(textureIndex, uv, requestedMip);
    if (resolved.valid) {
        return resolved.mipLevel;
    }
    return requestedMip;
}

fn samplePhysicalPage(physicalPage: u32, pixelX: u32, pixelY: u32) -> vec4<f32> {
    let pageSize = atlasInfo.pageSize;
    let localX = pixelX % pageSize;
    let localY = pixelY % pageSize;
    let texelIndex = physicalPage * pageSize * pageSize + localY * pageSize + localX;
    if (texelIndex >= arrayLength(&vtPhysicalPages)) {
        return vec4<f32>(1.0);
    }
    let rgba = vtPhysicalPages[texelIndex];
    return vec4<f32>(
        f32((rgba >> 0u) & 255u),
        f32((rgba >> 8u) & 255u),
        f32((rgba >> 16u) & 255u),
        f32((rgba >> 24u) & 255u)) / 255.0;
}

fn sampleVirtualTextureAtMip(textureIndex: u32, uv: vec2<f32>, mipLevel: u32) -> vec4<f32> {
    let resolved = resolveVirtualPageFromMip(textureIndex, uv, mipLevel);
    if (!resolved.valid) {
        return vec4<f32>(1.0);
    }
    return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
}

fn sampleFinestResidentVirtualTextureTexel(textureIndex: u32, uv: vec2<f32>) -> vec4<f32> {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries)) {
        return vec4<f32>(1.0);
    }
    if (textureEntries[textureIndex].mipCount == 0u || atlasInfo.pageSize == 0u || atlasInfo.physicalPagesPerAxis == 0u) {
        return vec4<f32>(1.0);
    }

    let resolved = resolveVirtualPageFromMip(textureIndex, uv, 0u);
    if (!resolved.valid) {
        return vec4<f32>(1.0);
    }
    return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
}

fn sampleVirtualTexture(textureIndex: u32, uv: vec2<f32>, mipLevel: u32) -> vec4<f32> {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries)) {
        return vec4<f32>(1.0);
    }
    if (textureEntries[textureIndex].mipCount == 0u) {
        return vec4<f32>(1.0);
    }

    if (atlasInfo.pageSize == 0u || atlasInfo.physicalPagesPerAxis == 0u) {
        return vec4<f32>(1.0);
    }

    let resolved = resolveVirtualPageFromMip(textureIndex, uv, mipLevel);
    if (!resolved.valid) {
        return vec4<f32>(1.0);
    }

    if (textureEntries[textureIndex].filterMode == 0u) {
        return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
    }

    let mipInfo = textureEntries[textureIndex].mips[resolved.mipLevel];
    let texelPos = textureMipTexel(textureIndex, resolved.mipLevel, uv);
    let base = vec2<i32>(floor(texelPos));
    let frac = texelPos - vec2<f32>(base);
    let invSize = vec2<f32>(
        1.0 / f32(max(mipInfo.width, 1u)),
        1.0 / f32(max(mipInfo.height, 1u)));

    let uv00 = (vec2<f32>(base) + vec2<f32>(0.5, 0.5)) * invSize;
    let uv10 = uv00 + vec2<f32>(invSize.x, 0.0);
    let uv01 = uv00 + vec2<f32>(0.0, invSize.y);
    let uv11 = uv00 + invSize;

    let c00 = sampleVirtualTextureAtMip(textureIndex, uv00, resolved.mipLevel);
    let c10 = sampleVirtualTextureAtMip(textureIndex, uv10, resolved.mipLevel);
    let c01 = sampleVirtualTextureAtMip(textureIndex, uv01, resolved.mipLevel);
    let c11 = sampleVirtualTextureAtMip(textureIndex, uv11, resolved.mipLevel);

    let cx0 = mix(c00, c10, frac.x);
    let cx1 = mix(c01, c11, frac.x);
    return mix(cx0, cx1, frac.y);
}

fn evaluate_surface_albedo(hit : SdfTraceHit, receiverAlbedo : vec3<f32>) -> vec4<f32> {
    let materialReferenceValid = (hit.surfaceMaterialReference.w & (1u << 31u)) != 0u;
    if (!materialReferenceValid || hit.surfaceMaterialReference.x >= arrayLength(&materialRuntime)) {
        return vec4<f32>(receiverAlbedo, 1.0);
    }

    let runtime = materialRuntime[hit.surfaceMaterialReference.x];
    let uv = vec2<f32>(bitcast<f32>(hit.surfaceMaterialReference.y), bitcast<f32>(hit.surfaceMaterialReference.z));
    var baseColor = runtime.baseColorFactor;
    if (uniforms.controls.w != 0u && runtime.baseColorTextureId != MATERIAL_INVALID_TEXTURE_ID) {
        let requestedMip = 0u;
        requestVirtualPage(runtime.baseColorTextureId, uv, requestedMip);
        baseColor = baseColor * sampleFinestResidentVirtualTextureTexel(runtime.baseColorTextureId, uv);
    }
    return vec4<f32>(clamp(baseColor.rgb, vec3<f32>(0.0), vec3<f32>(8.0)), clamp(runtime.pbrFactors.x, 0.0, 1.0));
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let rayId = gid.x;
    if (rayId >= uniforms.controls.y || rayId >= arrayLength(&sdfRayInput) || rayId >= arrayLength(&sdfHitOutput) || rayId >= arrayLength(&traceShading)) {
        return;
    }

    if (uniforms.controls.x == 0u) {
        traceShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let ray = sdfRayInput[rayId];
    let hit = sdfHitOutput[rayId];
    if (!hit_is_valid(hit)) {
        traceShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let rayDir = safe_normalize(ray.directionAndMaxT.xyz, vec3<f32>(0.0, 1.0, 0.0));
    let pixel = receiver_pixel(rayId, ray);
    let receiverNormal = safe_normalize(textureLoad(geometryNormalTexture, pixel, 0).xyz, -rayDir);
    let receiverAlbedo = clamp(textureLoad(baseColorTexture, pixel, 0).rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let hitNormal = safe_normalize(hit.worldNormalAndStepCount.xyz, -rayDir);
    let materialReferenceValid = (hit.surfaceMaterialReference.w & (1u << 31u)) != 0u;
    let materialId = select(hit.traceInfo.y, hit.surfaceMaterialReference.x, materialReferenceValid);
    let materialUv = select(hit.localUVWAndAux.xy, vec2<f32>(bitcast<f32>(hit.surfaceMaterialReference.y), bitcast<f32>(hit.surfaceMaterialReference.z)), materialReferenceValid);
    let materialAlbedo = evaluate_surface_albedo(hit, receiverAlbedo);

    var flags = TRACE_SHADING_FLAG_VALID | TRACE_SHADING_FLAG_SCREEN_TRACE_MISS | (TRACE_SHADING_SOURCE_SDF << 24u);
    if (materialReferenceValid) {
        flags |= TRACE_SHADING_FLAG_SURFACE_CACHE_MATERIAL;
    } else {
        flags |= TRACE_SHADING_FLAG_FALLBACK_MATERIAL;
    }

    traceShading[rayId] = TraceShadingParameters(
        vec4<f32>(hit.worldPositionAndDistance.xyz, hit.worldPositionAndDistance.w),
        vec4<f32>(hitNormal, 1.0),
        vec4<f32>(materialUv, hit.localUVWAndAux.z, hit.localUVWAndAux.w),
        vec4<f32>(materialAlbedo.rgb, materialAlbedo.a),
        vec4<f32>(ray.originAndMinT.xyz, 0.15915494),
        vec4<f32>(receiverNormal, 1.0),
        vec4<f32>(receiverAlbedo, 1.0),
        vec4<f32>(rayDir, 0.0),
        vec4<u32>(materialId, hit.traceInfo.z, ray.metadata.w, flags));
}
