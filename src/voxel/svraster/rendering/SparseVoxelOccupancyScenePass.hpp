#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <cstdint>
#include <string>

namespace voxel::svraster::rendering
{

class SparseVoxelOccupancyScenePass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Clear,
    Seed,
    Traverse,
    Finalize,
  };

  struct Buffers
  {
    ::rendering::Buffer dirtyQueryBoxes;
    ::rendering::Buffer denseDirtyMasks;
    ::rendering::Buffer baseWords;
    ::rendering::Buffer instances;
    ::rendering::Buffer hierarchyNodes;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer blasRoots;
    ::rendering::Buffer queueA;
    ::rendering::Buffer queueB;
    ::rendering::Buffer queueCounts;
    ::rendering::Buffer dispatchArgs;
  };

  SparseVoxelOccupancyScenePass(
      const Kernel kernel,
      const Buffers buffers,
      const SparseVoxelOccupancySceneTraversalGPU settings,
      const uint64_t baseWordCount)
      : kernel_(kernel),
        buffers_(buffers),
        settings_(settings),
        baseWordCount_(baseWordCount)
  {
  }

  void update(
      const SparseVoxelOccupancyVolumeGPU &state,
      const uint32_t queryBoxCount)
  {
    state_ = state;
    settings_.queryBoxCount = std::min(queryBoxCount, 3u);
    if (settingsBuffer_.isValid())
      renderGraph->bufferWrite(
          settingsBuffer_, 0u, sizeof(settings_), &settings_);
    if (stateBuffer_.isValid())
      renderGraph->bufferWrite(
          stateBuffer_, 0u, sizeof(state_), &state_);
  }

  ~SparseVoxelOccupancyScenePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (settingsBuffer_.isValid())
      renderGraph->deleteBuffer(settingsBuffer_);
    if (stateBuffer_.isValid())
      renderGraph->deleteBuffer(stateBuffer_);
  }

  void recordCommandBuffer(
      ::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(
        settingsBuffer_, 0u, sizeof(settings_), &settings_);
    renderGraph->bufferWrite(
        stateBuffer_, 0u, sizeof(state_), &state_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    if (kernel_ == Kernel::Clear)
      commandBuffer.cmdDispatch(
          std::max(
              1u,
              (settings_.traversalLevelCount + 63u) / 64u),
          1u,
          1u);
    else if (kernel_ == Kernel::Seed)
      commandBuffer.cmdDispatch(
          std::max(
              1u,
              ((settings_.queryBoxCount == 0u
                    ? 0u
                    : settings_.instanceCount) +
               63u) /
                  64u),
          1u,
          1u);
    else if (kernel_ == Kernel::Traverse)
      commandBuffer.cmdDispatchIndirect(
          buffers_.dispatchArgs,
          uint64_t{settings_.depth} *
              sizeof(IndirectDispatchArgsGPU));
    else
      commandBuffer.cmdDispatch(1u, 1u, 1u);
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
    settingsBuffer_ = renderGraph->createBuffer({
      .name = passName + "_Settings",
      .size = sizeof(settings_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    stateBuffer_ = renderGraph->createBuffer({
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
              {.name = "traversal", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "occupancyState", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dirtyQueryBoxes", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "denseDirtyMasks", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "occupancyAtomicWords", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "instances", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "hierarchyNodes", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "centersLengths", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "blasRoots", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "queueA", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "queueB", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "queueCounts", .binding = 11u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dispatchArgs", .binding = 12u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
              {.binding = 0u, .bufferView = view(settingsBuffer_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 1u, .bufferView = view(stateBuffer_, sizeof(state_), ::rendering::AccessPattern::UNIFORM_READ)},
              {.binding = 2u, .bufferView = view(buffers_.dirtyQueryBoxes, 3u * sizeof(SparseVoxelOccupancyAABBGPU), read)},
              {.binding = 3u, .bufferView = view(buffers_.denseDirtyMasks, baseWordCount_ * sizeof(uint32_t), readWrite)},
              {.binding = 4u, .bufferView = view(buffers_.baseWords, baseWordCount_ * sizeof(uint32_t), readWrite)},
              {.binding = 5u, .bufferView = view(buffers_.instances, uint64_t{settings_.instanceCount} * sizeof(SparseVoxelInstanceGPU), read)},
              {.binding = 6u, .bufferView = view(buffers_.hierarchyNodes, uint64_t{settings_.hierarchyNodeCount} * sizeof(SparseVoxelLODNodeGPU), read)},
              {.binding = 7u, .bufferView = view(buffers_.centersLengths, uint64_t{settings_.hierarchyNodeCount} * sizeof(SparseVoxelCenterLengthGPU), read)},
              {.binding = 8u, .bufferView = view(buffers_.blasRoots, uint64_t{settings_.blasCount} * sizeof(SparseVoxelBLASRootRangeGPU), read)},
              {.binding = 9u, .bufferView = view(buffers_.queueA, uint64_t{settings_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU), readWrite)},
              {.binding = 10u, .bufferView = view(buffers_.queueB, uint64_t{settings_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU), readWrite)},
              {.binding = 11u, .bufferView = view(buffers_.queueCounts, uint64_t{settings_.traversalLevelCount} * sizeof(uint32_t), readWrite)},
              {.binding = 12u, .bufferView = view(buffers_.dispatchArgs, uint64_t{settings_.traversalLevelCount} * sizeof(IndirectDispatchArgsGPU), readWrite)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-occupancy-scene.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = "clearOccupancySceneTraversal";
    if (kernel_ == Kernel::Seed)
      entry = "seedOccupancySceneTraversal";
    else if (kernel_ == Kernel::Traverse)
      entry = "traverseOccupancyScene";
    else if (kernel_ == Kernel::Finalize)
      entry = "finalizeOccupancySceneTraversal";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Kernel kernel_;
  Buffers buffers_{};
  SparseVoxelOccupancySceneTraversalGPU settings_{};
  SparseVoxelOccupancyVolumeGPU state_{};
  uint64_t baseWordCount_ = 0u;
  ::rendering::Buffer settingsBuffer_{};
  ::rendering::Buffer stateBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
