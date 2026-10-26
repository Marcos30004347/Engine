#include "voxel/svraster/TerrainGPUV2WorldMapAtlas.hpp"

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

} // namespace

bool TerrainGPUV2WorldMapAtlas::initialize(const procedural::world::MapExportBundle &bundle, const TerrainGPUV2WorldMapAtlasConfig &config, std::string *error)
{
  clear();
  if (config.maximumPinnedTiles == 0u || config.pageCache.slotCapacity == 0u)
    return fail(error, "terrain GPU V2 atlas configuration is invalid");
  if (!layout_.initialize(bundle, config.pageCache.slotCapacity, error) || !pageCache_.initialize(bundle, config.pageCache, error))
  {
    clear();
    return false;
  }
  std::vector<std::string> fields;
  const auto refined = bundle.field("refined_elevation");
  const std::array<const char *, kTerrainGPUV2ColumnFieldCount> names{
    refined != nullptr && refined->available ? "refined_elevation" : "base_elevation",
    "base_elevation", "uplift", "relief_roughness", "landform_id",
    "water_type", "water_level", "river", "coast_distance",
    "erosion_wear_flow", "erosion_deposition", "substrate_id",
    "soil_depth", "biome_mix_ids", "biome_mix_weights", "climate",
    "placement_potential", "soil_layers", "roads",
    bundle.field("settlement_activity") != nullptr &&
            bundle.field("settlement_activity")->available
        ? "settlement_activity" : "object_exclusion"};
  for (uint32_t index = 0u; index < names.size(); ++index)
  {
    const char *name = names[index];
    const auto *field = bundle.field(name);
    const bool required = index == static_cast<uint32_t>(
            TerrainGPUV2ColumnField::Elevation) ||
        (index >= static_cast<uint32_t>(TerrainGPUV2ColumnField::WaterType) &&
         index <= static_cast<uint32_t>(TerrainGPUV2ColumnField::River)) ||
        index == static_cast<uint32_t>(TerrainGPUV2ColumnField::Substrate) ||
        index == static_cast<uint32_t>(TerrainGPUV2ColumnField::SoilDepth) ||
        index == static_cast<uint32_t>(TerrainGPUV2ColumnField::BiomeIds) ||
        index == static_cast<uint32_t>(TerrainGPUV2ColumnField::BiomeWeights);
    if ((field == nullptr || !field->available) && required)
    {
      clear();
      return fail(error, std::string("terrain GPU V2 atlas lacks required field: ") + name);
    }
    if (field != nullptr && field->available &&
        std::find(fields.begin(), fields.end(), name) == fields.end())
      fields.emplace_back(name);
  }
  procedural::world::MapExportStreamingCache roots;
  procedural::world::MapExportStreamUpdate update{};
  procedural::world::MapExportStreamRequest request{};
  request.level = 0u;
  request.fields = std::move(fields);
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

void TerrainGPUV2WorldMapAtlas::clear()
{
  pageCache_.clear();
  layout_.clear();
  bundle_ = nullptr;
  config_ = {};
  baseline_.clear();
  currentTarget_.clear();
  pins_.clear();
  committedKeys_.clear();
  pendingKeys_.clear();
  pendingRevision_ = 0u;
  pendingUploadedBytes_ = 0u;
  pendingDeferredUnits_ = 0u;
  publicationPending_ = false;
  diagnostics_ = {};
}

bool TerrainGPUV2WorldMapAtlas::pin(const uint64_t tileId, const procedural::world::MapExportStreamRequest &request, std::string *error)
{
  if (bundle_ == nullptr || tileId == 0u || pins_.count(tileId) != 0u || pins_.size() >= config_.maximumPinnedTiles)
    return fail(error, "terrain GPU V2 atlas pin is invalid or duplicate");
  procedural::world::MapExportStreamingCache planner;
  procedural::world::MapExportStreamUpdate update{};
  if (!planner.initialize(*bundle_, error) || !planner.update(request, update, error))
    return false;
  pins_.emplace(tileId, Pin{planner.residentPages(), false});
  if (!refreshTarget(error))
  {
    pins_.erase(tileId);
    return false;
  }
  return true;
}

bool TerrainGPUV2WorldMapAtlas::unpin(const uint64_t tileId, std::string *error)
{
  if (pins_.erase(tileId) == 0u)
    return fail(error, "terrain GPU V2 atlas tile is not pinned");
  return refreshTarget(error);
}

bool TerrainGPUV2WorldMapAtlas::addFaultPage(Pin &pin, const TerrainGPUV2MapFaultGPU &fault, std::string *error)
{
  if (bundle_ == nullptr || fault.field >= layout_.fields().size() || fault.level > bundle_->maximumPageLevel || fault.level > 31u)
    return fail(error, "terrain GPU V2 map fault address is invalid");
  const uint32_t dimension = uint32_t{1u} << fault.level;
  if (fault.pageX >= dimension || fault.pageZ >= dimension)
    return fail(error, "terrain GPU V2 map fault page is out of range");
  const std::string &name = layout_.fields()[fault.field].name;
  const auto *source = bundle_->field(name);
  if (source == nullptr)
    return fail(error, "terrain GPU V2 map fault field is absent");
  std::vector<const procedural::world::MapExportField *> group;
  if (source->samplingGroup.empty())
    group.push_back(source);
  else
    for (const auto &field : bundle_->fields)
      if (field.available && field.samplingGroup == source->samplingGroup)
        group.push_back(&field);
  procedural::world::MapExportPageAddress address{static_cast<uint8_t>(fault.level), fault.pageX, fault.pageZ};
  for (;;)
  {
    const bool complete = std::all_of(
        group.begin(),
        group.end(),
        [&](const auto *field)
        {
          return field->page(address) != nullptr;
        });
    if (complete)
      break;
    if (address.level == 0u)
      return fail(error, "terrain GPU V2 fault has no complete ancestor");
    address = procedural::world::mapExportPageParent(address);
  }
  for (const auto *field : group)
    pin.pages.push_back({field->name, address});
  std::sort(pin.pages.begin(), pin.pages.end(), keyLess);
  pin.pages.erase(std::unique(pin.pages.begin(), pin.pages.end()), pin.pages.end());
  return true;
}

bool TerrainGPUV2WorldMapAtlas::reportFaults(const uint64_t tileId, const std::vector<TerrainGPUV2MapFaultGPU> &faults, std::string *error)
{
  const auto found = pins_.find(tileId);
  if (found == pins_.end() || faults.empty())
    return fail(error, "terrain GPU V2 fault report is empty or unpinned");
  for (const auto &fault : faults)
    if (!addFaultPage(found->second, fault, error))
      return false;
  found->second.ready = false;
  diagnostics_.reportedFaults += faults.size();
  ++diagnostics_.retryRequests;
  return refreshTarget(error);
}

bool TerrainGPUV2WorldMapAtlas::waitForPreparation(const std::chrono::milliseconds timeout, std::string *error)
{
  return pageCache_.waitForPreparation(timeout, error);
}

bool TerrainGPUV2WorldMapAtlas::publish(const size_t maximumUploadBytes, procedural::world::MapExportPagePublication &publication, std::string *error)
{
  if (publicationPending_)
    return fail(error, "terrain GPU V2 atlas publication is awaiting commit");
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

bool TerrainGPUV2WorldMapAtlas::commit(const procedural::world::MapExportPagePublication &publication, std::string *error)
{
  std::set<std::string> supplied;
  for (const auto &mapping : publication.pageTable)
    supplied.insert(packedKey(mapping.key));
  if (bundle_ == nullptr || !publicationPending_ || publication.revision != pendingRevision_ || supplied != pendingKeys_ || publication.revision < diagnostics_.committedRevision)
    return fail(error, "terrain GPU V2 atlas commit is stale or invalid");
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
  for (auto &[id, pin] : pins_)
    pin.ready = committed(pin);
  return true;
}

bool TerrainGPUV2WorldMapAtlas::ready(const uint64_t tileId) const
{
  const auto found = pins_.find(tileId);
  return found != pins_.end() && found->second.ready;
}

TerrainGPUV2WorldMapAtlasDiagnostics TerrainGPUV2WorldMapAtlas::diagnostics() const
{
  auto result = diagnostics_;
  result.pinnedTiles = static_cast<uint32_t>(pins_.size());
  result.readyTiles = static_cast<uint32_t>(std::count_if(
      pins_.begin(),
      pins_.end(),
      [](const auto &entry)
      {
        return entry.second.ready;
      }));
  result.waitingTiles = result.pinnedTiles - result.readyTiles;
  result.pinnedPages = static_cast<uint32_t>(std::min<size_t>(
      currentTarget_.size(),
      std::numeric_limits<uint32_t>::max()));
  return result;
}

bool TerrainGPUV2WorldMapAtlas::refreshTarget(std::string *error)
{
  std::vector<MapExportStreamKey> target = baseline_;
  for (const auto &[id, pin] : pins_)
    target.insert(target.end(), pin.pages.begin(), pin.pages.end());
  std::sort(target.begin(), target.end(), keyLess);
  target.erase(std::unique(target.begin(), target.end()), target.end());
  if (target == currentTarget_)
  {
    ++diagnostics_.skippedTargetRefreshes;
    if (error != nullptr)
      error->clear();
    return true;
  }
  if (!pageCache_.setTarget(target, error))
    return false;
  currentTarget_ = std::move(target);
  ++diagnostics_.targetRefreshes;
  return true;
}

bool TerrainGPUV2WorldMapAtlas::committed(const Pin &pin) const
{
  return std::all_of(
      pin.pages.begin(),
      pin.pages.end(),
      [&](const auto &page)
      {
        return committedKeys_.count(packedKey(page)) != 0u;
      });
}

} // namespace voxel::svraster
