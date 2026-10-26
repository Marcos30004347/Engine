// Shared compact heightfield column cache (plan stage R3).
//
// A terrain staircase-surface parent quantizes its four-corner patch to whole
// microvoxels, so every column depth is an integer in 0..subdivision and four
// bits store it exactly. For the default subdivision of 5 that is 25 columns,
// 100 bits, which fits one 16-byte record beside the existing micro-parent
// record with 28 bits to spare for a future octahedral macro normal.
//
// MicroPrepare generates the cache once per extracted parent and the raster
// fragment reads it instead of re-deriving the same interpolation, quantization
// and staircase classification per visited column. Both sides call
// sparseVoxelExactMicroStaircaseSurfaceDepth() below rather than reimplementing
// it, so the cached value is bit-identical to the live one by construction and
// not by review.
//
// This header deliberately depends on nothing but the packed surface patch:
// MicroPrepare must not acquire the geometry-IR bindings that the full
// micro-geometry implementation needs.

// Bit 31 of the fourth word. Column 24, the last one a subdivision of 5 can
// address, occupies bits 96..99 of that word, so the flag never collides.
const SPARSE_VOXEL_MICRO_COLUMN_CACHE_VALID : u32 = 1u << 31u;
// Four bits per column, so a field never straddles a 32-bit word boundary.
const SPARSE_VOXEL_MICRO_COLUMN_CACHE_CAPACITY : u32 = 25u;

fn sparseVoxelExactMicroSurfaceCorner(
    surfaceData : u32, corner : u32) -> f32 {
    return f32((surfaceData >> (corner * 6u)) & 0x3fu) / 63.0;
}

// The quantized retained depth of one surface column, in whole microvoxels.
// This is the body of the face-3 staircase-surface branch of
// sparseVoxelExactMicroCarveDepth(); that function returns f32 of this value.
fn sparseVoxelExactMicroStaircaseSurfaceDepth(
    subdivision : u32, surfaceData : u32, faceCell : vec2<u32>) -> u32 {
    // Four shared corner heights form a continuous bilinear coverage patch.
    // Evaluate at true microcell centres, then quantize once; parent-local
    // direction bins and endpoint sampling both created angle-dependent
    // brick/hole bands.
    let localX = (f32(faceCell.x) + 0.5) / f32(subdivision);
    let localZ =
        (f32(subdivision - 1u - faceCell.y) + 0.5) / f32(subdivision);
    let negativeZ = mix(
        sparseVoxelExactMicroSurfaceCorner(surfaceData, 0u),
        sparseVoxelExactMicroSurfaceCorner(surfaceData, 1u), localX);
    let positiveZ = mix(
        sparseVoxelExactMicroSurfaceCorner(surfaceData, 2u),
        sparseVoxelExactMicroSurfaceCorner(surfaceData, 3u), localX);
    let retainedHeight = mix(negativeZ, positiveZ, localZ);
    // Quantize the reconstructed plane to complete microvoxels. Adjacent
    // columns therefore rise or fall by one cubic cell; the shared parent-corner
    // phase keeps that digital curve continuous across brick boundaries and at
    // every angle.
    let continuousDepth = (1.0 - retainedHeight) * f32(subdivision);
    let flooredCoordinate = u32(clamp(
        floor(continuousDepth + 1.0e-5), 0.0, f32(subdivision)));
    return select(
        flooredCoordinate, subdivision,
        continuousDepth >= f32(subdivision) - 0.5);
}

fn sparseVoxelExactMicroColumnCacheIndex(
    subdivision : u32, faceCell : vec2<u32>) -> u32 {
    return faceCell.y * subdivision + faceCell.x;
}

fn sparseVoxelExactMicroColumnCacheValid(cache : vec4<u32>) -> bool {
    return (cache.w & SPARSE_VOXEL_MICRO_COLUMN_CACHE_VALID) != 0u;
}

// A subdivision of 5 or less keeps every column inside the 100 usable bits.
// Anything coarser stays on the live evaluation rather than growing the record.
fn sparseVoxelExactMicroColumnCacheFits(subdivision : u32) -> bool {
    return subdivision >= 2u &&
        subdivision * subdivision <= SPARSE_VOXEL_MICRO_COLUMN_CACHE_CAPACITY;
}

fn sparseVoxelExactMicroColumnCacheDepth(
    cache : vec4<u32>, index : u32) -> u32 {
    let shift = (index * 4u) & 31u;
    let word = (index * 4u) >> 5u;
    var packed = cache.w;
    if (word == 0u) {
        packed = cache.x;
    } else if (word == 1u) {
        packed = cache.y;
    } else if (word == 2u) {
        packed = cache.z;
    }
    return (packed >> shift) & 0xfu;
}

fn sparseVoxelExactMicroColumnCacheStore(
    cache : vec4<u32>, index : u32, depth : u32) -> vec4<u32> {
    let shift = (index * 4u) & 31u;
    let word = (index * 4u) >> 5u;
    let cleared = ~(0xfu << shift);
    let value = (depth & 0xfu) << shift;
    var result = cache;
    if (word == 0u) {
        result.x = (result.x & cleared) | value;
    } else if (word == 1u) {
        result.y = (result.y & cleared) | value;
    } else if (word == 2u) {
        result.z = (result.z & cleared) | value;
    } else {
        result.w = (result.w & cleared) | value;
    }
    return result;
}
