#ifndef MAX_MIP_LEVELS_PER_PASS
#define MAX_MIP_LEVELS_PER_PASS 8
#endif

const SPD_WORKGROUP_SIZE_X: u32 = 16u;
const SPD_WORKGROUP_SIZE_Y: u32 = 16u;
const SPD_TILE_SIZE: u32 = 64u;
const SPD_LOCAL_REDUCTION_LEVELS: u32 = 6u;

var<workgroup> spdLds: array<f32, 32u * 32u>;

/// @brief Computes the mip level dimensions by right-shifting source dimensions by the given reduction level.
/// @param srcSize The width and height of the source (top-level) texture.
/// @param reductionLevel The number of halvings from the source (0 = same size).
/// @returns The clamped mip dimensions, with a minimum of 1x1.
fn spdMipSizeFromSource(srcSize: vec2<u32>, reductionLevel: u32) -> vec2<u32> {
    let shift = min(reductionLevel, 31u);
    return max(vec2<u32>(srcSize.x >> shift, srcSize.y >> shift), vec2<u32>(1u, 1u));
}

/// @brief Clamps a texel coordinate so it stays within the valid [0, size-1] range.
/// @param coord The texel coordinate to clamp.
/// @param size The texture dimensions to clamp against.
/// @returns The clamped coordinate.
fn spdClampToSize(coord: vec2<u32>, size: vec2<u32>) -> vec2<u32> {
    return min(coord, size - vec2<u32>(1u, 1u));
}

/// @brief Converts a 2-D LDS tile coordinate into a flat array index (row-major, 32-wide).
/// @param x The horizontal coordinate within the LDS tile (0..31).
/// @param y The vertical coordinate within the LDS tile (0..31).
/// @returns The linear index into the spdLds workgroup-shared array.
fn spdLdsIndex(x: u32, y: u32) -> u32 {
    return y * 32u + x;
}

/// @brief Executes the single-pass downsampler (SPD) reduction for up to 6 mip levels within one workgroup.
/// @param localId The 3-D local invocation ID within the workgroup.
/// @param workgroupId The 3-D workgroup ID within the dispatch.
/// @param localIndex The flat local invocation index (0..255).
fn spdRun(
    localId: vec3<u32>,
    workgroupId: vec3<u32>,
    localIndex: u32
) {
    if (!spdIsActiveSlice(workgroupId)) {
        return;
    }

    let readSize = constants.readSize;
    let dispatchSize = constants.dispatchSize;
    let lid = localId.xy;
    let tgid = workgroupId.xy;
    let hasCopy = constants.copySourceToFirstMip != 0u;
    let firstReductionTarget = select(0u, 1u, hasCopy);
    let counterIndex = spdCounterIndex(workgroupId);

    var reductionsToGenerate = 0u;
    if (constants.levelsToWrite > firstReductionTarget) {
        reductionsToGenerate = constants.levelsToWrite - firstReductionTarget;
    }

    let ldsBase = lid * 2u;
    let baseCoord = tgid * SPD_TILE_SIZE + lid * 4u;

    let s00 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(0u, 0u), readSize), workgroupId);
    let s10 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(1u, 0u), readSize), workgroupId);
    let s20 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(2u, 0u), readSize), workgroupId);
    let s30 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(3u, 0u), readSize), workgroupId);

    let s01 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(0u, 1u), readSize), workgroupId);
    let s11 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(1u, 1u), readSize), workgroupId);
    let s21 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(2u, 1u), readSize), workgroupId);
    let s31 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(3u, 1u), readSize), workgroupId);

    let s02 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(0u, 2u), readSize), workgroupId);
    let s12 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(1u, 2u), readSize), workgroupId);
    let s22 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(2u, 2u), readSize), workgroupId);
    let s32 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(3u, 2u), readSize), workgroupId);

    let s03 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(0u, 3u), readSize), workgroupId);
    let s13 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(1u, 3u), readSize), workgroupId);
    let s23 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(2u, 3u), readSize), workgroupId);
    let s33 = spdLoadSource(spdClampToSize(baseCoord + vec2<u32>(3u, 3u), readSize), workgroupId);

    if (hasCopy && constants.levelsToWrite > 0u) {
        if (all(baseCoord + vec2<u32>(0u, 0u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(0u, 0u), workgroupId, s00); }
        if (all(baseCoord + vec2<u32>(1u, 0u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(1u, 0u), workgroupId, s10); }
        if (all(baseCoord + vec2<u32>(2u, 0u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(2u, 0u), workgroupId, s20); }
        if (all(baseCoord + vec2<u32>(3u, 0u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(3u, 0u), workgroupId, s30); }

        if (all(baseCoord + vec2<u32>(0u, 1u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(0u, 1u), workgroupId, s01); }
        if (all(baseCoord + vec2<u32>(1u, 1u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(1u, 1u), workgroupId, s11); }
        if (all(baseCoord + vec2<u32>(2u, 1u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(2u, 1u), workgroupId, s21); }
        if (all(baseCoord + vec2<u32>(3u, 1u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(3u, 1u), workgroupId, s31); }

        if (all(baseCoord + vec2<u32>(0u, 2u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(0u, 2u), workgroupId, s02); }
        if (all(baseCoord + vec2<u32>(1u, 2u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(1u, 2u), workgroupId, s12); }
        if (all(baseCoord + vec2<u32>(2u, 2u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(2u, 2u), workgroupId, s22); }
        if (all(baseCoord + vec2<u32>(3u, 2u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(3u, 2u), workgroupId, s32); }

        if (all(baseCoord + vec2<u32>(0u, 3u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(0u, 3u), workgroupId, s03); }
        if (all(baseCoord + vec2<u32>(1u, 3u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(1u, 3u), workgroupId, s13); }
        if (all(baseCoord + vec2<u32>(2u, 3u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(2u, 3u), workgroupId, s23); }
        if (all(baseCoord + vec2<u32>(3u, 3u) < dispatchSize)) { spdWriteMip(0u, baseCoord + vec2<u32>(3u, 3u), workgroupId, s33); }
    }

    let r0 = spdReduce2x2(s00, s10, s01, s11);
    let r1 = spdReduce2x2(s20, s30, s21, s31);
    let r2 = spdReduce2x2(s02, s12, s03, s13);
    let r3 = spdReduce2x2(s22, s32, s23, s33);

    spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 0u)] = r0;
    spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 0u)] = r1;
    spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 1u)] = r2;
    spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 1u)] = r3;

    if (reductionsToGenerate > 0u) {
        let mip1Base = tgid * 32u + ldsBase;
        let mip1Size = spdMipSizeFromSource(dispatchSize, 1u);

        if (all(mip1Base + vec2<u32>(0u, 0u) < mip1Size)) { spdWriteMip(firstReductionTarget + 0u, mip1Base + vec2<u32>(0u, 0u), workgroupId, r0); }
        if (all(mip1Base + vec2<u32>(1u, 0u) < mip1Size)) { spdWriteMip(firstReductionTarget + 0u, mip1Base + vec2<u32>(1u, 0u), workgroupId, r1); }
        if (all(mip1Base + vec2<u32>(0u, 1u) < mip1Size)) { spdWriteMip(firstReductionTarget + 0u, mip1Base + vec2<u32>(0u, 1u), workgroupId, r2); }
        if (all(mip1Base + vec2<u32>(1u, 1u) < mip1Size)) { spdWriteMip(firstReductionTarget + 0u, mip1Base + vec2<u32>(1u, 1u), workgroupId, r3); }
    }

    workgroupBarrier();

    var reduced = spdReduce2x2(
        spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 0u)],
        spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 0u)],
        spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 1u)],
        spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 1u)]
    );

    spdLds[spdLdsIndex(lid.x, lid.y)] = reduced;

    if (reductionsToGenerate > 1u) {
        let mip2Coord = tgid * 16u + lid;
        let mip2Size = spdMipSizeFromSource(dispatchSize, 2u);
        if (all(mip2Coord < mip2Size)) {
            spdWriteMip(firstReductionTarget + 1u, mip2Coord, workgroupId, reduced);
        }
    }

    workgroupBarrier();

    if (all(lid < vec2<u32>(8u, 8u))) {
        reduced = spdReduce2x2(
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 1u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 1u)]
        );

        spdLds[spdLdsIndex(lid.x, lid.y)] = reduced;

        if (reductionsToGenerate > 2u) {
            let mip3Coord = tgid * 8u + lid;
            let mip3Size = spdMipSizeFromSource(dispatchSize, 3u);
            if (all(mip3Coord < mip3Size)) {
                spdWriteMip(firstReductionTarget + 2u, mip3Coord, workgroupId, reduced);
            }
        }
    }

    workgroupBarrier();

    if (all(lid < vec2<u32>(4u, 4u))) {
        reduced = spdReduce2x2(
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 1u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 1u)]
        );

        spdLds[spdLdsIndex(lid.x, lid.y)] = reduced;

        if (reductionsToGenerate > 3u) {
            let mip4Coord = tgid * 4u + lid;
            let mip4Size = spdMipSizeFromSource(dispatchSize, 4u);
            if (all(mip4Coord < mip4Size)) {
                spdWriteMip(firstReductionTarget + 3u, mip4Coord, workgroupId, reduced);
            }
        }
    }

    workgroupBarrier();

    if (all(lid < vec2<u32>(2u, 2u))) {
        reduced = spdReduce2x2(
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 0u)],
            spdLds[spdLdsIndex(ldsBase.x + 0u, ldsBase.y + 1u)],
            spdLds[spdLdsIndex(ldsBase.x + 1u, ldsBase.y + 1u)]
        );

        spdLds[spdLdsIndex(lid.x, lid.y)] = reduced;

        if (reductionsToGenerate > 4u) {
            let mip5Coord = tgid * 2u + lid;
            let mip5Size = spdMipSizeFromSource(dispatchSize, 5u);
            if (all(mip5Coord < mip5Size)) {
                spdWriteMip(firstReductionTarget + 4u, mip5Coord, workgroupId, reduced);
            }
        }
    }

    workgroupBarrier();

    if (localIndex == 0u) {
        reduced = spdReduce2x2(
            spdLds[spdLdsIndex(0u, 0u)],
            spdLds[spdLdsIndex(1u, 0u)],
            spdLds[spdLdsIndex(0u, 1u)],
            spdLds[spdLdsIndex(1u, 1u)]
        );

        if (reductionsToGenerate > 5u) {
            let mip6Coord = tgid;
            let mip6Size = spdMipSizeFromSource(dispatchSize, 6u);
            if (all(mip6Coord < mip6Size)) {
                spdWriteMip(firstReductionTarget + 5u, mip6Coord, workgroupId, reduced);
            }
        }
    }

}

