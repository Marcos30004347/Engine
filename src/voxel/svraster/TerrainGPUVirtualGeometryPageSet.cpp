#include "voxel/svraster/TerrainGPUVirtualGeometryPageSet.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace voxel::svraster
{

bool TerrainGPUVirtualGeometryPageSet::configure(
    const TerrainGPUVGPageSetSettings &settings,
    std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (settings.capacity == 0u || !(settings.baseEdgeMetres > 0.0) ||
      !(settings.worldExtentMetres > 0.0) || !(settings.finestVoxelMetres > 0.0))
  {
    if (error != nullptr)
      *error = "terrain GPU VG page set settings are invalid";
    return false;
  }
  settings_ = settings;
  pages_.assign(settings.capacity, TerrainGPUVGPage{});
  freeSlots_.clear();
  freeSlots_.reserve(settings.capacity);
  for (uint32_t slot = settings.capacity; slot > 0u; --slot)
    freeSlots_.push_back(slot - 1u);
  slotByAddress_.clear();
  liveCount_ = 0u;
  return true;
}

uint32_t TerrainGPUVirtualGeometryPageSet::allocate()
{
  if (freeSlots_.empty())
    return UINT32_MAX;
  const uint32_t slot = freeSlots_.back();
  freeSlots_.pop_back();
  ++liveCount_;
  return slot;
}

void TerrainGPUVirtualGeometryPageSet::release(const uint32_t slot)
{
  TerrainGPUVGPage &page = pages_[slot];
  // The chunk's extra page slots go back with it. Leaking them would retire the
  // pool one chunk at a time and read as terrain that stops refining.
  for (const uint32_t extra : page.extraSlots)
  {
    pages_[extra] = TerrainGPUVGPage{};
    freeSlots_.push_back(extra);
    --liveCount_;
  }
  slotByAddress_.erase(page.address);
  page = TerrainGPUVGPage{};
  freeSlots_.push_back(slot);
  --liveCount_;
}

bool TerrainGPUVirtualGeometryPageSet::reservePageSlots(
    const uint32_t slot, const uint32_t pageCount, std::string *error)
{
  if (error != nullptr)
    error->clear();
  if (slot >= pages_.size() || !pages_[slot].live)
  {
    if (error != nullptr)
      *error = "terrain page slot is not live";
    return false;
  }
  if (pageCount == 0u)
  {
    if (error != nullptr)
      *error = "a terrain chunk must own at least one page slot";
    return false;
  }
  TerrainGPUVGPage &page = pages_[slot];
  const uint32_t wanted = pageCount - 1u;
  const uint32_t held = static_cast<uint32_t>(page.extraSlots.size());
  if (wanted == held)
    return true;
  if (wanted < held)
  {
    for (uint32_t index = wanted; index < held; ++index)
    {
      const uint32_t extra = page.extraSlots[index];
      pages_[extra] = TerrainGPUVGPage{};
      freeSlots_.push_back(extra);
      --liveCount_;
    }
    page.extraSlots.resize(wanted);
    return true;
  }
  // All or nothing: a chunk installed with only some of its pages reserved
  // would publish a partial DAG, and the culler cannot tell that from a
  // complete one.
  std::vector<uint32_t> claimed;
  claimed.reserve(wanted - held);
  for (uint32_t index = held; index < wanted; ++index)
  {
    const uint32_t extra = allocate();
    if (extra == UINT32_MAX)
    {
      for (const uint32_t rollback : claimed)
      {
        pages_[rollback] = TerrainGPUVGPage{};
        freeSlots_.push_back(rollback);
        --liveCount_;
      }
      if (error != nullptr)
        *error = "the terrain virtual-geometry page pool is exhausted: " +
            std::to_string(pageCount) + " slots requested";
      return false;
    }
    // An extra slot carries the chunk's address for diagnostics but is not
    // registered in slotByAddress_: the address resolves to the primary slot.
    pages_[extra] = TerrainGPUVGPage{};
    pages_[extra].address = page.address;
    pages_[extra].live = true;
    pages_[extra].extraPage = true;
    pages_[extra].ownerSlot = slot;
    claimed.push_back(extra);
  }
  page.extraSlots.insert(page.extraSlots.end(), claimed.begin(), claimed.end());
  return true;
}

uint32_t TerrainGPUVirtualGeometryPageSet::slotForAssetPage(
    const uint32_t slot, const uint32_t assetPageIndex) const
{
  if (slot >= pages_.size() || !pages_[slot].live)
    return UINT32_MAX;
  if (assetPageIndex == 0u)
    return slot;
  const auto &extras = pages_[slot].extraSlots;
  const uint32_t index = assetPageIndex - 1u;
  return index < extras.size() ? extras[index] : UINT32_MAX;
}

uint32_t TerrainGPUVirtualGeometryPageSet::ownerOf(const uint32_t pageSlot) const
{
  const TerrainGPUVGPage *page = this->page(pageSlot);
  if (page == nullptr)
    return UINT32_MAX;
  return page->extraPage ? page->ownerSlot : pageSlot;
}

uint32_t TerrainGPUVirtualGeometryPageSet::assetPageOf(const uint32_t pageSlot) const
{
  const TerrainGPUVGPage *page = this->page(pageSlot);
  if (page == nullptr)
    return UINT32_MAX;
  if (!page->extraPage)
    return 0u;
  const TerrainGPUVGPage *owner = this->page(page->ownerSlot);
  if (owner == nullptr)
    return UINT32_MAX;
  for (uint32_t index = 0u; index < owner->extraSlots.size(); ++index)
    if (owner->extraSlots[index] == pageSlot)
      return index + 1u;
  return UINT32_MAX;
}

uint32_t TerrainGPUVirtualGeometryPageSet::reservedPageCount(const uint32_t slot) const
{
  if (slot >= pages_.size() || !pages_[slot].live)
    return 0u;
  return 1u + static_cast<uint32_t>(pages_[slot].extraSlots.size());
}

WorldVoxelChunkAddress TerrainGPUVirtualGeometryPageSet::parentAddress(
    const WorldVoxelChunkAddress &address)
{
  WorldVoxelChunkAddress parent{};
  // floor division: the lattice is anchored at the world minimum corner, so
  // addresses are non-negative in practice, but keep negatives correct.
  parent.x = address.x >= 0 ? address.x / 2 : -((-address.x + 1) / 2);
  parent.z = address.z >= 0 ? address.z / 2 : -((-address.z + 1) / 2);
  parent.spatialLOD = address.spatialLOD + 1u;
  return parent;
}

uint32_t TerrainGPUVirtualGeometryPageSet::quadrantOf(const WorldVoxelChunkAddress &address)
{
  return static_cast<uint32_t>(address.x & 1) | (static_cast<uint32_t>(address.z & 1) << 1u);
}

float TerrainGPUVirtualGeometryPageSet::selfError(const uint32_t spatialLOD) const
{
  return static_cast<float>(std::ldexp(settings_.finestVoxelMetres, static_cast<int>(spatialLOD)));
}

float TerrainGPUVirtualGeometryPageSet::parentError(const uint32_t spatialLOD) const
{
  return selfError(spatialLOD + 1u);
}

void TerrainGPUVirtualGeometryPageSet::addressBounds(
    const WorldVoxelChunkAddress &address,
    double &minimumX,
    double &minimumZ,
    double &edge) const
{
  edge = std::ldexp(settings_.baseEdgeMetres, static_cast<int>(address.spatialLOD));
  minimumX = static_cast<double>(address.x) * edge - settings_.worldExtentMetres * 0.5;
  minimumZ = static_cast<double>(address.z) * edge - settings_.worldExtentMetres * 0.5;
}

void TerrainGPUVirtualGeometryPageSet::assignBounds(
    TerrainGPUVGPage &page,
    const TerrainGPUVGDirectoryPage &source)
{
  double minimumX = 0.0;
  double minimumZ = 0.0;
  double edge = 0.0;
  addressBounds(page.address, minimumX, minimumZ, edge);
  double minimumY = 0.0;
  double maximumY = 0.0;
  if (source.hasRecord)
  {
    minimumY = source.recordMinimumSurfaceMetres - settings_.elevationOriginMetres;
    maximumY = source.recordMaximumSurfaceMetres - settings_.elevationOriginMetres;
    // The geometry floor, not the surface floor - see
    // TerrainGPUVGDirectoryPage::recordElevationOriginMetres.
    if (source.hasElevationOrigin)
      minimumY = std::min(
          minimumY,
          source.recordElevationOriginMetres - settings_.elevationOriginMetres);
  }
  else if (page.parentSlot != UINT32_MAX && pages_[page.parentSlot].boundsValid)
  {
    const TerrainGPUVGPage &parent = pages_[page.parentSlot];
    const double slack = selfError(page.address.spatialLOD + 1u);
    minimumY = parent.boundsMin[1] - slack;
    maximumY = parent.boundsMax[1] + slack;
  }
  else
  {
    return;
  }
  if (maximumY < minimumY)
    std::swap(minimumY, maximumY);
  // A page always has some thickness: a flat page still draws a shell.
  const double thickness = std::max(maximumY - minimumY, static_cast<double>(selfError(page.address.spatialLOD)));
  maximumY = minimumY + thickness;
  page.boundsMin = {static_cast<float>(minimumX), static_cast<float>(minimumY), static_cast<float>(minimumZ)};
  page.boundsMax = {static_cast<float>(minimumX + edge), static_cast<float>(maximumY), static_cast<float>(minimumZ + edge)};
  page.boundsValid = true;
}

TerrainGPUVGPageSetSync TerrainGPUVirtualGeometryPageSet::sync(
    const std::vector<TerrainGPUVGDirectoryPage> &pages)
{
  TerrainGPUVGPageSetSync result{};
  // Release first, so a page entering this sync can take a slot a page
  // leaving it frees. Finest first: a parent's mask sees every child go.
  std::unordered_set<uint32_t> incoming;
  incoming.reserve(pages.size());
  for (const auto &page : pages)
  {
    const uint32_t slot = slotOf(page.address);
    if (slot != UINT32_MAX)
      incoming.insert(slot);
  }
  std::vector<uint32_t> leaving;
  for (uint32_t slot = 0u; slot < pages_.size(); ++slot)
    if (pages_[slot].live && !pages_[slot].extraPage &&
        incoming.count(slot) == 0u)
      leaving.push_back(slot);
  std::sort(
      leaving.begin(), leaving.end(),
      [&](const uint32_t left, const uint32_t right)
      { return pages_[left].address.spatialLOD < pages_[right].address.spatialLOD; });
  for (const uint32_t slot : leaving)
  {
    TerrainGPUVGPage &page = pages_[slot];
    if (page.installed)
    {
      const uint32_t parent = markUninstalled(slot);
      if (parent != UINT32_MAX)
        result.maskChanged.push_back(parent);
    }
    if (page.parentSlot != UINT32_MAX && pages_[page.parentSlot].live)
    {
      TerrainGPUVGPage &parent = pages_[page.parentSlot];
      for (auto &child : parent.childSlots)
        if (child == slot)
          child = UINT32_MAX;
    }
    for (const uint32_t child : page.childSlots)
      if (child != UINT32_MAX && pages_[child].live)
        pages_[child].parentSlot = UINT32_MAX;
    release(slot);
    result.released.push_back(slot);
  }
  // Coarse first, so a child finds its parent's slot and bounds.
  std::vector<const TerrainGPUVGDirectoryPage *> ordered;
  ordered.reserve(pages.size());
  for (const auto &page : pages)
    ordered.push_back(&page);
  std::stable_sort(
      ordered.begin(), ordered.end(),
      [](const TerrainGPUVGDirectoryPage *left, const TerrainGPUVGDirectoryPage *right)
      { return left->address.spatialLOD > right->address.spatialLOD; });
  for (const auto *source : ordered)
  {
    uint32_t slot = slotOf(source->address);
    if (slot == UINT32_MAX)
    {
      slot = allocate();
      if (slot == UINT32_MAX)
      {
        ++result.rejected;
        continue;
      }
      TerrainGPUVGPage &page = pages_[slot];
      page.address = source->address;
      page.live = true;
      slotByAddress_.emplace(source->address, slot);
      page.parentSlot = slotOf(parentAddress(source->address));
      page.rootPage = page.parentSlot == UINT32_MAX;
      if (page.parentSlot != UINT32_MAX)
      {
        TerrainGPUVGPage &parent = pages_[page.parentSlot];
        parent.childSlots[quadrantOf(source->address)] = slot;
      }
      result.assigned.push_back(slot);
    }
    TerrainGPUVGPage &page = pages_[slot];
    if (!page.boundsValid)
    {
      assignBounds(page, *source);
      if (page.boundsValid)
        result.boundsChanged.push_back(slot);
    }
  }
  return result;
}

uint32_t TerrainGPUVirtualGeometryPageSet::slotOf(const WorldVoxelChunkAddress &address) const
{
  const auto found = slotByAddress_.find(address);
  return found == slotByAddress_.end() ? UINT32_MAX : found->second;
}

const TerrainGPUVGPage *TerrainGPUVirtualGeometryPageSet::page(const uint32_t slot) const
{
  if (slot >= pages_.size() || !pages_[slot].live)
    return nullptr;
  return &pages_[slot];
}

const TerrainGPUVGPage *TerrainGPUVirtualGeometryPageSet::find(const WorldVoxelChunkAddress &address) const
{
  return page(slotOf(address));
}

uint32_t TerrainGPUVirtualGeometryPageSet::markInstalled(
    const uint32_t slot,
    const uint32_t clusterOffset,
    const uint32_t clusterCount,
    const uint64_t pageGeneration)
{
  if (slot >= pages_.size() || !pages_[slot].live)
    return UINT32_MAX;
  TerrainGPUVGPage &page = pages_[slot];
  const bool wasInstalled = page.installed;
  page.installed = true;
  page.clusterOffset = clusterOffset;
  page.clusterCount = clusterCount;
  page.pageGeneration = pageGeneration;
  if (wasInstalled || page.parentSlot == UINT32_MAX || !pages_[page.parentSlot].live)
    return UINT32_MAX;
  TerrainGPUVGPage &parent = pages_[page.parentSlot];
  parent.residentChildMask |= 1u << quadrantOf(page.address);
  return page.parentSlot;
}

uint32_t TerrainGPUVirtualGeometryPageSet::markUninstalled(const uint32_t slot)
{
  if (slot >= pages_.size() || !pages_[slot].live || !pages_[slot].installed)
    return UINT32_MAX;
  TerrainGPUVGPage &page = pages_[slot];
  page.installed = false;
  page.clusterOffset = 0u;
  page.clusterCount = 0u;
  if (page.parentSlot == UINT32_MAX || !pages_[page.parentSlot].live)
    return UINT32_MAX;
  TerrainGPUVGPage &parent = pages_[page.parentSlot];
  parent.residentChildMask &= ~(1u << quadrantOf(page.address));
  return page.parentSlot;
}

} // namespace voxel::svraster

namespace voxel::svraster
{

std::vector<TerrainVGBuildRequest> planTerrainVGPageBuilds(
    const TerrainGPUVirtualGeometryPageSet &pageSet,
    const std::vector<TerrainVGPageDemand> &demand,
    const std::vector<uint32_t> &inFlightSlots,
    const uint32_t maximumRequests)
{
  std::vector<TerrainVGBuildRequest> requests;
  if (maximumRequests == 0u || demand.empty())
    return requests;

  std::vector<TerrainVGPageDemand> ordered = demand;
  std::sort(
      ordered.begin(), ordered.end(),
      [](const TerrainVGPageDemand &left, const TerrainVGPageDemand &right)
      {
        if (left.priority != right.priority)
          return left.priority > right.priority;
        return left.slot < right.slot;
      });

  std::vector<uint32_t> claimed(inFlightSlots);
  const auto alreadyClaimed = [&claimed](const uint32_t slot)
  { return std::find(claimed.begin(), claimed.end(), slot) != claimed.end(); };

  requests.reserve(std::min<size_t>(ordered.size(), maximumRequests));
  for (const auto &entry : ordered)
  {
    const TerrainGPUVGPage *page = pageSet.page(entry.slot);
    if (page == nullptr || !page->live)
      continue;
    // A chunk is built and installed whole, so demand for one of its extra page
    // slots is demand for the chunk. The address resolves to the primary slot.
    const uint32_t primary = pageSet.slotOf(page->address);
    if (primary == UINT32_MAX)
      continue;
    const TerrainGPUVGPage *owner = pageSet.page(primary);
    if (owner == nullptr || !owner->live || owner->installed)
      continue;
    if (alreadyClaimed(primary))
      continue;
    claimed.push_back(primary);
    requests.push_back({owner->address, primary, entry.priority});
    if (requests.size() >= maximumRequests)
      break;
  }
  return requests;
}

} // namespace voxel::svraster
