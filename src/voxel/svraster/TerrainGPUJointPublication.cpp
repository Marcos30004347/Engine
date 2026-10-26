#include "voxel/svraster/TerrainGPUJointPublication.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace voxel::svraster
{
namespace
{
bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool rangeFits(const uint32_t offset, const uint32_t capacity, const uint32_t total)
{
  return offset <= total && capacity <= total - offset;
}
} // namespace

bool validateTerrainGPUJointPublicationRequest(const TerrainGPUJointPublicationRequestGPU &request, const TerrainGPUJointPublicationCapacities &capacities, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (capacities.requests == 0u || capacities.rasterResults == 0u || capacities.rasterNodes == 0u || capacities.rasterChildren == 0u || capacities.rasterMaterials == 0u || capacities.rasterRoots == 0u ||
      capacities.tree64Results == 0u || capacities.tree64Nodes == 0u)
    return fail(error, "GPU terrain publication capacities are zero");
  if (request.rasterResultIndex >= capacities.rasterResults || request.tree64ResultIndex >= capacities.tree64Results || request.pageFlags == 0u || request.internalLOD > 31u || request.canonicalTranslationScale[3] <= 0 ||
      request.tree64NodesPerPage == 0u || request.nodePageGeneration == 0u || request.materialPageGeneration == 0u || request.nodeCapacity == 0u || request.childCapacity == 0u || request.rootCapacity == 0u ||
      request.materialCapacity == 0u || request.paletteCapacity == 0u || (request.materialOffset & 3u) != 0u || (request.rasterMaterialSourceOffset & 3u) != 0u ||
      (request.finerClipPacked != 0u &&
       (request.pageSide == 0u || request.pageSide > 255u ||
        !(request.translation[3] > 0.0f))) ||
      request.rasterNodeSourceOffset >= capacities.rasterNodes || request.rasterChildSourceOffset >= capacities.rasterChildren || request.rasterRootSourceOffset >= capacities.rasterRoots ||
      request.rasterMaterialSourceOffset >= capacities.rasterMaterials || request.tree64NodeSourceOffset >= capacities.tree64Nodes)
    return fail(error, "GPU terrain publication request is invalid");
  for (const float value : request.translation)
    if (!std::isfinite(value))
      return fail(error, "GPU terrain publication translation is invalid");
  if (!rangeFits(request.nodeOffset, request.nodeCapacity, capacities.destinationNodes) || !rangeFits(request.childOffset, request.childCapacity, capacities.destinationChildren) ||
      !rangeFits(request.rootOffset, request.rootCapacity, capacities.destinationRoots) || !rangeFits(request.materialOffset, request.materialCapacity, capacities.destinationMaterials))
    return fail(error, "GPU terrain publication raster allocation is invalid");
  if (uint64_t{request.materialCellOffset} + request.materialCellCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "GPU terrain publication material page overflows");
  if (uint64_t{request.nodePageSlot} * request.tree64NodesPerPage + request.tree64NodesPerPage > capacities.destinationTree64Nodes)
    return fail(error, "GPU terrain publication Tree64 page is invalid");
  if (request.terrainSlot >= capacities.destinationTree64Instances)
    return fail(error, "GPU terrain publication Tree64 instance is invalid");
  return true;
}

bool TerrainGPUJointPublicationRetirementQueue::begin(const uint64_t publicationId, const TerrainGPUPhysicalPageLease replacement, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (publicationId == 0u || !replacement.valid() ||
      std::any_of(
          pending_.begin(),
          pending_.end(),
          [&](const Pending &entry)
          {
            return entry.id == publicationId;
          }))
    return fail(error, "GPU terrain publication lease is invalid or duplicate");
  pending_.push_back({publicationId, replacement});
  return true;
}

bool TerrainGPUJointPublicationRetirementQueue::complete(const uint64_t publicationId, const bool accepted, const TerrainGPUPhysicalPageLease previous, const uint64_t completionFence, std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto found = std::find_if(
      pending_.begin(),
      pending_.end(),
      [&](const Pending &entry)
      {
        return entry.id == publicationId;
      });
  if (found == pending_.end() || completionFence < completedFence_)
    return fail(error, "GPU terrain publication completion is invalid");
  const auto retired = accepted ? previous : found->replacement;
  if (retired.valid())
    retired_.push_back({retired, completionFence});
  pending_.erase(found);
  return true;
}

bool TerrainGPUJointPublicationRetirementQueue::retire(
    const TerrainGPUPhysicalPageLease lease, const uint64_t completionFence,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (!lease.valid() || completionFence < completedFence_)
    return fail(error, "GPU terrain page retirement is invalid");
  retired_.push_back({lease, completionFence});
  return true;
}

std::vector<TerrainGPUPhysicalPageLease> TerrainGPUJointPublicationRetirementQueue::reclaim(const uint64_t completedFence)
{
  completedFence_ = std::max(completedFence_, completedFence);
  std::vector<TerrainGPUPhysicalPageLease> result;
  auto retained = retired_.begin();
  for (auto entry = retired_.begin(); entry != retired_.end(); ++entry)
  {
    if (entry->fence <= completedFence_)
    {
      result.push_back(entry->lease);
      continue;
    }
    if (retained != entry)
      *retained = *entry;
    ++retained;
  }
  retired_.erase(retained, retired_.end());
  return result;
}

size_t TerrainGPUJointPublicationRetirementQueue::pendingCount() const
{
  return pending_.size();
}

size_t TerrainGPUJointPublicationRetirementQueue::retiredCount() const
{
  return retired_.size();
}

TerrainGPUPageIdentity TerrainGPUPageIdentity::from(
    const TerrainGPUGenerationKey &key)
{
  return {key.bundleSignature, key.chunkX, key.chunkZ, key.internalLOD,
      key.generatorRevision};
}

bool TerrainGPUPageIdentity::operator==(
    const TerrainGPUPageIdentity &other) const
{
  return bundleSignature == other.bundleSignature &&
      chunkX == other.chunkX && chunkZ == other.chunkZ &&
      internalLOD == other.internalLOD &&
      generatorRevision == other.generatorRevision;
}

bool TerrainGPUPageIdentity::operator<(
    const TerrainGPUPageIdentity &other) const
{
  return std::tie(bundleSignature, chunkX, chunkZ, internalLOD,
             generatorRevision) <
      std::tie(other.bundleSignature, other.chunkX, other.chunkZ,
             other.internalLOD, other.generatorRevision);
}

TerrainGPUResidentPageCache::TerrainGPUResidentPageCache(
    const TerrainGPUPageResidencySettings settings)
{
  configure(settings);
}

bool TerrainGPUResidentPageCache::configure(
    const TerrainGPUPageResidencySettings &settings, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.maximumResidentPages == 0u ||
      settings.maximumResidentBytes == 0u)
    return fail(error, "GPU terrain resident-page limits are invalid");
  if (!resident_.empty() || !pending_.empty() ||
      retirement_.pendingCount() != 0u || retirement_.retiredCount() != 0u)
    return fail(error, "GPU terrain resident-page cache is not empty");
  settings_ = settings;
  diagnostics_ = {};
  completedFence_ = 0u;
  return true;
}

bool TerrainGPUResidentPageCache::updateRequired(
    const uint64_t frameIndex, const uint64_t retirementFence,
    const std::vector<TerrainGPUPageDemand> &required,
    TerrainGPUBuildScheduler &scheduler,
    TerrainGPUPageResidencyUpdate &update, std::string *error)
{
  if (error != nullptr)
    error->clear();
  update = {};
  if (retirementFence < completedFence_)
    return fail(error, "GPU terrain eviction fence predates reclamation");

  std::map<TerrainGPUPageIdentity, const TerrainGPUPageDemand *> unique;
  uint64_t requiredBytes = 0u;
  uint64_t requiredBundle = 0u;
  for (const auto &demand : required)
  {
    const auto identity = TerrainGPUPageIdentity::from(demand.build.key);
    if (identity.bundleSignature == 0u || identity.generatorRevision == 0u ||
        demand.residentBytes == 0u ||
        demand.residentBytes > settings_.maximumResidentBytes)
      return fail(error, "GPU terrain page demand is invalid");
    if (requiredBundle != 0u &&
        requiredBundle != demand.build.key.bundleSignature)
      return fail(error, "GPU terrain required set spans multiple bundles");
    requiredBundle = demand.build.key.bundleSignature;
    const auto [found, inserted] = unique.emplace(identity, &demand);
    if (!inserted &&
        (!(found->second->build.key == demand.build.key) ||
         found->second->residentBytes != demand.residentBytes))
      return fail(error, "GPU terrain page demand has conflicting installs");
    if (!inserted)
      continue;
    if (requiredBytes > std::numeric_limits<uint64_t>::max() -
            demand.residentBytes)
      return fail(error, "GPU terrain required-page bytes overflow");
    requiredBytes += demand.residentBytes;
  }
  if (unique.size() > settings_.maximumResidentPages ||
      requiredBytes > settings_.maximumResidentBytes)
    return fail(error, "GPU terrain required set exceeds resident-page budget");

  // A cancelled or failed build has no reusable payload. Forget the job so a
  // current demand below can submit a deterministic rebuild.
  for (auto iterator = pending_.begin(); iterator != pending_.end();)
  {
    const auto job = scheduler.job(iterator->second.jobId);
    if (job && job->stage != TerrainGPUBuildStage::Cancelled &&
        job->stage != TerrainGPUBuildStage::Failed)
    {
      ++iterator;
      continue;
    }
    iterator = pending_.erase(iterator);
  }

  for (auto iterator = pending_.begin(); iterator != pending_.end();)
  {
    if (unique.find(iterator->first) != unique.end())
    {
      ++iterator;
      continue;
    }
    if (iterator->second.publicationBegun)
    {
      // The GPU CAS is already in flight and can still make this replacement
      // live. Keep owning it through completion; the following cut update will
      // emit an ordinary fence-deferred eviction if it was accepted.
      ++iterator;
      continue;
    }
    scheduler.cancel(iterator->second.jobId);
    update.cancelledBuilds.push_back(iterator->second.jobId);
    iterator = pending_.erase(iterator);
  }

  std::set<TerrainGPUPageIdentity> evict;
  if (settings_.evictUnrequired)
  {
    for (const auto &entry : resident_)
      if (unique.find(entry.first) == unique.end())
        evict.insert(entry.first);
  }
  else
  {
    uint64_t projectedBytes = requiredBytes;
    size_t projectedPages = unique.size();
    std::vector<const TerrainGPUResidentPage *> lru;
    for (const auto &entry : resident_)
    {
      if (unique.find(entry.first) != unique.end())
        continue;
      if (projectedBytes > std::numeric_limits<uint64_t>::max() -
              entry.second.residentBytes)
        return fail(error, "GPU terrain retained-page bytes overflow");
      projectedBytes += entry.second.residentBytes;
      ++projectedPages;
      if (!entry.second.pinned)
        lru.push_back(&entry.second);
    }
    std::sort(lru.begin(), lru.end(), [](const auto *left, const auto *right)
    {
      if (left->lastRequiredFrame != right->lastRequiredFrame)
        return left->lastRequiredFrame < right->lastRequiredFrame;
      return left->identity < right->identity;
    });
    for (const auto *candidate : lru)
    {
      if (projectedPages <= settings_.maximumResidentPages &&
          projectedBytes <= settings_.maximumResidentBytes)
        break;
      evict.insert(candidate->identity);
      --projectedPages;
      projectedBytes -= candidate->residentBytes;
    }
    if (projectedPages > settings_.maximumResidentPages ||
        projectedBytes > settings_.maximumResidentBytes)
      return fail(error, "GPU terrain pinned pages exceed resident-page budget");
  }

  for (auto iterator = resident_.begin(); iterator != resident_.end();)
  {
    const bool isRequired = unique.find(iterator->first) != unique.end();
    if (isRequired)
    {
      const auto &demand = *unique.at(iterator->first);
      iterator->second.lastRequiredFrame = frameIndex;
      iterator->second.pinned = demand.pinned;
      ++iterator;
      continue;
    }
    if (evict.find(iterator->first) == evict.end())
    {
      ++iterator;
      continue;
    }
    TerrainGPUPageEviction eviction{iterator->second.identity,
        iterator->second.key, iterator->second.lease,
        iterator->second.residentBytes};
    if (!retirement_.retire(eviction.lease, retirementFence, error))
      return false;
    update.evictions.push_back(eviction);
    ++diagnostics_.pageUninstallCount;
    ++diagnostics_.evictedPages;
    iterator = resident_.erase(iterator);
  }

  for (const auto &[identity, demandPointer] : unique)
  {
    const auto &demand = *demandPointer;
    const auto live = resident_.find(identity);
    if (live != resident_.end() && live->second.key == demand.build.key)
    {
      ++diagnostics_.cacheHits;
      continue;
    }

    auto pending = pending_.find(identity);
    if (pending != pending_.end())
    {
      if (pending->second.request.key == demand.build.key)
      {
        pending->second.lastRequiredFrame = frameIndex;
        pending->second.pinned = demand.pinned;
        uint64_t ignored = 0u;
        if (!scheduler.request(demand.build, ignored, error))
          return false;
        continue;
      }
      if (pending->second.publicationBegun)
        return fail(error,
            "GPU terrain page changed generation during publication");
      scheduler.cancel(pending->second.jobId);
      update.cancelledBuilds.push_back(pending->second.jobId);
      pending_.erase(pending);
    }

    scheduler.setBundleSignature(demand.build.key.bundleSignature);
    scheduler.setTerrainGeneration(demand.build.key.terrainSlot,
        demand.build.key.expectedTerrainGeneration);
    uint64_t jobId = 0u;
    if (!scheduler.request(demand.build, jobId, error))
      return false;
    PendingBuild build{};
    build.request = demand.build;
    build.jobId = jobId;
    build.residentBytes = demand.residentBytes;
    build.lastRequiredFrame = frameIndex;
    build.pinned = demand.pinned;
    pending_.emplace(identity, build);
    update.scheduledBuilds.push_back(jobId);
    ++diagnostics_.rebuildRequests;
  }

  diagnostics_.residentPages = static_cast<uint32_t>(resident_.size());
  diagnostics_.pendingBuilds = static_cast<uint32_t>(pending_.size());
  diagnostics_.residentBytes = 0u;
  diagnostics_.pendingPublications = 0u;
  for (const auto &[identity, page] : resident_)
    diagnostics_.residentBytes += page.residentBytes;
  for (const auto &[identity, build] : pending_)
    diagnostics_.pendingPublications += build.publicationBegun ? 1u : 0u;
  return true;
}

bool TerrainGPUResidentPageCache::beginPublication(
    const uint64_t jobId, const TerrainGPUPhysicalPageLease replacement,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  const auto found = std::find_if(pending_.begin(), pending_.end(),
      [&](const auto &entry) { return entry.second.jobId == jobId; });
  if (found == pending_.end() || found->second.publicationBegun ||
      !replacement.valid())
    return fail(error, "GPU terrain resident publication is invalid");
  if (!retirement_.begin(jobId, replacement, error))
    return false;
  found->second.publicationBegun = true;
  found->second.replacement = replacement;
  diagnostics_.pendingPublications += 1u;
  return true;
}

bool TerrainGPUResidentPageCache::completePublication(
    const uint64_t jobId, const bool accepted,
    const uint64_t completionFence, TerrainGPUBuildScheduler &scheduler,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (completionFence < completedFence_)
    return fail(error, "GPU terrain publication fence predates reclamation");
  const auto found = std::find_if(pending_.begin(), pending_.end(),
      [&](const auto &entry)
      {
        return entry.second.jobId == jobId &&
            entry.second.publicationBegun;
      });
  if (found == pending_.end())
    return fail(error, "GPU terrain resident publication was not begun");

  bool install = accepted;
  const auto job = scheduler.job(jobId);
  if (!job || !(job->key == found->second.request.key))
    install = false;
  const auto existing = resident_.find(found->first);
  const TerrainGPUPhysicalPageLease previous = existing == resident_.end()
      ? TerrainGPUPhysicalPageLease{} : existing->second.lease;

  if (!scheduler.completePublication(
          {jobId, completionFence, install}, error))
    return false;
  if (!retirement_.complete(
          jobId, install, previous, completionFence, error))
    return false;

  if (install)
  {
    TerrainGPUResidentPage page{};
    page.identity = found->first;
    page.key = found->second.request.key;
    page.lease = found->second.replacement;
    page.residentBytes = found->second.residentBytes;
    page.lastRequiredFrame = found->second.lastRequiredFrame;
    page.pinned = found->second.pinned;
    resident_[found->first] = page;
    ++diagnostics_.pageInstallCount;
  }
  else
  {
    ++diagnostics_.rejectedPublications;
  }
  pending_.erase(found);
  diagnostics_.residentPages = static_cast<uint32_t>(resident_.size());
  diagnostics_.pendingBuilds = static_cast<uint32_t>(pending_.size());
  diagnostics_.pendingPublications = 0u;
  diagnostics_.residentBytes = 0u;
  for (const auto &[identity, page] : resident_)
    diagnostics_.residentBytes += page.residentBytes;
  for (const auto &[identity, build] : pending_)
    diagnostics_.pendingPublications += build.publicationBegun ? 1u : 0u;
  return true;
}

std::vector<TerrainGPUPhysicalPageLease>
TerrainGPUResidentPageCache::reclaim(const uint64_t completedFence)
{
  completedFence_ = std::max(completedFence_, completedFence);
  auto reclaimed = retirement_.reclaim(completedFence_);
  diagnostics_.reclaimedLeases += reclaimed.size();
  return reclaimed;
}

std::optional<TerrainGPUResidentPage> TerrainGPUResidentPageCache::resident(
    const TerrainGPUPageIdentity &identity) const
{
  const auto found = resident_.find(identity);
  if (found == resident_.end())
    return std::nullopt;
  return found->second;
}

TerrainGPUPageResidencyDiagnostics
TerrainGPUResidentPageCache::diagnostics() const
{
  return diagnostics_;
}

} // namespace voxel::svraster
