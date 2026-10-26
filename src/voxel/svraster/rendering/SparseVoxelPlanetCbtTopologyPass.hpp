#pragma once

#include "os/File.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/PlanetCbt.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace voxel::svraster::rendering
{

// One primitive for the six ordered Phase-3 topology stages. Each instance
// owns a distinct pipeline so RenderGraph can infer storage dependencies and
// insert a barrier between command generation and the serialized transaction.
class SparseVoxelPlanetCbtTopologyPass final : public ::rendering::Pass
{
public:
  enum class Stage
  {
    ResetCommands,
    GenerateCommands,
    ReserveBlocks,
    FillNewBlocks,
    UpdateNeighbors,
    UpdateBitfield,
  };

  struct Buffers
  {
    ::rendering::Buffer pool;
    ::rendering::Buffer heap;
    ::rendering::Buffer pointers;
    ::rendering::Buffer counters;
    ::rendering::Buffer dispatches;
    ::rendering::Buffer roots;
    ::rendering::Buffer requests;
    ::rendering::Buffer bitfieldChanges;
    ::rendering::Buffer settings;
    ::rendering::Buffer scheduledDispatches;
    ::rendering::Buffer commandIndices;
    ::rendering::Texture heightMinMax;
    uint32_t heightMipCount = 1u;
  };

  SparseVoxelPlanetCbtTopologyPass(
      const Stage stage,
      const Buffers buffers,
      const uint32_t treeDepth,
      const uint32_t roundIndex = 0u)
      : stage_(stage), buffers_(buffers), treeDepth_(treeDepth),
        capacity_(planetCbtPoolCapacity(treeDepth)),
        roundIndex_(roundIndex),
        roundSettings_{roundIndex, 0u, 0u, 0u}
  {
    if (capacity_ == 0u || !buffers_.pool.isValid() || !buffers_.heap.isValid() || !buffers_.pointers.isValid() || !buffers_.counters.isValid() || !buffers_.dispatches.isValid() || !buffers_.roots.isValid() ||
        !buffers_.requests.isValid() || !buffers_.bitfieldChanges.isValid() ||
        !buffers_.settings.isValid() ||
        !buffers_.scheduledDispatches.isValid() ||
        !buffers_.commandIndices.isValid() ||
        !buffers_.heightMinMax.isValid() ||
        buffers_.heightMipCount == 0u)
      throw std::invalid_argument("Planet CBT topology buffers or depth are invalid");
  }

  ~SparseVoxelPlanetCbtTopologyPass() override
  {
    if (pipeline_.isValid())
      renderGraph->deleteComputePipeline(pipeline_);
    if (shader_.isValid())
      renderGraph->deleteShader(shader_);
    if (groups_.isValid())
      renderGraph->deleteBindingGroups(groups_);
    if (layout_.isValid())
      renderGraph->deleteBindingsLayout(layout_);
    if (roundSettingsBuffer_.isValid())
      renderGraph->deleteBuffer(roundSettingsBuffer_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commands) override
  {
    ensureResources();
    commands.cmdBindComputePipeline(pipeline_);
    commands.cmdBindBindingGroups(groups_, nullptr, 0u);
    switch (stage_)
    {
    case Stage::ResetCommands:
    case Stage::GenerateCommands:
      commands.cmdDispatchIndirect(buffers_.dispatches, offsetof(PlanetCbtDispatchesGPU, activeDispatch));
      break;
    case Stage::ReserveBlocks:
    case Stage::UpdateNeighbors:
      commands.cmdDispatchIndirect(
          buffers_.scheduledDispatches, scheduledDispatchOffset(1u));
      break;
    case Stage::FillNewBlocks:
    case Stage::UpdateBitfield:
      // Reservation applies heap leaf changes directly. Keep these legacy
      // graph nodes for ABI/test continuity, but schedule no GPU work.
      commands.cmdDispatch(0u, 1u, 1u);
      break;
    }
  }

private:
  uint64_t scheduledDispatchOffset(const uint32_t localIndex) const
  {
    return (uint64_t{roundIndex_} * (treeDepth_ + 2u) + localIndex) *
        sizeof(PlanetCbtDispatchIndirectGPU);
  }

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::ResetCommands:
      return "resetPlanetCbtCommands";
    case Stage::GenerateCommands:
      return "generatePlanetCbtCommands";
    case Stage::ReserveBlocks:
      return "reservePlanetCbtBlocks";
    case Stage::FillNewBlocks:
      return "fillPlanetCbtBlocks";
    case Stage::UpdateNeighbors:
      return "repairPlanetCbtNeighbors";
    case Stage::UpdateBitfield:
      return "updatePlanetCbtBitfield";
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

  ::rendering::AccessPattern poolAccess() const
  {
    return ::rendering::AccessPattern::SHADER_READ_WRITE;
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    roundSettingsBuffer_ = renderGraph->createBuffer({
      .name = passName + "_RoundSettings",
      .size = sizeof(roundSettings_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    renderGraph->bufferWrite(roundSettingsBuffer_, 0u,
        sizeof(roundSettings_), &roundSettings_);
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "bisectors", .binding = 0u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "cbtHeap", .binding = 1u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "pointerCache", .binding = 2u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "counters", .binding = 3u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "dispatches", .binding = 4u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "roots", .binding = 5u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "requests", .binding = 6u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "bitfieldChanges",
               .binding = 7u,
               .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer,
               .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute,
               .isDynamic = false},
              {.name = "settings", .binding = 8u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "roundSettings", .binding = 10u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "commandIndices", .binding = 11u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
            },
        .textures = {{.name = "heightMinMax", .binding = 9u,
            .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute}},
      }},
    });
    groups_ = renderGraph
                  ->createBindingGroups(
                      {
                        .name = passName + "_Groups",
                        .layout = layout_,
                        .groups =
                            {::rendering::GroupInfo{
                              .name = "group0",
                              .buffers =
                                  {
                                    {.bufferView = view(buffers_.pool, uint64_t{capacity_} * sizeof(PlanetCbtBisectorGPU), poolAccess()), .binding = 0u},
                                    {.bufferView = view(buffers_.heap, planetCbtHeapEntryCount(treeDepth_) * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 1u},
                                    {.bufferView = view(buffers_.pointers, uint64_t{capacity_} * sizeof(PlanetCbtPointerGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 2u},
                                    {.bufferView = view(buffers_.counters, sizeof(PlanetCbtCountersGPU), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 3u},
                                    {.bufferView = view(buffers_.dispatches, sizeof(PlanetCbtDispatchesGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 4u},
                                    {.bufferView = view(buffers_.roots, uint64_t{kPlanetCbtRootBisectorCount} * sizeof(PlanetCbtRootTriangleGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 5u},
                                    {.bufferView = view(buffers_.requests, uint64_t{kPlanetCbtMaximumForcedRequests} * sizeof(PlanetCbtRequestGPU), ::rendering::AccessPattern::SHADER_READ), .binding = 6u},
                                    {.bufferView = view(buffers_.bitfieldChanges, uint64_t{capacity_} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 7u},
                                    {.bufferView = view(buffers_.settings, sizeof(PlanetCbtUpdateSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ), .binding = 8u},
                                    {.bufferView = view(roundSettingsBuffer_, sizeof(roundSettings_), ::rendering::AccessPattern::UNIFORM_READ), .binding = 10u},
                                    {.bufferView = view(buffers_.commandIndices, uint64_t{capacity_} * sizeof(uint32_t), ::rendering::AccessPattern::SHADER_READ_WRITE), .binding = 11u},
                                  },
                              .textures = {{.binding = 9u,
                                  .textureView = {
                                    .texture = buffers_.heightMinMax,
                                    .access = ::rendering::AccessPattern::SHADER_READ,
                                    .layout = ::rendering::ResourceLayout::SHADER_READ_ONLY,
                                    .index = 0u,
                                    .flags = ::rendering::ImageAspectFlags::Color,
                                    .baseArrayLayer = 0u,
                                    .baseMipLevel = 0u,
                                    .layerCount = 1u,
                                    .levelCount = buffers_.heightMipCount,
                                  }}},
                            }},
                      });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile("assets/shaders/spirv/svraster/sparse-voxel-planet-cbt-update.spirv"),
      .type = ::rendering::ShaderType::SpirV,
      .layout = layout_,
    });
    pipeline_ = renderGraph->createComputePipeline({
      .name = passName + "_Pipeline",
      .shader = shader_,
      .entry = entry(),
      .layout = layout_,
    });
  }

  Stage stage_ = Stage::ResetCommands;
  Buffers buffers_{};
  uint32_t treeDepth_ = 0u;
  uint32_t capacity_ = 0u;
  uint32_t roundIndex_ = 0u;
  struct alignas(16) RoundSettings
  {
    uint32_t roundIndex = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
    uint32_t reserved2 = 0u;
  };
  static_assert(sizeof(RoundSettings) == 16u);
  RoundSettings roundSettings_{};
  ::rendering::Buffer roundSettingsBuffer_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
