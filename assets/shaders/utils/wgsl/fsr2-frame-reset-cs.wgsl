#include "fsr2-temporal-common.wgsl"

// Resets the pass's two scratch buffers: the reconstructed previous-depth slots
// to "nothing projected here", and the exposure accumulators to zero.
//
// A separate dispatch rather than a clear folded into the dilate stage, and that
// is forced: the dilate stage *scatters* - each source pixel writes into the
// slots its motion vector points at, which belong to other pixels - so clearing
// a pixel's own slot in the same dispatch would race against whatever already
// scattered into it. The clear has to complete before any scatter begins.
@group(0) @binding(0) var<uniform> uniforms : Fsr2Uniforms;
@group(0) @binding(1) var<storage, read_write> reconstructedDepth : array<u32>;
@group(0) @binding(2) var<storage, read_write> exposure : array<i32>;

@compute @workgroup_size(64, 1, 1)
fn main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    // Folded in here rather than given a dispatch of its own: it is the same
    // "clear before anything accumulates" barrier, and one invocation can do it.
    if (globalInvocationId.x < EXPOSURE_SLOTS) {
        exposure[globalInvocationId.x] = 0;
    }
    let count = uniforms.sourceViewport.x * uniforms.sourceViewport.y;
    if (globalInvocationId.x >= count) {
        return;
    }
    // The sentinel is the far end of whichever convention is in use, so the
    // atomic in the dilate stage always moves a slot *towards* the camera and an
    // untouched slot is recognisable. Depth is non-negative in both conventions,
    // and IEEE floats with the same sign order identically as unsigned integers,
    // which is what makes an integer atomic usable on them at all.
    reconstructedDepth[globalInvocationId.x] =
        select(0xffffffffu, 0u, uniforms.reverseDepth != 0u);
}
