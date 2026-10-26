#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <cstdint>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64BuildPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer occupiedCells;
    ::rendering::Buffer nodes;
    ::rendering::Buffer result;
  };

  struct Capacities
  {
    uint32_t cellCount = 0u;
    uint32_t nodeCount = 0u;
  };

  SparseVoxelTree64BuildPass(const Buffers buffers, const Capacities capacities) : buffers_(buffers), capacities_(capacities)
  {
    capacities_.cellCount = std::max(1u, capacities_.cellCount);
    capacities_.nodeCount = std::max(1u, capacities_.nodeCount);
  }

  ~SparseVoxelTree64BuildPass() override
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

  bool update(const uint32_t side, const uint32_t cellCount, const uint32_t nodeSlot, const uint32_t nodeSlotCapacity, const uint32_t contentGeneration)
  {
    if (cellCount > capacities_.cellCount || nodeSlotCapacity == 0u || (uint64_t{nodeSlot} + 1u) * nodeSlotCapacity > capacities_.nodeCount)
      return false;
    dispatch_.side = side;
    dispatch_.cellCount = cellCount;
    dispatch_.nodeSlot = nodeSlot;
    dispatch_.nodeSlotCapacity = nodeSlotCapacity;
    dispatch_.nodeCapacity = capacities_.nodeCount;
    dispatch_.contentGeneration = contentGeneration;
    if (uniforms_.isValid())
      renderGraph->bufferWrite(uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    return true;
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(dispatch_), &dispatch_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatch(1u, 1u, 1u);
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
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "occupiedCells",
               .binding = 0u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "outputNodes", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "buildResult", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dispatch", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
              {.binding = 0u, .bufferView = view(buffers_.occupiedCells, uint64_t{capacities_.cellCount} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 1u, .bufferView = view(buffers_.nodes, uint64_t{capacities_.nodeCount} * sizeof(SparseVoxelTree64NodeGPU), ::rendering::AccessPattern::SHADER_READ_WRITE)},
              {.binding = 2u, .bufferView = view(buffers_.result, sizeof(SparseVoxelTree64BuildResultGPU), ::rendering::AccessPattern::SHADER_READ_WRITE)},
              {.binding = 3u, .bufferView = view(uniforms_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-tree64-build.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "buildTree64Chunk",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  Capacities capacities_{};
  SparseVoxelTree64BuildDispatchGPU dispatch_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
