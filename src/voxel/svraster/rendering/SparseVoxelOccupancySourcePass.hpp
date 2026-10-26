#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancySourcePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer counters;
    ::rendering::Buffer selectedLeaves;
    ::rendering::Buffer instances;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer sourceAABBs;
  };

  SparseVoxelOccupancySourcePass(const Buffers buffers, const SparseVoxelOccupancySourceGPU settings) : buffers_(buffers), settings_(settings)
  {
  }

  bool updateSourceCount(const uint32_t sourceCount)
  {
    if (sourceCount > settings_.sourceCapacity ||
        sourceCount == std::numeric_limits<uint32_t>::max())
      return false;
    // Zero remains the legacy "read the shared visible counter" mode.
    settings_.reserved = sourceCount + 1u;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(
          uniforms_, 0u, sizeof(settings_), &settings_);
    return true;
  }

  ~SparseVoxelOccupancySourcePass() override
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
    commandBuffer.cmdDispatch(std::max(1u, (settings_.sourceCapacity + 255u) / 256u), 1u, 1u);
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
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(settings_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "sourceState", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "counters", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "selectedLeaves",
               .binding = 2u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "instances", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "centersLengths",
               .binding = 4u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "sourceAABBs", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = read | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(uniforms_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 1u, .bufferView = view(buffers_.counters, sizeof(SparseVoxelFrameCountersGPU), readWrite)},
              {.binding = 2u, .bufferView = view(buffers_.selectedLeaves, uint64_t{settings_.sourceCapacity} * sizeof(VisibleVoxelGPU), read)},
              {.binding = 3u, .bufferView = view(buffers_.instances, uint64_t{settings_.instanceCapacity} * sizeof(SparseVoxelInstanceGPU), read)},
              {.binding = 4u, .bufferView = view(buffers_.centersLengths, uint64_t{settings_.centerCapacity} * sizeof(SparseVoxelCenterLengthGPU), read)},
              {.binding = 5u, .bufferView = view(buffers_.sourceAABBs, uint64_t{settings_.sourceCapacity} * sizeof(SparseVoxelOccupancyAABBGPU), readWrite)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-occupancy-sources.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "transformOccupancySourceAABBs",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  SparseVoxelOccupancySourceGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
