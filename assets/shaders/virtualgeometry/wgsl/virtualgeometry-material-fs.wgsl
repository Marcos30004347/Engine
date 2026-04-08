struct FragmentInput {
    @builtin(position) clipPos : vec4<f32>,
    @location(0) @interpolate(flat) materialId : u32,
};

struct MaterialPassUniforms {
    feedbackStride : u32,
    feedbackRequestCapacity : u32,
    _padding0 : u32,
    _padding1 : u32,
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

struct MaterialEntry {
    textureIndices : array<u32, 5>,
    textureCount : u32,
    _padding0 : u32,
    _padding1 : u32,
    _padding2 : u32,
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

@group(0) @binding(1) var<uniform> materialPassUniforms : MaterialPassUniforms;
@group(0) @binding(2) var<uniform> atlasInfo : AtlasInfo;
@group(0) @binding(3) var<storage, read> materialEntries : array<MaterialEntry>;
@group(0) @binding(4) var<storage, read> textureEntries : array<VirtualTextureEntry>;
@group(0) @binding(5) var<storage, read> vtPageTable : array<VirtualTexturePageTableEntry>;
@group(0) @binding(6) var<storage, read> vtPhysicalPages : array<u32>;
@group(0) @binding(7) var<storage, read_write> feedbackBuffer : FeedbackBuffer;
@group(0) @binding(9) var materialUVTexture : texture_2d<f32>;
@group(0) @binding(10) var shadowLightingTexture : texture_2d<f32>;

/// @brief Applies a texture address mode (repeat or clamp) to a single UV coordinate component.
/// @param coord The input UV coordinate component.
/// @param mode The address mode: 1 for clamp-to-edge, any other value for repeat (fract).
/// @returns The wrapped or clamped coordinate.
fn applyAddressMode(coord: f32, mode: u32) -> f32 {
    if (mode == 1u) {
        return clamp(coord, 0.0, 1.0);
    }
    return coord - floor(coord);
}

/// @brief Applies the U and V address modes for the given virtual texture to a UV coordinate pair.
/// @param textureIndex The index of the virtual texture entry whose address modes are used.
/// @param uv The input UV coordinate.
/// @returns The UV coordinate with per-axis address modes applied.
fn sampleUV(textureIndex: u32, uv: vec2<f32>) -> vec2<f32> {
    return vec2<f32>(
        applyAddressMode(uv.x, textureEntries[textureIndex].addressModeU),
        applyAddressMode(uv.y, textureEntries[textureIndex].addressModeV)
    );
}

/// @brief Converts a UV coordinate to an integer pixel coordinate within a specific mip level of a virtual texture.
/// @param textureIndex The index of the virtual texture entry.
/// @param mipLevel The mip level to use for dimension lookup.
/// @param uv The UV coordinate (before address mode application).
/// @returns The clamped integer pixel coordinate within the mip level.
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

/// @brief Converts a UV coordinate to a continuous texel position (with 0.5 texel offset) within a specific mip level, suitable for bilinear filtering.
/// @param textureIndex The index of the virtual texture entry.
/// @param mipLevel The mip level to use for dimension lookup.
/// @param uv The UV coordinate (before address mode application).
/// @returns The continuous texel position within the mip level.
fn textureMipTexel(textureIndex: u32, mipLevel: u32, uv: vec2<f32>) -> vec2<f32> {
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    let wrapped = sampleUV(textureIndex, uv);
    let width = f32(max(mipInfo.width, 1u));
    let height = f32(max(mipInfo.height, 1u));
    return vec2<f32>(wrapped.x * width - 0.5, wrapped.y * height - 0.5);
}

/// @brief Computes the appropriate mip level to sample for a virtual texture at the current fragment, taking mip bias and clamping into account.
/// @param textureIndex The index of the virtual texture entry.
/// @param uv The interpolated UV coordinate at the current fragment.
/// @returns The selected mip level index clamped to [minMip, maxMip].
fn computeRequestedMip(textureIndex: u32, uv: vec2<f32>) -> u32 {
    if (textureEntries[textureIndex].mipCount == 0u) {
        return 0u;
    }

    let baseMip = textureEntries[textureIndex].mips[0];
    let texelScale = vec2<f32>(f32(max(baseMip.width, 1u)), f32(max(baseMip.height, 1u)));
    let dx = dpdx(uv * texelScale);
    let dy = dpdy(uv * texelScale);
    let footprint = max(dot(dx, dx), dot(dy, dy));
    let unclampedMip = max(0.0, floor(0.5 * log2(max(footprint, 1e-8))) + textureEntries[textureIndex].mipBias);
    let highestMip = textureEntries[textureIndex].mipCount - 1u;
    let minMip = min(textureEntries[textureIndex].minMip, highestMip);
    let maxMip = min(max(textureEntries[textureIndex].maxMip, minMip), highestMip);
    return u32(clamp(unclampedMip, f32(minMip), f32(maxMip)));
}

/// @brief Submits a streaming feedback request for the virtual texture page needed at the current pixel, skipping pixels outside the feedback stride and already-resident pages.
/// @param textureIndex The index of the virtual texture to request a page for.
/// @param uv The UV coordinate at the current fragment.
/// @param pixelCoords The integer screen pixel coordinates, used for feedback stride filtering.
fn requestVirtualPage(textureIndex: u32, uv: vec2<f32>, pixelCoords: vec2<u32>) {
    let stride = max(materialPassUniforms.feedbackStride, 1u);
    if ((pixelCoords.x % stride) != 0u || (pixelCoords.y % stride) != 0u) {
        return;
    }

    let requestedMip = computeRequestedMip(textureIndex, uv);
    let mipInfo = textureEntries[textureIndex].mips[requestedMip];
    if (mipInfo.pagesX == 0u || mipInfo.pagesY == 0u) {
        return;
    }

    let mipPixel = textureMipPixel(textureIndex, requestedMip, uv);
    let requestedPageX = min(mipPixel.x / atlasInfo.pageSize, mipInfo.pagesX - 1u);
    let requestedPageY = min(mipPixel.y / atlasInfo.pageSize, mipInfo.pagesY - 1u);
    let requestedPageIndex = mipInfo.pageTableOffset + requestedPageY * mipInfo.pagesX + requestedPageX;
    if (requestedPageIndex < arrayLength(&vtPageTable)) {
        let residentEntry = vtPageTable[requestedPageIndex];
        if ((residentEntry.flags & 0x1u) != 0u && residentEntry.physicalPage != 0xFFFFFFFFu) {
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

/// @brief Resolves the physical page backing a virtual texture sample, starting from the requested mip and falling back to coarser mips until a resident page is found.
/// @param textureIndex The index of the virtual texture to resolve.
/// @param uv The UV coordinate to resolve.
/// @param firstMip The preferred (finest) mip level to start resolution from.
/// @returns A ResolvedVirtualPage with the physical page index, resolved mip level, and pixel coordinates, or an invalid result if no resident page is found.
fn resolveVirtualPageFromMip(textureIndex: u32, uv: vec2<f32>, firstMip: u32) -> ResolvedVirtualPage {
    var resolved : ResolvedVirtualPage;
    resolved.valid = false;
    resolved.physicalPage = 0xFFFFFFFFu;
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
            if ((pageEntry.flags & 0x1u) != 0u && pageEntry.physicalPage != 0xFFFFFFFFu) {
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

/// @brief Resolves the physical page for a virtual texture sample at the automatically computed mip level.
/// @param textureIndex The index of the virtual texture to resolve.
/// @param uv The UV coordinate to resolve.
/// @returns A ResolvedVirtualPage containing the physical page and pixel coordinates, or an invalid result if no resident page is found.
fn resolveVirtualPage(textureIndex: u32, uv: vec2<f32>) -> ResolvedVirtualPage {
    return resolveVirtualPageFromMip(textureIndex, uv, computeRequestedMip(textureIndex, uv));
}

/// @brief Reads an RGBA texel from the physical page atlas buffer for a given physical page and texel coordinate.
/// @param physicalPage The flat physical page index within the atlas.
/// @param pixelX The global pixel X coordinate (local offset is computed via modulo with pageSize).
/// @param pixelY The global pixel Y coordinate (local offset is computed via modulo with pageSize).
/// @returns The decoded RGBA color as a vec4<f32> with each channel in [0, 1].
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

/// @brief Samples a virtual texture at a specific mip level, resolving the physical page and returning the texel color, or black if no resident page is found.
/// @param textureIndex The index of the virtual texture to sample.
/// @param uv The UV coordinate to sample.
/// @param mipLevel The mip level at which to begin page resolution.
/// @returns The sampled RGBA color, or vec4(0.0) if the page is not resident.
fn sampleVirtualTextureAtMip(textureIndex: u32, uv: vec2<f32>, mipLevel: u32) -> vec4<f32> {
    let resolved = resolveVirtualPageFromMip(textureIndex, uv, mipLevel);
    if (!resolved.valid) {
        return vec4<f32>(0.0);
    }
    return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
}

/// @brief Samples a virtual texture using the appropriate filter mode (nearest or bilinear), resolving the physical atlas and performing 2x2 bilinear blending when enabled.
/// @param textureIndex The index of the virtual texture to sample.
/// @param uv The UV coordinate to sample.
/// @returns The filtered RGBA color, or vec4(0.0) if no resident page is found.
fn sampleVirtualTexture(textureIndex: u32, uv: vec2<f32>) -> vec4<f32> {
    if (textureEntries[textureIndex].filterMode == 0u) {
        let resolved = resolveVirtualPage(textureIndex, uv);
        if (!resolved.valid) {
            return vec4<f32>(0.0);
        }
        return samplePhysicalPage(resolved.physicalPage, resolved.pixelX, resolved.pixelY);
    }

    let resolved = resolveVirtualPage(textureIndex, uv);
    if (!resolved.valid) {
        return vec4<f32>(0.0);
    }

    let mipLevel = resolved.mipLevel;
    let mipInfo = textureEntries[textureIndex].mips[mipLevel];
    let texelPos = textureMipTexel(textureIndex, mipLevel, uv);
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

    let c00 = sampleVirtualTextureAtMip(textureIndex, uv00, mipLevel);
    let c10 = sampleVirtualTextureAtMip(textureIndex, uv10, mipLevel);
    let c01 = sampleVirtualTextureAtMip(textureIndex, uv01, mipLevel);
    let c11 = sampleVirtualTextureAtMip(textureIndex, uv11, mipLevel);

    let cx0 = mix(c00, c10, frac.x);
    let cx1 = mix(c01, c11, frac.x);
    return mix(cx0, cx1, frac.y);
}

struct MaterialOutput {
    @location(0) color : vec4<f32>,
}

/// @brief Fragment shader that resolves the material's base color virtual texture for the current tile-material quad and outputs the sampled color.
@fragment
fn fs_main(in: FragmentInput) -> MaterialOutput {
    let pixel = vec2<i32>(in.clipPos.xy);
    let pixelCoords = vec2<u32>(pixel);

    var out : MaterialOutput;

    if (in.materialId >= arrayLength(&materialEntries)) {
        out.color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        return out;
    }

    let reconstructedUV = textureLoad(materialUVTexture, pixel, 0).xy;

    if (materialEntries[in.materialId].textureCount == 0u || materialEntries[in.materialId].textureIndices[0] == 0xFFFFFFFFu) {
        out.color = vec4<f32>(vec3<f32>(0.85), 1.0);
        return out;
    }

    let baseTextureIndex = materialEntries[in.materialId].textureIndices[0];
    if (baseTextureIndex >= arrayLength(&textureEntries)) {
        out.color = vec4<f32>(0.0, 0.0, 0.0, 1.0);
        return out;
    }

    requestVirtualPage(baseTextureIndex, reconstructedUV, pixelCoords);

    let baseColor = sampleVirtualTexture(baseTextureIndex, reconstructedUV);
    let shadowLighting = clamp(textureLoad(shadowLightingTexture, pixel, 0), vec4<f32>(0.0), vec4<f32>(1.0));
    out.color = vec4<f32>(baseColor.rgb * shadowLighting.rgb, baseColor.a);
    return out;
}
