#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

// Traverses the renderer's current resident instance/BLAS cut once for the
// active dirty-job prefix and splats reached leaves directly into T3's cell
// scratch. This is update-time traversal; shadow rays never touch this BLAS.
class SparseVoxelTree64ScenePass final : public ::rendering::Pass
{
public:
  enum class Kernel
  {
    Clear,
    Seed,
    Traverse,
    Finalize,
    PrepareFailClosed,
    FailClosed,
  };

  struct Buffers
  {
    ::rendering::Buffer jobs;
    ::rendering::Buffer cells;
    ::rendering::Buffer instances;
    ::rendering::Buffer hierarchyNodes;
    ::rendering::Buffer centersLengths;
    ::rendering::Buffer blasRoots;
    ::rendering::Buffer queueA;
    ::rendering::Buffer queueB;
    ::rendering::Buffer queueCounts;
    ::rendering::Buffer dispatchArgs;
    ::rendering::Buffer counters;
  };

  SparseVoxelTree64ScenePass(const Kernel kernel, const Buffers buffers, const SparseVoxelTree64SceneTraversalGPU settings) : kernel_(kernel), buffers_(buffers), settings_(settings)
  {
    if (settings_.traversalLevelCount == 0u || settings_.queueCapacity == 0u || settings_.chunkSide == 0u || settings_.cellCount == 0u || settings_.cellCapacity == 0u ||
        settings_.cellWordCount == 0u || settings_.cellWordCapacity == 0u)
      throw std::invalid_argument("Tree64 scene traversal settings are invalid");
    chunkCapacity_ = settings_.cellWordCapacity / settings_.cellWordCount;
    instanceCapacity_ = settings_.instanceCount;
    hierarchyNodeCapacity_ = settings_.hierarchyNodeCount;
    blasCapacity_ = settings_.blasCount;
  }

  ~SparseVoxelTree64ScenePass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (directDispatchArgs_.isValid())
      renderGraph->deleteBuffer(directDispatchArgs_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  bool update(const uint32_t chunkCount, const uint32_t instanceCount, const uint32_t hierarchyNodeCount, const uint32_t blasCount)
  {
    if (chunkCount > chunkCapacity_ || instanceCount > instanceCapacity_ || hierarchyNodeCount > hierarchyNodeCapacity_ || blasCount > blasCapacity_)
      return false;
    settings_.chunkCount = chunkCount;
    settings_.instanceCount = instanceCount;
    settings_.hierarchyNodeCount = hierarchyNodeCount;
    settings_.blasCount = blasCount;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    if (directDispatchArgs_.isValid())
      writeDirectDispatchArgs();
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    writeDirectDispatchArgs();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    switch (kernel_)
    {
    case Kernel::Clear:
    case Kernel::Seed:
    case Kernel::Finalize:
    case Kernel::PrepareFailClosed:
      commandBuffer.cmdDispatchIndirect(directDispatchArgs_, 0u);
      break;
    case Kernel::Traverse:
      commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, uint64_t{settings_.depth} * sizeof(IndirectDispatchArgsGPU));
      break;
    case Kernel::FailClosed:
      commandBuffer.cmdDispatchIndirect(buffers_.dispatchArgs, uint64_t{settings_.traversalLevelCount} * sizeof(IndirectDispatchArgsGPU));
      break;
    }
  }

private:
  void writeDirectDispatchArgs()
  {
    uint32_t groupCount = 0u;
    if (settings_.chunkCount != 0u)
    {
      if (kernel_ == Kernel::Clear)
        groupCount = std::max(1u, (settings_.traversalLevelCount + 64u) / 64u);
      else if (kernel_ == Kernel::Seed)
        groupCount = (settings_.instanceCount + 63u) / 64u;
      else if (kernel_ == Kernel::Finalize || kernel_ == Kernel::PrepareFailClosed)
        groupCount = 1u;
    }
    IndirectDispatchArgsGPU args{groupCount, 1u, 1u, 0u};
    renderGraph->bufferWrite(directDispatchArgs_, 0u, sizeof(args), &args);
  }

  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t size, const ::rendering::AccessPattern access)
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
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    directDispatchArgs_ = renderGraph->createBuffer({
      .name = passName + "_DirectDispatchArgs",
      .size = sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push,
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
                                    {.name = "traversal",
                                     .binding = 0u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "jobs",
                                     .binding = 1u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "cells",
                                     .binding = 2u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "instances",
                                     .binding = 3u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "hierarchyNodes",
                                     .binding = 4u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "centersLengths",
                                     .binding = 5u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "blasRoots",
                                     .binding = 6u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "queueA",
                                     .binding = 7u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "queueB",
                                     .binding = 8u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "queueCounts",
                                     .binding = 9u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "dispatchArgs",
                                     .binding = 10u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                    {.name = "counters",
                                     .binding = 11u,
                                     .isDynamic = false,
                                     .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
                                     .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
                                  },
                            }},
                      });
    const auto read = ::rendering::AccessPattern::SHADER_READ;
    const auto readWrite = read | ::rendering::AccessPattern::SHADER_WRITE;
    groups_ = renderGraph
                  ->createBindingGroups(
                      {
                        .layout = layout_,
                        .name = passName + "_Groups",
                        .groups =
                            {::rendering::GroupInfo{
                              .name = "group0",
                              .buffers =
                                  {
                                    {.binding = 0u, .bufferView = view(uniforms_, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ)},
                                    {.binding = 1u, .bufferView = view(buffers_.jobs, uint64_t{chunkCapacity_} * sizeof(SparseVoxelTree64BuildJobGPU), read)},
                                    {.binding = 2u, .bufferView = view(buffers_.cells, uint64_t{settings_.cellWordCapacity} * sizeof(uint32_t), readWrite)},
                                    {.binding = 3u, .bufferView = view(buffers_.instances, uint64_t{instanceCapacity_} * sizeof(SparseVoxelInstanceGPU), read)},
                                    {.binding = 4u, .bufferView = view(buffers_.hierarchyNodes, uint64_t{hierarchyNodeCapacity_} * sizeof(SparseVoxelLODNodeGPU), read)},
                                    {.binding = 5u, .bufferView = view(buffers_.centersLengths, uint64_t{hierarchyNodeCapacity_} * sizeof(SparseVoxelCenterLengthGPU), read)},
                                    {.binding = 6u, .bufferView = view(buffers_.blasRoots, uint64_t{blasCapacity_} * sizeof(SparseVoxelBLASRootRangeGPU), read)},
                                    {.binding = 7u, .bufferView = view(buffers_.queueA, uint64_t{settings_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU), readWrite)},
                                    {.binding = 8u, .bufferView = view(buffers_.queueB, uint64_t{settings_.queueCapacity} * sizeof(SparseVoxelLODQueueElementGPU), readWrite)},
                                    {.binding = 9u, .bufferView = view(buffers_.queueCounts, uint64_t{settings_.traversalLevelCount} * sizeof(uint32_t), readWrite)},
                                    {.binding = 10u, .bufferView = view(buffers_.dispatchArgs, uint64_t{settings_.traversalLevelCount + 1u} * sizeof(IndirectDispatchArgsGPU), readWrite)},
                                    {.binding = 11u, .bufferView = view(buffers_.counters, sizeof(SparseVoxelTree64SceneCountersGPU), readWrite)},
                                  },
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-tree64-scene.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    const char *entry = "clearTree64SceneTraversal";
    if (kernel_ == Kernel::Seed)
      entry = "seedTree64SceneTraversal";
    else if (kernel_ == Kernel::Traverse)
      entry = "traverseTree64Scene";
    else if (kernel_ == Kernel::Finalize)
      entry = "finalizeTree64SceneTraversal";
    else if (kernel_ == Kernel::PrepareFailClosed)
      entry = "prepareFailClosedTree64SceneTraversal";
    else if (kernel_ == Kernel::FailClosed)
      entry = "failClosedTree64SceneTraversal";
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry,
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Kernel kernel_;
  Buffers buffers_{};
  SparseVoxelTree64SceneTraversalGPU settings_{};
  uint32_t chunkCapacity_ = 0u;
  uint32_t instanceCapacity_ = 0u;
  uint32_t hierarchyNodeCapacity_ = 0u;
  uint32_t blasCapacity_ = 0u;
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer directDispatchArgs_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
