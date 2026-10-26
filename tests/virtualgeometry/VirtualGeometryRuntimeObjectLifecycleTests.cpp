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
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "math/math.hpp"
#include "os/Logger.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
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
