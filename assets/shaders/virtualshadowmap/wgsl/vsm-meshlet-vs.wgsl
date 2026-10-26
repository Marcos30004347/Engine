#ifdef VSM_ENABLE_CLIP_DISTANCES
enable clip_distances;
#endif

#include "virtualgeometry/wgsl/virtualgeometrydata.wgsl"
#include "vsm-common.wgsl"

struct DrawUniforms {
    pageTableResolution: u32,
    physicalPageSize: u32,
    activeLayers: u32,
    currentLayer: u32,
    scratchResolution: u32,
    physicalAtlasResolution: u32,
    reverseZ: u32,
    maxVisibleClusterDrawsPerLayer: u32,
    rsmAtlasResolution: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
}

struct CascadeState {
    pageOffset: vec2<i32>,
    pageShift: vec2<i32>,
    _padding: vec4<u32>,
}

struct CascadeMatrix {
    view: mat4x4<f32>,
    proj: mat4x4<f32>,
    viewProj: mat4x4<f32>,
    worldExtent: f32,
    pageWorldSize: f32,
    lightIndex: u32,
    cascadeIndex: u32,
    pageTableResolution: u32,
    lightType: u32,
    faceIndex: u32,
    mipLevel: u32,
}

struct ShadowVisibleClusterInfo {
    pageIndex: u32,
    pageLocalClusterIndex: u32,
    instanceIndex: u32,
    layer: u32,
    meshPartIndex: u32,
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    pageCoordsPacked: u32,
    linearPageCoordsPacked: u32,
#else
    _padding: u32,
#endif
}

@group(0) @binding(0) var<uniform> uniforms: DrawUniforms;
@group(0) @binding(1) var<storage, read> instances: array<InstanceData>;
@group(0) @binding(2) var<storage, read> scenePageTable: array<PageTableEntry>;
@group(0) @binding(3) var<storage, read> scenePagesBuffer: array<u32>;
@group(0) @binding(4) var<storage, read> shadowVisibleClusterInfos: array<ShadowVisibleClusterInfo>;
@group(0) @binding(5) var<storage, read> virtualPageTable: array<u32>;
@group(0) @binding(6) var<storage, read> cascadeStates: array<CascadeState>;
@group(0) @binding(7) var<storage, read> cascadeMatrices: array<CascadeMatrix>;
@group(0) @binding(8) var<storage, read> meshPartTransforms: array<mat4x4<f32>>;
@group(0) @binding(10) var<storage, read> decodedClusterGeometryInfos: array<DecodedClusterGeometryInfo>;
@group(0) @binding(11) var<storage, read> decodedVertexBuffer: array<DecodedVertexData>;
@group(0) @binding(12) var<storage, read> decodedIndexBuffer: array<u32>;

#ifdef VSM_RSM_OUTPUTS
struct DirectionalLight {
    position: vec4<f32>,
    direction: vec4<f32>,
    color: vec4<f32>,
    params: vec4<f32>,
    metadata: vec4<u32>,
}

struct RsmMaterial {
    materialId: u32,
    baseColorTextureId: u32,
    normalTextureId: u32,
    flags: u32,
    baseColorOpacity: vec4<f32>,
    pbrFactors: vec4<f32>,
    emissiveFactorAndStrength: vec4<f32>,
}

struct VirtualTextureMipInfo {
    width: u32,
    height: u32,
    pageTableOffset: u32,
    pagesX: u32,
    pagesY: u32,
    _padding0: u32,
    _padding1: u32,
    _padding2: u32,
}

struct VirtualTextureEntry {
    mips: array<VirtualTextureMipInfo, 16>,
    mipCount: u32,
    pageSize: u32,
    totalPageCount: u32,
    flags: u32,
    addressModeU: u32,
    addressModeV: u32,
    filterMode: u32,
    mipBias: f32,
    minMip: u32,
    maxMip: u32,
    _padding0: u32,
    _padding1: u32,
}

struct VirtualTexturePageTableEntry {
    physicalPage: u32,
    flags: u32,
    _padding0: u32,
    _padding1: u32,
}

struct VirtualTextureAtlasInfo {
    pageSize: u32,
    physicalPagesPerAxis: u32,
    atlasWidth: u32,
    atlasHeight: u32,
    totalPhysicalPages: u32,
    textureCount: u32,
    materialCount: u32,
    pageTableEntryCount: u32,
}

@group(0) @binding(13) var<storage, read> directionalLights: array<DirectionalLight>;
@group(0) @binding(14) var rsmFluxAtlasTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(15) var rsmNormalAtlasTexture: texture_storage_2d<rgba8unorm, write>;
@group(0) @binding(16) var rsmPositionAtlasTexture: texture_storage_2d<rgba16float, write>;
@group(0) @binding(17) var<storage, read> rsmMaterials: array<RsmMaterial>;
@group(0) @binding(18) var<uniform> rsmVtAtlasInfo: VirtualTextureAtlasInfo;
@group(0) @binding(19) var<storage, read> rsmTextureEntries: array<VirtualTextureEntry>;
@group(0) @binding(20) var<storage, read> rsmVtPageTable: array<VirtualTexturePageTableEntry>;
@group(0) @binding(21) var<storage, read> rsmVtPhysicalPages: array<u32>;

const RSM_LIGHT_TYPE_DIRECTIONAL : u32 = 0u;
const RSM_LIGHT_TYPE_SPOT : u32 = 1u;
const RSM_LIGHT_TYPE_POINT : u32 = 2u;

struct RsmDirectLightSample {
    directionToLight: vec3<f32>,
    radiance: vec3<f32>,
}
#endif

struct VertexOutput {
    @builtin(position) clipPos: vec4<f32>,
#ifdef VSM_ENABLE_CLIP_DISTANCES
    @builtin(clip_distances) clipDistances: array<f32, 4>,
#endif
#ifdef VSM_RSM_OUTPUTS
    @location(0) worldNormal: vec3<f32>,
    @location(1) @interpolate(flat) materialIndex: u32,
    @location(2) @interpolate(flat) lightIndex: u32,
    @location(3) worldPosition: vec3<f32>,
    @location(4) uv: vec2<f32>,
    @location(5) @interpolate(flat) rsmTexelWorldArea: f32,
#endif
}

/// @brief Reads a single word from a meshlet descriptor field in the scene pages buffer.
/// @param pageBase The word base of the page in the scene pages buffer.
/// @param localIdx The local meshlet index within the page.
/// @param fieldOff The word offset of the desired field within the meshlet descriptor.
/// @returns The descriptor word at the specified field.
fn readDesc(pageBase: u32, localIdx: u32, fieldOff: u32) -> u32 {
    return scenePagesBuffer[pageBase + PAGE_HEADER_WORDS + localIdx * MESHLET_DESC_WORDS + fieldOff];
}

/// @brief Extracts a bit-packed unsigned integer spanning up to 32 bits from the scene pages buffer.
/// @param wordBase The base word index in the scene pages buffer.
/// @param bitOffset The bit offset from the base word where the value starts.
/// @param numBits The number of bits to extract.
/// @returns The extracted unsigned integer value.
fn extractBits(wordBase: u32, bitOffset: u32, numBits: u32) -> u32 {
    if (numBits == 0u) { return 0u; }
    let wi = wordBase + (bitOffset / 32u);
    let bi = bitOffset % 32u;
    var val = scenePagesBuffer[wi] >> bi;
    if (bi + numBits > 32u) {
        let overflow = (bi + numBits) - 32u;
        val |= scenePagesBuffer[wi + 1u] << (numBits - overflow);
    }
    let mask = select((1u << numBits) - 1u, 0xFFFFFFFFu, numBits == 32u);
    return val & mask;
}

/// @brief Decodes the quantized local-space position of a vertex from the packed position bit-stream.
/// @param posDataBase Absolute word offset of the position block in the scene pages buffer.
/// @param posOff Word offset of the meshlet's position data within the position block.
/// @param bitsX Number of bits used for the X component.
/// @param bitsY Number of bits used for the Y component.
/// @param bitsZ Number of bits used for the Z component.
/// @param qFactor Quantization exponent used to scale back to mesh space.
/// @param minQX Quantized-space minimum X (stored as f32).
/// @param minQY Quantized-space minimum Y (stored as f32).
/// @param minQZ Quantized-space minimum Z (stored as f32).
/// @param unitScale Per-instance unit scale factor.
/// @param vertexIdx The local vertex index within the meshlet.
/// @returns The decoded local-space vertex position.
fn decodePosition(posDataBase: u32, posOff: u32, bitsX: u32, bitsY: u32, bitsZ: u32, qFactor: u32, minQX: f32, minQY: f32, minQZ: f32, unitScale: f32, vertexIdx: u32) -> vec3<f32> {
    let bitsPerVert = bitsX + bitsY + bitsZ;
    let startBit = vertexIdx * bitsPerVert;
    let base = posDataBase + posOff;
    let qx = extractBits(base, startBit, bitsX);
    let qy = extractBits(base, startBit + bitsX, bitsY);
    let qz = extractBits(base, startBit + bitsX + bitsY, bitsZ);
    let dequantScale = f32(1u << qFactor) * unitScale;
    return vec3<f32>(
        (minQX + f32(qx)) / dequantScale,
        (minQY + f32(qy)) / dequantScale,
        (minQZ + f32(qz)) / dequantScale);
}

/// @brief Decodes a single byte-packed triangle index from the index buffer.
/// @param idxDataBase Absolute word offset of the index block in the scene pages buffer.
/// @param idxOff Word offset of the meshlet's index data within the index block.
/// @param indexIdx The corner index (0-based) to decode.
/// @returns The local vertex index referenced by that corner.
fn decodeIndex(idxDataBase: u32, idxOff: u32, indexIdx: u32) -> u32 {
    let word = scenePagesBuffer[idxDataBase + idxOff + indexIdx / 4u];
    let shift = (indexIdx % 4u) * 8u;
    return (word >> shift) & 0xFFu;
}

/// @brief Resolves the model matrix for a mesh part, combining the instance model matrix with an optional per-part transform.
/// @param instance The instance data containing the base model matrix and transform offset.
/// @param meshPartIndex The local mesh part index, or 0xFFFFFFFF to use the instance model matrix directly.
/// @returns The resolved world-space model matrix for the mesh part.
fn resolveMeshPartModelMatrix(instance: InstanceData, meshPartIndex: u32) -> mat4x4<f32> {
    if (meshPartIndex == 0xFFFFFFFFu) {
        return instance.modelMatrix;
    }
    return instance.modelMatrix * meshPartTransforms[instance.meshPartTransformsOffset + meshPartIndex];
}

/// @brief Computes the flat virtual page table index for the given layer and page coordinate.
/// @param layer The cascade layer index.
/// @param pageCoord The 2D page coordinate within the layer.
/// @returns The flat array index into the virtual page table.
fn vsm_vpt_index(layer: u32, pageCoord: vec2<u32>) -> u32 {
    let pagesPerLayer = uniforms.pageTableResolution * uniforms.pageTableResolution;
    return layer * pagesPerLayer + pageCoord.y * uniforms.pageTableResolution + pageCoord.x;
}

fn layer_page_table_resolution(layer: u32) -> u32 {
    return max(cascadeMatrices[layer].pageTableResolution, 1u);
}

#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
fn unpackPageCoords(packed: u32) -> vec2<u32> {
    return vec2<u32>(packed & 0xFFFFu, (packed >> 16u) & 0xFFFFu);
}

fn unpackLinearPageCoords(packed: u32) -> vec2<i32> {
    let packedX = packed & 0xFFFFu;
    let packedY = (packed >> 16u) & 0xFFFFu;
    let x = select(i32(packedX), i32(packedX | 0xFFFF0000u), (packedX & 0x8000u) != 0u);
    let y = select(i32(packedY), i32(packedY | 0xFFFF0000u), (packedY & 0x8000u) != 0u);
    return vec2<i32>(x, y);
}
#endif

/// @brief Shadow meshlet vertex shader that decodes and transforms cluster geometry into scratch-buffer clip space.
@vertex
fn vs_main(@builtin(vertex_index) vertex_index: u32, @builtin(instance_index) instance_index: u32) -> VertexOutput {
    var out: VertexOutput;
    out.clipPos = vec4<f32>(-2.0, -2.0, 0.0, 1.0);
#ifdef VSM_ENABLE_CLIP_DISTANCES
    out.clipDistances = array<f32, 4>(1.0, 1.0, 1.0, 1.0);
#endif
#ifdef VSM_RSM_OUTPUTS
    out.worldNormal = vec3<f32>(0.0, 1.0, 0.0);
    out.materialIndex = 0u;
    out.lightIndex = 0u;
    out.worldPosition = vec3<f32>(0.0);
    out.uv = vec2<f32>(0.0);
    out.rsmTexelWorldArea = 1.0;
#endif

#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let clusterSlotLocal = instance_index;
    let localCorner = vertex_index;
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    let clusterSlot = clusterSlotLocal;
#else
    let clusterSlot = uniforms.currentLayer * uniforms.maxVisibleClusterDrawsPerLayer + clusterSlotLocal;
#endif
    let clusterInfo = shadowVisibleClusterInfos[clusterSlot];
#else
    let clusterInfo = shadowVisibleClusterInfos[instance_index];
#endif
    let layer = clusterInfo.layer;
    if (layer >= uniforms.activeLayers
#ifndef VSM_CLIP_SPACE_ATLAS_DEPTH
        || layer != uniforms.currentLayer
#endif
    ) {
        return out;
    }

    let instance = instances[clusterInfo.instanceIndex];
    let pageEntry = scenePageTable[clusterInfo.pageIndex];
    let localMeshletIdx = clusterInfo.pageLocalClusterIndex;
    let clusterGeometry = decodedClusterGeometryInfos[globalClusterIndex(pageEntry, localMeshletIdx)];
    let vtxCount = clusterGeometry.vertexCount;
    let triCount = clusterGeometry.triangleCount;

#ifdef VSM_IMPERFECT_POINT_SPLATS
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let triangleIdx = localCorner;
    if (localCorner >= triCount) {
        return out;
    }
#else
    let triangleIdx = vertex_index;
    if (vertex_index >= triCount) {
        return out;
    }
#endif
    let indexBase = clusterGeometry.indexOffset + triangleIdx * 3u;
    let localVertID0 = decodedIndexBuffer[indexBase + 0u];
    let localVertID1 = decodedIndexBuffer[indexBase + 1u];
    let localVertID2 = decodedIndexBuffer[indexBase + 2u];
    if (localVertID0 >= vtxCount || localVertID1 >= vtxCount || localVertID2 >= vtxCount) {
        return out;
    }

    let vertexData0 = decodedVertexBuffer[clusterGeometry.vertexOffset + localVertID0];
    let vertexData1 = decodedVertexBuffer[clusterGeometry.vertexOffset + localVertID1];
    let vertexData2 = decodedVertexBuffer[clusterGeometry.vertexOffset + localVertID2];
    let localPos =
        (vec3<f32>(vertexData0.positionX, vertexData0.positionY, vertexData0.positionZ) +
         vec3<f32>(vertexData1.positionX, vertexData1.positionY, vertexData1.positionZ) +
         vec3<f32>(vertexData2.positionX, vertexData2.positionY, vertexData2.positionZ)) * (1.0 / 3.0);
#ifdef VSM_RSM_OUTPUTS
    let localNormal = normalize(
        vec3<f32>(vertexData0.normalX, vertexData0.normalY, vertexData0.normalZ) +
        vec3<f32>(vertexData1.normalX, vertexData1.normalY, vertexData1.normalZ) +
        vec3<f32>(vertexData2.normalX, vertexData2.normalY, vertexData2.normalZ));
    let localUv =
        (vec2<f32>(vertexData0.uvX, vertexData0.uvY) +
         vec2<f32>(vertexData1.uvX, vertexData1.uvY) +
         vec2<f32>(vertexData2.uvX, vertexData2.uvY)) * (1.0 / 3.0);
#endif
#else
    let maxCorners = triCount * 3u;
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    let cornerIdx = min(localCorner, maxCorners - 1u);
#else
    let cornerIdx = min(vertex_index, maxCorners - 1u);
#endif
    let localVertID = decodedIndexBuffer[clusterGeometry.indexOffset + cornerIdx];
#ifdef DRAW_INDIRECT_COUNT_DISABLED
    if (localCorner >= maxCorners || localVertID >= vtxCount) {
        return out;
    }
#else
    if (vertex_index >= maxCorners || localVertID >= vtxCount) {
        return out;
    }
#endif

    let vertexData = decodedVertexBuffer[clusterGeometry.vertexOffset + localVertID];
    let localPos = vec3<f32>(vertexData.positionX, vertexData.positionY, vertexData.positionZ);
#ifdef VSM_RSM_OUTPUTS
    let localNormal = vec3<f32>(vertexData.normalX, vertexData.normalY, vertexData.normalZ);
    let localUv = vec2<f32>(vertexData.uvX, vertexData.uvY);
#endif
#endif
    let nodeModelMatrix = resolveMeshPartModelMatrix(instance, clusterInfo.meshPartIndex);
    let worldPos = (nodeModelMatrix * vec4<f32>(localPos, 1.0)).xyz;
#ifdef VSM_RSM_OUTPUTS
    let rawWorldNormal = (nodeModelMatrix * vec4<f32>(localNormal, 0.0)).xyz;
    let rawWorldNormalLength = length(rawWorldNormal);
    out.worldNormal = select(vec3<f32>(0.0, 1.0, 0.0), rawWorldNormal / rawWorldNormalLength, rawWorldNormalLength > 1.0e-6);
    out.materialIndex = instance.materialIndex;
    out.lightIndex = cascadeMatrices[layer].lightIndex;
    out.worldPosition = worldPos;
    out.uv = localUv;
    let rsmAtlasScale = f32(max(uniforms.rsmAtlasResolution, 1u)) / f32(max(uniforms.physicalAtlasResolution, 1u));
    let rsmTexelWorldSize = (cascadeMatrices[layer].pageWorldSize / f32(max(uniforms.physicalPageSize, 1u))) / max(rsmAtlasScale, 1.0e-6);
    out.rsmTexelWorldArea = rsmTexelWorldSize * rsmTexelWorldSize;
#endif

    let lightClip = cascadeMatrices[layer].viewProj * vec4<f32>(worldPos, 1.0);
    let invW = select(1.0, 1.0 / lightClip.w, abs(lightClip.w) > 1e-6);
    let ndc = lightClip.xyz * invW;
    let uv = ndc.xy * 0.5 + vec2<f32>(0.5, 0.5);
    let homogeneous_w = lightClip.w;
    let logical_page_table_resolution = f32(layer_page_table_resolution(layer));
    let storage_page_table_resolution = f32(max(uniforms.pageTableResolution, 1u));
    let layer_uv_scale = logical_page_table_resolution / storage_page_table_resolution;
#ifdef VSM_CLIP_SPACE_ATLAS_DEPTH
    let wrappedPageCoord = unpackPageCoords(clusterInfo.pageCoordsPacked);
    let linearPageCoord = unpackLinearPageCoords(clusterInfo.linearPageCoordsPacked);
    let pageSpace = uv * logical_page_table_resolution;
    let localPageUV = pageSpace - vec2<f32>(linearPageCoord);
    let physicalPageSize = f32(max(uniforms.physicalPageSize, 1u));
    let vptEntry = virtualPageTable[vsm_vpt_index(layer, wrappedPageCoord)];
    let atlasPageOrigin = vec2<u32>(get_page_x(vptEntry), get_page_y(vptEntry)) * max(uniforms.physicalPageSize, 1u);
    let atlasPixel = vec2<f32>(atlasPageOrigin) + localPageUV * physicalPageSize;
    let atlasUV = atlasPixel / f32(max(uniforms.physicalAtlasResolution, 1u));
    out.clipPos = vec4<f32>(
        (atlasUV * 2.0 - vec2<f32>(1.0, 1.0)) * homogeneous_w,
        lightClip.z,
        homogeneous_w);
#ifdef VSM_ENABLE_CLIP_DISTANCES
    let pageTexelMargin = 0.5 / physicalPageSize;
    let homogeneousPageSpace = (lightClip.xy * 0.5 + vec2<f32>(homogeneous_w * 0.5)) * logical_page_table_resolution;
    let pageMin = vec2<f32>(linearPageCoord) - vec2<f32>(pageTexelMargin);
    let pageMax = vec2<f32>(linearPageCoord) + vec2<f32>(1.0 + pageTexelMargin);
    out.clipDistances = array<f32, 4>(
        homogeneousPageSpace.x - pageMin.x * homogeneous_w,
        homogeneousPageSpace.y - pageMin.y * homogeneous_w,
        pageMax.x * homogeneous_w - homogeneousPageSpace.x,
        pageMax.y * homogeneous_w - homogeneousPageSpace.y);
#endif
#else
    out.clipPos = vec4<f32>(((uv * layer_uv_scale) * 2.0 - vec2<f32>(1.0, 1.0)) * homogeneous_w, lightClip.z, homogeneous_w);
#endif
    return out;
}

#ifdef VSM_RSM_OUTPUTS
struct RsmFragmentInput {
    @builtin(position) fragCoord: vec4<f32>,
    @location(0) worldNormal: vec3<f32>,
    @location(1) @interpolate(flat) materialIndex: u32,
    @location(2) @interpolate(flat) lightIndex: u32,
    @location(3) worldPosition: vec3<f32>,
    @location(4) uv: vec2<f32>,
    @location(5) @interpolate(flat) rsmTexelWorldArea: f32,
}

const MATERIAL_INVALID_TEXTURE_ID: u32 = 0xFFFFFFFFu;
fn loadRsmMaterial(materialIndex: u32) -> RsmMaterial {
    var material: RsmMaterial;
    material.materialId = materialIndex;
    material.baseColorTextureId = MATERIAL_INVALID_TEXTURE_ID;
    material.normalTextureId = MATERIAL_INVALID_TEXTURE_ID;
    material.flags = 0u;
    material.baseColorOpacity = vec4<f32>(0.85, 0.85, 0.85, 1.0);
    material.pbrFactors = vec4<f32>(0.5, 0.0, 1.0, 0.0);
    material.emissiveFactorAndStrength = vec4<f32>(0.0);
    if (materialIndex < arrayLength(&rsmMaterials)) {
        material = rsmMaterials[materialIndex];
    }
    return material;
}

fn octSignNotZero(v: vec2<f32>) -> vec2<f32> {
    return vec2<f32>(
        select(-1.0, 1.0, v.x >= 0.0),
        select(-1.0, 1.0, v.y >= 0.0));
}

fn encodeOctahedralNormal(normal: vec3<f32>) -> vec2<f32> {
    let invL1 = 1.0 / max(abs(normal.x) + abs(normal.y) + abs(normal.z), 1.0e-6);
    var p = normal.xy * invL1;
    if (normal.z < 0.0) {
        p = (vec2<f32>(1.0) - abs(p.yx)) * octSignNotZero(p);
    }
    return p * 0.5 + vec2<f32>(0.5);
}

fn rsmApplyNormalTexture(geometryNormal: vec3<f32>, worldPosition: vec3<f32>, uv: vec2<f32>, textureIndex: u32) -> vec3<f32> {
    _ = worldPosition;
    _ = uv;
    _ = textureIndex;
    return geometryNormal;
}

fn rsmAddressCoord(coord: f32, mode: u32) -> f32 {
    if (mode == 1u) {
        return clamp(coord, 0.0, 0.99999994);
    }
    return fract(coord);
}

fn rsmTexturePixel(textureIndex: u32, mipLevel: u32, uv: vec2<f32>) -> vec2<u32> {
    let addressedUv = vec2<f32>(
        rsmAddressCoord(uv.x, rsmTextureEntries[textureIndex].addressModeU),
        rsmAddressCoord(uv.y, rsmTextureEntries[textureIndex].addressModeV));
    let mipWidth = rsmTextureEntries[textureIndex].mips[mipLevel].width;
    let mipHeight = rsmTextureEntries[textureIndex].mips[mipLevel].height;
    let dims = vec2<u32>(max(mipWidth, 1u), max(mipHeight, 1u));
    let pixel = vec2<u32>(floor(addressedUv * vec2<f32>(dims)));
    return min(pixel, dims - vec2<u32>(1u, 1u));
}

fn rsmSamplePhysicalPage(physicalPage: u32, pixel: vec2<u32>) -> vec4<f32> {
    let pageSize = max(rsmVtAtlasInfo.pageSize, 1u);
    let local = pixel % vec2<u32>(pageSize, pageSize);
    let texelIndex = physicalPage * pageSize * pageSize + local.y * pageSize + local.x;
    if (texelIndex >= arrayLength(&rsmVtPhysicalPages)) {
        return vec4<f32>(1.0);
    }

    let packed = rsmVtPhysicalPages[texelIndex];
    return vec4<f32>(
        f32(packed & 0xFFu) / 255.0,
        f32((packed >> 8u) & 0xFFu) / 255.0,
        f32((packed >> 16u) & 0xFFu) / 255.0,
        f32((packed >> 24u) & 0xFFu) / 255.0);
}

fn rsmSampleFinestResidentVirtualTexture(textureIndex: u32, uv: vec2<f32>) -> vec4<f32> {
    if (textureIndex == MATERIAL_INVALID_TEXTURE_ID || textureIndex >= arrayLength(&rsmTextureEntries)) {
        return vec4<f32>(1.0);
    }

    let mipCount = rsmTextureEntries[textureIndex].mipCount;
    if (mipCount == 0u) {
        return vec4<f32>(1.0);
    }

    let minMip = min(rsmTextureEntries[textureIndex].minMip, mipCount - 1u);
    let maxMip = min(rsmTextureEntries[textureIndex].maxMip, mipCount - 1u);
    for (var mipLevel = minMip; mipLevel <= maxMip; mipLevel++) {
        let pagesX = rsmTextureEntries[textureIndex].mips[mipLevel].pagesX;
        let pagesY = rsmTextureEntries[textureIndex].mips[mipLevel].pagesY;
        if (pagesX == 0u || pagesY == 0u) {
            continue;
        }

        let pixel = rsmTexturePixel(textureIndex, mipLevel, uv);
        let pageX = min(pixel.x / max(rsmVtAtlasInfo.pageSize, 1u), pagesX - 1u);
        let pageY = min(pixel.y / max(rsmVtAtlasInfo.pageSize, 1u), pagesY - 1u);
        let pageTableOffset = rsmTextureEntries[textureIndex].mips[mipLevel].pageTableOffset;
        let pageTableIndex = pageTableOffset + pageY * pagesX + pageX;
        if (pageTableIndex >= arrayLength(&rsmVtPageTable)) {
            continue;
        }

        let pageEntry = rsmVtPageTable[pageTableIndex];
        if ((pageEntry.flags & 0x1u) != 0u && pageEntry.physicalPage != MATERIAL_INVALID_TEXTURE_ID) {
            return rsmSamplePhysicalPage(pageEntry.physicalPage, pixel);
        }
    }

    return vec4<f32>(1.0);
}

fn rsmEvaluateDirectLight(lightIndex: u32, worldPosition: vec3<f32>) -> RsmDirectLightSample {
    var sample: RsmDirectLightSample;
    sample.directionToLight = vec3<f32>(0.0, 1.0, 0.0);
    sample.radiance = vec3<f32>(0.0, 0.0, 0.0);
    if (lightIndex >= arrayLength(&directionalLights)) {
        return sample;
    }

    let light = directionalLights[lightIndex];
    let lightType = light.metadata.x;
    let lightColor = max(light.color.rgb, vec3<f32>(0.0, 0.0, 0.0));
    let lightIntensity = max(light.color.a, 0.0);
    if (lightType == RSM_LIGHT_TYPE_DIRECTIONAL) {
        sample.directionToLight = normalize(-light.direction.xyz);
        sample.radiance = lightColor * lightIntensity;
        return sample;
    }

    let toLight = light.position.xyz - worldPosition;
    let distanceSq = dot(toLight, toLight);
    if (distanceSq <= 1.0e-8) {
        return sample;
    }

    let distanceToLight = sqrt(distanceSq);
    let range = max(light.params.x, 1.0e-4);
    let effectiveRange = select(range, range * 1.15, lightType == RSM_LIGHT_TYPE_POINT);
    if (distanceToLight > effectiveRange) {
        return sample;
    }

    let directionToLight = toLight / distanceToLight;
    let normalizedDistance = clamp(distanceToLight / effectiveRange, 0.0, 1.0);
    var attenuation = pow(1.0 - smoothstep(0.0, 1.0, normalizedDistance), 2.0) / max(distanceSq, 1.0e-4);
    if (lightType == RSM_LIGHT_TYPE_SPOT) {
        let lightDirection = normalize(light.direction.xyz);
        let outerAngle = clamp(light.params.y, 0.0, 3.14159265);
        let cosOuter = cos(outerAngle * 0.5);
        let cosInner = min(cosOuter + 0.1, 1.0);
        let spotCos = dot(-directionToLight, lightDirection);
        attenuation = attenuation * smoothstep(cosOuter, cosInner, spotCos);
    }

    sample.directionToLight = directionToLight;
    sample.radiance = lightColor * lightIntensity * attenuation;
    return sample;
}

@fragment
@early_depth_test(force)
fn fs_main(in: RsmFragmentInput) {
    let normalLength = length(in.worldNormal);
    let geometryNormal = select(vec3<f32>(0.0, 1.0, 0.0), in.worldNormal / normalLength, normalLength > 1.0e-6);
    let material = loadRsmMaterial(in.materialIndex);
    let normal = geometryNormal;
    let roughness = clamp(material.pbrFactors.x, 0.0, 1.0);
    let metallic = clamp(material.pbrFactors.y, 0.0, 1.0);
    let baseColorOpacity = material.baseColorOpacity;
    let opacity = clamp(material.pbrFactors.z * baseColorOpacity.a, 0.0, 1.0);
    let diffuseReflectance = max(baseColorOpacity.rgb, vec3<f32>(0.0)) * (1.0 - metallic) * opacity;
    let directLight = rsmEvaluateDirectLight(in.lightIndex, in.worldPosition);
    let incomingIrradiance = max(dot(normal, directLight.directionToLight), 0.0);
    let reflectedFlux = diffuseReflectance * directLight.radiance * incomingIrradiance * max(in.rsmTexelWorldArea, 1.0e-8);
    _ = roughness;
    let rsmAtlasResolution = max(uniforms.rsmAtlasResolution, 1u);
    let atlasScale = f32(rsmAtlasResolution) / f32(max(uniforms.physicalAtlasResolution, 1u));
    let scaledTexel = min(vec2<u32>(floor(in.fragCoord.xy * atlasScale)), vec2<u32>(rsmAtlasResolution - 1u));
    let texel = vec2<i32>(scaledTexel);
    let encodedNormal = encodeOctahedralNormal(normal);
    textureStore(rsmFluxAtlasTexture, texel, vec4<f32>(reflectedFlux, 1.0));
    textureStore(rsmNormalAtlasTexture, texel, vec4<f32>(encodedNormal, 0.0, 1.0));
    textureStore(rsmPositionAtlasTexture, texel, vec4<f32>(in.worldPosition, 1.0));
}
#endif
