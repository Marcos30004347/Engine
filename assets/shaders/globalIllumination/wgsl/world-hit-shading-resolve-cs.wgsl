const INVALID_INDEX : u32 = 0xffffffffu;
const MATERIAL_INVALID_TEXTURE_ID : u32 = 0xffffffffu;

#include "globalIllumination/wgsl/trace-shading-types.wgsl"

struct WorldHitShadingResolveUniforms {
    controls : vec4<u32>,
};

struct TraceRay {
    originTMin : vec4<f32>,
    directionTMax : vec4<f32>,
};

struct TraceHit {
    instanceId : u32,
    triangleId : u32,
    hitT : f32,
    baryU : f32,
    baryV : f32,
    tlasInstanceIndex : u32,
    flags : u32,
    _padding0 : u32,
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

struct InstanceGPU {
    transform : mat4x4<f32>,
    inverseTransform : mat4x4<f32>,
    previousTransform : mat4x4<f32>,
    aabbMin : vec3<f32>,
    blasHandle : u32,
    aabbMax : vec3<f32>,
    sceneInstanceId : u32,
    flags : u32,
    _padding : array<u32, 3>,
};

struct TriangleBLASMetadata {
    handleId : u32,
    cwbvhNodeOffset : u32,
    cwbvhNodeCount : u32,
    leafRemapOffset : u32,
    primitiveRemapOffset : u32,
    primitiveCount : u32,
    triangleTransformOffset : u32,
    vertexOffset : u32,
    indexOffset : u32,
    primitiveMaterialOffset : u32,
};

struct FallbackMeshVertex {
    positionX : f32,
    positionY : f32,
    positionZ : f32,
    normalX : f32,
    normalY : f32,
    normalZ : f32,
    uvX : f32,
    uvY : f32,
    boneIndices : vec4<u32>,
    boneWeights : vec4<f32>,
    surfaceBaseColorOpacity : vec4<f32>,
    surfaceNormalRoughness : vec4<f32>,
    surfaceEmissiveMetallic : vec4<f32>,
    surfaceFlags : vec4<u32>,
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

@group(0) @binding(0) var<uniform> uniforms : WorldHitShadingResolveUniforms;
@group(0) @binding(1) var<storage, read> rayInput : array<TraceRay>;
@group(0) @binding(2) var<storage, read> hitOutput : array<TraceHit>;
@group(0) @binding(3) var<storage, read> failedRays : FailedRayBuffer;
@group(0) @binding(4) var<storage, read> tlasInstances : array<InstanceGPU>;
@group(0) @binding(5) var<storage, read> blasMetadata : array<TriangleBLASMetadata>;
@group(0) @binding(6) var<storage, read> fallbackVertices : array<FallbackMeshVertex>;
@group(0) @binding(7) var<storage, read> fallbackIndices : array<u32>;
@group(0) @binding(8) var<storage, read_write> worldHitShading : array<TraceShadingParameters>;
@group(0) @binding(9) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(10) var baseColorTexture : texture_2d<f32>;
@group(0) @binding(11) var<storage, read> materialRuntime : array<MaterialRuntimeData>;
@group(0) @binding(12) var<uniform> atlasInfo : AtlasInfo;
@group(0) @binding(13) var<storage, read> textureEntries : array<VirtualTextureEntry>;
@group(0) @binding(14) var<storage, read> vtPageTable : array<VirtualTexturePageTableEntry>;
@group(0) @binding(15) var<storage, read> vtPhysicalPages : array<u32>;
@group(0) @binding(16) var<storage, read_write> feedbackBuffer : FeedbackBuffer;
@group(0) @binding(17) var<storage, read> primitiveMaterialIds : array<u32>;

fn vertex_position(v : FallbackMeshVertex) -> vec3<f32> {
    return vec3<f32>(v.positionX, v.positionY, v.positionZ);
}

fn vertex_normal(v : FallbackMeshVertex) -> vec3<f32> {
    return vec3<f32>(v.normalX, v.normalY, v.normalZ);
}

fn vertex_uv(v : FallbackMeshVertex) -> vec2<f32> {
    return vec2<f32>(v.uvX, v.uvY);
}

fn safe_normalize(v : vec3<f32>, fallback : vec3<f32>) -> vec3<f32> {
    let len = length(v);
    return select(fallback, v / len, len > 1.0e-6);
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

fn evaluate_surface_albedo(materialId : u32, uv : vec2<f32>, receiverAlbedo : vec3<f32>) -> vec4<f32> {
    if (materialId >= arrayLength(&materialRuntime)) {
        return vec4<f32>(receiverAlbedo, 1.0);
    }

    let runtime = materialRuntime[materialId];
    var baseColor = runtime.baseColorFactor;
    if (runtime.baseColorTextureId != MATERIAL_INVALID_TEXTURE_ID) {
        requestVirtualPage(runtime.baseColorTextureId, uv, 0u);
        baseColor = baseColor * sampleFinestResidentVirtualTextureTexel(runtime.baseColorTextureId, uv);
    }
    return vec4<f32>(clamp(baseColor.rgb, vec3<f32>(0.0), vec3<f32>(8.0)), clamp(runtime.pbrFactors.x, 0.0, 1.0));
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) gid : vec3<u32>) {
    let rayId = gid.x;
    if (rayId >= uniforms.controls.y) {
        return;
    }

    if (uniforms.controls.x == 0u) {
        worldHitShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let hit = hitOutput[rayId];
    if (hit.instanceId == INVALID_INDEX ||
        hit.triangleId == INVALID_INDEX ||
        hit.tlasInstanceIndex == INVALID_INDEX ||
        hit.hitT <= 0.0) {
        worldHitShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let instance = tlasInstances[hit.tlasInstanceIndex];
    if (instance.blasHandle == INVALID_INDEX) {
        worldHitShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let metadata = blasMetadata[instance.blasHandle];
    if (metadata.handleId == INVALID_INDEX || hit.triangleId >= metadata.primitiveCount) {
        worldHitShading[rayId] = invalid_trace_shading_parameters();
        return;
    }

    let indexBase = metadata.indexOffset + hit.triangleId * 3u;
    let i0 = metadata.vertexOffset + fallbackIndices[indexBase + 0u];
    let i1 = metadata.vertexOffset + fallbackIndices[indexBase + 1u];
    let i2 = metadata.vertexOffset + fallbackIndices[indexBase + 2u];
    let v0 = fallbackVertices[i0];
    let v1 = fallbackVertices[i1];
    let v2 = fallbackVertices[i2];

    let bary = vec3<f32>(1.0 - hit.baryU - hit.baryV, hit.baryU, hit.baryV);
    let localPosition =
        vertex_position(v0) * bary.x +
        vertex_position(v1) * bary.y +
        vertex_position(v2) * bary.z;
    let localNormal =
        vertex_normal(v0) * bary.x +
        vertex_normal(v1) * bary.y +
        vertex_normal(v2) * bary.z;
    let uv =
        vertex_uv(v0) * bary.x +
        vertex_uv(v1) * bary.y +
        vertex_uv(v2) * bary.z;

    let ray = rayInput[rayId];
    let rayDir = safe_normalize(ray.directionTMax.xyz, vec3<f32>(0.0, 1.0, 0.0));
    var receiverPixel = vec2<i32>(0, 0);
    if (uniforms.controls.w != 0u) {
        receiverPixel = vec2<i32>(i32(rayId % uniforms.controls.w), i32(rayId / uniforms.controls.w));
    } else {
        let failedRay = failedRays.rays[rayId];
        receiverPixel = vec2<i32>(failedRay.pixelAndRay.xy);
    }
    let receiverNormal = safe_normalize(textureLoad(geometryNormalTexture, receiverPixel, 0).xyz, -rayDir);
    let receiverAlbedo = clamp(textureLoad(baseColorTexture, receiverPixel, 0).rgb, vec3<f32>(0.0), vec3<f32>(8.0));
    let uniformHemispherePdf = 0.15915494;
    let rayHitPosition = ray.originTMin.xyz + rayDir * hit.hitT;
    let transformedPosition = (instance.transform * vec4<f32>(localPosition, 1.0)).xyz;
    let position = select(rayHitPosition, transformedPosition, all(transformedPosition == transformedPosition));
    let worldNormal = safe_normalize((transpose(instance.inverseTransform) * vec4<f32>(localNormal, 0.0)).xyz, -rayDir);

    let materialLookupIndex = metadata.primitiveMaterialOffset + hit.triangleId;
    var materialId = 0u;
    if (materialLookupIndex < arrayLength(&primitiveMaterialIds)) {
        materialId = primitiveMaterialIds[materialLookupIndex];
    }
    let materialAlbedo = evaluate_surface_albedo(materialId, uv, receiverAlbedo);

    worldHitShading[rayId] = TraceShadingParameters(
        vec4<f32>(position, hit.hitT),
        vec4<f32>(worldNormal, 1.0),
        vec4<f32>(uv, hit.baryU, hit.baryV),
        vec4<f32>(materialAlbedo.rgb, materialAlbedo.a),
        vec4<f32>(ray.originTMin.xyz, uniformHemispherePdf),
        vec4<f32>(receiverNormal, 1.0),
        vec4<f32>(receiverAlbedo, 1.0),
        vec4<f32>(rayDir, 0.0),
        vec4<u32>(materialId, hit.triangleId, hit.tlasInstanceIndex, hit.flags | TRACE_SHADING_FLAG_VALID | TRACE_SHADING_FLAG_SURFACE_CACHE_MATERIAL | (TRACE_SHADING_SOURCE_BVH << 24u)));
}
