#pragma once

#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

#include <algorithm>
#include <array>

namespace voxel::svraster::rendering
{

class SparseVoxelSceneRayStatisticsPass final : public ::rendering::Pass
{
public:
  struct Buffers
  {
    ::rendering::Buffer rays;
    ::rendering::Buffer screenHits;
    ::rendering::Buffer mergedHits;
    ::rendering::Buffer reservoirs;
    ::rendering::Buffer lights;
    ::rendering::Buffer activeRayIndices;
    ::rendering::Buffer activeRayState;
    ::rendering::Buffer treeHits;
    ::rendering::Buffer canonicalToCompact;
    ::rendering::Buffer statistics;
  };

  SparseVoxelSceneRayStatisticsPass(
      Buffers buffers,
      const uint32_t rayCapacity,
      const uint32_t lightCapacity,
      const bool screenTraceEnabled,
      const bool reservoirEnabled)
      : buffers_(buffers),
        rayCapacity_(std::max(1u, rayCapacity)),
        lightCapacity_(std::max(1u, lightCapacity)),
        screenTraceEnabled_(screenTraceEnabled),
        reservoirEnabled_(reservoirEnabled)
  {
  }

  ~SparseVoxelSceneRayStatisticsPass() override
  {
    if (pipeline_.isValid()) renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid()) renderGraph->deleteShader(shader_);
    if (groups_.isValid()) renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid()) renderGraph->deleteBindingsLayout(layout_);
    if (uniforms_.isValid()) renderGraph->deleteBuffer(uniforms_);
    if (ownedScreenHits_.isValid()) renderGraph->deleteBuffer(ownedScreenHits_);
    if (ownedReservoirs_.isValid()) renderGraph->deleteBuffer(ownedReservoirs_);
    if (ownedLights_.isValid()) renderGraph->deleteBuffer(ownedLights_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    const std::array<uint32_t, 4u> settings{
      rayCapacity_,
      screenTraceEnabled_ ? 1u : 0u,
      reservoirEnabled_ ? 1u : 0u,
      lightCapacity_,
    };
    renderGraph->bufferWrite(
        uniforms_, 0u, sizeof(settings), const_cast<uint32_t *>(settings.data()));
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    commandBuffer.cmdDispatchIndirect(buffers_.activeRayState, 0u);
  }

private:
  static ::rendering::BufferView view(
      const ::rendering::Buffer buffer,
      const uint64_t bytes,
      const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = 0u,
      .size = std::max<uint64_t>(bytes, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid()) return;
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Settings",
      .size = 16u,
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
      .frameLocal = false,
    });
    if (!buffers_.screenHits.isValid())
    {
      ownedScreenHits_ = renderGraph->createBuffer({
        .name = passName + "_EmptyScreenHits",
        .size = sizeof(SparseVoxelSceneRayHitGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      SparseVoxelSceneRayHitGPU hit{};
      hit.status = static_cast<uint32_t>(SparseVoxelSceneRayStatus::Pending);
      renderGraph->bufferWrite(ownedScreenHits_, 0u, sizeof(hit), &hit);
      buffers_.screenHits = ownedScreenHits_;
    }
    if (!buffers_.reservoirs.isValid())
    {
      ownedReservoirs_ = renderGraph->createBuffer({
        .name = passName + "_EmptyReservoirs",
        .size = sizeof(SparseVoxelDIReservoirGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      SparseVoxelDIReservoirGPU reservoir{};
      renderGraph->bufferWrite(
          ownedReservoirs_, 0u, sizeof(reservoir), &reservoir);
      buffers_.reservoirs = ownedReservoirs_;
    }
    if (!buffers_.lights.isValid())
    {
      ownedLights_ = renderGraph->createBuffer({
        .name = passName + "_EmptyLights",
        .size = sizeof(globalillumination::worldgi::WorldGILightGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Storage |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      globalillumination::worldgi::WorldGILightGPU light{};
      renderGraph->bufferWrite(ownedLights_, 0u, sizeof(light), &light);
      buffers_.lights = ownedLights_;
    }
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{.buffers = {
        {.name = "settings", .binding = 0u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "rays", .binding = 1u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "screenHits", .binding = 2u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "mergedHits", .binding = 3u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "reservoirs", .binding = 4u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "lights", .binding = 5u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "activeRayIndices", .binding = 6u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "activeRayState", .binding = 7u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "statistics", .binding = 8u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "treeHits", .binding = 9u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
        {.name = "canonicalToCompact", .binding = 10u, .isDynamic = false, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute},
      }}},
    });
    const uint64_t rayBytes =
        uint64_t{rayCapacity_} * sizeof(SparseVoxelSceneRayGPU);
    const uint64_t hitBytes =
        uint64_t{rayCapacity_} * sizeof(SparseVoxelSceneRayHitGPU);
    groups_ = renderGraph->createBindingGroups({
      .layout = layout_,
      .name = passName + "_Groups",
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers = {
          {.binding = 0u, .bufferView = view(uniforms_, 16u, ::rendering::AccessPattern::UNIFORM_READ)},
          {.binding = 1u, .bufferView = view(buffers_.rays, rayBytes, ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 2u, .bufferView = view(buffers_.screenHits, screenTraceEnabled_ ? hitBytes : sizeof(SparseVoxelSceneRayHitGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 3u, .bufferView = view(buffers_.mergedHits, hitBytes, ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 4u, .bufferView = view(buffers_.reservoirs, reservoirEnabled_ ? uint64_t{rayCapacity_} * sizeof(SparseVoxelDIReservoirGPU) : sizeof(SparseVoxelDIReservoirGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 5u, .bufferView = view(buffers_.lights, uint64_t{lightCapacity_} * sizeof(globalillumination::worldgi::WorldGILightGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 6u, .bufferView = view(buffers_.activeRayIndices, uint64_t{rayCapacity_} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 7u, .bufferView = view(buffers_.activeRayState, 16u, ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 8u, .bufferView = view(buffers_.statistics, sizeof(SparseVoxelSceneRayStatisticsGPU), ::rendering::AccessPattern::SHADER_READ | ::rendering::AccessPattern::SHADER_WRITE)},
          {.binding = 9u, .bufferView = view(buffers_.treeHits, uint64_t{rayCapacity_} * sizeof(SparseVoxelOccupancyDDAResultGPU), ::rendering::AccessPattern::SHADER_READ)},
          {.binding = 10u, .bufferView = view(buffers_.canonicalToCompact, uint64_t{rayCapacity_} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ)},
        },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .layout = layout_,
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/sparse-voxel-scene-ray-statistics.spirv"),
      .type = ::rendering::ShaderType::SpirV,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .entry = "collectSparseVoxelSceneRayStatistics",
      .layout = layout_,
      .name = passName + "_Pipeline",
      .shader = shader_,
    });
  }

  Buffers buffers_{};
  uint32_t rayCapacity_ = 1u;
  uint32_t lightCapacity_ = 1u;
  bool screenTraceEnabled_ = false;
  bool reservoirEnabled_ = false;
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer ownedScreenHits_{};
  ::rendering::Buffer ownedReservoirs_{};
  ::rendering::Buffer ownedLights_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
