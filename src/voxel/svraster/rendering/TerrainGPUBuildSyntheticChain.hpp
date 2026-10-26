#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/TerrainGPUBuildScheduler.hpp"
#include "voxel/svraster/rendering/TerrainGPUBuildSyntheticPass.hpp"

#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Phase-2-only fixed graph. Every stage is registered once. update() changes
// only job records and indirect group counts; its output is diagnostic and is
// deliberately not connected to terrain publication buffers.
class TerrainGPUBuildSyntheticChain final
{
public:
  static constexpr uint32_t kStageCount = 10u;

  TerrainGPUBuildSyntheticChain(::rendering::RenderGraph &graph,
      uint32_t &passIndex, const uint32_t stageCapacity,
      std::string name = "TerrainGPUBuild.Phase2")
      : graph_(&graph), stageCapacity_(stageCapacity), name_(std::move(name))
  {
    if (stageCapacity == 0u)
      throw std::invalid_argument(
          "GPU terrain synthetic chain capacity is zero");
    const uint64_t recordCount = uint64_t{stageCapacity_} * kStageCount;
    work_ = graph_->createBuffer({
      .name = name_ + ".Work",
      .size = recordCount * sizeof(TerrainGPUSyntheticWorkGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    results_ = graph_->createBuffer({
      .name = name_ + ".Results",
      .size = recordCount * sizeof(TerrainGPUSyntheticResultGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    indirectArgs_ = graph_->createBuffer({
      .name = name_ + ".IndirectArgs",
      .size = kStageCount * sizeof(IndirectDispatchArgsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    const TerrainGPUBuildSyntheticPass::Buffers buffers{
      work_, results_, indirectArgs_};
    for (uint32_t stage = 0u; stage < kStageCount; ++stage)
      passes_[stage] = graph_->registerPass<TerrainGPUBuildSyntheticPass>(
          name_ + ".Stage" + std::to_string(stage), passIndex++, stage,
          stageCapacity_, kStageCount, buffers);
    update({});
  }

  ~TerrainGPUBuildSyntheticChain()
  {
    for (auto iterator = passes_.rbegin(); iterator != passes_.rend();
         ++iterator)
      delete *iterator;
    graph_->deleteBuffer(indirectArgs_);
    graph_->deleteBuffer(results_);
    graph_->deleteBuffer(work_);
  }

  bool update(const TerrainGPUFrameSchedule &schedule)
  {
    std::array<uint32_t, kStageCount> counts{};
    std::vector<TerrainGPUSyntheticWorkGPU> work(
        uint64_t{stageCapacity_} * kStageCount);
    for (const TerrainGPUDispatch &dispatch : schedule.dispatches)
    {
      const int32_t stage = syntheticStageIndex(dispatch.stage);
      if (stage < 0 || counts[stage] >= stageCapacity_)
        return false;
      const uint32_t slot = static_cast<uint32_t>(stage) * stageCapacity_ +
          counts[stage]++;
      work[slot] = {
        static_cast<uint32_t>(dispatch.jobId),
        static_cast<uint32_t>(dispatch.jobId >> 32u),
        static_cast<uint32_t>(dispatch.stage),
        dispatch.cursor,
        dispatch.workUnits,
        keyHash(dispatch.key),
        static_cast<uint32_t>(dispatch.dispatchId),
        static_cast<uint32_t>(dispatch.dispatchId >> 32u),
      };
    }
    std::array<IndirectDispatchArgsGPU, kStageCount> indirect{};
    for (uint32_t stage = 0u; stage < kStageCount; ++stage)
      indirect[stage] = {(counts[stage] + 63u) / 64u, 1u, 1u, 0u};
    graph_->bufferWrite(work_, 0u,
        work.size() * sizeof(TerrainGPUSyntheticWorkGPU), work.data());
    graph_->bufferWrite(indirectArgs_, 0u, sizeof(indirect), indirect.data());
    activeWorkItems_ = static_cast<uint32_t>(schedule.dispatches.size());
    return true;
  }

  ::rendering::Buffer results() const { return results_; }
  uint32_t stageCapacity() const { return stageCapacity_; }
  uint32_t activeWorkItems() const { return activeWorkItems_; }

private:
  static int32_t syntheticStageIndex(const TerrainGPUBuildStage stage)
  {
    switch (stage)
    {
    case TerrainGPUBuildStage::SampleColumns: return 0;
    case TerrainGPUBuildStage::FinalizeBoundaryStitches: return 1;
    case TerrainGPUBuildStage::CountFineBricks: return 2;
    case TerrainGPUBuildStage::CompactFineBricks: return 3;
    case TerrainGPUBuildStage::EmitFineBricks: return 4;
    case TerrainGPUBuildStage::ReduceBrickLevel: return 5;
    case TerrainGPUBuildStage::EmitBrickLevel: return 6;
    case TerrainGPUBuildStage::BuildTree64Level: return 7;
    case TerrainGPUBuildStage::FinalizeSidecars: return 8;
    case TerrainGPUBuildStage::Validate: return 9;
    default: return -1;
    }
  }

  static uint32_t keyHash(const TerrainGPUGenerationKey &key)
  {
    uint64_t hash = key.bundleSignature ^
        (uint64_t{static_cast<uint32_t>(key.chunkX)} << 32u) ^
        static_cast<uint32_t>(key.chunkZ);
    hash ^= uint64_t{key.internalLOD} << 7u;
    hash ^= uint64_t{key.generatorRevision} << 17u;
    hash ^= uint64_t{key.terrainSlot} << 29u;
    hash ^= uint64_t{key.expectedTerrainGeneration} << 43u;
    hash ^= hash >> 33u;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 33u;
    return static_cast<uint32_t>(hash ^ (hash >> 32u));
  }

  ::rendering::RenderGraph *graph_ = nullptr;
  uint32_t stageCapacity_ = 0u;
  uint32_t activeWorkItems_ = 0u;
  std::string name_{};
  ::rendering::Buffer work_{};
  ::rendering::Buffer results_{};
  ::rendering::Buffer indirectArgs_{};
  std::array<TerrainGPUBuildSyntheticPass *, kStageCount> passes_{};
};

} // namespace voxel::svraster::rendering
