#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace virtualgeometry {
namespace {
bool fail(std::string *error,const char *message) { if(error)*error=message; return false; }
std::array<int64_t,3> rotate(uint8_t turns,const std::array<int64_t,3> &p) {
  switch(turns) {
    case 1:return {-p[2],p[1],p[0]};
    case 2:return {-p[0],p[1],-p[2]};
    case 3:return {p[2],p[1],-p[0]};
    default:return p;
  }
}
}
bool CityVoxelSceneRuntime::open(const std::string &path,Budgets budgets,
    const voxel::svraster::CityVoxelSceneLimits &limits,std::string *error) {
  if(error)error->clear();
  if(!budgets.metadataBytes || !budgets.instanceBytes ||
      !budgets.geometryEncodedBytes || !budgets.appearanceBytes ||
      !budgets.stagingBytes || !budgets.fallbackBytes || !budgets.maximumActiveInstances ||
      (budgets.requireTrace && !budgets.traceBytes) ||
      (budgets.requireVirtualGeometry && !budgets.virtualGeometryBytes))
    return fail(error,"City runtime budgets must be positive");
  CityVoxelSceneRuntime next;
  next.budgets_=budgets;
  next.openGeneration_=openGeneration_+1u;
  if(!next.openGeneration_)next.openGeneration_=1u;
  if(!next.file_.open(path,limits,error))return false;
  const auto &index=next.file_.index();
  const uint64_t instanceBytes=uint64_t(index.instances.size())*sizeof(voxel::svraster::CityVoxelInstance);
  if(next.file_.indexBytes()<instanceBytes ||
      next.file_.indexBytes()-instanceBytes>budgets.metadataBytes ||
      instanceBytes>budgets.instanceBytes)
    return fail(error,"City runtime metadata or instance budget exceeded");
  next.stats_.metadataBytes=next.file_.indexBytes()-instanceBytes;
  next.stats_.instanceBytes=instanceBytes;
  for(uint32_t i=0;i<index.geometry.size();++i)
    next.geometryById_.emplace(index.geometry[i].id,i);
  for(uint32_t i=0;i<index.appearances.size();++i)
    next.appearanceById_.emplace(index.appearances[i].id,i);
  for(uint32_t i=0;i<index.pages.size();++i) {
    if(index.pages[i].kind==voxel::svraster::CityVoxelPageKind::Trace)
      next.tracePagesByGeometry_[index.pages[i].assetId].push_back(i);
    if(index.pages[i].kind==voxel::svraster::CityVoxelPageKind::VirtualGeometry)
      next.vgPagesByGeometry_[index.pages[i].assetId].push_back(i);
  }
  if(!next.makeBounds(error))return false;
  *this=std::move(next);
  return true;
}

bool CityVoxelSceneRuntime::makeBounds(std::string *error) {
  const auto &index=file_.index();
  bounds_.reserve(index.spatialCells.size());
  for(const auto &cell:index.spatialCells) {
    Bounds box;
    box.minimum.fill(std::numeric_limits<double>::infinity());
    box.maximum.fill(-std::numeric_limits<double>::infinity());
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i) {
      const auto &instance=index.instances[i];
      const auto found=geometryById_.find(instance.geometryId);
      if(found==geometryById_.end())return fail(error,"City cell references missing geometry");
      const auto &geometry=index.geometry[found->second];
      const double pitch=double(geometry.pitchMicrometres)*1e-6;
      for(unsigned corner=0;corner<8;++corner) {
        const std::array<int64_t,3> local={
            (corner&1)?geometry.maximumCell[0]:geometry.minimumCell[0],
            (corner&2)?geometry.maximumCell[1]:geometry.minimumCell[1],
            (corner&4)?geometry.maximumCell[2]:geometry.minimumCell[2]};
        if(instance.flags & voxel::svraster::kCityVoxelInstanceLocalFrame) {
          constexpr double tau=6.2831853071795864769;
          const double angle=tau*double(instance.frameId)/65536.0;
          const double c=std::cos(angle),s=std::sin(angle);
          const double x=double(instance.gridPosition[0])+local[0];
          const double y=double(instance.gridPosition[1])+local[1];
          const double z=double(instance.gridPosition[2])+local[2];
          const double values[3]={
              double(cell.originMillimetres[0])*1e-3+pitch*(c*x-s*z),
              double(cell.originMillimetres[1])*1e-3+pitch*y,
              double(cell.originMillimetres[2])*1e-3+pitch*(s*x+c*z)};
          for(unsigned axis=0;axis<3;++axis) {
            const double position=double(index.cityOriginMillimetres[axis])*1e-3+values[axis];
            box.minimum[axis]=std::min(box.minimum[axis],position);
            box.maximum[axis]=std::max(box.maximum[axis],position);
          }
          continue;
        }
        const auto rotated=rotate(instance.quarterTurn,local);
        for(unsigned axis=0;axis<3;++axis) {
          const double position=double(index.cityOriginMillimetres[axis])*1e-3+
              double(int64_t(instance.gridPosition[axis])+rotated[axis])*pitch;
          box.minimum[axis]=std::min(box.minimum[axis],position);
          box.maximum[axis]=std::max(box.maximum[axis],position);
        }
      }
    }
    if(!cell.instanceCount) {
      for(unsigned axis=0;axis<3;++axis)
        box.minimum[axis]=box.maximum[axis]=double(cell.originMillimetres[axis])*1e-3;
    }
    bounds_.push_back(box);
  }
  return true;
}

uint64_t &CityVoxelSceneRuntime::poolBytes(voxel::svraster::CityVoxelPageKind kind) {
  switch(kind) {
    case voxel::svraster::CityVoxelPageKind::Geometry:return stats_.geometryEncodedBytes;
    case voxel::svraster::CityVoxelPageKind::Appearance:return stats_.appearanceBytes;
    case voxel::svraster::CityVoxelPageKind::Trace:return stats_.traceBytes;
    case voxel::svraster::CityVoxelPageKind::VirtualGeometry:return stats_.virtualGeometryBytes;
    default:return stats_.fallbackBytes;
  }
}
uint64_t CityVoxelSceneRuntime::poolBudget(voxel::svraster::CityVoxelPageKind kind) const {
  switch(kind) {
    case voxel::svraster::CityVoxelPageKind::Geometry:return budgets_.geometryEncodedBytes;
    case voxel::svraster::CityVoxelPageKind::Appearance:return budgets_.appearanceBytes;
    case voxel::svraster::CityVoxelPageKind::Trace:return budgets_.traceBytes;
    case voxel::svraster::CityVoxelPageKind::VirtualGeometry:return budgets_.virtualGeometryBytes;
    default:return budgets_.fallbackBytes;
  }
}
const std::vector<uint8_t> *CityVoxelSceneRuntime::residentPage(uint32_t id) const {
  const auto found=resident_.find(id);
  return found==resident_.end()?nullptr:&found->second.bytes;
}
bool CityVoxelSceneRuntime::residentGeometry(uint32_t assetIndex,
    voxel::svraster::CityVoxelCompiledGeometry &result,std::string *error) const {
  const auto &catalog=file_.index().geometry;
  if(assetIndex>=catalog.size())return fail(error,"City geometry index is out of range");
  const auto &record=catalog[assetIndex];
  if(record.pageCount!=1)return fail(error,"City geometry page layout is unsupported");
  const auto *bytes=residentPage(record.firstPage);
  if(!bytes || bytes->size()<28u || (bytes->size()-28u)%76u)
    return fail(error,"City geometry is not resident or has an invalid stream");
  auto read32=[&](size_t at) {
    return uint32_t((*bytes)[at])|(uint32_t((*bytes)[at+1])<<8u)|
        (uint32_t((*bytes)[at+2])<<16u)|(uint32_t((*bytes)[at+3])<<24u);
  };
  const uint32_t brickCount=read32(24u);
  if(read32(0u)!=0x31475643u || read32(4u)!=record.pitchMicrometres ||
      28ull+uint64_t(brickCount)*76ull!=bytes->size())
    return fail(error,"City geometry stream header differs from index");
  uint64_t occupied=0;
  for(uint32_t brick=0;brick<brickCount;++brick)
    for(uint32_t word=0;word<16u;++word)
      occupied+=__builtin_popcount(read32(28ull+uint64_t(brick)*76ull+12ull+word*4ull));
  if(!occupied || occupied>UINT32_MAX)
    return fail(error,"City geometry occupancy count is invalid");
  voxel::svraster::CityVoxelCompiledGeometry asset;
  asset.id=record.id;asset.contentHash=record.contentHash;
  asset.addressHash=record.voxelAddressHash;
  asset.pitchMicrometres=record.pitchMicrometres;
  for(unsigned axis=0;axis<3;++axis) {
    const int64_t extent=int64_t(record.maximumCell[axis])-record.minimumCell[axis];
    if(extent<=0 || extent>INT32_MAX)
      return fail(error,"City geometry extent exceeds compiler representation");
    asset.extent[axis]=int32_t(extent);
  }
  asset.occupiedCount=uint32_t(occupied);asset.bytes=*bytes;
  result=std::move(asset);
  return true;
}
bool CityVoxelSceneRuntime::residentAppearance(uint32_t assetIndex,
    voxel::svraster::CityVoxelCompiledAppearance &result,std::string *error) const {
  const auto &catalog=file_.index().appearances;
  if(assetIndex>=catalog.size())return fail(error,"City appearance index is out of range");
  const auto &record=catalog[assetIndex];
  if(record.pageCount!=1)return fail(error,"City appearance page layout is unsupported");
  const auto *bytes=residentPage(record.firstPage);
  if(!bytes)return fail(error,"City appearance is not resident");
  const auto found=geometryById_.find(record.geometryId);
  if(found==geometryById_.end())return fail(error,"City appearance geometry is missing");
  voxel::svraster::CityVoxelCompiledGeometry geometry;
  if(!residentGeometry(found->second,geometry,error))return false;
  voxel::svraster::CityVoxelCompiledAppearance asset;
  asset.id=record.id;asset.contentHash=record.contentHash;
  asset.geometryAddressHash=record.geometryAddressHash;
  asset.geometryId=record.geometryId;asset.codec=record.codec;
  asset.occupiedCount=geometry.occupiedCount;asset.bytes=*bytes;
  result=std::move(asset);
  return true;
}
bool CityVoxelSceneRuntime::residentTrace(uint32_t geometryIndex,
    CityVoxelHardwareMesh &result,std::string *error) const {
  if(traceGroupCount(geometryIndex)!=1u)
    return fail(error,"City trace geometry has multiple groups");
  return residentTraceGroup(geometryIndex,0u,result,error);
}
uint32_t CityVoxelSceneRuntime::traceGroupCount(uint32_t geometryIndex) const {
  const auto &catalog=file_.index().geometry;
  if(geometryIndex>=catalog.size())return 0u;
  const auto found=tracePagesByGeometry_.find(catalog[geometryIndex].id);
  return found==tracePagesByGeometry_.end()?0u:uint32_t(found->second.size());
}
bool CityVoxelSceneRuntime::residentTraceGroup(uint32_t geometryIndex,
    uint32_t groupIndex,CityVoxelHardwareMesh &result,std::string *error) const {
  const auto &catalog=file_.index().geometry;
  if(geometryIndex>=catalog.size())return fail(error,"City trace geometry index is out of range");
  const auto &asset=catalog[geometryIndex];
  const auto pages=tracePagesByGeometry_.find(asset.id);
  if(pages==tracePagesByGeometry_.end() || groupIndex>=pages->second.size())
    return fail(error,"City trace group index is out of range");
  const auto *bytes=residentPage(pages->second[groupIndex]);
  if(!bytes)return fail(error,"City trace page is not resident");
  return deserializeCityVoxelHardwareMesh(bytes->data(),bytes->size(),
      asset.id,asset.contentHash,result,error);
}
bool CityVoxelSceneRuntime::residentVirtualGeometry(uint32_t geometryIndex,
    voxel::svraster::CityVoxelVGPageView &result,std::string *error) const {
  const auto &catalog=file_.index().geometry;
  if(geometryIndex>=catalog.size())return fail(error,"City VG geometry index is out of range");
  const auto &asset=catalog[geometryIndex];
  const auto pages=vgPagesByGeometry_.find(asset.id);
  if(pages==vgPagesByGeometry_.end() || pages->second.size()!=1u)
    return fail(error,"City VG page layout is unsupported");
  const auto *bytes=residentPage(pages->second.front());
  if(!bytes)return fail(error,"City VG page is not resident");
  return voxel::svraster::decodeCityVoxelVGPage(bytes->data(),bytes->size(),
      asset.id,asset.contentHash,result,error);
}
bool CityVoxelSceneRuntime::ensurePage(uint32_t id,uint64_t frame,
    const std::unordered_map<uint32_t,uint32_t> &wanted,
    uint32_t &reads,std::string *error) {
  const auto resident=resident_.find(id);
  if(resident!=resident_.end()) { resident->second.lastUse=frame; return true; }
  ++stats_.pageFaults;
  if(reads>=budgets_.maximumReadsPerFrame)return true;
  const auto &record=file_.index().pages[id];
  uint64_t &pool=poolBytes(record.kind);
  const uint64_t budget=poolBudget(record.kind);
  if(record.decodedBytes>budget || record.decodedBytes>budgets_.stagingBytes)return true;
  while(pool>budget-record.decodedBytes) {
    auto victim=resident_.end();
    for(auto it=resident_.begin();it!=resident_.end();++it) {
      if(file_.index().pages[it->first].kind!=record.kind || wanted.count(it->first))continue;
      if(victim==resident_.end() || it->second.lastUse<victim->second.lastUse)victim=it;
    }
    if(victim==resident_.end())return true;
    pool-=victim->second.bytes.size();resident_.erase(victim);++stats_.evictions;
  }
  std::vector<uint8_t> bytes;
  if(!file_.readPage(id,bytes,error))return false;
  stats_.stagingPeakBytes=std::max<uint64_t>(stats_.stagingPeakBytes,bytes.capacity());
  pool+=bytes.size();
  resident_.emplace(id,ResidentPage{std::move(bytes),frame});
  ++reads;++stats_.pageReads;
  return true;
}
bool CityVoxelSceneRuntime::hasAssetPages(uint64_t geometryId,uint64_t appearanceId,
    const std::vector<uint8_t> &ready) const {
  const auto &index=file_.index();
  const auto g=geometryById_.find(geometryId),a=appearanceById_.find(appearanceId);
  if(g==geometryById_.end() || a==appearanceById_.end())return false;
  const auto &geometry=index.geometry[g->second];
  const auto &appearance=index.appearances[a->second];
  if(!geometry.pageCount || !appearance.pageCount)return false;
  for(uint32_t i=geometry.firstPage;i<geometry.firstPage+geometry.pageCount;++i)
    if(!ready[i])return false;
  for(uint32_t i=appearance.firstPage;i<appearance.firstPage+appearance.pageCount;++i)
    if(!ready[i])return false;
  if(budgets_.requireTrace) {
    const auto trace=tracePagesByGeometry_.find(geometryId);
    if(trace==tracePagesByGeometry_.end() || trace->second.empty())return false;
    for(uint32_t page:trace->second)if(!ready[page])return false;
  }
  if(budgets_.requireVirtualGeometry) {
    const auto vg=vgPagesByGeometry_.find(geometryId);
    if(vg==vgPagesByGeometry_.end() || vg->second.empty())return false;
    for(uint32_t page:vg->second)if(!ready[page])return false;
  }
  return true;
}
bool CityVoxelSceneRuntime::update(uint64_t frame,const Visible &visible,std::string *error,
    const std::function<double(uint32_t,const Bounds&)> &priority) {
  if(error)error->clear();
  if(!visible || !file_.fileBytes() || frame<=stats_.frame)
    return fail(error,"City runtime frame or visibility query is invalid");
  const auto &index=file_.index();
  std::vector<uint32_t> visibleCells;
  std::unordered_set<uint32_t> detailedCandidates;
  std::unordered_map<uint32_t,uint32_t> wanted;
  std::vector<uint32_t> requests;
  auto addPage=[&](uint32_t page) {
    if(!wanted.emplace(page,1u).second)return;
    std::vector<std::pair<uint32_t,uint32_t>> pending{{page,0u}};
    while(!pending.empty()) {
      auto &top=pending.back();
      const auto &record=index.pages[top.first];
      if(top.second==record.dependencyCount) {
        requests.push_back(top.first);
        pending.pop_back();
        continue;
      }
      const uint32_t dependency=index.dependencies[record.firstDependency+top.second++];
      if(wanted.emplace(dependency,1u).second)pending.emplace_back(dependency,0u);
    }
  };
  uint32_t requestedInstances=0;
  for(uint32_t cellIndex=0;cellIndex<bounds_.size();++cellIndex)
    if(visible(cellIndex,bounds_[cellIndex]))visibleCells.push_back(cellIndex);
  if(priority) {
    std::vector<std::pair<double,uint32_t>> ordered;
    ordered.reserve(visibleCells.size());
    for(uint32_t cellIndex:visibleCells)ordered.push_back({priority(cellIndex,bounds_[cellIndex]),cellIndex});
    std::sort(ordered.begin(),ordered.end());
    for(size_t i=0;i<ordered.size();++i)visibleCells[i]=ordered[i].second;
  }
  // Each cell's own assets follow its fallback, so the read budget completes
  // whole cells in priority order instead of every fallback first.
  for(uint32_t cellIndex:visibleCells) {
    const auto &cell=index.spatialCells[cellIndex];
    if(cell.fallbackPage!=voxel::svraster::kCityVoxelInvalidPage)addPage(cell.fallbackPage);
    if(cell.instanceCount>budgets_.maximumActiveInstances-requestedInstances)continue;
    requestedInstances+=cell.instanceCount;
    detailedCandidates.insert(cellIndex);
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i) {
      const auto &instance=index.instances[i];
      const auto &g=index.geometry[geometryById_.at(instance.geometryId)];
      const auto &a=index.appearances[appearanceById_.at(instance.appearanceId)];
      for(uint32_t p=g.firstPage;p<g.firstPage+g.pageCount;++p)addPage(p);
      for(uint32_t p=a.firstPage;p<a.firstPage+a.pageCount;++p)addPage(p);
      if(budgets_.requireTrace) {
        const auto trace=tracePagesByGeometry_.find(instance.geometryId);
        if(trace!=tracePagesByGeometry_.end())for(uint32_t p:trace->second)addPage(p);
      }
      if(budgets_.requireVirtualGeometry) {
        const auto vg=vgPagesByGeometry_.find(instance.geometryId);
        if(vg!=vgPagesByGeometry_.end())for(uint32_t p:vg->second)addPage(p);
      }
    }
  }
  uint32_t reads=0;
  for(uint32_t page:requests)
    if(!ensurePage(page,frame,wanted,reads,error))return false;
  std::vector<uint8_t> pageReady(index.pages.size(),0u);
  for(uint32_t page:requests) {
    if(!resident_.count(page))continue;
    const auto &record=index.pages[page];
    bool dependenciesReady=true;
    for(uint32_t dependency=record.firstDependency;
        dependency<record.firstDependency+record.dependencyCount;++dependency)
      dependenciesReady &= pageReady[index.dependencies[dependency]]!=0u;
    pageReady[page]=dependenciesReady ? 1u : 0u;
  }
  std::vector<CellState> publication;
  publication.reserve(visibleCells.size());
  uint32_t active=0,detailed=0;
  for(uint32_t cellIndex:visibleCells) {
    const auto &cell=index.spatialCells[cellIndex];
    bool ready=detailedCandidates.count(cellIndex) &&
        cell.instanceCount<=budgets_.maximumActiveInstances-active;
    for(uint32_t i=cell.firstInstance;ready && i<cell.firstInstance+cell.instanceCount;++i) {
      const auto &instance=index.instances[i];
      ready=hasAssetPages(instance.geometryId,instance.appearanceId,pageReady);
    }
    publication.push_back({cellIndex,cell.firstInstance,cell.instanceCount,ready,bounds_[cellIndex]});
    if(ready){active+=cell.instanceCount;++detailed;}
  }
  // Once the new cut is known, unused pages may be evicted. No published
  // detailed cell can lose an asset during this transaction.
  for(auto it=resident_.begin();it!=resident_.end();) {
    if(wanted.count(it->first)) {++it;continue;}
    poolBytes(index.pages[it->first].kind)-=it->second.bytes.size();
    it=resident_.erase(it);++stats_.evictions;
  }
  published_=std::move(publication);
  stats_.frame=frame;
  stats_.visibleCells=uint32_t(visibleCells.size());
  stats_.detailedCells=detailed;
  stats_.fallbackCells=stats_.visibleCells-detailed;
  stats_.activeInstances=active;
  std::unordered_set<uint64_t> residentGeometry,residentAppearance;
  for(const auto &cell:published_) if(cell.detailed)
    for(uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i) {
      residentGeometry.insert(index.instances[i].geometryId);
      residentAppearance.insert(index.instances[i].appearanceId);
    }
  stats_.distinctResidentGeometry=uint32_t(residentGeometry.size());
  stats_.distinctResidentAppearances=uint32_t(residentAppearance.size());
  return true;
}
bool CityVoxelSceneRuntime::updateRadius(uint64_t frame,const std::array<double,3> &camera,
    double radiusMetres,std::string *error) {
  if(!std::isfinite(radiusMetres) || radiusMetres<0 ||
      !std::all_of(camera.begin(),camera.end(),[](double x){return std::isfinite(x);} ))
    return fail(error,"City runtime camera radius is invalid");
  return update(frame,[&](uint32_t,const Bounds &bounds){
    double distanceSquared=0;
    for(unsigned axis=0;axis<3;++axis) {
      const double delta=camera[axis]<bounds.minimum[axis] ? bounds.minimum[axis]-camera[axis] :
          camera[axis]>bounds.maximum[axis] ? camera[axis]-bounds.maximum[axis] : 0.0;
      distanceSquared+=delta*delta;
    }
    return distanceSquared<=radiusMetres*radiusMetres;
  },error,[&](uint32_t,const Bounds &bounds){
    double distanceSquared=0;
    for(unsigned axis=0;axis<3;++axis) {
      const double centre=0.5*(bounds.minimum[axis]+bounds.maximum[axis]);
      distanceSquared+=(centre-camera[axis])*(centre-camera[axis]);
    }
    return distanceSquared;
  });
}
} // namespace virtualgeometry
