#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include "editor/virtualgeometry/CityVoxelVirtualGeometryCompiler.hpp"
#include "editor/virtualgeometry/CityVoxelSurfaceVirtualGeometry.hpp"
#include "voxel/svraster/CityVoxelFallback.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <thread>
#include <mutex>

namespace virtualgeometry {
namespace {
bool fail(std::string *error, const char *message) {
  if (error) *error=message;
  return false;
}

bool makeVGPage(const voxel::svraster::CityVoxelCompiledGeometry &geometry,
    const voxel::svraster::CityVoxelSceneLimits &limits,
    std::vector<uint8_t> &result,std::string *error) {
  using namespace voxel::svraster;
  CityVoxelVGSettings settings;
  settings.maximumEncodedBytes=limits.maximumStoredPageBytes;
  CityVoxelVGAsset asset;
  if(!buildCityVoxelVirtualGeometry(geometry,settings,asset,error))return false;
  std::vector<uint8_t> blob;
  if(!virtualgeometry::VirtualGeometryFile::writeToMemory(asset.encoded,asset.buildPages,
      virtualgeometry::MESHLET_LZ4,blob,1u))
    return fail(error,"Cannot encode city virtual geometry file");
  if(blob.size()>limits.maximumStoredPageBytes)
    return fail(error,"City virtual geometry page exceeds file limit");
  const size_t blobBytes=blob.size();
  const uint64_t packageBytes=32ull+asset.clusterSources.size()*20ull+blobBytes;
  if(packageBytes>limits.maximumStoredPageBytes || blobBytes>UINT32_MAX ||
      asset.clusterSources.size()>UINT32_MAX)
    return fail(error,"City virtual geometry package exceeds page limit");
  std::vector<uint8_t> bytes;
  bytes.reserve(size_t(packageBytes));
  auto put=[&](uint64_t value,unsigned width) {
    for(unsigned i=0;i<width;++i)bytes.push_back(uint8_t(value>>(i*8)));
  };
  put(0x47565643u,4);put(1u,4);put(geometry.id,8);put(geometry.contentHash,8);
  put(asset.clusterSources.size(),4);put(blobBytes,4);
  for(const auto &source:asset.clusterSources) {
    for(int32_t x:source.brickOriginCells)put(uint32_t(x),4);
    put(source.cellScale,4);put(source.level,4);
  }
  bytes.insert(bytes.end(),blob.begin(),blob.end());
  result=std::move(bytes);
  return true;
}
bool worldCell(const CityVoxelStaticScene &city,
    const CityVoxelStaticPlacement &placement, std::array<int32_t,3> &out) {
  const auto &building=city.buildings()[placement.buildingIndex];
  if(building.localFrame) {out=placement.localOriginCell;return true;}
  const int64_t x=placement.localOriginCell[0], z=placement.localOriginCell[2];
  int64_t rx=x, rz=z;
  switch (building.quarterTurns) {
    case 1: rx=-z;rz=x;break;
    case 2: rx=-x;rz=-z;break;
    case 3: rx=z;rz=-x;break;
    default:break;
  }
  const int64_t values[3]={rx+building.originCell[0],
      int64_t(placement.localOriginCell[1])+building.originCell[1],
      rz+building.originCell[2]};
  for (unsigned axis=0;axis<3;++axis) {
    if (values[axis]<INT32_MIN || values[axis]>INT32_MAX) return false;
    out[axis]=int32_t(values[axis]);
  }
  return true;
}
}

bool writeCityVoxelCompiledScene(const std::string &path,
    const CityVoxelStaticScene &city,
    const std::vector<voxel::svraster::CityVoxelCompiledGeometry> &geometry,
    const std::vector<voxel::svraster::CityVoxelCompiledAppearance> &appearances,
    const std::vector<voxel::svraster::CityVoxelMaterialReference> &materials,
    uint64_t sourceHash, uint64_t settingsHash,
    const voxel::svraster::CityVoxelSceneLimits &limits, std::string *error,
    const std::unordered_map<uint64_t,uint64_t> *ownerByPlacementId,
    const CityVoxelScenePackOptions &options) {
  using namespace voxel::svraster;
  if (error) error->clear();
  if (geometry.empty() || geometry.size()!=city.models().size() ||
      appearances.empty() || !sourceHash || !settingsHash)
    return fail(error,"City packager asset tables or source hashes are invalid");
  CityVoxelSceneIndex index;
  index.sourceHash=sourceHash;
  index.settingsHash=settingsHash;
  index.nominalPitchMicrometres=city.pitchMicrometres();
  index.requiredFeatures=kCityVoxelFeatureSharedGeometry|kCityVoxelFeatureVirtualGeometry|
      (options.tracePages?kCityVoxelFeatureTracePages:0u);
  index.materials=materials;
  std::unordered_set<uint32_t> profiles;
  for (const auto &material:materials)
    if (!material.assetHash || !profiles.insert(material.profileId).second)
      return fail(error,"City packager material reference is invalid");
  std::unordered_map<uint64_t,uint32_t> geometryById,appearanceById;
  for (uint32_t i=0;i<geometry.size();++i) {
    const auto &asset=geometry[i];
    if (asset.id!=city.models()[i].geometryId ||
        asset.pitchMicrometres!=city.pitchMicrometres() ||
        !geometryById.emplace(asset.id,i).second)
      return fail(error,"City packager geometry identity or pitch differs from placement scene");
    CityVoxelGeometryAsset record;
    record.id=asset.id;record.contentHash=asset.contentHash;
    record.voxelAddressHash=asset.addressHash;
    record.rootPage=uint32_t(index.pages.size());
    record.firstPage=record.rootPage;record.pageCount=1;
    record.pitchMicrometres=asset.pitchMicrometres;
    record.maximumCell=asset.extent;
    index.geometry.push_back(record);
    index.pages.push_back({uint32_t(index.pages.size()),CityVoxelPageKind::Geometry,
        CityVoxelCodec::Raw,asset.id});
  }
  for (uint32_t i=0;i<appearances.size();++i) {
    const auto &asset=appearances[i];
    const auto found=geometryById.find(asset.geometryId);
    if (found==geometryById.end() ||
        asset.geometryAddressHash!=geometry[found->second].addressHash ||
        !appearanceById.emplace(asset.id,i).second)
      return fail(error,"City packager appearance does not match shared geometry");
    std::vector<CityVoxelCell> values;
    if (!decodeCityVoxelAppearance(asset,values,error)) return false;
    for (const auto &value:values)
      if ((value.rgba>>24u)!=255u || !profiles.count(value.materialProfile))
        return fail(error,"City packager appearance uses unsupported alpha or missing material profile");
    CityVoxelAppearanceAsset record;
    record.id=asset.id;record.contentHash=asset.contentHash;
    record.geometryAddressHash=asset.geometryAddressHash;
    record.geometryId=asset.geometryId;
    record.rootPage=uint32_t(index.pages.size());
    record.firstPage=record.rootPage;record.pageCount=1;record.codec=asset.codec;
    index.appearances.push_back(record);
    CityVoxelPageRecord page;
    page.id=uint32_t(index.pages.size());page.kind=CityVoxelPageKind::Appearance;
    page.codec=asset.codec;page.assetId=asset.id;
    if (asset.occupiedCount>UINT32_MAX/8u)
      return fail(error,"City appearance decoded page exceeds 32-bit length");
    page.decodedBytes=asset.occupiedCount*8u;
    index.pages.push_back(page);
  }
  const uint32_t firstTracePage=uint32_t(index.pages.size());
  constexpr uint32_t kTraceGroupTriangles=8192u;
  struct TracePageRef { uint32_t assetIndex, groupIndex; };
  std::vector<TracePageRef> traceRefs;
  for (uint32_t assetIndex=0;options.tracePages && assetIndex<geometry.size();++assetIndex) {
    CityVoxelHardwareMesh mesh;
    if (!buildCityVoxelHardwareMesh(geometry[assetIndex],mesh,error)) return false;
    const uint64_t groups=(mesh.triangleAddress.size()+kTraceGroupTriangles-1u)/
        kTraceGroupTriangles;
    if (!groups || groups>UINT32_MAX ||
        uint64_t(index.pages.size())+groups>UINT32_MAX)
      return fail(error,"City trace group count exceeds scene page limit");
    for(uint32_t group=0;group<groups;++group) {
      traceRefs.push_back({assetIndex,group});
      index.pages.push_back({uint32_t(index.pages.size()),CityVoxelPageKind::Trace,
          CityVoxelCodec::Raw,geometry[assetIndex].id});
    }
  }
  index.spatialCells.reserve(city.cells().size());
  index.instances.reserve(city.placements().size());
  for (uint32_t cellIndex=0;cellIndex<city.cells().size();++cellIndex) {
    const auto &cell=city.cells()[cellIndex];
    CityVoxelSpatialCell spatial;
    spatial.id=uint64_t(cellIndex)+1;
    spatial.firstInstance=cell.firstPlacement;
    spatial.instanceCount=cell.placementCount;
    if(cell.placementCount) {
      const auto &building=city.buildings()[city.placements()[cell.firstPlacement].buildingIndex];
      if(building.localFrame)spatial.originMillimetres=building.originMillimetres;
    }
    index.spatialCells.push_back(spatial);
    for (uint32_t p=cell.firstPlacement;p<cell.firstPlacement+cell.placementCount;++p) {
      const auto &placement=city.placements()[p];
      const auto geometryId=city.models()[placement.geometryIndex].geometryId;
      const auto appearanceId=city.appearanceId(p);
      const auto appearance=appearanceById.find(appearanceId);
      if (appearance==appearanceById.end() ||
          appearances[appearance->second].geometryId!=geometryId)
        return fail(error,"City packager placement appearance differs from geometry");
      CityVoxelInstance instance;
      instance.stableId=placement.stableId;
      instance.geometryId=geometryId;
      instance.appearanceId=appearanceId;
      if(ownerByPlacementId) {
        const auto owner=ownerByPlacementId->find(placement.stableId);
        if(owner==ownerByPlacementId->end() || !owner->second)
          return fail(error,"City packager placement has no authored owner");
        instance.ownerId=owner->second;
      } else instance.ownerId=placement.stableId;
      instance.spatialCellId=uint32_t(spatial.id);
      const auto &building=city.buildings()[placement.buildingIndex];
      instance.quarterTurn=uint8_t(building.quarterTurns);
      if(building.localFrame) {
        if(building.originMillimetres!=spatial.originMillimetres ||
            building.yawCode!=city.buildings()[city.placements()[cell.firstPlacement].buildingIndex].yawCode)
          return fail(error,"City spatial cell mixes local owner frames");
        instance.flags=kCityVoxelInstanceLocalFrame;
        instance.frameId=building.yawCode;
      }
      if (!worldCell(city,placement,instance.gridPosition))
        return fail(error,"City packager placement exceeds integer lattice");
      index.instances.push_back(instance);
    }
  }
  const uint32_t firstFallbackPage=uint32_t(index.pages.size());
  std::vector<CityVoxelFallbackProxy> fallback;
  for (auto &cell:index.spatialCells) {
    if (!cell.instanceCount) continue;
    std::array<int64_t,3> minimum{},maximum{};
    minimum.fill(INT64_MAX);maximum.fill(INT64_MIN);
    for (uint32_t i=cell.firstInstance;i<cell.firstInstance+cell.instanceCount;++i) {
      const auto &instance=index.instances[i];
      const auto &asset=geometry[geometryById.at(instance.geometryId)];
      for (unsigned corner=0;corner<8;++corner) {
        const int64_t x=(corner&1u)?asset.extent[0]:0;
        const int64_t y=(corner&2u)?asset.extent[1]:0;
        const int64_t z=(corner&4u)?asset.extent[2]:0;
        if(instance.flags & kCityVoxelInstanceLocalFrame) {
          constexpr double tau=6.2831853071795864769;
          const double angle=tau*double(instance.frameId)/65536.0;
          const double c=std::cos(angle),s=std::sin(angle);
          const double pitch=double(city.pitchMicrometres())*0.001;
          const double lx=double(instance.gridPosition[0])+x;
          const double ly=double(instance.gridPosition[1])+y;
          const double lz=double(instance.gridPosition[2])+z;
          const double values[3]={
              (double(cell.originMillimetres[0])+pitch*(c*lx-s*lz))/pitch,
              (double(cell.originMillimetres[1])+pitch*ly)/pitch,
              (double(cell.originMillimetres[2])+pitch*(s*lx+c*lz))/pitch};
          for(unsigned axis=0;axis<3;++axis) {
            minimum[axis]=std::min(minimum[axis],int64_t(std::floor(values[axis])));
            maximum[axis]=std::max(maximum[axis],int64_t(std::ceil(values[axis])));
          }
          continue;
        }
        std::array<int64_t,3> point{x,y,z};
        switch(instance.quarterTurn) {
          case 1:point={-z,y,x};break;
          case 2:point={-x,y,-z};break;
          case 3:point={z,y,-x};break;
          default:break;
        }
        for(unsigned axis=0;axis<3;++axis) {
          const int64_t world=int64_t(instance.gridPosition[axis])+point[axis];
          minimum[axis]=std::min(minimum[axis],world);
          maximum[axis]=std::max(maximum[axis],world);
        }
      }
    }
    CityVoxelFallbackProxy proxy;
    for(unsigned axis=0;axis<3;++axis) {
      if(minimum[axis]<INT32_MIN || maximum[axis]>INT32_MAX ||
          minimum[axis]>=maximum[axis])
        return fail(error,"City fallback proxy exceeds integer grid");
      proxy.minimumCell[axis]=int32_t(minimum[axis]);
      proxy.maximumCell[axis]=int32_t(maximum[axis]);
    }
    proxy.instanceCount=cell.instanceCount;
    cell.fallbackPage=uint32_t(index.pages.size());
    index.pages.push_back({uint32_t(index.pages.size()),CityVoxelPageKind::Fallback,
        CityVoxelCodec::Raw,0});
    fallback.push_back(proxy);
  }
  const uint32_t firstVGPage=uint32_t(index.pages.size());
  for(const auto &asset:geometry)
    index.pages.push_back({uint32_t(index.pages.size()),CityVoxelPageKind::VirtualGeometry,
        CityVoxelCodec::Raw,asset.id});
  uint32_t cachedTraceAsset=UINT32_MAX;
  std::vector<CityVoxelHardwareMesh> cachedTraceGroups;
  // VG pages are independent per geometry and dominate packing time; build
  // them a batch at a time on all workers, then hand them out in page order.
  constexpr uint32_t kVGBatch=4096u;
  std::vector<std::vector<uint8_t>> vgBatch;
  uint32_t vgBatchFirst=UINT32_MAX;
  const auto buildVGBatch=[&](uint32_t first,std::string *batchError) {
    const uint32_t count=std::min<uint32_t>(kVGBatch,uint32_t(geometry.size())-first);
    vgBatch.assign(count,{});
    std::atomic<uint32_t> next{0};
    std::atomic<bool> failed{false};
    std::mutex failureMutex;
    std::string failure;
    std::vector<std::thread> workers;
    for(uint32_t t=0;t<std::max(1u,options.workers);++t)
      workers.emplace_back([&] {
        for(uint32_t i;(i=next.fetch_add(1))<count && !failed;) {
          std::string local;
          if(options.surfaceVirtualGeometry &&
              buildCityVoxelSurfaceVGPackage(geometry[first+i],limits.maximumStoredPageBytes,vgBatch[i],nullptr,&local))
            continue;
          if(!makeVGPage(geometry[first+i],limits,vgBatch[i],&local)) {
            std::lock_guard<std::mutex> lock(failureMutex);
            if(!failed.exchange(true))failure=local;
          }
        }
      });
    for(auto &worker:workers)worker.join();
    if(failed)return fail(batchError,failure.empty()?"City VG page build failed":failure.c_str());
    vgBatchFirst=first;
    return true;
  };
  const CityVoxelPageProducer producer=[&](uint32_t pageId,
      std::vector<uint8_t> &payload,std::string *pageError) {
    if (pageId<geometry.size()) { payload=geometry[pageId].bytes; return true; }
    if (pageId<firstTracePage) { payload=appearances[pageId-geometry.size()].bytes; return true; }
    if(pageId<firstFallbackPage) {
      const auto ref=traceRefs[pageId-firstTracePage];
      const auto &asset=geometry[ref.assetIndex];
      if(cachedTraceAsset!=ref.assetIndex) {
        CityVoxelHardwareMesh mesh;
        if(!buildCityVoxelHardwareMesh(asset,mesh,pageError) ||
            !splitCityVoxelHardwareMesh(mesh,kTraceGroupTriangles,cachedTraceGroups,pageError))
          return false;
        cachedTraceAsset=ref.assetIndex;
      }
      if(ref.groupIndex>=cachedTraceGroups.size())
        return fail(pageError,"City trace group producer is out of sync");
      const bool encoded=serializeCityVoxelHardwareMesh(asset.id,asset.contentHash,
          cachedTraceGroups[ref.groupIndex],payload,pageError);
      if(ref.groupIndex+1u==cachedTraceGroups.size()) {
        cachedTraceGroups.clear();
        cachedTraceAsset=UINT32_MAX;
      }
      return encoded;
    }
    if(pageId<firstVGPage)
      return encodeCityVoxelFallback(fallback[pageId-firstFallbackPage],payload,pageError);
    const uint32_t asset=pageId-firstVGPage;
    if(vgBatchFirst==UINT32_MAX || asset<vgBatchFirst || asset>=vgBatchFirst+vgBatch.size())
      if(!buildVGBatch(asset-asset%kVGBatch,pageError))return false;
    payload=std::move(vgBatch[asset-vgBatchFirst]);
    return true;
  };
  CityVoxelSceneCompression compression;
  compression.enabled=options.compressPages;
  compression.workers=std::max(1u,options.workers);
  return writeCityVoxelSceneFile(path,std::move(index),limits,producer,error,compression);
}

bool repackCityVoxelSceneInstances(const std::string &libraryPath,
    const std::string &outputPath,const CityVoxelStaticScene &city,
    const std::unordered_map<uint64_t,uint64_t> &ownerByPlacementId,
    uint64_t sourceHash,uint64_t settingsHash,
    const voxel::svraster::CityVoxelSceneLimits &limits,std::string *error) {
  using namespace voxel::svraster;
  if(error)error->clear();
  CityVoxelSceneFileReader library;
  if(!library.open(libraryPath,limits,error))return false;
  CityVoxelSceneIndex index=library.index();
  if(!sourceHash || !settingsHash ||
      index.geometry.size()!=city.models().size() ||
      index.appearances.empty() ||
      index.nominalPitchMicrometres!=city.pitchMicrometres() ||
      libraryPath==outputPath)
    return fail(error,"Instance repack asset tables, hashes or paths differ");
  for(uint32_t i=0;i<index.geometry.size();++i)
    if(index.geometry[i].id!=city.models()[i].geometryId)
      return fail(error,"Instance repack geometry order differs from library");
  // The producer below hands back decoded pages, and the writer takes a
  // non-raw page as already encoded: give the library's pages back as raw
  // (the writer compresses them again) and drop its dictionaries, which the
  // writer retrains and appends after every other page.
  while(!index.pages.empty() && index.pages.back().kind==CityVoxelPageKind::Dictionary)
    index.pages.pop_back();
  for(auto &page:index.pages) {
    page.fileOffset=0;page.storedBytes=0;page.checksum=0;
    page.codec=CityVoxelCodec::Raw;page.decodedBytes=0;
  }
  const uint32_t libraryPages=uint32_t(index.pages.size());
  index.sourceHash=sourceHash;
  index.settingsHash=settingsHash;
  index.cityOriginMillimetres={};
  index.instances.clear();
  index.spatialCells.clear();
  index.instances.reserve(city.placements().size());
  index.spatialCells.reserve(city.cells().size());
  std::unordered_map<uint64_t,const CityVoxelGeometryAsset *> geometry;
  std::unordered_map<uint64_t,uint64_t> appearanceGeometry;
  for(const auto &asset:index.geometry)geometry.emplace(asset.id,&asset);
  for(const auto &asset:index.appearances)
    appearanceGeometry.emplace(asset.id,asset.geometryId);
  std::vector<CityVoxelFallbackProxy> fallbacks;
  fallbacks.reserve(city.cells().size());
  for(uint32_t cellIndex=0;cellIndex<city.cells().size();++cellIndex) {
    const auto &sourceCell=city.cells()[cellIndex];
    if(!sourceCell.placementCount ||
        uint64_t(sourceCell.firstPlacement)+sourceCell.placementCount>
            city.placements().size())
      return fail(error,"Instance repack has an empty or invalid owner cell");
    CityVoxelSpatialCell cell;
    cell.id=uint64_t(cellIndex)+1;
    cell.firstInstance=sourceCell.firstPlacement;
    cell.instanceCount=sourceCell.placementCount;
    const auto &firstBuilding=city.buildings()[city.placements()[cell.firstInstance].buildingIndex];
    if(firstBuilding.localFrame)cell.originMillimetres=firstBuilding.originMillimetres;
    cell.fallbackPage=uint32_t(index.pages.size());
    index.pages.push_back({uint32_t(index.pages.size()),CityVoxelPageKind::Fallback,
        CityVoxelCodec::Raw,0});
    std::array<int64_t,3> low{INT64_MAX,INT64_MAX,INT64_MAX};
    std::array<int64_t,3> high{INT64_MIN,INT64_MIN,INT64_MIN};
    for(uint32_t p=cell.firstInstance;p<cell.firstInstance+cell.instanceCount;++p) {
      const auto &source=city.placements()[p];
      if(source.geometryIndex>=city.models().size() ||
          source.buildingIndex>=city.buildings().size())
        return fail(error,"Instance repack placement indexes are invalid");
      CityVoxelInstance instance;
      instance.stableId=source.stableId;
      instance.geometryId=city.models()[source.geometryIndex].geometryId;
      instance.appearanceId=city.appearanceId(p);
      const auto owner=ownerByPlacementId.find(instance.stableId);
      const auto asset=geometry.find(instance.geometryId);
      const auto appearance=appearanceGeometry.find(instance.appearanceId);
      if(owner==ownerByPlacementId.end() || !owner->second ||
          asset==geometry.end() || appearance==appearanceGeometry.end() ||
          appearance->second!=instance.geometryId)
        return fail(error,"Instance repack placement has no matching owner or asset");
      instance.ownerId=owner->second;
      instance.spatialCellId=uint32_t(cell.id);
      const auto &building=city.buildings()[source.buildingIndex];
      instance.quarterTurn=uint8_t(building.quarterTurns);
      if(building.localFrame) {
        if(!firstBuilding.localFrame || building.originMillimetres!=cell.originMillimetres ||
            building.yawCode!=firstBuilding.yawCode)
          return fail(error,"Instance repack mixes local owner frames");
        instance.flags=kCityVoxelInstanceLocalFrame;
        instance.frameId=building.yawCode;
      } else if(firstBuilding.localFrame)
        return fail(error,"Instance repack mixes local and world placements");
      if(!worldCell(city,source,instance.gridPosition))
        return fail(error,"Instance repack placement exceeds integer grid");
      const auto &bounds=*asset->second;
      if(building.localFrame) {
        for(unsigned axis=0;axis<3;++axis) {
          low[axis]=std::min<int64_t>(low[axis],sourceCell.minCell[axis]);
          high[axis]=std::max<int64_t>(high[axis],sourceCell.maxCell[axis]);
        }
        index.instances.push_back(instance);
        continue;
      }
      for(unsigned corner=0;corner<8;++corner) {
        const int64_t x=corner&1u?bounds.maximumCell[0]:bounds.minimumCell[0];
        const int64_t y=corner&2u?bounds.maximumCell[1]:bounds.minimumCell[1];
        const int64_t z=corner&4u?bounds.maximumCell[2]:bounds.minimumCell[2];
        std::array<int64_t,3> point{x,y,z};
        switch(instance.quarterTurn) {
          case 1:point={-z,y,x};break;
          case 2:point={-x,y,-z};break;
          case 3:point={z,y,-x};break;
          default:break;
        }
        for(unsigned axis=0;axis<3;++axis) {
          const int64_t value=int64_t(instance.gridPosition[axis])+point[axis];
          low[axis]=std::min(low[axis],value);
          high[axis]=std::max(high[axis],value);
        }
      }
      index.instances.push_back(instance);
    }
    CityVoxelFallbackProxy fallback;
    for(unsigned axis=0;axis<3;++axis) {
      if(low[axis]<INT32_MIN || high[axis]>INT32_MAX || low[axis]>=high[axis])
        return fail(error,"Instance repack fallback exceeds integer grid");
      fallback.minimumCell[axis]=int32_t(low[axis]);
      fallback.maximumCell[axis]=int32_t(high[axis]);
    }
    fallback.instanceCount=cell.instanceCount;
    fallbacks.push_back(fallback);
    index.spatialCells.push_back(cell);
  }
  if(index.instances.size()!=city.placements().size())
    return fail(error,"Instance repack omitted placements");
  const CityVoxelPageProducer producer=[&](uint32_t pageId,
      std::vector<uint8_t> &payload,std::string *pageError) {
    return pageId<libraryPages ? library.readPage(pageId,payload,pageError) :
        encodeCityVoxelFallback(fallbacks[pageId-libraryPages],payload,pageError);
  };
  return writeCityVoxelSceneFile(outputPath,std::move(index),limits,producer,error);
}
} // namespace virtualgeometry
