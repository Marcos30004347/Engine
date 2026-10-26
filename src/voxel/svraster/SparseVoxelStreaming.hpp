#pragma once

#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelSceneFile.hpp"
#include "voxel/svraster/SparseVoxelTerrainTriangleMesh.hpp"
#include "voxel/svraster/SparseVoxelTree64.hpp"
#include "voxel/svraster/SparseVoxelVirtualTrace.hpp"

#include <array>
#include <cstdint>
#include <future>
#include <limits>
#include <string>
#include <vector>

namespace voxel::svraster
{

enum class SparseVoxelPageState : uint32_t
{
  Unloaded = 0u,
  Requested,
  Reading,
  Decoded,
  UploadQueued,
  Resident,
  EvictPending,
};

inline bool isSparseVoxelPageResident(const SparseVoxelPageState state)
{
  return state == SparseVoxelPageState::Resident;
}

inline bool isSparseVoxelPageLiveOrInFlight(const SparseVoxelPageState state)
{
  return state != SparseVoxelPageState::Unloaded;
}

enum class SparseVoxelPageProducerKind : uint32_t
{
  Unknown = 0u,
  PrebuiltSparseVoxel,
  CPUGenerated,
  GPUGeneratedTerrain,
  GPUGeneratedObject,
};

struct SparseVoxelPageId
{
  // Stable source namespace. Page zero is a valid local page, so producer zero
  // is reserved as the invalid/default namespace.
  uint64_t producer = 0u;
  uint64_t localPage = std::numeric_limits<uint64_t>::max();

  bool valid() const
  {
    return producer != 0u && localPage != std::numeric_limits<uint64_t>::max();
  }

  bool operator==(const SparseVoxelPageId &other) const
  {
    return producer == other.producer && localPage == other.localPage;
  }

  bool operator<(const SparseVoxelPageId &other) const
  {
    return producer < other.producer || (producer == other.producer && localPage < other.localPage);
  }
};

struct SparseVoxelPageProducer
{
  uint64_t producerId = 0u;
  SparseVoxelPageProducerKind kind = SparseVoxelPageProducerKind::Unknown;
  uint32_t producerRevision = 0u;
  bool producesOnCPU = false;
  bool producesOnGPU = false;
  const char *debugName = nullptr;

  bool valid() const
  {
    return producerId != 0u;
  }
};

struct SparseVoxelStreamedInstance
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageProducer producer{};
  SparseVoxelPageId rootPage{};
  uint32_t firstRootNode = kInvalidSparseVoxelIndex;
  uint32_t rootNodeCount = 0u;
  uint32_t firstPage = 0u;
  uint32_t pageCount = 0u;
  std::array<float, 16> modelToWorld{};

  bool valid() const
  {
    return instanceIndex != kInvalidSparseVoxelIndex && producer.valid() && rootPage.valid() && rootNodeCount != 0u;
  }
};

class SparseVoxelSharedPageTable;

struct SparseVoxelInstanceLODSelection
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId selectedPage{};
  SparseVoxelPageId desiredPage{};
  bool rootResident = false;
  bool desiredResident = false;
  bool selectedFallback = false;
  bool requestedDesiredPage = false;
};

bool selectSparseVoxelStreamedInstanceLOD(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelStreamedInstance &instance,
    SparseVoxelPageId desiredPage,
    SparseVoxelInstanceLODSelection &selection,
    std::string *error = nullptr);

struct SparseVoxelUnifiedGeometryCutSettings
{
  bool requireRasterPayload = true;
  bool requireTracePayload = true;
  bool requireMaterialPayload = false;
  bool allowRootFallback = true;
};

struct SparseVoxelUnifiedGeometryLODSelection
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId selectedPage{};
  SparseVoxelPageId desiredPage{};
  bool rootResident = false;
  bool rootRasterVisible = false;
  bool rootTraceable = false;
  bool rootMaterialTraceable = false;
  bool desiredResident = false;
  bool desiredRasterVisible = false;
  bool desiredTraceable = false;
  bool desiredMaterialTraceable = false;
  bool selectedRasterVisible = false;
  bool selectedTraceable = false;
  bool selectedMaterialTraceable = false;
  bool selectedFallback = false;
  bool requestedDesiredPage = false;
  bool missingRasterPayload = false;
  bool missingTracePayload = false;
  bool missingMaterialPayload = false;
};

bool selectSparseVoxelUnifiedGeometryLOD(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelStreamedInstance &instance,
    SparseVoxelPageId desiredPage,
    const SparseVoxelUnifiedGeometryCutSettings &settings,
    SparseVoxelUnifiedGeometryLODSelection &selection,
    std::string *error = nullptr);

struct SparseVoxelBufferRange
{
  uint32_t first = kInvalidSparseVoxelIndex;
  uint32_t count = 0u;

  bool empty() const
  {
    return count == 0u;
  }
  bool valid() const
  {
    return count == 0u || first != kInvalidSparseVoxelIndex;
  }
};

constexpr uint32_t kSparseVoxelPackedPageSlotBytes = 1024u * 1024u;
constexpr uint32_t kSparseVoxelInvalidPackedPageSlot = std::numeric_limits<uint32_t>::max();
constexpr uint32_t kSparseVoxelInvalidPackedPagelet = std::numeric_limits<uint32_t>::max();

struct SparseVoxelPhysicalPageAllocation
{
  SparseVoxelPageId page{};
  uint64_t allocationRevision = 0u;
  // Shared geometry revision for every installed view on this page. Trace,
  // raster/culling and material payloads publish under the same logical page
  // generation and geometry revision; a direct trace view must never name a
  // different revision from the payload it is installed beside.
  uint64_t geometryRevision = 0u;
  // Optional material identity used by exact trace/material resolution. Legacy
  // raster-only pages may leave this as zero, but unified direct-trace pages
  // that expose material ranges publish it with the same revision as geometry.
  uint64_t materialRevision = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  SparseVoxelBufferRange hierarchyNodes{};
  SparseVoxelBufferRange childIndices{};
  SparseVoxelBufferRange brickNodes{};
  SparseVoxelBufferRange brickOccupancy{};
  SparseVoxelBufferRange materials{};
  SparseVoxelBufferRange materialCells{};
  SparseVoxelBufferRange tree64Nodes{};
  SparseVoxelBufferRange tree64Materials{};
  SparseVoxelBufferRange traceNodes{};
  SparseVoxelBufferRange traceErrorWords{};
  SparseVoxelBufferRange cullErrorWords{};
  SparseVoxelBufferRange triangleClusters{};
  SparseVoxelBufferRange triangleVertices{};
  SparseVoxelBufferRange triangleIndices{};
  SparseVoxelBufferRange virtualGeometryMeshlets{};
  SparseVoxelBufferRange virtualGeometryHierarchyNodes{};
  SparseVoxelBufferRange virtualGeometryGroups{};
  SparseVoxelBufferRange virtualGeometryPositionWords{};
  SparseVoxelBufferRange virtualGeometryNormalWords{};
  SparseVoxelBufferRange virtualGeometryUVWords{};
  SparseVoxelBufferRange virtualGeometryIndexBytes{};
  SparseVoxelBufferRange virtualGeometryBoneWeightWords{};
  // Physical upload/streaming placement. A packed slot is a coarse transfer
  // granularity; the logical page remains the residency/cut identity.
  uint32_t packedSlot = kSparseVoxelInvalidPackedPageSlot;
  uint32_t packedPagelet = kSparseVoxelInvalidPackedPagelet;
  uint32_t packedSlotOffsetBytes = 0u;
  uint32_t packedSlotBytes = 0u;

  bool valid() const
  {
    const bool tracePayload = !traceNodes.empty() || !traceErrorWords.empty();
    const bool traceFormatValid = traceFormat == kVoxelTraceFormatCubic64 || traceFormat == kVoxelTraceFormatBounds8 || traceFormat == kVoxelTraceFormatBounds16;
    const bool materialIdentityValid = materialRevision == 0u || geometryRevision == 0u || materialRevision == geometryRevision;
    const bool trianglePayload = !triangleClusters.empty() || !triangleVertices.empty() || !triangleIndices.empty();
    const bool trianglePayloadComplete = !trianglePayload || (!triangleClusters.empty() && !triangleVertices.empty() && !triangleIndices.empty());
    const bool virtualGeometryPayload =
        !virtualGeometryMeshlets.empty() ||
        !virtualGeometryHierarchyNodes.empty() ||
        !virtualGeometryGroups.empty() ||
        !virtualGeometryPositionWords.empty() ||
        !virtualGeometryNormalWords.empty() ||
        !virtualGeometryUVWords.empty() ||
        !virtualGeometryIndexBytes.empty() ||
        !virtualGeometryBoneWeightWords.empty();
    const bool virtualGeometryPayloadComplete =
        !virtualGeometryPayload ||
        (!virtualGeometryMeshlets.empty() &&
         !virtualGeometryHierarchyNodes.empty() &&
         !virtualGeometryGroups.empty() &&
         !virtualGeometryPositionWords.empty() &&
         !virtualGeometryNormalWords.empty() &&
         !virtualGeometryUVWords.empty() &&
         !virtualGeometryIndexBytes.empty());
    return page.valid() && hierarchyNodes.valid() && childIndices.valid() && brickNodes.valid() && brickOccupancy.valid() && materials.valid() && materialCells.valid() && tree64Nodes.valid() && tree64Materials.valid() && traceNodes.valid() && traceErrorWords.valid() && cullErrorWords.valid() && triangleClusters.valid() && triangleVertices.valid() && triangleIndices.valid() && virtualGeometryMeshlets.valid() && virtualGeometryHierarchyNodes.valid() && virtualGeometryGroups.valid() && virtualGeometryPositionWords.valid() && virtualGeometryNormalWords.valid() && virtualGeometryUVWords.valid() && virtualGeometryIndexBytes.valid() && virtualGeometryBoneWeightWords.valid() &&
        (!tracePayload || (traceFormatValid && geometryRevision != 0u && !traceNodes.empty())) &&
        (cullErrorWords.empty() || geometryRevision != 0u) &&
        (!trianglePayload || geometryRevision != 0u) &&
        (!virtualGeometryPayload || geometryRevision != 0u) &&
        trianglePayloadComplete &&
        virtualGeometryPayloadComplete &&
        materialIdentityValid;
  }

  bool hasBrickRasterPayload() const
  {
    return valid() && !hierarchyNodes.empty() && !brickNodes.empty();
  }

  bool hasTriangleRasterPayload() const
  {
    return valid() && !triangleClusters.empty() && !triangleVertices.empty() && !triangleIndices.empty();
  }

  bool requiresTriangleRasterPayload() const
  {
    return triangleClusters.count != 0u && triangleVertices.count != 0u && triangleIndices.count != 0u;
  }

  bool hasVirtualGeometryRasterPayload() const
  {
    return valid() &&
        !virtualGeometryMeshlets.empty() &&
        !virtualGeometryHierarchyNodes.empty() &&
        !virtualGeometryGroups.empty() &&
        !virtualGeometryPositionWords.empty() &&
        !virtualGeometryNormalWords.empty() &&
        !virtualGeometryUVWords.empty() &&
        !virtualGeometryIndexBytes.empty();
  }

  bool requiresVirtualGeometryRasterPayload() const
  {
    return virtualGeometryMeshlets.count != 0u &&
        virtualGeometryHierarchyNodes.count != 0u &&
        virtualGeometryGroups.count != 0u &&
        virtualGeometryPositionWords.count != 0u &&
        virtualGeometryNormalWords.count != 0u &&
        virtualGeometryUVWords.count != 0u &&
        virtualGeometryIndexBytes.count != 0u;
  }

  bool hasRasterPayload() const
  {
    return hasBrickRasterPayload() || hasTriangleRasterPayload() || hasVirtualGeometryRasterPayload();
  }

  bool hasMaterialPayload() const
  {
    return valid() && (!materials.empty() || !materialCells.empty() || !tree64Materials.empty());
  }

  bool hasRenderablePayload() const
  {
    return valid() && (!hierarchyNodes.empty() || !brickNodes.empty() || !tree64Nodes.empty() || !traceNodes.empty() || hasTriangleRasterPayload() || hasVirtualGeometryRasterPayload());
  }

  bool hasTracePayload() const
  {
    return valid() && !traceNodes.empty();
  }

  bool hasCullErrorSidecar() const
  {
    return valid() && !cullErrorWords.empty();
  }
};

uint64_t sparseVoxelPhysicalPagePayloadBytes(const SparseVoxelPhysicalPageAllocation &allocation);

struct SparseVoxelPageInstallRewrite
{
  SparseVoxelPageId page{};
  SparseVoxelPhysicalPageAllocation allocation{};
  SparseVoxelPhysicalPageRewriteGPU rewrite{};
  uint64_t requiredProducerRevision = 0u;
};

struct SparseVoxelPageUninstallRewrite
{
  SparseVoxelPageId page{};
  SparseVoxelPhysicalPageRewriteGPU rewrite{};
  uint64_t retiredAllocationRevision = 0u;
};

enum class SparseVoxelPhysicalBufferKind : uint32_t
{
  HierarchyNodes = 0u,
  ChildIndices,
  BrickNodes,
  BrickOccupancy,
  Materials,
  MaterialCells,
  Tree64Nodes,
  Tree64Materials,
  TraceNodes,
  TraceErrorWords,
  CullErrorWords,
  TriangleClusters,
  TriangleVertices,
  TriangleIndices,
  VirtualGeometryMeshlets,
  VirtualGeometryHierarchyNodes,
  VirtualGeometryGroups,
  VirtualGeometryPositionWords,
  VirtualGeometryNormalWords,
  VirtualGeometryUVWords,
  VirtualGeometryIndexBytes,
  VirtualGeometryBoneWeightWords,
  Count,
};

struct SparseVoxelPhysicalAllocatorStats
{
  uint32_t capacity = 0u;
  uint32_t active = 0u;
  uint32_t retiring = 0u;
  uint32_t largestFreeRange = 0u;
  uint32_t activeRangeCount = 0u;
  uint32_t retiringRangeCount = 0u;
};

class SparseVoxelPhysicalRangeAllocator
{
public:
  bool configure(uint32_t capacity, std::string *error = nullptr);
  bool canAllocate(uint32_t count) const;
  bool allocate(uint32_t count, SparseVoxelBufferRange &range, std::string *error = nullptr);
  bool retire(SparseVoxelBufferRange range, uint64_t completionFence, std::string *error = nullptr);
  uint32_t reclaim(uint64_t completedFence);
  bool containsActive(SparseVoxelBufferRange range) const;
  bool containsRetiring(SparseVoxelBufferRange range) const;
  SparseVoxelPhysicalAllocatorStats stats() const;

private:
  bool fail(std::string *error, const std::string &message) const;
  static bool rangeContains(SparseVoxelBufferRange owner, SparseVoxelBufferRange range);
  void insertFree(SparseVoxelBufferRange range);

  struct RetiringRange
  {
    SparseVoxelBufferRange range{};
    uint64_t completionFence = 0u;
  };

  uint32_t capacity_ = 0u;
  uint32_t active_ = 0u;
  uint32_t retiring_ = 0u;
  uint64_t completedFence_ = 0u;
  std::vector<SparseVoxelBufferRange> freeRanges_{};
  std::vector<SparseVoxelBufferRange> activeRanges_{};
  std::vector<RetiringRange> retiringRanges_{};
};

struct SparseVoxelSharedPageTableSettings
{
  uint32_t hierarchyNodeCapacity = 0u;
  uint32_t childIndexCapacity = 0u;
  uint32_t brickNodeCapacity = 0u;
  uint32_t brickOccupancyCapacity = 0u;
  uint32_t materialCapacity = 0u;
  uint32_t materialCellCapacity = 0u;
  uint32_t tree64NodeCapacity = 0u;
  uint32_t tree64MaterialCapacity = 0u;
  uint32_t traceNodeCapacity = 0u;
  uint32_t traceErrorWordCapacity = 0u;
  uint32_t cullErrorWordCapacity = 0u;
  // Zero disables packed-slot accounting. Non-zero values are aligned to at
  // least 16 bytes; the terrain/virtual-geometry convergence target is 1 MiB.
  uint32_t packedPageSlotBytes = kSparseVoxelPackedPageSlotBytes;
  uint32_t triangleClusterCapacity = 0u;
  uint32_t triangleVertexCapacity = 0u;
  uint32_t triangleIndexCapacity = 0u;
  uint32_t virtualGeometryMeshletCapacity = 0u;
  uint32_t virtualGeometryHierarchyNodeCapacity = 0u;
  uint32_t virtualGeometryGroupCapacity = 0u;
  uint32_t virtualGeometryPositionWordCapacity = 0u;
  uint32_t virtualGeometryNormalWordCapacity = 0u;
  uint32_t virtualGeometryUVWordCapacity = 0u;
  uint32_t virtualGeometryIndexByteCapacity = 0u;
  uint32_t virtualGeometryBoneWeightWordCapacity = 0u;
};

struct SparseVoxelPageTableDirtyRange
{
  uint32_t first = 0u;
  uint32_t count = 0u;

  bool valid() const
  {
    return count != 0u;
  }
};

std::vector<SparseVoxelPageTableDirtyRange> coalesceSparseVoxelPageTableDirtyRanges(
    const std::vector<uint32_t> &indices,
    uint32_t maximumGap = 0u);

struct SparseVoxelPackedPageSlotDebug
{
  uint32_t slot = kSparseVoxelInvalidPackedPageSlot;
  uint32_t slotBytes = 0u;
  uint32_t usedBytes = 0u;
  uint32_t pageletCount = 0u;
  uint32_t livePageletCount = 0u;
  uint32_t residentPageletCount = 0u;
  uint32_t retiringPageletCount = 0u;
};

struct SparseVoxelPackedPageSlotDecision
{
  uint32_t slot = kSparseVoxelInvalidPackedPageSlot;
  bool known = false;
  bool enabled = false;
  bool reusable = false;
  uint32_t slotBytes = 0u;
  uint32_t usedBytes = 0u;
  uint32_t pageletCount = 0u;
  uint32_t livePageletCount = 0u;
  uint32_t residentPageletCount = 0u;
  uint32_t retiringPageletCount = 0u;
  uint32_t liveDependentCount = 0u;
  uint32_t residentDependentCount = 0u;
};

struct SparseVoxelSharedPageTableEntry
{
  SparseVoxelPageId page{};
  SparseVoxelPageProducer producer{};
  SparseVoxelPageState state = SparseVoxelPageState::Unloaded;
  SparseVoxelPhysicalPageAllocation allocation{};
  uint64_t generation = 0u;
  uint64_t geometryRevision = 0u;
  uint64_t materialRevision = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  uint64_t retirementFence = 0u;
  uint32_t dependencyCount = 0u;
  uint32_t residentDependencyCount = 0u;
  uint32_t residentDependentCount = 0u;
  bool pinned = false;

  bool resident() const
  {
    return state == SparseVoxelPageState::Resident;
  }
  bool retiring() const
  {
    return state == SparseVoxelPageState::EvictPending;
  }
  bool traceable() const
  {
    return resident() && allocation.hasTracePayload() && geometryRevision == allocation.geometryRevision && traceFormat == allocation.traceFormat;
  }
  bool rasterVisible() const
  {
    return resident() && allocation.hasRasterPayload();
  }
  bool materialTraceable() const
  {
    return traceable() && allocation.hasMaterialPayload() && materialRevision != 0u && materialRevision == geometryRevision && materialRevision == allocation.materialRevision;
  }
  bool unifiedGeometryVisible() const
  {
    return resident() && rasterVisible() && traceable();
  }
  bool hasCullErrorSidecar() const
  {
    return resident() && allocation.hasCullErrorSidecar() && geometryRevision == allocation.geometryRevision;
  }
};

struct SparseVoxelProducedPagePayload
{
  SparseVoxelPageId page{};
  uint64_t expectedPageGeneration = 0u;
  uint32_t requiredProducerRevision = 0u;
  uint64_t payloadRevision = 0u;
  uint64_t geometryRevision = 0u;
  uint64_t materialRevision = 0u;
  uint64_t productionFence = 0u;
  SparseVoxelPhysicalPageAllocation requirements{};
  uint32_t installRewriteCount = 0u;
  uint32_t uninstallRewriteCount = 0u;
  bool payloadComplete = false;

  bool valid() const
  {
    return page.valid() && payloadRevision != 0u && payloadComplete && (requirements.hierarchyNodes.count != 0u || requirements.brickNodes.count != 0u || requirements.tree64Nodes.count != 0u || requirements.traceNodes.count != 0u || requirements.requiresTriangleRasterPayload() || requirements.requiresVirtualGeometryRasterPayload());
  }
};

struct SparseVoxelReservedPagePayload
{
  SparseVoxelProducedPagePayload produced{};
  SparseVoxelPhysicalPageAllocation allocation{};
  uint64_t copyFence = 0u;
  bool copied = false;

  bool valid() const
  {
    return produced.valid() && allocation.valid() && allocation.hasRenderablePayload();
  }
};

struct SparseVoxelPrebuiltPagePackage
{
  SparseVoxelPageId page{};
  uint64_t expectedPageGeneration = 0u;
  uint32_t requiredProducerRevision = 0u;
  uint64_t payloadRevision = 0u;
  VirtualSparseVoxelPageData pageData{};
  std::vector<VirtualSparseVoxelPageRewrite> installRewrites{};
  std::vector<VirtualSparseVoxelPageRewrite> uninstallRewrites{};
  uint32_t childIndexCount = 0u;
  uint32_t brickOccupancyCount = 0u;
  uint32_t materialCellCount = 0u;
  uint32_t tree64NodeCount = 0u;
  uint32_t tree64MaterialCount = 0u;
  uint32_t traceFormat = kVoxelTraceFormatInvalid;
  uint32_t traceNodeCount = 0u;
  uint32_t traceErrorWordCount = 0u;
  uint32_t cullErrorWordCount = 0u;
  bool decoded = false;

  bool hasRenderablePayload() const
  {
    return !pageData.nodes.empty() || tree64NodeCount != 0u || traceNodeCount != 0u;
  }
};

bool sparseVoxelProducedPagePayloadFromPrebuilt(const SparseVoxelPrebuiltPagePackage &package, SparseVoxelProducedPagePayload &payload, std::string *error = nullptr);

bool sparseVoxelProducedPagePayloadFromTerrainTriangleSidecar(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    SparseVoxelPageId page,
    uint64_t expectedPageGeneration,
    uint32_t requiredProducerRevision,
    uint64_t payloadRevision,
    SparseVoxelProducedPagePayload &payload,
    std::string *error = nullptr);

struct SparseVoxelPageEvictionDecision
{
  SparseVoxelPageId page{};
  SparseVoxelPageState state = SparseVoxelPageState::Unloaded;
  bool pageKnown = false;
  bool resident = false;
  bool pinned = false;
  bool retiring = false;
  bool hasLiveAllocation = false;
  bool evictable = false;
  uint32_t dependencyCount = 0u;
  uint32_t residentDependencyCount = 0u;
  uint32_t residentDependentCount = 0u;
  uint32_t liveDependentCount = 0u;
};

enum class SparseVoxelPageConsumerKind : uint32_t
{
  Raster = 0u,
  Trace = 1u,
};

struct SparseVoxelPageConsumerReference
{
  SparseVoxelPageId page{};
  uint64_t generation = 0u;
  SparseVoxelPageConsumerKind kind = SparseVoxelPageConsumerKind::Raster;
};

struct SparseVoxelPageConsumerReferenceSummary
{
  SparseVoxelPageId page{};
  bool pageKnown = false;
  bool resident = false;
  bool baseEvictable = false;
  bool evictable = false;
  uint32_t rasterConsumerCount = 0u;
  uint32_t traceConsumerCount = 0u;
  uint32_t staleConsumerCount = 0u;
};

SparseVoxelPageConsumerReferenceSummary summarizeSparseVoxelPageConsumerReferences(
    const SparseVoxelSharedPageTable &table,
    SparseVoxelPageId page,
    const std::vector<SparseVoxelPageConsumerReference> &references);

bool canEvictSparseVoxelPageWithConsumers(
    const SparseVoxelSharedPageTable &table,
    SparseVoxelPageId page,
    const std::vector<SparseVoxelPageConsumerReference> &references,
    SparseVoxelPageConsumerReferenceSummary *summary = nullptr,
    std::string *error = nullptr);

class SparseVoxelSharedPageTable
{
public:
  bool configure(const SparseVoxelSharedPageTableSettings &settings, std::string *error = nullptr);
  bool addPage(SparseVoxelPageId page, SparseVoxelPageProducer producer, const std::vector<SparseVoxelPageId> &dependencies = {}, bool pinned = false, std::string *error = nullptr);
  bool allocatePage(SparseVoxelPageId page, const SparseVoxelPhysicalPageAllocation &requirements, SparseVoxelPhysicalPageAllocation &allocation, std::string *error = nullptr);
  bool installPage(SparseVoxelPageId page, const SparseVoxelPhysicalPageAllocation &allocation, const std::vector<SparseVoxelPageInstallRewrite> &rewrites, uint64_t generation, std::string *error = nullptr);
  bool uninstallPage(SparseVoxelPageId page, const std::vector<SparseVoxelPageUninstallRewrite> &rewrites, uint64_t retirementFence, std::string *error = nullptr);
  bool reserveProducedPagePayload(const SparseVoxelProducedPagePayload &payload, SparseVoxelReservedPagePayload &reservation, std::string *error = nullptr);
  bool markPayloadCopied(SparseVoxelReservedPagePayload &reservation, uint64_t copyFence, std::string *error = nullptr) const;
  bool installCopiedPagePayload(const SparseVoxelReservedPagePayload &reservation, const std::vector<SparseVoxelPageInstallRewrite> &rewrites, std::string *error = nullptr);
  SparseVoxelPageEvictionDecision evictionDecision(SparseVoxelPageId page) const;
  bool canEvictPage(SparseVoxelPageId page, SparseVoxelPageEvictionDecision *decision = nullptr, std::string *error = nullptr) const;
  uint32_t reclaim(uint64_t completedFence);
  bool canReusePackedPageSlot(uint32_t slot, SparseVoxelPackedPageSlotDecision *decision = nullptr, std::string *error = nullptr) const;
  bool collectPageRequestClosure(SparseVoxelPageId page, std::vector<SparseVoxelPageId> &pages, std::string *error = nullptr) const;

  const SparseVoxelSharedPageTableEntry *entry(SparseVoxelPageId page) const;
  std::vector<SparseVoxelSharedPageTableEntry> debugReadback() const;
  std::vector<SparseVoxelPackedPageSlotDebug> packedPageSlots() const;
  SparseVoxelPhysicalAllocatorStats allocatorStats(SparseVoxelPhysicalBufferKind kind) const;
  const std::vector<SparseVoxelPageInstallRewrite> &installRewrites() const
  {
    return installRewrites_;
  }
  const std::vector<SparseVoxelPageUninstallRewrite> &uninstallRewrites() const
  {
    return uninstallRewrites_;
  }
  const std::vector<SparseVoxelPageTableDirtyRange> &dirtyPageTableRanges() const
  {
    return dirtyPageTableRanges_;
  }
  void clearDirtyPageTableRanges();

private:
  bool fail(std::string *error, const std::string &message) const;
  SparseVoxelSharedPageTableEntry *mutableEntry(SparseVoxelPageId page);
  const std::vector<SparseVoxelPageId> *dependenciesFor(SparseVoxelPageId page) const;
  const std::vector<SparseVoxelPageId> *dependentsFor(SparseVoxelPageId page) const;
  SparseVoxelPhysicalRangeAllocator &allocator(SparseVoxelPhysicalBufferKind kind);
  const SparseVoxelPhysicalRangeAllocator &allocator(SparseVoxelPhysicalBufferKind kind) const;
  bool validateAllocationActive(const SparseVoxelPhysicalPageAllocation &allocation, std::string *error) const;
  bool validateRewriteActive(const SparseVoxelPhysicalPageRewriteGPU &rewrite, std::string *error) const;
  bool retireAllocation(const SparseVoxelPhysicalPageAllocation &allocation, uint64_t retirementFence, std::string *error);
  bool assignPackedPageSlot(SparseVoxelPhysicalPageAllocation &allocation, uint64_t payloadBytes, std::string *error);
  SparseVoxelPackedPageSlotDecision packedPageSlotDecision(uint32_t slot) const;
  void markPageTableDirty(uint32_t index);

  bool configured_ = false;
  uint64_t completedFence_ = 0u;
  uint64_t nextAllocationRevision_ = 1u;
  uint32_t packedPageSlotBytes_ = kSparseVoxelPackedPageSlotBytes;
  struct PackedPageSlot
  {
    uint32_t usedBytes = 0u;
    std::vector<SparseVoxelPageId> pagelets{};
  };
  std::vector<PackedPageSlot> packedPageSlots_{};
  std::vector<SparseVoxelSharedPageTableEntry> entries_{};
  std::vector<std::vector<SparseVoxelPageId>> dependencies_{};
  std::vector<std::vector<SparseVoxelPageId>> dependents_{};
  SparseVoxelPhysicalRangeAllocator allocators_[static_cast<uint32_t>(SparseVoxelPhysicalBufferKind::Count)]{};
  std::vector<SparseVoxelPageInstallRewrite> installRewrites_{};
  std::vector<SparseVoxelPageUninstallRewrite> uninstallRewrites_{};
  std::vector<SparseVoxelPageTableDirtyRange> dirtyPageTableRanges_{};
};

enum class SparseVoxelMissingRegionCategory : uint32_t
{
  None = 0u,
  CutSelection,
  HZBOrFrustum,
  RasterProxyCoverage,
  Tree64Lighting,
  PageResidency,
};

struct SparseVoxelCutDiagnostics
{
  uint64_t pageInstallCount = 0u;
  uint64_t pageUninstallCount = 0u;
  uint64_t parentFallbackCount = 0u;
  uint64_t childSelectionCount = 0u;
  uint64_t invalidGenerationFallbackCount = 0u;
  uint64_t hzbRejectionCount = 0u;
  uint64_t frustumRejectionCount = 0u;
  uint64_t rasterProxyCoverageMissCount = 0u;
  uint64_t tree64LightingMissCount = 0u;
  uint64_t pageResidencyMissCount = 0u;
  uint64_t selectedNeitherParentNorChildCount = 0u;
};

SparseVoxelMissingRegionCategory classifySparseVoxelMissingRegion(const SparseVoxelCutDiagnostics &diagnostics);
SparseVoxelCutDiagnostics sparseVoxelCutDiagnosticsFromFrameCounters(const SparseVoxelFrameCountersGPU &counters);

// Larger words have higher admission priority. The representation is shared
// by the CPU oracle and GPU atomicMax reduction: visible, projected error, then
// inverse distance. Page level and page ID are deterministic CPU tie breakers.
uint32_t quantizeSparseVoxelPagePriority(bool visible, float projectedErrorPixels, float distance);

struct SparseVoxelPagePriorityView
{
  std::array<float, 16> modelToWorld{};
  std::array<float, 16> worldToCamera{};
  std::array<float, 2> focalPixels{};
  std::array<float, 3> cameraWorldPosition{};
  float nearPlane = 0.001f;
};

uint32_t scoreSparseVoxelPagePriority(const VirtualSparseVoxelFilePageRecord &page, const SparseVoxelPagePriorityView &view, bool visible = true);

struct SparseVoxelTraversalEmission
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId page{};
  bool fallback = false;
};

struct SparseVoxelTraversalPageRequest
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId page{};
  uint32_t priority = 0u;
  bool visible = false;
};

struct SparseVoxelTraversalCandidate
{
  SparseVoxelStreamedInstance instance{};
  SparseVoxelPageId desiredPage{};
  VirtualSparseVoxelFilePageRecord desiredPageBounds{};
  bool visible = true;
};

struct SparseVoxelTraversalFrameResult
{
  std::vector<SparseVoxelTraversalEmission> emissions{};
  std::vector<SparseVoxelTraversalPageRequest> pageRequests{};
  SparseVoxelCutDiagnostics diagnostics{};
};

struct SparseVoxelUnifiedGeometryTraversalCounters
{
  uint32_t inputCandidateCount = 0u;
  uint32_t selectedChildCount = 0u;
  uint32_t selectedFallbackCount = 0u;
  uint32_t requestedPageCount = 0u;
  uint32_t missingRasterPageCount = 0u;
  uint32_t missingTracePageCount = 0u;
  uint32_t missingMaterialPageCount = 0u;
  uint32_t brickFallbackPageCount = 0u;
  uint32_t triangleRasterPageCount = 0u;
  uint32_t tracePageCount = 0u;
  uint32_t protectedParentPageCount = 0u;
  uint32_t offscreenRequestCount = 0u;
  uint64_t triangleSidecarBytes = 0u;
};

struct SparseVoxelUnifiedGeometryTraversalResult
{
  std::vector<SparseVoxelUnifiedGeometryLODSelection> selections{};
  std::vector<SparseVoxelTraversalPageRequest> pageRequests{};
  SparseVoxelUnifiedGeometryTraversalCounters counters{};
};

bool traverseSparseVoxelUnifiedGeometryLOD(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const SparseVoxelPagePriorityView &view,
    const SparseVoxelUnifiedGeometryCutSettings &settings,
    SparseVoxelUnifiedGeometryTraversalResult &result,
    std::string *error = nullptr);

bool traverseSparseVoxelStreamedInstancesLOD(
    const SparseVoxelSharedPageTable &table,
    const std::vector<SparseVoxelTraversalCandidate> &candidates,
    const SparseVoxelPagePriorityView &view,
    SparseVoxelTraversalFrameResult &result,
    std::string *error = nullptr);

enum class SparseVoxelTraceRayClass : uint32_t
{
  Unknown = 0u,
  Shadow = 1u,
  GlobalIllumination = 2u,
  Reflection = 3u,
  Primary = 4u,
  RasterFeedback = 5u,
  StabilityFeedback = 6u,
};

enum class SparseVoxelTraceLODMode : uint32_t
{
  ConservativeVisibility = 0u,
  Exact = 1u,
};

constexpr uint32_t kSparseVoxelTraceRequestOffscreen = 1u << 0u;
constexpr uint32_t kSparseVoxelTraceRequestVisibleReceiver = 1u << 1u;
constexpr uint32_t kSparseVoxelTraceRequestConservativeFallback = 1u << 2u;
constexpr uint32_t kSparseVoxelTraceRequestExactUnresolved = 1u << 3u;
constexpr uint32_t kSparseVoxelTraceRequestRasterFeedback = 1u << 4u;
constexpr uint32_t kSparseVoxelTraceRequestStabilityFeedback = 1u << 5u;
constexpr uint32_t kSparseVoxelTraceReceiverValid = 1u << 0u;
constexpr uint32_t kSparseVoxelTraceReceiverSelectedFallback = 1u << 1u;

// Bounded GPU-facing request ABI for pages discovered by ray traversal. Page
// IDs are split into 32-bit halves so the layout mirrors WGSL storage buffers
// without relying on native u64 support in early shader prototypes.
struct alignas(16) SparseVoxelTracePageRequestGPU
{
  uint32_t producerIdLo = 0u;
  uint32_t producerIdHi = 0u;
  uint32_t pageIdLo = 0u;
  uint32_t pageIdHi = 0u;

  uint32_t desiredLOD = 0u;
  uint32_t rayClass = 0u;
  uint32_t receiverPageIdLo = 0u;
  uint32_t receiverPageIdHi = 0u;

  uint32_t receiverGenerationLo = 0u;
  uint32_t receiverGenerationHi = 0u;
  uint32_t flags = 0u;
  uint32_t priority = 0u;

  float desiredError = 0.0f;
  float rayConeWidth = 0.0f;
  float minimumT = 0.0f;
  float maximumT = 0.0f;
};

static_assert(sizeof(SparseVoxelTracePageRequestGPU) == 64u);
static_assert(alignof(SparseVoxelTracePageRequestGPU) == 16u);

// G-buffer/receiver metadata consumed by scene-ray creation. It records the
// exact shared-page-table identity selected by the screen cut, so trace
// traversal can reject stale receivers and use the same page generation for
// self-shadow and exact-ray policy.
struct alignas(16) SparseVoxelTraceReceiverGPU
{
  uint32_t producerIdLo = 0u;
  uint32_t producerIdHi = 0u;
  uint32_t selectedPageIdLo = 0u;
  uint32_t selectedPageIdHi = 0u;

  uint32_t selectedGenerationLo = 0u;
  uint32_t selectedGenerationHi = 0u;
  uint32_t selectedLOD = 0u;
  uint32_t flags = 0u;
};

static_assert(sizeof(SparseVoxelTraceReceiverGPU) == 32u);
static_assert(alignof(SparseVoxelTraceReceiverGPU) == 16u);

struct SparseVoxelTracePageRequestKey
{
  SparseVoxelPageId page{};
  uint32_t desiredLOD = 0u;
  SparseVoxelTraceRayClass rayClass = SparseVoxelTraceRayClass::Unknown;

  bool operator==(const SparseVoxelTracePageRequestKey &other) const
  {
    return page == other.page && desiredLOD == other.desiredLOD && rayClass == other.rayClass;
  }
};

struct SparseVoxelTraceRequestQueueStats
{
  uint32_t capacity = 0u;
  uint32_t emitted = 0u;
  uint32_t accepted = 0u;
  uint32_t deduplicated = 0u;
  uint32_t overflowed = 0u;
  uint32_t rasterRequests = 0u;
  uint32_t traceRequests = 0u;
  uint32_t stabilityRequests = 0u;
  uint32_t offscreenRequests = 0u;
  uint32_t fallbackTraceCount = 0u;
  uint32_t unresolvedExactCount = 0u;
};

enum class SparseVoxelTraceRequestQueueStatus : uint32_t
{
  Accepted = 0u,
  Deduplicated = 1u,
  ReplacedLowerPriority = 2u,
  DroppedOverflow = 3u,
  Invalid = 4u,
};

class SparseVoxelTraceRequestQueue
{
public:
  void configure(uint32_t capacity);
  void clear();
  SparseVoxelTraceRequestQueueStatus emit(const SparseVoxelTracePageRequestGPU &request, std::string *error = nullptr);
  std::vector<SparseVoxelTracePageRequestGPU> sortedRequests() const;

  uint32_t capacity() const
  {
    return capacity_;
  }
  uint32_t size() const
  {
    return static_cast<uint32_t>(requests_.size());
  }
  bool overflowed() const
  {
    return stats_.overflowed != 0u;
  }
  const std::vector<SparseVoxelTracePageRequestGPU> &requests() const
  {
    return requests_;
  }
  SparseVoxelTraceRequestQueueStats stats() const
  {
    SparseVoxelTraceRequestQueueStats result = stats_;
    result.capacity = capacity_;
    return result;
  }

private:
  static bool validRequest(const SparseVoxelTracePageRequestGPU &request);
  static SparseVoxelTracePageRequestKey keyFor(const SparseVoxelTracePageRequestGPU &request);
  static bool lessPriority(const SparseVoxelTracePageRequestGPU &left, const SparseVoxelTracePageRequestGPU &right);
  void recordEmission(const SparseVoxelTracePageRequestGPU &request);

  uint32_t capacity_ = 0u;
  std::vector<SparseVoxelTracePageRequestGPU> requests_{};
  SparseVoxelTraceRequestQueueStats stats_{};
};

struct SparseVoxelTraceFeedbackInput
{
  SparseVoxelStreamedInstance instance{};
  SparseVoxelPageId desiredPage{};
  SparseVoxelPageId receiverPage{};
  uint64_t receiverGeneration = 0u;
  uint32_t desiredLOD = 0u;
  SparseVoxelTraceRayClass rayClass = SparseVoxelTraceRayClass::Unknown;
  SparseVoxelTraceLODMode mode = SparseVoxelTraceLODMode::ConservativeVisibility;
  uint32_t priority = 0u;
  float desiredError = 0.0f;
  float rayConeWidth = 0.0f;
  float minimumT = 0.0f;
  float maximumT = 0.0f;
  bool offscreen = false;
  bool allowCoarseExactFallback = false;
  bool requireDirectTraceView = false;
};

struct SparseVoxelTraceFeedbackResult
{
  uint32_t instanceIndex = kInvalidSparseVoxelIndex;
  SparseVoxelPageId desiredPage{};
  SparseVoxelPageId selectedPage{};
  bool desiredResident = false;
  bool desiredTraceable = false;
  bool selectedConservativeFallback = false;
  bool emittedRequest = false;
  bool requestOverflowed = false;
  bool unresolvedExact = false;
};

struct SparseVoxelTraceCutDiagnostics
{
  SparseVoxelPageId screenSelectedPage{};
  uint64_t screenSelectedGeneration = 0u;
  uint32_t screenSelectedLOD = 0u;
  SparseVoxelPageId traceDesiredPage{};
  SparseVoxelPageId traceSelectedPage{};
  SparseVoxelPageId traceFallbackPage{};
  SparseVoxelPageId traceRequestPage{};
  uint64_t receiverGeneration = 0u;
  bool receiverValid = false;
  bool receiverGenerationMatches = false;
  bool receiverFallback = false;
  bool traceOffscreen = false;
  bool traceConservativeFallback = false;
  bool traceExactUnresolved = false;
  bool traceRequestEmitted = false;
  bool traceRequestOverflowed = false;
};

SparseVoxelTracePageRequestGPU makeSparseVoxelTracePageRequestGPU(
    SparseVoxelPageId page,
    uint32_t desiredLOD,
    SparseVoxelTraceRayClass rayClass,
    SparseVoxelPageId receiverPage,
    uint64_t receiverGeneration,
    uint32_t flags,
    uint32_t priority,
    float desiredError,
    float rayConeWidth,
    float minimumT,
    float maximumT);

SparseVoxelTraceReceiverGPU makeSparseVoxelTraceReceiverGPU(
    SparseVoxelPageId selectedPage,
    uint64_t selectedGeneration,
    uint32_t selectedLOD,
    bool selectedFallback = false);

SparseVoxelPageId sparseVoxelTraceRequestPageId(const SparseVoxelTracePageRequestGPU &request);
SparseVoxelPageId sparseVoxelTraceRequestReceiverPageId(const SparseVoxelTracePageRequestGPU &request, uint64_t producer);
uint64_t sparseVoxelTraceRequestReceiverGeneration(const SparseVoxelTracePageRequestGPU &request);
SparseVoxelPageId sparseVoxelTraceReceiverPageId(const SparseVoxelTraceReceiverGPU &receiver);
uint64_t sparseVoxelTraceReceiverGeneration(const SparseVoxelTraceReceiverGPU &receiver);

bool processSparseVoxelTraceFeedback(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTraceFeedbackInput &input,
    SparseVoxelTraceRequestQueue &requests,
    SparseVoxelTraceFeedbackResult &result,
    std::string *error = nullptr);

bool evaluateSparseVoxelTraceCutConsistency(
    const SparseVoxelSharedPageTable &table,
    const SparseVoxelTraceReceiverGPU &receiver,
    const SparseVoxelTraceFeedbackInput &input,
    SparseVoxelTraceRequestQueue &requests,
    SparseVoxelTraceCutDiagnostics &diagnostics,
    std::string *error = nullptr);

struct SparseVoxelStreamingBudgets
{
  uint32_t maxPages = ~uint32_t{0};
  uint32_t maxNodes = ~uint32_t{0};
  uint64_t maxStoredBytes = ~uint64_t{0};
};

struct SparseVoxelPageRuntimeState
{
  SparseVoxelPageState state = SparseVoxelPageState::Unloaded;
  uint32_t physicalFirstNode = kInvalidSparseVoxelIndex;
  uint32_t priority = 0u;
  uint32_t requestingInstance = kInvalidSparseVoxelIndex;
  uint64_t lastRequestedFrame = 0u;
  bool pinned = false;
};

struct SparseVoxelPageInstallBatch
{
  uint64_t generation = 0u;
  uint32_t pageIndex = kInvalidSparseVoxelIndex;
  uint32_t physicalFirstNode = kInvalidSparseVoxelIndex;
  std::vector<SparseVoxelLODNodeGPU> nodes;
  std::vector<SparseVoxelPathLevelGPU> pathsLevels;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelMaterialGPU> materials;
  std::vector<SparseVoxelResidentNodeMetadataGPU> metadata;
  std::vector<SparseVoxelPhysicalPageRewriteGPU> publishRewrites;
};

struct SparseVoxelPageEvictionBatch
{
  uint64_t generation = 0u;
  uint32_t pageIndex = kInvalidSparseVoxelIndex;
  uint32_t physicalFirstNode = kInvalidSparseVoxelIndex;
  uint32_t nodeCount = 0u;
  std::vector<SparseVoxelPhysicalPageRewriteGPU> unpublishRewrites;
};

// Deterministic CPU residency oracle and runtime scheduler. It owns the
// physical-node mapping and mirrors published GPU state, but leaves file IO and
// command submission to the caller so delayed, failed and asynchronous reads
// cannot accidentally expose a partial hierarchy cut.
class SparseVoxelPageResidency
{
public:
  bool initialize(const VirtualSparseVoxelSceneMetadata &metadata, uint32_t physicalNodeCapacity, std::string *error = nullptr);

  bool requestPage(uint32_t pageIndex, uint32_t priority, uint32_t instanceIndex, uint64_t frameIndex, std::string *error = nullptr);
  std::vector<uint32_t> planAdmissions(const SparseVoxelStreamingBudgets &budgets);
  bool markReading(uint32_t pageIndex, std::string *error = nullptr);
  bool markDecoded(uint32_t pageIndex, std::string *error = nullptr);
  bool failIO(uint32_t pageIndex, bool retry, std::string *error = nullptr);

  bool prepareInstall(const VirtualSparseVoxelPageData &page, SparseVoxelPageInstallBatch &batch, std::string *error = nullptr);
  bool commitInstall(const SparseVoxelPageInstallBatch &batch, std::string *error = nullptr);
  bool cancelInstall(const SparseVoxelPageInstallBatch &batch, std::string *error = nullptr);

  std::vector<uint32_t> planEvictions(uint32_t nodesToFree, const std::vector<uint32_t> &protectedPages = {}) const;
  bool prepareEviction(uint32_t pageIndex, SparseVoxelPageEvictionBatch &batch, std::string *error = nullptr);
  bool commitEviction(const SparseVoxelPageEvictionBatch &batch, std::string *error = nullptr);

  uint64_t generation() const
  {
    return generation_;
  }
  uint64_t contentRevision() const
  {
    return contentRevision_;
  }
  uint32_t physicalNodeCapacity() const
  {
    return physicalNodeCapacity_;
  }
  uint32_t residentNodeCount() const
  {
    return residentNodeCount_;
  }
  uint32_t freeNodeCount() const
  {
    return physicalNodeCapacity_ - residentNodeCount_ - pendingNodeCount_;
  }
  uint32_t largestFreeNodeRange() const;
  uint32_t logicalToPhysical(uint32_t logicalNodeIndex) const;
  uint32_t refinementPageForLogicalNode(uint32_t logicalNodeIndex) const;
  // Returns the stable page IDs installed or evicted after revision. This lets
  // derived acceleration structures invalidate page bounds instead of every
  // instance root whenever the resident cut changes.
  std::vector<uint32_t> changedPagesSince(uint64_t revision) const;
  void resetDiagnostics()
  {
    diagnostics_ = {};
  }
  SparseVoxelCutDiagnostics diagnostics() const
  {
    return diagnostics_;
  }
  bool diagnoseCutCoverage(uint32_t modelIndex, SparseVoxelCutDiagnostics &diagnostics, std::string *error = nullptr) const;
  const std::vector<SparseVoxelPageRuntimeState> &pages() const
  {
    return pageStates_;
  }
  const std::vector<SparseVoxelLODNodeGPU> &physicalNodes() const
  {
    return physicalNodes_;
  }
  const std::vector<SparseVoxelResidentNodeMetadataGPU> &physicalMetadata() const
  {
    return physicalMetadata_;
  }
  const std::vector<SparseVoxelPathLevelGPU> &physicalPathsLevels() const
  {
    return physicalPathsLevels_;
  }
  const std::vector<SparseVoxelCenterLengthGPU> &physicalCentersLengths() const
  {
    return physicalCentersLengths_;
  }
  const std::vector<SparseVoxelMaterialGPU> &physicalMaterials() const
  {
    return physicalMaterials_;
  }

private:
  struct FreeRange
  {
    uint32_t first = 0u;
    uint32_t count = 0u;
  };

  bool fail(std::string *error, const std::string &message) const;
  bool validPage(uint32_t pageIndex, std::string *error) const;
  bool dependenciesResident(uint32_t pageIndex) const;
  bool hasResidentDependent(uint32_t pageIndex) const;
  uint32_t allocate(uint32_t count);
  void release(uint32_t first, uint32_t count);
  void applyRewrite(const SparseVoxelPhysicalPageRewriteGPU &rewrite);

  VirtualSparseVoxelSceneMetadata metadata_{};
  bool initialized_ = false;
  uint64_t generation_ = 0u;
  uint64_t contentRevision_ = 0u;
  uint32_t physicalNodeCapacity_ = 0u;
  uint32_t residentNodeCount_ = 0u;
  uint32_t pendingNodeCount_ = 0u;
  std::vector<SparseVoxelPageRuntimeState> pageStates_;
  std::vector<uint64_t> pageContentRevisions_;
  std::vector<std::vector<uint32_t>> dependents_;
  std::vector<uint32_t> refinementPages_;
  std::vector<uint32_t> logicalToPhysical_;
  std::vector<FreeRange> freeRanges_;
  std::vector<SparseVoxelLODNodeGPU> physicalNodes_;
  std::vector<SparseVoxelPathLevelGPU> physicalPathsLevels_;
  std::vector<SparseVoxelCenterLengthGPU> physicalCentersLengths_;
  std::vector<SparseVoxelMaterialGPU> physicalMaterials_;
  std::vector<SparseVoxelResidentNodeMetadataGPU> physicalMetadata_;
  SparseVoxelCutDiagnostics diagnostics_{};
};

using SparseVoxelResidencyManager = SparseVoxelPageResidency;

struct SparseVoxelPageReadCompletion
{
  uint64_t generation = 0u;
  uint32_t pageIndex = kInvalidSparseVoxelIndex;
  bool success = false;
  bool stale = false;
  VirtualSparseVoxelPageData page;
  std::string error;
};

// Bounded asynchronous transport for independently compressed VSVO pages.
// Cancellation is generation based: an in-flight filesystem read may finish,
// but a completion from an old scene is marked stale and carries no payload.
class SparseVoxelAsyncPageReader
{
public:
  SparseVoxelAsyncPageReader() = default;
  SparseVoxelAsyncPageReader(const SparseVoxelAsyncPageReader &) = delete;
  SparseVoxelAsyncPageReader &operator=(const SparseVoxelAsyncPageReader &) = delete;
  SparseVoxelAsyncPageReader(SparseVoxelAsyncPageReader &&) = delete;
  SparseVoxelAsyncPageReader &operator=(SparseVoxelAsyncPageReader &&) = delete;
  ~SparseVoxelAsyncPageReader();
  bool initialize(std::string path, VirtualSparseVoxelSceneMetadata metadata, uint64_t generation, uint32_t maxInFlight, std::string *error = nullptr);
  bool enqueue(uint32_t pageIndex, std::string *error = nullptr);
  std::vector<SparseVoxelPageReadCompletion> poll();
  void cancelAndAdvanceGeneration();
  uint64_t generation() const
  {
    return generation_;
  }
  uint32_t inFlightCount() const
  {
    return static_cast<uint32_t>(pending_.size());
  }

private:
  struct PendingRead
  {
    uint64_t generation = 0u;
    uint32_t pageIndex = kInvalidSparseVoxelIndex;
    std::future<SparseVoxelPageReadCompletion> future;
  };

  std::string path_;
  VirtualSparseVoxelSceneMetadata metadata_{};
  uint64_t generation_ = 0u;
  uint32_t maxInFlight_ = 0u;
  std::vector<PendingRead> pending_;
};

} // namespace voxel::svraster
