#undef NDEBUG
#include "virtualgeometry/CityVoxelTraceScene.hpp"
#include "virtualgeometry/CityVoxelHardwareMesh.hpp"
#include "editor/virtualgeometry/CityVoxelScenePackager.hpp"
#include "editor/virtualgeometry/CityVoxelReleaseAudit.hpp"
#include "voxel/svraster/CityVoxelFallback.hpp"
#include "voxel/svraster/CityVoxelVGPage.hpp"
#include "voxel/svraster/CityVoxelAssetCompiler.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unordered_map>

using namespace virtualgeometry;
namespace svr = voxel::svraster;

int main() {
  svr::CityVoxelAssetCompiler compiler;
  std::vector<svr::CityVoxelCell> redBlue, green;
  for (int x = 0; x < 2; ++x)
    for (int y = 0; y < 2; ++y) {
      redBlue.push_back({{{x,y,0}},x ? 0xffff0000u : 0xff0000ffu,
                         uint32_t(x ? 11u : 10u)});
      green.push_back({{{x+20,y,0}},0xff00ff00u,12u});
    }
  svr::CityVoxelCompiledPlacement a{}, b{};
  std::string error;
  assert(compiler.submit(1u,100000u,redBlue,a,&error));
  assert(compiler.submit(2u,100000u,green,b,&error));
  assert(compiler.geometry().size() == 1u && compiler.appearances().size() == 2u);
  CityVoxelHardwareMesh mesh;
  assert(buildCityVoxelHardwareMesh(compiler.geometry()[0],mesh,&error));
  assert(mesh.indices.size()/3 == 32u && mesh.triangleAddress.size() == 32u);
  assert(mesh.positions.size()/3 < 32u); // Shared lattice corners, not four vertices per face.
  for (auto address:mesh.triangleAddress) {
    const auto &cell=mesh.cells[address>>3u];
    assert(cell[0]>=0 && cell[0]<2 && cell[1]>=0 && cell[1]<2 && cell[2]==0);
  }
  std::vector<uint8_t> serializedMesh;
  assert(serializeCityVoxelHardwareMesh(compiler.geometry()[0].id,
      compiler.geometry()[0].contentHash,mesh,serializedMesh,&error));
  CityVoxelHardwareMesh restoredMesh;
  assert(deserializeCityVoxelHardwareMesh(serializedMesh.data(),serializedMesh.size(),
      compiler.geometry()[0].id,compiler.geometry()[0].contentHash,restoredMesh,&error));
  assert(restoredMesh.indices==mesh.indices &&
      restoredMesh.triangleAddress==mesh.triangleAddress &&
      restoredMesh.cells==mesh.cells);
  serializedMesh.back()=0xff;
  assert(!deserializeCityVoxelHardwareMesh(serializedMesh.data(),serializedMesh.size(),
      compiler.geometry()[0].id,compiler.geometry()[0].contentHash,restoredMesh,&error));

  CityVoxelStaticScene city;
  const uint64_t firstId = 0x100000001ull, secondId = 0x200000002ull;
  const uint64_t rotatedId = 0x300000003ull;
  assert(city.load({{compiler.geometry()[0].id,"shared",0u}},
      {compiler.appearances()[0].id,compiler.appearances()[1].id},
      {{{0,0,0},0u},{{30,0,0},1u}},
      {{{-10,-10,-10},{100,100,100},0u,3u}},
      {{firstId,0u,0u,0u,{0,0,0}},
       {secondId,0u,1u,0u,{10,0,0}},
       {rotatedId,0u,0u,1u,{0,0,0}}},
      100000u,&error));

  const auto &rotatedPlacement = city.placements()[2];
  const auto rotatedX = city.worldRotation(rotatedPlacement) * math::Vec3f(1,0,0);
  assert(std::abs(rotatedX.x()) < 0.0001f &&
         std::abs(rotatedX.z() - 1.0f) < 0.0001f);

  CityVoxelTraceScene scene;
  assert(scene.build(city,compiler.geometry(),compiler.appearances(),&error));
  const auto filePath=std::filesystem::temp_directory_path()/"city-voxel-compiled-phase9.cvscene";
  const std::unordered_map<uint64_t,uint64_t> ownerByPlacement{
      {firstId,101u},{secondId,101u},{rotatedId,202u}};
  assert(writeCityVoxelCompiledScene(filePath.string(),city,compiler.geometry(),
      compiler.appearances(),{{10u,0u,101u},{11u,0u,102u},{12u,0u,103u}},
      0xabcdu,0x1234u,{},&error,&ownerByPlacement));
  const std::unordered_map<uint64_t,uint64_t> missingOwner{{firstId,101u},{secondId,101u}};
  assert(!writeCityVoxelCompiledScene(filePath.string(),city,compiler.geometry(),
      compiler.appearances(),{{10u,0u,101u},{11u,0u,102u},{12u,0u,103u}},
      0xabcdu,0x1234u,{},&error,&missingOwner));
  assert(!writeCityVoxelCompiledScene(filePath.string(),city,compiler.geometry(),
      compiler.appearances(),{{10u,0u,101u}},0xabcdu,0x1234u,{},&error));
  svr::CityVoxelSceneFileReader file;
  assert(file.open(filePath.string(),{},&error));
  CityVoxelReleaseAuditReport releaseReport;
  assert(auditCityVoxelSceneForRelease(filePath.string(),
      {{101u,"dwelling"},{202u,"tower"}}, {},100000u,releaseReport,&error));
  assert(releaseReport.instances==3u && releaseReport.owners==2u &&
      releaseReport.traceAssets==1u && releaseReport.vgAssets==1u &&
      releaseReport.fallbackCells==1u && releaseReport.unknownMaterialProfiles==0u &&
      releaseReport.coveredByCategory.at("dwelling")==1u &&
      releaseReport.coveredByCategory.at("tower")==1u);
  assert(!auditCityVoxelSceneForRelease(filePath.string(),
      {{101u,"dwelling"},{303u,"tower"}}, {},100000u,releaseReport,&error));
  assert(releaseReport.missingOwnerIds==std::vector<uint64_t>{303u} &&
      releaseReport.unexpectedOwnerIds==std::vector<uint64_t>{202u});
  assert(file.index().geometry.size()==1u && file.index().appearances.size()==2u &&
      file.index().instances.size()==3u && file.index().pages.size()==6u &&
      file.index().instances[2].quarterTurn==1u &&
      file.index().instances[2].gridPosition[0]==30 &&
      file.index().instances[0].ownerId==101u &&
      file.index().instances[2].ownerId==202u);
  CityVoxelStaticScene fileCity;
  assert(fileCity.loadSceneIndex(file.index(),&error) &&
      fileCity.placements().size()==city.placements().size());
  for(size_t i=0;i<city.placements().size();++i) {
    const auto original=city.worldPosition(city.placements()[i]);
    const auto loaded=fileCity.worldPosition(fileCity.placements()[i]);
    assert(fileCity.placements()[i].stableId==city.placements()[i].stableId &&
        std::abs(original.x()-loaded.x())<0.0001f &&
        std::abs(original.y()-loaded.y())<0.0001f &&
        std::abs(original.z()-loaded.z())<0.0001f);
  }
  auto translatedIndex=file.index();
  translatedIndex.cityOriginMillimetres={1250,-500,250};
  CityVoxelStaticScene translatedCity;
  assert(translatedCity.loadSceneIndex(translatedIndex,&error));
  const auto translated=translatedCity.worldPosition(translatedCity.placements()[0]);
  assert(std::abs(translated.x()-1.25f)<0.0001f &&
      std::abs(translated.y()+0.5f)<0.0001f &&
      std::abs(translated.z()-0.25f)<0.0001f);
  std::vector<uint8_t> assetPage;
  assert(file.readPage(0u,assetPage,&error) && assetPage==compiler.geometry()[0].bytes);
  assert(file.readPage(1u,assetPage,&error) && assetPage==compiler.appearances()[0].bytes);
  assert(file.readPage(2u,assetPage,&error) && assetPage==compiler.appearances()[1].bytes);
  std::vector<uint8_t> tracePage;
  assert(file.readPage(3u,tracePage,&error));
  assert(deserializeCityVoxelHardwareMesh(tracePage.data(),tracePage.size(),
      compiler.geometry()[0].id,compiler.geometry()[0].contentHash,restoredMesh,&error));
  assert(restoredMesh.triangleAddress==mesh.triangleAddress);
  svr::CityVoxelFallbackProxy fallback;
  assert(file.readPage(4u,tracePage,&error) &&
      svr::decodeCityVoxelFallback(tracePage.data(),tracePage.size(),fallback,&error) &&
      fallback.instanceCount==3u);
  svr::CityVoxelVGPageView vg;
  assert(file.readPage(5u,tracePage,&error) &&
      svr::decodeCityVoxelVGPage(tracePage.data(),tracePage.size(),
          compiler.geometry()[0].id,compiler.geometry()[0].contentHash,vg,&error) &&
      !vg.clusterSources.empty() && vg.vgFileSize>0u);
  const auto vgPath=std::filesystem::temp_directory_path()/"city-voxel-compiled-phase9.vg";
  {
    std::ofstream out(vgPath,std::ios::binary|std::ios::trunc);
    out.write(reinterpret_cast<const char *>(vg.vgFileBytes),vg.vgFileSize);
    assert(out.good());
  }
  VirtualGeometryFile vgFile(vgPath.string());
  assert(vgFile.isOpen() && !vgFile.getPageTable().empty() &&
      !vgFile.getHierarchy().empty());
  std::filesystem::remove(vgPath);
  std::filesystem::remove(filePath);
  const auto stats = scene.stats();
  assert(stats.uniqueGeometry == 1u && stats.placements == 3u &&
         stats.tlasNodes == 1u && stats.treeNodeBytes != 0u);
  const CityVoxelTraceScene::Ray first{{-1.0,0.05,0.05},{1.0,0.0,0.0},10.0};
  const CityVoxelTraceScene::Ray second{{0.5,0.05,0.05},{1.0,0.0,0.0},10.0};
  const CityVoxelTraceScene::Ray rotated{{2.5,0.05,0.05},{1.0,0.0,0.0},10.0};
  const auto hitA = scene.trace(first);
  const auto hitB = scene.trace(second);
  const auto hitRotated = scene.trace(rotated);
  assert(hitA.status == CityVoxelTraceScene::Status::Hit &&
         hitA.stableId == firstId && hitA.modelCell == (std::array<int32_t,3>{0,0,0}) &&
         hitA.rgba == 0xff0000ffu && hitA.materialProfile == 10u &&
         hitA.opacity == 1.0f && hitA.worldNormal[0] == -1.0f);
  assert(hitB.status == CityVoxelTraceScene::Status::Hit &&
         hitB.stableId == secondId && hitB.rgba == 0xff00ff00u &&
         hitB.materialProfile == 12u);
  assert(hitRotated.status == CityVoxelTraceScene::Status::Hit &&
         hitRotated.stableId == rotatedId &&
         hitRotated.modelCell == (std::array<int32_t,3>{0,0,0}) &&
         hitRotated.worldNormal[0] == -1.0f);
  assert(scene.shadowOccluded(first));
  assert(!scene.shadowOccluded({{-1.0,4.0,0.05},{1.0,0.0,0.0},10.0}));

  std::vector<svr::SparseVoxelTree64DynamicModelSource> models;
  std::vector<svr::SparseVoxelTree64DynamicInstanceSource> instances;
  scene.exportTree64Sources(models,instances);
  assert(models.size() == 1u && instances.size() == 3u &&
         models[0].prebuiltNodes.size() * sizeof(svr::SparseVoxelTree64NodeGPU) ==
             stats.treeNodeBytes &&
         instances[0].modelId == instances[1].modelId &&
         instances[1].modelId == instances[2].modelId &&
         scene.stableIdForTree64Instance(instances[2].stableInstanceId) == rotatedId);

  assert(scene.publishResidency({1u},{1u,0u},2u,&error));
  const auto unavailable = scene.trace(second);
  assert(unavailable.status == CityVoxelTraceScene::Status::Unavailable &&
         unavailable.stableId == secondId && scene.shadowOccluded(second));
  assert(!scene.publishResidency({1u},{1u,1u},2u,&error));
  assert(scene.publishResidency({1u},{1u,1u},3u,&error));
  assert(scene.trace(second).status == CityVoxelTraceScene::Status::Hit);

  auto translucent = compiler.appearances();
  const size_t alpha = translucent[0].codec == svr::CityVoxelCodec::Palette8 ||
      translucent[0].codec == svr::CityVoxelCodec::Palette16 ? 7u : 3u;
  translucent[0].bytes[alpha] = 127u;
  CityVoxelTraceScene rejected;
  assert(!rejected.build(city,compiler.geometry(),translucent,&error));
  std::cout << "City voxel shared Tree64 trace scene tests passed\n";
}
