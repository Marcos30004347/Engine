#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace voxel::svraster::rendering
{

class SparseVoxelTree64PublishPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer results;
    ::rendering::Buffer nodes;
    ::rendering::Buffer records;
    ::rendering::Buffer targetState;
    ::rendering::Buffer directory;
    ::rendering::Buffer committedState;
  };

  struct Capacities
  {
    uint32_t recordCount = 0u;
    uint32_t resultCount = 0u;
    uint32_t directoryCount = 0u;
    uint32_t nodeSlotCapacity = 0u;
    uint32_t nodeCount = 0u;
  };

  SparseVoxelTree64PublishPass(const Buffers buffers, const Capacities capacities) : buffers_(buffers), capacities_(capacities)
  {
    if (capacities_.recordCount == 0u || capacities_.resultCount == 0u || capacities_.directoryCount == 0u || capacities_.nodeSlotCapacity == 0u || capacities_.nodeCount == 0u)
      throw std::invalid_argument("Tree64 publish capacities are invalid");
    dispatch_.recordCapacity = capacities_.recordCount;
    dispatch_.resultCapacity = capacities_.resultCount;
    dispatch_.directoryCapacity = capacities_.directoryCount;
    dispatch_.nodeSlotCapacity = capacities_.nodeSlotCapacity;
    dispatch_.nodeCapacity = capacities_.nodeCount;
  }

  ~SparseVoxelTree64PublishPass() override
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

  bool update(const uint32_t recordCount)
  {
    if (recordCount > capacities_.recordCount)
      return false;
    dispatch_.recordCount = recordCount;
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
    // Commands are recorded at registration; the uniform guards the active
    // prefix on later updates.
    commandBuffer.cmdDispatch(std::max(1u, (capacities_.recordCount + 63u) / 64u), 1u, 1u);
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
              {.name = "results", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "nodes", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "records", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "targetState", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "directory", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "committedState",
               .binding = 5u,
               .isDynamic = false,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
              {.name = "dispatch", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
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
              {.binding = 0u, .bufferView = view(buffers_.results, uint64_t{capacities_.resultCount} * sizeof(SparseVoxelTree64BuildResultGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 1u, .bufferView = view(buffers_.nodes, uint64_t{capacities_.nodeCount} * sizeof(SparseVoxelTree64NodeGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 2u, .bufferView = view(buffers_.records, uint64_t{capacities_.recordCount} * sizeof(SparseVoxelTree64PublishRecordGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 3u, .bufferView = view(buffers_.targetState, sizeof(SparseVoxelTree64StateGPU), ::rendering::AccessPattern::SHADER_READ)},
              {.binding = 4u, .bufferView = view(buffers_.directory, uint64_t{capacities_.directoryCount} * sizeof(SparseVoxelTree64ChunkGPU), ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 5u, .bufferView = view(buffers_.committedState, sizeof(SparseVoxelTree64StateGPU), ::rendering::AccessPattern::SHADER_WRITE)},
              {.binding = 6u, .bufferView = view(uniforms_, sizeof(dispatch_), ::rendering::AccessPattern::UNIFORM_READ)},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-tree64-publish.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "publishTree64Directory",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  Capacities capacities_{};
  SparseVoxelTree64PublishDispatchGPU dispatch_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
