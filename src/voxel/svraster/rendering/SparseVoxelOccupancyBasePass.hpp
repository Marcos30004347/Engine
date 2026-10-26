#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyBasePass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    ClearDirtyWords,
    SplatAABBs,
    CopyDirtyWordsToTexture,
    ClassifyDenseDirtyMasks,
    ClearDenseDirtyWords,
    CopyDenseDirtyWordsToTexture,
  };

  struct Buffers
  {
    ::rendering::Buffer baseWords;
    ::rendering::Buffer dirtyWords;
    ::rendering::Buffer sourceAABBs;
    ::rendering::Buffer denseDirtyMasks;
    ::rendering::Buffer dirtyQueryBoxes;
  };

  struct Capacities
  {
    uint64_t baseWordCount = 0u;
    uint32_t dirtyWordCount = 0u;
    uint32_t sourceAABBCount = 0u;
  };

  SparseVoxelOccupancyBasePass(const Kernel kernel, const Buffers buffers, const ::rendering::Texture occupancyTexture, const Capacities capacities)
      : kernel_(kernel), buffers_(buffers), occupancyTexture_(occupancyTexture), capacities_(capacities)
  {
    capacities_.dirtyWordCount = std::max(1u, capacities_.dirtyWordCount);
    capacities_.sourceAABBCount = std::max(1u, capacities_.sourceAABBCount);
  }

  ~SparseVoxelOccupancyBasePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (dispatchUniforms_.isValid())
      renderGraph->deleteBuffer(dispatchUniforms_);
    if (stateUniforms_.isValid())
      renderGraph->deleteBuffer(stateUniforms_);
  }

  void update(const SparseVoxelOccupancyVolumeGPU &state, const SparseVoxelOccupancyDispatchGPU &dispatch)
  {
    state_ = state;
    dispatch_ = dispatch;
    if (stateUniforms_.isValid())
    {
      renderGraph->bufferWrite(stateUniforms_, 0u, sizeof(state_), &state_);
      renderGraph->bufferWrite(dispatchUniforms_, 0u, sizeof(dispatch_), &dispatch_);
    }
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(stateUniforms_, 0u, sizeof(state_), &state_);
    renderGraph->bufferWrite(dispatchUniforms_, 0u, sizeof(dispatch_), &dispatch_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    const bool splat = kernel_ == Kernel::SplatAABBs;
    const bool dense =
        kernel_ == Kernel::ClassifyDenseDirtyMasks ||
        kernel_ == Kernel::ClearDenseDirtyWords ||
        kernel_ == Kernel::CopyDenseDirtyWordsToTexture;
    uint64_t capacity = splat
        ? capacities_.sourceAABBCount
        : (dense ? capacities_.baseWordCount
                 : capacities_.dirtyWordCount);
    if (dense && dispatch_.reserved0 == 0u)
      capacity = 1u;
    commandBuffer.cmdDispatch(
        static_cast<uint32_t>(
            std::max<uint64_t>(1u, (capacity + 63u) / 64u)),
        1u,
        1u);
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

  ::rendering::TextureView storageTextureView() const
  {
    return {
      .texture = occupancyTexture_,
      .access = ::rendering::AccessPattern::SHADER_WRITE,
      .layout = ::rendering::ResourceLayout::GENERAL,
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

    stateUniforms_ = renderGraph->createBuffer({
      .name = passName + "_State",
      .size = sizeof(state_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    dispatchUniforms_ = renderGraph->createBuffer({
      .name = passName + "_Dispatch",
      .size = sizeof(dispatch_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph
                  ->createBindingsLayout(
                      {
                        .name = passName + "_Layout",
                        .groups =
                            {::rendering::BindingGroupLayout{
                              .buffers =
                                  {
                                    {.name = "occupancyState",
                                     .binding = 0u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "occupancyAtomicWords",
                                     .binding = 2u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "dirtyWords",
                                     .binding = 3u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "occupancyDispatch",
                                     .binding = 6u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "sourceAABBs",
                                     .binding = 7u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "denseDirtyMasks",
                                     .binding = 8u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "dirtyQueryBoxes",
                                     .binding = 9u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                              .storageTextures =
                                  {
                                    {.name = "occupancyTextureWrite", .binding = 5u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                            }},
                      });

    const auto readWrite = ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ =
        renderGraph->createBindingGroups(
            {
              .layout = layout_,
              .name = passName + "_Groups",
              .groups = {::rendering::GroupInfo{
                .name = "group0",
                .buffers =
                    {
                      {.binding = 0u, .bufferView = view(stateUniforms_, sizeof(state_), ::rendering::AccessPattern::UNIFORM_READ)},
                      {.binding = 2u, .bufferView = view(buffers_.baseWords, capacities_.baseWordCount * sizeof(uint32_t), readWrite)},
                      {.binding = 3u, .bufferView = view(buffers_.dirtyWords, uint64_t{capacities_.dirtyWordCount} * sizeof(SparseVoxelOccupancyDirtyWordGPU), ::rendering::AccessPattern::SHADER_READ)},
                      {.binding = 6u, .bufferView = view(dispatchUniforms_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
                      {.binding = 7u, .bufferView = view(buffers_.sourceAABBs, uint64_t{capacities_.sourceAABBCount} * sizeof(SparseVoxelOccupancyAABBGPU), ::rendering::AccessPattern::SHADER_READ)},
                      {.binding = 8u, .bufferView = view(buffers_.denseDirtyMasks, capacities_.baseWordCount * sizeof(uint32_t), readWrite)},
                      {.binding = 9u, .bufferView = view(buffers_.dirtyQueryBoxes, 3u * sizeof(SparseVoxelOccupancyAABBGPU), ::rendering::AccessPattern::SHADER_READ)},
                    },
                .storageTextures =
                    {
                      {.textureView = storageTextureView(), .binding = 5u},
                    },
              }},
            });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = "copyDirtyOccupancyWordsToTexture";
    if (kernel_ == Kernel::ClearDirtyWords)
      entry = "clearDirtyWords";
    else if (kernel_ == Kernel::SplatAABBs)
      entry = "splatOccupancyAABBs";
    else if (kernel_ == Kernel::ClassifyDenseDirtyMasks)
      entry = "classifyDenseDirtyMasks";
    else if (kernel_ == Kernel::ClearDenseDirtyWords)
      entry = "clearDenseDirtyWords";
    else if (kernel_ == Kernel::CopyDenseDirtyWordsToTexture)
      entry = "copyDenseDirtyWordsToTexture";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Kernel kernel_{};
  Buffers buffers_{};
  ::rendering::Texture occupancyTexture_{};
  Capacities capacities_{};
  SparseVoxelOccupancyVolumeGPU state_{};
  SparseVoxelOccupancyDispatchGPU dispatch_{};
  ::rendering::Buffer stateUniforms_{};
  ::rendering::Buffer dispatchUniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
