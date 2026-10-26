#pragma once

#include "rendering/gpu/RenderGraph.hpp"
#include "voxel/svraster/PlanetCbtReference.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtAllocatorPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetCbtTopologyPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelPlanetSurfaceAtlas.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace voxel::svraster::rendering
{

// Owns the fixed-capacity concurrent CBT and its complete per-frame Phase-3
// update chain. Counts, pointer lists, reservations, and indirect arguments
// remain entirely GPU-resident.
class SparseVoxelPlanetCbtUpdateChain final
{
public:
  SparseVoxelPlanetCbtUpdateChain(::rendering::RenderGraph &graph, uint32_t &passIndex, const uint32_t treeDepth = kPlanetCbtDefaultDepth, std::string name = "SparseVoxelPlanetCbt")
      : SparseVoxelPlanetCbtUpdateChain(
            graph, passIndex,
            SparseVoxelPlanetSurfaceAtlas::Resources{}, treeDepth,
            std::move(name))
  {
  }

  SparseVoxelPlanetCbtUpdateChain(
      ::rendering::RenderGraph &graph,
      uint32_t &passIndex,
      const SparseVoxelPlanetSurfaceAtlas::Resources surfaceAtlas,
      const uint32_t treeDepth = kPlanetCbtDefaultDepth,
      std::string name = "SparseVoxelPlanetCbt")
      : graph_(&graph), treeDepth_(treeDepth), capacity_(planetCbtPoolCapacity(treeDepth)), name_(std::move(name)), surfaceAtlas_(surfaceAtlas)
  {
    procedural::PlanetSurfaceHalfedgeMesh mesh;
    PlanetCbtReference initial;
    if (capacity_ < kPlanetCbtRootBisectorCount || !buildPlanetCbtIcosahedron(mesh) || !initial.initialize(mesh, treeDepth_))
      throw std::invalid_argument("Planet CBT update chain depth cannot hold its roots");

    pool_ = graph_->createBuffer({
      .name = name_ + ".Pool",
      .size = uint64_t{capacity_} * sizeof(PlanetCbtBisectorGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    heap_ = graph_->createBuffer({
      .name = name_ + ".Heap",
      .size = planetCbtHeapEntryCount(treeDepth_) * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push | ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    pointers_ = graph_->createBuffer({
      .name = name_ + ".Pointers",
      .size = uint64_t{capacity_} * sizeof(PlanetCbtPointerGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    counters_ = graph_->createBuffer({
      .name = name_ + ".Counters",
      .size = sizeof(PlanetCbtCountersGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    dispatches_ = graph_->createBuffer({
      .name = name_ + ".Dispatches",
      .size = sizeof(PlanetCbtDispatchesGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    drawIndirect_ = graph_->createBuffer({
      .name = name_ + ".DrawIndirect",
      .size = sizeof(PlanetCbtDrawIndirectGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage |
          ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push |
          ::rendering::BufferUsage::BufferUsage_CopySrc,
    });
    roots_ = graph_->createBuffer({
      .name = name_ + ".Roots",
      .size = uint64_t{kPlanetCbtRootBisectorCount} * sizeof(PlanetCbtRootTriangleGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
    });
    requests_ = graph_->createBuffer({
      .name = name_ + ".Requests",
      .size = uint64_t{kPlanetCbtMaximumForcedRequests} * sizeof(PlanetCbtRequestGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage | ::rendering::BufferUsage::BufferUsage_Push,
    });
    bitfieldChanges_ = graph_->createBuffer({
      .name = name_ + ".BitfieldChanges",
      .size = uint64_t{capacity_} * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage,
    });
    commandIndices_ = graph_->createBuffer({
      .name = name_ + ".CommandIndices",
      .size = uint64_t{capacity_} * sizeof(uint32_t),
      .usage = ::rendering::BufferUsage::BufferUsage_Storage,
    });
    settings_ = graph_->createBuffer({
      .name = name_ + ".UpdateSettings",
      .size = sizeof(PlanetCbtUpdateSettingsGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Uniform | ::rendering::BufferUsage::BufferUsage_Push,
    });
    scheduledDispatches_ = graph_->createBuffer({
      .name = name_ + ".ScheduledDispatches",
      .size = uint64_t{kPlanetCbtMaximumUpdateRounds} *
          (treeDepth_ + 2u) * sizeof(PlanetCbtDispatchIndirectGPU),
      .usage = ::rendering::BufferUsage::BufferUsage_Indirect |
          ::rendering::BufferUsage::BufferUsage_Push,
    });
    initialPool_ = initial.pool();
    initialHeap_ = initial.heap();
    initialPointers_.resize(kPlanetCbtRootBisectorCount);
    for (uint32_t index = 0u; index < kPlanetCbtRootBisectorCount; ++index)
      initialPointers_[index].activePoolSlot =
          planetCbtOneToBitID(initialHeap_, treeDepth_, index);
    initialCounters_.activeCount = kPlanetCbtRootBisectorCount;
    initialCounters_.freeCount = capacity_ - kPlanetCbtRootBisectorCount;
    const auto dispatch = [](const uint32_t count)
    {
      return PlanetCbtDispatchIndirectGPU{
        (count + kPlanetCbtWorkgroupSize - 1u) /
            kPlanetCbtWorkgroupSize,
        1u, 1u, count};
    };
    initialDispatches_.activeDispatch =
        dispatch(kPlanetCbtRootBisectorCount);
    initialDispatches_.freeDispatch = dispatch(initialCounters_.freeCount);
    initialDispatches_.pointerCacheDispatch =
        dispatch(std::max(initialCounters_.activeCount,
            initialCounters_.freeCount));
    initialDrawIndirect_.vertexCount = kPlanetCbtRootBisectorCount * 3u;
    resetTopology();
    const auto rootTriangles = buildPlanetCbtRootTriangles(mesh);
    if (rootTriangles.size() != kPlanetCbtRootBisectorCount)
      throw std::runtime_error("Planet CBT root triangle generation failed");
    graph_->bufferWrite(roots_, 0u, rootTriangles.size() * sizeof(PlanetCbtRootTriangleGPU), const_cast<PlanetCbtRootTriangleGPU *>(rootTriangles.data()));
    std::vector<PlanetCbtRequestGPU> emptyRequests(kPlanetCbtMaximumForcedRequests);
    graph_->bufferWrite(requests_, 0u, emptyRequests.size() * sizeof(PlanetCbtRequestGPU), emptyRequests.data());
    updateSettings_.maximumDepth = kPlanetCbtMaximumSubdivisionDepth;
    graph_->bufferWrite(settings_, 0u, sizeof(updateSettings_), &updateSettings_);
    uploadScheduledDispatches(updateSettings_.updateRoundCount);

    if (!surfaceAtlas_.heightMinMax.isValid())
    {
      fallbackHeightMinMax_ = graph_->createTexture({
        .name = name_ + ".FallbackHeightMinMax",
        .format = ::rendering::Format::Format_RG16Float,
        .height = 1u,
        .width = 1u,
        .mipLevels = 1u,
        .usage = ::rendering::ImageUsage::ImageUsage_Sampled |
            ::rendering::ImageUsage::ImageUsage_TransferDst,
      });
      std::array<uint16_t, 2> zero{};
      graph_->textureWrite(fallbackHeightMinMax_, sizeof(zero), zero.data());
      surfaceAtlas_.heightMinMax = fallbackHeightMinMax_;
      surfaceAtlas_.mipCount = 1u;
    }

    const SparseVoxelPlanetCbtAllocatorPass::Buffers buffers{
      .heap = heap_,
      .pointers = pointers_,
      .counters = counters_,
      .dispatches = dispatches_,
      .drawIndirect = drawIndirect_,
      .updateSettings = settings_,
      .scheduledDispatches = scheduledDispatches_,
    };
    const SparseVoxelPlanetCbtTopologyPass::Buffers topologyBuffers{
      .pool = pool_,
      .heap = heap_,
      .pointers = pointers_,
      .counters = counters_,
      .dispatches = dispatches_,
      .roots = roots_,
      .requests = requests_,
      .bitfieldChanges = bitfieldChanges_,
      .settings = settings_,
      .scheduledDispatches = scheduledDispatches_,
      .commandIndices = commandIndices_,
      .heightMinMax = surfaceAtlas_.heightMinMax,
      .heightMipCount = surfaceAtlas_.mipCount,
    };
    constexpr std::array topologyStages{
      SparseVoxelPlanetCbtTopologyPass::Stage::ResetCommands,
      SparseVoxelPlanetCbtTopologyPass::Stage::GenerateCommands,
      SparseVoxelPlanetCbtTopologyPass::Stage::ReserveBlocks,
    };
    commandBuffer_.coalescePipelineNodes = true;
    rounds_.reserve(kPlanetCbtMaximumUpdateRounds);
    for (uint32_t roundIndex = 0u;
         roundIndex < kPlanetCbtMaximumUpdateRounds; ++roundIndex)
    {
      RoundPasses round{};
      const std::string prefix =
          name_ + ".Round" + std::to_string(roundIndex);
      round.preReset = graph_->registerPass<SparseVoxelPlanetCbtAllocatorPass>(
          prefix + ".PreReset", passIndex++, commandBuffer_,
          ::rendering::ContinueCommandSequence{},
          SparseVoxelPlanetCbtAllocatorPass::Stage::Reset, buffers,
          treeDepth_, 0u, SparseVoxelPlanetCbtAllocatorPass::Offsets{},
          roundIndex);
      commandBuffer_.cmdMemoryBarrier();
      round.preCache = graph_->registerPass<SparseVoxelPlanetCbtAllocatorPass>(
          prefix + ".PreCachePointers", passIndex++, commandBuffer_,
          ::rendering::ContinueCommandSequence{},
          SparseVoxelPlanetCbtAllocatorPass::Stage::CachePointers, buffers,
          treeDepth_, 0u, SparseVoxelPlanetCbtAllocatorPass::Offsets{},
          roundIndex);
      commandBuffer_.cmdMemoryBarrier();
      round.topology.reserve(topologyStages.size());
      for (const auto stage : topologyStages)
      {
        round.topology.push_back(
            graph_->registerPass<SparseVoxelPlanetCbtTopologyPass>(
                prefix + ".Topology" +
                    std::to_string(round.topology.size()),
                passIndex++, commandBuffer_,
                ::rendering::ContinueCommandSequence{},
                stage, topologyBuffers,
                treeDepth_, roundIndex));
        commandBuffer_.cmdMemoryBarrier();
      }
      round.finalReset =
          graph_->registerPass<SparseVoxelPlanetCbtAllocatorPass>(
              prefix + ".FinalReset", passIndex++, commandBuffer_,
              ::rendering::ContinueCommandSequence{},
              SparseVoxelPlanetCbtAllocatorPass::Stage::Reset, buffers,
              treeDepth_, 1u, SparseVoxelPlanetCbtAllocatorPass::Offsets{},
              roundIndex);
      commandBuffer_.cmdMemoryBarrier();
      round.finalCache =
          graph_->registerPass<SparseVoxelPlanetCbtAllocatorPass>(
              prefix + ".FinalCachePointers", passIndex++, commandBuffer_,
              ::rendering::ContinueCommandSequence{},
              SparseVoxelPlanetCbtAllocatorPass::Stage::CachePointers,
              buffers, treeDepth_, 0u,
              SparseVoxelPlanetCbtAllocatorPass::Offsets{}, roundIndex);
      commandBuffer_.cmdMemoryBarrier();
      rounds_.push_back(std::move(round));
    }
  }

  ~SparseVoxelPlanetCbtUpdateChain()
  {
    for (auto roundIterator = rounds_.rbegin();
         roundIterator != rounds_.rend(); ++roundIterator)
    {
      delete roundIterator->finalCache;
      delete roundIterator->finalReset;
      for (auto iterator = roundIterator->topology.rbegin();
           iterator != roundIterator->topology.rend(); ++iterator)
        delete *iterator;
      delete roundIterator->preCache;
      delete roundIterator->preReset;
    }
    if (fallbackHeightMinMax_.isValid())
      graph_->deleteTexture(fallbackHeightMinMax_);
    graph_->deleteBuffer(scheduledDispatches_);
    graph_->deleteBuffer(settings_);
    graph_->deleteBuffer(commandIndices_);
    graph_->deleteBuffer(bitfieldChanges_);
    graph_->deleteBuffer(requests_);
    graph_->deleteBuffer(roots_);
    graph_->deleteBuffer(drawIndirect_);
    graph_->deleteBuffer(dispatches_);
    graph_->deleteBuffer(counters_);
    graph_->deleteBuffer(pointers_);
    graph_->deleteBuffer(heap_);
    graph_->deleteBuffer(pool_);
  }

  uint32_t treeDepth() const
  {
    return treeDepth_;
  }
  uint32_t capacity() const
  {
    return capacity_;
  }
  ::rendering::Buffer pool() const
  {
    return pool_;
  }
  ::rendering::Buffer heap() const
  {
    return heap_;
  }
  ::rendering::Buffer pointers() const
  {
    return pointers_;
  }
  ::rendering::Buffer counters() const
  {
    return counters_;
  }
  ::rendering::Buffer dispatches() const
  {
    return dispatches_;
  }
  ::rendering::Buffer drawIndirect() const
  {
    return drawIndirect_;
  }

  ::rendering::Buffer roots() const
  {
    return roots_;
  }
  ::rendering::Buffer requests() const
  {
    return requests_;
  }
  ::rendering::Buffer bitfieldChanges() const
  {
    return bitfieldChanges_;
  }
  ::rendering::Buffer settings() const
  {
    return settings_;
  }

  bool update(const PlanetCbtUpdateSettingsGPU &settings, const std::vector<PlanetCbtRequestGPU> &requests = {})
  {
    if (requests.size() > kPlanetCbtMaximumForcedRequests ||
        settings.maximumDepth > kPlanetCbtMaximumSubdivisionDepth ||
        settings.updateRoundCount > kPlanetCbtMaximumUpdateRounds ||
        settings.renderSubdivisionDepth >
            kPlanetCbtMaximumRenderSubdivisionDepth)
      return false;
    updateSettings_ = settings;
    const double sourceRadius = settings.cameraUp[3];
    if (surfaceAtlas_.valid() && sourceRadius > 0.0)
    {
      updateSettings_.heightMapInfo = {
        static_cast<float>(surfaceAtlas_.faceResolution),
        static_cast<float>(surfaceAtlas_.mipCount),
        static_cast<float>(surfaceAtlas_.heightMinimumMetres / sourceRadius),
        static_cast<float>(surfaceAtlas_.heightRangeMetres / sourceRadius),
      };
    }
    else
      updateSettings_.heightMapInfo = {};
    updateSettings_.requestCount = static_cast<uint32_t>(requests.size());
    graph_->bufferWrite(settings_, 0u, sizeof(updateSettings_), &updateSettings_);
    uploadScheduledDispatches(updateSettings_.updateRoundCount);
    if (!requests.empty())
      graph_->bufferWrite(requests_, 0u, requests.size() * sizeof(PlanetCbtRequestGPU), const_cast<PlanetCbtRequestGPU *>(requests.data()));
    return true;
  }

  bool updateLifecycle(
      const PlanetCbtUpdateSettingsGPU &settings,
      const PlanetCbtLifecycleInput &input,
      const std::vector<PlanetCbtRequestGPU> &requests = {})
  {
    lastLifecycleDecision_ = lifecycle_.advance(settings, input);
    if (lastLifecycleDecision_.resetTopology)
      resetTopology();
    PlanetCbtUpdateSettingsGPU scheduled = settings;
    scheduled.updateRoundCount = lastLifecycleDecision_.updateRoundCount;
    scheduled.frameIndex = lastLifecycleDecision_.frameIndex;
    if (!lastLifecycleDecision_.overviewVisible || !input.adaptiveGeometry)
      scheduled.lifecycleFlags |=
          PlanetCbtLifecycleFlag_DisableAdaptiveDraw;
    return update(scheduled, requests);
  }

  const PlanetCbtLifecycleDecision &lastLifecycleDecision() const
  {
    return lastLifecycleDecision_;
  }

  void resetTopology()
  {
    graph_->bufferWrite(pool_, 0u,
        initialPool_.size() * sizeof(PlanetCbtBisectorGPU),
        initialPool_.data());
    graph_->bufferWrite(heap_, 0u,
        initialHeap_.size() * sizeof(uint32_t), initialHeap_.data());
    graph_->bufferWrite(pointers_, 0u,
        initialPointers_.size() * sizeof(PlanetCbtPointerGPU),
        initialPointers_.data());
    graph_->bufferWrite(counters_, 0u, sizeof(initialCounters_),
        &initialCounters_);
    graph_->bufferWrite(dispatches_, 0u, sizeof(initialDispatches_),
        &initialDispatches_);
    graph_->bufferWrite(drawIndirect_, 0u, sizeof(initialDrawIndirect_),
        &initialDrawIndirect_);
  }

  uint64_t storageBytes() const
  {
    return planetCbtStorageBytes(treeDepth_);
  }

private:
  void uploadScheduledDispatches(const uint32_t updateRoundCount)
  {
    const size_t dispatchCount =
        uint64_t{kPlanetCbtMaximumUpdateRounds} * (treeDepth_ + 2u);
    if (scheduledDispatchUpload_.size() != dispatchCount)
      scheduledDispatchUpload_.resize(dispatchCount);
    else
      std::fill(scheduledDispatchUpload_.begin(),
          scheduledDispatchUpload_.end(), PlanetCbtDispatchIndirectGPU{});
    const auto makeDispatch = [](const uint32_t itemCount)
    {
      return PlanetCbtDispatchIndirectGPU{
        (itemCount + kPlanetCbtWorkgroupSize - 1u) /
            kPlanetCbtWorkgroupSize,
        1u, 1u, itemCount};
    };
    for (uint32_t round = 0u; round < kPlanetCbtMaximumUpdateRounds;
         ++round)
    {
      if (round >= updateRoundCount)
        continue;
      const size_t base = static_cast<size_t>(round) * (treeDepth_ + 2u);
      scheduledDispatchUpload_[base] = makeDispatch(capacity_);
      scheduledDispatchUpload_[base + 1u] = {1u, 1u, 1u, 1u};
      // Topology reservation incrementally updates every changed leaf's
      // ancestor path. Reduction slots intentionally remain zero in the
      // production schedule; standalone allocator tests still exercise the
      // complete bottom-up reduction primitive.
    }
    graph_->bufferWrite(scheduledDispatches_, 0u,
        scheduledDispatchUpload_.size() *
            sizeof(PlanetCbtDispatchIndirectGPU),
        scheduledDispatchUpload_.data());
  }

  struct RoundPasses
  {
    SparseVoxelPlanetCbtAllocatorPass *preReset = nullptr;
    SparseVoxelPlanetCbtAllocatorPass *preCache = nullptr;
    std::vector<SparseVoxelPlanetCbtTopologyPass *> topology;
    SparseVoxelPlanetCbtAllocatorPass *finalReset = nullptr;
    SparseVoxelPlanetCbtAllocatorPass *finalCache = nullptr;
  };

  ::rendering::RenderGraph *graph_ = nullptr;
  // All fixed stages share one recorder. This preserves explicit graph nodes
  // and per-stage timestamps while avoiding a command-buffer submission for
  // every tiny dispatch.
  ::rendering::CommandRecorder commandBuffer_{};
  uint32_t treeDepth_ = 0u;
  uint32_t capacity_ = 0u;
  std::string name_;
  ::rendering::Buffer pool_{};
  ::rendering::Buffer heap_{};
  ::rendering::Buffer pointers_{};
  ::rendering::Buffer counters_{};
  ::rendering::Buffer dispatches_{};
  ::rendering::Buffer drawIndirect_{};
  ::rendering::Buffer roots_{};
  ::rendering::Buffer requests_{};
  ::rendering::Buffer bitfieldChanges_{};
  ::rendering::Buffer commandIndices_{};
  ::rendering::Buffer settings_{};
  ::rendering::Buffer scheduledDispatches_{};
  PlanetCbtUpdateSettingsGPU updateSettings_{};
  PlanetCbtLifecycle lifecycle_{};
  PlanetCbtLifecycleDecision lastLifecycleDecision_{};
  std::vector<PlanetCbtBisectorGPU> initialPool_{};
  std::vector<uint32_t> initialHeap_{};
  std::vector<PlanetCbtPointerGPU> initialPointers_{};
  PlanetCbtCountersGPU initialCounters_{};
  PlanetCbtDispatchesGPU initialDispatches_{};
  PlanetCbtDrawIndirectGPU initialDrawIndirect_{};
  SparseVoxelPlanetSurfaceAtlas::Resources surfaceAtlas_{};
  ::rendering::Texture fallbackHeightMinMax_{};
  std::vector<PlanetCbtDispatchIndirectGPU> scheduledDispatchUpload_{};
  std::vector<RoundPasses> rounds_{};
};

} // namespace voxel::svraster::rendering
