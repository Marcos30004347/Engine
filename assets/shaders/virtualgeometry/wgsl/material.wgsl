#include "virtualgeometrydata.wgsl"
#include "material/wgsl/material-gbuffer-types.wgsl"

const MATERIAL_WORKGROUP_SIZE : u32 = 16u;
const MATERIAL_INVALID_TEXTURE_ID : u32 = 0xFFFFFFFFu;

struct MaterialPassUniforms {
    viewportWidth : u32,
    viewportHeight : u32,
    tileSize : u32,
    tilesX : u32,
    tilesY : u32,
    tileCount : u32,
    wordsPerMaterial : u32,
    tileWorkgroupCountX : u32,
    tileWorkgroupCountY : u32,
    materialCount : u32,
    feedbackStride : u32,
    feedbackRequestCapacity : u32,
    displayViewportWidth : u32,
    displayViewportHeight : u32,
    temporalMipBias : f32,
    temporalRenderScale : f32,
    _padding0 : vec2<f32>,
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
}

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
}

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
}

struct VirtualTexturePageTableEntry {
    physicalPage : u32,
    flags : u32,
    _padding0 : u32,
    _padding1 : u32,
}

struct FeedbackRequest {
    textureIndex : u32,
    mipLevel : u32,
    pageX : u32,
    pageY : u32,
}

struct FeedbackBuffer {
    requestCount : atomic<u32>,
    overflowCount : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
    requests : array<FeedbackRequest>,
}

struct ResolvedVirtualPage {
    physicalPage : u32,
    mipLevel : u32,
    pixelX : u32,
    pixelY : u32,
    valid : bool,
}

// The packed identity both rasterizers emit: full instance index in the low
// word, the visibility payload (globalCluster << 7 | triangle) in the high
// word. Geometry resolves through decodedClusterGeometryInfos[globalCluster].
struct MaterialGeometryIds {
    instanceIndex : u32,
    globalCluster : u32,
    triangleId : u32,
}

struct MaterialSurfaceInput {
    pixelCoords : vec2<u32>,
    materialId : u32,
    geometryIds : MaterialGeometryIds,
    uv : vec2<f32>,
    geometryNormal : vec3<f32>,
    valid : bool,
}

struct MaterialPixelContext {
    pixelCoords : vec2<u32>,
    materialId : u32,
    valid : bool,
}

@group(0) @binding(0) var<uniform> materialPassUniforms : MaterialPassUniforms;
@group(0) @binding(1) var<storage, read> materialRuntime : array<MaterialRuntimeData>;
@group(0) @binding(2) var<storage, read> materialTileList : array<u32>;
@group(0) @binding(3) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(4) var materialUVTexture : texture_2d<f32>;
@group(0) @binding(5) var shadowLightingTexture : texture_2d<f32>;
@group(0) @binding(6) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(7) var packedGeometryIdsLoTexture : texture_2d<u32>;
@group(0) @binding(8) var packedGeometryIdsHiTexture : texture_2d<u32>;
@group(0) @binding(9) var geometryNormalTexture : texture_2d<f32>;
@group(0) @binding(10) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(11) var baseColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(12) var<uniform> atlasInfo : AtlasInfo;
@group(0) @binding(13) var<storage, read> textureEntries : array<VirtualTextureEntry>;
@group(0) @binding(14) var<storage, read> vtPageTable : array<VirtualTexturePageTableEntry>;
@group(0) @binding(15) var<storage, read> vtPhysicalPages : array<u32>;
@group(0) @binding(16) var<storage, read_write> feedbackBuffer : FeedbackBuffer;
@group(0) @binding(17) var<storage, read> instances : array<InstanceData>;
@group(0) @binding(18) var<storage, read> decodedClusterGeometryInfos : array<DecodedClusterGeometryInfo>;
@group(0) @binding(19) var<storage, read> decodedVertexBuffer : array<DecodedVertexData>;
@group(0) @binding(20) var normalTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(21) var materialPropertiesTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(22) var emissiveTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(23) var<storage, read> decodedIndexBuffer : array<u32>;

fn unpackGeometryIds(pixelCoords: vec2<u32>) -> MaterialGeometryIds {
    let lo = textureLoad(packedGeometryIdsLoTexture, vec2<i32>(pixelCoords), 0).x;
    let hi = textureLoad(packedGeometryIdsHiTexture, vec2<i32>(pixelCoords), 0).x;

    var ids : MaterialGeometryIds;
    ids.instanceIndex = lo;
    ids.globalCluster = visibilityPayloadCluster(hi);
    ids.triangleId = visibilityPayloadTriangle(hi);
    return ids;
}

fn material_tile_index(workgroupId: vec3<u32>) -> u32 {
    return materialTileList[materialRuntime[0].tileListBase + workgroupId.z];
}

fn material_tile_origin(workgroupId: vec3<u32>) -> vec2<u32> {
    let tileIndex = material_tile_index(workgroupId);
    return vec2<u32>(tileIndex % materialPassUniforms.tilesX, tileIndex / materialPassUniforms.tilesX) * materialPassUniforms.tileSize;
}

fn material_pixel_coords(workgroupId: vec3<u32>, localInvocationId: vec3<u32>) -> vec2<u32> {
    let tileOrigin = material_tile_origin(workgroupId);
    return tileOrigin
        + vec2<u32>(workgroupId.xy) * MATERIAL_WORKGROUP_SIZE
        + localInvocationId.xy;
}

fn material_get_depth_value(pixelCoords: vec2<u32>) -> f32 {
    return textureLoad(sceneDepthTexture, vec2<i32>(pixelCoords), 0);
}

fn material_make_context(workgroupId: vec3<u32>, localInvocationId: vec3<u32>) -> MaterialPixelContext {
    var context : MaterialPixelContext;
    context.pixelCoords = material_pixel_coords(workgroupId, localInvocationId);
    context.materialId = materialRuntime[0].materialId;
    context.valid = false;

    if (context.pixelCoords.x >= materialPassUniforms.viewportWidth || context.pixelCoords.y >= materialPassUniforms.viewportHeight) {
        return context;
    }

    if (material_get_depth_value(context.pixelCoords) <= 0.0) {
        return context;
    }

    let materialId = textureLoad(materialIdTexture, vec2<i32>(context.pixelCoords), 0).x;
    if (materialId != materialRuntime[0].materialId) {
        return context;
    }

    context.valid = true;
    return context;
}

fn material_get_uv_value(pixelCoords: vec2<u32>) -> vec2<f32> {
    return textureLoad(materialUVTexture, vec2<i32>(pixelCoords), 0).xy;
}

fn material_get_shadow_factor(pixelCoords: vec2<u32>) -> vec4<f32> {
    return max(textureLoad(shadowLightingTexture, vec2<i32>(pixelCoords), 0), vec4<f32>(0.0));
}

fn material_get_instance_index(pixelCoords: vec2<u32>) -> u32 {
    return unpackGeometryIds(pixelCoords).instanceIndex;
}

fn material_make_screen_surface_input(pixelCoords: vec2<u32>) -> MaterialSurfaceInput {
    var input : MaterialSurfaceInput;
    input.pixelCoords = pixelCoords;
    input.materialId = textureLoad(materialIdTexture, vec2<i32>(pixelCoords), 0).x;
    input.geometryIds = unpackGeometryIds(pixelCoords);
    input.uv = material_get_uv_value(pixelCoords);
    input.geometryNormal = getGeometryNormal(pixelCoords);
    input.valid = material_get_depth_value(pixelCoords) > 0.0;
    return input;
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
    let width = max(mipInfo.width, 1u);
    let height = max(mipInfo.height, 1u);
    return vec2<u32>(
        min(u32(wrapped.x * f32(width)), width - 1u),
        min(u32(wrapped.y * f32(height)), height - 1u)
    );
}

fn textureMipTexel(textureIndex: u32, mipLevel: u32, uv: vec2<f32>) -> vec2<f32> {
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    let wrapped = sampleUV(textureIndex, uv);
    let width = f32(max(mipInfo.width, 1u));
    let height = f32(max(mipInfo.height, 1u));
    return vec2<f32>(wrapped.x * width - 0.5, wrapped.y * height - 0.5);
}

fn computeRequestedMip(textureIndex: u32, pixelCoords: vec2<u32>, uv: vec2<f32>) -> u32 {
    if (textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }

    let nextX = vec2<u32>(min(pixelCoords.x + 1u, materialPassUniforms.viewportWidth - 1u), pixelCoords.y);
    let nextY = vec2<u32>(pixelCoords.x, min(pixelCoords.y + 1u, materialPassUniforms.viewportHeight - 1u));

    let uvDx = material_get_uv_value(nextX) - uv;
    let uvDy = material_get_uv_value(nextY) - uv;

    let baseMip = textureEntries[textureIndex].mips[0];
    let texelScale = vec2<f32>(f32(max(baseMip.width, 1u)), f32(max(baseMip.height, 1u)));
    let dx = uvDx * texelScale;
    let dy = uvDy * texelScale;
    let footprint = max(dot(dx, dx), dot(dy, dy));
    let temporalScale = clamp(materialPassUniforms.temporalRenderScale, 0.01, 1.0);
    let tsrMipBias = select(0.0, materialPassUniforms.temporalMipBias, temporalScale < 0.999);
    let unclampedMip = max(0.0, floor(0.5 * log2(max(footprint, 1e-8))) + textureEntries[textureIndex].mipBias + tsrMipBias);
    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    let minMip = min(textureEntries[textureIndex].minMip, highestMip);
    let maxMip = min(max(textureEntries[textureIndex].maxMip, minMip), highestMip);
    return u32(clamp(unclampedMip, f32(minMip), f32(maxMip)));
}

fn requestVirtualPage(textureIndex: u32, uv: vec2<f32>, pixelCoords: vec2<u32>, requestedMip: u32) {
    let stride = max(materialPassUniforms.feedbackStride, 1u);
    if ((pixelCoords.x % stride) != 0u || (pixelCoords.y % stride) != 0u) {
        return;
    }

    let mipInfo = textureEntries[textureIndex].mips[requestedMip];
    if (mipInfo.pagesX == 0u || mipInfo.pagesY == 0u) {
        return;
    }

    let mipPixel = textureMipPixel(textureIndex, requestedMip, uv);
    let requestedPageX = min(mipPixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u);
    let requestedPageY = min(mipPixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u);
    let pageTableIndex = mipInfo.pageTableOffset + requestedPageY * mipInfo.pagesX + requestedPageX;
    if (pageTableIndex < arrayLength(&vtPageTable)) {
        let pageEntry = vtPageTable[pageTableIndex];
        if ((pageEntry.flags & 0x1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
            return;
        }
    }

    let requestIndex = atomicAdd(&feedbackBuffer.requestCount, 1u);
    if (requestIndex >= materialPassUniforms.feedbackRequestCapacity) {
        atomicAdd(&feedbackBuffer.overflowCount, 1u);
        return;
    }

    feedbackBuffer.requests[requestIndex] = FeedbackRequest(textureIndex, requestedMip, requestedPageX, requestedPageY);
}

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
            if ((pageEntry.flags & 0x1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
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
    let packed = vtPhysicalPages[texelIndex];
    return vec4<f32>(
        f32(packed & 0xFFu) / 255.0,
        f32((packed >> 8u) & 0xFFu) / 255.0,
        f32((packed >> 16u) & 0xFFu) / 255.0,
        f32((packed >> 24u) & 0xFFu) / 255.0
    );
}

fn sampleVirtualTextureAtMip(textureIndex: u32, uv: vec2<f32>, mipLevel: u32) -> vec4<f32> {
    let resolved = resolveVirtualPageFromMip(textureIndex, uv, mipLevel);
    if (!resolved.valid) {
        return vec4<f32>(0.0);
    }
    return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
}

fn sampleVirtualTexture(textureIndex: u32, pixelCoords: vec2<u32>, uv: vec2<f32>) -> vec4<f32> {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries)) {
        return vec4<f32>(0.0);
    }

    let requestedMip = computeRequestedMip(textureIndex, pixelCoords, uv);
    requestVirtualPage(textureIndex, uv, pixelCoords, requestedMip);

    let resolved = resolveVirtualPageFromMip(textureIndex, uv, requestedMip);
    if (!resolved.valid) {
        return vec4<f32>(0.0);
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
        1.0 / f32(max(mipInfo.height, 1u))
    );

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

fn getGeometryNormal(pixelCoords: vec2<u32>) -> vec3<f32> {
    let sampledNormal = textureLoad(geometryNormalTexture, vec2<i32>(pixelCoords), 0).xyz;
    let sampledLength = length(sampledNormal);
    if (sampledLength > 1e-6) {
        return sampledNormal / sampledLength;
    }
    return vec3<f32>(0.0, 0.0, 1.0);
}

fn layerTextureNormalForSurface(input: MaterialSurfaceInput, normalTextureId: u32) -> vec3<f32> {
    let geometryNormal = input.geometryNormal;
    if (normalTextureId == MATERIAL_INVALID_TEXTURE_ID || normalTextureId >= arrayLength(&textureEntries)) {
        return geometryNormal;
    }

    let ids = input.geometryIds;
    if (ids.globalCluster >= arrayLength(&decodedClusterGeometryInfos) ||
        ids.instanceIndex >= arrayLength(&instances)) {
        return geometryNormal;
    }

    let clusterGeometry = decodedClusterGeometryInfos[ids.globalCluster];
    if (ids.triangleId >= clusterGeometry.triangleCount) {
        return geometryNormal;
    }
    let triangleBase = clusterGeometry.indexOffset + ids.triangleId * 3u;
    let i0 = u32(decodedIndexBuffer[triangleBase + 0u]);
    let i1 = u32(decodedIndexBuffer[triangleBase + 1u]);
    let i2 = u32(decodedIndexBuffer[triangleBase + 2u]);
    let v0 = decodedVertexBuffer[clusterGeometry.vertexOffset + i0];
    let v1 = decodedVertexBuffer[clusterGeometry.vertexOffset + i1];
    let v2 = decodedVertexBuffer[clusterGeometry.vertexOffset + i2];

    let localPos0 = vec3<f32>(v0.positionX, v0.positionY, v0.positionZ);
    let localPos1 = vec3<f32>(v1.positionX, v1.positionY, v1.positionZ);
    let localPos2 = vec3<f32>(v2.positionX, v2.positionY, v2.positionZ);
    let uv0 = vec2<f32>(v0.uvX, v0.uvY);
    let uv1 = vec2<f32>(v1.uvX, v1.uvY);
    let uv2 = vec2<f32>(v2.uvX, v2.uvY);

    let edge1 = localPos1 - localPos0;
    let edge2 = localPos2 - localPos0;
    let deltaUv1 = uv1 - uv0;
    let deltaUv2 = uv2 - uv0;
    let det = deltaUv1.x * deltaUv2.y - deltaUv1.y * deltaUv2.x;
    if (abs(det) <= 1e-6) {
        return geometryNormal;
    }

    let invDet = 1.0 / det;
    var localTangent = (edge1 * deltaUv2.y - edge2 * deltaUv1.y) * invDet;
    var localBitangent = (edge2 * deltaUv1.x - edge1 * deltaUv2.x) * invDet;
    if (length(localTangent) <= 1e-6 || length(localBitangent) <= 1e-6) {
        return geometryNormal;
    }

    let modelMatrix = instances[ids.instanceIndex].modelMatrix;
    var tangent = normalize((modelMatrix * vec4<f32>(localTangent, 0.0)).xyz);
    var bitangent = normalize((modelMatrix * vec4<f32>(localBitangent, 0.0)).xyz);
    let normal = normalize(geometryNormal);

    tangent = normalize(tangent - normal * dot(normal, tangent));
    if (length(tangent) <= 1e-6) {
        return geometryNormal;
    }

    let bitangentSign = select(-1.0, 1.0, dot(cross(normal, tangent), bitangent) >= 0.0);
    bitangent = normalize(cross(normal, tangent)) * bitangentSign;

    let sampledNormal = sampleVirtualTexture(normalTextureId, input.pixelCoords, input.uv).xyz * 2.0 - vec3<f32>(1.0, 1.0, 1.0);
    if (length(sampledNormal) <= 1e-6) {
        return geometryNormal;
    }

    let layeredNormal = normalize(
        tangent * sampledNormal.x +
        bitangent * sampledNormal.y +
        normal * sampledNormal.z
    );
    if (length(layeredNormal) <= 1e-6) {
        return geometryNormal;
    }
    return layeredNormal;
}

fn layerTextureNormal(pixelCoords: vec2<u32>, geometryNormal: vec3<f32>, normalTextureId: u32) -> vec3<f32> {
    var input = material_make_screen_surface_input(pixelCoords);
    input.geometryNormal = geometryNormal;
    return layerTextureNormalForSurface(input, normalTextureId);
}

fn material_get_base_color_for_surface(input: MaterialSurfaceInput) -> vec4<f32> {
    let runtime = materialRuntime[0];
    var baseColor = material_gbuffer_make_default().baseColorOpacity;
    if (runtime.baseColorTextureId != MATERIAL_INVALID_TEXTURE_ID) {
        baseColor = sampleVirtualTexture(runtime.baseColorTextureId, input.pixelCoords, input.uv);
    }
    baseColor *= runtime.baseColorFactor;
    baseColor.a *= runtime.pbrFactors.z;
    return baseColor;
}

fn material_get_base_color(pixelCoords: vec2<u32>) -> vec4<f32> {
    return material_get_base_color_for_surface(material_make_screen_surface_input(pixelCoords));
}

fn material_get_normal_value_for_surface(input: MaterialSurfaceInput) -> vec3<f32> {
    let runtime = materialRuntime[0];
    if (runtime.hasNormalTexture != 0u) {
        return layerTextureNormalForSurface(input, runtime.normalTextureId);
    }
    return input.geometryNormal;
}

fn material_get_normal_value(pixelCoords: vec2<u32>) -> vec3<f32> {
    return material_get_normal_value_for_surface(material_make_screen_surface_input(pixelCoords));
}

fn material_evaluate_gbuffer_for_surface(input: MaterialSurfaceInput) -> MaterialGBufferOutput {
    let runtime = materialRuntime[0];
    var materialFlags = MATERIAL_GBUFFER_FLAG_NONE;
    if (runtime.baseColorTextureId != MATERIAL_INVALID_TEXTURE_ID) {
        materialFlags |= MATERIAL_GBUFFER_FLAG_HAS_BASE_COLOR_TEXTURE;
    }
    if (runtime.hasNormalTexture != 0u) {
        materialFlags |= MATERIAL_GBUFFER_FLAG_HAS_NORMAL_TEXTURE;
    }

    return material_gbuffer_make(
        material_get_base_color_for_surface(input),
        material_get_normal_value_for_surface(input),
        runtime.pbrFactors.x,
        runtime.pbrFactors.y,
        runtime.emissiveFactorAndStrength.rgb * runtime.emissiveFactorAndStrength.a,
        materialFlags
    );
}

fn material_evaluate_gbuffer(pixelCoords: vec2<u32>) -> MaterialGBufferOutput {
    return material_evaluate_gbuffer_for_surface(material_make_screen_surface_input(pixelCoords));
}

fn material_evaluate_direct_only(materialOutput: MaterialGBufferOutput, pixelCoords: vec2<u32>) -> vec4<f32> {
    let shadowFactor = material_get_shadow_factor(pixelCoords);
    return material_gbuffer_direct_only_color(materialOutput, shadowFactor.rgb);
}

fn material_sample_texture_uv(textureIndex: u32, pixelCoords: vec2<u32>, uv: vec2<f32>) -> vec4<f32> {
    return sampleVirtualTexture(textureIndex, pixelCoords, uv);
}

fn material_sample_texture(pixelCoords: vec2<u32>, textureIndex: u32) -> vec4<f32> {
    return material_sample_texture_uv(textureIndex, pixelCoords, material_get_uv_value(pixelCoords));
}

fn material_store_gbuffer_outputs(pixelCoords: vec2<u32>, materialOutput: MaterialGBufferOutput) {
    let pixelCoordsI = vec2<i32>(pixelCoords);
    textureStore(baseColorTexture, pixelCoordsI, materialOutput.baseColorOpacity);
    textureStore(normalTexture, pixelCoordsI, vec4<f32>(material_gbuffer_normal(materialOutput), materialOutput.normalRoughness.w));
    textureStore(
        materialPropertiesTexture,
        pixelCoordsI,
        vec4<f32>(
            materialOutput.normalRoughness.w,
            materialOutput.emissiveMetallic.w,
            materialOutput.baseColorOpacity.a,
            f32(materialOutput.flags.x)
        )
    );
    textureStore(emissiveTexture, pixelCoordsI, vec4<f32>(materialOutput.emissiveMetallic.rgb, 1.0));
}

fn material_store_output(pixelCoords: vec2<u32>, color: vec4<f32>) {
    textureStore(outputTexture, vec2<i32>(pixelCoords), color);
}
