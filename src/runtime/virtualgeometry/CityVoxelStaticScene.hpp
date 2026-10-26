#pragma once

#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "voxel/svraster/CityVoxelSceneFormat.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace virtualgeometry {

// Offline city placements stay in this compact, read-only form. Only placements
// in selected spatial cells are expanded into the legacy VG matrix buffer.
// IDs refer to scene-local model and building tables; no string or matrix is
// stored per placement. All coordinates use the same integer voxel lattice.
struct CityVoxelStaticPlacement {
  uint64_t stableId = 0;
  uint32_t geometryIndex = UINT32_MAX;
  uint32_t appearanceIndex = UINT32_MAX;
  uint32_t buildingIndex = UINT32_MAX;
  std::array<int32_t, 3> localOriginCell{};
};
static_assert(sizeof(CityVoxelStaticPlacement) == 32);

struct CityVoxelStaticModel {
  uint64_t geometryId = 0;
  std::string registeredName;
  uint32_t materialIndex = 0;
};

struct CityVoxelStaticBuilding {
  std::array<int32_t, 3> originCell{};
  uint32_t quarterTurns = 0;
  std::array<int64_t,3> originMillimetres{};
  uint16_t yawCode = 0;
  bool localFrame = false;
};

struct CityVoxelStaticCell {
  std::array<int32_t, 3> minCell{};
  std::array<int32_t, 3> maxCell{}; // Exclusive.
  uint32_t firstPlacement = 0;
  uint32_t placementCount = 0;
};
static_assert(sizeof(CityVoxelStaticCell) == 32);

class CityVoxelStaticScene {
public:
  using CellVisible = std::function<bool(const CityVoxelStaticCell &)>;
  struct Stats {
    uint64_t placementBytes = 0;
    uint64_t cellBytes = 0;
    uint32_t activePlacements = 0;
    uint32_t visibleCells = 0;
  };

  bool load(std::vector<CityVoxelStaticModel> models,
            std::vector<uint64_t> appearanceIds,
            std::vector<CityVoxelStaticBuilding> buildings,
            std::vector<CityVoxelStaticCell> cells,
            std::vector<CityVoxelStaticPlacement> placements,
            uint32_t pitchMicrometres, std::string *error = nullptr);
  // Reconstruct compact placement metadata directly from a .cvscene index.
  bool loadSceneIndex(const voxel::svraster::CityVoxelSceneIndex &,
                      std::string *error = nullptr);

  // Select coarse cells first, then stream their placements into the shared
  // renderer's bounded instance pool. A failed activation leaves its prior
  // placement live and may be retried on the next call.
  bool sync(VirtualGeometryScene &scene, const CellVisible &visible,
            uint32_t maximumActivePlacements, std::string *error = nullptr);
  bool installAppearanceAssets(
      VirtualGeometryScene &scene,
      const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
      const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
      const std::vector<CityVoxelGPUProfileSlot> &profileSlots,
      std::string *error = nullptr);
  void clear(VirtualGeometryScene &scene);
  Stats stats() const;
  bool stableIdForDenseInstance(const VirtualGeometryScene &scene, uint32_t denseInstance,
                                uint64_t &stableId) const;
  const std::vector<CityVoxelStaticPlacement> &placements() const { return placements_; }
  const std::vector<CityVoxelStaticModel> &models() const { return models_; }
  const std::vector<CityVoxelStaticBuilding> &buildings() const { return buildings_; }
  const std::vector<CityVoxelStaticCell> &cells() const { return cells_; }
  uint32_t pitchMicrometres() const { return pitchMicrometres_; }
  math::Vec3f worldPosition(const CityVoxelStaticPlacement &placement) const;
  math::Quatf worldRotation(const CityVoxelStaticPlacement &placement) const;
  uint64_t appearanceId(uint32_t index) const { return appearances_.at(placements_.at(index).appearanceIndex); }

private:
  std::vector<CityVoxelStaticModel> models_;
  std::vector<uint64_t> appearances_;
  std::vector<CityVoxelStaticBuilding> buildings_;
  std::vector<CityVoxelStaticCell> cells_;
  std::vector<CityVoxelStaticPlacement> placements_;
  std::unordered_map<uint32_t, VirtualGeometryScene::ModelPlacementHandle> active_;
  uint32_t pitchMicrometres_ = 100000;
  std::array<int64_t,3> cityOriginMillimetres_{};
  uint32_t visibleCellCount_ = 0;
  bool compactUploaded_ = false;
};

} // namespace virtualgeometry
