#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelStreamingPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    ClearPageRequests,
    ApplyPhysicalPageRewrites,
  };

  struct Buffers
  {
    ::rendering::Buffer pageRequestBits;
    ::rendering::Buffer pageRequestPriorities;
    ::rendering::Buffer pageRequestInstances;
    ::rendering::Buffer rewrites;
    ::rendering::Buffer nodes;
  };

  SparseVoxelStreamingPass(const Kernel kernel, const Buffers buffers, const SparseVoxelStreamingPassUniformsGPU settings)
      : kernel_(kernel), buffers_(buffers), settings_(settings)
  {
  }

  ~SparseVoxelStreamingPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    const uint32_t count = kernel_ == Kernel::ClearPageRequests ? std::max(settings_.pageCount, settings_.requestWordCount) : settings_.rewriteCount;
    commandBuffer.cmdDispatch(std::max(1u, (count + 63u) / 64u), 1u, 1u);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {.buffer = buffer, .offset = 0u, .size = std::max<uint64_t>(size, sizeof(uint32_t)), .access = access};
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer(
        {.name = passName + "_Uniforms", .size = sizeof(settings_), .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push});
    std::vector<::rendering::BindingGroupLayoutBufferEntry> entries = {
      {.name = "streaming", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
    };
    for (uint32_t binding = 1u; binding <= 5u; ++binding)
      entries.push_back({.name = "storage" + std::to_string(binding), .binding = binding, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute});
    layout_ = renderGraph->createBindingsLayout({.name = passName + "_Layout", .groups = {::rendering::BindingGroupLayout{.buffers = entries}}});
    const auto rw = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups(
        {.layout = layout_,
         .name = passName + "_Groups",
         .groups = {::rendering::GroupInfo{
           .name = "group0",
           .buffers = {
             {.binding = 0u, .bufferView = view(uniforms_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
             {.binding = 1u, .bufferView = view(buffers_.pageRequestBits, uint64_t{settings_.requestWordCount} * sizeof(uint32_t), rw)},
             {.binding = 2u, .bufferView = view(buffers_.pageRequestPriorities, uint64_t{settings_.pageCount} * sizeof(uint32_t), rw)},
             {.binding = 3u, .bufferView = view(buffers_.pageRequestInstances, uint64_t{settings_.pageCount} * sizeof(uint32_t), rw)},
             {.binding = 4u, .bufferView = view(buffers_.rewrites, uint64_t{settings_.rewriteCount} * sizeof(SparseVoxelPhysicalPageRewriteGPU), ::rendering::AccessPattern::SHADER_READ)},
             {.binding = 5u, .bufferView = view(buffers_.nodes, uint64_t{settings_.physicalNodeCapacity} * sizeof(SparseVoxelLODNodeGPU), rw)},
           }}}});
    shader_ = renderGraph->createShader(
        {.name = passName + "_Shader", .layout = layout_, .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-streaming.spirv"), .type = ::rendering::ShaderType::SpirV});
    pipeline_ = renderGraph->createComputePipeline(
        {.entry = kernel_ == Kernel::ClearPageRequests ? "clearPageRequests" : "applyPhysicalPageRewrites", .layout = layout_, .name = passName + "_Pipeline", .shader = shader_});
  }

  Kernel kernel_{};
  Buffers buffers_{};
  SparseVoxelStreamingPassUniformsGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
