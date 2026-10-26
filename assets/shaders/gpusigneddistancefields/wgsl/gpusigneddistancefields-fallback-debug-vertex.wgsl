#include "gpusigneddistancefields_common.wgsl"

struct DrawUniforms {
    viewProjection : mat4x4<f32>,
    pathCapacity : u32,
    mode : u32,
    _padding1 : u32,
    _padding2 : u32,
}

struct VSOutput {
    @builtin(position) position : vec4<f32>,
    @location(0) @interpolate(flat) triangleId : u32,
    @location(1) normal : vec3<f32>,
    @location(2) baseColorOpacity : vec4<f32>,
    @location(3) roughnessMetallic : vec2<f32>,
    @location(4) emissive : vec3<f32>,
    @location(5) @interpolate(flat) surfaceFlags : vec4<u32>,
    @location(6) @interpolate(flat) mode : u32,
}

@group(0) @binding(0) var<uniform> gUniforms : DrawUniforms;
@group(0) @binding(1) var<storage, read> meshTable : array<MeshTableEntryGPU>;
@group(0) @binding(2) var<storage, read> vertices : array<VertexGPU>;
@group(0) @binding(3) var<storage, read> indices : array<u32>;
@group(0) @binding(4) var<storage, read> boneWeights : array<BoneWeightGPU>;
@group(0) @binding(5) var<storage, read> boneMatrices : array<mat4x4<f32>>;
@group(0) @binding(6) var<storage, read> staticInstances : array<InstanceGPU>;
@group(0) @binding(7) var<storage, read> skinnedInstances : array<InstanceGPU>;

fn fetch_instance(instanceIndex: u32) -> InstanceGPU {
    if (instanceIndex < gUniforms.pathCapacity) {
        return staticInstances[instanceIndex];
    }
    return skinnedInstances[instanceIndex - gUniforms.pathCapacity];
}

fn fetch_local_position(vertexIndex: u32, instance: InstanceGPU, mesh: MeshTableEntryGPU) -> vec3<f32> {
    let vertex = vertices[vertexIndex];
    var localPosition = vec4<f32>(vertex.position, 1.0);

    if ((instance.flags & 1u) != 0u && mesh.boneWeightsPerVertex != 0u && instance.boneCount != 0u) {
        var skinnedPosition = vec4<f32>(0.0);
        var totalWeight = 0.0;
        let influenceCount = min(mesh.boneWeightsPerVertex, 4u);
        for (var influenceIndex = 0u; influenceIndex < 4u; influenceIndex++) {
            if (influenceIndex >= influenceCount) {
                break;
            }

            let boneWeight = boneWeights[vertex.boneWeightOffset + influenceIndex];
            if (boneWeight.weight <= 0.0 || boneWeight.boneIndex >= instance.boneCount) {
                continue;
            }

            skinnedPosition += boneMatrices[instance.boneMatrixOffset + boneWeight.boneIndex] * localPosition * boneWeight.weight;
            totalWeight += boneWeight.weight;
        }

        if (totalWeight > 1.0e-5) {
            localPosition = skinnedPosition;
        }
    }

    return localPosition.xyz;
}

fn fetch_local_normal(vertexIndex: u32, instance: InstanceGPU, mesh: MeshTableEntryGPU) -> vec3<f32> {
    let vertex = vertices[vertexIndex];
    var localNormal = vec4<f32>(vertex.surfaceNormalRoughness.xyz, 0.0);

    if ((instance.flags & 1u) != 0u && mesh.boneWeightsPerVertex != 0u && instance.boneCount != 0u) {
        var skinnedNormal = vec4<f32>(0.0);
        var totalWeight = 0.0;
        let influenceCount = min(mesh.boneWeightsPerVertex, 4u);
        for (var influenceIndex = 0u; influenceIndex < 4u; influenceIndex++) {
            if (influenceIndex >= influenceCount) {
                break;
            }

            let boneWeight = boneWeights[vertex.boneWeightOffset + influenceIndex];
            if (boneWeight.weight <= 0.0 || boneWeight.boneIndex >= instance.boneCount) {
                continue;
            }

            skinnedNormal += boneMatrices[instance.boneMatrixOffset + boneWeight.boneIndex] * localNormal * boneWeight.weight;
            totalWeight += boneWeight.weight;
        }

        if (totalWeight > 1.0e-5) {
            localNormal = skinnedNormal;
        }
    }

    return localNormal.xyz;
}

@vertex
fn vs_main(@builtin(vertex_index) vertexIndex : u32, @builtin(instance_index) instanceIndex : u32) -> VSOutput {
    var output : VSOutput;
    let instance = fetch_instance(instanceIndex);
    let mesh = meshTable[instance.meshIndex];
    let triangleIndex = vertexIndex / 3u;
    let cornerIndex = vertexIndex % 3u;
    let meshVertexIndex = indices[mesh.indexOffset + triangleIndex * 3u + cornerIndex];
    let absoluteVertexIndex = mesh.vertexOffset + meshVertexIndex;
    let vertex = vertices[absoluteVertexIndex];
    let localPosition = fetch_local_position(absoluteVertexIndex, instance, mesh);
    let localNormal = fetch_local_normal(absoluteVertexIndex, instance, mesh);
    let worldPosition = instance.modelMatrix * vec4<f32>(localPosition, 1.0);
    let worldNormal = instance.modelMatrix * vec4<f32>(localNormal, 0.0);
    output.position = gUniforms.viewProjection * worldPosition;
    output.position.y = -output.position.y;
    output.triangleId = mesh.indexOffset / 3u + triangleIndex;
    output.normal = normalize(worldNormal.xyz);
    output.baseColorOpacity = vertex.surfaceBaseColorOpacity;
    output.roughnessMetallic = vec2<f32>(vertex.surfaceNormalRoughness.w, vertex.surfaceEmissiveMetallic.w);
    output.emissive = vertex.surfaceEmissiveMetallic.xyz;
    output.surfaceFlags = vertex.surfaceFlags;
    output.mode = gUniforms.mode;
    return output;
}
