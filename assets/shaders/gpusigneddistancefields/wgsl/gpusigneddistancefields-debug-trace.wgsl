#include "gpusigneddistancefields_common.wgsl"

struct TraceDebugUniforms {
    inverseViewProjection : mat4x4<f32>,
    cameraPosition : vec3<f32>,
    maxTraceDistance : f32,
    outputWidth : u32,
    outputHeight : u32,
    mode : u32,
    maxSteps : u32,
    atlasBricksPerAxis : u32,
    cascadeCount : u32,
    cascadeOverride : i32,
    reverseZ : u32,
    hitThresholdScale : f32,
    emptySpaceStepScale : f32,
    enabled : u32,
    stepCountColor : vec3<f32>,
    surfaceCachePageResolution : u32,
    surfaceCachePagesPerRow : u32,
    feedbackRequestCapacity : u32,
    localTraceStatsEnabled : u32,
    _padding3 : u32,
    traceHeatmapLowColor : vec4<f32>,
    traceHeatmapMidColor : vec4<f32>,
    traceHeatmapHighColor : vec4<f32>,
    viewProjection : mat4x4<f32>,
}

const MATERIAL_INVALID_TEXTURE_ID : u32 = 0xffffffffu;

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
}

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

struct VSDFRuntimeInfo {
    instanceCount : u32,
    cardCount : u32,
    pageCount : u32,
    pageSize : u32,
    sheetWidth : u32,
    sheetHeight : u32,
    sheetCount : u32,
    cascadeCount : u32,
}

struct VSDFInstance {
    localToWorld : mat4x4<f32>,
    worldToLocal : mat4x4<f32>,
    localBoundsMin : vec3<f32>,
    meshIndex : u32,
    localBoundsMax : vec3<f32>,
    firstCard : u32,
    cardCount : u32,
    firstSdfChunk : u32,
    sdfChunkCount : u32,
    flags : u32,
    worldBoundsMin : vec3<f32>,
    narrowBandDistance : f32,
    worldBoundsMax : vec3<f32>,
    _padding1 : u32,
}

struct VSDFCard {
    localBoundsMin : vec3<f32>,
    firstPage : u32,
    localBoundsMax : vec3<f32>,
    pageCount : u32,
    instanceIndex : u32,
    resolutionX : u32,
    resolutionY : u32,
    mipCount : u32,
    projectionAxis : u32,
    projectionSign : u32,
    layoutId : u32,
    flags : u32,
}

struct VSDFPage {
    cardIndex : u32,
    mipLevel : u32,
    pageX : u32,
    pageY : u32,
    sheetIndex : u32,
    sheetPageX : u32,
    sheetPageY : u32,
    flags : u32,
}

struct VSDFCascade {
    worldBoundsMin : vec3<f32>,
    firstInstance : u32,
    worldBoundsMax : vec3<f32>,
    instanceCount : u32,
}

struct VSDFSdfChunk {
    byteOffset : u32,
    byteCount : u32,
    brickResolutionX : u32,
    brickCount : u32,
    flags : u32,
    brickResolutionY : u32,
    brickResolutionZ : u32,
    _padding2 : u32,
}

struct VSDFSparseTLASInfo {
    worldBoundsMin : vec3<f32>,
    cellSize : f32,
    fineDimX : u32,
    fineDimY : u32,
    fineDimZ : u32,
    pageDim : u32,
    pageDimX : u32,
    pageDimY : u32,
    pageDimZ : u32,
    pageCount : u32,
    cellCount : u32,
    refCount : u32,
    _padding0 : u32,
    _padding1 : u32,
}

struct VSDFSparseTLASCell {
    firstRef : u32,
    refCount : u32,
}

struct VSDFTraceHit {
    hit : bool,
    t : f32,
    instanceIndex : u32,
    stepCount : u32,
    maxStepsReached : u32,
    normal : vec3<f32>,
}

struct VSDFSdfSample {
    distance : f32,
    encodedDistance : f32,
    validity : f32,
}

@group(0) @binding(0) var<uniform> gUniforms : TraceDebugUniforms;
@group(0) @binding(1) var<storage, read> cascadeInfos : array<CascadeInfoGPU>;
@group(0) @binding(3) var sdfAtlasTexture : texture_3d<f32>;
@group(0) @binding(13) var sdfAtlasSampler : sampler;
@group(0) @binding(4) var<storage, read> combinedBrickMap0 : array<u32>;
@group(0) @binding(5) var<storage, read> combinedBrickMap1 : array<u32>;
@group(0) @binding(6) var<storage, read> combinedBrickMap2 : array<u32>;
@group(0) @binding(7) var<storage, read> combinedBrickMap3 : array<u32>;
@group(0) @binding(8) var<storage, read> combinedBrickMap4 : array<u32>;
@group(0) @binding(9) var<storage, read> combinedBrickMap5 : array<u32>;
@group(0) @binding(10) var<storage, read> combinedBrickMap6 : array<u32>;
@group(0) @binding(11) var<storage, read> combinedBrickMap7 : array<u32>;
@group(0) @binding(12) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(14) var<storage, read> contextBrickAabb : array<u32>;
@group(0) @binding(15) var<storage, read> combinedAabbTree0 : array<u32>;
@group(0) @binding(16) var<storage, read> combinedAabbTree1 : array<u32>;
@group(0) @binding(17) var<storage, read> combinedAabbTree2 : array<u32>;
@group(0) @binding(18) var<storage, read> combinedAabbTree3 : array<u32>;
@group(0) @binding(19) var<storage, read> combinedAabbTree4 : array<u32>;
@group(0) @binding(20) var<storage, read> combinedAabbTree5 : array<u32>;
@group(0) @binding(21) var<storage, read> combinedAabbTree6 : array<u32>;
@group(0) @binding(22) var<storage, read> combinedAabbTree7 : array<u32>;
@group(0) @binding(25) var surfaceCacheMaterialReferenceAtlas : texture_2d<u32>;
@group(0) @binding(26) var<storage, read> materialRuntime : array<MaterialRuntimeData>;
@group(0) @binding(27) var<uniform> atlasInfo : AtlasInfo;
@group(0) @binding(28) var<storage, read> textureEntries : array<VirtualTextureEntry>;
@group(0) @binding(29) var<storage, read> vtPageTable : array<VirtualTexturePageTableEntry>;
@group(0) @binding(30) var<storage, read> vtPhysicalPages : array<u32>;
@group(0) @binding(31) var surfaceCacheMaterialReferenceBasisAtlas : texture_2d<u32>;
@group(0) @binding(32) var<storage, read_write> feedbackBuffer : FeedbackBuffer;
@group(0) @binding(33) var<uniform> vsdfRuntimeInfo : VSDFRuntimeInfo;
@group(0) @binding(34) var<storage, read> vsdfInstances : array<VSDFInstance>;
@group(0) @binding(35) var<storage, read> vsdfCards : array<VSDFCard>;
@group(0) @binding(36) var<storage, read> vsdfPages : array<VSDFPage>;
@group(0) @binding(37) var<storage, read> vsdfAlbedoPages : array<u32>;
@group(0) @binding(38) var<storage, read> vsdfNormalPages : array<u32>;
@group(0) @binding(39) var<storage, read> vsdfCascades : array<VSDFCascade>;
@group(0) @binding(40) var<storage, read> vsdfCascadeInstanceIndices : array<u32>;
@group(0) @binding(41) var<storage, read> vsdfSdfChunks : array<VSDFSdfChunk>;
@group(0) @binding(42) var<storage, read> vsdfSdfWords : array<u32>;
@group(0) @binding(43) var<storage, read_write> localTraceStats : array<atomic<u32>, 18>;
@group(0) @binding(44) var<uniform> vsdfSparseTLASInfo : VSDFSparseTLASInfo;
@group(0) @binding(45) var<storage, read> vsdfSparseTLASPageTable : array<u32>;
@group(0) @binding(46) var<storage, read> vsdfSparseTLASCells : array<VSDFSparseTLASCell>;
@group(0) @binding(47) var<storage, read> vsdfSparseTLASInstanceRefs : array<u32>;

#include "gpusigneddistancefields-trace-common.wgsl"

const LOCAL_TRACE_TOTAL_PIXELS : u32 = 0u;
const LOCAL_TRACE_DISABLED_PIXELS : u32 = 1u;
const LOCAL_TRACE_LOCAL_MODE_PIXELS : u32 = 2u;
const LOCAL_TRACE_NO_CACHE_PIXELS : u32 = 3u;
const LOCAL_TRACE_CACHE_PIXELS : u32 = 4u;
const LOCAL_TRACE_CASCADE_TESTS : u32 = 5u;
const LOCAL_TRACE_CASCADE_HITS : u32 = 6u;
const LOCAL_TRACE_CASCADE_INSTANCE_REFS : u32 = 7u;
const LOCAL_TRACE_INSTANCE_TESTS : u32 = 8u;
const LOCAL_TRACE_INSTANCE_AABB_HITS : u32 = 9u;
const LOCAL_TRACE_SDF_SAMPLES : u32 = 10u;
const LOCAL_TRACE_SDF_HITS : u32 = 11u;
const LOCAL_TRACE_NO_HIT_PIXELS : u32 = 12u;
const LOCAL_TRACE_HIT_PIXELS : u32 = 13u;
const LOCAL_TRACE_ALBEDO_SAMPLES : u32 = 14u;
const LOCAL_TRACE_CASCADE_DEBUG_PIXELS : u32 = 15u;
const LOCAL_TRACE_CASCADE_DEBUG_HITS : u32 = 16u;
const VSDF_SPARSE_TLAS_VISITED_CACHE_SIZE : u32 = 32u;
const VSDF_SDF_CHUNK_FLAG_SPARSE_BRICK8 : u32 = 0x00400000u;
const VSDF_SDF_CHUNK_FLAG_SPARSE_OCTREE_BRICK8 : u32 = 0x00800000u;
const VSDF_SPARSE_BRICK_PAYLOAD_HEADER_BYTES : u32 = 24u;
const VSDF_SPARSE_BRICK_INVALID : u32 = 0xffffffffu;
const VSDF_SPARSE_OCTREE_PAYLOAD_HEADER_BYTES : u32 = 32u;
const VSDF_SPARSE_OCTREE_NODE_LEAF : u32 = 1u;
const VSDF_SPARSE_OCTREE_NODE_RESIDENT : u32 = 2u;

fn local_trace_stats_add(counter: u32, value: u32) {
    if (gUniforms.localTraceStatsEnabled == 0u) {
        return;
    }
    atomicAdd(&localTraceStats[counter], value);
}

struct SDFMaterialReferenceSample {
    reference : vec4<u32>,
    faceIndex : u32,
    pageTexel : vec2<u32>,
}

fn flipped_output_pixel(pixel: vec2<u32>) -> vec2<i32> {
    let flippedY = gUniforms.outputHeight - 1u - pixel.y;
    return vec2<i32>(i32(pixel.x), i32(flippedY));
}

fn step_count_heatmap(t: f32) -> vec3<f32> {
    let x = clamp(t, 0.0, 1.0);
    let lowColor = clamp(gUniforms.traceHeatmapLowColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let midColor = clamp(gUniforms.traceHeatmapMidColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    let highColor = clamp(gUniforms.traceHeatmapHighColor.rgb, vec3<f32>(0.0), vec3<f32>(1.0));
    if (x < 0.5) {
        return mix(lowColor, midColor, x / 0.5);
    }
    return mix(midColor, highColor, (x - 0.5) / 0.5);
}

fn cascade_debug_color(cascadeIndex: u32) -> vec3<f32> {
    let hue = fract(f32(cascadeIndex + 1u) * 0.173);
    let k = vec3<f32>(1.0, 2.0 / 3.0, 1.0 / 3.0);
    let p = abs(fract(vec3<f32>(hue) + k) * 6.0 - 3.0);
    return clamp(p - 1.0, vec3<f32>(0.0), vec3<f32>(1.0));
}

fn hash_debug_color(value: u32) -> vec3<f32> {
    let x = f32(value);
    let r = fract(sin(x * 12.9898 + 0.1234) * 43758.5453);
    let g = fract(sin(x * 78.2330 + 1.2345) * 24634.6345);
    let b = fract(sin(x * 39.4250 + 2.3456) * 56445.2344);
    return 0.2 + 0.8 * vec3<f32>(r, g, b);
}

fn load_surface_cache_material_reference_texel(brickId: u32, faceIndex: u32, pageTexel: vec2<u32>) -> vec4<u32> {
    let pageResolution = gUniforms.surfaceCachePageResolution;
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(
        brickId,
        faceIndex,
        pageTexel,
        pageResolution,
        gUniforms.surfaceCachePagesPerRow);
    return textureLoad(surfaceCacheMaterialReferenceAtlas, vec2<i32>(atlasTexel), 0);
}

fn load_surface_cache_material_reference_basis(brickId: u32, faceIndex: u32, pageTexel: vec2<u32>) -> vec4<f32> {
    let pageResolution = gUniforms.surfaceCachePageResolution;
    let atlasTexel = GPUSignedDistanceFieldsSurfaceCacheTexel(
        brickId,
        faceIndex,
        pageTexel,
        pageResolution,
        gUniforms.surfaceCachePagesPerRow);
    let packed = textureLoad(surfaceCacheMaterialReferenceBasisAtlas, vec2<i32>(atlasTexel), 0);
    return vec4<f32>(bitcast<f32>(packed.x), bitcast<f32>(packed.y), bitcast<f32>(packed.z), bitcast<f32>(packed.w));
}

fn material_reference_valid(reference: vec4<u32>) -> bool {
    return (reference.w & (1u << 31u)) != 0u;
}

fn material_reference_uv(reference: vec4<u32>) -> vec2<f32> {
    return vec2<f32>(bitcast<f32>(reference.y), bitcast<f32>(reference.z));
}

fn material_reference_uv_window(reference: vec4<u32>) -> vec2<f32> {
    let qx = reference.w & 2047u;
    let qy = (reference.w >> 11u) & 2047u;
    let signX = select(1.0, -1.0, (reference.w & (1u << 22u)) != 0u);
    let signY = select(1.0, -1.0, (reference.w & (1u << 23u)) != 0u);
    return vec2<f32>(f32(qx) * signX, f32(qy) * signY) / 2047.0;
}

fn pack_material_reference_uv_window(uvWindowSize: vec2<f32>) -> u32 {
    let width = clamp(u32(ceil(clamp(abs(uvWindowSize.x), 0.0, 1.0) * 2047.0)), 1u, 2047u);
    let height = clamp(u32(ceil(clamp(abs(uvWindowSize.y), 0.0, 1.0) * 2047.0)), 1u, 2047u);
    let signX = select(0u, 1u << 22u, uvWindowSize.x < 0.0);
    let signY = select(0u, 1u << 23u, uvWindowSize.y < 0.0);
    return width | (height << 11u) | signX | signY | (1u << 31u);
}

fn pack_material_reference(materialId: u32, uv: vec2<f32>, uvWindowSize: vec2<f32>) -> vec4<u32> {
    return vec4<u32>(materialId, bitcast<u32>(uv.x), bitcast<u32>(uv.y), pack_material_reference_uv_window(uvWindowSize));
}

fn unwrap_material_reference_uv(baseUv: vec2<f32>, uv: vec2<f32>) -> vec2<f32> {
    let delta = uv - baseUv;
    return baseUv + delta - round(delta);
}

fn filter_surface_cache_material_reference(
    r00: vec4<u32>,
    r10: vec4<u32>,
    r01: vec4<u32>,
    r11: vec4<u32>,
    texelFract: vec2<f32>) -> vec4<u32> {
    if (!material_reference_valid(r00) ||
        !material_reference_valid(r10) ||
        !material_reference_valid(r01) ||
        !material_reference_valid(r11) ||
        r00.x != r10.x ||
        r00.x != r01.x ||
        r00.x != r11.x) {
        return vec4<u32>(0u);
    }

    let uv00 = material_reference_uv(r00);
    let uv10 = unwrap_material_reference_uv(uv00, material_reference_uv(r10));
    let uv01 = unwrap_material_reference_uv(uv00, material_reference_uv(r01));
    let uv11 = unwrap_material_reference_uv(uv00, material_reference_uv(r11));
    let uv = mix(mix(uv00, uv10, texelFract.x), mix(uv01, uv11, texelFract.x), texelFract.y);

    let window00 = material_reference_uv_window(r00);
    let window10 = material_reference_uv_window(r10);
    let window01 = material_reference_uv_window(r01);
    let window11 = material_reference_uv_window(r11);
    let uvWindow = mix(mix(window00, window10, texelFract.x), mix(window01, window11, texelFract.x), texelFract.y);
    return pack_material_reference(r00.x, uv, uvWindow);
}

fn material_reference_at_hit_uv(reference: vec4<u32>, uvBasis: vec4<f32>, faceUv: vec2<f32>, pageTexel: vec2<u32>, pageResolution: u32) -> vec4<u32> {
    if (!material_reference_valid(reference)) {
        return reference;
    }
    let uvRangeOrigin = material_reference_uv(reference);
    let texelLocal = clamp(faceUv * f32(pageResolution) - vec2<f32>(pageTexel), vec2<f32>(0.0), vec2<f32>(1.0));
    let uv = uvRangeOrigin + uvBasis.xy * texelLocal.x + uvBasis.zw * texelLocal.y;
    return vec4<u32>(reference.x, bitcast<u32>(uv.x), bitcast<u32>(uv.y), reference.w);
}

fn sample_hit_material_reference_face(hit: TraceHit, faceIndex: u32) -> SDFMaterialReferenceSample {
    let pageResolution = gUniforms.surfaceCachePageResolution;
    let faceUv = clamp(GPUSignedDistanceFieldsSurfaceCacheFaceUv(faceIndex, hit.uvw), vec2<f32>(0.0), vec2<f32>(0.999999));
    let pageTexel = min(vec2<u32>(faceUv * f32(pageResolution)), vec2<u32>(max(pageResolution, 1u) - 1u));
    let direct = load_surface_cache_material_reference_texel(hit.brickId, faceIndex, pageTexel);
    if (material_reference_valid(direct)) {
        let uvBasis = load_surface_cache_material_reference_basis(hit.brickId, faceIndex, pageTexel);
        return SDFMaterialReferenceSample(material_reference_at_hit_uv(direct, uvBasis, faceUv, pageTexel, pageResolution), faceIndex, pageTexel);
    }

    let maxTexel = i32(pageResolution) - 1i;
    let center = vec2<i32>(pageTexel);
    let maxRadius = min(2i, maxTexel);
    for (var radius = 1i; radius <= maxRadius; radius = radius + 1i) {
        for (var y = -radius; y <= radius; y = y + 1i) {
            for (var x = -radius; x <= radius; x = x + 1i) {
                let texel = center + vec2<i32>(x, y);
                if (all(texel >= vec2<i32>(0i)) && all(texel <= vec2<i32>(maxTexel))) {
                    let reference = load_surface_cache_material_reference_texel(hit.brickId, faceIndex, vec2<u32>(texel));
                    if (material_reference_valid(reference)) {
                        let uvBasis = load_surface_cache_material_reference_basis(hit.brickId, faceIndex, vec2<u32>(texel));
                        return SDFMaterialReferenceSample(material_reference_at_hit_uv(reference, uvBasis, faceUv, vec2<u32>(texel), pageResolution), faceIndex, vec2<u32>(texel));
                    }
                }
            }
        }
    }

    return SDFMaterialReferenceSample(direct, faceIndex, pageTexel);
}

fn sample_hit_material_reference(hit: TraceHit, normal: vec3<f32>) -> SDFMaterialReferenceSample {
    if (gUniforms.surfaceCachePageResolution == 0u || gUniforms.surfaceCachePagesPerRow == 0u || !GPUSignedDistanceFieldsIsBrickValid(hit.brickId)) {
        return SDFMaterialReferenceSample(vec4<u32>(0u), 0u, vec2<u32>(0u));
    }

    let faceIndex = GPUSignedDistanceFieldsSurfaceCacheFaceFromNormal(normal);
    let preferred = sample_hit_material_reference_face(hit, faceIndex);
    if (material_reference_valid(preferred.reference)) {
        return preferred;
    }

    for (var fallbackFace = 0u; fallbackFace < GPUSignedDistanceFields_SurfaceCacheFaceCount; fallbackFace = fallbackFace + 1u) {
        if (fallbackFace == faceIndex) {
            continue;
        }
        let fallback = sample_hit_material_reference_face(hit, fallbackFace);
        if (material_reference_valid(fallback.reference)) {
            return fallback;
        }
    }

    return preferred;
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

fn decode_uv_window_mip(reference: vec4<u32>, textureIndex: u32) -> u32 {
    if (!material_reference_valid(reference) || textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }
    let uvWindow = abs(material_reference_uv_window(reference));
    let footprint = max(
        uvWindow.x * f32(max(textureEntries[textureIndex].mips[0].width, 1u)),
        uvWindow.y * f32(max(textureEntries[textureIndex].mips[0].height, 1u)));
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
    if (gUniforms.feedbackRequestCapacity == 0u || textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries)) {
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
    if (requestIndex >= gUniforms.feedbackRequestCapacity) {
        atomicAdd(&feedbackBuffer.overflowCount, 1u);
        return;
    }
    feedbackBuffer.requests[requestIndex] = FeedbackRequest(textureIndex, mip, requestedPageX, requestedPageY);
}

fn virtualTextureMipResidentAtUv(textureIndex: u32, uv: vec2<f32>, mipLevel: u32) -> bool {
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    if (mipInfo.pagesX == 0u || mipInfo.pagesY == 0u || atlasInfo.pageSize == 0u) {
        return false;
    }

    let mipPixel = textureMipPixel(textureIndex, mipLevel, uv);
    let pageX = min(mipPixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u);
    let pageY = min(mipPixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u);
    let pageTableIndex = mipInfo.pageTableOffset + pageY * mipInfo.pagesX + pageX;
    if (pageTableIndex >= arrayLength(&vtPageTable)) {
        return false;
    }

    let pageEntry = vtPageTable[pageTableIndex];
    return (pageEntry.flags & 1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID;
}

fn finest_resident_mip_at_uv(textureIndex: u32, uv: vec2<f32>) -> u32 {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&textureEntries) || textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }

    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    for (var mipLevel = 0u; mipLevel <= highestMip; mipLevel = mipLevel + 1u) {
        if (virtualTextureMipResidentAtUv(textureIndex, uv, mipLevel)) {
            return mipLevel;
        }
    }
    return 0u;
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

    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    var mip = min(mipLevel, highestMip);
    loop {
        let mipInfo = textureEntries[textureIndex].mips[mip];
        if (mipInfo.pagesX > 0u && mipInfo.pagesY > 0u) {
            let wrapped = sampleUV(textureIndex, uv);
            let pixel = vec2<u32>(
                min(u32(wrapped.x * f32(max(mipInfo.width, 1u))), max(mipInfo.width, 1u) - 1u),
                min(u32(wrapped.y * f32(max(mipInfo.height, 1u))), max(mipInfo.height, 1u) - 1u));
            let page = vec2<u32>(min(pixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u), min(pixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u));
            let pageTableIndex = mipInfo.pageTableOffset + page.y * mipInfo.pagesX + page.x;
            if (pageTableIndex < arrayLength(&vtPageTable)) {
                let pageEntry = vtPageTable[pageTableIndex];
                if ((pageEntry.flags & 1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
                    let localPixel = pixel - page * atlasInfo.pageSize;
                    let physicalPage = pageEntry.physicalPage;
                    let pageSize = atlasInfo.pageSize;
                    let clampedLocalPixel = min(localPixel, vec2<u32>(pageSize - 1u));
                    let texelIndex = physicalPage * pageSize * pageSize + clampedLocalPixel.y * pageSize + clampedLocalPixel.x;
                    if (texelIndex < arrayLength(&vtPhysicalPages)) {
                        let rgba = vtPhysicalPages[texelIndex];
                        return vec4<f32>(
                            f32((rgba >> 0u) & 255u),
                            f32((rgba >> 8u) & 255u),
                            f32((rgba >> 16u) & 255u),
                            f32((rgba >> 24u) & 255u)) / 255.0;
                    }
                }
            }
        }

        if (mip >= highestMip) {
            break;
        }
        mip = mip + 1u;
    }

    return vec4<f32>(1.0);
}

fn evaluate_material_albedo(reference: vec4<u32>) -> vec4<f32> {
    if (!material_reference_valid(reference) || reference.x >= arrayLength(&materialRuntime)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }

    let runtime = materialRuntime[reference.x];
    let uv = material_reference_uv(reference);
    var baseColor = runtime.baseColorFactor;
    if (runtime.baseColorTextureId != MATERIAL_INVALID_TEXTURE_ID) {
        let requestedMip = 0u;
        requestVirtualPage(runtime.baseColorTextureId, uv, requestedMip);
        let residentMip = finest_resident_mip_at_uv(runtime.baseColorTextureId, uv);
        baseColor = baseColor * sampleVirtualTexture(runtime.baseColorTextureId, uv, residentMip);
    }
    return vec4<f32>(clamp(baseColor.rgb, vec3<f32>(0.0), vec3<f32>(8.0)), 1.0);
}

fn decode_atlas_distance(atlasValue: f32) -> f32 {
    // Atlas encoding (from CompressBricks, matching Brixelizer line 1471):
    //   stored = sign(dist) * sqrt(|dist_brixels|) * 4.0 / (BrickRes-1)
    // Since we have no Eikonal pass, sign is always +1 and values are in [0,1].
    // Decode: brixel_dist = (atlasVal * (BrickRes-1) / 4.0)^2
    // But for sphere-tracing we use a simpler conservative step:
    //   atlasVal ~ 0 means on surface, atlasVal ~ 1 means far away.
    // Return: number of brixels to step (matches Brixelizer behavior)
    return atlasValue * f32(GPUSignedDistanceFields_BrickResolution - 1u);
}

fn reconstruct_world_position(uv: vec2<f32>, depthValue: f32) -> vec3<f32> {
    let ndc = vec4<f32>(uv.x * 2.0 - 1.0, (1.0 - uv.y) * 2.0 - 1.0, depthValue, 1.0);
    let world = gUniforms.inverseViewProjection * ndc;
    let invW = select(1.0, 1.0 / world.w, abs(world.w) > 1.0e-6);
    return world.xyz * invW;
}

fn project_world_position_to_uv(worldPosition: vec3<f32>) -> vec3<f32> {
    let clipRaw = gUniforms.viewProjection * vec4<f32>(worldPosition, 1.0);
    if (abs(clipRaw.w) <= 1.0e-6) {
        return vec3<f32>(-1.0, -1.0, -1.0);
    }

    let ndc = clipRaw.xyz / clipRaw.w;
    return vec3<f32>(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5, ndc.z);
}

fn root_aabb_projection_debug(uv: vec2<f32>) -> vec4<f32> {
    let cascadeCount = min(gUniforms.cascadeCount, GPUSignedDistanceFields_TraceMaxCascades);
    var color = vec3<f32>(0.0);
    var covered = false;

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex++) {
        let cascade = cascadeInfos[cascadeIndex];
        let cascadeAabbMinLocal = load_tree_u32x3(cascadeIndex, GPUSignedDistanceFields_RootAabbTreeWordOffset + 0u);
        let cascadeAabbMaxLocal = load_tree_u32x3(cascadeIndex, GPUSignedDistanceFields_RootAabbTreeWordOffset + 3u);
        if (tree_aabb_is_empty_u32(cascadeAabbMinLocal, cascadeAabbMaxLocal)) {
            continue;
        }
        let cascadeAabbMin = tree_local_coord_to_world(cascade, cascadeAabbMinLocal);
        let cascadeAabbMax = tree_local_coord_to_world(cascade, cascadeAabbMaxLocal);

        var minUv = vec2<f32>(1.0e20);
        var maxUv = vec2<f32>(-1.0e20);
        var anyProjected = false;
        for (var cornerIndex = 0u; cornerIndex < 8u; cornerIndex++) {
            let corner = mix(
                cascadeAabbMin,
                cascadeAabbMax,
                vec3<f32>(
                    f32(cornerIndex & 1u),
                    f32((cornerIndex >> 1u) & 1u),
                    f32((cornerIndex >> 2u) & 1u)));
            let projected = project_world_position_to_uv(corner);
            if (projected.z >= 0.0 && projected.z <= 1.0) {
                minUv = min(minUv, projected.xy);
                maxUv = max(maxUv, projected.xy);
                anyProjected = true;
            }
        }

        if (!anyProjected) {
            continue;
        }

        let inside = all(uv >= minUv) && all(uv <= maxUv);
        if (inside) {
            let edgeDistance = min(
                min(abs(uv.x - minUv.x), abs(uv.x - maxUv.x)),
                min(abs(uv.y - minUv.y), abs(uv.y - maxUv.y)));
            let edge = edgeDistance < 0.003;
            let cascadeColor = cascade_debug_color(cascadeIndex);
            color = select(cascadeColor * 0.25, cascadeColor, edge);
            covered = true;
        }
    }

    return vec4<f32>(select(vec3<f32>(0.0), color, covered), 1.0);
}

fn make_trace_parameters() -> TraceParameters {
    return TraceParameters(
        gUniforms.maxSteps,
        gUniforms.atlasBricksPerAxis,
        gUniforms.cascadeCount,
        gUniforms.cascadeOverride,
        gUniforms.hitThresholdScale,
        gUniforms.emptySpaceStepScale);
}

fn make_debug_ray(uv: vec2<f32>) -> vec3<f32> {
    let rayDepth = select(0.75, 0.25, gUniforms.reverseZ != 0u);
    let rayPoint = reconstruct_world_position(uv, rayDepth);
    return normalize(rayPoint - gUniforms.cameraPosition);
}

fn vsdf_transform_point(m: mat4x4<f32>, p: vec3<f32>) -> vec3<f32> {
    let q = m * vec4<f32>(p, 1.0);
    return q.xyz / max(abs(q.w), 1.0e-6);
}

fn vsdf_transform_vector(m: mat4x4<f32>, v: vec3<f32>) -> vec3<f32> {
    return (m * vec4<f32>(v, 0.0)).xyz;
}

fn vsdf_transform_normal(worldToLocal: mat4x4<f32>, n: vec3<f32>) -> vec3<f32> {
    return (transpose(worldToLocal) * vec4<f32>(n, 0.0)).xyz;
}

fn vsdf_ray_aabb(origin: vec3<f32>, direction: vec3<f32>, aabbMin: vec3<f32>, aabbMax: vec3<f32>, maxDistance: f32) -> vec2<f32> {
    let invDir = 1.0 / select(direction, vec3<f32>(1.0e-6), abs(direction) < vec3<f32>(1.0e-6));
    let t0 = (aabbMin - origin) * invDir;
    let t1 = (aabbMax - origin) * invDir;
    let tmin3 = min(t0, t1);
    let tmax3 = max(t0, t1);
    let tNear = max(max(tmin3.x, tmin3.y), max(tmin3.z, 0.0));
    let tFar = min(min(tmax3.x, tmax3.y), min(tmax3.z, maxDistance));
    return vec2<f32>(tNear, tFar);
}

fn vsdf_box_normal(localPoint: vec3<f32>, aabbMin: vec3<f32>, aabbMax: vec3<f32>) -> vec3<f32> {
    let dMin = abs(localPoint - aabbMin);
    let dMax = abs(aabbMax - localPoint);
    var best = dMin.x;
    var n = vec3<f32>(-1.0, 0.0, 0.0);
    if (dMax.x < best) { best = dMax.x; n = vec3<f32>(1.0, 0.0, 0.0); }
    if (dMin.y < best) { best = dMin.y; n = vec3<f32>(0.0, -1.0, 0.0); }
    if (dMax.y < best) { best = dMax.y; n = vec3<f32>(0.0, 1.0, 0.0); }
    if (dMin.z < best) { best = dMin.z; n = vec3<f32>(0.0, 0.0, -1.0); }
    if (dMax.z < best) { n = vec3<f32>(0.0, 0.0, 1.0); }
    return n;
}

fn vsdf_load_sdf_byte(byteOffset: u32) -> u32 {
    let wordIndex = byteOffset / 4u;
    if (wordIndex >= arrayLength(&vsdfSdfWords)) {
        return 255u;
    }
    let shift = (byteOffset & 3u) * 8u;
    return (vsdfSdfWords[wordIndex] >> shift) & 255u;
}

fn vsdf_load_sdf_u32(byteOffset: u32) -> u32 {
    return vsdf_load_sdf_byte(byteOffset) |
        (vsdf_load_sdf_byte(byteOffset + 1u) << 8u) |
        (vsdf_load_sdf_byte(byteOffset + 2u) << 16u) |
        (vsdf_load_sdf_byte(byteOffset + 3u) << 24u);
}

fn vsdf_load_chunk_sdf_byte(chunk: VSDFSdfChunk, cell: vec3<u32>) -> u32 {
    if ((chunk.flags & VSDF_SDF_CHUNK_FLAG_SPARSE_BRICK8) == 0u) {
        let strideY = max(chunk.brickResolutionX, 1u);
        let strideZ = strideY * max(chunk.brickResolutionY, 1u);
        return vsdf_load_sdf_byte(chunk.byteOffset + cell.x + cell.y * strideY + cell.z * strideZ);
    }

    if (chunk.byteCount < VSDF_SPARSE_BRICK_PAYLOAD_HEADER_BYTES) {
        return 255u;
    }
    let brickDim = max(vsdf_load_sdf_u32(chunk.byteOffset + 4u), 1u);
    let brickGridX = vsdf_load_sdf_u32(chunk.byteOffset + 8u);
    let brickGridY = vsdf_load_sdf_u32(chunk.byteOffset + 12u);
    let brickGridZ = vsdf_load_sdf_u32(chunk.byteOffset + 16u);
    let residentBrickCount = vsdf_load_sdf_u32(chunk.byteOffset + 20u);
    if (brickGridX == 0u || brickGridY == 0u || brickGridZ == 0u || residentBrickCount == 0u) {
        return 255u;
    }

    let brick = cell / vec3<u32>(brickDim);
    if (any(brick >= vec3<u32>(brickGridX, brickGridY, brickGridZ))) {
        return 255u;
    }
    let local = cell % vec3<u32>(brickDim);
    let brickGridCount = brickGridX * brickGridY * brickGridZ;
    let tableIndex = (brick.z * brickGridY + brick.y) * brickGridX + brick.x;
    let tableByteOffset = chunk.byteOffset + VSDF_SPARSE_BRICK_PAYLOAD_HEADER_BYTES + tableIndex * 4u;
    let residentBrickIndex = vsdf_load_sdf_u32(tableByteOffset);
    if (residentBrickIndex == VSDF_SPARSE_BRICK_INVALID || residentBrickIndex >= residentBrickCount) {
        return 255u;
    }

    let brickCellCount = brickDim * brickDim * brickDim;
    let brickPayloadByteOffset = chunk.byteOffset + VSDF_SPARSE_BRICK_PAYLOAD_HEADER_BYTES + brickGridCount * 4u + residentBrickIndex * brickCellCount;
    let localIndex = (local.z * brickDim + local.y) * brickDim + local.x;
    return vsdf_load_sdf_byte(brickPayloadByteOffset + localIndex);
}

fn vsdf_instance_narrow_band(instance: VSDFInstance) -> f32 {
    let extent = max(instance.localBoundsMax - instance.localBoundsMin, vec3<f32>(1.0e-3));
    let fallbackNarrowBand = max(max(extent.x, extent.y), extent.z) * 0.25;
    return select(fallbackNarrowBand, instance.narrowBandDistance, instance.narrowBandDistance > 1.0e-5);
}

fn vsdf_chunk_cell_size(instance: VSDFInstance, chunk: VSDFSdfChunk) -> vec3<f32> {
    let extent = max(instance.localBoundsMax - instance.localBoundsMin, vec3<f32>(1.0e-5));
    let resolution = vec3<f32>(
        max(f32(chunk.brickResolutionX), 1.0),
        max(f32(chunk.brickResolutionY), 1.0),
        max(f32(chunk.brickResolutionZ), 1.0));
    return extent / resolution;
}

fn vsdf_chunk_min_cell_radius(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let cell = vsdf_chunk_cell_size(instance, chunk);
    return min(min(cell.x, cell.y), cell.z);
}

fn vsdf_chunk_max_cell_radius(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let cell = vsdf_chunk_cell_size(instance, chunk);
    return max(max(cell.x, cell.y), cell.z);
}

fn vsdf_chunk_median_cell_radius(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let cell = vsdf_chunk_cell_size(instance, chunk);
    return max(min(cell.x, cell.y), min(max(cell.x, cell.y), cell.z));
}

fn vsdf_chunk_min_cell_axis(instance: VSDFInstance, chunk: VSDFSdfChunk) -> vec3<f32> {
    let cell = vsdf_chunk_cell_size(instance, chunk);
    var axis = vec3<f32>(1.0, 0.0, 0.0);
    var best = cell.x;
    if (cell.y < best) {
        best = cell.y;
        axis = vec3<f32>(0.0, 1.0, 0.0);
    }
    if (cell.z < best) {
        axis = vec3<f32>(0.0, 0.0, 1.0);
    }
    return axis;
}

fn vsdf_chunk_surface_step_radius(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let unsignedDistance = (chunk.flags & 1u) != 0u;
    let cellRadius = select(vsdf_chunk_min_cell_radius(instance, chunk), vsdf_chunk_median_cell_radius(instance, chunk), unsignedDistance);
    let quantization = vsdf_instance_narrow_band(instance) / 255.0;
    return max(cellRadius * select(2.0, 1.5, unsignedDistance), quantization * 0.5);
}

fn vsdf_chunk_surface_hit_threshold(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let minCell = vsdf_chunk_min_cell_radius(instance, chunk);
    let quantization = vsdf_instance_narrow_band(instance) / 255.0 * gUniforms.hitThresholdScale;
    return max(max(minCell * 0.75 * gUniforms.hitThresholdScale, quantization * 0.35), 1.0e-4);
}

fn vsdf_chunk_unsigned_surface_hit_threshold(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let cellRadius = vsdf_chunk_median_cell_radius(instance, chunk);
    let quantization = vsdf_instance_narrow_band(instance) / 255.0;
    let scale = max(gUniforms.hitThresholdScale, 1.0);
    return max(max(cellRadius * 0.55 * scale, quantization * 0.35), 1.0e-4);
}

fn vsdf_chunk_normal_sample_radius(instance: VSDFInstance, chunk: VSDFSdfChunk) -> f32 {
    let minCell = vsdf_chunk_min_cell_radius(instance, chunk);
    let medianCell = vsdf_chunk_median_cell_radius(instance, chunk);
    let maxExtent = max(max(instance.localBoundsMax.x - instance.localBoundsMin.x, instance.localBoundsMax.y - instance.localBoundsMin.y), instance.localBoundsMax.z - instance.localBoundsMin.z);
    let unsignedDistance = (chunk.flags & 1u) != 0u;
    return select(max(minCell * 0.75, maxExtent / 2048.0), max(medianCell * 0.75, maxExtent / 4096.0), unsignedDistance);
}

fn vsdf_unsigned_validated_hit_threshold(instance: VSDFInstance, chunk: VSDFSdfChunk, localPoint: vec3<f32>, localRayDirection: vec3<f32>) -> f32 {
    let minCell = vsdf_chunk_min_cell_radius(instance, chunk);
    let eps = max(minCell * 0.75, max(max(instance.localBoundsMax.x - instance.localBoundsMin.x, instance.localBoundsMax.y - instance.localBoundsMin.y), instance.localBoundsMax.z - instance.localBoundsMin.z) / 2048.0);
    let center = sample_vsdf_local_sdf_full(instance, localPoint);
    let dx = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, localPoint + vec3<f32>(eps, 0.0, 0.0)), sample_vsdf_local_sdf_full(instance, localPoint - vec3<f32>(eps, 0.0, 0.0)));
    let dy = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, localPoint + vec3<f32>(0.0, eps, 0.0)), sample_vsdf_local_sdf_full(instance, localPoint - vec3<f32>(0.0, eps, 0.0)));
    let dz = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, localPoint + vec3<f32>(0.0, 0.0, eps)), sample_vsdf_local_sdf_full(instance, localPoint - vec3<f32>(0.0, 0.0, eps)));
    let gradient = vec3<f32>(dx, dy, dz);
    let gradientLength = length(gradient);
    var normal = select(vsdf_chunk_min_cell_axis(instance, chunk), gradient / gradientLength, gradientLength > 1.0e-6);
    if (dot(normal, -localRayDirection) < 0.0) {
        normal = -normal;
    }
    let cell = vsdf_chunk_cell_size(instance, chunk);
    let normalCell = dot(abs(normal), cell);
    let quantization = vsdf_instance_narrow_band(instance) / 255.0;
    return max(max(normalCell * 1.25, quantization * 0.5), 1.0e-4);
}

fn vsdf_sample_sparse_octree_brick8(instance: VSDFInstance, chunk: VSDFSdfChunk, localPoint: vec3<f32>) -> VSDFSdfSample {
    let chunkIsUnsigned = (chunk.flags & 1u) != 0u;
    if (any(localPoint < instance.localBoundsMin) || any(localPoint > instance.localBoundsMax)) {
        return VSDFSdfSample(1.0e6, 1.0, select(1.0, 0.0, chunkIsUnsigned));
    }
    if (chunk.byteCount < VSDF_SPARSE_OCTREE_PAYLOAD_HEADER_BYTES) {
        return VSDFSdfSample(1.0e6, 1.0, 0.0);
    }

    let brickDim = max(vsdf_load_sdf_u32(chunk.byteOffset + 4u), 1u);
    let rootDim = max(vsdf_load_sdf_u32(chunk.byteOffset + 8u), 1u);
    let maxDepth = min(vsdf_load_sdf_u32(chunk.byteOffset + 12u), 8u);
    let nodeCount = vsdf_load_sdf_u32(chunk.byteOffset + 16u);
    let residentBrickCount = vsdf_load_sdf_u32(chunk.byteOffset + 20u);
    let nodeTableOffset = vsdf_load_sdf_u32(chunk.byteOffset + 24u);
    let brickDataOffset = vsdf_load_sdf_u32(chunk.byteOffset + 28u);
    if (nodeCount == 0u || rootDim == 0u || brickDim == 0u) {
        return VSDFSdfSample(1.0e6, 1.0, 0.0);
    }

    let extent = max(instance.localBoundsMax - instance.localBoundsMin, vec3<f32>(1.0e-5));
    let uvw = clamp((localPoint - instance.localBoundsMin) / extent, vec3<f32>(0.0), vec3<f32>(0.999999));
    let rootCell = min(vec3<u32>(uvw * f32(rootDim)), vec3<u32>(rootDim - 1u));
    var nodeIndex = (rootCell.z * rootDim + rootCell.y) * rootDim + rootCell.x;
    var regionMin = vec3<f32>(rootCell) / f32(rootDim);
    var regionSize = vec3<f32>(1.0 / f32(rootDim));
    var fallbackBrickIndex = VSDF_SPARSE_BRICK_INVALID;
    var fallbackRegionMin = regionMin;
    var fallbackRegionSize = regionSize;

    for (var depth = 0u; depth <= 8u; depth = depth + 1u) {
        if (nodeIndex >= nodeCount) {
            return VSDFSdfSample(1.0e6, 1.0, 0.0);
        }
        let nodeByteOffset = chunk.byteOffset + nodeTableOffset + nodeIndex * 12u;
        let childBase = vsdf_load_sdf_u32(nodeByteOffset);
        let nodeBrickIndex = vsdf_load_sdf_u32(nodeByteOffset + 4u);
        let nodeFlags = vsdf_load_sdf_u32(nodeByteOffset + 8u);
        if ((nodeFlags & VSDF_SPARSE_OCTREE_NODE_RESIDENT) != 0u && nodeBrickIndex < residentBrickCount) {
            fallbackBrickIndex = nodeBrickIndex;
            fallbackRegionMin = regionMin;
            fallbackRegionSize = regionSize;
        }
        if ((nodeFlags & VSDF_SPARSE_OCTREE_NODE_LEAF) != 0u || depth >= maxDepth) {
            var sampleBrickIndex = nodeBrickIndex;
            var sampleRegionMin = regionMin;
            var sampleRegionSize = regionSize;
            if ((nodeFlags & VSDF_SPARSE_OCTREE_NODE_RESIDENT) == 0u || nodeBrickIndex >= residentBrickCount) {
                sampleBrickIndex = fallbackBrickIndex;
                sampleRegionMin = fallbackRegionMin;
                sampleRegionSize = fallbackRegionSize;
            }
            if (sampleBrickIndex == VSDF_SPARSE_BRICK_INVALID || sampleBrickIndex >= residentBrickCount) {
                let regionLocalMin = instance.localBoundsMin + extent * regionMin;
                let regionLocalMax = instance.localBoundsMin + extent * (regionMin + regionSize);
                let toMin = max(localPoint - regionLocalMin, vec3<f32>(0.0));
                let toMax = max(regionLocalMax - localPoint, vec3<f32>(0.0));
                let emptyDistance = max(min(min(min(toMin.x, toMin.y), toMin.z), min(min(toMax.x, toMax.y), toMax.z)), vsdf_chunk_min_cell_radius(instance, chunk));
                let encodedEmptyDistance = clamp(emptyDistance / max(vsdf_instance_narrow_band(instance), 1.0e-3), 0.0, 1.0);
                return VSDFSdfSample(emptyDistance, encodedEmptyDistance, 0.25);
            }

            let localUVW = clamp((uvw - sampleRegionMin) / max(sampleRegionSize, vec3<f32>(1.0e-6)), vec3<f32>(0.0), vec3<f32>(1.0));
            let grid = clamp(
                localUVW * f32(brickDim) - vec3<f32>(0.5),
                vec3<f32>(0.0),
                vec3<f32>(f32(brickDim - 1u)));
            let cell0 = vec3<u32>(floor(grid));
            let cell1 = min(cell0 + vec3<u32>(1u), vec3<u32>(brickDim - 1u));
            let f = fract(grid);
            let brickCellCount = brickDim * brickDim * brickDim;
            let brickBase = chunk.byteOffset + brickDataOffset + sampleBrickIndex * brickCellCount;
            let b000 = vsdf_load_sdf_byte(brickBase + (cell0.z * brickDim + cell0.y) * brickDim + cell0.x);
            let b100 = vsdf_load_sdf_byte(brickBase + (cell0.z * brickDim + cell0.y) * brickDim + cell1.x);
            let b010 = vsdf_load_sdf_byte(brickBase + (cell0.z * brickDim + cell1.y) * brickDim + cell0.x);
            let b110 = vsdf_load_sdf_byte(brickBase + (cell0.z * brickDim + cell1.y) * brickDim + cell1.x);
            let b001 = vsdf_load_sdf_byte(brickBase + (cell1.z * brickDim + cell0.y) * brickDim + cell0.x);
            let b101 = vsdf_load_sdf_byte(brickBase + (cell1.z * brickDim + cell0.y) * brickDim + cell1.x);
            let b011 = vsdf_load_sdf_byte(brickBase + (cell1.z * brickDim + cell1.y) * brickDim + cell0.x);
            let b111 = vsdf_load_sdf_byte(brickBase + (cell1.z * brickDim + cell1.y) * brickDim + cell1.x);
            let wx0 = 1.0 - f.x;
            let wy0 = 1.0 - f.y;
            let wz0 = 1.0 - f.z;
            let wx1 = f.x;
            let wy1 = f.y;
            let wz1 = f.z;
            let w000 = wx0 * wy0 * wz0;
            let w100 = wx1 * wy0 * wz0;
            let w010 = wx0 * wy1 * wz0;
            let w110 = wx1 * wy1 * wz0;
            let w001 = wx0 * wy0 * wz1;
            let w101 = wx1 * wy0 * wz1;
            let w011 = wx0 * wy1 * wz1;
            let w111 = wx1 * wy1 * wz1;
            let v000 = select(0.0, w000, b000 < 255u);
            let v100 = select(0.0, w100, b100 < 255u);
            let v010 = select(0.0, w010, b010 < 255u);
            let v110 = select(0.0, w110, b110 < 255u);
            let v001 = select(0.0, w001, b001 < 255u);
            let v101 = select(0.0, w101, b101 < 255u);
            let v011 = select(0.0, w011, b011 < 255u);
            let v111 = select(0.0, w111, b111 < 255u);
            let validity = v000 + v100 + v010 + v110 + v001 + v101 + v011 + v111;
            if (validity <= 1.0e-5) {
                return VSDFSdfSample(1.0e6, 1.0, 0.0);
            }
            let encodedDistance = (
                f32(min(b000, 254u)) / 254.0 * v000 +
                f32(min(b100, 254u)) / 254.0 * v100 +
                f32(min(b010, 254u)) / 254.0 * v010 +
                f32(min(b110, 254u)) / 254.0 * v110 +
                f32(min(b001, 254u)) / 254.0 * v001 +
                f32(min(b101, 254u)) / 254.0 * v101 +
                f32(min(b011, 254u)) / 254.0 * v011 +
                f32(min(b111, 254u)) / 254.0 * v111) / validity;
            return VSDFSdfSample(encodedDistance * vsdf_instance_narrow_band(instance), encodedDistance, validity);
        }

        let childUVW = clamp((uvw - regionMin) / max(regionSize, vec3<f32>(1.0e-6)), vec3<f32>(0.0), vec3<f32>(0.999999));
        let childCell = vec3<u32>(childUVW * 2.0);
        let childLinear = (childCell.z * 2u + childCell.y) * 2u + childCell.x;
        if (childBase == VSDF_SPARSE_BRICK_INVALID) {
            if (fallbackBrickIndex != VSDF_SPARSE_BRICK_INVALID) {
                nodeIndex = nodeCount;
                continue;
            }
            return VSDFSdfSample(1.0e6, 1.0, 0.0);
        }
        nodeIndex = childBase + childLinear;
        regionMin = regionMin + vec3<f32>(childCell) * regionSize * 0.5;
        regionSize = regionSize * 0.5;
    }

    return VSDFSdfSample(1.0e6, 1.0, 0.0);
}

fn sample_vsdf_local_sdf_full(instance: VSDFInstance, localPoint: vec3<f32>) -> VSDFSdfSample {
    if (instance.sdfChunkCount == 0u || instance.firstSdfChunk >= arrayLength(&vsdfSdfChunks)) {
        return VSDFSdfSample(1.0e6, 1.0, 0.0);
    }
    let chunk = vsdfSdfChunks[instance.firstSdfChunk];
    if ((chunk.flags & VSDF_SDF_CHUNK_FLAG_SPARSE_OCTREE_BRICK8) != 0u) {
        return vsdf_sample_sparse_octree_brick8(instance, chunk, localPoint);
    }
    let resolution = vec3<u32>(max(chunk.brickResolutionX, 1u), max(chunk.brickResolutionY, 1u), max(chunk.brickResolutionZ, 1u));
    let chunkIsSparseBrick8 = (chunk.flags & VSDF_SDF_CHUNK_FLAG_SPARSE_BRICK8) != 0u;
    let expectedByteCount = select(resolution.x * resolution.y * resolution.z, VSDF_SPARSE_BRICK_PAYLOAD_HEADER_BYTES, chunkIsSparseBrick8);
    if (chunk.byteCount < expectedByteCount) {
        return VSDFSdfSample(1.0e6, 1.0, 0.0);
    }
    let chunkIsUnsigned = (chunk.flags & 1u) != 0u;
    if (any(localPoint < instance.localBoundsMin) || any(localPoint > instance.localBoundsMax)) {
        return VSDFSdfSample(1.0e6, 1.0, select(1.0, 0.0, chunkIsUnsigned));
    }

    let extent = max(instance.localBoundsMax - instance.localBoundsMin, vec3<f32>(1.0e-5));
    let uvw = clamp((localPoint - instance.localBoundsMin) / extent, vec3<f32>(0.0), vec3<f32>(1.0));
    let grid = clamp(
        uvw * vec3<f32>(resolution) - vec3<f32>(0.5),
        vec3<f32>(0.0),
        vec3<f32>(resolution - vec3<u32>(1u)));
    let cell0 = vec3<u32>(floor(grid));
    let cell1 = min(cell0 + vec3<u32>(1u), resolution - vec3<u32>(1u));
    let f = fract(grid);

    let b000 = vsdf_load_chunk_sdf_byte(chunk, cell0);
    let b100 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell1.x, cell0.y, cell0.z));
    let b010 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell0.x, cell1.y, cell0.z));
    let b110 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell1.x, cell1.y, cell0.z));
    let b001 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell0.x, cell0.y, cell1.z));
    let b101 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell1.x, cell0.y, cell1.z));
    let b011 = vsdf_load_chunk_sdf_byte(chunk, vec3<u32>(cell0.x, cell1.y, cell1.z));
    let b111 = vsdf_load_chunk_sdf_byte(chunk, cell1);
    let narrowBand = vsdf_instance_narrow_band(instance);
    if (chunkIsUnsigned) {
        let wx0 = 1.0 - f.x;
        let wy0 = 1.0 - f.y;
        let wz0 = 1.0 - f.z;
        let wx1 = f.x;
        let wy1 = f.y;
        let wz1 = f.z;
        let w000 = wx0 * wy0 * wz0;
        let w100 = wx1 * wy0 * wz0;
        let w010 = wx0 * wy1 * wz0;
        let w110 = wx1 * wy1 * wz0;
        let w001 = wx0 * wy0 * wz1;
        let w101 = wx1 * wy0 * wz1;
        let w011 = wx0 * wy1 * wz1;
        let w111 = wx1 * wy1 * wz1;
        let v000 = select(0.0, w000, b000 < 255u);
        let v100 = select(0.0, w100, b100 < 255u);
        let v010 = select(0.0, w010, b010 < 255u);
        let v110 = select(0.0, w110, b110 < 255u);
        let v001 = select(0.0, w001, b001 < 255u);
        let v101 = select(0.0, w101, b101 < 255u);
        let v011 = select(0.0, w011, b011 < 255u);
        let v111 = select(0.0, w111, b111 < 255u);
        let validity = v000 + v100 + v010 + v110 + v001 + v101 + v011 + v111;
        if (validity <= 1.0e-5) {
            return VSDFSdfSample(1.0e6, 1.0, 0.0);
        }
        let encodedDistance = (
            f32(min(b000, 254u)) / 254.0 * v000 +
            f32(min(b100, 254u)) / 254.0 * v100 +
            f32(min(b010, 254u)) / 254.0 * v010 +
            f32(min(b110, 254u)) / 254.0 * v110 +
            f32(min(b001, 254u)) / 254.0 * v001 +
            f32(min(b101, 254u)) / 254.0 * v101 +
            f32(min(b011, 254u)) / 254.0 * v011 +
            f32(min(b111, 254u)) / 254.0 * v111) / validity;
        return VSDFSdfSample(encodedDistance * narrowBand, encodedDistance, validity);
    }

    let c000 = f32(b000) / 255.0;
    let c100 = f32(b100) / 255.0;
    let c010 = f32(b010) / 255.0;
    let c110 = f32(b110) / 255.0;
    let c001 = f32(b001) / 255.0;
    let c101 = f32(b101) / 255.0;
    let c011 = f32(b011) / 255.0;
    let c111 = f32(b111) / 255.0;

    let c00 = mix(c000, c100, f.x);
    let c10 = mix(c010, c110, f.x);
    let c01 = mix(c001, c101, f.x);
    let c11 = mix(c011, c111, f.x);
    let c0 = mix(c00, c10, f.y);
    let c1 = mix(c01, c11, f.y);
    let encodedDistance = mix(c0, c1, f.z);
    return VSDFSdfSample((encodedDistance * 2.0 - 1.0) * narrowBand, encodedDistance, 1.0);
}

fn sample_vsdf_local_sdf(instance: VSDFInstance, localPoint: vec3<f32>) -> f32 {
    return sample_vsdf_local_sdf_full(instance, localPoint).distance;
}

fn vsdf_unsigned_sample_has_coverage(sample: VSDFSdfSample) -> bool {
    return sample.validity >= 0.125;
}

fn vsdf_unsigned_sample_has_hit_coverage(sample: VSDFSdfSample) -> bool {
    return sample.validity >= 0.35;
}

fn vsdf_unsigned_normal_sample_has_coverage(sample: VSDFSdfSample) -> bool {
    return sample.validity >= 0.5;
}

fn vsdf_valid_distance_delta(_center: VSDFSdfSample, positive: VSDFSdfSample, negative: VSDFSdfSample) -> f32 {
    let positiveValid = vsdf_unsigned_normal_sample_has_coverage(positive);
    let negativeValid = vsdf_unsigned_normal_sample_has_coverage(negative);
    if (positiveValid && negativeValid) {
        return positive.distance - negative.distance;
    }
    return 0.0;
}

fn vsdf_sdf_normal(instance: VSDFInstance, localPoint: vec3<f32>, localRayDirection: vec3<f32>, normalSampleRadius: f32) -> vec3<f32> {
    let extent = max(instance.localBoundsMax - instance.localBoundsMin, vec3<f32>(1.0e-5));
    let maxExtent = max(max(extent.x, extent.y), extent.z);
    let eps = max(normalSampleRadius, maxExtent / 2048.0);
    let samplePoint = localPoint - localRayDirection * eps;
    let chunk = vsdfSdfChunks[instance.firstSdfChunk];
    let chunkIsUnsigned = (chunk.flags & 1u) != 0u;

    let center = sample_vsdf_local_sdf_full(instance, samplePoint);
    let dx0 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(eps, 0.0, 0.0)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(eps, 0.0, 0.0)));
    let dy0 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(0.0, eps, 0.0)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(0.0, eps, 0.0)));
    let dz0 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(0.0, 0.0, eps)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(0.0, 0.0, eps)));
    let epsWide = eps * 2.0;
    let dx1 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(epsWide, 0.0, 0.0)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(epsWide, 0.0, 0.0)));
    let dy1 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(0.0, epsWide, 0.0)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(0.0, epsWide, 0.0)));
    let dz1 = vsdf_valid_distance_delta(center, sample_vsdf_local_sdf_full(instance, samplePoint + vec3<f32>(0.0, 0.0, epsWide)), sample_vsdf_local_sdf_full(instance, samplePoint - vec3<f32>(0.0, 0.0, epsWide)));

    let gradient = vec3<f32>(dx0 + dx1 * 0.5, dy0 + dy1 * 0.5, dz0 + dz1 * 0.5);
    let gradientLength = length(gradient);
    var fallbackNormal = select(-localRayDirection, vsdf_chunk_min_cell_axis(instance, chunk), chunkIsUnsigned);
    if (dot(fallbackNormal, -localRayDirection) < 0.0) {
        fallbackNormal = -fallbackNormal;
    }
    var normal = select(fallbackNormal, gradient / gradientLength, gradientLength > 1.0e-6);
    if (dot(normal, -localRayDirection) < 0.0) {
        normal = -normal;
    }
    return normal;
}


fn refine_vsdf_local_hit(instance: VSDFInstance, localOrigin: vec3<f32>, localDirection: vec3<f32>, lowTIn: f32, highTIn: f32) -> f32 {
    var lowT = lowTIn;
    var highT = highTIn;
    if (highT <= lowT) {
        return highT;
    }

    for (var iteration = 0u; iteration < 6u; iteration = iteration + 1u) {
        let t0 = mix(lowT, highT, 0.33333334);
        let t1 = mix(lowT, highT, 0.6666667);
        let d0 = abs(sample_vsdf_local_sdf(instance, localOrigin + localDirection * t0));
        let d1 = abs(sample_vsdf_local_sdf(instance, localOrigin + localDirection * t1));
        if (d0 < d1) {
            highT = t1;
        } else {
            lowT = t0;
        }
    }
    return (lowT + highT) * 0.5;
}

fn trace_vsdf_instance(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, maxDistance: f32, instanceIndex: u32, bestHitIn: VSDFTraceHit, needNormal: bool) -> VSDFTraceHit {
    var bestHit = bestHitIn;
    local_trace_stats_add(LOCAL_TRACE_INSTANCE_TESTS, 1u);
    if (instanceIndex < arrayLength(&vsdfInstances)) {
        let instance = vsdfInstances[instanceIndex];
        let localOrigin = vsdf_transform_point(instance.worldToLocal, rayOrigin);
        let localFar = vsdf_transform_point(instance.worldToLocal, rayOrigin + rayDirection * maxDistance);
        let localDelta = localFar - localOrigin;
        let localMaxDistance = length(localDelta);
        if (localMaxDistance > 1.0e-6) {
        let localDirection = localDelta / localMaxDistance;
        let localT = vsdf_ray_aabb(localOrigin, localDirection, instance.localBoundsMin, instance.localBoundsMax, localMaxDistance);
        if (localT.x <= localT.y) {
            local_trace_stats_add(LOCAL_TRACE_INSTANCE_AABB_HITS, 1u);
            var localRayT = localT.x;
            let localRayEnd = localT.y;
            var localHit = localOrigin + localDirection * localRayT;
            var previousLocalRayT = localRayT;
            var previousDistance = 1.0e6;
            var hitFound = false;
            let chunk = vsdfSdfChunks[instance.firstSdfChunk];
            let chunkIsUnsigned = (chunk.flags & 1u) != 0u;
            let chunkIsOrientedSurface = (chunk.flags & 16u) != 0u;
            let chunkUsesSurfaceShellTrace = chunkIsUnsigned || chunkIsOrientedSurface;
            let surfaceStepRadius = vsdf_chunk_surface_step_radius(instance, chunk);
            let quantizationThreshold = vsdf_instance_narrow_band(instance) / 255.0 * gUniforms.hitThresholdScale;
            let threshold = select(
                max(quantizationThreshold, 1.0e-4),
                select(
                    vsdf_chunk_surface_hit_threshold(instance, chunk),
                    vsdf_chunk_unsigned_surface_hit_threshold(instance, chunk),
                    chunkIsUnsigned),
                chunkUsesSurfaceShellTrace);
            var previousPreviousLocalRayT = localRayT;
            var previousHitDistance = 1.0e6;
            var traceStepCount = 0u;
            for (var step = 0u; step < gUniforms.maxSteps && localRayT <= localRayEnd; step = step + 1u) {
                traceStepCount = step + 1u;
                localHit = localOrigin + localDirection * localRayT;
                let sdfSample = sample_vsdf_local_sdf_full(instance, localHit);
                let distance = sdfSample.distance;
                local_trace_stats_add(LOCAL_TRACE_SDF_SAMPLES, 1u);
                let uncoveredUnsignedSample = chunkIsUnsigned && !vsdf_unsigned_sample_has_coverage(sdfSample);
                let hitDistance = select(select(abs(distance), distance, chunkIsUnsigned), 1.0e6, uncoveredUnsignedSample);
                let crossedSignedSurface = !chunkUsesSurfaceShellTrace && previousDistance > threshold && distance < -threshold;
                let bracketedSurfaceMinimum = chunkUsesSurfaceShellTrace && previousHitDistance < 1.0e5 && hitDistance > previousHitDistance && previousHitDistance <= surfaceStepRadius;
                let shellThresholdHit = chunkUsesSurfaceShellTrace && previousHitDistance < 1.0e5 && hitDistance <= threshold && hitDistance <= previousHitDistance;
                if ((!chunkUsesSurfaceShellTrace && hitDistance <= threshold) || crossedSignedSurface || bracketedSurfaceMinimum || shellThresholdHit) {
                    let refineLowT = select(previousLocalRayT, previousPreviousLocalRayT, bracketedSurfaceMinimum);
                    let candidateLocalRayT = refine_vsdf_local_hit(instance, localOrigin, localDirection, refineLowT, localRayT);
                    let candidateLocalHit = localOrigin + localDirection * candidateLocalRayT;
                    let refinedSample = sample_vsdf_local_sdf_full(instance, candidateLocalHit);
                    let refinedDistance = refinedSample.distance;
                    let refinedHitDistance = select(abs(refinedDistance), refinedDistance, chunkIsUnsigned);
                    let uncoveredUnsignedRefinedSample = chunkIsUnsigned && !vsdf_unsigned_sample_has_hit_coverage(refinedSample);
                    let validatedThreshold = select(
                        threshold,
                        min(threshold, vsdf_unsigned_validated_hit_threshold(instance, chunk, candidateLocalHit, localDirection)),
                        chunkIsUnsigned);
                    let refinedDistanceAccepted = select(
                        !bracketedSurfaceMinimum || refinedHitDistance <= threshold,
                        refinedHitDistance <= validatedThreshold,
                        chunkIsUnsigned);
                    if (refinedDistanceAccepted && !uncoveredUnsignedRefinedSample) {
                        local_trace_stats_add(LOCAL_TRACE_SDF_HITS, 1u);
                        localRayT = candidateLocalRayT;
                        localHit = candidateLocalHit;
                        hitFound = true;
                        break;
                    }
                }
                previousPreviousLocalRayT = previousLocalRayT;
                previousLocalRayT = localRayT;
                previousDistance = distance;
                previousHitDistance = hitDistance;
                let conservativeDistance = select(
                    max(abs(distance), threshold),
                    select(max(abs(distance) - surfaceStepRadius, threshold), max(surfaceStepRadius, threshold), uncoveredUnsignedSample),
                    chunkUsesSurfaceShellTrace);
                localRayT = localRayT + max(conservativeDistance * gUniforms.emptySpaceStepScale, threshold);
            }
            let traceMaxStepsReached = traceStepCount >= gUniforms.maxSteps && localRayT <= localRayEnd;

            if (!hitFound && instance.sdfChunkCount == 0u) {
                hitFound = true;
                localHit = localOrigin + localDirection * localT.x;
            }

            if (hitFound) {
                let worldHit = vsdf_transform_point(instance.localToWorld, localHit);
                let worldT = dot(worldHit - rayOrigin, rayDirection);
                if (worldT > 0.0 && worldT < bestHit.t) {
                    var worldNormal = vec3<f32>(0.0, 0.0, 1.0);
                    if (needNormal) {
                        let localNormal = vsdf_sdf_normal(instance, localHit, localDirection, vsdf_chunk_normal_sample_radius(instance, chunk));
                        worldNormal = normalize(vsdf_transform_normal(instance.worldToLocal, localNormal));
                        if (dot(worldNormal, rayDirection) > 0.0) {
                            worldNormal = -worldNormal;
                        }
                    }
                        bestHit = VSDFTraceHit(true, worldT, instanceIndex, traceStepCount, select(0u, 1u, traceMaxStepsReached), worldNormal);
                }
            }
            if (!hitFound && traceStepCount > bestHit.stepCount) {
                bestHit.stepCount = traceStepCount;
                bestHit.maxStepsReached = select(0u, 1u, traceMaxStepsReached);
            }
        }
        }
    }
    return bestHit;
}

fn vsdf_sparse_tlas_page_table_index(page: vec3<u32>) -> u32 {
    return (page.z * vsdfSparseTLASInfo.pageDimY + page.y) * vsdfSparseTLASInfo.pageDimX + page.x;
}

fn vsdf_sparse_tlas_cell_offset(cell: vec3<u32>) -> u32 {
    let pageDim = max(vsdfSparseTLASInfo.pageDim, 1u);
    let local = cell % vec3<u32>(pageDim);
    return (local.z * pageDim + local.y) * pageDim + local.x;
}

fn trace_vsdf_sparse_tlas_instances(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, maxDistance: f32, needNormal: bool) -> VSDFTraceHit {
    var bestHit = VSDFTraceHit(false, maxDistance, 0u, 0u, 0u, vec3<f32>(0.0, 0.0, 1.0));
    if (vsdfSparseTLASInfo.pageCount == 0u || vsdfSparseTLASInfo.cellCount == 0u || vsdfSparseTLASInfo.cellSize <= 0.0) {
        return bestHit;
    }

    let fineDimsU = vec3<u32>(vsdfSparseTLASInfo.fineDimX, vsdfSparseTLASInfo.fineDimY, vsdfSparseTLASInfo.fineDimZ);
    if (fineDimsU.x == 0u || fineDimsU.y == 0u || fineDimsU.z == 0u) {
        return bestHit;
    }

    let worldMin = vsdfSparseTLASInfo.worldBoundsMin;
    let worldMax = worldMin + vec3<f32>(fineDimsU) * vsdfSparseTLASInfo.cellSize;
    let boundsT = vsdf_ray_aabb(rayOrigin, rayDirection, worldMin, worldMax, maxDistance);
    if (boundsT.x > boundsT.y) {
        return bestHit;
    }

    let pageDim = max(vsdfSparseTLASInfo.pageDim, 1u);
    let fineDimsI = vec3<i32>(i32(fineDimsU.x), i32(fineDimsU.y), i32(fineDimsU.z));
    var rayT = max(boundsT.x, 0.0);
    var cellPoint = rayOrigin + rayDirection * (rayT + 1.0e-4);
    var cell = clamp(
        vec3<i32>(
            i32(floor((cellPoint.x - worldMin.x) / vsdfSparseTLASInfo.cellSize)),
            i32(floor((cellPoint.y - worldMin.y) / vsdfSparseTLASInfo.cellSize)),
            i32(floor((cellPoint.z - worldMin.z) / vsdfSparseTLASInfo.cellSize))),
        vec3<i32>(0),
        fineDimsI - vec3<i32>(1));

    let step = vec3<i32>(
        select(-1, 1, rayDirection.x >= 0.0),
        select(-1, 1, rayDirection.y >= 0.0),
        select(-1, 1, rayDirection.z >= 0.0));
    let safeDir = select(rayDirection, vec3<f32>(1.0), abs(rayDirection) < vec3<f32>(1.0e-6));
    let nextBoundary = worldMin + (vec3<f32>(cell) + vec3<f32>(
        select(0.0, 1.0, step.x > 0),
        select(0.0, 1.0, step.y > 0),
        select(0.0, 1.0, step.z > 0))) * vsdfSparseTLASInfo.cellSize;
    var tMax = select((nextBoundary - rayOrigin) / safeDir, vec3<f32>(1.0e30), abs(rayDirection) < vec3<f32>(1.0e-6));
    let tDelta = select(abs(vec3<f32>(vsdfSparseTLASInfo.cellSize) / safeDir), vec3<f32>(1.0e30), abs(rayDirection) < vec3<f32>(1.0e-6));
    let maxRayT = min(maxDistance, boundsT.y);
    let maxDdaSteps = min(1024u, fineDimsU.x + fineDimsU.y + fineDimsU.z + 3u);
    var visitedInstances: array<u32, 32>;
    var visitedInstanceCount = 0u;

    for (var ddaStep = 0u; ddaStep < maxDdaSteps && rayT <= maxRayT && rayT <= bestHit.t; ddaStep = ddaStep + 1u) {
        if (any(cell < vec3<i32>(0)) || any(cell >= fineDimsI)) {
            break;
        }

        let cellEnterT = rayT;
        let cellExitT = min(min(tMax.x, tMax.y), min(tMax.z, maxRayT));
        let cellU = vec3<u32>(cell);
        let page = cellU / vec3<u32>(pageDim);
        if (page.x < vsdfSparseTLASInfo.pageDimX && page.y < vsdfSparseTLASInfo.pageDimY && page.z < vsdfSparseTLASInfo.pageDimZ) {
            let pageTableIndex = vsdf_sparse_tlas_page_table_index(page);
            if (pageTableIndex < arrayLength(&vsdfSparseTLASPageTable)) {
                let firstCell = vsdfSparseTLASPageTable[pageTableIndex];
                if (firstCell != 0xffffffffu) {
                    let tlasCellIndex = firstCell + vsdf_sparse_tlas_cell_offset(cellU);
                    if (tlasCellIndex < arrayLength(&vsdfSparseTLASCells)) {
                        let tlasCell = vsdfSparseTLASCells[tlasCellIndex];
                        let endRef = min(tlasCell.firstRef + tlasCell.refCount, arrayLength(&vsdfSparseTLASInstanceRefs));
                        for (var refIndex = tlasCell.firstRef; refIndex < endRef; refIndex = refIndex + 1u) {
                            let instanceIndex = vsdfSparseTLASInstanceRefs[refIndex];
                            if (instanceIndex >= arrayLength(&vsdfInstances)) {
                                continue;
                            }
                            let instance = vsdfInstances[instanceIndex];
                            let instanceT = vsdf_ray_aabb(rayOrigin, rayDirection, instance.worldBoundsMin, instance.worldBoundsMax, min(maxDistance, bestHit.t));
                            if (instanceT.x > instanceT.y || instanceT.x > cellExitT + 1.0e-3 || instanceT.y + 1.0e-3 < cellEnterT) {
                                continue;
                            }
                            var alreadyVisited = false;
                            for (var visitedIndex = 0u; visitedIndex < visitedInstanceCount; visitedIndex = visitedIndex + 1u) {
                                if (visitedInstances[visitedIndex] == instanceIndex) {
                                    alreadyVisited = true;
                                    break;
                                }
                            }
                            if (alreadyVisited) {
                                continue;
                            }
                            if (visitedInstanceCount < VSDF_SPARSE_TLAS_VISITED_CACHE_SIZE) {
                                visitedInstances[visitedInstanceCount] = instanceIndex;
                                visitedInstanceCount = visitedInstanceCount + 1u;
                            }
                            bestHit = trace_vsdf_instance(rayOrigin, rayDirection, min(maxDistance, bestHit.t), instanceIndex, bestHit, needNormal);
                        }
                    }
                }
            }
        }

        if (tMax.x <= tMax.y && tMax.x <= tMax.z) {
            rayT = tMax.x;
            cell.x = cell.x + step.x;
            tMax.x = tMax.x + tDelta.x;
        } else if (tMax.y <= tMax.z) {
            rayT = tMax.y;
            cell.y = cell.y + step.y;
            tMax.y = tMax.y + tDelta.y;
        } else {
            rayT = tMax.z;
            cell.z = cell.z + step.z;
            tMax.z = tMax.z + tDelta.z;
        }
    }

    return bestHit;
}

fn trace_vsdf_cascaded_instances(rayOrigin: vec3<f32>, rayDirection: vec3<f32>, maxDistance: f32, needNormal: bool) -> VSDFTraceHit {
    var bestHit = VSDFTraceHit(false, maxDistance, 0u, 0u, 0u, vec3<f32>(0.0, 0.0, 1.0));
    let cascadeCount = min(vsdfRuntimeInfo.cascadeCount, arrayLength(&vsdfCascades));
    if (cascadeCount == 0u) {
        let instanceCount = min(vsdfRuntimeInfo.instanceCount, arrayLength(&vsdfInstances));
        for (var instanceIndex = 0u; instanceIndex < instanceCount; instanceIndex = instanceIndex + 1u) {
            bestHit = trace_vsdf_instance(rayOrigin, rayDirection, maxDistance, instanceIndex, bestHit, needNormal);
        }
        return bestHit;
    }

    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex = cascadeIndex + 1u) {
        let cascade = vsdfCascades[cascadeIndex];
        if (cascade.instanceCount == 0u) {
            continue;
        }
        local_trace_stats_add(LOCAL_TRACE_CASCADE_TESTS, 1u);
        let cascadeT = vsdf_ray_aabb(rayOrigin, rayDirection, cascade.worldBoundsMin, cascade.worldBoundsMax, maxDistance);
        if (cascadeT.x > cascadeT.y || cascadeT.x > bestHit.t) {
            continue;
        }
        local_trace_stats_add(LOCAL_TRACE_CASCADE_HITS, 1u);
        let endInstance = min(cascade.firstInstance + cascade.instanceCount, arrayLength(&vsdfCascadeInstanceIndices));
        local_trace_stats_add(LOCAL_TRACE_CASCADE_INSTANCE_REFS, endInstance - cascade.firstInstance);
        for (var listIndex = cascade.firstInstance; listIndex < endInstance; listIndex = listIndex + 1u) {
            bestHit = trace_vsdf_instance(rayOrigin, rayDirection, maxDistance, vsdfCascadeInstanceIndices[listIndex], bestHit, needNormal);
        }
    }
    return bestHit;
}

fn unpack_rgba8(value: u32) -> vec4<f32> {
    return vec4<f32>(
        f32((value >> 0u) & 255u),
        f32((value >> 8u) & 255u),
        f32((value >> 16u) & 255u),
        f32((value >> 24u) & 255u)) / 255.0;
}

fn vsdf_cascade_output(uv: vec2<f32>, requireInstances: bool) -> vec4<f32> {
    local_trace_stats_add(LOCAL_TRACE_CASCADE_DEBUG_PIXELS, 1u);
    if (vsdfRuntimeInfo.cascadeCount == 0u) {
        return vec4<f32>(0.45, 0.05, 0.08, 1.0);
    }

    let rayDirection = make_debug_ray(uv);
    let cascadeCount = min(vsdfRuntimeInfo.cascadeCount, arrayLength(&vsdfCascades));
    var bestT = gUniforms.maxTraceDistance;
    var bestCascade = 0xffffffffu;
    var bestInstanceCount = 0u;
    for (var cascadeIndex = 0u; cascadeIndex < cascadeCount; cascadeIndex = cascadeIndex + 1u) {
        let cascade = vsdfCascades[cascadeIndex];
        if (requireInstances && cascade.instanceCount == 0u) {
            continue;
        }
        let cascadeT = vsdf_ray_aabb(gUniforms.cameraPosition, rayDirection, cascade.worldBoundsMin, cascade.worldBoundsMax, gUniforms.maxTraceDistance);
        if (cascadeT.x <= cascadeT.y && cascadeT.x <= bestT) {
            bestT = cascadeT.x;
            bestCascade = cascadeIndex;
            bestInstanceCount = cascade.instanceCount;
        }
    }

    if (bestCascade == 0xffffffffu) {
        return vec4<f32>(0.02, 0.025, 0.035, 1.0);
    }

    local_trace_stats_add(LOCAL_TRACE_CASCADE_DEBUG_HITS, 1u);
    let pressure = select(1.0, clamp(f32(bestInstanceCount) / 16.0, 0.15, 1.0), requireInstances);
    let color = cascade_debug_color(bestCascade) * pressure;
    return vec4<f32>(color, 1.0);
}

fn sample_vsdf_instance_albedo(instanceIndex: u32) -> vec4<f32> {
    if (instanceIndex >= arrayLength(&vsdfInstances) || vsdfRuntimeInfo.pageSize == 0u || vsdfRuntimeInfo.sheetWidth == 0u) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let instance = vsdfInstances[instanceIndex];
    if (instance.cardCount == 0u || instance.firstCard >= arrayLength(&vsdfCards)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let card = vsdfCards[instance.firstCard];
    if (card.pageCount == 0u || card.firstPage >= arrayLength(&vsdfPages)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    let page = vsdfPages[card.firstPage];
    let texelX = page.sheetPageX * vsdfRuntimeInfo.pageSize;
    let texelY = page.sheetPageY * vsdfRuntimeInfo.pageSize;
    let sheetTexels = vsdfRuntimeInfo.sheetWidth * vsdfRuntimeInfo.sheetHeight;
    let texelIndex = page.sheetIndex * sheetTexels + texelY * vsdfRuntimeInfo.sheetWidth + texelX;
    if (texelIndex >= arrayLength(&vsdfAlbedoPages)) {
        return vec4<f32>(0.0, 0.0, 0.0, 1.0);
    }
    return unpack_rgba8(vsdfAlbedoPages[texelIndex]);
}

fn vsdf_trace_output(uv: vec2<f32>) -> vec4<f32> {
    local_trace_stats_add(LOCAL_TRACE_LOCAL_MODE_PIXELS, 1u);
    if (gUniforms.mode == 9u) {
        return vsdf_cascade_output(uv, false);
    }
    if (gUniforms.mode == 10u) {
        return vsdf_cascade_output(uv, true);
    }
    if (vsdfRuntimeInfo.instanceCount == 0u) {
        local_trace_stats_add(LOCAL_TRACE_NO_CACHE_PIXELS, 1u);
        return vec4<f32>(0.45, 0.05, 0.08, 1.0);
    }

    local_trace_stats_add(LOCAL_TRACE_CACHE_PIXELS, 1u);
    let rayDirection = make_debug_ray(uv);
    let needNormal = gUniforms.mode == 7u || gUniforms.mode == 11u;
    var hit = trace_vsdf_sparse_tlas_instances(gUniforms.cameraPosition, rayDirection, gUniforms.maxTraceDistance, needNormal);
    if (vsdfSparseTLASInfo.pageCount == 0u) {
        hit = trace_vsdf_cascaded_instances(gUniforms.cameraPosition, rayDirection, gUniforms.maxTraceDistance, needNormal);
    }
    if (gUniforms.mode == 12u) {
        let normalizedStepCount = f32(hit.stepCount) / max(f32(gUniforms.maxSteps), 1.0);
        let color = step_count_heatmap(normalizedStepCount);
        let noTraceColor = gUniforms.traceHeatmapLowColor.rgb * 0.15;
        return vec4<f32>(select(noTraceColor, color, hit.stepCount > 0u), 1.0);
    }
    if (!hit.hit) {
        local_trace_stats_add(LOCAL_TRACE_NO_HIT_PIXELS, 1u);
        return vec4<f32>(0.02, 0.025, 0.035, 1.0);
    }
    local_trace_stats_add(LOCAL_TRACE_HIT_PIXELS, 1u);
    if (gUniforms.mode == 7u) {
        return vec4<f32>(hit.normal * 0.5 + vec3<f32>(0.5), 1.0);
    }
    if (gUniforms.mode == 8u) {
        local_trace_stats_add(LOCAL_TRACE_ALBEDO_SAMPLES, 1u);
        return sample_vsdf_instance_albedo(hit.instanceIndex);
    }
    if (gUniforms.mode == 11u) {
        local_trace_stats_add(LOCAL_TRACE_ALBEDO_SAMPLES, 1u);
        let albedo = sample_vsdf_instance_albedo(hit.instanceIndex).rgb;
        let normal = normalize(hit.normal);
        let cameraForward = normalize(make_debug_ray(vec2<f32>(0.5, 0.5)));
        let lightDir = normalize(-cameraForward);
        let viewDir = normalize(-rayDirection);
        let halfDir = normalize(lightDir + viewDir);
        let diffuse = max(dot(normal, lightDir), 0.0);
        let specular = pow(max(dot(normal, halfDir), 0.0), 48.0) * 0.28;
        let lit = albedo * (0.08 + 0.86 * diffuse) + vec3<f32>(specular);
        return vec4<f32>(lit, 1.0);
    }
    let normalizedT = clamp(hit.t / max(gUniforms.maxTraceDistance, 1.0e-3), 0.0, 1.0);
    return vec4<f32>(mix(vec3<f32>(0.15, 0.55, 1.0), vec3<f32>(1.0, 0.85, 0.20), normalizedT), 1.0);
}

fn trace_output(uv: vec2<f32>) -> vec4<f32> {
    if (gUniforms.mode >= 6u && gUniforms.mode <= 12u) {
        return vsdf_trace_output(uv);
    }

    let rayDirection = make_debug_ray(uv);
    let hit = trace_brixelizer_raw(
        gUniforms.cameraPosition,
        rayDirection,
        0.0,
        gUniforms.maxTraceDistance,
        make_trace_parameters());
    if (hit.hit) {
        let normalizedStepCount = f32(hit.stepCount) / max(f32(gUniforms.maxSteps), 1.0);
        if (gUniforms.mode == 1u) {
            return vec4<f32>(step_count_heatmap(normalizedStepCount), 1.0);
        }
        let normal = estimate_hit_normal(hit, rayDirection, make_trace_parameters());
        if (gUniforms.mode >= 2u && gUniforms.mode <= 5u) {
            let materialReference = sample_hit_material_reference(hit, normal);
            if (!material_reference_valid(materialReference.reference)) {
                return vec4<f32>(0.0, 0.0, 0.0, 1.0);
            }
            if (gUniforms.mode == 2u) {
                return vec4<f32>(hash_debug_color(materialReference.reference.x), 1.0);
            }
            if (gUniforms.mode == 3u) {
                let materialUv = material_reference_uv(materialReference.reference);
                return vec4<f32>(fract(materialUv), 1.0, 1.0);
            }
            if (gUniforms.mode == 4u) {
                let uvWindow = material_reference_uv_window(materialReference.reference);
                return vec4<f32>(abs(uvWindow), f32(materialReference.faceIndex) / 5.0, 1.0);
            }
            return evaluate_material_albedo(materialReference.reference);
        }
        let lightDirection = normalize(vec3<f32>(0.5, 0.7, 0.35));
        let lighting = clamp(dot(normal, lightDirection) * 0.5 + 0.5, 0.0, 1.0);
        let shaded = (normal * 0.5 + vec3<f32>(0.5)) * (0.35 + 0.65 * lighting);
        return vec4<f32>(shaded, 1.0);
    }

    if (gUniforms.mode == 1u) {
        let normalizedStepCount = f32(hit.stepCount) / max(f32(gUniforms.maxSteps), 1.0);
        return vec4<f32>(step_count_heatmap(normalizedStepCount), 1.0);
    }

    return vec4<f32>(0.0, 0.0, 0.0, 1.0);
}

@compute @workgroup_size(8, 8, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    if (globalInvocationId.x >= gUniforms.outputWidth || globalInvocationId.y >= gUniforms.outputHeight) {
        return;
    }

    let outputPixel = flipped_output_pixel(globalInvocationId.xy);
    local_trace_stats_add(LOCAL_TRACE_TOTAL_PIXELS, 1u);
    if (gUniforms.enabled == 0u) {
        local_trace_stats_add(LOCAL_TRACE_DISABLED_PIXELS, 1u);
        textureStore(outputTexture, outputPixel, vec4<f32>(0.0, 0.0, 0.0, 1.0));
        return;
    }

    let outputSize = vec2<f32>(f32(gUniforms.outputWidth), f32(gUniforms.outputHeight));
    let uv = (vec2<f32>(globalInvocationId.xy) + vec2<f32>(0.5)) / outputSize;
    textureStore(outputTexture, outputPixel, trace_output(uv));
}
