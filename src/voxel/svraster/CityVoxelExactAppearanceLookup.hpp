#pragma once

#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "voxel/svraster/CityVoxelSurfaceAddress.hpp"

namespace voxel::svraster {

// CPU reference for the city material resolver. It reads the phase-4 encoded
// streams directly, keeping only one u32 prefix per sparse brick. Shader and
// ray-hit implementations must return these exact RGBA/profile values.
class CityVoxelExactAppearanceLookup {
public:
  bool bind(const CityVoxelCompiledGeometry &geometry,
            const CityVoxelCompiledAppearance &appearance,
            std::string *error = nullptr);
  bool lookupCell(const std::array<int32_t, 3> &cell,
                  uint32_t &rgba, uint32_t &materialProfile) const;
  bool lookupHit(const CityVoxelVGClusterSource &source,
                 const std::array<float, 3> &localPosition,
                 const std::array<float, 3> &localNormal,
                 const std::array<float, 2> &uv,
                 uint32_t &rgba, uint32_t &materialProfile) const;
  uint64_t scratchBytes() const { return brickBaseRanks_.capacity() * sizeof(uint32_t); }

private:
  const CityVoxelCompiledGeometry *geometry_ = nullptr;
  const CityVoxelCompiledAppearance *appearance_ = nullptr;
  std::vector<uint32_t> brickBaseRanks_;
  uint32_t paletteCount_ = 0;
  uint32_t overrideCount_ = 0;
};

} // namespace voxel::svraster
