#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyDirtyPass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Reset,
    ResetAndCullQueries,
    CullQueries,
    MarkFull,
    MarkQuery,
    Finalize,
    ClearFull,
    Clear,
    PublishFull,
    Publish,
  };

  struct Buffers
  {
    ::rendering::Buffer dirtyQueryBoxes;
    ::rendering::Buffer denseDirtyMasks;
    ::rendering::Buffer dirtyWordIndices;
    ::rendering::Buffer dirtyWordCount;
    ::rendering::Buffer dispatchArgs;
    ::rendering::Buffer baseWords;
    ::rendering::Buffer instances;
    ::rendering::Buffer mipWordCounts;
  };

  SparseVoxelOccupancyDirtyPass(
      const Kernel kernel,
      const Buffers buffers,
      const ::rendering::Texture occupancyTexture,
      const uint32_t wordCapacity,
      const uint32_t instanceCount,
      const uint32_t mipLevelCount,
      const uint32_t queryIndex = 0u)
      : kernel_(kernel),
        buffers_(buffers),
        occupancyTexture_(occupancyTexture),
        wordCapacity_(std::max(1u, wordCapacity)),
        instanceCount_(instanceCount),
        mipLevelCount_(mipLevelCount),
        queryIndex_(queryIndex)
  {
  }

  void update(
      const SparseVoxelOccupancyVolumeGPU &state,
      const std::vector<SparseVoxelAABB> &queryBoxes)
  {
    state_ = state;
    queryBoxCount_ =
        static_cast<uint32_t>(std::min<size_t>(queryBoxes.size(), 3u));
    uint32_t cameraQueryBoxCount = 0u;
    if (state.fullRebuild == 0u)
      for (const int32_t delta : state.scrollDelta)
        cameraQueryBoxCount += delta != 0 ? 1u : 0u;
    cameraQueryBoxCount =
        std::min(cameraQueryBoxCount, queryBoxCount_);
    dispatch_ = {
      queryIndex_,
      queryBoxCount_,
      wordCapacity_,
      instanceCount_,
      cameraQueryBoxCount,
      mipLevelCount_,
      0u,
      0u,
    };
    writeUniforms();
  }

  ~SparseVoxelOccupancyDirtyPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (dispatchBuffer_.isValid())
      renderGraph->deleteBuffer(dispatchBuffer_);
    if (stateBuffer_.isValid())
      renderGraph->deleteBuffer(stateBuffer_);
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    writeUniforms();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::ResetAndCullQueries)
      commandBuffer.cmdDispatch(1u, 1u, 1u);
    else if (kernel_ == Kernel::CullQueries)
      commandBuffer.cmdDispatch(3u, 1u, 1u);
    else if (kernel_ == Kernel::MarkFull ||
        kernel_ == Kernel::ClearFull ||
        kernel_ == Kernel::PublishFull)
      commandBuffer.cmdDispatchIndirect(
          buffers_.dispatchArgs,
          4u * sizeof(IndirectDispatchArgsGPU));
    else if (kernel_ == Kernel::MarkQuery)
      commandBuffer.cmdDispatchIndirect(
          buffers_.dispatchArgs,
          uint64_t{queryIndex_ + 1u} *
              sizeof(IndirectDispatchArgsGPU));
    else if (kernel_ == Kernel::Clear ||
             kernel_ == Kernel::Publish)
      commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, 0u);
    else
      commandBuffer.cmdDispatch(1u, 1u, 1u);
  }

private:
  struct alignas(16) Dispatch
  {
    uint32_t queryIndex = 0u;
    uint32_t queryBoxCount = 0u;
    uint32_t wordCapacity = 0u;
    uint32_t instanceCount = 0u;
    uint32_t cameraQueryBoxCount = 0u;
    uint32_t mipLevelCount = 0u;
    uint32_t reserved1 = 0u;
    uint32_t reserved2 = 0u;
  };

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

  void writeUniforms()
  {
    if (!stateBuffer_.isValid())
      return;
    renderGraph->bufferWrite(
        stateBuffer_, 0u, sizeof(state_), &state_);
    renderGraph->bufferWrite(
        dispatchBuffer_, 0u, sizeof(dispatch_), &dispatch_);
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    stateBuffer_ = renderGraph->createBuffer({
      .name = passName + "_State",
      .size = sizeof(state_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    dispatchBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Dispatch",
      .size = sizeof(dispatch_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "occupancyState", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyDispatch", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyQueryBoxes", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "denseDirtyMasks", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyWordIndices", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyWordCount", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyDispatchArgs", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "occupancyAtomicWords", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "instances", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "mipWordCounts", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
        .storageTextures =
            {
              {.name = "occupancyTextureWrite", .binding = 8u, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite =
        read | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.binding = 0u, .bufferView = view(stateBuffer_, sizeof(state_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 1u, .bufferView = view(dispatchBuffer_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 2u, .bufferView = view(buffers_.dirtyQueryBoxes, 3u * sizeof(SparseVoxelOccupancyAABBGPU), readWrite)},
              {.binding = 3u, .bufferView = view(buffers_.denseDirtyMasks, uint64_t{wordCapacity_} * sizeof(uint32_t), readWrite)},
              {.binding = 4u, .bufferView = view(buffers_.dirtyWordIndices, uint64_t{wordCapacity_} * sizeof(uint32_t), readWrite)},
              {.binding = 5u, .bufferView = view(buffers_.dirtyWordCount, sizeof(uint32_t), readWrite)},
              {.binding = 6u, .bufferView = view(buffers_.dispatchArgs, uint64_t{5u + mipLevelCount_} * sizeof(IndirectDispatchArgsGPU), readWrite)},
              {.binding = 7u, .bufferView = view(buffers_.baseWords, uint64_t{wordCapacity_} * sizeof(uint32_t), readWrite)},
              {.binding = 9u, .bufferView = view(buffers_.instances, uint64_t{std::max(1u, instanceCount_)} * sizeof(SparseVoxelInstanceGPU), read)},
              {.binding = 10u, .bufferView = view(buffers_.mipWordCounts, uint64_t{std::max(1u, mipLevelCount_)} * sizeof(uint32_t), read)},
            },
        .storageTextures =
            {
              {.binding = 8u, .textureView = storageTextureView()},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-dirty.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = "resetSparseDirtyState";
    if (kernel_ == Kernel::ResetAndCullQueries)
      entry = "resetAndCullSparseDirtyCameraQueries";
    else if (kernel_ == Kernel::CullQueries)
      entry = "cullSparseDirtyCameraQueries";
    else if (kernel_ == Kernel::MarkFull)
      entry = "markAllDirtyWords";
    else if (kernel_ == Kernel::MarkQuery)
      entry = "markSparseDirtyQuery";
    else if (kernel_ == Kernel::Finalize)
      entry = "finalizeSparseDirtyState";
    else if (kernel_ == Kernel::ClearFull)
      entry = "clearFullDirtyWords";
    else if (kernel_ == Kernel::Clear)
      entry = "clearSparseDirtyWords";
    else if (kernel_ == Kernel::PublishFull)
      entry = "publishFullDirtyWords";
    else if (kernel_ == Kernel::Publish)
      entry = "publishSparseDirtyWords";
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
  uint32_t wordCapacity_ = 1u;
  uint32_t instanceCount_ = 0u;
  uint32_t mipLevelCount_ = 0u;
  uint32_t queryIndex_ = 0u;
  uint32_t queryBoxCount_ = 0u;
  SparseVoxelOccupancyVolumeGPU state_{};
  Dispatch dispatch_{};
  ::rendering::Buffer stateBuffer_{};
  ::rendering::Buffer dispatchBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
