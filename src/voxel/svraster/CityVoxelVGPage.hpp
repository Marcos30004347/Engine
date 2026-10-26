#pragma once
#include "voxel/svraster/CityVoxelSurfaceAddress.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster {
struct CityVoxelVGPageView {
  std::vector<CityVoxelVGClusterSource> clusterSources;
  const uint8_t *vgFileBytes=nullptr;
  uint32_t vgFileSize=0;
};
bool decodeCityVoxelVGPage(const uint8_t *,size_t,uint64_t geometryId,
    uint64_t geometryHash,CityVoxelVGPageView &,std::string *error=nullptr);
}
