const MATERIAL_PASS_WORKGROUP_SIZE : u32 = 16u;
const MATERIAL_INVALID_MATERIAL_ID : u32 = 0xFFFFFFFFu;

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

struct MaterialDispatchIndirectArgs {
    groupCountX : u32,
    groupCountY : u32,
    groupCountZ : atomic<u32>,
};

@group(0) @binding(0) var<uniform> materialPassUniforms : MaterialPassUniforms;
@group(0) @binding(1) var<storage, read_write> materialTileBitset : array<atomic<u32>>;
@group(0) @binding(2) var<storage, read_write> materialDispatchArgs : array<MaterialDispatchIndirectArgs>;
@group(0) @binding(3) var<storage, read_write> materialTileList : array<u32>;
@group(0) @binding(4) var materialIdTexture : texture_2d<u32>;
@group(0) @binding(5) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(6) var outputTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(7) var baseColorTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(8) var normalTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(9) var materialPropertiesTexture : texture_storage_2d<rgba16float, write>;
@group(0) @binding(10) var emissiveTexture : texture_storage_2d<rgba16float, write>;

@compute @workgroup_size(MATERIAL_PASS_WORKGROUP_SIZE, MATERIAL_PASS_WORKGROUP_SIZE, 1)
fn clearColor(
    @builtin(global_invocation_id) globalInvocationId : vec3<u32>
) {
    if (globalInvocationId.x >= materialPassUniforms.viewportWidth || globalInvocationId.y >= materialPassUniforms.viewportHeight) {
        return;
    }

    let pixelCoords = vec2<i32>(globalInvocationId.xy);
    textureStore(outputTexture, pixelCoords, vec4<f32>(0.0, 0.0, 0.0, 0.0));
    textureStore(baseColorTexture, pixelCoords, vec4<f32>(0.0, 0.0, 0.0, 0.0));
    textureStore(normalTexture, pixelCoords, vec4<f32>(0.0, 0.0, 0.0, 0.0));
    textureStore(materialPropertiesTexture, pixelCoords, vec4<f32>(0.0, 0.0, 0.0, 0.0));
    textureStore(emissiveTexture, pixelCoords, vec4<f32>(0.0, 0.0, 0.0, 0.0));
}

@compute @workgroup_size(MATERIAL_PASS_WORKGROUP_SIZE, MATERIAL_PASS_WORKGROUP_SIZE, 1)
fn cullMaterials(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localInvocationId : vec3<u32>
) {
    if (workgroupId.x >= materialPassUniforms.tilesX || workgroupId.y >= materialPassUniforms.tilesY) {
        return;
    }

    let tileIndex = workgroupId.y * materialPassUniforms.tilesX + workgroupId.x;
    let tileOrigin = vec2<u32>(workgroupId.xy) * materialPassUniforms.tileSize;

    for (var localY = localInvocationId.y; localY < materialPassUniforms.tileSize; localY += MATERIAL_PASS_WORKGROUP_SIZE) {
        let pixelY = tileOrigin.y + localY;
        if (pixelY >= materialPassUniforms.viewportHeight) {
            continue;
        }

        for (var localX = localInvocationId.x; localX < materialPassUniforms.tileSize; localX += MATERIAL_PASS_WORKGROUP_SIZE) {
            let pixelX = tileOrigin.x + localX;
            if (pixelX >= materialPassUniforms.viewportWidth) {
                continue;
            }

            let pixelCoords = vec2<i32>(i32(pixelX), i32(pixelY));
            let depth = textureLoad(sceneDepthTexture, pixelCoords, 0);
            if (depth <= 0.0) {
                continue;
            }
            let materialId = textureLoad(materialIdTexture, pixelCoords, 0).x;
            if (materialId >= materialPassUniforms.materialCount) {
                continue;
            }

            let wordIndex = tileIndex / 32u;
            let bitMask = 1u << (tileIndex % 32u);
            let bitsetIndex = materialId * materialPassUniforms.wordsPerMaterial + wordIndex;
            atomicOr(&materialTileBitset[bitsetIndex], bitMask);
        }
    }
}

@compute @workgroup_size(1, 1, 1)
fn prepareMaterialDispatches(
    @builtin(global_invocation_id) globalInvocationId : vec3<u32>
) {
    let wordIndex = globalInvocationId.x;
    let materialIndex = globalInvocationId.y;
    if (wordIndex >= materialPassUniforms.wordsPerMaterial || materialIndex >= materialPassUniforms.materialCount) {
        return;
    }

    let bitsetIndex = materialIndex * materialPassUniforms.wordsPerMaterial + wordIndex;
    let mask = atomicLoad(&materialTileBitset[bitsetIndex]);
    if (mask == 0u) {
        return;
    }

    let tileListBase = materialIndex * materialPassUniforms.tileCount;
    let baseOffset = atomicAdd(&materialDispatchArgs[materialIndex].groupCountZ, countOneBits(mask));

    var localOffset = 0u;
    for (var bitIndex = 0u; bitIndex < 32u; bitIndex++) {
        let bitMask = 1u << bitIndex;
        if ((mask & bitMask) == 0u) {
            continue;
        }

        let tileIndex = wordIndex * 32u + bitIndex;
        if (tileIndex >= materialPassUniforms.tileCount) {
            break;
        }

        materialTileList[tileListBase + baseOffset + localOffset] = tileIndex;
        localOffset++;
    }
}
