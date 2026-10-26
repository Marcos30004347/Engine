#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyProbePass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer worldPositions;
    ::rendering::Buffer results;
  };

  SparseVoxelOccupancyProbePass(const ::rendering::Texture occupancyTexture, const Buffers buffers, const uint32_t probeCapacity)
      : occupancyTexture_(occupancyTexture), buffers_(buffers), probeCapacity_(std::max(1u, probeCapacity))
  {
  }

  ~SparseVoxelOccupancyProbePass() override
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

  bool update(const SparseVoxelOccupancyVolumeGPU &state, const uint32_t probeCount)
  {
    if (probeCount > probeCapacity_)
      return false;
    settings_.state = state;
    settings_.probeCount = probeCount;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(std::max(1u, (probeCapacity_ + 63u) / 64u), 1u, 1u);
  }

private:
  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  ::rendering::TextureView sampledTextureView() const
  {
    return {
      .texture = occupancyTexture_,
      .access = ::rendering::AccessPattern::SHADER_READ,
      .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
      .index = 0u,
      .flags = ::rendering::ImageAspectFlags::Color,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
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
              {.name = "probe", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "probeWorldPositions",
               .binding = 2u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "probeResults", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
        .textures =
            {
              {.name = "occupancyTextureRead", .binding = 1u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(uniforms_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 2u, .bufferView = view(buffers_.worldPositions, uint64_t{probeCapacity_} * sizeof(std::array<float, 4>), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 3u, .bufferView = view(buffers_.results, uint64_t{probeCapacity_} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_WRITE)},
            },
        .textures =
            {
              {.textureView = sampledTextureView(), .binding = 1u},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-probe.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "probeOccupancyAtWorldPositions",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  ::rendering::Texture occupancyTexture_{};
  Buffers buffers_{};
  uint32_t probeCapacity_ = 1u;
  SparseVoxelOccupancyProbeGPU settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
