#pragma once

#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "voxel/svraster/CityVoxelKit.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace voxel::svraster {

// Streams an offline city (.cvscene) into a virtual-geometry scene around the
// camera. The file runtime decides which spatial cells are resident; this
// turns a detailed cell into slice placements of shared kit geometries with
// atlas appearances, and takes them away again when the cell leaves. Shared
// geometry and appearances are reference counted across cells, so a wall bay
// used by a hundred nearby houses is installed once.
//
// Call `open` before the renderer creates its passes: it reserves the city
// atlas, whose buffers those passes capture.
//
// Placements are made per (cell, owner): an owner is one house. A house can
// be kept out by its distance (`ownerRadiusMetres`) or by the caller's
// filter, which is how a far proxy scene hands a house to the detailed scene
// and takes it back: the proxy stays until `ownerComplete` says every block
// of that house is placed.
class CityVoxelStreamingScene {
public:
  struct Settings {
    double radiusMetres = 220.0;           // Cells within this distance are detailed.
    // Houses whose centre is farther than this are not placed (0: no limit).
    // A placed house is kept up to 10% beyond it, so the edge does not flap.
    double ownerRadiusMetres = 0.0;
    uint32_t maximumActivePlacements = 40000;
    uint32_t maximumPlacementsPerUpdate = 8192;
    CityVoxelKit::Settings kit{};
    virtualgeometry::VirtualGeometryScene::CityVoxelAtlasSettings atlas{};
    virtualgeometry::CityVoxelSceneRuntime::Budgets budgets{};
    // City frame (metres) -> renderer frame: added to every placement.
    std::array<double, 3> renderOffsetMetres{};
  };
  struct Stats {
    uint32_t detailedCells = 0, activeCells = 0, activePlacements = 0, completeOwners = 0;
    uint32_t residentGeometries = 0, residentAppearances = 0;
    uint32_t deferredCells = 0;            // Wanted but over the placement budget.
    uint64_t installs = 0, failures = 0;
    std::string lastFailure;
  };

  bool open(virtualgeometry::VirtualGeometryScene &scene, const std::string &path,
            const Settings &settings, std::string *error = nullptr);
  bool opened() const { return scene_ != nullptr; }
  using OwnerFilter = std::function<bool(uint64_t ownerId)>;
  // `cameraRender` is in the renderer frame; the offset is removed here.
  bool update(uint64_t frame, const std::array<double, 3> &cameraRender, std::string *error = nullptr,
              const OwnerFilter &wantOwner = {});
  const Stats &stats() const { return stats_; }
  // Every block this file has for the house is placed.
  bool ownerComplete(uint64_t ownerId) const;
  // City-frame bounds of every spatial cell, for far-field and debug views.
  const virtualgeometry::CityVoxelSceneRuntime &runtime() const { return runtime_; }

private:
  struct Asset { uint32_t slot = UINT32_MAX; uint32_t references = 0; };
  struct ActivePlacement {
    virtualgeometry::VirtualGeometryScene::ModelPlacementHandle handle;
    uint32_t geometry = 0, appearance = 0;
  };
  struct Owner {
    uint64_t id = 0;
    std::array<double, 3> centre{}; // City frame.
    uint32_t units = 0, activeUnits = 0;
  };
  struct Unit { uint32_t owner = 0; std::vector<uint32_t> instances; };
  static uint64_t unitKey(uint32_t cell, uint32_t owner) { return (uint64_t(cell) << 32) | owner; }
  bool activate(uint32_t cell, const Unit &unit, std::string *error);
  void deactivate(uint64_t key);
  bool acquireGeometry(uint32_t geometry, std::string *error);
  bool acquireAppearance(uint32_t appearance, uint32_t geometry, std::string *error);
  void releaseGeometry(uint32_t geometry);
  void releaseAppearance(uint32_t appearance);

  virtualgeometry::VirtualGeometryScene *scene_ = nullptr;
  Settings settings_;
  virtualgeometry::CityVoxelSceneRuntime runtime_;
  virtualgeometry::CityVoxelStaticScene transforms_;
  CityVoxelKit kit_;
  std::unordered_map<uint32_t, Asset> geometries_, appearances_;
  std::vector<Owner> owners_;
  std::unordered_map<uint64_t, uint32_t> ownerIndex_;
  std::vector<std::vector<Unit>> cellUnits_;
  std::unordered_map<uint64_t, std::vector<ActivePlacement>> activeUnits_;
  uint32_t activePlacements_ = 0;
  uint64_t runtimeFrame_ = 0;
  Stats stats_;
};

} // namespace voxel::svraster
