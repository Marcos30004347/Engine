const INVALID_MATERIAL_ID : u32 = 0xFFFFFFFFu;
const TILE_WORKGROUP_SIZE : u32 = 8u;
const HASH_CAPACITY : u32 = 256u;

struct TileUniforms {
    viewportWidth : u32,
    viewportHeight : u32,
    tileSize : u32,
    tilesX : u32,
    tilesY : u32,
    materialCount : u32,
    maxDrawEntries : u32,
    _padding0 : u32,
};

struct TileMaterialDrawEntry {
    tileX : u32,
    tileY : u32,
    materialId : u32,
    _padding0 : u32,
};

struct TileDrawCounters {
    drawCount : atomic<u32>,
    overflowCount : atomic<u32>,
    _padding0 : u32,
    _padding1 : u32,
};

@group(0) @binding(0) var<uniform> tileUniforms : TileUniforms;
@group(0) @binding(1) var<storage, read_write> tileDrawCounters : TileDrawCounters;
@group(0) @binding(2) var<storage, read_write> tileDrawEntries : array<TileMaterialDrawEntry>;
@group(0) @binding(3) var<storage, read_write> materialDrawIndirect : array<u32>;
@group(0) @binding(4) var sceneDepthTexture : texture_depth_2d;
@group(0) @binding(5) var materialIdTexture : texture_2d<u32>;

var<workgroup> materialSet : array<atomic<u32>, HASH_CAPACITY>;

/// @brief Inserts a material ID into the workgroup-local hash set using open addressing, incrementing the overflow counter if the table is full.
/// @param materialId The material identifier to insert.
fn insertMaterial(materialId: u32) {
    let hash = materialId * 0x9E3779B9u;
    for (var probe = 0u; probe < HASH_CAPACITY; probe++) {
        let slot = (hash + probe) & (HASH_CAPACITY - 1u);
        let result = atomicCompareExchangeWeak(&materialSet[slot], INVALID_MATERIAL_ID, materialId);
        if (result.exchanged || result.old_value == materialId) {
            return;
        }
    }
    atomicAdd(&tileDrawCounters.overflowCount, 1u);
}

/// @brief Compute shader that collects the set of unique materials visible within each screen tile and appends one draw entry per material-tile pair to the global draw list.
@compute @workgroup_size(TILE_WORKGROUP_SIZE, TILE_WORKGROUP_SIZE, 1)
fn prepareTileMaterials(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localInvocationId : vec3<u32>,
    @builtin(local_invocation_index) localInvocationIndex : u32
) {
    if (workgroupId.x >= tileUniforms.tilesX || workgroupId.y >= tileUniforms.tilesY) {
        return;
    }

    for (var initIndex = localInvocationIndex; initIndex < HASH_CAPACITY; initIndex += TILE_WORKGROUP_SIZE * TILE_WORKGROUP_SIZE) {
        atomicStore(&materialSet[initIndex], INVALID_MATERIAL_ID);
    }
    workgroupBarrier();

    let tileOrigin = vec2<u32>(workgroupId.xy) * tileUniforms.tileSize;
    for (var localY = localInvocationId.y; localY < tileUniforms.tileSize; localY += TILE_WORKGROUP_SIZE) {
        let pixelY = tileOrigin.y + localY;
        if (pixelY >= tileUniforms.viewportHeight) {
            continue;
        }

        for (var localX = localInvocationId.x; localX < tileUniforms.tileSize; localX += TILE_WORKGROUP_SIZE) {
            let pixelX = tileOrigin.x + localX;
            if (pixelX >= tileUniforms.viewportWidth) {
                continue;
            }

            let pixelCoords = vec2<i32>(i32(pixelX), i32(pixelY));
            let sceneDepth = textureLoad(sceneDepthTexture, pixelCoords, 0);
            if (sceneDepth <= 0.0) {
                continue;
            }

            let materialId = textureLoad(materialIdTexture, pixelCoords, 0).x;
            if (materialId >= tileUniforms.materialCount) {
                continue;
            }

            insertMaterial(materialId);
        }
    }

    workgroupBarrier();

    for (var flushIndex = localInvocationIndex; flushIndex < HASH_CAPACITY; flushIndex += TILE_WORKGROUP_SIZE * TILE_WORKGROUP_SIZE) {
        let materialId = atomicLoad(&materialSet[flushIndex]);
        if (materialId == INVALID_MATERIAL_ID) {
            continue;
        }

        let drawIndex = atomicAdd(&tileDrawCounters.drawCount, 1u);
        if (drawIndex >= tileUniforms.maxDrawEntries) {
            atomicAdd(&tileDrawCounters.overflowCount, 1u);
            continue;
        }

        tileDrawEntries[drawIndex] = TileMaterialDrawEntry(
            workgroupId.x,
            workgroupId.y,
            materialId,
            0u
        );
    }
}

/// @brief Compute shader that writes the indirect draw argument buffer for the material tile pass based on the total draw entry count.
@compute @workgroup_size(1, 1, 1)
fn prepareMaterialIndirectArgs() {
    let drawCount = atomicLoad(&tileDrawCounters.drawCount);
    materialDrawIndirect[0] = drawCount * 6u;
    materialDrawIndirect[1] = 1u;
    materialDrawIndirect[2] = 0u;
    materialDrawIndirect[3] = 0u;
}
