#include "voxel/svraster/TerrainGPUWorldMapAtlas.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace voxel::svraster
{
namespace
{
using procedural::world::MapExportStreamKey;

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool keyLess(const MapExportStreamKey &left, const MapExportStreamKey &right)
{
  return std::tie(left.field, left.address.level, left.address.x, left.address.y) < std::tie(right.field, right.address.level, right.address.x, right.address.y);
}

std::string packedKey(const MapExportStreamKey &key)
{
  return key.field + "\n" + std::to_string(key.address.level) + ":" + std::to_string(key.address.x) + ":" + std::to_string(key.address.y);
}

bool terminalStage(const TerrainGPUBuildStage stage)
{
  return stage == TerrainGPUBuildStage::Published || stage == TerrainGPUBuildStage::Cancelled || stage == TerrainGPUBuildStage::Failed;
}
} // namespace

bool terrainGPUWorldMapFields(const procedural::world::MapExportBundle &bundle, std::vector<std::string> &fields, std::string *error)
{
  using procedural::world::MapExportFilter;
  using procedural::world::MapExportFormat;
  fields.clear();
  const auto available = [&](const std::string &name)
  {
    const auto *field = bundle.field(name);
    return field != nullptr && field->available;
  };
  const std::string elevation = available("refined_elevation") ? "refined_elevation" : "base_elevation";
  const std::array<std::string, 8u> required{elevation, "water_type", "water_level", "river", "substrate_id", "soil_depth", "biome_mix_ids", "biome_mix_weights"};
  for (const auto &name : required)
    if (!available(name))
      return fail(error, "GPU terrain atlas is missing required field: " + name);
    else
      fields.push_back(name);
  const auto contract = [&](const std::string &name, const MapExportFormat format, const MapExportFilter filter)
  {
    const auto *field = bundle.field(name);
    return field != nullptr && field->format == format && field->filter == filter;
  };
  if (!contract(elevation, MapExportFormat::R16Unorm, MapExportFilter::Linear) || !contract("water_type", MapExportFormat::R8Uint, MapExportFilter::Nearest) ||
      !contract("water_level", MapExportFormat::R16Float, MapExportFilter::Linear) || !contract("river", MapExportFormat::RG16Float, MapExportFilter::Linear) ||
      !contract("substrate_id", MapExportFormat::R8Uint, MapExportFilter::Nearest) || !contract("soil_depth", MapExportFormat::R16Float, MapExportFilter::Linear) ||
      !contract("biome_mix_ids", MapExportFormat::RGBA8Uint, MapExportFilter::SparseMixture) || !contract("biome_mix_weights", MapExportFormat::RGBA8Unorm, MapExportFilter::SparseMixture))
    return fail(error, "GPU terrain atlas field format contract is invalid");
  const auto *biomeIds = bundle.field("biome_mix_ids");
  const auto *biomeWeights = bundle.field("biome_mix_weights");
  if (biomeIds->samplingGroup.empty() || biomeIds->samplingGroup != biomeWeights->samplingGroup)
    return fail(error, "GPU terrain atlas biome sampling group is invalid");

  const std::array<std::tuple<std::string, MapExportFormat, MapExportFilter>, 14u> optional{{
    {"base_elevation", MapExportFormat::R16Unorm, MapExportFilter::Linear},
    {"uplift", MapExportFormat::R16Float, MapExportFilter::Linear},
    {"relief_roughness", MapExportFormat::RG16Float, MapExportFilter::Linear},
    {"landform_id", MapExportFormat::R8Uint, MapExportFilter::Nearest},
    {"erosion_wear_flow", MapExportFormat::RG16Float, MapExportFilter::Linear},
    {"erosion_deposition", MapExportFormat::R16Float, MapExportFilter::Linear},
    {"coast_distance", MapExportFormat::R16Float, MapExportFilter::Linear},
    {"climate", MapExportFormat::RG16Float, MapExportFilter::Linear},
    {"placement_potential", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear},
    {"soil_layers", MapExportFormat::RGBA8Unorm, MapExportFilter::Linear},
    {"roads", MapExportFormat::R8Uint, MapExportFilter::Nearest},
    {"settlement_activity", MapExportFormat::R8Uint, MapExportFilter::Nearest},
    {"object_exclusion", MapExportFormat::R8Uint, MapExportFilter::Nearest},
    {"biome_id", MapExportFormat::R8Uint, MapExportFilter::Nearest},
  }};
  for (const auto &[name, format, filter] : optional)
    if (available(name) && std::find(fields.begin(), fields.end(), name) == fields.end())
    {
      if (!contract(name, format, filter))
        return fail(error, "GPU terrain atlas optional field format contract is invalid");
      fields.push_back(name);
    }
  return true;
}

bool TerrainGPUWorldMapAtlas::initialize(const procedural::world::MapExportBundle &bundle, const TerrainGPUWorldMapAtlasConfig &config, std::string *error)
{
  clear();
  if (config.maximumTrackedJobs == 0u || config.pageCache.slotCapacity == 0u || config.pinnedRootFields.empty())
    return fail(error, "GPU terrain atlas configuration is invalid");
  if (!layout_.initialize(bundle, config.pageCache.slotCapacity, error) || !pageCache_.initialize(bundle, config.pageCache, error))
  {
    clear();
    return false;
  }
  procedural::world::MapExportStreamingCache roots;
  procedural::world::MapExportStreamUpdate update{};
  procedural::world::MapExportStreamRequest request{};
  request.level = 0u;
  request.fields = config.pinnedRootFields;
  request.byteBudget = std::numeric_limits<size_t>::max();
  if (!roots.initialize(bundle, error) || !roots.update(request, update, error))
  {
    clear();
    return false;
  }
  baseline_ = roots.residentPages();
  bundle_ = &bundle;
  config_ = config;
  if (!refreshTarget(error))
  {
    clear();
    return false;
  }
  return true;
}

void TerrainGPUWorldMapAtlas::clear()
{
  pageCache_.clear();
  layout_.clear();
  bundle_ = nullptr;
  config_ = {};
  baseline_.clear();
  jobs_.clear();
  committedKeys_.clear();
  pendingKeys_.clear();
  pendingRevision_ = 0u;
  pendingUploadedBytes_ = 0u;
  pendingDeferredUnits_ = 0u;
  publicationPending_ = false;
  diagnostics_ = {};
}

bool TerrainGPUWorldMapAtlas::request(const uint64_t jobId, const procedural::world::MapExportStreamRequest &request, std::string *error)
{
  if (bundle_ == nullptr || jobId == 0u || jobs_.find(jobId) != jobs_.end() || jobs_.size() >= config_.maximumTrackedJobs)
    return fail(error, "GPU terrain atlas job request is invalid or duplicate");
  procedural::world::MapExportStreamingCache planner;
  procedural::world::MapExportStreamUpdate update{};
  if (!planner.initialize(*bundle_, error) || !planner.update(request, update, error))
    return false;
  jobs_.emplace(jobId, Job{planner.residentPages(), false});
  if (!refreshTarget(error))
  {
    jobs_.erase(jobId);
    return false;
  }
  return true;
}

bool TerrainGPUWorldMapAtlas::release(const uint64_t jobId, std::string *error)
{
  if (jobs_.erase(jobId) == 0u)
    return fail(error, "GPU terrain atlas job is not tracked");
  return refreshTarget(error);
}

bool TerrainGPUWorldMapAtlas::waitForPreparation(const std::chrono::milliseconds timeout, std::string *error)
{
  return pageCache_.waitForPreparation(timeout, error);
}

bool TerrainGPUWorldMapAtlas::publish(const size_t maximumUploadBytes, procedural::world::MapExportPagePublication &publication, std::string *error)
{
  if (publicationPending_)
    return fail(error, "GPU terrain atlas publication is awaiting commit");
  if (!pageCache_.publish(maximumUploadBytes, publication, error))
    return false;
  pendingKeys_.clear();
  for (const auto &mapping : publication.pageTable)
    pendingKeys_.insert(packedKey(mapping.key));
  pendingRevision_ = publication.revision;
  pendingUploadedBytes_ = publication.uploadedBytes;
  pendingDeferredUnits_ = publication.deferredUnits;
  publicationPending_ = true;
  return true;
}

bool TerrainGPUWorldMapAtlas::commit(const procedural::world::MapExportPagePublication &publication, TerrainGPUBuildScheduler &scheduler, std::string *error)
{
  std::set<std::string> suppliedKeys;
  for (const auto &mapping : publication.pageTable)
    suppliedKeys.insert(packedKey(mapping.key));
  if (bundle_ == nullptr || !publicationPending_ || publication.revision != pendingRevision_ || suppliedKeys != pendingKeys_ || publication.revision < diagnostics_.committedRevision)
    return fail(error, "GPU terrain atlas commit is stale or uninitialized");
  committedKeys_ = pendingKeys_;
  diagnostics_.uploadedBytes += pendingUploadedBytes_;
  diagnostics_.deferredUnits += pendingDeferredUnits_;
  diagnostics_.committedRevision = publication.revision;
  ++diagnostics_.publications;
  pendingKeys_.clear();
  pendingRevision_ = 0u;
  pendingUploadedBytes_ = 0u;
  pendingDeferredUnits_ = 0u;
  publicationPending_ = false;

  bool targetChanged = false;
  for (auto iterator = jobs_.begin(); iterator != jobs_.end();)
  {
    const auto schedulerJob = scheduler.job(iterator->first);
    if (!schedulerJob || terminalStage(schedulerJob->stage))
    {
      iterator = jobs_.erase(iterator);
      targetChanged = true;
      continue;
    }
    if (!iterator->second.ready && committed(iterator->second))
    {
      if (schedulerJob->stage != TerrainGPUBuildStage::WaitingForMaps || !scheduler.markMapsReady(iterator->first, 0u, error))
        return fail(error, "GPU terrain atlas could not release a map-ready scheduler job");
      iterator->second.ready = true;
    }
    ++iterator;
  }
  if (targetChanged && !refreshTarget(error))
    return false;
  return true;
}

bool TerrainGPUWorldMapAtlas::ready(const uint64_t jobId) const
{
  const auto found = jobs_.find(jobId);
  return found != jobs_.end() && found->second.ready;
}

TerrainGPUWorldMapAtlasDiagnostics TerrainGPUWorldMapAtlas::diagnostics() const
{
  auto result = diagnostics_;
  result.trackedJobs = static_cast<uint32_t>(jobs_.size());
  result.readyJobs = static_cast<uint32_t>(std::count_if(
      jobs_.begin(),
      jobs_.end(),
      [](const auto &entry)
      {
        return entry.second.ready;
      }));
  result.waitingJobs = result.trackedJobs - result.readyJobs;
  std::set<std::string> pages;
  for (const auto &key : baseline_)
    pages.insert(packedKey(key));
  for (const auto &[id, job] : jobs_)
    for (const auto &key : job.pages)
      pages.insert(packedKey(key));
  result.pinnedPages = static_cast<uint32_t>(pages.size());
  return result;
}

bool TerrainGPUWorldMapAtlas::refreshTarget(std::string *error)
{
  std::vector<MapExportStreamKey> target = baseline_;
  for (const auto &[id, job] : jobs_)
    target.insert(target.end(), job.pages.begin(), job.pages.end());
  std::sort(target.begin(), target.end(), keyLess);
  target.erase(std::unique(target.begin(), target.end()), target.end());
  return pageCache_.setTarget(target, error);
}

bool TerrainGPUWorldMapAtlas::committed(const Job &job) const
{
  return std::all_of(
      job.pages.begin(),
      job.pages.end(),
      [&](const auto &key)
      {
        return committedKeys_.find(packedKey(key)) != committedKeys_.end();
      });
}

} // namespace voxel::svraster
