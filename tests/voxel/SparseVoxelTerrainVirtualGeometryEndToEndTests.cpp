// Terrain, end to end, on the GPU: generate, build, install, stream, render.
//
// Every other test in this suite works on synthetic voxel geometry. This one
// starts from a real generated terrain chunk and follows it all the way to
// pixels through the passes the sample uses:
//
//   buildWorldMapVoxelTerrain            generate the chunk and its brick octree
//   buildSparseVoxelTerrainOctreeVirtualGeometryAsset
//                                        mesh every octree level, build the DAG,
//                                        encode streamable pages
//   installRuntimeEncodedObject          the one-shot install the sample uses
//   registerObjectForStreaming           the incremental path, for comparison
//   page decode -> culling -> hardware draw -> id readback
//
// and then asks the questions that matter for terrain specifically: does the
// chunk render at all, does the cut refine as the camera closes, and is the
// selected geometry a valid cut rather than several levels stacked on top of
// each other.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "TerrainGenerationPhase0Utils.hpp"
#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryIdReadbackPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryPageDecodePass.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

namespace svr = voxel::svraster;
namespace fs = std::filesystem;

namespace
{

constexpr uint32_t kPageNotInstalledBit = 1u << 31u;
constexpr uint32_t kImageSide = 256u;
constexpr uint32_t kMaxQueueElements = 1u << 20;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

// Everything the CPU side produces for one terrain chunk.
struct TerrainChunkAsset
{
  svr::PlanetVoxelChunkSet chunks;
  VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats buildStats;
  VirtualGeometryEncodedData encoded;
  svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats bridgeStats;
  std::array<double, 3> pageOrigin{};
  double edgeMetres = 0.0;
  double voxelSizeMetres = 0.0;
  uint32_t leafClusterCount = 0u;
  bool valid = false;
};

TerrainChunkAsset generateTerrainChunk(const terrain_phase0::FixtureKind kind)
{
  TerrainChunkAsset asset{};
  std::string error;

  const auto bundle = terrain_phase0::makeFixture(kind);
  const auto settings = terrain_phase0::fixtureSettings(kind, 0u);
  svr::WorldMapVoxelGenerationMetrics metrics{};
  if (!svr::buildWorldMapVoxelTerrain(
          bundle, settings, asset.chunks, &error, {}, nullptr, &metrics))
  {
    os::Logger::errorf("terrain generation failed: %s", error.c_str());
    return asset;
  }
  if (asset.chunks.chunks.empty())
  {
    os::Logger::error("terrain generation produced no chunks");
    return asset;
  }

  const auto &record = asset.chunks.chunks[0];
  asset.edgeMetres = record.edgeMetres;
  asset.voxelSizeMetres = record.voxelSizeMetres;

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  VirtualGeometryBuildSettings buildSettings{};
  // Small pages so streaming has real work; the sample's default is 64 groups.
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.maxRootPageGroups = 1u;
  QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  quantization.padMeshlets = true;

  if (!svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
          asset.chunks, 0u, octreeSettings, buildSettings, quantization,
          asset.encoded, &asset.pageOrigin, &asset.bridgeStats, &error))
  {
    os::Logger::errorf("terrain virtual geometry build failed: %s", error.c_str());
    return asset;
  }

  // The bridge returns the encoded pages; rebuild the uncompressed build data so
  // the test can map a visible cluster back to the octree level it came from.
  svr::SparseVoxelOctreeVirtualGeometrySettings sourceSettings = octreeSettings;
  sourceSettings.originOffset = asset.pageOrigin;
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          asset.chunks.bricks, record.brickNodeOffset, record.brickNodeCapacity,
          sourceSettings, buildSettings, asset.build, &asset.buildStats, &error))
  {
    os::Logger::errorf("terrain source build failed: %s", error.c_str());
    return asset;
  }

  for (uint32_t cluster = 0u; cluster < asset.build.clusterDAG.size(); ++cluster)
    asset.leafClusterCount += asset.build.clusterDAG[cluster].empty() ? 1u : 0u;

  asset.valid = true;
  return asset;
}

struct FrameResult
{
  uint32_t visibleClusters = 0u;
  uint32_t distinctClusters = 0u;
  uint32_t duplicateEntries = 0u;
  uint32_t outOfRangeEntries = 0u;
  uint32_t coveredPixels = 0u;
  uint32_t distinctSurfaces = 0u;
  uint32_t installedPages = 0u;
  std::map<uint32_t, uint32_t> clustersPerLevel;
};

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Terrain virtual geometry, end to end on the GPU");
  os::Logger::log("===============================================");

  os::Logger::log("[1] generating a terrain chunk and building its virtual geometry");
  TerrainChunkAsset asset = generateTerrainChunk(terrain_phase0::FixtureKind::Mountain);
  check(asset.valid, "the terrain chunk generates and builds virtual geometry");
  if (!asset.valid)
  {
    os::Logger::shutdown();
    return 1;
  }

  const uint32_t pageCount = static_cast<uint32_t>(asset.encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(asset.encoded.hierarchy.size());
  os::Logger::logf(
      "  chunk edge=%.1fm voxel=%.2fm octreeLevels=%u bricks=%u clusters=%zu "
      "leaves=%u pages=%u hierarchyNodes=%u meshlets=%u",
      asset.edgeMetres, asset.voxelSizeMetres, asset.buildStats.levelCount,
      asset.buildStats.nodeCount, asset.build.clusters.size(), asset.leafClusterCount,
      pageCount, hierarchySize, asset.bridgeStats.encodedMeshletCount);

  check(asset.buildStats.levelCount > 1u, "the chunk octree has more than one level");
  check(pageCount > 1u, "the chunk's DAG is split over several pages");
  check(asset.leafClusterCount > 0u, "the chunk's DAG has leaf clusters");

  const fs::path assetPath =
      fs::path(os::io::getExecutableDirectory()) / "terrain-chunk.virtualgeometry";
  {
    VirtualGeometryFile writer(assetPath.string(), /*write=*/true);
    if (!writer.isOpen() ||
        !writer.write(asset.encoded, asset.build.pages, MESHLET_LZ4))
    {
      os::Logger::errorf("could not write %s", assetPath.string().c_str());
      os::Logger::shutdown();
      return 1;
    }
  }

  DeviceRequiredLimits limits{};
  const DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute |
      DeviceFeatures::DeviceFeatures_Graphics |
      DeviceFeatures::DeviceFeatures_Subgroup_Basic |
      DeviceFeatures::DeviceFeatures_Subgroup_Shuffle |
      DeviceFeatures::DeviceFeatures_Timestamp |
      DeviceFeatures::DeviceFeatures_ClipDistances;
  auto *rhi = new backend::vulkan::VulkanRHI(
      backend::vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi->init(surfaces);

  int exitCode = 0;
  try
  {
    const uint32_t hierarchyBytes = std::max(
        hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
    auto graphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *graph = graphStorage.get();
    VirtualGeometryScene scene(graph, hierarchyBytes, 256ull * 1024 * 1024);

    os::Logger::log("[2] installing the chunk the way the sample does");
    VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
    install.name = "terrain-chunk";
    install.asset = &asset.encoded;
    install.basePageGeneration = 1u;
    install.geometryRevision = 1u;
    install.materialRevision = 1u;
    // The bridge emits page-relative positions, so the instance carries the page
    // origin - exactly what the sample does with its per-chunk asset set.
    install.position = math::Vec3f(0.0f, 0.0f, 0.0f);
    install.hasTracePayload = true;
    install.enqueuePageDecode = true;

    VirtualGeometryScene::RuntimeEncodedObjectInstallStats installStats{};
    std::string installError;
    const bool installed =
        scene.installRuntimeEncodedObject(install, &installStats, &installError);
    check(installed, "the terrain chunk installs as a runtime object: " + installError);
    if (!installed)
      throw std::runtime_error("terrain install failed");
    check(
        installStats.installedPageCount == pageCount,
        "every terrain page installs");
    scene.updateInstanceBuffer();

    // The chunk's local extent, from the cluster block boxes the builder emitted.
    std::array<float, 3> localMin{1e30f, 1e30f, 1e30f};
    std::array<float, 3> localMax{-1e30f, -1e30f, -1e30f};
    for (const auto &source : asset.buildStats.clusterSources)
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        localMin[axis] = std::min(localMin[axis], source.blockMinimum[axis]);
        localMax[axis] = std::max(localMax[axis], source.blockMinimum[axis] + source.blockExtent);
      }
    const math::Vec3f chunkCenter(
        (localMin[0] + localMax[0]) * 0.5f,
        (localMin[1] + localMax[1]) * 0.5f,
        (localMin[2] + localMax[2]) * 0.5f);
    const float chunkRadius = 0.5f * std::sqrt(
        std::pow(localMax[0] - localMin[0], 2.0f) +
        std::pow(localMax[1] - localMin[1], 2.0f) +
        std::pow(localMax[2] - localMin[2], 2.0f));
    os::Logger::logf(
        "  chunk local extent (%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f) radius=%.1f",
        localMin[0], localMin[1], localMin[2], localMax[0], localMax[1], localMax[2],
        chunkRadius);

    // ── passes ───────────────────────────────────────────────────────────────
    const auto makeColorTexture = [&](const char *name, const Format format)
    {
      return graph->createTexture(TextureInfo{
        .name = name,
        .format = format,
        .height = kImageSide,
        .width = kImageSide,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      });
    };
    const auto colorView = [](const Texture texture)
    {
      return TextureView{
        .texture = texture,
        .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = ResourceLayout::COLOR_ATTACHMENT,
        .index = 0u,
        .flags = ImageAspectFlags::Color,
        .baseArrayLayer = 0u,
        .baseMipLevel = 0u,
        .layerCount = 1u,
        .levelCount = 1u,
      };
    };

    Texture depthTexture = graph->createTexture(TextureInfo{
      .name = "Terrain_Depth.texture",
      .format = Format::Format_Depth32Float,
      .height = kImageSide,
      .width = kImageSide,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
    });
    const TextureView depthView{
      .texture = depthTexture,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE |
          AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
      .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0u,
      .flags = ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };

    Texture idsLo = makeColorTexture("Terrain_IdsLo.texture", Format::Format_R32Uint);
    Texture idsHi = makeColorTexture("Terrain_IdsHi.texture", Format::Format_R32Uint);
    Texture materialId = makeColorTexture("Terrain_MaterialId.texture", Format::Format_R32Uint);
    Texture materialUV = makeColorTexture("Terrain_MaterialUV.texture", Format::Format_RG32Float);
    Texture motionVector = makeColorTexture("Terrain_Motion.texture", Format::Format_RG16Float);
    Texture baseColor = makeColorTexture("Terrain_BaseColor.texture", Format::Format_RGBA16Float);
    Texture materialProperties =
        makeColorTexture("Terrain_MaterialProperties.texture", Format::Format_RGBA16Float);
    Texture emissive = makeColorTexture("Terrain_Emissive.texture", Format::Format_RGBA16Float);
    Texture geometryNormal =
        makeColorTexture("Terrain_GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture normal = makeColorTexture("Terrain_Normal.texture", Format::Format_RGBA16Float);
    Texture color = makeColorTexture("Terrain_Color.texture", Format::Format_RGBA16Float);

    const float errorBudget = 1.0f;
    VirtualGeometryCullingMultipleDispatchesPass::Settings cullingSettings;
    cullingSettings.maxHierarchyLevels = 32u;
    cullingSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    cullingSettings.errorTreshhold = errorBudget;
    cullingSettings.viewPortWidth = kImageSide;
    cullingSettings.viewPortHeight = kImageSide;
    cullingSettings.enableOcclusionCulling = false;

    graph->registerPass<VirtualGeometryPageDecodePass>("terrain_pageDecode", 0, scene);
    auto *culling = graph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(
        "terrain_culling", 1, scene, cullingSettings, depthTexture);

    VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
      .packedGeometryIdsLoView = colorView(idsLo),
      .packedGeometryIdsHiView = colorView(idsHi),
      .materialIdView = colorView(materialId),
      .materialUVView = colorView(materialUV),
      .baseColorView = colorView(baseColor),
      .materialPropertiesView = colorView(materialProperties),
      .emissiveView = colorView(emissive),
      .geometryNormalView = colorView(geometryNormal),
      .normalView = colorView(normal),
      .motionVectorView = colorView(motionVector),
      .depthView = depthView,
      .depthTexture = depthTexture,
      .baseColorTexture = baseColor,
      .materialPropertiesTexture = materialProperties,
      .emissiveTexture = emissive,
      .geometryNormalTexture = geometryNormal,
      .normalTexture = normal,
      .motionVectorTexture = motionVector,
      .colorView = colorView(color),
      .colorTexture = color,
    };
    VirtualGeometryHardwareDrawPass::Settings drawSettings;
    drawSettings.viewPortWidth = kImageSide;
    drawSettings.viewPortHeight = kImageSide;
    drawSettings.cullMode = CullMode::None;
    drawSettings.directDrawWhenIndirectCountUnavailable = true;
    auto *hardwareDraw = graph->registerPass<VirtualGeometryHardwareDrawPass>(
        "terrain_hardwareDraw", 2, scene,
        culling->getHWDrawIndirectBuffer(),
        culling->getHWVisibleClusterInfosBuffer(),
        culling->getCullingStatisticsBuffer(),
        frameTarget, /*shouldClearDepth=*/true, 0.0f, drawSettings);

    const uint64_t idBytes =
        static_cast<uint64_t>(kImageSide) * kImageSide * sizeof(uint32_t) * 2u;
    Buffer idBuffer = graph->createBuffer(BufferInfo{
      .name = "Terrain_Ids.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    Buffer idStaging = graph->createBuffer(BufferInfo{
      .name = "Terrain_IdsStaging.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
    });
    const uint64_t visibleClusterBytes =
        static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) *
        sizeof(VisibleClusterInfo);
    Buffer visibleStaging = graph->createBuffer(BufferInfo{
      .name = "Terrain_VisibleStaging.buffer",
      .size = visibleClusterBytes,
      .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
    });

    graph->registerPass<VirtualGeometryIdReadbackPass>(
        "terrain_idReadback", 3, idsLo, idsHi, idBuffer, kImageSide, kImageSide);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "terrain_copyIds", 4, idBuffer, 0, idBytes, idStaging, 0, idBytes);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "terrain_copyVisible", 5, culling->getHWVisibleClusterInfosBuffer(), 0,
        visibleClusterBytes, visibleStaging, 0, visibleClusterBytes);
    graph->compile();

    uint32_t frameSlot = 0u;
    const auto renderAt = [&](const float distance) -> FrameResult
    {
      const math::Vec3f eye(
          chunkCenter[0],
          chunkCenter[1] + distance * 0.4f,
          chunkCenter[2] + distance);
      Camera camera(60.0f * 3.14159265f / 180.0f, eye, math::Vec3f(0.0f, 0.0f, -1.0f), true);
      camera.setAspectRatio(1.0f);
      camera.setNearFar(0.5f, 20000.0f);
      camera.lookAt(chunkCenter);
      camera.updateMatrices();
      const float *viewArray = camera.getViewMatrix().data;
      const float *projectionArray = camera.getProjectionMatrix().data;
      const math::Vec3f &position = camera.getPosition();
      float viewPosition[4] = {position[0], position[1], position[2], 1.0f};
      culling->updateUniforms(
          viewArray, projectionArray, viewPosition,
          kImageSide, kImageSide, 0.5f, 20000.0f, 1u);
      hardwareDraw->updateUniforms(
          viewArray, projectionArray, viewPosition,
          kImageSide, kImageSide, 0.5f, 20000.0f, 1u, errorBudget);

      scene.prepareFrame(frameSlot);
      RenderGraph::Overrides overrides;
      scene.appendFrameOverrides(overrides, frameSlot);
      culling->appendFrameOverrides(overrides, frameSlot);
      hardwareDraw->appendFrameOverrides(overrides, frameSlot);
      RenderGraph::Frame frame;
      graph->run(frame, overrides);
      graph->waitFrame(frame);
      frameSlot = (frameSlot + 1u) % graph->getMaxFramesInFlight();

      FrameResult result{};
      CullingCounters counters{};
      graph->bufferRead(
          culling->getCullingStatisticsBuffer(), 0, sizeof(CullingCounters),
          [&](const void *data) { std::memcpy(&counters, data, sizeof(CullingCounters)); });
      result.visibleClusters = counters.visibleClusterHardwareCount;

      std::vector<PageTableEntry> pageTable(pageCount);
      graph->bufferRead(
          scene.pageTableBuffer, 0, pageCount * sizeof(PageTableEntry),
          [&](const void *data)
          { std::memcpy(pageTable.data(), data, pageCount * sizeof(PageTableEntry)); });
      for (const auto &entry : pageTable)
        result.installedPages += entry.isInstalled != 0u ? 1u : 0u;

      if (result.visibleClusters != 0u &&
          result.visibleClusters <= VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
      {
        std::vector<VisibleClusterInfo> visible(result.visibleClusters);
        graph->bufferRead(
            visibleStaging, 0, result.visibleClusters * sizeof(VisibleClusterInfo),
            [&](const void *data)
            {
              std::memcpy(
                  visible.data(), data,
                  result.visibleClusters * sizeof(VisibleClusterInfo));
            });
        std::set<uint64_t> distinct;
        for (const VisibleClusterInfo &info : visible)
        {
          if (info.pageIndex >= pageCount ||
              info.pageLocalClusterIndex >= asset.build.pages[info.pageIndex].clusterCount)
          {
            ++result.outOfRangeEntries;
            continue;
          }
          if (!distinct
                   .insert(
                       (static_cast<uint64_t>(info.pageIndex) << 32u) |
                       info.pageLocalClusterIndex)
                   .second)
          {
            ++result.duplicateEntries;
            continue;
          }
          const uint32_t cluster =
              asset.build.pages[info.pageIndex].clusterOffset + info.pageLocalClusterIndex;
          ++result.clustersPerLevel[asset.buildStats.clusterSources[cluster].level];
        }
        result.distinctClusters = static_cast<uint32_t>(distinct.size());
      }

      std::vector<uint32_t> packed(kImageSide * kImageSide * 2u, 0u);
      graph->bufferRead(
          idStaging, 0, idBytes,
          [&](const void *data) { std::memcpy(packed.data(), data, idBytes); });
      std::set<uint32_t> surfaces;
      for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
      {
        const uint32_t id = packed[pixel * 2u + 1u];
        if (id == 0u)
          continue;
        ++result.coveredPixels;
        surfaces.insert(id);
      }
      result.distinctSurfaces = static_cast<uint32_t>(surfaces.size());
      return result;
    };

    // ── [3] does the terrain render at all ──────────────────────────────────
    os::Logger::log("[3] rendering the installed chunk");
    const float nearDistance = chunkRadius * 1.2f;
    const FrameResult nearFrame = renderAt(nearDistance);
    os::Logger::logf(
        "  at %.1fm: pages=%u clusters=%u distinct=%u pixels=%u surfaces=%u",
        nearDistance, nearFrame.installedPages, nearFrame.visibleClusters,
        nearFrame.distinctClusters, nearFrame.coveredPixels, nearFrame.distinctSurfaces);

    check(nearFrame.installedPages == pageCount, "every terrain page is resident");
    check(nearFrame.visibleClusters > 0u, "the culler selects terrain clusters");
    check(nearFrame.coveredPixels > 0u, "the terrain chunk rasterises to pixels");
    check(
        nearFrame.distinctSurfaces > 1u,
        "the rendered terrain is made of many distinct surfaces");

    // ── [4] LOD refinement ──────────────────────────────────────────────────
    os::Logger::log("[4] LOD refinement across camera distance");
    const std::array<float, 6> distances{
        chunkRadius * 40.0f, chunkRadius * 16.0f, chunkRadius * 8.0f,
        chunkRadius * 4.0f,  chunkRadius * 2.0f,  chunkRadius * 1.2f};

    std::vector<FrameResult> sweep;
    for (const float distance : distances)
      sweep.push_back(renderAt(distance));

    std::ostringstream summary;
    for (size_t index = 0u; index < distances.size(); ++index)
    {
      summary << "\n    d=" << std::fixed << distances[index]
              << "m clusters=" << sweep[index].distinctClusters
              << " pixels=" << sweep[index].coveredPixels << " byLevel:";
      for (const auto &[level, count] : sweep[index].clustersPerLevel)
        summary << " L" << level << "=" << count;
    }
    os::Logger::logf("  LOD sweep:%s", summary.str().c_str());

    uint32_t framesWithDuplicates = 0u;
    uint32_t framesWithOutOfRange = 0u;
    uint32_t framesWithNothing = 0u;
    uint32_t framesOverLeafBound = 0u;
    for (const FrameResult &frame : sweep)
    {
      framesWithDuplicates += frame.duplicateEntries != 0u ? 1u : 0u;
      framesWithOutOfRange += frame.outOfRangeEntries != 0u ? 1u : 0u;
      framesWithNothing += frame.coveredPixels == 0u ? 1u : 0u;
      framesOverLeafBound +=
          frame.distinctClusters > asset.leafClusterCount ? 1u : 0u;
    }
    check(framesWithNothing == 0u, "terrain renders at every distance");
    check(
        framesWithDuplicates == 0u,
        "no distance emits the same terrain cluster twice");
    check(
        framesWithOutOfRange == 0u,
        "no distance emits a terrain cluster outside its page");
    check(
        framesOverLeafBound == 0u,
        "the terrain cut never exceeds the leaf count: levels are not stacked");

    // The coarsest view must draw coarser octree levels than the closest one.
    const auto finestLevelDrawn = [](const FrameResult &frame)
    {
      return frame.clustersPerLevel.empty() ? UINT32_MAX
                                            : frame.clustersPerLevel.begin()->first;
    };
    const uint32_t farFinest = finestLevelDrawn(sweep.front());
    const uint32_t nearFinest = finestLevelDrawn(sweep.back());
    check(
        farFinest != UINT32_MAX && nearFinest != UINT32_MAX,
        "both the far and near views draw terrain");
    check(
        nearFinest <= farFinest,
        "closing on the terrain draws at least as fine a level (" +
            std::to_string(nearFinest) + " vs " + std::to_string(farFinest) + ")");
    check(
        sweep.back().distinctClusters > sweep.front().distinctClusters,
        "the near view of the terrain draws more clusters than the far view (" +
            std::to_string(sweep.back().distinctClusters) + " vs " +
            std::to_string(sweep.front().distinctClusters) + ")");
    check(
        sweep.front().distinctClusters < asset.build.clusters.size(),
        "the far view draws a coarse cut, not the whole terrain DAG");

    // ── [5] streaming the same chunk in page by page ────────────────────────
    os::Logger::log("[5] streaming the same chunk in, one page per update");
    auto streamGraphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *streamGraph = streamGraphStorage.get();
    VirtualGeometryScene streamScene(streamGraph, hierarchyBytes, 256ull * 1024 * 1024);
    streamScene.registerObjectForStreaming("terrain-streamed", assetPath.string());
    streamScene.instantiateObjectInstance(
        "terrain-streamed", math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
    streamScene.updateInstanceBuffer();

    const auto makeStreamTexture = [&](const char *name, const Format format)
    {
      return streamGraph->createTexture(TextureInfo{
        .name = name,
        .format = format,
        .height = kImageSide,
        .width = kImageSide,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      });
    };
    Texture streamDepth = streamGraph->createTexture(TextureInfo{
      .name = "TerrainStream_Depth.texture",
      .format = Format::Format_Depth32Float,
      .height = kImageSide,
      .width = kImageSide,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
    });
    const TextureView streamDepthView{
      .texture = streamDepth,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE |
          AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
      .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
      .index = 0u,
      .flags = ImageAspectFlags::Depth,
      .baseArrayLayer = 0u,
      .baseMipLevel = 0u,
      .layerCount = 1u,
      .levelCount = 1u,
    };
    Texture streamIdsLo = makeStreamTexture("TerrainStream_IdsLo.texture", Format::Format_R32Uint);
    Texture streamIdsHi = makeStreamTexture("TerrainStream_IdsHi.texture", Format::Format_R32Uint);
    Texture streamMaterialId =
        makeStreamTexture("TerrainStream_MaterialId.texture", Format::Format_R32Uint);
    Texture streamMaterialUV =
        makeStreamTexture("TerrainStream_MaterialUV.texture", Format::Format_RG32Float);
    Texture streamMotion =
        makeStreamTexture("TerrainStream_Motion.texture", Format::Format_RG16Float);
    Texture streamBaseColor =
        makeStreamTexture("TerrainStream_BaseColor.texture", Format::Format_RGBA16Float);
    Texture streamMaterialProperties =
        makeStreamTexture("TerrainStream_MaterialProps.texture", Format::Format_RGBA16Float);
    Texture streamEmissive =
        makeStreamTexture("TerrainStream_Emissive.texture", Format::Format_RGBA16Float);
    Texture streamGeometryNormal =
        makeStreamTexture("TerrainStream_GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture streamNormal =
        makeStreamTexture("TerrainStream_Normal.texture", Format::Format_RGBA16Float);
    Texture streamColor =
        makeStreamTexture("TerrainStream_Color.texture", Format::Format_RGBA16Float);

    VirtualGeometryCullingMultipleDispatchesPass::Settings streamCulling = cullingSettings;
    streamGraph->registerPass<VirtualGeometryPageDecodePass>(
        "terrainStream_pageDecode", 0, streamScene);
    auto *streamCullingPass =
        streamGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(
            "terrainStream_culling", 1, streamScene, streamCulling, streamDepth);

    VirtualGeometryHardwareDrawPass::FrameTarget streamTarget{
      .packedGeometryIdsLoView = colorView(streamIdsLo),
      .packedGeometryIdsHiView = colorView(streamIdsHi),
      .materialIdView = colorView(streamMaterialId),
      .materialUVView = colorView(streamMaterialUV),
      .baseColorView = colorView(streamBaseColor),
      .materialPropertiesView = colorView(streamMaterialProperties),
      .emissiveView = colorView(streamEmissive),
      .geometryNormalView = colorView(streamGeometryNormal),
      .normalView = colorView(streamNormal),
      .motionVectorView = colorView(streamMotion),
      .depthView = streamDepthView,
      .depthTexture = streamDepth,
      .baseColorTexture = streamBaseColor,
      .materialPropertiesTexture = streamMaterialProperties,
      .emissiveTexture = streamEmissive,
      .geometryNormalTexture = streamGeometryNormal,
      .normalTexture = streamNormal,
      .motionVectorTexture = streamMotion,
      .colorView = colorView(streamColor),
      .colorTexture = streamColor,
    };
    auto *streamDraw = streamGraph->registerPass<VirtualGeometryHardwareDrawPass>(
        "terrainStream_hardwareDraw", 2, streamScene,
        streamCullingPass->getHWDrawIndirectBuffer(),
        streamCullingPass->getHWVisibleClusterInfosBuffer(),
        streamCullingPass->getCullingStatisticsBuffer(),
        streamTarget, /*shouldClearDepth=*/true, 0.0f, drawSettings);

    Buffer streamIdBuffer = streamGraph->createBuffer(BufferInfo{
      .name = "TerrainStream_Ids.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    Buffer streamIdStaging = streamGraph->createBuffer(BufferInfo{
      .name = "TerrainStream_IdsStaging.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
    });
    streamGraph->registerPass<VirtualGeometryIdReadbackPass>(
        "terrainStream_idReadback", 3, streamIdsLo, streamIdsHi, streamIdBuffer,
        kImageSide, kImageSide);
    streamGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "terrainStream_copyIds", 4, streamIdBuffer, 0, idBytes, streamIdStaging, 0, idBytes);
    streamGraph->compile();

    struct StreamFrame
    {
      uint32_t residentPages = 0u;
      uint32_t visibleClusters = 0u;
      uint32_t coveredPixels = 0u;
      uint32_t distinctSurfaces = 0u;
      uint64_t imageHash = 0u;
    };

    uint32_t streamSlot = 0u;
    const auto streamFrame = [&]() -> StreamFrame
    {
      const math::Vec3f eye(
          chunkCenter[0], chunkCenter[1] + nearDistance * 0.4f, chunkCenter[2] + nearDistance);
      Camera camera(60.0f * 3.14159265f / 180.0f, eye, math::Vec3f(0.0f, 0.0f, -1.0f), true);
      camera.setAspectRatio(1.0f);
      camera.setNearFar(0.5f, 20000.0f);
      camera.lookAt(chunkCenter);
      camera.updateMatrices();
      const math::Vec3f &position = camera.getPosition();
      float viewPosition[4] = {position[0], position[1], position[2], 1.0f};
      streamCullingPass->updateUniforms(
          camera.getViewMatrix().data, camera.getProjectionMatrix().data, viewPosition,
          kImageSide, kImageSide, 0.5f, 20000.0f, 1u);
      streamDraw->updateUniforms(
          camera.getViewMatrix().data, camera.getProjectionMatrix().data, viewPosition,
          kImageSide, kImageSide, 0.5f, 20000.0f, 1u, errorBudget);

      streamScene.prepareFrame(streamSlot);
      RenderGraph::Overrides overrides;
      streamScene.appendFrameOverrides(overrides, streamSlot);
      streamCullingPass->appendFrameOverrides(overrides, streamSlot);
      streamDraw->appendFrameOverrides(overrides, streamSlot);
      RenderGraph::Frame frame;
      streamGraph->run(frame, overrides);
      streamGraph->waitFrame(frame);
      streamSlot = (streamSlot + 1u) % streamGraph->getMaxFramesInFlight();

      StreamFrame result{};
      CullingCounters counters{};
      streamGraph->bufferRead(
          streamCullingPass->getCullingStatisticsBuffer(), 0, sizeof(CullingCounters),
          [&](const void *data) { std::memcpy(&counters, data, sizeof(CullingCounters)); });
      result.visibleClusters = counters.visibleClusterHardwareCount;
      std::vector<PageTableEntry> pageTable(pageCount);
      streamGraph->bufferRead(
          streamScene.pageTableBuffer, 0, pageCount * sizeof(PageTableEntry),
          [&](const void *data)
          { std::memcpy(pageTable.data(), data, pageCount * sizeof(PageTableEntry)); });
      for (const auto &entry : pageTable)
        result.residentPages += entry.isInstalled != 0u ? 1u : 0u;

      // The rendered surface itself, not just the counters: which voxel face
      // won each pixel.
      std::vector<uint32_t> packed(kImageSide * kImageSide * 2u, 0u);
      streamGraph->bufferRead(
          streamIdStaging, 0, idBytes,
          [&](const void *data) { std::memcpy(packed.data(), data, idBytes); });
      std::set<uint32_t> surfaces;
      uint64_t hash = 1469598103934665603ull;
      for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
      {
        const uint32_t id = packed[pixel * 2u + 1u];
        hash = (hash ^ id) * 1099511628211ull;
        if (id == 0u)
          continue;
        ++result.coveredPixels;
        surfaces.insert(id);
      }
      result.distinctSurfaces = static_cast<uint32_t>(surfaces.size());
      result.imageHash = hash;
      return result;
    };

    std::vector<StreamFrame> streamProgress;
    streamProgress.push_back(streamFrame());
    for (uint32_t update = 0u; update < pageCount * 4u; ++update)
    {
      streamScene.updatePageStreaming("terrain-streamed", 1u);
      streamScene.updateInstanceBuffer();
      streamProgress.push_back(streamFrame());
    }

    std::ostringstream progress;
    for (const StreamFrame &frame : streamProgress)
      progress << ' ' << frame.residentPages << "p/" << frame.visibleClusters << "c/"
               << frame.distinctSurfaces << "s";
    os::Logger::logf("  streaming progression (pages/clusters/surfaces):%s", progress.str().c_str());

    check(
        streamProgress.front().residentPages >= 1u,
        "the terrain chunk's root page is resident on the first frame");
    check(
        streamProgress.front().visibleClusters > 0u,
        "the first streamed frame already draws terrain from the root page");
    check(
        streamProgress.front().coveredPixels > 0u,
        "the first streamed frame already rasterises terrain pixels");
    check(
        streamProgress.back().residentPages > streamProgress.front().residentPages,
        "terrain pages stream in beyond the root page");
    check(
        streamProgress.back().visibleClusters > streamProgress.front().visibleClusters,
        "more terrain clusters are drawn once the finer pages arrive");

    // ── [6] the rendered surface changes as pages install ───────────────────
    //
    // Counters can move while the picture does not. This walks the sequence and
    // asks the question directly: when a page install changed residency, did the
    // rendered surface actually change, and did the detail in it grow.
    os::Logger::log("[6] the rendered surface changes from page install to page install");

    // The precise relationship: the picture moves exactly when the drawn cut
    // moves. A page can install whose clusters the error budget does not select
    // at this distance - residency grows, the cut does not, and the image must
    // stay identical. And refining can *reduce* the number of distinct surfaces
    // on screen, because finer faces land sub-pixel and fewer of them win a
    // pixel, so "detail never decreases" is not an invariant at a fixed
    // resolution. What must hold is that nothing renders differently without a
    // reason, and that streaming multiplies the detail overall.
    uint32_t residencyChanges = 0u;
    uint32_t cutChanges = 0u;
    uint32_t cutChangesThatMovedTheImage = 0u;
    uint32_t installsThatChangedTheCut = 0u;
    uint32_t unexplainedImageChanges = 0u;
    for (size_t index = 1u; index < streamProgress.size(); ++index)
    {
      const StreamFrame &previous = streamProgress[index - 1u];
      const StreamFrame &current = streamProgress[index];
      const bool residencyMoved = current.residentPages != previous.residentPages;
      const bool cutMoved = current.visibleClusters != previous.visibleClusters;
      const bool imageMoved = current.imageHash != previous.imageHash;

      residencyChanges += residencyMoved ? 1u : 0u;
      cutChanges += cutMoved ? 1u : 0u;
      installsThatChangedTheCut += (residencyMoved && cutMoved) ? 1u : 0u;
      if (cutMoved)
        cutChangesThatMovedTheImage += imageMoved ? 1u : 0u;
      else if (imageMoved && !residencyMoved)
        ++unexplainedImageChanges;
    }

    os::Logger::logf(
        "  page installs=%u, cut changes=%u (image moved on %u), installs that "
        "changed the cut=%u, unexplained image changes=%u",
        residencyChanges, cutChanges, cutChangesThatMovedTheImage,
        installsThatChangedTheCut, unexplainedImageChanges);

    check(residencyChanges > 2u, "several page installs happened during streaming");
    check(
        installsThatChangedTheCut > 2u,
        "page installs repeatedly changed the drawn cut, not just residency");
    // Not every cut change has to move the image: occlusion culling is off in
    // this test, so a page can add clusters that are selected, drawn, and then
    // lose the depth test behind terrain already on screen. What must hold is
    // that installs move the picture repeatedly rather than only at the end.
    check(
        cutChangesThatMovedTheImage * 2u >= cutChanges,
        "most changes to the drawn cut changed the rendered surface (" +
            std::to_string(cutChangesThatMovedTheImage) + "/" +
            std::to_string(cutChanges) + ")");
    check(
        cutChangesThatMovedTheImage > 3u,
        "the rendered surface changed on several separate page installs");
    check(
        unexplainedImageChanges == 0u,
        "the rendered surface never moves without residency or the cut moving");
    check(
        streamProgress.back().distinctSurfaces > streamProgress.front().distinctSurfaces * 2u,
        "streaming multiplies the distinct surfaces on screen (" +
            std::to_string(streamProgress.front().distinctSurfaces) + " -> " +
            std::to_string(streamProgress.back().distinctSurfaces) + ")");
    check(
        streamProgress.back().coveredPixels >= streamProgress.front().coveredPixels,
        "the streamed terrain never covers fewer pixels than the root page did");

    bool everyStreamFrameDrew = true;
    bool residencyOnlyGrows = true;
    for (size_t index = 0u; index < streamProgress.size(); ++index)
    {
      everyStreamFrameDrew =
          everyStreamFrameDrew && streamProgress[index].coveredPixels > 0u;
      if (index > 0u)
        residencyOnlyGrows =
            residencyOnlyGrows &&
            streamProgress[index].residentPages >= streamProgress[index - 1u].residentPages;
    }
    check(everyStreamFrameDrew, "no streaming frame leaves the terrain empty");
    check(residencyOnlyGrows, "terrain residency only ever grows while streaming in");
  }
  catch (const std::exception &exception)
  {
    os::Logger::errorf("EXCEPTION: %s", exception.what());
    exitCode = 1;
  }

  delete rhi;
  if (failures != 0 || exitCode != 0)
  {
    os::Logger::errorf("%d terrain virtual geometry end-to-end checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all terrain virtual geometry end-to-end checks passed");
  os::Logger::shutdown();
  return 0;
}
