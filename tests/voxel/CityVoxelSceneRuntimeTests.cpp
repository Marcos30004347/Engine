#undef NDEBUG
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "virtualgeometry/CityVoxelSceneRuntime.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include <cassert>
#include <filesystem>
#include <iostream>

using namespace virtualgeometry;
namespace svr=voxel::svraster;

int main() {
  svr::CityVoxelAssetCompiler compiler;
  svr::CityVoxelCompiledPlacement first,second;
  std::string error;
  assert(compiler.submit(1,100000,{{{{0,0,0}},0xff0000ffu,10}},first,&error));
  assert(compiler.submit(2,100000,{{{{0,0,0}},0xff00ff00u,11}},second,&error));
  CityVoxelStaticScene city;
  assert(city.load({{first.geometryId,"shared",0}},
      {first.appearanceId,second.appearanceId},{{{0,0,0},0}},
      {{{0,0,0},{5,5,5},0,1},{{100,0,0},{105,5,5},1,1}},
      {{101,0,0,0,{0,0,0}},{202,0,1,0,{100,0,0}}},100000,&error));
  const auto path=std::filesystem::temp_directory_path()/"city-voxel-phase10-runtime.cvscene";
  assert(writeCityVoxelCompiledScene(path.string(),city,compiler.geometry(),
      compiler.appearances(),{{10,0,101},{11,0,102}},11,22,{},&error));
  CityVoxelSceneRuntime runtime;
  CityVoxelSceneRuntime::Budgets budgets;
  budgets.metadataBytes=1<<20;budgets.instanceBytes=1<<20;
  budgets.geometryEncodedBytes=1<<20;
  budgets.appearanceBytes=std::max(compiler.appearances()[0].bytes.size(),
                                   compiler.appearances()[1].bytes.size());
  budgets.traceBytes=1024;budgets.fallbackBytes=40;
  budgets.virtualGeometryBytes=1<<20;
  budgets.stagingBytes=1<<20;budgets.maximumReadsPerFrame=1;
  budgets.maximumActiveInstances=1;
  assert(runtime.open(path.string(),budgets,{},&error));
  assert(runtime.index().spatialCells.size()==2);
  assert(runtime.updateRadius(1,{0,0,0},2,&error));
  assert(runtime.publishedCells().size()==1 && !runtime.publishedCells()[0].detailed);
  for(uint64_t frame=2;frame<=6;++frame)
    assert(runtime.updateRadius(frame,{0,0,0},2,&error));
  assert(runtime.publishedCells()[0].detailed && runtime.statistics().activeInstances==1);
  assert(runtime.statistics().distinctResidentGeometry==1 &&
      runtime.statistics().distinctResidentAppearances==1);
  svr::CityVoxelCompiledGeometry residentGeometry;
  svr::CityVoxelCompiledAppearance residentAppearance;
  svr::CityVoxelVGPageView residentVG;
  CityVoxelHardwareMesh residentTrace;
  assert(runtime.residentGeometry(0,residentGeometry,&error) &&
      runtime.residentAppearance(0,residentAppearance,&error) &&
      runtime.residentTrace(0,residentTrace,&error) &&
      runtime.residentVirtualGeometry(0,residentVG,&error) &&
      residentGeometry.bytes==compiler.geometry()[0].bytes &&
      residentAppearance.bytes==compiler.appearances()[0].bytes &&
      residentVG.vgFileSize>0 && residentTrace.triangleAddress.size()==12u);
  assert(runtime.updateRadius(7,{10,0,0},2,&error));
  assert(runtime.publishedCells().size()==1 && runtime.publishedCells()[0].cellIndex==1 &&
      !runtime.publishedCells()[0].detailed);
  for(uint64_t frame=8;frame<=13;++frame)
    assert(runtime.updateRadius(frame,{10,0,0},2,&error));
  assert(runtime.publishedCells()[0].detailed && runtime.statistics().activeInstances==1);
  assert(runtime.statistics().appearanceBytes<=budgets.appearanceBytes &&
      runtime.statistics().traceBytes<=budgets.traceBytes &&
      runtime.statistics().virtualGeometryBytes<=budgets.virtualGeometryBytes &&
      runtime.statistics().fallbackBytes<=budgets.fallbackBytes &&
      runtime.statistics().evictions>0 && runtime.statistics().fallbackCells==0);
  assert(runtime.updateRadius(14,{0,0,0},2,&error));
  for(uint64_t frame=15;frame<=20;++frame)
    assert(runtime.updateRadius(frame,{0,0,0},2,&error));
  assert(runtime.publishedCells()[0].detailed && runtime.statistics().activeInstances==1);
  const uint64_t readsBeforeDemandSpike=runtime.statistics().pageReads;
  assert(runtime.updateRadius(21,{5,0,0},20,&error));
  assert(runtime.statistics().visibleCells==2 &&
      runtime.statistics().detailedCells==1 &&
      runtime.statistics().fallbackCells==1 &&
      runtime.statistics().activeInstances==1 &&
      runtime.statistics().pageReads-readsBeforeDemandSpike<=1);
  std::filesystem::remove(path);
  std::cout<<"City voxel scene runtime teleport, fallback and eviction tests passed\n";
}
