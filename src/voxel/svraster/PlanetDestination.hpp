#pragma once

#include "procedural/PlanetCubeMapping.hpp"
#include "procedural/PlanetPackage.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace voxel::svraster
{

struct PlanetDestinationSelection
{
  procedural::SpherePoint direction{};
  procedural::PlanetTileAddress tile{};
  procedural::PlanetTileAddress heightSourceTile{};
  procedural::PlanetTileAddress materialSourceTile{};
  double elevationMetres = 0.0;
  uint8_t material = 0u;
};

const char *planetSurfaceMaterialName(uint8_t material);

// Reuses independently compressed raster blocks across selections. The package
// reader must outlive the sampler.
class PlanetDestinationSampler
{
public:
  explicit PlanetDestinationSampler(const procedural::PlanetPackageReader &package) : package_(&package)
  {
  }

  bool sample(const procedural::SpherePoint &direction, uint8_t targetLevel, PlanetDestinationSelection &selection, std::string *error = nullptr);

  void clearCache()
  {
    blockCache_.clear();
  }

private:
  bool block(const procedural::PlanetPackageBlockRecord &record, const std::vector<uint8_t> *&bytes, std::string *error);

  const procedural::PlanetPackageReader *package_ = nullptr;
  std::map<procedural::PlanetPackageBlockKey, std::vector<uint8_t>> blockCache_;
};

} // namespace voxel::svraster
