#include "voxel/svraster/CityVoxelStreamingScene.hpp"

#include "virtualgeometry/VirtualGeometryFile.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <tuple>
#include <unordered_set>

namespace voxel::svraster {
namespace {
bool fail(std::string *error, const std::string &message) {
  if (error) *error = message;
  return false;
}
} // namespace

bool CityVoxelStreamingScene::open(virtualgeometry::VirtualGeometryScene &scene, const std::string &path,
                                   const Settings &settings, std::string *error) {
  if (scene_) return fail(error, "city streaming scene is already open");
  auto budgets = settings.budgets;
  budgets.requireTrace = false; // Raster-only city files carry no trace pages.
  budgets.requireVirtualGeometry = true;
  if (!runtime_.open(path, budgets, {}, error)) return false;
  if (!transforms_.loadSceneIndex(runtime_.index(), error)) return false;
  if (!scene.cityVoxelAtlasReserved() && !scene.reserveCityVoxelAtlas(settings.atlas, error)) return false;
  // Every profile shades through the base material; the exact voxel colour
  // arrives through the resolve's city albedo. Scenes share one atlas, so
  // this file's profiles join those already there.
  std::vector<virtualgeometry::CityVoxelGPUProfileSlot> profiles = scene.cityVoxelProfileSlots();
  for (const auto &material : runtime_.index().materials) profiles.push_back({material.profileId, 0u});
  std::sort(profiles.begin(), profiles.end(),
            [](const auto &a, const auto &b) { return a.profileId < b.profileId; });
  profiles.erase(std::unique(profiles.begin(), profiles.end(),
                             [](const auto &a, const auto &b) { return a.profileId == b.profileId; }),
                 profiles.end());
  if (!scene.setCityVoxelProfileSlots(profiles, error)) return false;

  // Houses: each owner's centre (from its placements' extent) and its
  // instances grouped per spatial cell.
  const auto &index = runtime_.index();
  std::vector<std::array<double, 6>> extent;
  cellUnits_.assign(index.spatialCells.size(), {});
  for (uint32_t cell = 0; cell < index.spatialCells.size(); ++cell) {
    const auto &record = index.spatialCells[cell];
    std::unordered_map<uint32_t, uint32_t> unitByOwner;
    for (uint32_t i = record.firstInstance; i < record.firstInstance + record.instanceCount; ++i) {
      const uint64_t id = index.instances[i].ownerId;
      auto [found, added] = ownerIndex_.try_emplace(id, uint32_t(owners_.size()));
      if (added) {
        owners_.push_back({id, {}, 0u, 0u});
        const double inf = std::numeric_limits<double>::infinity();
        extent.push_back({inf, inf, inf, -inf, -inf, -inf});
      }
      const uint32_t owner = found->second;
      const math::Vec3f at = transforms_.worldPosition(transforms_.placements()[i]);
      const double p[3] = {at.x(), at.y(), at.z()};
      auto &box = extent[owner];
      for (unsigned axis = 0; axis < 3; ++axis) {
        box[axis] = std::min(box[axis], p[axis]);
        box[axis + 3] = std::max(box[axis + 3], p[axis]);
      }
      auto [unit, fresh] = unitByOwner.try_emplace(owner, uint32_t(cellUnits_[cell].size()));
      if (fresh) {
        cellUnits_[cell].push_back({owner, {}});
        ++owners_[owner].units;
      }
      cellUnits_[cell][unit->second].instances.push_back(i);
    }
  }
  for (size_t o = 0; o < owners_.size(); ++o)
    for (unsigned axis = 0; axis < 3; ++axis)
      owners_[o].centre[axis] = 0.5 * (extent[o][axis] + extent[o][axis + 3]);
  scene.enableAccurateVisibilityInstanceIdentity();
  settings_ = settings;
  scene_ = &scene;
  return true;
}

bool CityVoxelStreamingScene::acquireGeometry(uint32_t geometry, std::string *error) {
  auto &asset = geometries_[geometry];
  if (asset.references++) return true;
  const auto fault = [&](const std::string &why) {
    asset.references = 0;
    geometries_.erase(geometry);
    return fail(error, why);
  };
  CityVoxelCompiledGeometry compiled;
  CityVoxelVGPageView view;
  std::string why;
  if (!runtime_.residentGeometry(geometry, compiled, &why) ||
      !runtime_.residentVirtualGeometry(geometry, view, &why))
    return fault("city geometry " + std::to_string(geometry) + " is not resident: " + why);
  if (!kit_.created()) {
    virtualgeometry::VirtualGeometryEncodedData first;
    if (!virtualgeometry::VirtualGeometryFile::readFromMemory(view.vgFileBytes, view.vgFileSize, first, &why))
      return fault(why);
    virtualgeometry::AABB bounds{};
    const float extent[2][3] = {{-1.0e5f, -1.0e4f, -1.0e5f}, {1.0e5f, 1.0e4f, 1.0e5f}};
    bounds.expandBy(extent[0]);
    bounds.expandBy(extent[1]);
    if (!kit_.create(*scene_, settings_.kit, first.quantizationConfig, bounds, &why)) return fault(why);
  }
  // Skip the atlas upload for a geometry the kit would refuse this frame.
  if (!kit_.accepting(compiled.id)) return fault("city kit BLAS queue is full; pending");
  if (!scene_->addCityVoxelAtlasGeometry(compiled, asset.slot, &why)) return fault(why);
  if (!kit_.install(compiled.id, view, &why)) {
    scene_->releaseCityVoxelAtlasGeometry(asset.slot);
    return fault(why);
  }
  ++stats_.installs;
  return true;
}

bool CityVoxelStreamingScene::acquireAppearance(uint32_t appearance, uint32_t geometry, std::string *error) {
  auto &asset = appearances_[appearance];
  if (asset.references++) return true;
  CityVoxelCompiledGeometry compiledGeometry;
  CityVoxelCompiledAppearance compiled;
  std::string why;
  if (!runtime_.residentGeometry(geometry, compiledGeometry, &why) ||
      !runtime_.residentAppearance(appearance, compiled, &why) ||
      !scene_->addCityVoxelAtlasAppearance(compiled, compiledGeometry, asset.slot, &why)) {
    asset.references = 0;
    appearances_.erase(appearance);
    return fail(error, "city appearance " + std::to_string(appearance) + ": " + why);
  }
  return true;
}

void CityVoxelStreamingScene::releaseGeometry(uint32_t geometry) {
  const auto found = geometries_.find(geometry);
  if (found == geometries_.end() || --found->second.references) return;
  kit_.retire(runtime_.index().geometry[geometry].id, nullptr);
  scene_->releaseCityVoxelAtlasGeometry(found->second.slot);
  geometries_.erase(found);
}

void CityVoxelStreamingScene::releaseAppearance(uint32_t appearance) {
  const auto found = appearances_.find(appearance);
  if (found == appearances_.end() || --found->second.references) return;
  scene_->releaseCityVoxelAtlasAppearance(found->second.slot);
  appearances_.erase(found);
}

bool CityVoxelStreamingScene::activate(uint32_t cell, const Unit &unit, std::string *error) {
  std::vector<ActivePlacement> placed;
  placed.reserve(unit.instances.size());
  const auto undo = [&] {
    for (const auto &entry : placed) {
      kit_.unplace(runtime_.index().geometry[entry.geometry].id, entry.handle);
      releaseGeometry(entry.geometry);
      releaseAppearance(entry.appearance);
    }
  };
  for (const uint32_t i : unit.instances) {
    const auto &placement = transforms_.placements()[i];
    const uint32_t geometry = placement.geometryIndex, appearance = placement.appearanceIndex;
    if (!acquireGeometry(geometry, error)) { undo(); return false; }
    if (!acquireAppearance(appearance, geometry, error)) { releaseGeometry(geometry); undo(); return false; }
    const math::Vec3f local = transforms_.worldPosition(placement);
    const math::Vec3f position(
        float(double(local.x()) + settings_.renderOffsetMetres[0]),
        float(double(local.y()) + settings_.renderOffsetMetres[1]),
        float(double(local.z()) + settings_.renderOffsetMetres[2]));
    ActivePlacement entry{{}, geometry, appearance};
    if (!kit_.place(runtime_.index().geometry[geometry].id, position, transforms_.worldRotation(placement),
                    entry.handle, error) ||
        !scene_->setModelPlacementCityMetadata(entry.handle, geometries_[geometry].slot,
                                               appearances_[appearance].slot, placement.stableId,
                                               (i % 0x7fffffffu) + 1u)) {
      if (entry.handle.valid()) kit_.unplace(runtime_.index().geometry[geometry].id, entry.handle);
      releaseGeometry(geometry);
      releaseAppearance(appearance);
      undo();
      return error && !error->empty() ? false : fail(error, "city placement metadata was refused");
    }
    placed.push_back(entry);
  }
  activePlacements_ += uint32_t(placed.size());
  ++owners_[unit.owner].activeUnits;
  activeUnits_[unitKey(cell, unit.owner)] = std::move(placed);
  return true;
}

void CityVoxelStreamingScene::deactivate(uint64_t key) {
  const auto found = activeUnits_.find(key);
  if (found == activeUnits_.end()) return;
  for (const auto &entry : found->second) {
    kit_.unplace(runtime_.index().geometry[entry.geometry].id, entry.handle);
    releaseGeometry(entry.geometry);
    releaseAppearance(entry.appearance);
  }
  activePlacements_ -= uint32_t(found->second.size());
  --owners_[uint32_t(key)].activeUnits;
  activeUnits_.erase(found);
}

bool CityVoxelStreamingScene::ownerComplete(uint64_t ownerId) const {
  const auto found = ownerIndex_.find(ownerId);
  if (found == ownerIndex_.end()) return false;
  const auto &owner = owners_[found->second];
  return owner.units && owner.activeUnits == owner.units;
}

bool CityVoxelStreamingScene::update(uint64_t frame, const std::array<double, 3> &cameraRender,
                                     std::string *error, const OwnerFilter &wantOwner) {
  if (!scene_) return fail(error, "city streaming scene is not open");
  const std::array<double, 3> camera{cameraRender[0] - settings_.renderOffsetMetres[0],
                                     cameraRender[1] - settings_.renderOffsetMetres[1],
                                     cameraRender[2] - settings_.renderOffsetMetres[2]};
  // The file runtime wants a strictly increasing frame above zero; callers'
  // counters may start at zero or repeat across a renderer rebuild.
  runtimeFrame_ = std::max(runtimeFrame_ + 1u, frame);
  if (!runtime_.updateRadius(runtimeFrame_, camera, settings_.radiusMetres, error)) return false;
  const auto ownerDistance = [&](uint32_t owner) {
    double d = 0.0;
    for (unsigned axis = 0; axis < 3; ++axis) {
      const double delta = owners_[owner].centre[axis] - camera[axis];
      d += delta * delta;
    }
    return std::sqrt(d);
  };
  std::unordered_set<uint64_t> wanted;
  std::vector<std::tuple<double, uint32_t, uint32_t>> pending; // distance, cell, unit
  uint32_t detailed = 0;
  for (const auto &cell : runtime_.publishedCells()) {
    if (!cell.detailed || !cell.instanceCount) continue;
    ++detailed;
    const auto &units = cellUnits_[cell.cellIndex];
    for (uint32_t u = 0; u < units.size(); ++u) {
      const uint32_t owner = units[u].owner;
      const uint64_t key = unitKey(cell.cellIndex, owner);
      const bool active = activeUnits_.count(key) != 0;
      const double distance = ownerDistance(owner);
      if (settings_.ownerRadiusMetres > 0.0 &&
          distance > settings_.ownerRadiusMetres * (active ? 1.1 : 1.0))
        continue;
      if (wantOwner && !wantOwner(owners_[owner].id)) continue;
      wanted.insert(key);
      if (!active) pending.push_back({distance, cell.cellIndex, u});
    }
  }
  scene_->beginModelPlacementBatch();
  std::vector<uint64_t> leaving;
  for (const auto &[key, placements] : activeUnits_)
    if (!wanted.count(key)) leaving.push_back(key);
  for (const uint64_t key : leaving) deactivate(key);
  // Nearest house first, so a budget-limited frame fills in around the camera.
  std::sort(pending.begin(), pending.end());
  uint32_t added = 0;
  stats_.deferredCells = 0;
  for (const auto &[distance, cell, u] : pending) {
    const Unit &unit = cellUnits_[cell][u];
    const uint32_t count = uint32_t(unit.instances.size());
    if (activePlacements_ + count > settings_.maximumActivePlacements ||
        added + count > settings_.maximumPlacementsPerUpdate) {
      ++stats_.deferredCells;
      continue;
    }
    std::string why;
    if (!activate(cell, unit, &why)) {
      ++stats_.failures;
      stats_.lastFailure = why;
      continue;
    }
    added += count;
  }
  scene_->endModelPlacementBatch();
  if (kit_.created()) kit_.endUpdate();
  std::unordered_set<uint32_t> activeCells;
  for (const auto &[key, placements] : activeUnits_) activeCells.insert(uint32_t(key >> 32));
  stats_.detailedCells = detailed;
  stats_.activeCells = uint32_t(activeCells.size());
  stats_.activePlacements = activePlacements_;
  stats_.completeOwners = 0;
  for (const auto &owner : owners_) stats_.completeOwners += owner.units && owner.activeUnits == owner.units;
  stats_.residentGeometries = uint32_t(geometries_.size());
  stats_.residentAppearances = uint32_t(appearances_.size());
  return true;
}

} // namespace voxel::svraster
