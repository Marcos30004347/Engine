#pragma once

#include <algorithm>

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "softwarebvh/SoftwareBLASManager.hpp"

namespace softwarebvh
{
namespace rendering
{

class SoftwareBLASGPUFinalizePass : public ::rendering::Pass
{
public:
  SoftwareBLASGPUFinalizePass(softwarebvh::SoftwareBLASManager &manager, uint32_t blasHandle) : manager_(manager), blasHandle_(blasHandle)
  {
  }

  ~SoftwareBLASGPUFinalizePass() override
  {
    renderGraph->deleteComputePipeline(setFlagsPipeline_);
    renderGraph->deleteComputePipeline(buildHierarchyPipeline_);
    renderGraph->deleteComputePipeline(refitAabbsPipeline_);
    renderGraph->deleteComputePipeline(initializeCWBVHPipeline_);
    renderGraph->deleteComputePipeline(buildCWBVHPipeline_);
    renderGraph->deleteShader(cwbvhShader_);
    renderGraph->deleteBindingGroups(bindingGroups_);
    renderGraph->deleteBindingsLayout(layout_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    softwarebvh::SoftwareBLASManager::GPUBuildDescriptor descriptor{};
    if (!manager_.tryGetGPUBuildDescriptor(blasHandle_, &descriptor) || descriptor.primitiveCount == 0u)
    {
      return;
    }

    auto uniformsBuffer = createFrameLocalBuffer(
        ::rendering::BufferInfo{
            .name = passName + "_Uniforms.buffer",
            .size = sizeof(softwarebvh::SoftwareBLASManager::GPUBuildUniforms),
            .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
        });

    softwarebvh::SoftwareBLASManager::GPUBuildUniforms uniforms{};
    uniforms.num = descriptor.primitiveCount;
    uniforms.targetPrimitivesPerNode = 1u;
    uniforms.targetNumberOfLeaves = descriptor.primitiveCount;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      uniforms.volumeAabb.minPoint[axis] = descriptor.localBounds.minPoint[axis];
      uniforms.volumeAabb.maxPoint[axis] = descriptor.localBounds.maxPoint[axis];
    }
    renderGraph->bufferWrite(uniformsBuffer, 0u, sizeof(uniforms), &uniforms);

    const uint64_t aabbSize = static_cast<uint64_t>(descriptor.primitiveCount) * sizeof(softwarebvh::SoftwareBLASManager::GPUBuildAABBGPU);
    const uint64_t valueSize = static_cast<uint64_t>(descriptor.primitiveCount) * sizeof(uint32_t);
    const uint64_t flagsSize = static_cast<uint64_t>(std::max(1u, descriptor.primitiveCount * 2u)) * sizeof(uint32_t);
    const uint64_t hlbvhOffset = static_cast<uint64_t>(descriptor.gpuHLBVHNodeOffset) * sizeof(softwarebvh::SoftwareBLASManager::CanonicalNodeGPU);
    const uint64_t hlbvhSize = static_cast<uint64_t>(descriptor.gpuHLBVHNodeCount) * sizeof(softwarebvh::SoftwareBLASManager::CanonicalNodeGPU);
    const uint64_t cwbvhOffset = static_cast<uint64_t>(descriptor.gpuCWBVHNodeOffset) * sizeof(softwarebvh::SoftwareBLASManager::CWBVHNodeGPU);
    const uint64_t cwbvhSize = static_cast<uint64_t>(std::max(1u, descriptor.gpuCWBVHNodeCount)) * sizeof(softwarebvh::SoftwareBLASManager::CWBVHNodeGPU);
    const uint64_t leafRemapOffset = static_cast<uint64_t>(descriptor.gpuLeafRemapOffset) * sizeof(uint32_t);
    const uint64_t leafRemapSize = static_cast<uint64_t>(descriptor.primitiveCount) * sizeof(uint32_t);
    const uint64_t debugSize = static_cast<uint64_t>(std::max(1u, descriptor.primitiveCount * 8u)) * sizeof(uint32_t);

    layout_ = renderGraph->createBindingsLayout(
        ::rendering::BindingsLayoutInfo{
            .name = passName + "_Layout",
            .groups = {::rendering::BindingGroupLayout{
                .buffers = {
                    {.name = "uniforms", .binding = 0, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "aabbs", .binding = 1, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "inputLeafIndexRemap", .binding = 2, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "sortedMortonCodes", .binding = 3, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "flags", .binding = 4, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "signals", .binding = 5, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "hlbvhNodes", .binding = 6, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "cwbvhNodes", .binding = 7, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "outputLeafIndicesRemap", .binding = 8, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                    {.name = "debug", .binding = 9, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                },
            }},
        });

    bindingGroups_ = renderGraph->createBindingGroups(
        ::rendering::BindingGroupsInfo{
            .layout = layout_,
            .name = passName + "_Bindings",
            .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers = {
                    {.binding = 0, .bufferView = {.buffer = uniformsBuffer, .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = sizeof(softwarebvh::SoftwareBLASManager::GPUBuildUniforms)}},
                    {.binding = 1, .bufferView = {.buffer = manager_.getGPUBuildAabbsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = aabbSize}},
                    {.binding = 2, .bufferView = {.buffer = manager_.getGPUBuildPrimitiveIndicesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = valueSize}},
                    {.binding = 3, .bufferView = {.buffer = manager_.getGPUBuildMortonCodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ, .offset = 0u, .size = valueSize}},
                    {.binding = 4, .bufferView = {.buffer = manager_.getGPUBuildFlagsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = flagsSize}},
                    {.binding = 5, .bufferView = {.buffer = manager_.getGPUBuildSignalsBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = 64u * sizeof(uint32_t)}},
                    {.binding = 6, .bufferView = {.buffer = manager_.getGPUBuildHLBVHNodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = hlbvhOffset, .size = hlbvhSize}},
                    {.binding = 7, .bufferView = {.buffer = manager_.getGPUCWBVHNodesBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = cwbvhOffset, .size = cwbvhSize}},
                    {.binding = 8, .bufferView = {.buffer = manager_.getGPULeafRemapBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = leafRemapOffset, .size = leafRemapSize}},
                    {.binding = 9, .bufferView = {.buffer = manager_.getGPUBuildDebugBuffer(), .access = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE, .offset = 0u, .size = debugSize}},
                },
            }},
        });

    cwbvhShader_ = renderGraph->createShader(
        ::rendering::ShaderInfo{
            .name = passName + "_Shader",
            .layout = layout_,
            .src = os::io::readRelativeFile("assets/shaders/spirv/cwbvh-build.spirv"),
            .type = ::rendering::ShaderType::SpirV,
        });

    setFlagsPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "set_flags_to_zero",
            .layout = layout_,
            .name = passName + "_SetFlags.pipeline",
            .shader = cwbvhShader_,
        });
    buildHierarchyPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "hlbvh_build_hierarchy",
            .layout = layout_,
            .name = passName + "_BuildHierarchy.pipeline",
            .shader = cwbvhShader_,
        });
    refitAabbsPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "hlbvh_refit_aabbs",
            .layout = layout_,
            .name = passName + "_RefitAABBs.pipeline",
            .shader = cwbvhShader_,
        });
    initializeCWBVHPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "cwbvh_initialize",
            .layout = layout_,
            .name = passName + "_InitializeCWBVH.pipeline",
            .shader = cwbvhShader_,
        });
    buildCWBVHPipeline_ = renderGraph->createComputePipeline(
        ::rendering::ComputePipelineInfo{
            .entry = "cwbvh_build",
            .layout = layout_,
            .name = passName + "_BuildCWBVH.pipeline",
            .shader = cwbvhShader_,
        });

    const uint32_t dispatchCount = std::max(1u, (descriptor.primitiveCount + 63u) / 64u);

    commandBuffer.cmdBindComputePipeline(setFlagsPipeline_);
    commandBuffer.cmdBindBindingGroups(bindingGroups_, nullptr, 0u);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(buildHierarchyPipeline_);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(refitAabbsPipeline_);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(initializeCWBVHPipeline_);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);

    commandBuffer.cmdBindComputePipeline(buildCWBVHPipeline_);
    commandBuffer.cmdDispatch(dispatchCount, 1u, 1u);
  }

  void appendFrameOverrides(::rendering::RenderGraph::Overrides &overrides, uint32_t frameSlot) const
  {
    manager_.appendFrameOverrides(overrides, frameSlot);
  }

private:
  softwarebvh::SoftwareBLASManager &manager_;
  uint32_t blasHandle_ = softwarebvh::SoftwareBLASManager::INVALID_BLAS_HANDLE;

  ::rendering::BindingsLayout layout_;
  ::rendering::BindingGroups bindingGroups_;
  ::rendering::Shader cwbvhShader_;
  ::rendering::ComputePipeline setFlagsPipeline_;
  ::rendering::ComputePipeline buildHierarchyPipeline_;
  ::rendering::ComputePipeline refitAabbsPipeline_;
  ::rendering::ComputePipeline initializeCWBVHPipeline_;
  ::rendering::ComputePipeline buildCWBVHPipeline_;
};

} // namespace rendering
} // namespace softwarebvh
