#include "vsm-screen-space-shadow-common.wgsl"

@compute @workgroup_size(64, 1, 1)
fn main(
    @builtin(workgroup_id) workgroup_id: vec3<u32>,
    @builtin(local_invocation_id) local_id: vec3<u32>
) {
    write_screen_space_shadow(workgroup_id, local_id.x);
}
