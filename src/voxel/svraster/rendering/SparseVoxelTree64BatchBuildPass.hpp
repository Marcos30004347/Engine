#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64BatchBuildPass final : public ::rendering::Pass
{
public:
  enum class Stage : uint32_t
  {
    ClearCells,
    SplatSources,
    TerminalMasks,
    MiddleMasks,
    RootMasks,
    Compact,
  };

  struct Buffers
  {
    ::rendering::Buffer jobs;
    ::rendering::Buffer sources;
    ::rendering::Buffer cells;
    ::rendering::Buffer terminalMasks;
    ::rendering::Buffer middleMasks;
    ::rendering::Buffer rootMasks;
    ::rendering::Buffer nodes;
    ::rendering::Buffer results;
  };

  struct Capacities
  {
    uint32_t side = 0u;
    uint32_t chunkCount = 0u;
    uint32_t sourceCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t nodeCount = 0u;
  };

  SparseVoxelTree64BatchBuildPass(const Stage stage, const Buffers buffers, const Capacities capacities) : stage_(stage), buffers_(buffers), capacities_(capacities)
  {
    if (!sparseVoxelTree64SupportedSide(capacities_.side) || capacities_.chunkCount == 0u || capacities_.nodeSlotCapacity == 0u || capacities_.nodeCount == 0u)
      throw std::invalid_argument("Tree64 batch build capacities are invalid");
    capacities_.sourceCount = std::max(1u, capacities_.sourceCount);
    dispatch_.side = capacities_.side;
    dispatch_.cellCount = capacities_.side * capacities_.side * capacities_.side;
    dispatch_.nodeSlotCapacity = capacities_.nodeSlotCapacity;
    dispatch_.nodeCapacity = capacities_.nodeCount;
    const uint32_t terminalAxis = capacities_.side / 4u;
    dispatch_.terminalCount = terminalAxis * terminalAxis * terminalAxis;
    dispatch_.middleCount = capacities_.side == 64u ? 64u : 0u;
    dispatch_.cellCapacity = capacities_.chunkCount * dispatch_.cellCount;
    dispatch_.cellWordCount = (dispatch_.cellCount + 31u) / 32u;
    dispatch_.cellWordCapacity = capacities_.chunkCount * dispatch_.cellWordCount;
    dispatch_.terminalCapacity = capacities_.chunkCount * dispatch_.terminalCount;
    dispatch_.middleCapacity = std::max(1u, capacities_.chunkCount * dispatch_.middleCount);
    dispatch_.resultCapacity = capacities_.chunkCount;
  }

  ~SparseVoxelTree64BatchBuildPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (indirectArgs_.isValid())
      renderGraph->deleteBuffer(indirectArgs_);
    if (uniforms_.isValid())
      renderGraph->deleteBuffer(uniforms_);
  }

  bool update(const uint32_t chunkCount, const uint32_t sourceCount)
  {
    if (chunkCount > capacities_.chunkCount || sourceCount > capacities_.sourceCount)
      return false;
    dispatch_.chunkCount = chunkCount;
    dispatch_.sourceCount = sourceCount;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    if (indirectArgs_.isValid())
      writeIndirectArgs();
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    writeIndirectArgs();
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(indirectArgs_, 0u);
  }

private:
  uint64_t stageItemCount() const
  {
    switch (stage_)
    {
    case Stage::ClearCells:
      return uint64_t{dispatch_.chunkCount} * dispatch_.cellWordCount;
    case Stage::SplatSources:
      return dispatch_.sourceCount;
    case Stage::TerminalMasks:
      return uint64_t{dispatch_.chunkCount} * dispatch_.terminalCount;
    case Stage::MiddleMasks:
      return uint64_t{dispatch_.chunkCount} * dispatch_.middleCount;
    case Stage::RootMasks:
    case Stage::Compact:
      return dispatch_.chunkCount;
    }
    return 0u;
  }

  void writeIndirectArgs()
  {
    const uint64_t groupCount = (stageItemCount() + 63u) / 64u;
    if (groupCount > std::numeric_limits<uint32_t>::max())
      throw std::overflow_error("Tree64 batch build dispatch exceeds 32 bits");
    IndirectDispatchArgsGPU args{
      static_cast<uint32_t>(groupCount),
      1u,
      1u,
      0u,
    };
    renderGraph->bufferWrite(indirectArgs_, 0u, sizeof(args), &args);
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::ClearCells:
      return "clearTree64Cells";
    case Stage::SplatSources:
      return "splatTree64Sources";
    case Stage::TerminalMasks:
      return "buildTree64TerminalMasks";
    case Stage::MiddleMasks:
      return "buildTree64MiddleMasks";
    case Stage::RootMasks:
      return "buildTree64RootMasks";
    case Stage::Compact:
      return "compactTree64Chunks";
    }
    return "";
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

  ::rendering::AccessPattern cellsAccess() const
  {
    switch (stage_)
    {
    case Stage::ClearCells:
      return ::rendering::AccessPattern::SHADER_WRITE;
    case Stage::SplatSources:
      return ::rendering::AccessPattern::SHADER_READ_WRITE;
    case Stage::TerminalMasks:
      return ::rendering::AccessPattern::SHADER_READ;
    default:
      return ::rendering::AccessPattern::NONE;
    }
  }

  ::rendering::AccessPattern terminalMasksAccess() const
  {
    switch (stage_)
    {
    case Stage::TerminalMasks:
      return ::rendering::AccessPattern::SHADER_WRITE;
    case Stage::MiddleMasks:
    case Stage::RootMasks:
    case Stage::Compact:
      return ::rendering::AccessPattern::SHADER_READ;
    default:
      return ::rendering::AccessPattern::NONE;
    }
  }

  ::rendering::AccessPattern middleMasksAccess() const
  {
    switch (stage_)
    {
    case Stage::MiddleMasks:
      return ::rendering::AccessPattern::SHADER_WRITE;
    case Stage::RootMasks:
    case Stage::Compact:
      return ::rendering::AccessPattern::SHADER_READ;
    default:
      return ::rendering::AccessPattern::NONE;
    }
  }

  ::rendering::AccessPattern rootMasksAccess() const
  {
    switch (stage_)
    {
    case Stage::RootMasks:
      return ::rendering::AccessPattern::SHADER_WRITE;
    case Stage::Compact:
      return ::rendering::AccessPattern::SHADER_READ;
    default:
      return ::rendering::AccessPattern::NONE;
    }
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Uniforms",
      .size = sizeof(dispatch_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    indirectArgs_ = renderGraph->createBuffer({
      .name = passName + "_IndirectArgs",
      .size = sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect | ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = true,
    });
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "jobs", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "sources", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "cells", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "terminalMasks",
               .binding = 3u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "middleMasks", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "rootMasks", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "outputNodes", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "results", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dispatch", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
            },
      }},
    });
    const uint64_t cellStorageCount = uint64_t{capacities_.chunkCount} * dispatch_.cellWordCount;
    const uint64_t terminalCount = uint64_t{capacities_.chunkCount} * dispatch_.terminalCount;
    const uint64_t middleCount = std::max<uint64_t>(1u, uint64_t{capacities_.chunkCount} * dispatch_.middleCount);
    groups_ = renderGraph
                  ->createBindingGroups(
                      {
                        .layout = layout_,
                        .name = passName + "_Groups",
                        .groups =
                            {
                              ::rendering::GroupInfo{
                                .name = "group0",
                                .buffers =
                                    {
                                      {.binding = 0u, .bufferView = view(buffers_.jobs, uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildJobGPU), ::rendering::AccessPattern::SHADER_READ)},
                                      {.binding = 1u, .bufferView = view(buffers_.sources, uint64_t{capacities_.sourceCount} * sizeof(SparseVoxelTree64BuildSourceGPU), ::rendering::AccessPattern::SHADER_READ)},
                                      {.binding = 2u, .bufferView = view(buffers_.cells, cellStorageCount * sizeof(uint32_t), cellsAccess())},
                                      {.binding = 3u, .bufferView = view(buffers_.terminalMasks, terminalCount * sizeof(SparseVoxelTree64NodeGPU), terminalMasksAccess())},
                                      {.binding = 4u, .bufferView = view(buffers_.middleMasks, middleCount * sizeof(SparseVoxelTree64NodeGPU), middleMasksAccess())},
                                      {.binding = 5u, .bufferView = view(buffers_.rootMasks, uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64NodeGPU), rootMasksAccess())},
                                      {.binding = 6u,
                                       .bufferView = view(
                                           buffers_.nodes, uint64_t{capacities_.nodeCount} * sizeof(SparseVoxelTree64NodeGPU), stage_ == Stage::Compact ? ::rendering::AccessPattern::SHADER_WRITE : ::rendering::AccessPattern::NONE)},
                                      {.binding = 7u,
                                       .bufferView = view(
                                           buffers_.results, uint64_t{capacities_.chunkCount} * sizeof(SparseVoxelTree64BuildResultGPU), stage_ == Stage::Compact ? ::rendering::AccessPattern::SHADER_WRITE : ::rendering::AccessPattern::NONE)},
                                      {.binding = 8u, .bufferView = view(uniforms_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
                                    },
                              }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-tree64-batch-build.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = entry(),
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Stage stage_ = Stage::ClearCells;
  Buffers buffers_{};
  Capacities capacities_{};
  SparseVoxelTree64BatchBuildDispatchGPU dispatch_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer indirectArgs_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
