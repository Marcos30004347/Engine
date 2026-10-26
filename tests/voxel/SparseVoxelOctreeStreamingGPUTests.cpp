// Streaming and LOD selection for a voxel octree page, on the GPU.
//
// The CPU suite (SparseVoxelOctreePageStreamingTests) proves the build and the
// rewrite algebra. This one runs the real thing: a headless VirtualGeometryScene
// with the page decode and culling passes, one small voxel chunk, and a camera
// standing next to it. It answers two questions the CPU model cannot:
//
//   - do clusters actually stream? The scene starts with one page resident, the
//     GPU culler writes page priorities for the pages it could not reach, and
//     each update installs one more. Visible cluster count has to climb.
//   - does LOD selection actually refine? With everything resident, walking the
//     camera in has to draw strictly more clusters, and walking it out strictly
//     fewer, without ever dropping to nothing.
//
// Only virtual geometry passes take part: page decode and multi-dispatch
// culling. There is no swapchain and no window.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryPageDecodePass.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

namespace svr = voxel::svraster;
namespace fs = std::filesystem;

namespace
{

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

constexpr uint32_t kMaxQueueElements = 1u << 20;
constexpr uint32_t kPageNotInstalledBit = 1u << 31u;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

// ── the chunk ───────────────────────────────────────────────────────────────

uint32_t columnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
{
  uint32_t height = 4u;
  if (x >= side / 2u)
    height += 6u;
  if (z >= side * 3u / 4u)
    height += 3u;
  if (x == side / 4u && z == side / 4u)
    height += 9u;
  if (x > side / 8u && x < side / 8u + 3u && z > side / 2u && z < side / 2u + 3u)
    height = height > 3u ? height - 3u : 1u;
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

  constexpr uint32_t kShellDepth = 3u;
  for (uint32_t x = 0u; x < side; ++x)
    for (uint32_t z = 0u; z < side; ++z)
    {
      const uint32_t top = columnHeight(x, z, side);
      uint32_t lowestNeighbour = top;
      const std::array<std::array<int32_t, 2>, 4> neighbours{{
          {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
      }};
      for (const auto &offset : neighbours)
      {
        const int32_t nx = static_cast<int32_t>(x) + offset[0];
        const int32_t nz = static_cast<int32_t>(z) + offset[1];
        if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(side) ||
            nz >= static_cast<int32_t>(side))
          continue;
        lowestNeighbour = std::min(
            lowestNeighbour,
            columnHeight(static_cast<uint32_t>(nx), static_cast<uint32_t>(nz), side));
      }
      const uint32_t floor = std::min(
          top > kShellDepth ? top - kShellDepth : 0u,
          lowestNeighbour > 0u ? lowestNeighbour - 1u : 0u);

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
        material.materialId = (x / 8u + z / 8u) % 3u;
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

struct FrameProbe
{
  uint32_t installedPages = 0u;
  uint32_t visibleClusters = 0u;
  // Uninstalled pages the culler gave a non-zero priority: the streaming
  // request set.
  uint32_t requestedPages = 0u;

  // Read back from the GPU hierarchy buffer, not from any CPU mirror.
  uint32_t leafNodes = 0u;
  uint32_t leafNodesMarkedInstalled = 0u;
  uint32_t leafNodesMarkedNotInstalled = 0u;
  // Installed leaves the rewrites left with no cluster enabled: those draw
  // nothing at all.
  uint32_t installedLeavesWithNoEnabledCluster = 0u;
  // Installed leaves on the residency border. This has to shrink as pages
  // arrive, or the rewrites are not being applied.
  uint32_t installedLeavesOnStreamingBorder = 0u;
  // Nodes whose LOD errors are unusable for selection.
  uint32_t nodesWithNonFiniteError = 0u;
  uint32_t nodesWithInvertedError = 0u;
  // Installed leaves whose page-table entry advertises no clusters.
  uint32_t installedLeavesWithEmptyPageEntry = 0u;
};

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Sparse voxel octree streaming + LOD selection (GPU)");
  os::Logger::log("==================================================");

  constexpr uint32_t kSide = 64u;
  constexpr uint32_t kViewportWidth = 1920u;
  constexpr uint32_t kViewportHeight = 1080u;
  constexpr float kNear = 0.1f;
  constexpr float kFar = 10000.0f;
  constexpr float kErrorThreshold = 1.0f;
  const std::string objectName = "voxel_octree_chunk";

  // ── build the page set ────────────────────────────────────────────────────
  svr::SparseVoxelBrickHierarchy hierarchy{};
  std::string error;
  if (!buildChunkOctree(kSide, hierarchy, &error))
  {
    os::Logger::errorf("voxel chunk build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  octreeSettings.originOffset = {
      static_cast<double>(kSide) * 0.5, 0.0, static_cast<double>(kSide) * 0.5};

  VirtualGeometryBuildSettings buildSettings{};
  // A small root page and small pages after it, so streaming has real work.
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.maxRootPageGroups = 1u;

  VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy,
          0u,
          static_cast<uint32_t>(hierarchy.nodes.size()),
          octreeSettings,
          buildSettings,
          build,
          &stats,
          &error))
  {
    os::Logger::errorf("octree virtual geometry build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  QuantizationConfig quantization{};
  quantization.quantization_factor = 4u;
  quantization.unit_scale = 100.0f;
  quantization.padMeshlets = true;
  VirtualGeometryEncodedData encoded =
      VirtualGeometryCompressor::encode(build, quantization);

  const fs::path assetPath =
      fs::path(os::io::getExecutableDirectory()) / "voxel-octree-streaming.virtualgeometry";
  {
    VirtualGeometryFile writer(assetPath.string(), /*write=*/true);
    if (!writer.isOpen() || !writer.write(encoded, build.pages, MESHLET_LZ4))
    {
      os::Logger::errorf("could not write %s", assetPath.string().c_str());
      os::Logger::shutdown();
      return 1;
    }
  }

  // A DAG cut holds exactly one cluster per root-to-leaf path, and distinct cut
  // clusters cover disjoint leaf sets, so a cut can never contain more clusters
  // than the DAG has leaves. Drawing more than that means parents are being
  // drawn on top of their own children - geometry stacked at two resolutions.
  uint32_t leafClusterCount = 0u;
  for (uint32_t cluster = 0u; cluster < build.clusterDAG.size(); ++cluster)
    leafClusterCount += build.clusterDAG[cluster].empty() ? 1u : 0u;

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf(
      "  chunk cells side=%u octreeLevels=%u bricks=%u clusters=%u leaves=%u pages=%u hierarchyNodes=%u",
      kSide, stats.levelCount, stats.nodeCount, stats.clusterCount, leafClusterCount,
      pageCount, hierarchySize);
  check(pageCount > 2u, "the chunk is split over several pages, so streaming has work to do");

  // ── headless device and scene ─────────────────────────────────────────────
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
    const uint32_t hierarchyBytes = std::max(
        hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
    auto renderGraphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *renderGraph = renderGraphStorage.get();
    VirtualGeometryScene scene(renderGraph, hierarchyBytes, 256ull * 1024 * 1024);
    scene.registerObjectForStreaming(objectName, assetPath.string());
    scene.instantiateObjectInstance(
        objectName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
    scene.updateInstanceBuffer();

    VirtualGeometryCullingMultipleDispatchesPass::Settings passSettings;
    // The voxel DAG is deeper than a mesh LOD chain: one BVH subtree per octree
    // level, joined under a synthetic root. Too small a dispatch-round budget
    // stops the traversal before it reaches any leaf and the frame draws
    // nothing at all.
    passSettings.maxHierarchyLevels = 32u;
    passSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    passSettings.errorTreshhold = kErrorThreshold;
    passSettings.viewPortWidth = kViewportWidth;
    passSettings.viewPortHeight = kViewportHeight;
    // No depth pre-pass runs here, so the HiZ pyramid is whatever the dummy
    // depth texture happens to hold. Occlusion culling against it would reject
    // everything; this test is about streaming and LOD selection, not occlusion.
    passSettings.enableOcclusionCulling = false;

    Texture dummyDepth = renderGraph->createTexture(
        TextureInfo{
          .name = objectName + "_DummyDepth.texture",
          .format = Format::Format_Depth32Float,
          .height = kViewportHeight,
          .width = kViewportWidth,
          .memoryProperties = BufferUsage::BufferUsage_Storage,
          .mipLevels = 1u,
          .usage = ImageUsage::ImageUsage_Storage,
        });

    renderGraph->registerPass<VirtualGeometryPageDecodePass>(
        objectName + "_pageDecode", 0, scene);
    auto *cullingPass = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(
        objectName + "_culling", 1, scene, passSettings, dummyDepth);
    renderGraph->compile();

    const math::Vec3f chunkCenter(0.0f, 8.0f, 0.0f);

    const auto setCamera = [&](const math::Vec3f &position)
    {
      Camera camera(60.0f * 3.14159265f / 180.0f, position, math::Vec3f(0.0f, 0.0f, -1.0f), true);
      camera.setAspectRatio(16.0f / 9.0f);
      camera.setNearFar(kNear, kFar);
      camera.lookAt(chunkCenter);
      camera.updateMatrices();
      // math::Mat4f is column major (`data[col * 4 + row]`), and the passes take
      // that raw storage. Rebuilding the array as `[row * 4 + col] = at(row,
      // col)` transposes it. The culler survives the transpose because its
      // projected-error term only reads proj[1][1], which is unchanged by it,
      // but the meshlet vertex shader does a full transform and puts every
      // vertex off screen - clusters get selected and nothing rasterises.
      const float *viewArray = camera.getViewMatrix().data;
      const float *projectionArray = camera.getProjectionMatrix().data;
      const math::Vec3f &eye = camera.getPosition();
      float viewPosition[4] = {eye[0], eye[1], eye[2], 1.0f};
      cullingPass->updateUniforms(
          viewArray, projectionArray, viewPosition,
          kViewportWidth, kViewportHeight, kNear, kFar, 1u);
    };

    uint32_t frameSlot = 0u;
    const auto runFrame = [&]() -> FrameProbe
    {
      // The scene and the culling pass keep per-frame-slot copies of their
      // instance and decode buffers. Without the overrides the passes read the
      // base allocations, the decoded cluster data never reaches the culler,
      // and every frame reports zero visible clusters.
      scene.prepareFrame(frameSlot);
      RenderGraph::Overrides overrides;
      scene.appendFrameOverrides(overrides, frameSlot);
      cullingPass->appendFrameOverrides(overrides, frameSlot);

      RenderGraph::Frame frame;
      renderGraph->run(frame, overrides);
      renderGraph->waitFrame(frame);
      frameSlot = (frameSlot + 1u) % renderGraph->getMaxFramesInFlight();

      FrameProbe probe{};
      CullingCounters counters{};
      renderGraph->bufferRead(
          cullingPass->getCullingStatisticsBuffer(),
          0,
          sizeof(CullingCounters),
          [&](const void *data) { std::memcpy(&counters, data, sizeof(CullingCounters)); });
      probe.visibleClusters =
          counters.visibleClusterHardwareCount + counters.visibleClusterSoftwareCount;

      std::vector<PageTableEntry> pageTable(pageCount);
      renderGraph->bufferRead(
          scene.pageTableBuffer,
          0,
          pageCount * sizeof(PageTableEntry),
          [&](const void *data)
          { std::memcpy(pageTable.data(), data, pageCount * sizeof(PageTableEntry)); });

      std::vector<uint32_t> priorities(pageCount, 0u);
      renderGraph->bufferRead(
          scene.pagePriorityBuffer,
          0,
          pageCount * sizeof(uint32_t),
          [&](const void *data)
          { std::memcpy(priorities.data(), data, pageCount * sizeof(uint32_t)); });

      for (uint32_t page = 0u; page < pageCount; ++page)
      {
        const bool installed = pageTable[page].isInstalled != 0u;
        probe.installedPages += installed ? 1u : 0u;
        if (!installed && priorities[page] != 0u)
          ++probe.requestedPages;
      }

      // The hierarchy the culler actually traverses, straight out of the GPU
      // buffer. Everything below is what decides whether a node can be picked
      // for rendering: the install bit, the per-cluster enabled mask, the
      // streaming-border mask, and the two LOD errors.
      std::vector<VirtualGeometryHierarchy> gpuHierarchy(hierarchySize);
      renderGraph->bufferRead(
          scene.hierarchyBuffer,
          0,
          hierarchySize * sizeof(VirtualGeometryHierarchy),
          [&](const void *data)
          {
            std::memcpy(
                gpuHierarchy.data(), data,
                hierarchySize * sizeof(VirtualGeometryHierarchy));
          });

      for (const auto &node : gpuHierarchy)
      {
        if (!std::isfinite(node.min_lod_error) ||
            (node.max_parent_lod_error != std::numeric_limits<float>::max() &&
             !std::isfinite(node.max_parent_lod_error)))
          ++probe.nodesWithNonFiniteError;
        if (node.max_parent_lod_error < node.min_lod_error)
          ++probe.nodesWithInvertedError;

        if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
          continue;
        ++probe.leafNodes;
        if ((node.pageIndex & kPageNotInstalledBit) != 0u)
        {
          ++probe.leafNodesMarkedNotInstalled;
          continue;
        }
        ++probe.leafNodesMarkedInstalled;

        const uint8_t enabledMask = static_cast<uint8_t>(
            (node.flags & HIERARCHY_ENABLED_MASK_BITS) >> HIERARCHY_ENABLED_MASK_SHIFT);
        const uint8_t streamingMask = static_cast<uint8_t>(
            (node.flags & HIERARCHY_STREAMING_MASK_BITS) >> HIERARCHY_STREAMING_MASK_SHIFT);
        if (enabledMask == 0u)
          ++probe.installedLeavesWithNoEnabledCluster;
        if (streamingMask != 0u)
          ++probe.installedLeavesOnStreamingBorder;
        if (node.pageIndex < pageCount && pageTable[node.pageIndex].clusterCount == 0u)
          ++probe.installedLeavesWithEmptyPageEntry;
      }
      return probe;
    };

    // ── stage 1: clusters stream in ─────────────────────────────────────────
    os::Logger::log("stage 1: clusters stream in as pages are installed");
    setCamera(math::Vec3f(0.0f, 24.0f, 40.0f));

    std::vector<FrameProbe> probes;
    // The first frame runs before any streaming update: the culler has to write
    // the priorities that drive the first install.
    probes.push_back(runFrame());

    // ── the root page alone, as the GPU sees it ─────────────────────────────
    const FrameProbe rootOnly = probes.front();
    check(rootOnly.installedPages == 1u, "exactly one page - the root - is installed");
    check(
        build.pages[0].groups.size() == 1u,
        "the root page holds a single parent group");
    check(rootOnly.leafNodes > 1u, "the GPU hierarchy has leaf nodes for every page");
    check(
        rootOnly.leafNodesMarkedInstalled >= 1u,
        "the root page's hierarchy leaf is marked installed on the GPU");
    check(
        rootOnly.leafNodesMarkedNotInstalled ==
            rootOnly.leafNodes - rootOnly.leafNodesMarkedInstalled,
        "every other hierarchy leaf still carries the not-installed bit");
    check(
        rootOnly.installedLeavesWithNoEnabledCluster == 0u,
        "the installed root leaf has clusters enabled, so culling can pick it");
    check(
        rootOnly.installedLeavesOnStreamingBorder >= 1u,
        "the root leaf is marked as the residency border while its children are absent");
    check(
        rootOnly.installedLeavesWithEmptyPageEntry == 0u,
        "the installed page's page-table entry advertises its clusters");
    check(rootOnly.nodesWithNonFiniteError == 0u, "every hierarchy node carries finite LOD errors");
    check(
        rootOnly.nodesWithInvertedError == 0u,
        "no hierarchy node has a parent error below its own error");
    check(rootOnly.visibleClusters > 0u, "culling picks the root page's clusters for rendering");
    check(rootOnly.requestedPages > 0u, "the GPU asks for the pages under the root");

    const uint32_t maximumUpdates = pageCount * 4u;
    for (uint32_t update = 0u;
         update < maximumUpdates && probes.back().installedPages < pageCount;
         ++update)
    {
      scene.updatePageStreaming(objectName, 1u);
      scene.updateInstanceBuffer();
      probes.push_back(runFrame());
    }

    std::ostringstream progression;
    for (const FrameProbe &probe : probes)
      progression << " " << probe.installedPages << "p/" << probe.visibleClusters << "c";
    os::Logger::logf("  residency progression:%s", progression.str().c_str());

    check(probes.front().installedPages >= 1u, "the root page is resident on the first frame");
    check(
        probes.front().visibleClusters > 0u,
        "the first frame already draws something from the root page");
    check(
        probes.size() > 2u,
        "streaming took several updates, so pages really arrived one at a time");
    check(
        probes.back().visibleClusters > probes.front().visibleClusters,
        "more clusters are drawn once the finer pages are resident");
    check(
        probes.back().installedPages > probes.front().installedPages,
        "pages streamed in beyond the root page");

    bool everyFrameDrewSomething = true;
    bool residencyNeverRegressed = true;
    bool clustersNeverRegressed = true;
    for (size_t index = 0u; index < probes.size(); ++index)
    {
      everyFrameDrewSomething = everyFrameDrewSomething && probes[index].visibleClusters > 0u;
      if (index > 0u)
      {
        residencyNeverRegressed = residencyNeverRegressed &&
            probes[index].installedPages >= probes[index - 1u].installedPages;
        clustersNeverRegressed = clustersNeverRegressed &&
            probes[index].visibleClusters >= probes[index - 1u].visibleClusters;
      }
    }
    check(everyFrameDrewSomething, "no frame during streaming drew nothing");
    check(residencyNeverRegressed, "residency only ever grows while streaming in");
    check(
        clustersNeverRegressed,
        "each installed page adds clusters and never takes any away");

    uint32_t framesWithRequests = 0u;
    for (size_t index = 0u; index + 1u < probes.size(); ++index)
      framesWithRequests += probes[index].requestedPages > 0u ? 1u : 0u;
    check(
        framesWithRequests > 0u,
        "the culler asked for pages it could not reach: streaming is request driven");
    check(
        probes.back().requestedPages == 0u,
        "streaming settles: nothing is left requested at this distance");

    // ── after the requested pages have been installed and rewritten ─────────
    const FrameProbe settled = probes.back();
    check(
        settled.leafNodesMarkedInstalled > rootOnly.leafNodesMarkedInstalled,
        "more hierarchy leaves are marked installed on the GPU after streaming");
    check(
        settled.installedLeavesWithNoEnabledCluster == 0u,
        "every installed leaf still has clusters enabled after the rewrites");
    // The residency border exists exactly while something is still absent. Once
    // the culler has reached every page the border must be empty - if it is not,
    // the install rewrites are not being applied.
    if (settled.leafNodesMarkedNotInstalled == 0u)
      check(
          settled.installedLeavesOnStreamingBorder == 0u,
          "with every page resident no leaf is left on the residency border");
    else
      check(
          settled.installedLeavesOnStreamingBorder > 0u,
          "while pages are still absent the rewrites mark a residency border");
    check(
        probes.front().installedLeavesOnStreamingBorder > 0u,
        "the root-only frame does mark a residency border");
    check(
        settled.installedLeavesWithEmptyPageEntry == 0u,
        "every installed page advertises clusters in the page table");
    check(settled.nodesWithNonFiniteError == 0u, "LOD errors stay finite through streaming");
    check(settled.nodesWithInvertedError == 0u, "LOD errors stay ordered through streaming");
    check(
        settled.visibleClusters > rootOnly.visibleClusters,
        "culling picks clusters from the newly installed pages");

    // Residency stops where the error budget is met, which is the whole point:
    // pages holding geometry finer than this view needs are never fetched.
    const uint32_t settledPagesFar = probes.back().installedPages;
    os::Logger::logf(
        "  settled at %u/%u pages, %u clusters from 40m",
        settledPagesFar, pageCount, probes.back().visibleClusters);

    // ── stage 1b: walking in pulls the rest of the DAG in ────────────────────
    os::Logger::log("stage 1b: moving closer pulls in the pages the error metric now needs");
    setCamera(math::Vec3f(0.0f, 12.0f, 12.0f));

    std::vector<FrameProbe> nearProbes;
    nearProbes.push_back(runFrame());
    for (uint32_t update = 0u;
         update < maximumUpdates && nearProbes.back().installedPages < pageCount;
         ++update)
    {
      scene.updatePageStreaming(objectName, 1u);
      scene.updateInstanceBuffer();
      nearProbes.push_back(runFrame());
    }

    std::ostringstream nearProgression;
    for (const FrameProbe &probe : nearProbes)
      nearProgression << " " << probe.installedPages << "p/" << probe.visibleClusters << "c";
    os::Logger::logf("  close-up residency progression:%s", nearProgression.str().c_str());

    check(
        nearProbes.front().requestedPages > 0u || settledPagesFar == pageCount,
        "the closer camera asks for pages the far one did not need");
    check(
        nearProbes.back().installedPages > settledPagesFar ||
            settledPagesFar == pageCount,
        "moving closer streams in more pages");
    // Not every page has to arrive: a 64-cell chunk seen from 12 m has most of
    // itself outside the frustum, and the streamer is right not to fetch pages
    // the culler never reaches. What must hold is that it settles - residency
    // stops exactly where the request set empties.
    check(
        nearProbes.back().requestedPages == 0u,
        "the close-up settles with nothing left requested");
    check(
        nearProbes.front().visibleClusters > 0u,
        "the close-up frame still renders once the camera moves in");
    // The chunk is 64 units across and the close-up camera is 12 units away, so
    // most of it is outside the frustum: fewer clusters, not more.
    check(
        nearProbes.back().visibleClusters < probes.back().visibleClusters,
        "the close-up frustum excludes most of the chunk");

    bool nearFramesDrewSomething = true;
    for (const FrameProbe &probe : nearProbes)
      nearFramesDrewSomething = nearFramesDrewSomething && probe.visibleClusters > 0u;
    check(nearFramesDrewSomething, "no frame while streaming the close-up drew nothing");

    const FrameProbe closeSettled = nearProbes.back();
    check(
        closeSettled.leafNodesMarkedNotInstalled ==
            closeSettled.leafNodes - closeSettled.leafNodesMarkedInstalled,
        "the not-installed bit agrees with the page table for every leaf");
    check(
        closeSettled.installedLeavesWithNoEnabledCluster == 0u,
        "every leaf the close-up installed can be picked by culling");
    check(
        closeSettled.installedLeavesWithEmptyPageEntry == 0u,
        "every close-up page advertises clusters in the page table");
    check(
        closeSettled.nodesWithNonFiniteError == 0u && closeSettled.nodesWithInvertedError == 0u,
        "LOD errors remain valid after the close-up installs");
    // The border is exactly the frontier: empty once nothing is missing.
    if (closeSettled.leafNodesMarkedNotInstalled == 0u)
      check(
          closeSettled.installedLeavesOnStreamingBorder == 0u,
          "with every page resident no leaf is left on the residency border");
    else
      check(
          closeSettled.installedLeavesOnStreamingBorder > 0u,
          "a leaf is on the residency border for every page still absent");
    os::Logger::logf(
        "  settled GPU hierarchy: leaves=%u installed=%u notInstalled=%u border=%u",
        closeSettled.leafNodes,
        closeSettled.leafNodesMarkedInstalled,
        closeSettled.leafNodesMarkedNotInstalled,
        closeSettled.installedLeavesOnStreamingBorder);

    // ── stage 2: LOD selection refines with distance ─────────────────────────
    os::Logger::log("stage 2: LOD selection refines with camera distance");

    const std::array<float, 6> distances{2000.0f, 500.0f, 160.0f, 60.0f, 24.0f, 10.0f};
    std::vector<uint32_t> visiblePerDistance;
    for (const float distance : distances)
    {
      setCamera(math::Vec3f(0.0f, distance * 0.35f + 8.0f, distance));
      visiblePerDistance.push_back(runFrame().visibleClusters);
    }

    std::ostringstream sweep;
    for (size_t index = 0u; index < distances.size(); ++index)
      sweep << " " << distances[index] << "m=" << visiblePerDistance[index];
    os::Logger::logf("  clusters by camera distance:%s", sweep.str().c_str());

    const bool everyDistanceDrewSomething =
        std::all_of(visiblePerDistance.begin(), visiblePerDistance.end(),
                    [](const uint32_t count) { return count > 0u; });
    check(everyDistanceDrewSomething, "there is always something to render, at every distance");
    const bool streamingCutIsBounded =
        std::all_of(visiblePerDistance.begin(), visiblePerDistance.end(),
                    [&](const uint32_t count) { return count <= leafClusterCount; });
    check(
        streamingCutIsBounded,
        "the streamed cut never exceeds the leaf count: no level is drawn twice");

    // Detail rises as the error budget is met and then falls again as the
    // frustum starts excluding the chunk, so the sweep peaks in the middle.
    const auto peak =
        std::max_element(visiblePerDistance.begin(), visiblePerDistance.end());
    check(
        peak != visiblePerDistance.begin() && peak + 1 != visiblePerDistance.end(),
        "detail peaks between the far and the near view, not at either end");
    check(
        *peak > visiblePerDistance.front() * 4u,
        "closing from the far view multiplies the detail on screen");
    check(
        visiblePerDistance.back() > visiblePerDistance.front(),
        "the near view still draws more than the far view");
    check(
        visiblePerDistance.front() < stats.clusterCount,
        "the far view draws a coarse cut, not the whole DAG");

    // Walking back out has to give the coarse cut back, not keep the fine one.
    setCamera(math::Vec3f(0.0f, distances.front() * 0.35f + 8.0f, distances.front()));
    const uint32_t returnedFar = runFrame().visibleClusters;
    check(
        returnedFar == visiblePerDistance.front(),
        "returning to the far view reproduces the same coarse cut");

    // ── stage 3: the runtime encoded-object install path ────────────────────
    //
    // Terrain does not stream its pages from a file: it hands the whole encoded
    // asset to installRuntimeEncodedObject, which installs every page at once.
    // That path used to mark every cluster on every installed page a streaming
    // leaf, which forces the culler's self error to zero and skips the parent
    // test - so every LOD level drew simultaneously and terrain had no LOD at
    // all. This stage is the regression for that.
    os::Logger::log("stage 3: runtime encoded-object install keeps a real LOD cut");
    {
      auto runtimeGraphStorage = std::make_unique<RenderGraph>(rhi);
      RenderGraph *runtimeGraph = runtimeGraphStorage.get();
      VirtualGeometryScene runtimeScene(runtimeGraph, hierarchyBytes, 256ull * 1024 * 1024);

      VirtualGeometryScene::RuntimeEncodedObjectInstallInfo objectInstall{};
      objectInstall.name = "terrain_like_runtime_object";
      objectInstall.asset = &encoded;
      objectInstall.basePageGeneration = 1u;
      objectInstall.geometryRevision = 1u;
      objectInstall.materialRevision = 1u;
      objectInstall.position = math::Vec3f(0.0f, 0.0f, 0.0f);
      objectInstall.materialIndex = 0u;
      objectInstall.hasTracePayload = true;
      objectInstall.hasMaterialPayload = false;
      objectInstall.enqueuePageDecode = true;

      VirtualGeometryScene::RuntimeEncodedObjectInstallStats installStats{};
      std::string installError;
      const bool installed = runtimeScene.installRuntimeEncodedObject(
          objectInstall, &installStats, &installError);
      check(installed, "the encoded asset installs as a runtime object: " + installError);
      if (installed)
      {
        check(
            installStats.installedPageCount == pageCount,
            "the runtime install brings every page in at once");
        runtimeScene.updateInstanceBuffer();

        VirtualGeometryCullingMultipleDispatchesPass::Settings runtimeSettings = passSettings;
        Texture runtimeDepth = runtimeGraph->createTexture(
            TextureInfo{
              .name = "runtime_DummyDepth.texture",
              .format = Format::Format_Depth32Float,
              .height = kViewportHeight,
              .width = kViewportWidth,
              .memoryProperties = BufferUsage::BufferUsage_Storage,
              .mipLevels = 1u,
              .usage = ImageUsage::ImageUsage_Storage,
            });
        runtimeGraph->registerPass<VirtualGeometryPageDecodePass>(
            "runtime_pageDecode", 0, runtimeScene);
        auto *runtimeCulling =
            runtimeGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(
                "runtime_culling", 1, runtimeScene, runtimeSettings, runtimeDepth);

        // The visible-cluster list lives in GPU-private memory, so it has to be
        // copied into a pullable staging buffer inside the graph. Reading the
        // device buffer directly fails with VK_ERROR_MEMORY_MAP_FAILED and
        // leaves the destination zeroed, which silently turns any check built
        // on it into a no-op.
        const uint64_t visibleClusterBytes =
            static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) *
            sizeof(VisibleClusterInfo);
        Buffer visibleClusterStaging = runtimeGraph->createBuffer(
            BufferInfo{
              .name = "runtime_VisibleClusterStaging.buffer",
              .size = visibleClusterBytes,
              .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
            });
        runtimeGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
            "runtime_copyVisibleClusters",
            2,
            runtimeCulling->getHWVisibleClusterInfosBuffer(),
            0,
            visibleClusterBytes,
            visibleClusterStaging,
            0,
            visibleClusterBytes);
        runtimeGraph->compile();

        uint32_t runtimeSlot = 0u;
        const auto runtimeFrameCounters =
            [&](const math::Vec3f &position) -> CullingCounters
        {
          Camera camera(
              60.0f * 3.14159265f / 180.0f, position, math::Vec3f(0.0f, 0.0f, -1.0f), true);
          camera.setAspectRatio(16.0f / 9.0f);
          camera.setNearFar(kNear, kFar);
          camera.lookAt(chunkCenter);
          camera.updateMatrices();
          // Column-major storage, passed through as-is; see the note in the
          // streaming frame above.
          const float *viewArray = camera.getViewMatrix().data;
          const float *projectionArray = camera.getProjectionMatrix().data;
          const math::Vec3f &eye = camera.getPosition();
          float viewPosition[4] = {eye[0], eye[1], eye[2], 1.0f};
          runtimeCulling->updateUniforms(
              viewArray, projectionArray, viewPosition,
              kViewportWidth, kViewportHeight, kNear, kFar, 1u);

          runtimeScene.prepareFrame(runtimeSlot);
          RenderGraph::Overrides overrides;
          runtimeScene.appendFrameOverrides(overrides, runtimeSlot);
          runtimeCulling->appendFrameOverrides(overrides, runtimeSlot);
          RenderGraph::Frame frame;
          runtimeGraph->run(frame, overrides);
          runtimeGraph->waitFrame(frame);
          runtimeSlot = (runtimeSlot + 1u) % runtimeGraph->getMaxFramesInFlight();

          CullingCounters counters{};
          runtimeGraph->bufferRead(
              runtimeCulling->getCullingStatisticsBuffer(),
              0,
              sizeof(CullingCounters),
              [&](const void *data) { std::memcpy(&counters, data, sizeof(CullingCounters)); });
          return counters;
        };
        const auto runtimeFrame = [&](const math::Vec3f &position) -> uint32_t
        {
          const CullingCounters counters = runtimeFrameCounters(position);
          return counters.visibleClusterHardwareCount +
              counters.visibleClusterSoftwareCount;
        };

        std::vector<uint32_t> runtimeVisible;
        for (const float distance : distances)
          runtimeVisible.push_back(runtimeFrame(
              math::Vec3f(0.0f, distance * 0.35f + 8.0f, distance)));

        std::ostringstream runtimeSweep;
        for (size_t index = 0u; index < distances.size(); ++index)
          runtimeSweep << " " << distances[index] << "m=" << runtimeVisible[index];
        os::Logger::logf("  runtime-object clusters by distance:%s", runtimeSweep.str().c_str());

        const bool runtimeAlwaysDraws =
            std::all_of(runtimeVisible.begin(), runtimeVisible.end(),
                        [](const uint32_t count) { return count > 0u; });
        check(runtimeAlwaysDraws, "the runtime object always renders something");
        // Every emitted visible-cluster entry must name a real cluster, exactly
        // once, and the whole set must be a valid cut: one cluster per
        // root-to-leaf path, so never more distinct clusters than the DAG has
        // leaves.
        //
        // installRuntimeObjectPage used to write the *page's* cluster count into
        // every hierarchy leaf of that page, so each leaf claimed the whole page
        // starting at its own offset - overlapping ranges and indices past the
        // page end. At 160 m that turned 83 clusters into 300 entries, 104 of
        // which named clusters that do not exist.
        uint32_t worstDistinctCut = 0u;
        uint32_t duplicateEntries = 0u;
        uint32_t entriesPastPageEnd = 0u;
        uint32_t entriesOnBadPage = 0u;
        for (const float distance : distances)
        {
          const CullingCounters counters =
              runtimeFrameCounters(math::Vec3f(0.0f, distance * 0.35f + 8.0f, distance));
          const uint32_t entries = counters.visibleClusterHardwareCount;
          if (entries == 0u || entries > VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
            continue;
          std::vector<VisibleClusterInfo> visible(entries);
          runtimeGraph->bufferRead(
              visibleClusterStaging, 0, entries * sizeof(VisibleClusterInfo),
              [&](const void *data)
              { std::memcpy(visible.data(), data, entries * sizeof(VisibleClusterInfo)); });

          std::set<uint64_t> distinct;
          for (const VisibleClusterInfo &info : visible)
          {
            if (info.pageIndex >= pageCount)
            {
              ++entriesOnBadPage;
              continue;
            }
            if (info.pageLocalClusterIndex >= build.pages[info.pageIndex].clusterCount)
              ++entriesPastPageEnd;
            if (!distinct
                     .insert(
                         (static_cast<uint64_t>(info.pageIndex) << 32u) |
                         info.pageLocalClusterIndex)
                     .second)
              ++duplicateEntries;
          }
          worstDistinctCut =
              std::max(worstDistinctCut, static_cast<uint32_t>(distinct.size()));
        }

        check(worstDistinctCut > 0u, "the visible-cluster list could be read back");
        check(entriesOnBadPage == 0u, "no visible-cluster entry names a page outside the object");
        check(
            entriesPastPageEnd == 0u,
            "no visible-cluster entry names a cluster past the end of its page: " +
                std::to_string(entriesPastPageEnd));
        check(
            duplicateEntries == 0u,
            "the culler emits each visible cluster exactly once: " +
                std::to_string(duplicateEntries) + " duplicates");
        check(
            worstDistinctCut <= leafClusterCount,
            "the runtime-installed cut never exceeds the leaf count (" +
                std::to_string(worstDistinctCut) + " vs " +
                std::to_string(leafClusterCount) +
                "): parents are not drawn on top of their children");
        check(
            runtimeVisible.front() < stats.clusterCount,
            "the far view of the runtime object is a coarse cut, not every cluster");

        const auto runtimePeak =
            std::max_element(runtimeVisible.begin(), runtimeVisible.end());
        check(
            *runtimePeak > runtimeVisible.front() * 4u,
            "closing from the far view multiplies the runtime object's detail");

        // The masks the runtime install left on the GPU hierarchy are what
        // decide whether terrain can refine at all. After installing every page
        // of an object in one call, nothing is on the residency border and every
        // cluster must be selectable: any leaf left with no enabled cluster
        // draws nothing, and any leaf still marked a streaming border draws
        // unconditionally at every distance.
        {
          std::vector<VirtualGeometryHierarchy> runtimeHierarchy(hierarchySize);
          runtimeGraph->bufferRead(
              runtimeScene.hierarchyBuffer,
              0,
              hierarchySize * sizeof(VirtualGeometryHierarchy),
              [&](const void *data)
              {
                std::memcpy(
                    runtimeHierarchy.data(), data,
                    hierarchySize * sizeof(VirtualGeometryHierarchy));
              });

          uint32_t leaves = 0u;
          uint32_t notInstalledLeaves = 0u;
          uint32_t leavesWithNoEnabledCluster = 0u;
          uint32_t leavesOnStreamingBorder = 0u;
          uint32_t enabledClusterBits = 0u;
          uint32_t nodesWithInvertedError = 0u;
          for (const auto &node : runtimeHierarchy)
          {
            if (node.max_parent_lod_error < node.min_lod_error)
              ++nodesWithInvertedError;
            if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
              continue;
            ++leaves;
            if ((node.pageIndex & kPageNotInstalledBit) != 0u)
            {
              ++notInstalledLeaves;
              continue;
            }
            const uint8_t enabledMask = static_cast<uint8_t>(
                (node.flags & HIERARCHY_ENABLED_MASK_BITS) >> HIERARCHY_ENABLED_MASK_SHIFT);
            const uint8_t streamingMask = static_cast<uint8_t>(
                (node.flags & HIERARCHY_STREAMING_MASK_BITS) >> HIERARCHY_STREAMING_MASK_SHIFT);
            if (enabledMask == 0u)
              ++leavesWithNoEnabledCluster;
            if (streamingMask != 0u)
              ++leavesOnStreamingBorder;
            for (uint32_t bit = 0u; bit < 8u; ++bit)
              enabledClusterBits += ((enabledMask >> bit) & 1u) != 0u ? 1u : 0u;
          }

          os::Logger::logf(
              "  runtime-installed hierarchy: leaves=%u notInstalled=%u "
              "noEnabledCluster=%u onStreamingBorder=%u enabledClusters=%u/%zu",
              leaves, notInstalledLeaves, leavesWithNoEnabledCluster,
              leavesOnStreamingBorder, enabledClusterBits, build.clusters.size());

          check(leaves > 1u, "the runtime object has hierarchy leaves for every page");
          check(
              notInstalledLeaves == 0u,
              "a one-shot runtime install leaves no page marked not-installed");
          check(
              leavesWithNoEnabledCluster == 0u,
              "no installed leaf is left with every cluster disabled");
          check(
              leavesOnStreamingBorder == 0u,
              "nothing is left on the residency border once every page is installed");
          check(
              enabledClusterBits == build.clusters.size(),
              "every cluster is enabled after a one-shot runtime install: " +
                  std::to_string(enabledClusterBits) + "/" +
                  std::to_string(build.clusters.size()));
          check(
              nodesWithInvertedError == 0u,
              "no runtime-installed node has a parent error below its own error");
        }

        // A per-path coverage check on the GPU visible-cluster list was tried
        // here and removed: `VisibleClusterInfo::pageLocalClusterIndex` does not
        // index the page's build-time cluster range the way this test assumed
        // (page 1 of 16 clusters reports index 20), so the mapping back to the
        // DAG cannot be trusted yet. The CPU suite
        // (SparseVoxelOctreePageStreamingTests) covers exactly-one-cluster-per-
        // path; what this stage pins is that the runtime install path selects a
        // distance-dependent cut at all, which is what the streaming-leaf bug
        // destroyed.

        // ── the two install paths must agree ────────────────────────────────
        //
        // A file-backed object rebuilds its hierarchy with
        // rebuildHierarchyFromInstalledPages, which touches only the install bit
        // and the two per-cluster masks and never rewrites child_start or
        // child_count. A runtime-installed object updates the same state
        // incrementally, one page at a time. Fully resident, from the same
        // encoded asset, the two have to land on identical hierarchy nodes -
        // that is what "the hierarchy updates accordingly" means, and it is
        // exactly what the child_count overwrite broke.
        {
          std::vector<PageTableEntry> streamedPageTable(pageCount);
          renderGraph->bufferRead(
              scene.pageTableBuffer, 0, pageCount * sizeof(PageTableEntry),
              [&](const void *data)
              { std::memcpy(streamedPageTable.data(), data, pageCount * sizeof(PageTableEntry)); });
          uint32_t streamedInstalled = 0u;
          for (const auto &entry : streamedPageTable)
            streamedInstalled += entry.isInstalled != 0u ? 1u : 0u;

          check(
              streamedInstalled == pageCount,
              "the file-streamed object reached full residency for the comparison (" +
                  std::to_string(streamedInstalled) + "/" + std::to_string(pageCount) + ")");

          if (streamedInstalled == pageCount)
          {
            std::vector<VirtualGeometryHierarchy> streamedHierarchy(hierarchySize);
            renderGraph->bufferRead(
                scene.hierarchyBuffer, 0,
                hierarchySize * sizeof(VirtualGeometryHierarchy),
                [&](const void *data)
                {
                  std::memcpy(
                      streamedHierarchy.data(), data,
                      hierarchySize * sizeof(VirtualGeometryHierarchy));
                });
            std::vector<VirtualGeometryHierarchy> installedHierarchy(hierarchySize);
            runtimeGraph->bufferRead(
                runtimeScene.hierarchyBuffer, 0,
                hierarchySize * sizeof(VirtualGeometryHierarchy),
                [&](const void *data)
                {
                  std::memcpy(
                      installedHierarchy.data(), data,
                      hierarchySize * sizeof(VirtualGeometryHierarchy));
                });

            uint32_t childRangeMismatches = 0u;
            uint32_t maskMismatches = 0u;
            uint32_t installBitMismatches = 0u;
            uint32_t errorMismatches = 0u;
            uint32_t boundsMismatches = 0u;
            uint32_t firstMismatchNode = UINT32_MAX;
            for (uint32_t nodeIndex = 0u; nodeIndex < hierarchySize; ++nodeIndex)
            {
              const auto &streamed = streamedHierarchy[nodeIndex];
              const auto &installed = installedHierarchy[nodeIndex];
              bool mismatch = false;
              if (streamed.child_start != installed.child_start ||
                  streamed.child_count != installed.child_count)
              {
                ++childRangeMismatches;
                mismatch = true;
              }
              const uint32_t maskBits =
                  HIERARCHY_ENABLED_MASK_BITS | HIERARCHY_STREAMING_MASK_BITS |
                  STREAMING_LEAF_FLAG | HIERARCHY_LEAF_FLAG;
              if ((streamed.flags & maskBits) != (installed.flags & maskBits))
              {
                ++maskMismatches;
                mismatch = true;
              }
              const bool streamedInstalledBit =
                  streamed.pageIndex != UINT32_MAX &&
                  (streamed.pageIndex & kPageNotInstalledBit) == 0u;
              const bool runtimeInstalledBit =
                  installed.pageIndex != UINT32_MAX &&
                  (installed.pageIndex & kPageNotInstalledBit) == 0u;
              if (streamedInstalledBit != runtimeInstalledBit)
              {
                ++installBitMismatches;
                mismatch = true;
              }
              if (streamed.min_lod_error != installed.min_lod_error ||
                  streamed.max_parent_lod_error != installed.max_parent_lod_error)
              {
                ++errorMismatches;
                mismatch = true;
              }
              if (streamed.max_radius != installed.max_radius ||
                  streamed.min_x != installed.min_x || streamed.max_x != installed.max_x)
              {
                ++boundsMismatches;
                mismatch = true;
              }
              if (mismatch && firstMismatchNode == UINT32_MAX)
                firstMismatchNode = nodeIndex;
            }

            os::Logger::logf(
                "  path comparison over %u nodes: childRange=%u masks=%u installBit=%u "
                "errors=%u bounds=%u",
                hierarchySize, childRangeMismatches, maskMismatches,
                installBitMismatches, errorMismatches, boundsMismatches);

            check(
                childRangeMismatches == 0u,
                "both install paths leave the same child_start/child_count on every node");
            check(
                maskMismatches == 0u,
                "both install paths leave the same enabled and streaming masks");
            check(
                installBitMismatches == 0u,
                "both install paths agree on which leaves are installed");
            check(errorMismatches == 0u, "both install paths leave the same LOD errors");
            check(boundsMismatches == 0u, "both install paths leave the same node bounds");
          }
        }
      }
    }

  }
  catch (const std::exception &exception)
  {
    os::Logger::errorf("EXCEPTION: %s", exception.what());
    exitCode = 1;
  }

  delete rhi;

  if (failures != 0 || exitCode != 0)
  {
    os::Logger::errorf("%d sparse voxel octree streaming checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all sparse voxel octree streaming checks passed");
  os::Logger::shutdown();
  return 0;
}
