#pragma once

#include <algorithm>
#include <string>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"

namespace softwarebvh
{
namespace rendering
{

class SoftwareBLASGPUPreparePass : public ::rendering::Pass
{
public:
  SoftwareBLASGPUPreparePass(softwarebvh::SoftwareBLASManager &manager, uint32_t blasHandle) : manager_(manager), blasHandle_(blasHandle)
  {
  }

  ~SoftwareBLASGPUPreparePass() override
  {
    renderGraph->deleteComputePipeline(buildTriangleAabbsPipeline_);
    renderGraph->deleteComputePipeline(buildMortonCodesPipeline_);
    renderGraph->deleteShader(triangleAabbsShader_);
    renderGraph->deleteShader(mortonShader_);
    renderGraph->deleteBindingGroups(triangleAabbsBindingGroups_);
    renderGraph->deleteBindingGroups(mortonBindingGroups_);
    renderGraph->deleteBindingsLayout(triangleAabbsLayout_);
    renderGraph->deleteBindingsLayout(mortonLayout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    softwarebvh::SoftwareBLASManager::GPUBuildDescriptor descriptor{};
    if (!manager_.tryGetGPUBuildDescriptor(blasHandle_, &descriptor) || descriptor.primitiveCount == 0u)
    {
      return;
    }

    const uint64_t indicesOffset = static_cast<uint64_t>(descriptor.indexOffset) * sizeof(uint32_t);
    const uint64_t indicesSize = static_cast<uint64_t>(descriptor.indexCount) * sizeof(uint32_t);
    const uint64_t positionsOffset = static_cast<uint64_t>(descriptor.vertexOffset) * sizeof(softwarebvh::SoftwareBLASManager::VertexPositionGPU);
    const uint64_t positionsSize = static_cast<uint64_t>(descriptor.vertexCount) * sizeof(softwarebvh::SoftwareBLASManager::VertexPositionGPU);
    const uint64_t aabbSize = static_cast<uint64_t>(descriptor.primitiveCount) * sizeof(softwarebvh::SoftwareBLASManager::GPUBuildAABBGPU);
    const uint64_t mortonSize = static_cast<uint64_t>(descriptor.primitiveCount) * sizeof(uint32_t);

    auto triangleUniformsBuffer = createFrameLocalBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_TriangleAABBUniforms.buffer",
            .size = sizeof(softwarebvh::SoftwareBLASManager::TriangleAABBUniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        });

    const softwarebvh::SoftwareBLASManager::TriangleAABBUniforms triangleUniforms{
        .primitiveCount = descriptor.primitiveCount,
    };
    renderGraph->bufferWrite(triangleUniformsBuffer, 0u, sizeof(triangleUniforms), const_cast<softwarebvh::SoftwareBLASManager::TriangleAABBUniforms *>(&triangleUniforms));

    triangleAabbsLayout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
            .name = passName + "_TriangleAABBs.layout",
            .groups = {::rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "indices", .binding = 1, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "vertices", .binding = 2, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "aabbs", .binding = 3, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    triangleAabbsBindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = triangleAabbsLayout_,
            .name = passName + "_TriangleAABBs.bindings",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = {.buffer = triangleUniformsBuffer, .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = sizeof(softwarebvh::SoftwareBLASManager::TriangleAABBUniforms)}},
                    {.binding = 1, .bufferView = {.buffer = manager_.getIndicesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = indicesOffset, .size = indicesSize}},
                    {.binding = 2, .bufferView = {.buffer = manager_.getVertexPositionsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = positionsOffset, .size = positionsSize}},
                    {.binding = 3, .bufferView = {.buffer = manager_.getGPUBuildAabbsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = aabbSize}},
                },
            }},
        });

    triangleAabbsShader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
            .name = passName + "_TriangleAABBs.shader",
            .layout = triangleAabbsLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/cwbvh-triangle-aabbs.spirv"),
            .type = ::rendering::ShaderType::SpirV,
        });

    buildTriangleAabbsPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "build_triangle_aabbs",
            .layout = triangleAabbsLayout_,
            .name = passName + "_TriangleAABBs.pipeline",
            .shader = triangleAabbsShader_,
        });

    auto mortonUniformsBuffer = createFrameLocalBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_MortonUniforms.buffer",
            .size = sizeof(softwarebvh::SoftwareBLASManager::GPUBuildUniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        });

    softwarebvh::SoftwareBLASManager::GPUBuildUniforms mortonUniforms{};
    mortonUniforms.num = descriptor.primitiveCount;
    mortonUniforms.targetPrimitivesPerNode = 1u;
    mortonUniforms.targetNumberOfLeaves = descriptor.primitiveCount;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      mortonUniforms.volumeAabb.minPoint[axis] = descriptor.localBounds.minPoint[axis];
      mortonUniforms.volumeAabb.maxPoint[axis] = descriptor.localBounds.maxPoint[axis];
    }
    renderGraph->bufferWrite(mortonUniformsBuffer, 0u, sizeof(mortonUniforms), &mortonUniforms);

    mortonLayout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
            .name = passName + "_Morton.layout",
            .groups = {::rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "aabbs", .binding = 1, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "mortonCodes", .binding = 2, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "indices", .binding = 3, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    mortonBindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = mortonLayout_,
            .name = passName + "_Morton.bindings",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = {.buffer = mortonUniformsBuffer, .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = sizeof(softwarebvh::SoftwareBLASManager::GPUBuildUniforms)}},
                    {.binding = 1, .bufferView = {.buffer = manager_.getGPUBuildAabbsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = aabbSize}},
                    {.binding = 2, .bufferView = {.buffer = manager_.getGPUBuildMortonCodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = mortonSize}},
                    {.binding = 3, .bufferView = {.buffer = manager_.getGPUBuildPrimitiveIndicesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = mortonSize}},
                },
            }},
        });

    mortonShader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
            .name = passName + "_Morton.shader",
            .layout = mortonLayout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/cwbvh-morton.spirv"),
            .type = ::rendering::ShaderType::SpirV,
        });

    buildMortonCodesPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "compute_morton_codes",
            .layout = mortonLayout_,
            .name = passName + "_Morton.pipeline",
            .shader = mortonShader_,
        });

    const uint32_t dispatchCount = std::max(1u, (descriptor.primitiveCount + 63u) / 64u);
    commandBuffer.cmdBindComputePipeline(buildTriangleAabbsPipeline_);
    commandBuffer.cmdBindBindingGroups(triangleAabbsBindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(buildMortonCodesPipeline_);
    commandBuffer.cmdBindBindingGroups(mortonBindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);
  }

  void appendFrameOverrides(::rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    manager_.appendFrameOverrides(overrides, frameSlot);
  }

private:
  softwarebvh::SoftwareBLASManager &manager_;
  uint32_t blasHandle_ = softwarebvh::SoftwareBLASManager::INVALID_BLAS_HANDLE;

  ::rendering::BindingsLayout triangleAabbsLayout_;
  ::rendering::BindingGroups triangleAabbsBindingGroups_;
  ::rendering::Shader triangleAabbsShader_;
  ::rendering::ComputePipeline buildTriangleAabbsPipeline_;

  ::rendering::BindingsLayout mortonLayout_;
  ::rendering::BindingGroups mortonBindingGroups_;
  ::rendering::Shader mortonShader_;
  ::rendering::ComputePipeline buildMortonCodesPipeline_;
};

} // namespace rendering
} // namespace softwarebvh
