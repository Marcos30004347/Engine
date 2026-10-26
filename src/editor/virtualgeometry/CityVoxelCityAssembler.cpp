#include "editor/virtualgeometry/CityVoxelCityAssembler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace virtualgeometry {
namespace {
bool fail(std::string *error,const char *message) {
  if(error)*error=message;
  return false;
}
std::array<int64_t,3> rotate(const std::array<int64_t,3> &p,uint8_t turn) {
  switch(turn) {
    case 1:return {-p[2],p[1],p[0]};
    case 2:return {-p[0],p[1],-p[2]};
    case 3:return {p[2],p[1],-p[0]};
    default:return p;
  }
}
}

CityVoxelCityAssembler::CityVoxelCityAssembler(
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    uint32_t pitchMicrometres)
    :geometry_(geometry),appearances_(appearances),pitchMicrometres_(pitchMicrometres) {
  for(uint32_t i=0;i<geometry.size();++i) geometryIndex_.emplace(geometry[i].id,i);
  for(uint32_t i=0;i<appearances.size();++i) appearanceIndex_.emplace(appearances[i].id,i);
}

bool CityVoxelCityAssembler::addOwner(uint64_t ownerId,
    const std::array<int32_t,3> &originCell,uint8_t quarterTurn,
    const std::vector<Part> &parts,std::string *error) {
  if(error)error->clear();
  if(!ownerId || ownerIds_.count(ownerId) || quarterTurn>3 || parts.empty() ||
      parts.size()>UINT32_MAX-placements_.size() || !pitchMicrometres_)
    return fail(error,"Invalid or duplicate city owner placement batch");
  std::unordered_set<uint64_t> localIds;
  localIds.reserve(parts.size());
  std::vector<CityVoxelStaticPlacement> pending;
  pending.reserve(parts.size());
  std::array<int64_t,3> low{INT64_MAX,INT64_MAX,INT64_MAX};
  std::array<int64_t,3> high{INT64_MIN,INT64_MIN,INT64_MIN};
  for(const auto &part:parts) {
    const auto g=geometryIndex_.find(part.geometryId);
    const auto a=appearanceIndex_.find(part.appearanceId);
    if(!part.stableId || !localIds.insert(part.stableId).second ||
        placementIds_.count(part.stableId) || g==geometryIndex_.end() ||
        a==appearanceIndex_.end())
      return fail(error,"Part has duplicate identity or unknown asset");
    const auto &asset=geometry_[g->second];
    const auto &appearance=appearances_[a->second];
    if(asset.pitchMicrometres!=pitchMicrometres_ ||
        appearance.geometryId!=asset.id ||
        appearance.geometryAddressHash!=asset.addressHash ||
        !asset.extent[0] || !asset.extent[1] || !asset.extent[2]) {
      if(error)*error="Part geometry, appearance or pitch differs: placement="+
          std::to_string(part.stableId)+" geometry="+std::to_string(asset.id)+
          " pitch="+std::to_string(asset.pitchMicrometres)+
          " expected="+std::to_string(pitchMicrometres_)+
          " appearanceGeometry="+std::to_string(appearance.geometryId);
      return false;
    }
    std::array<int64_t,3> local{};
    for(unsigned axis=0;axis<3;++axis)local[axis]=part.localOriginCell[axis];
    const auto rotatedOrigin=rotate(local,quarterTurn);
    for(unsigned corner=0;corner<8;++corner) {
      std::array<int64_t,3> point{
          corner&1u?asset.extent[0]:0,
          corner&2u?asset.extent[1]:0,
          corner&4u?asset.extent[2]:0};
      const auto rotated=rotate(point,quarterTurn);
      for(unsigned axis=0;axis<3;++axis) {
        const int64_t world=int64_t(originCell[axis])+rotatedOrigin[axis]+rotated[axis];
        low[axis]=std::min(low[axis],world);
        high[axis]=std::max(high[axis],world);
      }
    }
    pending.push_back({part.stableId,g->second,a->second,
        uint32_t(buildings_.size()),part.localOriginCell});
  }
  CityVoxelStaticCell cell;
  for(unsigned axis=0;axis<3;++axis) {
    if(low[axis]<INT32_MIN || high[axis]>INT32_MAX || low[axis]>=high[axis])
      return fail(error,"City owner extent exceeds signed voxel grid");
    cell.minCell[axis]=int32_t(low[axis]);
    cell.maxCell[axis]=int32_t(high[axis]);
  }
  cell.firstPlacement=uint32_t(placements_.size());
  cell.placementCount=uint32_t(pending.size());
  buildings_.push_back({originCell,quarterTurn});
  cells_.push_back(cell);
  for(const auto &part:pending) {
    placementIds_.insert(part.stableId);
    ownerByPlacement_.emplace(part.stableId,ownerId);
  }
  ownerIds_.insert(ownerId);
  placements_.insert(placements_.end(),pending.begin(),pending.end());
  return true;
}

bool CityVoxelCityAssembler::addOwnerFrame(uint64_t ownerId,
    const std::array<int64_t,3> &originMillimetres,uint16_t yawCode,
    const std::vector<Part> &parts,std::string *error,uint64_t reportedOwnerId) {
  if(error)error->clear();
  if(!reportedOwnerId)reportedOwnerId=ownerId;
  if(!ownerId || ownerIds_.count(ownerId) || parts.empty() || !pitchMicrometres_ ||
      parts.size()>UINT32_MAX-placements_.size())
    return fail(error,"Invalid or duplicate city frame owner");
  constexpr double tau=6.2831853071795864769;
  const double angle=tau*double(yawCode)/65536.0;
  const double c=std::cos(angle),s=std::sin(angle);
  const double pitch=double(pitchMicrometres_)*0.001; // millimetres
  std::unordered_set<uint64_t> localIds;
  std::vector<CityVoxelStaticPlacement> pending;
  pending.reserve(parts.size());
  std::array<double,3> low{INFINITY,INFINITY,INFINITY},high{-INFINITY,-INFINITY,-INFINITY};
  for(const auto &part:parts) {
    const auto g=geometryIndex_.find(part.geometryId);
    const auto a=appearanceIndex_.find(part.appearanceId);
    if(!part.stableId || !localIds.insert(part.stableId).second ||
        placementIds_.count(part.stableId) || g==geometryIndex_.end() ||
        a==appearanceIndex_.end())
      return fail(error,"Frame part has duplicate identity or unknown asset");
    const auto &asset=geometry_[g->second];
    const auto &appearance=appearances_[a->second];
    if(asset.pitchMicrometres!=pitchMicrometres_ ||
        appearance.geometryId!=asset.id || appearance.geometryAddressHash!=asset.addressHash ||
        !asset.extent[0] || !asset.extent[1] || !asset.extent[2])
      return fail(error,"Frame part geometry or appearance differs");
    for(unsigned corner=0;corner<8;++corner) {
      const double x=double(part.localOriginCell[0])+((corner&1u)?asset.extent[0]:0);
      const double y=double(part.localOriginCell[1])+((corner&2u)?asset.extent[1]:0);
      const double z=double(part.localOriginCell[2])+((corner&4u)?asset.extent[2]:0);
      const std::array<double,3> world{
          (double(originMillimetres[0])+pitch*(c*x-s*z))/pitch,
          (double(originMillimetres[1])+pitch*y)/pitch,
          (double(originMillimetres[2])+pitch*(s*x+c*z))/pitch};
      for(unsigned axis=0;axis<3;++axis) {
        low[axis]=std::min(low[axis],world[axis]);
        high[axis]=std::max(high[axis],world[axis]);
      }
    }
    pending.push_back({part.stableId,g->second,a->second,
        uint32_t(buildings_.size()),part.localOriginCell});
  }
  CityVoxelStaticCell cell;
  for(unsigned axis=0;axis<3;++axis) {
    const double minimum=std::floor(low[axis]),maximum=std::ceil(high[axis]);
    if(!std::isfinite(minimum)||!std::isfinite(maximum)||
        minimum<INT32_MIN||maximum>INT32_MAX||minimum>=maximum)
      return fail(error,"Frame owner exceeds signed city lattice");
    cell.minCell[axis]=int32_t(minimum);cell.maxCell[axis]=int32_t(maximum);
  }
  cell.firstPlacement=uint32_t(placements_.size());
  cell.placementCount=uint32_t(pending.size());
  CityVoxelStaticBuilding building;
  building.originMillimetres=originMillimetres;
  building.yawCode=yawCode;
  building.localFrame=true;
  buildings_.push_back(building);
  cells_.push_back(cell);
  for(const auto &part:pending) {
    placementIds_.insert(part.stableId);
    ownerByPlacement_.emplace(part.stableId,reportedOwnerId);
  }
  ownerIds_.insert(ownerId);
  placements_.insert(placements_.end(),pending.begin(),pending.end());
  return true;
}

bool CityVoxelCityAssembler::finish(CityVoxelStaticScene &scene,
    std::unordered_map<uint64_t,uint64_t> &ownerByPlacement,
    std::string *error) const {
  if(error)error->clear();
  if(placements_.empty() || geometry_.empty() || appearances_.empty())
    return fail(error,"City assembler has no complete assets or placements");
  std::vector<CityVoxelStaticModel> models;
  models.reserve(geometry_.size());
  for(const auto &asset:geometry_)
    models.push_back({asset.id,"city-geometry-"+std::to_string(asset.id),0u});
  std::vector<uint64_t> appearanceIds;
  appearanceIds.reserve(appearances_.size());
  for(const auto &asset:appearances_)appearanceIds.push_back(asset.id);
  if(!scene.load(std::move(models),std::move(appearanceIds),buildings_,cells_,
       placements_,pitchMicrometres_,error))return false;
  ownerByPlacement=ownerByPlacement_;
  return true;
}

CityVoxelCityAssembler::Stats CityVoxelCityAssembler::stats() const {
  Stats result;
  result.owners=uint32_t(ownerIds_.size());
  result.placements=uint32_t(placements_.size());
  result.sharedGeometry=uint32_t(geometry_.size());
  result.appearances=uint32_t(appearances_.size());
  result.placementBytes=buildings_.size()*sizeof(CityVoxelStaticBuilding)+
      cells_.size()*sizeof(CityVoxelStaticCell)+
      placements_.size()*sizeof(CityVoxelStaticPlacement);
  for(const auto &asset:geometry_)result.encodedGeometryBytes+=asset.bytes.size();
  for(const auto &asset:appearances_)result.appearanceBytes+=asset.bytes.size();
  return result;
}
} // namespace virtualgeometry
