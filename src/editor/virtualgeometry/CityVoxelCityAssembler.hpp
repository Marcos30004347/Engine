#pragma once

#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace virtualgeometry {

// An offline placement builder. Each asset is compiled once; every dwelling,
// terrain tile or prop owner only contributes compact references to it.
class CityVoxelCityAssembler {
public:
  struct Part {
    uint64_t stableId=0;
    uint64_t geometryId=0;
    uint64_t appearanceId=0;
    std::array<int32_t,3> localOriginCell{};
  };
  struct Stats {
    uint32_t owners=0, placements=0, sharedGeometry=0, appearances=0;
    uint64_t placementBytes=0, encodedGeometryBytes=0, appearanceBytes=0;
  };

  CityVoxelCityAssembler(const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
      const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
      uint32_t pitchMicrometres);
  bool addOwner(uint64_t ownerId, const std::array<int32_t,3> &originCell,
      uint8_t quarterTurn, const std::vector<Part> &parts, std::string *error=nullptr);
  bool addOwnerFrame(uint64_t ownerId, const std::array<int64_t,3> &originMillimetres,
      uint16_t yawCode, const std::vector<Part> &parts, std::string *error=nullptr,
      uint64_t reportedOwnerId=0); // Nonzero: the audited owner when one
                                   // building spans several turned frames.
  bool finish(CityVoxelStaticScene &scene,
      std::unordered_map<uint64_t,uint64_t> &ownerByPlacement,
      std::string *error=nullptr) const;
  Stats stats() const;

private:
  const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry_;
  const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances_;
  uint32_t pitchMicrometres_=0;
  std::unordered_map<uint64_t,uint32_t> geometryIndex_, appearanceIndex_;
  std::unordered_set<uint64_t> placementIds_, ownerIds_;
  std::vector<CityVoxelStaticBuilding> buildings_;
  std::vector<CityVoxelStaticCell> cells_;
  std::vector<CityVoxelStaticPlacement> placements_;
  std::unordered_map<uint64_t,uint64_t> ownerByPlacement_;
};

} // namespace virtualgeometry
