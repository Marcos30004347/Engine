#include "voxel/svraster/CityVoxelKit.hpp"

#include "virtualgeometry/VirtualGeometryFile.hpp"


#include <algorithm>

namespace voxel::svraster {
namespace {
bool fail(std::string *error, const std::string &message) {
  if (error) *error = message;
  return false;
}
} // namespace

bool CityVoxelKit::create(virtualgeometry::VirtualGeometryScene &scene, const Settings &settings,
                          const virtualgeometry::QuantizationConfig &quantization,
                          const virtualgeometry::AABB &bounds, std::string *error) {
  if (scene_) return fail(error, "city kit already created");
  if (!settings.slots || settings.maxNodesPerGeometry < 2 || !settings.pagesPerGeometry ||
      uint64_t(settings.slots) * settings.pagesPerGeometry > UINT32_MAX)
    return fail(error, "city kit settings are invalid");
  layout_.capacity = settings.slots;
  layout_.maxChunkNodes = settings.maxNodesPerGeometry;
  std::vector<virtualgeometry::VirtualGeometryHierarchy> hierarchy;
  if (!buildSparseVoxelTerrainVGSharedObjectHierarchy(layout_, bounds, hierarchy, error)) return false;
  virtualgeometry::VirtualGeometryScene::RuntimeObjectCreateInfo info{};
  info.name = settings.objectName;
  info.pageCount = settings.slots * settings.pagesPerGeometry;
  info.hierarchy = std::move(hierarchy);
  info.quantizationConfig = quantization;
  info.localBounds = bounds;
  // Only slot roots are ever instanced; an instance of the shared root would
  // draw every resident geometry at the origin.
  info.createDefaultPlacement = false;
  if (!scene.registerRuntimeObject(info, nullptr, error)) return false;
  // Slots are installed and retired by reference count here; the scene's LRU
  // eviction would take pages placements still point at.
  if (!scene.setRuntimeObjectExplicitResidency(settings.objectName, true, error)) return false;
  if (!settings.rayTraceCasters && !scene.setRuntimeObjectRayTraceCasters(settings.objectName, false, error))
    return false;
  if (!scene.setRuntimeObjectRayTracePageCut(settings.objectName, settings.rayTracePageCutError, error))
    return false;
  settings_ = settings;
  freeSlots_.clear();
  for (uint32_t slot = settings.slots; slot-- > 0;) freeSlots_.push_back(slot);
  slotByGeometry_.clear();
  stats_ = {};
  scene_ = &scene;
  return true;
}

std::vector<uint32_t> CityVoxelKit::pageSlotsFor(uint32_t slot, uint32_t pages) const {
  std::vector<uint32_t> result(pages);
  for (uint32_t page = 0; page < pages; ++page)
    result[page] = slot * settings_.pagesPerGeometry + page;
  return result;
}

bool CityVoxelKit::install(uint64_t geometryId, const CityVoxelVGPageView &view, std::string *error) {
  if (!scene_) return fail(error, "city kit is not created");
  if (slotByGeometry_.count(geometryId)) return true;
  if (!accepting(geometryId)) return fail(error, "city kit BLAS queue is full; pending");
  virtualgeometry::VirtualGeometryEncodedData asset;
  if (!virtualgeometry::VirtualGeometryFile::readFromMemory(view.vgFileBytes, view.vgFileSize, asset, error))
    return false;
  const uint32_t pages = uint32_t(asset.pages.size());
  if (!pages || pages > settings_.pagesPerGeometry || asset.hierarchy.empty() ||
      asset.hierarchy.size() > settings_.maxNodesPerGeometry) {
    ++stats_.rejected;
    return fail(error, "city geometry " + std::to_string(geometryId) + " exceeds a kit slot (" +
                       std::to_string(pages) + " pages, " + std::to_string(asset.hierarchy.size()) +
                       " nodes)");
  }
  uint64_t clusters = 0;
  for (const auto &page : asset.pages) clusters += page.meshlets.size();
  if (clusters != view.clusterSources.size())
    return fail(error, "city geometry cluster sources do not cover its pages");
  uint32_t slot;
  const auto reserved = pendingSlot_.find(geometryId);
  if (reserved != pendingSlot_.end()) {
    slot = reserved->second.slot;
    reserved->second.asked = true;
  } else {
    if (freeSlots_.empty()) return fail(error, "city kit has no free slot");
    slot = freeSlots_.back();
    freeSlots_.pop_back();
    pendingSlot_.emplace(geometryId, Pending{slot, true});
  }
  const auto release = [&] { pendingSlot_.erase(geometryId); };
  const auto pageSlots = pageSlotsFor(slot, pages);
  SparseVoxelTerrainVGEncodedAssetRuntimeInstallSettings install{};
  install.hasTracePayload = false;
  // The page BLAS is keyed by (slot page, generation, revision): a revision
  // of the geometry's own makes a recycled slot never match another
  // geometry's BLAS.
  install.geometryRevision = (geometryId ^ (geometryId >> 29)) | 1u;
  // Placements start at the slot root; see sliceRelativeChildren.
  install.sliceRelativeChildren = true;
  if (!installSparseVoxelTerrainVGChunkIntoSlot(*scene_, settings_.objectName, layout_, slot,
                                                pageSlots, asset, install, error)) {
    // Waiting on the BLAS: keep the slot. Anything else frees it.
    if (!(error && error->find("pending") != std::string::npos)) {
      release();
      freeSlots_.push_back(slot);
    }
    return false;
  }
  // Each page's clusters resolve exact appearance through these sources.
  size_t first = 0;
  for (uint32_t page = 0; page < pages; ++page) {
    const size_t count = asset.pages[page].meshlets.size();
    const std::vector<CityVoxelVGClusterSource> sources(view.clusterSources.begin() + first,
                                                        view.clusterSources.begin() + first + count);
    if (!scene_->setRuntimeObjectPageCityClusterSources(settings_.objectName, pageSlots[page],
                                                        sources, error))
      return false;
    first += count;
  }
  release();
  slotByGeometry_[geometryId] = {slot, 0u, pages};
  ++stats_.installs;
  return true;
}

bool CityVoxelKit::place(uint64_t geometryId, const math::Vec3f &position, const math::Quatf &rotation,
                         virtualgeometry::VirtualGeometryScene::ModelPlacementHandle &out,
                         std::string *error) {
  const auto found = slotByGeometry_.find(geometryId);
  if (found == slotByGeometry_.end()) return fail(error, "city geometry is not resident in the kit");
  if (!scene_->instantiateModelSlicePlacement(settings_.objectName,
          layout_.nodeIndex(found->second.index, 0u), position, rotation, 1.0f, 0u, out, error))
    return false;
  ++found->second.placements;
  ++stats_.placements;
  return true;
}

void CityVoxelKit::unplace(uint64_t geometryId,
                           virtualgeometry::VirtualGeometryScene::ModelPlacementHandle placement) {
  const auto found = slotByGeometry_.find(geometryId);
  if (found == slotByGeometry_.end() || !scene_->destroyModelPlacement(placement)) return;
  if (found->second.placements) --found->second.placements;
  if (stats_.placements) --stats_.placements;
}

bool CityVoxelKit::retire(uint64_t geometryId, std::string *error) {
  const auto found = slotByGeometry_.find(geometryId);
  if (found == slotByGeometry_.end()) return true;
  if (found->second.placements) return fail(error, "city geometry still has placements");
  const auto pageSlots = pageSlotsFor(found->second.index, found->second.pages);
  // Pages depend on each other and the scene refuses to drop a page something
  // still depends on; each pass removes what nothing depends on any more.
  for (uint32_t pass = 0; pass <= found->second.pages; ++pass) {
    uint32_t remaining = 0;
    for (const uint32_t pageSlot : pageSlots) {
      const auto *status = scene_->getRuntimeObjectPageStatus(settings_.objectName, pageSlot);
      if (!status || !status->installed) continue;
      if (scene_->uninstallRuntimeObjectPage(settings_.objectName, pageSlot, 0u, nullptr)) {
        scene_->forgetRuntimeObjectPageSource(settings_.objectName, pageSlot);
        scene_->setRuntimeObjectPageCityClusterSources(settings_.objectName, pageSlot, {}, nullptr);
      } else {
        ++remaining;
      }
    }
    if (!remaining) {
      freeSlots_.push_back(found->second.index);
      slotByGeometry_.erase(found);
      ++stats_.retirements;
      return true;
    }
  }
  return fail(error, "city geometry pages are still depended on");
}

void CityVoxelKit::endUpdate() {
  for (auto it = pendingSlot_.begin(); it != pendingSlot_.end();) {
    if (it->second.asked) {
      it->second.asked = false;
      ++it;
      continue;
    }
    // Its prepared BLAS stays cached under this slot until the next geometry
    // installed here retires the other versions.
    freeSlots_.push_back(it->second.slot);
    it = pendingSlot_.erase(it);
  }
}

uint32_t CityVoxelKit::placementCount(uint64_t geometryId) const {
  const auto found = slotByGeometry_.find(geometryId);
  return found == slotByGeometry_.end() ? 0u : found->second.placements;
}

CityVoxelKit::Stats CityVoxelKit::stats() const {
  Stats result = stats_;
  result.residentGeometries = uint32_t(slotByGeometry_.size());
  result.freeSlots = uint32_t(freeSlots_.size());
  return result;
}

} // namespace voxel::svraster
