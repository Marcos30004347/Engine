#pragma once

#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace voxel::svraster {

struct CityVoxelVGClusterSource {
  // One record per linearized VG cluster. LOD0 yields exact appearance cells;
  // coarse LODs yield the parent cell and require a separate appearance rule.
  std::array<int32_t,3> brickOriginCells{};
  uint32_t cellScale = 1;
  uint32_t level = 0;
};

// Position is model-local metres; UV is the interpolated integer-lattice face
// UV emitted by the VG builder. The integer face plane comes from position and
// normal. This avoids a per-triangle voxel-address payload even after greedy
// merging. Only level 0 can directly index the exact appearance stream.
inline bool cityVoxelSourceCellFromHit(
    const CityVoxelVGClusterSource &source, const std::array<float,3> &position,
    const std::array<float,3> &normal, const std::array<float,2> &uv,
    uint32_t pitchMicrometres, std::array<int32_t,3> &cell) {
  if(!pitchMicrometres || !source.cellScale ||
     !std::isfinite(uv[0]) || !std::isfinite(uv[1]))return false;
  uint32_t axis=0;
  for(uint32_t i=1;i<3;++i)
    if(std::abs(normal[i])>std::abs(normal[axis]))axis=i;
  if(!std::isfinite(normal[axis]) || std::abs(normal[axis])<0.5f ||
     !std::isfinite(position[axis]))return false;
  const uint32_t direction=axis*2u+(normal[axis]>0.0f?1u:0u);
  const auto axes=sparseVoxelBrickFaceAxes(direction);
  const auto uvCell=[](float u) -> int64_t {
    if(u<=0.0f)return 0;
    if(u>=8.0f)return 7;
    return int64_t(std::floor(u));
  };
  std::array<int64_t,3> local{};
  local[axes[0]]=uvCell(uv[0]);
  local[axes[1]]=uvCell(uv[1]);
  const double pitch=double(pitchMicrometres)*1e-6;
  const double relative=(double(position[axis])/pitch-
      double(source.brickOriginCells[axis]))/double(source.cellScale);
  if(!std::isfinite(relative) || relative<-1.0 || relative>9.0)return false;
  const int64_t plane=std::llround(relative);
  local[axis]=plane-(direction&1u?1:0);
  if(local[axis]<0 || local[axis]>=8)return false;
  for(uint32_t i=0;i<3;++i) {
    const int64_t coordinate=int64_t(source.brickOriginCells[i])+
        local[i]*source.cellScale;
    if(coordinate<INT32_MIN || coordinate>INT32_MAX)return false;
    cell[i]=int32_t(coordinate);
  }
  return true;
}

} // namespace voxel::svraster
