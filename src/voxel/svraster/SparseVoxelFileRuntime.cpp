#include "voxel/svraster/SparseVoxelFileRuntime.hpp"

#include <algorithm>
#include <limits>

namespace voxel::svraster
{

bool SparseVoxelFileRuntime::fail(std::string *error, const std::string &message) const
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool SparseVoxelFileRuntime::openMetadata(
    const std::string &path,
    const SparseVoxelFileRuntimeOptions &options,
    std::string *error)
{
  if (options.maxInFlightReads == 0u || options.maxAdmissionsPerUpdate == 0u || options.maxStoredBytesPerUpdate == 0u)
    return fail(error, "VSVO runtime queue and admission budgets must be non-zero");

  VirtualSparseVoxelSceneMetadata metadata;
  if (!readVirtualSparseVoxelSceneMetadata(path, metadata, error))
    return false;
  const uint32_t capacity = options.physicalNodeCapacity == 0u ? metadata.nodeCount : options.physicalNodeCapacity;
  uint64_t rootNodes = 0u;
  for (const auto &page : metadata.pages)
    if ((page.flags & kVirtualSparseVoxelPageRoot) != 0u)
      rootNodes += page.nodeCount;
  if (rootNodes > capacity)
    return fail(error, "VSVO physical node budget cannot hold all pinned root pages");

  SparseVoxelPageResidency residency;
  if (!residency.initialize(metadata, capacity, error))
    return false;
  auto reader = std::make_unique<SparseVoxelAsyncPageReader>();
  if (!reader->initialize(path, metadata, residency.generation(), options.maxInFlightReads, error))
    return false;

  // Commit only after the complete metadata/runtime setup succeeds.
  if (reader_)
    reader_->cancelAndAdvanceGeneration();
  path_ = path;
  options_ = options;
  metadata_ = std::move(metadata);
  residency_ = std::move(residency);
  reader_ = std::move(reader);
  open_ = true;
  if (options_.loadAllPages && !requestAllPages(0u, error))
    return false;
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelFileRuntime::requestPage(
    const uint32_t pageIndex,
    const uint32_t priority,
    const uint32_t instanceIndex,
    const uint64_t frameIndex,
    std::string *error)
{
  if (!open_)
    return fail(error, "VSVO runtime is not open");
  return residency_.requestPage(pageIndex, priority, instanceIndex, frameIndex, error);
}

bool SparseVoxelFileRuntime::requestPages(
    const std::vector<uint32_t> &requestBits,
    const std::vector<uint32_t> &priorities,
    const std::vector<uint32_t> &instances,
    const uint64_t frameIndex,
    std::string *error)
{
  if (!open_ || requestBits.size() != (metadata_.pageCount + 31u) / 32u || priorities.size() != metadata_.pageCount || instances.size() != metadata_.pageCount)
    return fail(error, "VSVO GPU page-request arrays do not match the open metadata");
  for (uint32_t pageIndex = 0u; pageIndex < metadata_.pageCount; ++pageIndex)
  {
    if ((requestBits[pageIndex >> 5u] & (1u << (pageIndex & 31u))) == 0u)
      continue;
    if (!residency_.requestPage(pageIndex, priorities[pageIndex], instances[pageIndex], frameIndex, error))
      return false;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelFileRuntime::requestAllPages(const uint64_t frameIndex, std::string *error)
{
  if (!open_)
    return fail(error, "VSVO runtime is not open");
  for (uint32_t pageIndex = 0u; pageIndex < metadata_.pageCount; ++pageIndex)
  {
    const uint32_t levelPriority = std::numeric_limits<uint32_t>::max() - metadata_.pages[pageIndex].maxLevel;
    if (!residency_.requestPage(pageIndex, levelPriority, kInvalidSparseVoxelIndex, frameIndex, error))
      return false;
  }
  return true;
}

bool SparseVoxelFileRuntime::update(SparseVoxelFileRuntimeUpdate &update, std::string *error)
{
  update = {};
  if (!open_ || !reader_)
    return fail(error, "VSVO runtime is not open");

  std::string firstReadFailure;
  for (SparseVoxelPageReadCompletion &completion : reader_->poll())
  {
    ++update.completedReads;
    if (!completion.success || completion.stale)
    {
      ++update.failedReads;
      std::string ignored;
      residency_.failIO(completion.pageIndex, false, &ignored);
      if (firstReadFailure.empty())
        firstReadFailure = "VSVO page " + std::to_string(completion.pageIndex) + " failed: " + completion.error;
      continue;
    }
    if (!residency_.markDecoded(completion.pageIndex, error))
      return false;
    SparseVoxelPageInstallBatch install;
    if (!residency_.prepareInstall(completion.page, install, error))
      return false;
    if (!residency_.commitInstall(install, error))
      return false;
    update.installs.push_back(std::move(install));
  }

  const uint32_t availableReads = options_.maxInFlightReads - std::min(options_.maxInFlightReads, reader_->inFlightCount());
  const SparseVoxelStreamingBudgets budgets{
    std::min(options_.maxAdmissionsPerUpdate, availableReads),
    residency_.freeNodeCount(),
    options_.maxStoredBytesPerUpdate,
  };
  for (const uint32_t pageIndex : residency_.planAdmissions(budgets))
  {
    if (!residency_.markReading(pageIndex, error))
      return false;
    if (!reader_->enqueue(pageIndex, error))
    {
      std::string ignored;
      residency_.failIO(pageIndex, true, &ignored);
      return false;
    }
  }
  if (!firstReadFailure.empty())
    return fail(error, firstReadFailure);
  if (error != nullptr)
    error->clear();
  return true;
}

bool SparseVoxelFileRuntime::rootsResident() const
{
  if (!open_)
    return false;
  for (uint32_t pageIndex = 0u; pageIndex < metadata_.pageCount; ++pageIndex)
    if ((metadata_.pages[pageIndex].flags & kVirtualSparseVoxelPageRoot) != 0u && residency_.pages()[pageIndex].state != SparseVoxelPageState::Resident)
      return false;
  return true;
}

bool SparseVoxelFileRuntime::fullyResident() const
{
  return open_ && std::all_of(residency_.pages().begin(), residency_.pages().end(), [](const SparseVoxelPageRuntimeState &page) {
           return page.state == SparseVoxelPageState::Resident;
         });
}

bool SparseVoxelFileRuntime::makeRootScene(SparseVoxelScene &scene, std::string *error) const
{
  if (!rootsResident())
    return fail(error, "VSVO root scene is unavailable until every pinned root page is resident");
  SparseVoxelScene next{};
  next.tlasFanout = metadata_.tlasFanout;
  next.tlasLeafSize = metadata_.tlasLeafSize;
  next.sourceInfo = metadata_.sourceInfo;
  next.sourceContentHash = metadata_.sourceContentHash;
  next.buildSettingsHash = metadata_.buildSettingsHash;
  next.models.reserve(metadata_.models.size());
  for (const auto &record : metadata_.models)
  {
    SparseVoxelBLAS model{};
    model.rootCenter = record.rootCenter;
    model.rootExtent = record.rootExtent;
    model.maxLevel = record.maxLevel;
    model.flags = record.flags;
    model.firstRoot = 0u;
    model.rootCount = record.rootCount;
    for (uint32_t rootOffset = 0u; rootOffset < record.rootCount; ++rootOffset)
    {
      const uint32_t logical = record.firstRoot + rootOffset;
      const uint32_t physical = residency_.logicalToPhysical(logical);
      if (physical == kInvalidSparseVoxelIndex)
        return fail(error, "VSVO resident root mapping is incomplete");
      SparseVoxelLODNodeGPU node = residency_.physicalNodes()[physical];
      node.firstChild = 0u;
      node.childCount = 0u;
      node.voxelIndex = rootOffset;
      model.hierarchyNodes.push_back(node);
      const auto &pathLevel = residency_.physicalPathsLevels()[physical];
      model.octreePaths.push_back(static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u));
      model.octreeLevels.push_back(static_cast<uint8_t>(pathLevel.level));
      model.centersLengths.push_back(residency_.physicalCentersLengths()[physical]);
      model.materials.push_back(residency_.physicalMaterials()[physical]);
    }
    next.models.push_back(std::move(model));
  }
  next.instances.reserve(metadata_.instances.size());
  for (const auto &instance : metadata_.instances)
    next.instances.push_back({instance.blasIndex, instance.flags, instance.modelToWorld});
  if (!validateSparseVoxelSceneSource(next, error))
    return false;

  // Root pages are allocated in file order (level, then model), matching the
  // scene manager's flattened per-model root ranges. Lock that ABI explicitly.
  uint32_t expectedPhysical = 0u;
  for (const auto &record : metadata_.models)
  {
    if (residency_.logicalToPhysical(record.firstRoot) != expectedPhysical)
      return fail(error, "VSVO root physical order does not match TLAS BLAS ranges");
    expectedPhysical += record.rootCount;
  }
  scene = std::move(next);
  if (error != nullptr)
    error->clear();
  return true;
}

} // namespace voxel::svraster
