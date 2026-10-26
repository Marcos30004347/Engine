#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyDDAPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer baseState;
    ::rendering::Buffer pyramidState;
    ::rendering::Buffer levelDescriptors;
    ::rendering::Buffer mipWords;
    ::rendering::Buffer rays;
    ::rendering::Buffer results;
  };

  struct Capacities
  {
    uint32_t rayCount = 0u;
    uint32_t levelCount = 0u;
    uint64_t mipWordCount = 0u;
  };

  SparseVoxelOccupancyDDAPass(
      const ::rendering::Texture occupancyTexture,
      const Buffers buffers,
      const Capacities capacities)
      : occupancyTexture_(occupancyTexture), buffers_(buffers),
        capacities_(capacities)
  {
    capacities_.rayCount = std::max(1u, capacities_.rayCount);
    capacities_.levelCount = std::max(1u, capacities_.levelCount);
    capacities_.mipWordCount = std::max<uint64_t>(1u, capacities_.mipWordCount);
  }

  ~SparseVoxelOccupancyDDAPass() override
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

  bool update(const uint32_t rayCount, const uint32_t maximumQueries)
  {
    if (rayCount > capacities_.rayCount || maximumQueries == 0u)
      return false;
    dispatch_.rayCount = rayCount;
    dispatch_.maximumQueries = maximumQueries;
    dispatch_.levelCapacity = capacities_.levelCount;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(
          uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    return true;
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(
        std::max(1u, (capacities_.rayCount + 63u) / 64u), 1u, 1u);
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
      .size = sizeof(dispatch_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "occupancyState", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "pyramidState", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "levelDescriptors", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "mipWords", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "rays", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "results", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dispatch", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
        .textures =
            {
              {.name = "occupancyTextureRead", .binding = 6u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
              {.binding = 0u, .bufferView = view(buffers_.baseState, sizeof(SparseVoxelOccupancyVolumeGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 1u, .bufferView = view(buffers_.pyramidState, sizeof(SparseVoxelOccupancyPyramidGPU), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 2u, .bufferView = view(buffers_.levelDescriptors, uint64_t{capacities_.levelCount} * sizeof(SparseVoxelOccupancyMipLevelGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 3u, .bufferView = view(buffers_.mipWords, capacities_.mipWordCount * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 4u, .bufferView = view(buffers_.rays, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDARayGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 5u, .bufferView = view(buffers_.results, uint64_t{capacities_.rayCount} * sizeof(SparseVoxelOccupancyDDAResultGPU), ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 7u, .bufferView = view(uniforms_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
            },
        .textures =
            {
              {.textureView = sampledTextureView(), .binding = 6u},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-dda.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "traceOccupancyDDA",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  ::rendering::Texture occupancyTexture_{};
  Buffers buffers_{};
  Capacities capacities_{};
  SparseVoxelOccupancyDDADispatchGPU dispatch_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
