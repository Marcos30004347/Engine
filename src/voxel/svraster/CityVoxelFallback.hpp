#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster {
struct CityVoxelFallbackProxy {
  std::array<int32_t,3> minimumCell{}, maximumCell{};
  uint32_t instanceCount=0;
};
bool encodeCityVoxelFallback(const CityVoxelFallbackProxy &,std::vector<uint8_t> &,
                             std::string *error=nullptr);
bool decodeCityVoxelFallback(const uint8_t *,size_t,CityVoxelFallbackProxy &,
                             std::string *error=nullptr);
}
