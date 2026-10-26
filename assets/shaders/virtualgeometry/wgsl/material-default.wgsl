#include "material.wgsl"

@compute @workgroup_size(MATERIAL_WORKGROUP_SIZE, MATERIAL_WORKGROUP_SIZE, 1)
fn main(
    @builtin(workgroup_id) workgroupId : vec3<u32>,
    @builtin(local_invocation_id) localInvocationId : vec3<u32>
) {
    let context = material_make_context(workgroupId, localInvocationId);
    if (!context.valid) {
        return;
    }

    let materialOutput = material_evaluate_gbuffer(context.pixelCoords);
    material_store_gbuffer_outputs(context.pixelCoords, materialOutput);
    material_store_output(context.pixelCoords, material_evaluate_direct_only(materialOutput, context.pixelCoords));
}
