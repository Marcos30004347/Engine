#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace voxel::svraster::rendering
{

// Fuses stable-leaf transformation, dirty-query rejection, and toroidally
// addressed atomic bit splatting. No CPU AABBs or intermediate GPU source
// list are required.
class SparseVoxelOccupancySourceSplatPass final
    : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer selectedLeaves;
    ::rendering::Buffer instances;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer dirtyQueryBoxes;
    ::rendering::Buffer baseWords;
    ::rendering::Buffer denseDirtyMasks;
  };

  SparseVoxelOccupancySourceSplatPass(
      const Buffers buffers,
      const SparseVoxelOccupancySourceGPU settings,
      const uint64_t baseWordCount)
      : buffers_(buffers),
        settings_(settings),
        baseWordCount_(baseWordCount)
  {
  }

  bool update(
      const SparseVoxelOccupancyVolumeGPU &state,
      const uint32_t sourceCount)
  {
    if (sourceCount > settings_.sourceCapacity ||
        sourceCount == std::numeric_limits<uint32_t>::max())
      return false;
    settings_.reserved = sourceCount + 1u;
    state_ = state;
    if (uniforms_.isValid())
    {
      renderGraph->bufferWrite(
          uniforms_, 0u, sizeof(settings_), &settings_);
      renderGraph->bufferWrite(
          stateUniforms_, 0u, sizeof(state_), &state_);
    }
    return true;
  }

  ~SparseVoxelOccupancySourceSplatPass() override
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
    if (stateUniforms_.isValid())
      renderGraph->deleteBuffer(stateUniforms_);
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(settings_), &settings_);
    renderGraph->bufferWrite(
        stateUniforms_, 0u, sizeof(state_), &state_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(
            1u,
            (settings_.sourceCapacity + 255u) / 256u),
        1u,
        1u);
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer,
      const uint64_t size,
      const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(settings_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    stateUniforms_ = renderGraph->createBuffer({
      .name = passName + "_OccupancyState",
      .size = sizeof(state_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "sourceState", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "selectedLeaves", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "instances", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "centersLengths", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "occupancyState", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyQueryBoxes", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "occupancyAtomicWords", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "denseDirtyMasks", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });
    const auto read =
        ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite =
        read | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(uniforms_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 2u, .bufferView = view(buffers_.selectedLeaves, uint64_t{settings_.sourceCapacity} * sizeof(VisibleVoxelGPU), read)},
              {.binding = 3u, .bufferView = view(buffers_.instances, uint64_t{settings_.instanceCapacity} * sizeof(SparseVoxelInstanceGPU), read)},
              {.binding = 4u, .bufferView = view(buffers_.centersLengths, uint64_t{settings_.centerCapacity} * sizeof(SparseVoxelCenterLengthGPU), read)},
              {.binding = 6u, .bufferView = view(stateUniforms_, sizeof(state_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 7u, .bufferView = view(buffers_.dirtyQueryBoxes, 3u * sizeof(SparseVoxelOccupancyAABBGPU), read)},
              {.binding = 9u, .bufferView = view(buffers_.baseWords, baseWordCount_ * sizeof(uint32_t), readWrite)},
              {.binding = 10u, .bufferView = view(buffers_.denseDirtyMasks, baseWordCount_ * sizeof(uint32_t), readWrite)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-sources.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "splatFilteredOccupancySources",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  SparseVoxelOccupancySourceGPU settings_{};
  SparseVoxelOccupancyVolumeGPU state_{};
  uint64_t baseWordCount_ = 0u;
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer stateUniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
