#undef NDEBUG
#include <cstdio>
#include "editor/virtualgeometry/CityVoxelCityAssembler.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "virtualgeometry/CityVoxelSceneRuntime.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <iostream>

namespace svr=voxel::svraster;
using namespace virtualgeometry;

int main() {
  svr::CityVoxelAssetCompiler compiler;
  std::string error;
  svr::CityVoxelCompiledPlacement wall{},window{};
  const std::vector<svr::CityVoxelCell> wallCells={
      {{{0,0,0}},0xffaaaaaau,1u},{{{0,1,0}},0xffaaaaaau,1u},
      {{{1,0,0}},0xffaaaaaau,1u},{{{1,1,0}},0xffaaaaaau,1u}};
  const std::vector<svr::CityVoxelCell> windowCells={
      {{{0,0,0}},0xff332211u,2u}};
  assert(compiler.submit(10,100000,wallCells,wall,&error));
  assert(compiler.submit(11,100000,windowCells,window,&error));
  CityVoxelCityAssembler builder(compiler.geometry(),compiler.appearances(),100000);
  using Part=CityVoxelCityAssembler::Part;
  for(uint32_t house=0;house<1000;++house) {
    const std::vector<Part> parts={
        {uint64_t(house)*2+1,wall.geometryId,wall.appearanceId,{0,0,0}},
        {uint64_t(house)*2+2,window.geometryId,window.appearanceId,{3,1,0}}};
    assert(builder.addOwner(uint64_t(house)+100,{int32_t(house%40)*20,0,
        int32_t(house/40)*20},uint8_t(house%4),parts,&error));
  }
  const auto before=builder.stats();
  assert(!builder.addOwner(110,{0,0,0},0,
      {{3000,wall.geometryId,wall.appearanceId,{0,0,0}}},&error));
  assert(!builder.addOwner(5000,{0,0,0},0,
      {{1,wall.geometryId,wall.appearanceId,{0,0,0}}},&error));
  assert(builder.stats().placements==before.placements);
  CityVoxelStaticScene city;
  std::unordered_map<uint64_t,uint64_t> owners;
  assert(builder.finish(city,owners,&error));
  assert(before.owners==1000 && before.placements==2000 &&
      before.sharedGeometry==2 && before.appearances==2 &&
      before.placementBytes==1000*sizeof(CityVoxelStaticBuilding)+
          1000*sizeof(CityVoxelStaticCell)+2000*sizeof(CityVoxelStaticPlacement));
  assert(owners.size()==2000 && city.cells().size()==1000 &&
      city.cells()[0].minCell==(std::array<int32_t,3>{0,0,0}) &&
      city.cells()[0].maxCell==(std::array<int32_t,3>{4,2,1}));
  const auto path=std::filesystem::temp_directory_path()/"city-voxel-thousand-instances.cvscene";
  assert(writeCityVoxelCompiledScene(path.string(),city,compiler.geometry(),
      compiler.appearances(),{{1u,0u,1u},{2u,0u,2u}},1u,2u,{},&error,&owners));
  CityVoxelReleaseAuditReport report;
  std::vector<CityVoxelExpectedOwner> manifest;
  for(uint32_t house=0;house<1000;++house)
    manifest.push_back({uint64_t(house)+100,"house"});
  assert(auditCityVoxelSceneForRelease(path.string(),manifest,{},100000,report,&error));
  assert(report.instances==2000 && report.owners==1000 && report.traceAssets==2);
  const auto repacked=std::filesystem::temp_directory_path()/
      "city-voxel-thousand-instances-repacked.cvscene";
  const bool repackedOk=repackCityVoxelSceneInstances(path.string(),repacked.string(),city,owners,
      3u,4u,{},&error);
  if(!repackedOk) std::fprintf(stderr,"repack: %s\n",error.c_str());
  assert(repackedOk);
  assert(auditCityVoxelSceneForRelease(repacked.string(),manifest,{},100000,
      report,&error));
  svr::CityVoxelSceneFileReader first,second;
  assert(first.open(path.string(),{},&error) && second.open(repacked.string(),{},&error));
  assert(second.index().instances.size()==2000 && second.index().geometry.size()==2 &&
      second.index().appearances.size()==2);
  for(uint32_t page=0;page<first.index().pages.size();++page) {
    std::vector<uint8_t> sourceBytes,copyBytes;
    assert(first.readPage(page,sourceBytes,&error) &&
        second.readPage(page,copyBytes,&error) && sourceBytes==copyBytes);
  }
  std::filesystem::remove(repacked);
  std::filesystem::remove(path);
  // A real dwelling grid can be rotated by an arbitrary city angle without
  // changing its shared geometry or rounding individual parts to world cells.
  CityVoxelCityAssembler framed(compiler.geometry(),compiler.appearances(),100000);
  assert(framed.addOwnerFrame(9001,{123456,0,654321},8192,
      {{9002,wall.geometryId,wall.appearanceId,{20,0,10}},
       {9003,window.geometryId,window.appearanceId,{23,1,10}}},&error));
  CityVoxelStaticScene framedCity;
  std::unordered_map<uint64_t,uint64_t> framedOwners;
  assert(framed.finish(framedCity,framedOwners,&error));
  const auto framedPath=std::filesystem::temp_directory_path()/"city-voxel-frame.cvscene";
  assert(writeCityVoxelCompiledScene(framedPath.string(),framedCity,
      compiler.geometry(),compiler.appearances(),{{1u,0u,1u},{2u,0u,2u}},
      10u,11u,{},&error,&framedOwners));
  svr::CityVoxelSceneFileReader framedFile;
  assert(framedFile.open(framedPath.string(),{},&error));
  assert(framedFile.index().instances.size()==2u &&
      framedFile.index().instances[0].frameId==8192u &&
      framedFile.index().instances[0].gridPosition[0]==20 &&
      framedFile.index().spatialCells[0].originMillimetres[0]==123456);
  CityVoxelStaticScene loadedFrame;
  assert(loadedFrame.loadSceneIndex(framedFile.index(),&error));
  const auto beforeFrame=framedCity.worldPosition(framedCity.placements()[0]);
  const auto afterFrame=loadedFrame.worldPosition(loadedFrame.placements()[0]);
  assert(std::abs(beforeFrame.x()-afterFrame.x())<0.0001f &&
      std::abs(beforeFrame.z()-afterFrame.z())<0.0001f);
  CityVoxelSceneRuntime framedRuntime;
  assert(framedRuntime.open(framedPath.string(),{}, {},&error));
  assert(framedRuntime.cellBounds(0).minimum[0]<beforeFrame.x() &&
      framedRuntime.cellBounds(0).maximum[0]>beforeFrame.x());
  const auto framedRepack=std::filesystem::temp_directory_path()/"city-voxel-frame-repack.cvscene";
  assert(repackCityVoxelSceneInstances(framedPath.string(),framedRepack,
      framedCity,framedOwners,12u,13u,{},&error));
  assert(auditCityVoxelSceneForRelease(framedRepack.string(),{{9001,"house"}},
      {},100000,report,&error));
  std::filesystem::remove(framedRepack);
  std::filesystem::remove(framedPath);
  std::cout<<"City assembler: 1,000 owners, 2,000 placements, 2 shared geometry assets\n";
}
