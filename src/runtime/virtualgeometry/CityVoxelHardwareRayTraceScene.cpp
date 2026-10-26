#include "virtualgeometry/CityVoxelHardwareRayTraceScene.hpp"

#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/PageBlasGate.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>

namespace virtualgeometry {
namespace {
std::vector<voxel::svraster::CityVoxelCompiledGeometry> geometryCatalog(
    const CityVoxelSceneRuntime &runtime) {
  std::vector<voxel::svraster::CityVoxelCompiledGeometry> result;
  result.reserve(runtime.index().geometry.size());
  for (const auto &record:runtime.index().geometry) {
    voxel::svraster::CityVoxelCompiledGeometry asset;
    asset.id=record.id;asset.contentHash=record.contentHash;
    asset.addressHash=record.voxelAddressHash;
    asset.pitchMicrometres=record.pitchMicrometres;
    for (unsigned axis=0;axis<3;++axis)
      asset.extent[axis]=record.maximumCell[axis]-record.minimumCell[axis];
    result.push_back(std::move(asset));
  }
  return result;
}
std::vector<voxel::svraster::CityVoxelCompiledAppearance> appearanceCatalog(
    const CityVoxelSceneRuntime &runtime) {
  std::vector<voxel::svraster::CityVoxelCompiledAppearance> result;
  result.reserve(runtime.index().appearances.size());
  for (const auto &record:runtime.index().appearances) {
    voxel::svraster::CityVoxelCompiledAppearance asset;
    asset.id=record.id;asset.contentHash=record.contentHash;
    asset.geometryId=record.geometryId;
    asset.geometryAddressHash=record.geometryAddressHash;
    asset.codec=record.codec;
    result.push_back(std::move(asset));
  }
  return result;
}
}
rendering::Buffer CityVoxelHardwareRayTraceScene::create(const char *name,
    uint64_t bytes, bool frameLocal) {
  return graph_.createBuffer({.name=std::string(name)+"."+std::to_string(owner_)+
      "."+std::to_string(bufferSerial_++),
      .size=bytes,.usage=rendering::BufferUsage_Storage|rendering::BufferUsage_Push,
      .frameLocal=frameLocal,.metalExportable=true});
}

CityVoxelHardwareRayTraceScene::CityVoxelHardwareRayTraceScene(rendering::RenderGraph &graph,
    const CityVoxelStaticScene &city,
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    uint32_t maximumRays, uint32_t frameSlots,
    CityVoxelHardwareRayBudgets budgets)
    : graph_(graph),city_(city),geometry_(geometry),appearances_(appearances),budgets_(budgets) {
  initialise(maximumRays,frameSlots);
}

CityVoxelHardwareRayTraceScene::CityVoxelHardwareRayTraceScene(rendering::RenderGraph &graph,
    const CityVoxelStaticScene &city, const CityVoxelSceneRuntime &runtime,
    uint32_t maximumRays, uint32_t frameSlots,
    CityVoxelHardwareRayBudgets budgets)
    : graph_(graph),city_(city),ownedGeometry_(geometryCatalog(runtime)),
      ownedAppearances_(appearanceCatalog(runtime)),geometry_(ownedGeometry_),
      appearances_(ownedAppearances_),budgets_(budgets),streamed_(&runtime),ownsAssets_(true) {
  initialise(maximumRays,frameSlots);
}

void CityVoxelHardwareRayTraceScene::initialise(uint32_t maximumRays, uint32_t frameSlots) {
  const auto &city=city_;
  const auto &geometry=geometry_;
  const auto &appearances=appearances_;
  const auto &budgets=budgets_;
  auto &graph=graph_;
  if (geometry.size() != city.models().size() || city.placements().empty() ||
      city.placements().size() > UINT32_MAX || !frameSlots ||
      !budgets.maximumActiveInstances ||
      !budgets.maximumInstallsPerUpdate || !budgets.maximumInputBytesPerUpdate ||
      !budgets.maximumResidentInputBytes || !budgets.maximumBlasBytes)
    throw std::invalid_argument("Invalid city hardware ray trace tables");
  Backend::Configuration config;
  config.metallibPath = VG_RT_METALLIB_PATH;
  uint64_t potentialRayInstances=0;
  for (const auto &placement:city.placements()) {
    if (placement.geometryIndex>=geometry.size())
      throw std::invalid_argument("City hardware placement geometry is out of range");
    const uint32_t groups=streamed_ ? streamed_->traceGroupCount(placement.geometryIndex) : 1u;
    if (!groups) throw std::invalid_argument("City hardware geometry has no trace groups");
    potentialRayInstances+=groups;
  }
  config.maxInstances=uint32_t(std::min<uint64_t>(potentialRayInstances,
      budgets.maximumActiveInstances));
  config.maxRays = maximumRays;
  config.frameSlots = frameSlots;
  config.blasBudgetBytes = budgets.maximumBlasBytes;
  config.maxBlasBuildsPerFrame = budgets.maximumInstallsPerUpdate;
  config.maxBlasBuildBytesPerFrame = budgets.maximumInputBytesPerUpdate;
  backend_ = std::make_shared<Backend>(config,graph.getRHI()->exportMetalDevice());
  if (!backend_->capabilities().available) return;
  owner_ = raytracing::allocateBlasOwner();
  instances_ = create("city.hardware.instances",uint64_t(config.maxInstances)*sizeof(raytracing::RayTraceInstance),true);
  count_ = create("city.hardware.instanceCount",sizeof(uint32_t),true);
  modelForGeometry_.reserve(geometry.size());
  std::unordered_map<uint64_t,uint32_t> byId;
  for (uint32_t index=0; index<geometry.size(); ++index) {
    const auto &asset=geometry[index];
    if (asset.id != city.models()[index].geometryId ||
        asset.pitchMicrometres != city.pitchMicrometres())
      throw std::invalid_argument("City hardware geometry differs from raster");
    const auto found=byId.find(asset.id);
    if (found!=byId.end()) { modelForGeometry_.push_back(found->second); continue; }
    const uint32_t model=uint32_t(sourceGeometry_.size());
    byId.emplace(asset.id,model);
    modelForGeometry_.push_back(model);
    sourceGeometry_.push_back(index);
  }
  groups_.resize(sourceGeometry_.size());
  uint64_t groupKey=0;
  for (uint32_t model=0;model<groups_.size();++model) {
    const uint32_t count=streamed_ ? streamed_->traceGroupCount(sourceGeometry_[model]) : 1u;
    if (!count || groupKey+count>UINT32_MAX)
      throw std::invalid_argument("City hardware trace group key limit exceeded");
    groups_[model].resize(count);
    for (auto &group:groups_[model]) group.keyIndex=uint32_t(groupKey++);
  }
  lookups_.resize(appearances.size());
  for (uint32_t i=0;i<appearances.size();++i) {
    const auto &appearance=appearances[i];
    const auto source=std::find_if(geometry.begin(),geometry.end(),
        [&](const auto &g){return g.id==appearance.geometryId;});
    if (source==geometry.end()) throw std::invalid_argument("City hardware appearance has no geometry");
    if (source->addressHash!=appearance.geometryAddressHash)
      throw std::invalid_argument("City hardware appearance address differs from geometry");
  }
  // A file-backed scene has no resident ray cut until its streamer publishes
  // one. Construction must not decode a city worth of leaf meshes or BLASes.
  geometryReady_.assign(geometry.size(),0u);
  appearanceReady_.assign(appearances.size(),0u);
  lookupBound_.assign(appearances.size(),0u);
  update();
}

CityVoxelHardwareRayTraceScene::~CityVoxelHardwareRayTraceScene() {
  backend_.reset(); // Drain native input reads before releasing model buffers.
  for (const auto &model:groups_) for (const auto &group:model) {
    if (group.input.vertices.isValid()) graph_.deleteBuffer(group.input.vertices);
    if (group.input.indices.isValid()) graph_.deleteBuffer(group.input.indices);
  }
  if (instances_.isValid()) graph_.deleteBuffer(instances_);
  if (count_.isValid()) graph_.deleteBuffer(count_);
}

void CityVoxelHardwareRayTraceScene::publishResidency(
    const std::vector<uint8_t> &geometryReady,
    const std::vector<uint8_t> &appearanceReady) {
  if (geometryReady.size()!=geometryReady_.size() ||
      appearanceReady.size()!=appearanceReady_.size())
    throw std::invalid_argument("City hardware residency table size changed");
  if (ownsAssets_) throw std::invalid_argument("File-backed city ray scene requires a streamed publication");
  streamed_=nullptr;
  geometryReady_=geometryReady;
  appearanceReady_=appearanceReady;
  update();
}

void CityVoxelHardwareRayTraceScene::publishFromRuntime(
    const CityVoxelSceneRuntime &runtime) {
  const auto &index=runtime.index();
  if (&runtime!=validatedRuntime_ ||
      runtime.openGeneration()!=validatedRuntimeGeneration_) {
    if (index.geometry.size()!=geometry_.size() ||
      index.appearances.size()!=appearances_.size() ||
      index.instances.size()!=city_.placements().size())
      throw std::invalid_argument("City ray and streamed scene tables differ");
    for (uint32_t i=0;i<geometry_.size();++i)
      if(index.geometry[i].id!=geometry_[i].id ||
       index.geometry[i].contentHash!=geometry_[i].contentHash)
        throw std::invalid_argument("City ray geometry differs from streamed scene");
    for (uint32_t model=0;model<sourceGeometry_.size();++model)
      if (groups_[model].size()!=runtime.traceGroupCount(sourceGeometry_[model]))
        throw std::invalid_argument("City ray trace group layout differs from streamed scene");
    for (uint32_t i=0;i<appearances_.size();++i)
      if(index.appearances[i].id!=appearances_[i].id ||
       index.appearances[i].contentHash!=appearances_[i].contentHash)
        throw std::invalid_argument("City ray appearance differs from streamed scene");
    for (uint32_t i=0;i<index.instances.size();++i)
      if (index.instances[i].stableId!=city_.placements()[i].stableId ||
        index.instances[i].geometryId!=geometry_[city_.placements()[i].geometryIndex].id ||
        index.instances[i].appearanceId!=appearances_[city_.placements()[i].appearanceIndex].id ||
        index.instances[i].quarterTurn!=city_.buildings()[city_.placements()[i].buildingIndex].quarterTurns ||
        bool(index.instances[i].flags & voxel::svraster::kCityVoxelInstanceLocalFrame)!=
            city_.buildings()[city_.placements()[i].buildingIndex].localFrame ||
        ((index.instances[i].flags & voxel::svraster::kCityVoxelInstanceLocalFrame) &&
         index.instances[i].frameId!=city_.buildings()[city_.placements()[i].buildingIndex].yawCode))
        throw std::invalid_argument("City ray placements differ from streamed scene");
    const double pitch=double(index.nominalPitchMicrometres)*1.0e-6;
    for (uint32_t i=0;i<index.instances.size();++i) {
      const auto position=city_.worldPosition(city_.placements()[i]);
      const double coordinates[3]={position.x(),position.y(),position.z()};
      for(unsigned axis=0;axis<3;++axis) {
        const double expected=(index.instances[i].flags & voxel::svraster::kCityVoxelInstanceLocalFrame)
            ? coordinates[axis]
            : double(index.instances[i].gridPosition[axis])*pitch+
              double(index.cityOriginMillimetres[axis])*1.0e-3;
        if(std::abs(coordinates[axis]-expected)>0.001)
          throw std::invalid_argument("City ray placement transform differs from streamed scene");
      }
    }
    validatedRuntime_=&runtime;
    validatedRuntimeGeneration_=runtime.openGeneration();
  }
  std::vector<uint8_t> geometryReady(geometry_.size(),0u);
  std::vector<uint8_t> appearanceReady(appearances_.size(),0u);
  for(const auto &cell:runtime.publishedCells()) {
    if(!cell.detailed)continue;
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i) {
      const auto &placement=city_.placements()[i];
      geometryReady[placement.geometryIndex]=1u;
      appearanceReady[placement.appearanceIndex]=1u;
    }
  }
  if (ownsAssets_) {
    std::string error;
    for (uint32_t i=0;i<geometryReady.size();++i)
      if (geometryReady[i] && !geometryReady_[i] &&
          !runtime.residentGeometry(i,ownedGeometry_[i],&error))
        throw std::invalid_argument("Cannot publish streamed ray geometry: "+error);
    for (uint32_t i=0;i<appearanceReady.size();++i)
      if (appearanceReady[i] && !appearanceReady_[i] &&
          !runtime.residentAppearance(i,ownedAppearances_[i],&error))
        throw std::invalid_argument("Cannot publish streamed ray appearance: "+error);
  }
  streamed_=&runtime;
  geometryReady_=std::move(geometryReady);
  appearanceReady_=std::move(appearanceReady);
  update();
  if (ownsAssets_) {
    for (uint32_t i=0;i<geometryReady_.size();++i)
      if (!geometryReady_[i]) std::vector<uint8_t>().swap(ownedGeometry_[i].bytes);
    for (uint32_t i=0;i<appearanceReady_.size();++i)
      if (!appearanceReady_[i]) std::vector<uint8_t>().swap(ownedAppearances_[i].bytes);
  }
}

uint32_t CityVoxelHardwareRayTraceScene::residentGeometryCount() const {
  return uint32_t(std::count_if(groups_.begin(),groups_.end(),
      [](const auto &groups){return std::any_of(groups.begin(),groups.end(),
          [](const ModelGroup &group){return group.handle!=0u;});}));
}

uint32_t CityVoxelHardwareRayTraceScene::residentGroupCount() const {
  uint32_t count=0;
  for (const auto &model:groups_) for (const auto &group:model)
    count+=group.handle!=0u;
  return count;
}

void CityVoxelHardwareRayTraceScene::reconcileGeometry() {
  for (uint32_t i=0;i<appearanceReady_.size();++i) {
    if (!appearanceReady_[i]) {
      lookups_[i]={};
      lookupBound_[i]=0u;
      continue;
    }
    if (lookupBound_[i]) continue;
    const auto &appearance=appearances_[i];
    const auto source=std::find_if(geometry_.begin(),geometry_.end(),
        [&](const auto &g){return g.id==appearance.geometryId;});
    std::string error;
    if (source==geometry_.end() || !lookups_[i].bind(*source,appearance,&error))
      throw std::invalid_argument("Cannot bind city ray appearance: "+error);
    std::vector<voxel::svraster::CityVoxelCell> values;
    if (!voxel::svraster::decodeCityVoxelAppearance(appearance,values,&error))
      throw std::invalid_argument(error);
    for (const auto &value:values)
      if ((value.rgba>>24u)!=255u)
        throw std::invalid_argument("City hardware ray trace supports opaque appearances only");
    lookupBound_[i]=1u;
  }
  std::vector<uint8_t> desired(sourceGeometry_.size(),0u);
  for (uint32_t index=0;index<geometryReady_.size();++index)
    if (geometryReady_[index]) desired[modelForGeometry_[index]]=1u;
  for (uint32_t model=0;model<desired.size();++model) {
    if (desired[model]) continue;
    const auto &asset=geometry_[sourceGeometry_[model]];
    for (auto &group:groups_[model]) {
      if (!group.handle) continue;
      const raytracing::GeometryKey key{owner_,asset.contentHash,group.keyIndex,3};
      if (!backend_->tryEvictGeometryAfterInputReads(key)) continue;
      if (group.input.vertices.isValid()) graph_.deleteBuffer(group.input.vertices);
      if (group.input.indices.isValid()) graph_.deleteBuffer(group.input.indices);
      residentInputBytes_-=group.input.bytes;
      triangles_-=group.input.triangles;
      group.input={};group.handle=0u;group.mesh={};
    }
  }
  uint32_t installed=0u;
  uint64_t installedBytes=0u;
  for (uint32_t model=0;model<desired.size();++model) {
    if (!desired[model]) continue;
    const auto &asset=geometry_[sourceGeometry_[model]];
    for (uint32_t groupIndex=0;groupIndex<groups_[model].size();++groupIndex) {
      auto &group=groups_[model][groupIndex];
      if (group.handle || installed>=budgets_.maximumInstallsPerUpdate) continue;
      CityVoxelHardwareMesh mesh;
      std::string error;
      const bool built=streamed_ ? streamed_->residentTraceGroup(
          sourceGeometry_[model],groupIndex,mesh,&error) :
          buildCityVoxelHardwareMesh(asset,mesh,&error);
      if (!built || mesh.indices.empty())
        throw std::invalid_argument("Cannot build city hardware BLAS group: "+error);
      const uint64_t bytes=mesh.positions.size()*sizeof(float)+
          mesh.indices.size()*sizeof(uint32_t);
      if (bytes>budgets_.maximumResidentInputBytes ||
          bytes>budgets_.maximumInputBytesPerUpdate)
        throw std::length_error("City ray trace group exceeds the configured input budget");
      if (residentInputBytes_+bytes>budgets_.maximumResidentInputBytes ||
          installedBytes+bytes>budgets_.maximumInputBytesPerUpdate)
        continue;
      const auto vb=create("city.hardware.vertices",mesh.positions.size()*sizeof(float),false);
      const auto ib=create("city.hardware.indices",mesh.indices.size()*sizeof(uint32_t),false);
      graph_.bufferWrite(vb,0,mesh.positions.size()*sizeof(float),mesh.positions.data());
      graph_.bufferWrite(ib,0,mesh.indices.size()*sizeof(uint32_t),mesh.indices.data());
      Backend::Geometry input;
      input.key={owner_,asset.contentHash,group.keyIndex,3};
      input.vertexBuffer=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(vb));
      input.indexBuffer=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(ib));
      input.vertexStride=12;
      input.vertexCount=uint32_t(mesh.positions.size()/3);
      input.triangleCount=uint32_t(mesh.indices.size()/3);
      group.handle=backend_->cacheGeometry(input);
      group.input={vb,ib,bytes,input.triangleCount};
      triangles_+=input.triangleCount;
      residentInputBytes_+=bytes;
      installedBytes+=bytes;
      ++installed;
      std::vector<float>().swap(mesh.positions);
      std::vector<uint32_t>().swap(mesh.indices);
      group.mesh=std::move(mesh);
    }
  }
}

void CityVoxelHardwareRayTraceScene::update() {
  if (!backend_->capabilities().available) return;
  reconcileGeometry();
  std::vector<raytracing::RayTraceInstance> records;
  records.reserve(std::min<size_t>(backend_->configuration().maxInstances,
      budgets_.maximumActiveInstances));
  rayInstances_.clear();
  coverageComplete_=!streamed_ || streamed_->statistics().fallbackCells==0u;
  auto append=[&](uint32_t placementIndex) {
    const auto &placement=city_.placements()[placementIndex];
    if (placement.geometryIndex>=geometry_.size() ||
        placement.appearanceIndex>=appearances_.size() ||
        appearances_[placement.appearanceIndex].geometryId!=geometry_[placement.geometryIndex].id)
      throw std::invalid_argument("City hardware placement references wrong asset");
    if (!geometryReady_[placement.geometryIndex] ||
        !appearanceReady_[placement.appearanceIndex]) {
      coverageComplete_=false;
      return;
    }
    const uint32_t model=modelForGeometry_[placement.geometryIndex];
    const auto &building=city_.buildings()[placement.buildingIndex];
    const uint32_t turns=building.quarterTurns;
    const float basis[4][9]={{1,0,0,0,1,0,0,0,1},
        {0,0,1,0,1,0,-1,0,0},
        {-1,0,0,0,1,0,0,0,-1},
        {0,0,-1,0,1,0,1,0,0}};
    const auto origin=city_.worldPosition(placement);
    for (uint32_t groupIndex=0;groupIndex<groups_[model].size();++groupIndex) {
      if (records.size()>=backend_->configuration().maxInstances) {
        coverageComplete_=false;
        return;
      }
      raytracing::RayTraceInstance record;
      const uint32_t handle=groups_[model][groupIndex].handle;
      record.geometryHandle=backend_->geometryState(handle)==Backend::GeometryState::Ready ? handle : 0u;
      if (!record.geometryHandle) coverageComplete_=false;
      for (unsigned i=0;i<9;++i) record.transform[i]=basis[turns][i];
      if(building.localFrame) {
        constexpr double tau=6.2831853071795864769;
        const double angle=tau*double(building.yawCode)/65536.0;
        const float c=float(std::cos(angle)),s=float(std::sin(angle));
        const float yawBasis[9]={c,0,s,0,1,0,-s,0,c};
        for(unsigned i=0;i<9;++i)record.transform[i]=yawBasis[i];
      }
      for (unsigned axis=0;axis<3;++axis) record.transform[9+axis]=origin[axis];
      records.push_back(record);
      rayInstances_.push_back({placementIndex,groupIndex});
    }
  };
  if (streamed_) {
    for (const auto &cell:streamed_->publishedCells()) {
      if (!cell.detailed) continue;
      for (uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i)
        append(i);
    }
  } else {
    for (uint32_t i=0;i<city_.placements().size();++i)append(i);
  }
  uint32_t count=uint32_t(records.size());
  if(!records.empty())
    graph_.bufferWrite(instances_,0,uint64_t(records.size())*sizeof(records[0]),records.data());
  graph_.bufferWrite(count_,0,sizeof(count),&count);
}

bool CityVoxelHardwareRayTraceScene::bindTraceFrame(
    Backend::Frame &frame,std::string *error) {
  if(error)error->clear();
  if(!backend_->capabilities().available || !coverageComplete_ ||
      !instances_.isValid() || !count_.isValid()) {
    if(error)*error="City hardware ray cut is incomplete";
    return false;
  }
  frame.instances=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(instances_));
  frame.instanceCount=graph_.getRHI()->exportMetalBuffer(graph_.resolveBufferId(count_));
  return frame.instances && frame.instanceCount;
}

bool CityVoxelHardwareRayTraceScene::resolveClosestHit(
    const raytracing::ClosestHitResult &hit, ResolvedHit &out) const {
  if (hit.state!=1u || hit.instance>=rayInstances_.size()) return false;
  const auto source=rayInstances_[hit.instance];
  const uint32_t placementIndex=source.placement;
  const auto &placement=city_.placements()[placementIndex];
  if (!geometryReady_[placement.geometryIndex] || !appearanceReady_[placement.appearanceIndex]) return false;
  const auto &mesh=groups_[modelForGeometry_[placement.geometryIndex]][source.group].mesh;
  if (hit.primitive>=mesh.triangleAddress.size()) return false;
  uint32_t rgba=0, profile=0;
  const uint32_t address=mesh.triangleAddress[hit.primitive];
  const auto cell=mesh.cells[address>>3u];
  if (!lookups_[placement.appearanceIndex].lookupCell(cell,rgba,profile)) return false;
  constexpr float normals[6][3]={{-1,0,0},{1,0,0},{0,-1,0},{0,1,0},{0,0,-1},{0,0,1}};
  const auto &normal=normals[address&7u];
  const uint32_t turns=city_.buildings()[placement.buildingIndex].quarterTurns;
  std::array<float,3> rotated{};
  const auto &building=city_.buildings()[placement.buildingIndex];
  if(building.localFrame) {
    constexpr double tau=6.2831853071795864769;
    const double angle=tau*double(building.yawCode)/65536.0;
    const float c=float(std::cos(angle)),s=float(std::sin(angle));
    rotated={c*normal[0]-s*normal[2],normal[1],s*normal[0]+c*normal[2]};
  } else
  switch (turns) {
    case 1: rotated={-normal[2],normal[1],normal[0]}; break;
    case 2: rotated={-normal[0],normal[1],-normal[2]}; break;
    case 3: rotated={normal[2],normal[1],-normal[0]}; break;
    default:rotated={normal[0],normal[1],normal[2]}; break;
  }
  out={placement.stableId,placementIndex,cell,rotated,rgba,profile,float(rgba>>24u)/255.0f};
  return true;
}
} // namespace virtualgeometry
#endif
