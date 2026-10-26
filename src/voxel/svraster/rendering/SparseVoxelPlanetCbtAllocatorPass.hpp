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

// Phase-2 allocator primitive. Reduction is instantiated once per CBT level so
// the render graph emits a storage barrier between dependent levels. Reset
// derives counts/arguments from the root, and Cache consumes its GPU-generated
// indirect dispatch without a host-visible count.
class SparseVoxelPlanetCbtAllocatorPass final : public ::rendering::Pass
{
public:
  enum class Stage
  {
    Reduce,
    Reset,
    CachePointers,
  };

  struct Buffers
  {
    ::rendering::Buffer heap;
    ::rendering::Buffer pointers;
    ::rendering::Buffer counters;
    ::rendering::Buffer dispatches;
    ::rendering::Buffer drawIndirect;
    ::rendering::Buffer updateSettings;
    // Optional in standalone allocator tests. Production uses this indirect
    // table to turn inactive round reductions into true zero-work dispatches.
    ::rendering::Buffer scheduledDispatches;
  };

  // Offsets make the primitive independently testable with aligned guard
  // regions surrounding every payload. Production buffers use zero offsets.
  struct Offsets
  {
    uint64_t heap = 0u;
    uint64_t pointers = 0u;
    uint64_t counters = 0u;
    uint64_t dispatches = 0u;
    uint64_t drawIndirect = 0u;
  };

  SparseVoxelPlanetCbtAllocatorPass(
      const Stage stage, const Buffers buffers, const uint32_t treeDepth,
      const uint32_t reductionLevel, const Offsets offsets,
      const uint32_t roundIndex = 0u)
      : stage_(stage), buffers_(buffers), offsets_(offsets),
        treeDepth_(treeDepth), reductionLevel_(reductionLevel),
        capacity_(planetCbtPoolCapacity(treeDepth)), roundIndex_(roundIndex)
  {
    if (!buffers_.heap.isValid() || !buffers_.pointers.isValid() || !buffers_.counters.isValid() || !buffers_.dispatches.isValid() || !buffers_.drawIndirect.isValid() || capacity_ == 0u)
      throw std::invalid_argument("Planet CBT allocator buffers or depth are invalid");
    if (stage_ == Stage::Reduce && (reductionLevel_ == 0u || reductionLevel_ > treeDepth_))
      throw std::invalid_argument("Planet CBT reduction level is invalid");
  }

  ~SparseVoxelPlanetCbtAllocatorPass() override
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
    if (fallbackUpdateSettings_.isValid())
      renderGraph->deleteBuffer(fallbackUpdateSettings_);
  }

  void recordCommandBuffer(::rendering::CommandRecorder &commandBuffer) override
  {
    ensureResources();
    renderGraph->bufferWrite(uniforms_, 0u, sizeof(settings_), &settings_);
    commandBuffer.cmdBindComputePipeline(pipeline_);
    commandBuffer.cmdBindBindingGroups(groups_, nullptr, 0u);
    switch (stage_)
    {
    case Stage::Reduce:
    {
      const uint32_t nodeCount = 1u << (reductionLevel_ - 1u);
      if (buffers_.scheduledDispatches.isValid())
        commandBuffer.cmdDispatchIndirect(
            buffers_.scheduledDispatches, scheduledReductionOffset());
      else
        commandBuffer.cmdDispatch(
            (nodeCount + kPlanetCbtWorkgroupSize - 1u) /
                kPlanetCbtWorkgroupSize,
            1u, 1u);
      break;
    }
    case Stage::Reset:
      commandBuffer.cmdDispatch(1u, 1u, 1u);
      break;
    case Stage::CachePointers:
      commandBuffer.cmdDispatchIndirect(buffers_.dispatches, offsets_.dispatches + offsetof(PlanetCbtDispatchesGPU, pointerCacheDispatch));
      break;
    }
  }

private:
  uint64_t scheduledReductionOffset() const
  {
    const uint64_t dispatchIndex =
        uint64_t{roundIndex_} * (treeDepth_ + 2u) + 2u +
        (reductionLevel_ - 1u);
    return dispatchIndex * sizeof(PlanetCbtDispatchIndirectGPU);
  }

  struct alignas(16) Settings
  {
    uint32_t treeDepth = 0u;
    uint32_t reductionLevel = 0u;
    uint32_t capacity = 0u;
    uint32_t workgroupSize = kPlanetCbtWorkgroupSize;
    uint32_t roundIndex = 0u;
    uint32_t reserved0 = 0u;
    uint32_t reserved1 = 0u;
    uint32_t reserved2 = 0u;
  };
  static_assert(sizeof(Settings) == 32u);

  const char *entry() const
  {
    switch (stage_)
    {
    case Stage::Reduce:
      return "reducePlanetCbtLevel";
    case Stage::Reset:
      return "resetPlanetCbtAllocator";
    case Stage::CachePointers:
      return "cachePlanetCbtPointers";
    }
    return "";
  }

  ::rendering::AccessPattern heapAccess() const
  {
    return stage_ == Stage::Reduce ? ::rendering::AccessPattern::SHADER_READ_WRITE : ::rendering::AccessPattern::SHADER_READ;
  }

  ::rendering::AccessPattern pointersAccess() const
  {
    return stage_ == Stage::CachePointers ? ::rendering::AccessPattern::SHADER_WRITE : ::rendering::AccessPattern::NONE;
  }

  ::rendering::AccessPattern countersAccess() const
  {
    if (stage_ == Stage::Reset)
      return ::rendering::AccessPattern::SHADER_WRITE;
    if (stage_ == Stage::CachePointers)
      return ::rendering::AccessPattern::SHADER_READ;
    return ::rendering::AccessPattern::NONE;
  }

  ::rendering::AccessPattern dispatchesAccess() const
  {
    return stage_ == Stage::Reset ? ::rendering::AccessPattern::SHADER_WRITE : ::rendering::AccessPattern::NONE;
  }

  ::rendering::AccessPattern drawAccess() const
  {
    return stage_ == Stage::Reset ? ::rendering::AccessPattern::SHADER_WRITE : ::rendering::AccessPattern::NONE;
  }

  static ::rendering::BufferView view(const ::rendering::Buffer buffer, const uint64_t offset, const uint64_t size, const ::rendering::AccessPattern access)
  {
    return {
      .buffer = buffer,
      .offset = offset,
      .size = std::max<uint64_t>(size, sizeof(uint32_t)),
      .access = access,
    };
  }

  void ensureResources()
  {
    if (layout_.isValid())
      return;
    settings_ = {treeDepth_, reductionLevel_, capacity_,
        kPlanetCbtWorkgroupSize,
        roundIndex_, 0u, 0u, 0u};
    uniforms_ = renderGraph->createBuffer({
      .name = passName + "_Settings",
      .size = sizeof(settings_),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
    });
    ::rendering::Buffer updateSettings = buffers_.updateSettings;
    if (!updateSettings.isValid())
    {
      fallbackUpdateSettings_ = renderGraph->createBuffer({
        .name = passName + "_UpdateSettingsFallback",
        .size = sizeof(PlanetCbtUpdateSettingsGPU),
        .usage = ::rendering::BufferUsage::BufferUsage_Uniform |
            ::rendering::BufferUsage::BufferUsage_Push,
      });
      PlanetCbtUpdateSettingsGPU fallback{};
      // Standalone allocator users do not participate in the production
      // per-round allocation budget. Preserve their full pointer-cache
      // contract while the update chain supplies the tuned budget explicitly.
      fallback.maximumAllocationsPerRound = capacity_;
      renderGraph->bufferWrite(fallbackUpdateSettings_, 0u,
          sizeof(fallback), &fallback);
      updateSettings = fallbackUpdateSettings_;
    }
    layout_ = renderGraph->createBindingsLayout({
      .name = passName + "_Layout",
      .groups = {::rendering::BindingGroupLayout{
        .buffers =
            {
              {.name = "cbtHeap", .binding = 0u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "pointerCache", .binding = 1u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "counters", .binding = 2u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "dispatches", .binding = 3u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "drawIndirect", .binding = 4u, .type = ::rendering::BufferBindingType::BufferBindingType_StorageBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "settings", .binding = 5u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
              {.name = "updateSettings", .binding = 6u, .type = ::rendering::BufferBindingType::BufferBindingType_UniformBuffer, .visibility = ::rendering::BindingVisibility::BindingVisibility_Compute, .isDynamic = false},
            },
      }},
    });
    groups_ = renderGraph->createBindingGroups({
      .name = passName + "_Groups",
      .layout = layout_,
      .groups = {::rendering::GroupInfo{
        .name = "group0",
        .buffers =
            {
              {.bufferView = view(buffers_.heap, offsets_.heap, planetCbtHeapEntryCount(treeDepth_) * sizeof(uint32_t), heapAccess()), .binding = 0u},
              {.bufferView = view(buffers_.pointers, offsets_.pointers, uint64_t{capacity_} * sizeof(PlanetCbtPointerGPU), pointersAccess()), .binding = 1u},
              {.bufferView = view(buffers_.counters, offsets_.counters, sizeof(PlanetCbtCountersGPU), countersAccess()), .binding = 2u},
              {.bufferView = view(buffers_.dispatches, offsets_.dispatches, sizeof(PlanetCbtDispatchesGPU), dispatchesAccess()), .binding = 3u},
              {.bufferView = view(buffers_.drawIndirect, offsets_.drawIndirect, sizeof(PlanetCbtDrawIndirectGPU), drawAccess()), .binding = 4u},
              {.bufferView = view(uniforms_, 0u, sizeof(settings_), ::rendering::AccessPattern::UNIFORM_READ), .binding = 5u},
              {.bufferView = view(updateSettings, 0u, sizeof(PlanetCbtUpdateSettingsGPU), ::rendering::AccessPattern::UNIFORM_READ), .binding = 6u},
            },
      }},
    });
    shader_ = renderGraph->createShader({
      .name = passName + "_Shader",
      .src = os::io::readRelativeFile(
          "assets/shaders/spirv/svraster/"
          "sparse-voxel-planet-cbt-allocator.spirv"),
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

  Stage stage_ = Stage::Reduce;
  Buffers buffers_{};
  Offsets offsets_{};
  uint32_t treeDepth_ = 0u;
  uint32_t reductionLevel_ = 0u;
  uint32_t capacity_ = 0u;
  uint32_t roundIndex_ = 0u;
  Settings settings_{};
  ::rendering::Buffer uniforms_{};
  ::rendering::Buffer fallbackUpdateSettings_{};
  ::rendering::BindingsLayout layout_{};
  ::rendering::BindingGroups groups_{};
  ::rendering::Shader shader_{};
  ::rendering::ComputePipeline pipeline_{};
};

} // namespace voxel::svraster::rendering
