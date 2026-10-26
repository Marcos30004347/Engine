#include "editor/virtualgeometry/CityVoxelTerrainTileSampler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace virtualgeometry {
namespace {
bool fail(std::string *error,const char *reason) {
  if(error)*error=reason;
  return false;
}
}

bool sampleCityVoxelTerrainTile(const CityVoxelTerrainTileSpec &spec,
    const std::function<double(double,double)> &height,
    std::vector<voxel::svraster::CityVoxelCell> &result,std::string *error) {
  if(error)error->clear();
  if(!height||!std::isfinite(spec.voxelMetres)||spec.voxelMetres<=0||
     !std::isfinite(spec.originX)||!std::isfinite(spec.originZ)||
     spec.cellsPerSide==0||spec.cellsPerSide>512||spec.overlapHaloCells>2||!spec.maximumCells||
     !spec.materialProfile) return fail(error,"Invalid terrain tile specification");
  const int64_t firstX=int64_t(spec.tileX)*spec.cellsPerSide;
  const int64_t firstZ=int64_t(spec.tileZ)*spec.cellsPerSide;
  if(firstX<INT32_MIN+int64_t(spec.overlapHaloCells)+1||
     firstZ<INT32_MIN+int64_t(spec.overlapHaloCells)+1||
     firstX+spec.cellsPerSide+spec.overlapHaloCells>=INT32_MAX-1||
     firstZ+spec.cellsPerSide+spec.overlapHaloCells>=INT32_MAX-1)
    return fail(error,"Terrain tile address exceeds lattice range");
  const auto elevation=[&](int64_t x,int64_t z,int32_t &y) {
    const double wx=spec.originX+(double(x)+.5)*spec.voxelMetres;
    const double wz=spec.originZ+(double(z)+.5)*spec.voxelMetres;
    const double metres=height(wx,wz);
    const double cell=std::floor(metres/spec.voxelMetres);
    if(!std::isfinite(cell)||cell<INT32_MIN+1||cell>INT32_MAX-1) return false;
    y=int32_t(cell);return true;
  };
  std::vector<voxel::svraster::CityVoxelCell> next;
  const int32_t side=int32_t(spec.cellsPerSide),halo=int32_t(spec.overlapHaloCells);
  next.reserve(std::min<uint64_t>(uint64_t(side+2*halo)*(side+2*halo),spec.maximumCells));
  for(int32_t z=-halo;z<side+halo;++z) for(int32_t x=-halo;x<side+halo;++x) {
    const int64_t gx=firstX+x,gz=firstZ+z;
    const double wx=spec.originX+(double(gx)+.5)*spec.voxelMetres;
    const double wz=spec.originZ+(double(gz)+.5)*spec.voxelMetres;
    if(spec.excludeColumn && spec.excludeColumn(wx,wz))continue;
    int32_t top=0,lowest=INT32_MAX;
    if(!elevation(gx,gz,top)) return fail(error,"Nonfinite terrain height");
    for(const auto offset:{std::pair<int, int>{-1,0},{1,0},{0,-1},{0,1}}) {
      int32_t neighbour=0;
      if(!elevation(gx+offset.first,gz+offset.second,neighbour))
        return fail(error,"Nonfinite terrain halo height");
      lowest=std::min(lowest,neighbour);
    }
    // A top voxel plus the exposed side column. No buried fill is emitted.
    const int64_t bottom=std::min<int64_t>(top,int64_t(lowest)+1);
    if(uint64_t(top-bottom+1)>spec.maximumCells-next.size())
      return fail(error,"Terrain tile cell budget exceeded");
    const uint32_t hash=(uint32_t(gx)*0x9e3779b1u)^(uint32_t(gz)*0x85ebca6bu);
    const uint8_t variation=uint8_t((hash^(hash>>13u))&15u);
    const int32_t slope=top-lowest;
    for(int64_t y=bottom;y<=top;++y) {
      uint32_t rgba=spec.rgba,profile=spec.materialProfile;
      if(spec.naturalPalette) {
        if(y<top) {rgba=0xff4f5e66u+variation;profile=6;}
        else if(slope>5) {rgba=0xff81898bu+variation;profile=1;}
        else {rgba=0xff3e6c51u+variation;profile=6;}
      }
      if(y==top && spec.topAppearance) {
        const auto authored=spec.topAppearance(wx,wz);
        if(!authored.second || (authored.first>>24u)!=255u)
          return fail(error,"Invalid authored terrain appearance");
        rgba=authored.first;profile=authored.second;
      }
      next.push_back({{int32_t(gx),int32_t(y),int32_t(gz)},rgba,profile});
    }
  }
  result=std::move(next);
  return true;
}

bool sampleCityVoxelBaseTile(const CityVoxelTerrainTileSpec &spec,
    const std::vector<CityVoxelBasePrism> &prisms,
    std::vector<voxel::svraster::CityVoxelCell> &result,std::string *error,
    const std::function<double(double,double)> &ground) {
  if(error)error->clear();
  if(!std::isfinite(spec.voxelMetres)||spec.voxelMetres<=0||
     !std::isfinite(spec.originX)||!std::isfinite(spec.originZ)||
     !spec.cellsPerSide||spec.cellsPerSide>512||!spec.maximumCells)
    return fail(error,"Invalid base tile specification");
  const int64_t firstX=int64_t(spec.tileX)*spec.cellsPerSide;
  const int64_t firstZ=int64_t(spec.tileZ)*spec.cellsPerSide;
  if(firstX<INT32_MIN||firstZ<INT32_MIN||
     firstX+spec.cellsPerSide>INT32_MAX||firstZ+spec.cellsPerSide>INT32_MAX)
    return fail(error,"Base tile address exceeds lattice range");
  struct Candidate {const CityVoxelBasePrism *prism;procedural::geom2::Box box;};
  std::vector<Candidate> candidates;
  for(const auto &prism:prisms) {
    if(prism.footprint.empty())continue;
    if(!std::isfinite(prism.bottomMetres)||!std::isfinite(prism.topMetres)||
       prism.topMetres<prism.bottomMetres||!prism.topMaterial||!prism.sideMaterial||
       (prism.topRgba>>24u)!=255u||(prism.sideRgba>>24u)!=255u)
      return fail(error,"Invalid authored base prism");
    const auto box=procedural::geom2::bounds(prism.footprint);
    const double loX=spec.originX+(double(firstX)-.5)*spec.voxelMetres;
    const double loZ=spec.originZ+(double(firstZ)-.5)*spec.voxelMetres;
    const double hiX=loX+double(spec.cellsPerSide+1)*spec.voxelMetres;
    const double hiZ=loZ+double(spec.cellsPerSide+1)*spec.voxelMetres;
    if(double(box.maxX)<double(procedural::geom2::toUnit(loX))||
       double(box.minX)>double(procedural::geom2::toUnit(hiX))||
       double(box.maxY)<double(procedural::geom2::toUnit(loZ))||
       double(box.minY)>double(procedural::geom2::toUnit(hiZ)))continue;
    if(prism.terrainFollowing&&!ground)
      return fail(error,"Terrain-following base prism requires ground field");
    candidates.push_back({&prism,box});
  }
  if(candidates.empty()){result.clear();return true;}
  struct Sample {const Candidate *candidate=nullptr;int32_t bottom=0,top=0;};
  bool valid=true;
  const auto at=[&](int64_t gx,int64_t gz)->Sample {
    const double wx=spec.originX+(double(gx)+.5)*spec.voxelMetres;
    const double wz=spec.originZ+(double(gz)+.5)*spec.voxelMetres;
    const auto p=procedural::geom2::Point{
        procedural::geom2::toUnit(wx),procedural::geom2::toUnit(wz)};
    Sample selected;
    for(const auto &candidate:candidates) {
      if(!candidate.box.contains(p)||!procedural::geom2::contains(candidate.prism->footprint,p))continue;
      const double offset=candidate.prism->terrainFollowing?ground(wx,wz):0.0;
      const double low=std::floor((offset+candidate.prism->bottomMetres)/spec.voxelMetres);
      const double high=std::floor((offset+candidate.prism->topMetres)/spec.voxelMetres);
      if(!std::isfinite(low)||!std::isfinite(high)||low<INT32_MIN||high>INT32_MAX) {
        valid=false;return {};
      }
      if(!selected.candidate||high>selected.top)
        selected={&candidate,int32_t(low),int32_t(high)};
    }
    return selected;
  };
  std::vector<voxel::svraster::CityVoxelCell> next;
  for(uint32_t z=0;z<spec.cellsPerSide;++z)for(uint32_t x=0;x<spec.cellsPerSide;++x) {
    const int64_t gx=firstX+x,gz=firstZ+z;
    const Sample own=at(gx,gz);
    if(!valid)return fail(error,"Base prism elevation exceeds lattice range");
    if(!own.candidate)continue;
    int32_t bottom=own.top;
    for(const auto offset:{std::pair<int,int>{-1,0},{1,0},{0,-1},{0,1}}) {
      const Sample adjacent=at(gx+offset.first,gz+offset.second);
      if(!valid)return fail(error,"Base prism elevation exceeds lattice range");
      if(!adjacent.candidate||adjacent.top<own.top)
        bottom=std::min(bottom,adjacent.candidate?
            std::max(own.bottom,adjacent.top+1):own.bottom);
    }
    if(uint64_t(int64_t(own.top)-bottom+1)>spec.maximumCells-next.size())
      return fail(error,"Base tile cell budget exceeded");
    for(int64_t y=bottom;y<=own.top;++y)
      next.push_back({{int32_t(gx),int32_t(y),int32_t(gz)},
          y==own.top?own.candidate->prism->topRgba:own.candidate->prism->sideRgba,
          y==own.top?own.candidate->prism->topMaterial:own.candidate->prism->sideMaterial});
  }
  result=std::move(next);
  return true;
}
} // namespace virtualgeometry
