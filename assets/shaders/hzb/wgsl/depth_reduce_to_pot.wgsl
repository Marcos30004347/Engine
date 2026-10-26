#ifndef USE_REVERSE_Z
#define USE_REVERSE_Z
#endif

const WORKGROUP_SIZE_X: u32 = 8u;
const WORKGROUP_SIZE_Y: u32 = 8u;

struct ReductionConstants {
    srcSize: vec2<u32>,
    dstSize: vec2<u32>,
}

@group(0) @binding(0) var<uniform> constants: ReductionConstants;
@group(0) @binding(1) var srcDepth: texture_depth_2d;
@group(0) @binding(2) var dstMip0: texture_storage_2d<r32float, write>;

/// @brief Returns the conservative depth between two samples (min for reverse-Z, max for forward-Z).
/// @param a First depth sample.
/// @param b Second depth sample.
/// @returns The conservatively closer depth value.
fn reduceDepth(a: f32, b: f32) -> f32 {
#ifdef USE_REVERSE_Z
    return min(a, b);
#else
    return max(a, b);
#endif
}

/// @brief Compute entry point that downsamples a non-power-of-two depth texture into a power-of-two mip level by conservative reduction.
@compute @workgroup_size(WORKGROUP_SIZE_X, WORKGROUP_SIZE_Y, 1)
fn depth_reduce_to_pot(@builtin(global_invocation_id) id: vec3<u32>) {
    let dstCoord = id.xy;
    if (any(dstCoord >= constants.dstSize)) {
        return;
    }

    let srcSizeF = vec2<f32>(constants.srcSize);
    let dstSizeF = vec2<f32>(constants.dstSize);

    let srcMin = vec2<u32>(floor(vec2<f32>(dstCoord) * srcSizeF / dstSizeF));
    let srcMaxExclusive = min(
        vec2<u32>(ceil(vec2<f32>(dstCoord + vec2<u32>(1u)) * srcSizeF / dstSizeF)),
        constants.srcSize
    );

    var reduced = textureLoad(srcDepth, vec2<i32>(srcMin), 0);

    var y = srcMin.y;
    while (y < srcMaxExclusive.y) {
        var x = srcMin.x;
        while (x < srcMaxExclusive.x) {
            reduced = reduceDepth(reduced, textureLoad(srcDepth, vec2<i32>(vec2<u32>(x, y)), 0));
            x = x + 1u;
        }
        y = y + 1u;
    }

    textureStore(dstMip0, vec2<i32>(dstCoord), vec4<f32>(reduced, 0.0, 0.0, 1.0));
}
