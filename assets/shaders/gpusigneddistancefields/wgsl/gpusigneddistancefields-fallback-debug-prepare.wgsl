#include "gpusigneddistancefields_common.wgsl"

struct PrepareUniforms {
    frustumPlanes : array<vec4<f32>, 6>,
    staticInstanceCount : u32,
    skinnedInstanceCount : u32,
    meshCount : u32,
    pathCapacity : u32,
    commandCapacity : u32,
    enableFrustumCulling : u32,
    enabled : u32,
    _padding0 : u32,
}

struct DrawIndirectCommandGPU {
    vertexCount : u32,
    instanceCount : u32,
    firstVertex : u32,
    firstInstance : u32,
}

@group(0) @binding(0) var<uniform> gUniforms : PrepareUniforms;
@group(0) @binding(1) var<storage, read> meshTable : array<MeshTableEntryGPU>;
@group(0) @binding(2) var<storage, read> staticInstances : array<InstanceGPU>;
@group(0) @binding(3) var<storage, read> skinnedInstances : array<InstanceGPU>;
@group(0) @binding(4) var<storage, read_write> drawCommands : array<DrawIndirectCommandGPU>;

fn zero_command() -> DrawIndirectCommandGPU {
    return DrawIndirectCommandGPU(0u, 0u, 0u, 0u);
}

fn is_visible(instance: InstanceGPU) -> bool {
    for (var planeIndex = 0u; planeIndex < 6u; planeIndex++) {
        let plane = gUniforms.frustumPlanes[planeIndex];
        var positiveVertex = instance.aabbMin;
        if (plane.x >= 0.0) { positiveVertex.x = instance.aabbMax.x; }
        if (plane.y >= 0.0) { positiveVertex.y = instance.aabbMax.y; }
        if (plane.z >= 0.0) { positiveVertex.z = instance.aabbMax.z; }
        if (dot(plane.xyz, positiveVertex) + plane.w < 0.0) {
            return false;
        }
    }
    return true;
}

fn make_command(instance: InstanceGPU, firstInstance: u32) -> DrawIndirectCommandGPU {
    var command = zero_command();
    if (instance.meshIndex >= gUniforms.meshCount) {
        return command;
    }

    if (gUniforms.enableFrustumCulling != 0u && !is_visible(instance)) {
        return command;
    }

    let mesh = meshTable[instance.meshIndex];
    command.vertexCount = mesh.triangleCount * 3u;
    command.instanceCount = select(0u, 1u, command.vertexCount > 0u);
    command.firstVertex = 0u;
    command.firstInstance = firstInstance;
    return command;
}

@compute @workgroup_size(64, 1, 1)
fn cs_main(@builtin(global_invocation_id) globalInvocationId : vec3<u32>) {
    let commandIndex = globalInvocationId.x;
    if (commandIndex >= gUniforms.commandCapacity) {
        return;
    }

    var command = zero_command();
    if (gUniforms.enabled != 0u) {
        if (commandIndex < gUniforms.pathCapacity) {
            if (commandIndex < gUniforms.staticInstanceCount) {
                command = make_command(staticInstances[commandIndex], commandIndex);
            }
        } else {
            let skinnedInstanceIndex = commandIndex - gUniforms.pathCapacity;
            if (skinnedInstanceIndex < gUniforms.skinnedInstanceCount) {
                command = make_command(skinnedInstances[skinnedInstanceIndex], commandIndex);
            }
        }
    }

    drawCommands[commandIndex] = command;
}
