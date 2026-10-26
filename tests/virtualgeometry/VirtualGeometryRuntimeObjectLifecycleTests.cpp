// Runtime object lifecycle: registration, unregistration, slot recycling and
// LRU eviction in VirtualGeometryScene.
//
// Terrain installs virtual-geometry chunks into a live scene and replaces them
// as they refine, continuously, for as long as the application runs. That only
// works if every resource an object holds comes back when it is unregistered:
// its hierarchy allocation, its page-table and priority slot ranges, and its
// page-buffer allocations. Before this existed the only way to reclaim them was
// to recompile the whole render graph, which stalled for hundreds of
// milliseconds and re-bootstrapped terrain streaming with it.
//
// The checks here are the ones that fail loudly if any of that regresses:
//
//   - unregister returns the page table to where it was, so a scene that
//     replaces one object forever does not exhaust it;
//   - a replaced object is immediately usable again, with its pages installed;
//   - a tight hierarchy buffer survives many more replacements than it has room
//     for objects, which is only true if the allocation is really freed;
//   - eviction under page-buffer pressure retires the least recently used page,
//     never a root page and never one that protects an installed child.

#include <algorithm>
#include <map>
#include <tuple>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <thread>
#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
#include "virtualgeometry/raytracing/metal/MetalPageBlasGate.hpp"
#endif
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/CityVoxelVirtualGeometryCompiler.hpp"
#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "math/math.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/CityVoxelStaticScene.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

using namespace rendering;
using namespace virtualgeometry;
namespace svr = voxel::svraster;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

class ControlledBlasGate final : public raytracing::PageBlasGate
{
  using Key = std::tuple<uint64_t,uint32_t,uint64_t,uint64_t>;
  struct Page { uint32_t clusters, completed; };
  std::map<Key,Page> pages;
  static Key key(raytracing::PageBlasKey k) { return {k.owner,k.page,k.generation,k.revision}; }
public:
  uint32_t released = 0;
  State prepareEncoded(raytracing::PageBlasKey k, const uint32_t *words, uint64_t,
      uint32_t, float) override
  {
    pages.emplace(key(k),Page{words[0],0});
    return state(k,words[0]);
  }
  State state(raytracing::PageBlasKey k, uint32_t clusters) override
  {
    auto it=pages.find(key(k));
    if (it==pages.end() || it->second.clusters!=clusters) return State::Missing;
    return it->second.completed==clusters ? State::Ready : State::Pending;
  }
  std::vector<uint32_t> handles(raytracing::PageBlasKey k) override
  {
    auto it=pages.find(key(k));
    if (it==pages.end() || state(k,it->second.clusters)!=State::Ready) return {};
    return std::vector<uint32_t>(it->second.clusters,1);
  }
  void pump() override {}
  bool releasePage(raytracing::PageBlasKey) override { return true; }
  void drainInputReads() override {}
  void complete(bool partial=false)
  { for (auto &[key,page]:pages) if (page.completed<page.clusters) page.completed=page.clusters-(partial ? 1u : 0u); }
  void releaseOwner(uint64_t owner) override
  {
    ++released;
    for (auto it=pages.begin();it!=pages.end();)
      if (std::get<0>(it->first)==owner) it=pages.erase(it); else ++it;
  }
};

uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 3u;
  if (x >= side / 2u)
    height += 4u;
  if (z >= side * 3u / 4u)
    height += 2u;
  return height;
}

bool buildChunkOctree(
    const uint32_t side,
    svr::SparseVoxelBrickHierarchy &hierarchy,
    std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      const uint32_t floor = top > 2u ? top - 2u : 0u;
      for (uint32_t y = floor; y < top; ++y)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        material.albedoRGBA8 = 0xff7f9f5fu;
        material.materialId = 0u;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
      }
    }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 16u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, hierarchy, error);
}

VirtualGeometryScene::RuntimeEncodedObjectInstallInfo makeInstall(
    const std::string &name,
    const VirtualGeometryEncodedData &asset,
    const uint64_t pageGenerationBase,
    const math::Vec3f &position)
{
  VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
  install.name = name;
  install.asset = &asset;
  install.basePageGeneration = pageGenerationBase;
  install.geometryRevision = pageGenerationBase;
  install.materialRevision = 1u;
  install.position = position;
  install.materialIndex = 0u;
  install.hasTracePayload = true;
  install.hasMaterialPayload = false;
  // Decoding is a GPU pass this test does not register; installing the encoded
  // payload is what exercises the allocators.
  install.enqueuePageDecode = false;
  return install;
}

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Virtual geometry runtime object lifecycle");
  os::Logger::log("=========================================");

  svr::SparseVoxelBrickHierarchy hierarchy{};
  std::string error;
  if (!buildChunkOctree(32u, hierarchy, &error))
  {
    os::Logger::errorf("voxel chunk build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.maxRootPageGroups = 1u;

  VirtualGeometryBuildData build{};
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy, 0u, static_cast<uint32_t>(hierarchy.nodes.size()),
          octreeSettings, buildSettings, build, nullptr, &error))
  {
    os::Logger::errorf("octree virtual geometry build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  QuantizationConfig quantization{};
  quantization.quantization_factor = 4u;
  quantization.unit_scale = 100.0f;
  quantization.padMeshlets = true;
  const VirtualGeometryEncodedData asset =
      VirtualGeometryCompressor::encode(build, quantization);
  const uint32_t assetPageCount = static_cast<uint32_t>(asset.pages.size());
  const uint32_t assetHierarchyNodes =
      static_cast<uint32_t>(asset.hierarchy.size());
  os::Logger::logf(
      "  asset pages=%u hierarchyNodes=%u", assetPageCount, assetHierarchyNodes);

  DeviceRequiredLimits limits{};
  const DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute |
      DeviceFeatures::DeviceFeatures_Subgroup_Basic |
      DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
      DeviceFeatures::DeviceFeatures_Timestamp;
  auto *rhi = new backend::vulkan::VulkanRHI(
      backend::vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi->init(surfaces);

  int exitCode = 0;
  try
  {
    auto renderGraphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *renderGraph = renderGraphStorage.get();

    // Deliberately tight: room for about four objects' hierarchies at a time.
    const uint32_t hierarchyBytes = std::max<uint32_t>(
        assetHierarchyNodes * sizeof(VirtualGeometryHierarchy) * 4u, 4096u);
    VirtualGeometryScene scene(
        renderGraph, hierarchyBytes, 64ull * 1024ull * 1024ull);

    {
      os::Logger::log("phase 6: shared city model and bounded static placement activation");
      VirtualGeometryScene::RuntimeObjectCreateInfo model{};
      model.name = "city-shared";
      model.pageCount = assetPageCount;
      model.hierarchy = asset.hierarchy;
      model.quantizationConfig = asset.quantizationConfig;
      model.createDefaultPlacement = false;
      check(scene.registerRuntimeObject(model, nullptr, &error), "register city geometry without a placement: " + error);
      VirtualGeometryScene::RuntimeObjectDiagnostics before{};
      check(scene.getRuntimeObjectDiagnostics(model.name, before) &&
            before.sceneInstanceId == INVALID_SCENE_INSTANCE_ID &&
            scene.getInstanceCount() == 0u,
            "model registration allocates one hierarchy and no matrix instances");

      CityVoxelStaticScene city;
      std::vector<CityVoxelStaticModel> models{{0x1234u, model.name, 0u}};
      std::vector<uint64_t> appearances{0xaaaau, 0xbbbbu};
      std::vector<CityVoxelStaticBuilding> buildings{{{0, 0, 0}, 0u}};
      std::vector<CityVoxelStaticCell> cells;
      std::vector<CityVoxelStaticPlacement> placements;
      cells.reserve(250u);
      placements.reserve(250000u);
      for (uint32_t cell = 0u; cell < 250u; ++cell)
      {
        cells.push_back({{static_cast<int32_t>(cell * 100), 0, 0},
                         {static_cast<int32_t>((cell + 1u) * 100), 20, 20},
                         cell * 1000u, 1000u});
        for (uint32_t local = 0u; local < 1000u; ++local)
          placements.push_back({static_cast<uint64_t>(cell) * 1000u + local + 1u,
                                0u, local & 1u, 0u,
                                {static_cast<int32_t>(cell * 100u + local % 100u), 0,
                                 static_cast<int32_t>(local / 100u)}});
      }
      check(city.load(std::move(models), std::move(appearances), std::move(buildings),
                      std::move(cells), std::move(placements), 100000u, &error),
            "load 250,000 compact city placements: " + error);
      check(city.stats().placementBytes == 250000ull * 32u &&
            city.appearanceId(0u) == 0xaaaau && city.appearanceId(1u) == 0xbbbbu,
            "placements use 32 bytes and preserve separate appearance IDs");
      check(city.sync(scene, [](const CityVoxelStaticCell &cell) { return cell.minCell[0] < 200; },
                      2500u, &error), "activate first two spatial cells: " + error);
      VirtualGeometryScene::RuntimeObjectDiagnostics after{};
      check(scene.getRuntimeObjectDiagnostics(model.name, after) &&
            after.hierarchyStartOffset == before.hierarchyStartOffset &&
            after.pageTableOffset == before.pageTableOffset &&
            scene.getInstanceCount() == 2000u && city.stats().activePlacements == 2000u,
            "2,000 placements share one hierarchy and one page table");
      uint64_t pickedStableId = 0u;
      CityVoxelStaticPlacement gpuPlacement{};
      renderGraph->bufferRead(scene.cityCompactPlacementBuffer, 0u,
          sizeof(gpuPlacement), [&](const void *bytes) {
            std::memcpy(&gpuPlacement, bytes, sizeof(gpuPlacement));
          });
      check(scene.cityCompactPlacementBytes == 250000ull * 32u &&
            city.stableIdForDenseInstance(scene, 0u, pickedStableId) && pickedStableId == 1u &&
            gpuPlacement.stableId == pickedStableId,
            "GPU compact buffer retains 32-byte placement stride and picking ID");
      check(!city.sync(scene, [](const CityVoxelStaticCell &) { return true; },
                       2500u, &error) && scene.getInstanceCount() == 2000u,
            "over-budget visibility preserves the previous complete cell set");
      check(city.sync(scene, [](const CityVoxelStaticCell &cell) {
                        return cell.minCell[0] >= 200 && cell.minCell[0] < 400;
                      }, 2500u, &error) && scene.getInstanceCount() == 2000u,
            "spatial cell swap keeps active matrix count bounded: " + error);
      city.clear(scene);
      check(scene.getInstanceCount() == 0u &&
            scene.unregisterRuntimeObject(model.name, &error),
            "city placements and shared model release independently: " + error);
    }

    {
      os::Logger::log("phase 7: exact city appearance atlas and model-local source table");
      svr::CityVoxelAssetCompiler compiler;
      std::vector<svr::CityVoxelCell> colorA, colorB;
      for (int x = 0; x < 8; ++x)
        for (int y = 0; y < 8; ++y)
        {
          colorA.push_back({{{x,y,0}}, uint32_t(0xff000000u | uint32_t(x) | (uint32_t(y) << 8u)),
                            uint32_t(10u + (x & 1))});
          colorB.push_back({{{x+40,y,0}}, uint32_t(0xff000000u | (uint32_t(y) << 16u) | uint32_t(x)),
                            uint32_t(11u - (x & 1))});
        }
      svr::CityVoxelCompiledPlacement placementA{}, placementB{};
      check(compiler.submit(1u, 100000u, colorA, placementA, &error) &&
            compiler.submit(2u, 100000u, colorB, placementB, &error),
            "compile two exact appearances on shared occupancy: " + error);
      svr::CityVoxelVGAsset vg{};
      svr::CityVoxelVGSettings vgSettings{};
      vgSettings.maximumLevels = 1u; // Exact appearance is currently LOD0 only.
      check(svr::buildCityVoxelVirtualGeometry(compiler.geometry()[0], vgSettings, vg, &error),
            "encode one shared city VG model: " + error);
      CityVoxelStaticScene city;
      check(city.load({{compiler.geometry()[0].id, "city-colored", 0u}},
                      {compiler.appearances()[0].id, compiler.appearances()[1].id},
                      {{{0,0,0},0u}}, {{{0,0,0},{100,20,20},0u,2u}},
                      {{1u,0u,0u,0u,{0,0,0}}, {2u,0u,1u,0u,{40,0,0}}},
                      100000u, &error), "index two differently colored placements: " + error);
      check(city.installAppearanceAssets(scene, compiler.geometry(), compiler.appearances(),
                {{10u,0u},{11u,1u}}, &error),
            "upload sparse bricks and compressed exact appearances: " + error);
      auto translucent = compiler.appearances();
      const size_t alphaByte =
          translucent[0].codec == svr::CityVoxelCodec::Palette8 ||
          translucent[0].codec == svr::CityVoxelCodec::Palette16 ? 7u : 3u;
      translucent[0].bytes[alphaByte] = 127u;
      check(!city.installAppearanceAssets(scene, compiler.geometry(), translucent,
                 {{10u,0u},{11u,1u}}, &error) &&
            error.find("opaque RGBA") != std::string::npos,
            "translucent voxel color is rejected before opaque depth rendering");
      CityVoxelGPUGeometry gpuGeometry{};
      CityVoxelGPUAppearance gpuAppearanceA{}, gpuAppearanceB{};
      renderGraph->bufferRead(scene.cityGeometryTableBuffer, 0u, sizeof(gpuGeometry),
          [&](const void *bytes) { std::memcpy(&gpuGeometry, bytes, sizeof(gpuGeometry)); });
      renderGraph->bufferRead(scene.cityAppearanceTableBuffer, 0u, sizeof(gpuAppearanceA),
          [&](const void *bytes) { std::memcpy(&gpuAppearanceA, bytes, sizeof(gpuAppearanceA)); });
      renderGraph->bufferRead(scene.cityAppearanceTableBuffer, sizeof(gpuAppearanceA), sizeof(gpuAppearanceB),
          [&](const void *bytes) { std::memcpy(&gpuAppearanceB, bytes, sizeof(gpuAppearanceB)); });
      check(gpuGeometry.brickCount != 0u &&
            gpuAppearanceA.codec == static_cast<uint32_t>(compiler.appearances()[0].codec) &&
            gpuAppearanceB.codec == static_cast<uint32_t>(compiler.appearances()[1].codec) &&
            gpuAppearanceA.byteOffset != gpuAppearanceB.byteOffset,
            "GPU atlas keeps two exact appearance ranges on one brick geometry");
      auto installInfo = makeInstall("city-colored", vg.encoded, 5000u, math::Vec3f(0,0,0));
      installInfo.createDefaultPlacement = false;
      check(scene.installRuntimeEncodedObject(installInfo, nullptr, &error),
            "install shared city VG pages without default placement: " + error);
      std::vector<uint32_t> pageClusterCounts;
      for (const auto &page : vg.encoded.pages)
        pageClusterCounts.push_back(static_cast<uint32_t>(page.meshlets.size()));
      check(scene.attachCityVoxelClusterSources("city-colored", pageClusterCounts,
                                                vg.clusterSources, &error),
            "attach page-local brick source metadata: " + error);
      auto coarseSources = vg.clusterSources;
      coarseSources[0].cellScale = 2u;
      check(!scene.attachCityVoxelClusterSources("city-colored", pageClusterCounts,
                                                 coarseSources, &error) &&
            error.find("LOD0") != std::string::npos,
            "coarse city source is rejected until appearance LODs are encoded");
      check(city.sync(scene, [](const CityVoxelStaticCell &) { return true; }, 2u, &error) &&
            scene.getInstanceCount() == 2u, "activate both appearance variants: " + error);
      const auto &instances = scene.instanceManager.packedInstances;
      check(instances.size() == 2u && instances[0].hierarchyStartOffset == instances[1].hierarchyStartOffset &&
            instances[0]._padding[0] == 0u && instances[0]._padding[1] == 0u &&
            instances[1]._padding[0] == 0u && instances[1]._padding[1] == 1u &&
            instances[0]._padding2[0] == 1u && instances[1]._padding2[0] == 2u,
            "shared triangles carry distinct appearance indices");
      const auto *page = scene.getRuntimeObjectPageStatus("city-colored", 0u);
      CityVoxelGPUClusterSource source{};
      if (page && page->installed)
        renderGraph->bufferRead(scene.cityClusterSourceBuffer,
            uint64_t(page->pageAllocation.clusterOffset) * sizeof(source),
            sizeof(source), [&](const void *bytes) { std::memcpy(&source, bytes, sizeof(source)); });
      check(page && page->installed && source.cellScale != 0u,
            "resident cluster points to its city voxel brick source");
      city.clear(scene);
      check(scene.unregisterRuntimeObject("city-colored", &error),
            "city appearance model releases after placements: " + error);
    }

    {
      os::Logger::log("BLAS install gate: whole object, partial page and replacement publication");
      struct TemporaryAsset
      {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
            ("vg-blas-gate-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".virtualgeometry");
        ~TemporaryAsset() { std::error_code ignored; std::filesystem::remove(path,ignored); }
      } temporary;
      auto gate=std::make_shared<ControlledBlasGate>();
      auto gatedGraph=std::make_unique<RenderGraph>(rhi);
      VirtualGeometryScene::Settings settings; settings.pageBlasGate=gate;
      VirtualGeometryScene gated(gatedGraph.get(),hierarchyBytes,64ull*1024*1024,settings);
      auto info=makeInstall("gated",asset,1,math::Vec3f(0,0,0));
      check(!gated.installRuntimeEncodedObject(info,nullptr,&error) &&
          !gated.isRuntimeObjectRegistered("gated"),"pending object does not publish registration");
      gate->complete();
      check(gated.installRuntimeEncodedObject(info,nullptr,&error),"completed object retry installs: "+error);
      const uint32_t page=asset.rootPageIndex;
      const auto before=*gated.getRuntimeObjectPageStatus("gated",page);
      std::vector<VirtualGeometryHierarchy> hierarchyBefore, hierarchyPending;
      gated.debugReadRuntimeObjectHierarchy("gated",hierarchyBefore);
      gated.consumePendingPageDecodeRequests();
      VirtualGeometryScene::RuntimeEncodedPageInstallInfo replacement;
      replacement.localPageIndex=page; replacement.pageGeneration=1000; replacement.geometryRevision=2;
      replacement.materialRevision=1; replacement.rootPage=true;
      replacement.unitScaleBits=before.pageAllocation.unitScaleBits;
      replacement.encodedPageWords=PageBuffer::encode(asset.pages[page]).data;
      replacement.hierarchyClusterFlagUpdates=asset.pages[page].installUpdates.hierarchyUpdates;
      replacement.dependencies=asset.pages[page].dependencies;
      check(!gated.installRuntimeObjectEncodedPage("gated",replacement,&error),"replacement waits for BLAS");
      gate->complete(true);
      check(!gated.installRuntimeObjectEncodedPage("gated",replacement,&error),"one unfinished cluster blocks the whole page");
      const auto pending=*gated.getRuntimeObjectPageStatus("gated",page);
      gated.debugReadRuntimeObjectHierarchy("gated",hierarchyPending);
      check(pending.installed && pending.pageGeneration==before.pageGeneration &&
          pending.pageAllocation.bufferOffset==before.pageAllocation.bufferOffset,"old resident allocation survives pending replacement");
      check(hierarchyBefore.size()==hierarchyPending.size() &&
          std::memcmp(hierarchyBefore.data(),hierarchyPending.data(),hierarchyBefore.size()*sizeof(VirtualGeometryHierarchy))==0 &&
          gated.consumePendingPageDecodeRequests().empty(),"pending/partial BLAS writes neither hierarchy rewrites nor decode publication");
      VirtualGeometryScene::RuntimePageInstallInfo direct;
      direct.localPageIndex=page; direct.pageGeneration=2000; direct.geometryRevision=3;
      direct.pageAllocation=before.pageAllocation; direct.pageTableEntry=before.pageTableEntry;
      check(!gated.installRuntimeObjectPage("gated",direct,&error),"direct/generated publication cannot bypass the BLAS gate");
      gate->complete();
      check(gated.installRuntimeObjectEncodedPage("gated",replacement,&error),"replacement commits after every BLAS completes: "+error);
      check(gated.getRuntimeObjectPageStatus("gated",page)->pageGeneration==1000 &&
          gated.runtimePageBlasHandles("gated",page).size()==before.pageAllocation.clusterCount,"committed page exposes matching completed BLAS manifest");
      check(gated.unregisterRuntimeObject("gated",&error) && gate->released==1,"unregister retires BLAS owner");
      check(!gated.installRuntimeEncodedObject(info,nullptr,&error),"recycled page slots require a new BLAS owner");
      {
        VirtualGeometryFile writer(temporary.path.string(),true);
        check(writer.write(asset,build.pages,MESHLET_LZ4),"file-backed install fixture writes");
      }
      gated.consumePendingPageDecodeRequests();
      gated.registerObjectForStreaming("streamed",temporary.path.string());
      const InstanceId firstFilePlacement = gated.instantiateObjectInstance(
          "streamed", math::Vec3f(0,0,0), math::Quatf::identity());
      const InstanceId secondFilePlacement = gated.instantiateObjectInstance(
          "streamed", math::Vec3f(10,0,0), math::Quatf::identity());
      const auto &firstRecord = gated.objectInstances[gated.objectSparseToDense[firstFilePlacement]];
      const auto &secondRecord = gated.objectInstances[gated.objectSparseToDense[secondFilePlacement]];
      const auto *firstGpu = gated.instanceManager.getInstance(firstRecord.sceneInstanceIds[0]);
      const auto *secondGpu = gated.instanceManager.getInstance(secondRecord.sceneInstanceIds[0]);
      check(firstGpu && secondGpu &&
            firstGpu->hierarchyStartOffset == secondGpu->hierarchyStartOffset &&
            gated.files.find("streamed").value()->sharedHierarchySlices.size() == 1u,
            "file-backed placements share one hierarchy upload");
      check(gated.destroyObjectInstance(firstFilePlacement) &&
            gated.destroyObjectInstance(secondFilePlacement) &&
            gated.files.find("streamed").value()->sharedHierarchySlices.size() == 1u,
            "file-backed hierarchy remains owned by model after placements retire");
      auto *streamed=gated.files.find("streamed").value();
      auto &manager=*streamed->streamingManager;
      const auto root=streamed->pageDependencies.rootPageIds.front();
      check(!manager.isPageInstalled(root) && manager.getReadyPageCount()>0 &&
          gated.consumePendingPageDecodeRequests().empty(),"file root queues without residency/decode publication");
      bool blockedOnMemory=true;
      check(manager.installReadyPages(streamed->pageDependencies,64,&blockedOnMemory)==0 && !blockedOnMemory,
          "pending file BLAS cannot trigger page-pool eviction");
      gate->complete(true);
      check(manager.installReadyPages(streamed->pageDependencies,64)==0 && !manager.isPageInstalled(root),
          "partially completed file root stays uninstalled");
      gate->complete();
      check(manager.installReadyPages(streamed->pageDependencies,64)>0 && manager.isPageInstalled(root) &&
          !manager.pageBlasHandles(root).empty(),"file root publishes after complete BLAS manifest");

    }

#if defined(__APPLE__) && defined(ENGINE_VG_HAS_METAL_RAYTRACE)
    {
      using NativeBackend=raytracing::VirtualGeometryMetalRayTraceBackend;
      NativeBackend::Configuration config{VG_RT_METALLIB_PATH,1,1,1,64ull*1024*1024};
      config.maxBlasBuildsPerFrame=16;
      auto native=std::make_shared<NativeBackend>(config,rhi->exportMetalDevice());
      if (native->capabilities().available)
      {
        auto nativeGate=std::make_shared<raytracing::MetalPageBlasGate>(native,0);
        auto nativeGraph=std::make_unique<RenderGraph>(rhi);
        VirtualGeometryScene::Settings settings; settings.pageBlasGate=nativeGate;
        VirtualGeometryScene nativeScene(nativeGraph.get(),hierarchyBytes,64ull*1024*1024,settings);
        auto info=makeInstall("native-install",asset,1,math::Vec3f(0,0,0));
        check(!nativeScene.installRuntimeEncodedObject(info,nullptr,&error),"real encoded asset begins with pending native BLAS");
        bool installed=false;
        for (uint32_t attempt=0;attempt<2000 && !installed;++attempt)
        {
          nativeGate->pump();
          installed=nativeScene.installRuntimeEncodedObject(info,nullptr,&error);
          if (!installed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(installed,"real compressor output completes native BLAS before scene install: "+error);
        check(native->cacheStatistics().builds>0 && native->cacheStatistics().pendingBuilds==0 &&
            !nativeScene.runtimePageBlasHandles("native-install",asset.rootPageIndex).empty(),
            "real asset publishes completed native cluster manifest");
      }
    }
#endif

    os::Logger::log("stage 1: unregister returns every slot it took");
    uint64_t pageGeneration = 1u;
    const auto install = [&](const std::string &name, const float x)
    {
      const auto info =
          makeInstall(name, asset, pageGeneration, math::Vec3f(x, 0.0f, 0.0f));
      pageGeneration += assetPageCount + 1u;
      VirtualGeometryScene::RuntimeEncodedObjectInstallStats stats{};
      std::string installError;
      const bool ok = scene.installRuntimeEncodedObject(info, &stats, &installError);
      if (!ok)
        os::Logger::errorf("  install %s failed: %s", name.c_str(), installError.c_str());
      return ok ? stats.installedPageCount : 0u;
    };

    check(install("object-a", 0.0f) == assetPageCount, "the first object installs every page");
    check(install("object-b", 64.0f) == assetPageCount, "a second object installs beside it");
    const uint32_t highWaterAfterTwo = scene.nextPageTableSlot;

    VirtualGeometryScene::RuntimeObjectDiagnostics diagnosticsB{};
    check(
        scene.getRuntimeObjectDiagnostics("object-b", diagnosticsB),
        "the second object reports diagnostics");
    const uint32_t pageTableOffsetB = diagnosticsB.pageTableOffset;

    check(scene.unregisterRuntimeObject("object-b", &error), "the second object unregisters: " + error);
    check(!scene.isRuntimeObjectRegistered("object-b"), "it is no longer registered");
    check(scene.isRuntimeObjectRegistered("object-a"), "its neighbour is untouched");

    check(install("object-c", 64.0f) == assetPageCount, "a replacement installs into the freed slots");
    VirtualGeometryScene::RuntimeObjectDiagnostics diagnosticsC{};
    check(
        scene.getRuntimeObjectDiagnostics("object-c", diagnosticsC),
        "the replacement reports diagnostics");
    check(
        diagnosticsC.pageTableOffset == pageTableOffsetB,
        "the replacement reuses the retired page-table range");
    check(
        scene.nextPageTableSlot == highWaterAfterTwo,
        "the page table high-water mark did not advance");

    os::Logger::log("stage 2: replacing forever exhausts nothing");
    // Far more cycles than the hierarchy buffer has room for objects. Every one
    // of them has to succeed, and the page table must not creep.
    uint32_t replaceFailures = 0u;
    uint32_t installFailures = 0u;
    for (uint32_t cycle = 0u; cycle < 200u; ++cycle)
    {
      if (!scene.unregisterRuntimeObject("object-c", &error))
      {
        ++replaceFailures;
        break;
      }
      if (install("object-c", 64.0f) != assetPageCount)
      {
        ++installFailures;
        break;
      }
    }
    check(replaceFailures == 0u, "every unregister in the replace loop succeeded");
    check(installFailures == 0u, "every reinstall in the replace loop succeeded");
    check(
        scene.nextPageTableSlot == highWaterAfterTwo,
        "200 replacements left the page-table high-water mark where it was");

    os::Logger::log("stage 3: the page buffer evicts least recently used first");
    // A separate scene with a page buffer barely larger than two objects, so
    // the third install has to make room.
    uint64_t encodedBytes = 0u;
    for (const auto &page : asset.pages)
      encodedBytes += PageBuffer::encode(page).data.size() * sizeof(uint32_t);
    const uint64_t tightPagesBytes = encodedBytes * 5u / 2u;
    auto tightGraphStorage = std::make_unique<RenderGraph>(rhi);
    VirtualGeometryScene tight(
        tightGraphStorage.get(),
        std::max<uint32_t>(assetHierarchyNodes * sizeof(VirtualGeometryHierarchy) * 16u, 4096u),
        tightPagesBytes);

    uint64_t tightGeneration = 1u;
    const auto tightInstall = [&](const std::string &name, const uint64_t frame)
    {
      tight.setRuntimeStreamingFrame(frame);
      const auto info = makeInstall(name, asset, tightGeneration, math::Vec3f(0.0f, 0.0f, 0.0f));
      tightGeneration += assetPageCount + 1u;
      VirtualGeometryScene::RuntimeEncodedObjectInstallStats stats{};
      std::string installError;
      const bool ok = tight.installRuntimeEncodedObject(info, &stats, &installError);
      if (!ok)
        os::Logger::errorf("  tight install %s failed: %s", name.c_str(), installError.c_str());
      return ok;
    };

    check(tightInstall("old", 10u), "the older object installs");
    check(tightInstall("recent", 20u), "the newer object installs");

    const auto installedPages = [&](const char *name)
    {
      VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
      if (!tight.getRuntimeObjectDiagnostics(name, diagnostics))
        return 0u;
      uint32_t installed = 0u;
      for (uint32_t page = 0u; page < diagnostics.pageCount; ++page)
      {
        const auto *status = tight.getRuntimeObjectPageStatus(name, page);
        installed += status != nullptr && status->installed ? 1u : 0u;
      }
      return installed;
    };
    const uint32_t oldBefore = installedPages("old");
    const uint32_t recentBefore = installedPages("recent");

    // This one does not fit without eviction.
    check(tightInstall("newest", 30u), "a third object installs by making room");
    const uint32_t oldAfter = installedPages("old");
    const uint32_t recentAfter = installedPages("recent");
    os::Logger::logf(
        "  installed pages old=%u->%u recent=%u->%u",
        oldBefore, oldAfter, recentBefore, recentAfter);
    check(
        oldAfter < oldBefore,
        "the least recently used object gave up pages");
    check(
        oldBefore - oldAfter >= recentBefore - recentAfter,
        "it gave up at least as many as the more recently used one");

    // Root pages anchor the hierarchy: evicting one would leave the object with
    // nothing drawable at any distance.
    uint32_t evictedRoots = 0u;
    for (const char *name : {"old", "recent", "newest"})
    {
      VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
      if (!tight.getRuntimeObjectDiagnostics(name, diagnostics))
        continue;
      const auto *status =
          tight.getRuntimeObjectPageStatus(name, asset.rootPageIndex);
      if (status != nullptr && status->rootPage && !status->installed)
        ++evictedRoots;
    }
    check(evictedRoots == 0u, "no root page was evicted");
  }
  catch (const std::exception &failure)
  {
    os::Logger::errorf("exception: %s", failure.what());
    exitCode = 1;
  }

  delete rhi;

  if (failures != 0)
  {
    os::Logger::errorf("%d runtime object lifecycle checks failed", failures);
    exitCode = 1;
  }
  else if (exitCode == 0)
    os::Logger::log("Virtual geometry runtime object lifecycle tests passed");
  os::Logger::shutdown();
  return exitCode;
}
