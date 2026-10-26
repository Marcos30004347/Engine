#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace virtualgeometry {
namespace {
bool fail(std::string *error, const std::string &message) {
  if (error) *error = message;
  return false;
}
struct PlacementBatch {
  VirtualGeometryScene &scene;
  explicit PlacementBatch(VirtualGeometryScene &value) : scene(value) { scene.beginModelPlacementBatch(); }
  ~PlacementBatch() { scene.endModelPlacementBatch(); }
};
} // namespace

bool CityVoxelStaticScene::load(
    std::vector<CityVoxelStaticModel> models,
    std::vector<uint64_t> appearanceIds,
    std::vector<CityVoxelStaticBuilding> buildings,
    std::vector<CityVoxelStaticCell> cells,
    std::vector<CityVoxelStaticPlacement> placements,
    uint32_t pitchMicrometres, std::string *error) {
  if (error) error->clear();
  if (!active_.empty()) return fail(error, "clear active city placements before reloading");
  if (pitchMicrometres == 0) return fail(error, "city voxel pitch must be positive");
  if (placements.size() >= 0x80000000ull) return fail(error, "too many city placements");
  std::unordered_set<uint64_t> stableIds;
  stableIds.reserve(placements.size());
  for (const auto &model : models)
    if (model.geometryId == 0 || model.registeredName.empty())
      return fail(error, "city model needs a geometry ID and registered VG name");
  for (uint64_t appearanceId : appearanceIds)
    if (appearanceId == 0u)
      return fail(error, "city appearance ID must be non-zero");
  for (const auto &building : buildings)
    if (building.quarterTurns > 3u || (building.localFrame && building.quarterTurns))
      return fail(error, "city building rotation must be a quarter turn");
  for (const auto &placement : placements) {
    if (placement.stableId == 0u)
      return fail(error, "city placement stable ID must be non-zero");
    if (placement.geometryIndex >= models.size() ||
        placement.appearanceIndex >= appearanceIds.size() ||
        placement.buildingIndex >= buildings.size())
      return fail(error, "city placement table reference out of range");
    if (!stableIds.insert(placement.stableId).second)
      return fail(error, "duplicate city placement stable ID");
  }
  uint64_t next = 0;
  for (const auto &cell : cells) {
    if (cell.firstPlacement != next ||
        static_cast<uint64_t>(cell.firstPlacement) + cell.placementCount > placements.size())
      return fail(error, "city cells must partition the placement array in order");
    for (uint32_t axis = 0; axis < 3; ++axis)
      if (cell.minCell[axis] >= cell.maxCell[axis])
        return fail(error, "city spatial cell has invalid bounds");
    next += cell.placementCount;
  }
  if (next != placements.size())
    return fail(error, "city spatial cells do not cover every placement");
  models_ = std::move(models);
  appearances_ = std::move(appearanceIds);
  buildings_ = std::move(buildings);
  cells_ = std::move(cells);
  placements_ = std::move(placements);
  pitchMicrometres_ = pitchMicrometres;
  cityOriginMillimetres_={};
  visibleCellCount_ = 0;
  compactUploaded_ = false;
  return true;
}

bool CityVoxelStaticScene::loadSceneIndex(
    const voxel::svraster::CityVoxelSceneIndex &index,std::string *error) {
  if (error) error->clear();
  std::vector<CityVoxelStaticModel> models;
  std::vector<uint64_t> appearances;
  std::unordered_map<uint64_t,uint32_t> geometryById,appearanceById;
  for(uint32_t i=0;i<index.geometry.size();++i) {
    if(index.geometry[i].pitchMicrometres!=index.nominalPitchMicrometres ||
        !geometryById.emplace(index.geometry[i].id,i).second)
      return fail(error,"City file model pitch or identity is invalid");
    models.push_back({index.geometry[i].id,
        "city-geometry-"+std::to_string(index.geometry[i].id),0u});
  }
  for(uint32_t i=0;i<index.appearances.size();++i) {
    if(!appearanceById.emplace(index.appearances[i].id,i).second)
      return fail(error,"City file appearance identity is duplicated");
    appearances.push_back(index.appearances[i].id);
  }
  std::vector<CityVoxelStaticBuilding> buildings(4u);
  for(uint32_t turn=0;turn<4u;++turn)buildings[turn].quarterTurns=turn;
  std::vector<uint32_t> frameBuilding(index.spatialCells.size(),UINT32_MAX);
  std::vector<uint32_t> cellForInstance(index.instances.size(),UINT32_MAX);
  for(uint32_t cellIndex=0;cellIndex<index.spatialCells.size();++cellIndex) {
    const auto &cell=index.spatialCells[cellIndex];
    if(!cell.instanceCount)continue;
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i)
      cellForInstance[i]=cellIndex;
    const auto &first=index.instances[cell.firstInstance];
    if(!(first.flags & voxel::svraster::kCityVoxelInstanceLocalFrame))continue;
    CityVoxelStaticBuilding building;
    building.localFrame=true;
    building.originMillimetres=cell.originMillimetres;
    building.yawCode=first.frameId;
    frameBuilding[cellIndex]=uint32_t(buildings.size());
    buildings.push_back(building);
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i)
      if(!(index.instances[i].flags & voxel::svraster::kCityVoxelInstanceLocalFrame) ||
          index.instances[i].frameId!=first.frameId)
        return fail(error,"City local frame differs within one spatial cell");
  }
  std::vector<CityVoxelStaticPlacement> placements;
  placements.reserve(index.instances.size());
  for(uint32_t instanceIndex=0;instanceIndex<index.instances.size();++instanceIndex) {
    const auto &instance=index.instances[instanceIndex];
    const auto g=geometryById.find(instance.geometryId);
    const auto a=appearanceById.find(instance.appearanceId);
    if(g==geometryById.end() || a==appearanceById.end() ||
        instance.quarterTurn>3u ||
        index.appearances[a->second].geometryId!=instance.geometryId)
      return fail(error,"City file placement asset or rotation is invalid");
    const bool localFrame=(instance.flags & voxel::svraster::kCityVoxelInstanceLocalFrame)!=0;
    const int64_t x=instance.gridPosition[0],z=instance.gridPosition[2];
    std::array<int64_t,3> local{x,instance.gridPosition[1],z};
    switch(localFrame?0:instance.quarterTurn) {
      case 1:local={z,instance.gridPosition[1],-x};break;
      case 2:local={-x,instance.gridPosition[1],-z};break;
      case 3:local={-z,instance.gridPosition[1],x};break;
      default:break;
    }
    CityVoxelStaticPlacement placement;
    placement.stableId=instance.stableId;
    placement.geometryIndex=g->second;
    placement.appearanceIndex=a->second;
    placement.buildingIndex=instance.quarterTurn;
    if(localFrame) {
      if(cellForInstance[instanceIndex]==UINT32_MAX)
        return fail(error,"City local frame has no spatial cell");
      placement.buildingIndex=frameBuilding[cellForInstance[instanceIndex]];
      if(placement.buildingIndex==UINT32_MAX)
        return fail(error,"City local frame has no spatial cell");
    }
    for(unsigned axis=0;axis<3;++axis) {
      if(local[axis]<INT32_MIN || local[axis]>INT32_MAX)
        return fail(error,"City file placement transform exceeds static grid");
      placement.localOriginCell[axis]=int32_t(local[axis]);
    }
    placements.push_back(placement);
  }
  std::vector<CityVoxelStaticCell> cells;
  cells.reserve(index.spatialCells.size());
  for(const auto &record:index.spatialCells) {
    if(uint64_t(record.firstInstance)+record.instanceCount>placements.size())
      return fail(error,"City file spatial cell exceeds placement table");
    CityVoxelStaticCell cell;
    cell.firstPlacement=record.firstInstance;
    cell.placementCount=record.instanceCount;
    std::array<int64_t,3> minimum{},maximum{};
    minimum.fill(INT64_MAX);maximum.fill(INT64_MIN);
    for(uint32_t i=record.firstInstance;i<record.firstInstance+record.instanceCount;++i) {
      const auto &instance=index.instances[i];
      const auto &geometry=index.geometry[placements[i].geometryIndex];
      for(unsigned corner=0;corner<8;++corner) {
        const int64_t x=(corner&1u)?geometry.maximumCell[0]:geometry.minimumCell[0];
        const int64_t y=(corner&2u)?geometry.maximumCell[1]:geometry.minimumCell[1];
        const int64_t z=(corner&4u)?geometry.maximumCell[2]:geometry.minimumCell[2];
        if(instance.flags & voxel::svraster::kCityVoxelInstanceLocalFrame) {
          constexpr double tau=6.2831853071795864769;
          const double angle=tau*double(instance.frameId)/65536.0;
          const double c=std::cos(angle),s=std::sin(angle);
          const double pitch=double(index.nominalPitchMicrometres)*0.001;
          const double coordinates[3]={
              (double(record.originMillimetres[0])+pitch*(c*(instance.gridPosition[0]+x)-s*(instance.gridPosition[2]+z)))/pitch,
              (double(record.originMillimetres[1])+pitch*(instance.gridPosition[1]+y))/pitch,
              (double(record.originMillimetres[2])+pitch*(s*(instance.gridPosition[0]+x)+c*(instance.gridPosition[2]+z)))/pitch};
          for(unsigned axis=0;axis<3;++axis) {
            minimum[axis]=std::min(minimum[axis],int64_t(std::floor(coordinates[axis])));
            maximum[axis]=std::max(maximum[axis],int64_t(std::ceil(coordinates[axis])));
          }
          continue;
        }
        std::array<int64_t,3> rotated{x,y,z};
        switch(instance.quarterTurn) {
          case 1:rotated={-z,y,x};break;
          case 2:rotated={-x,y,-z};break;
          case 3:rotated={z,y,-x};break;
          default:break;
        }
        for(unsigned axis=0;axis<3;++axis) {
          const int64_t value=int64_t(instance.gridPosition[axis])+rotated[axis];
          minimum[axis]=std::min(minimum[axis],value);
          maximum[axis]=std::max(maximum[axis],value);
        }
      }
    }
    for(unsigned axis=0;axis<3;++axis) {
      if(!record.instanceCount) {minimum[axis]=0;maximum[axis]=1;}
      if(minimum[axis]<INT32_MIN || maximum[axis]>INT32_MAX ||
          minimum[axis]>=maximum[axis])
        return fail(error,"City file spatial bounds exceed static grid");
      cell.minCell[axis]=int32_t(minimum[axis]);
      cell.maxCell[axis]=int32_t(maximum[axis]);
    }
    cells.push_back(cell);
  }
  if(!load(std::move(models),std::move(appearances),std::move(buildings),
      std::move(cells),std::move(placements),index.nominalPitchMicrometres,error))
    return false;
  cityOriginMillimetres_=index.cityOriginMillimetres;
  return true;
}

math::Vec3f CityVoxelStaticScene::worldPosition(const CityVoxelStaticPlacement &placement) const {
  const auto &building = buildings_[placement.buildingIndex];
  if(building.localFrame) {
    constexpr double tau=6.2831853071795864769;
    const double angle=tau*double(building.yawCode)/65536.0;
    const double c=std::cos(angle),s=std::sin(angle);
    const double pitch=double(pitchMicrometres_)*1.0e-6;
    const double x=placement.localOriginCell[0]*pitch,z=placement.localOriginCell[2]*pitch;
    return math::Vec3f(float(double(cityOriginMillimetres_[0])*0.001+
          double(building.originMillimetres[0])*0.001+c*x-s*z),
        float(double(cityOriginMillimetres_[1]+building.originMillimetres[1])*0.001+
          placement.localOriginCell[1]*pitch),
        float(double(cityOriginMillimetres_[2])*0.001+
          double(building.originMillimetres[2])*0.001+s*x+c*z));
  }
  const int64_t x = placement.localOriginCell[0];
  const int64_t z = placement.localOriginCell[2];
  int64_t rx = x, rz = z;
  switch (building.quarterTurns) {
    case 1: rx = -z; rz = x; break;
    case 2: rx = -x; rz = -z; break;
    case 3: rx = z; rz = -x; break;
    default: break;
  }
  const double pitch = static_cast<double>(pitchMicrometres_) * 0.000001;
  return math::Vec3f(
      static_cast<float>((rx + building.originCell[0]) * pitch+
          double(cityOriginMillimetres_[0])*0.001),
      static_cast<float>((static_cast<int64_t>(placement.localOriginCell[1]) + building.originCell[1]) * pitch+
          double(cityOriginMillimetres_[1])*0.001),
      static_cast<float>((rz + building.originCell[2]) * pitch+
          double(cityOriginMillimetres_[2])*0.001));
}

math::Quatf CityVoxelStaticScene::worldRotation(const CityVoxelStaticPlacement &placement) const {
  const auto &building=buildings_[placement.buildingIndex];
  if(building.localFrame)
    return math::Quatf::fromAxisAngle(math::Vec3f(0.0f,1.0f,0.0f),
        -float(double(building.yawCode)*6.2831853071795864769/65536.0));
  const auto turns = building.quarterTurns;
  return math::Quatf::fromAxisAngle(math::Vec3f(0.0f, 1.0f, 0.0f),
                                    -static_cast<float>(turns) * 1.5707963267948966f);
}

bool CityVoxelStaticScene::sync(VirtualGeometryScene &scene,
                                 const CellVisible &visible,
                                 uint32_t maximumActivePlacements,
                                 std::string *error) {
  if (error) error->clear();
  if (!visible) return fail(error, "city cell visibility callback is required");
  if (!compactUploaded_) {
    if (!scene.installCityVoxelCompactPlacements(placements_, error)) return false;
    compactUploaded_ = true;
  }
  std::vector<uint32_t> selected;
  selected.reserve(std::min<size_t>(placements_.size(), maximumActivePlacements));
  visibleCellCount_ = 0;
  for (const auto &cell : cells_) {
    if (!visible(cell)) continue;
    ++visibleCellCount_;
    for (uint32_t i = 0; i < cell.placementCount; ++i) {
      if (selected.size() == maximumActivePlacements)
        return fail(error, "visible city placement count exceeds active instance budget");
      selected.push_back(cell.firstPlacement + i);
    }
  }
  if (scene.getInstanceCount() < active_.size() ||
      scene.getInstanceCount() - active_.size() + selected.size() > scene.maximumActiveInstanceCount())
    return fail(error, "visible city placements exceed VG instance buffer capacity");
  for (const uint32_t index : selected)
    if (!scene.hasRegisteredModel(models_[placements_[index].geometryIndex].registeredName))
      return fail(error, "visible city model is not registered in VG scene");
  const std::unordered_set<uint32_t> wanted(selected.begin(), selected.end());
  PlacementBatch batch(scene);
  for (auto it = active_.begin(); it != active_.end();) {
    if (wanted.find(it->first) != wanted.end()) { ++it; continue; }
    scene.destroyModelPlacement(it->second);
    it = active_.erase(it);
  }
  for (const uint32_t index : selected) {
    if (active_.find(index) != active_.end()) continue;
    const auto &placement = placements_[index];
    const auto &model = models_[placement.geometryIndex];
    VirtualGeometryScene::ModelPlacementHandle handle;
    if (!scene.instantiateModelPlacement(model.registeredName,
          worldPosition(placement), worldRotation(placement), 1.0f,
          model.materialIndex, handle, error))
      return false;
    if (!scene.setModelPlacementCityMetadata(handle, placement.geometryIndex,
          placement.appearanceIndex, placement.stableId, index + 1u)) {
      scene.destroyModelPlacement(handle);
      return fail(error, "failed to publish city placement metadata");
    }
    active_.emplace(index, handle);
  }
  return true;
}

bool CityVoxelStaticScene::installAppearanceAssets(
    VirtualGeometryScene &scene,
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    const std::vector<CityVoxelGPUProfileSlot> &profileSlots,
    std::string *error) {
  if (geometry.size() != models_.size() || appearances.size() != appearances_.size())
    return fail(error, "city material tables do not match placement table sizes");
  for (size_t i = 0; i < models_.size(); ++i)
    if (geometry[i].id != models_[i].geometryId)
      return fail(error, "city geometry index differs from placement model index");
  for (size_t i = 0; i < appearances_.size(); ++i)
    if (appearances[i].id != appearances_[i])
      return fail(error, "city appearance index differs from placement table index");
  for (const auto &placement : placements_)
    if (appearances[placement.appearanceIndex].geometryId !=
        geometry[placement.geometryIndex].id)
      return fail(error, "city placement appearance addresses another geometry");
  if (!scene.installCityVoxelCompactPlacements(placements_, error)) return false;
  compactUploaded_ = true;
  return scene.installCityVoxelAppearanceAssets(geometry, appearances, profileSlots, error);
}

bool CityVoxelStaticScene::stableIdForDenseInstance(
    const VirtualGeometryScene &scene, uint32_t denseInstance,
    uint64_t &stableId) const {
  if (denseInstance >= scene.instanceManager.packedInstances.size()) return false;
  const uint32_t token = scene.instanceManager.packedInstances[denseInstance]._padding2[0];
  if (token == 0u || token > placements_.size()) return false;
  stableId = placements_[token - 1u].stableId;
  return true;
}

void CityVoxelStaticScene::clear(VirtualGeometryScene &scene) {
  PlacementBatch batch(scene);
  for (const auto &[index, handle] : active_)
    scene.destroyModelPlacement(handle);
  active_.clear();
  visibleCellCount_ = 0;
}

CityVoxelStaticScene::Stats CityVoxelStaticScene::stats() const {
  return {placements_.size() * sizeof(CityVoxelStaticPlacement),
          cells_.size() * sizeof(CityVoxelStaticCell),
          static_cast<uint32_t>(active_.size()), visibleCellCount_};
}

} // namespace virtualgeometry
