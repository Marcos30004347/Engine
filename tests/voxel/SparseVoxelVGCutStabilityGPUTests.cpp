// Cut stability on the GPU, through the two-pass occlusion chain the sample
// uses.
//
// The other GPU suites drive a single culling pass and wait for every frame.
// The terrain route runs VirtualGeometryRendererPass with two-pass occlusion:
// phase one redraws last frame's cluster set, a depth pyramid is built from
// it, phase two draws what survives. That chain carries state across frames
// (the per-cluster drawn/last-frame bitset), and it is where the terrain
// blink lives. This suite asks the questions that state machine has to answer:
//
//   [settle]      a static camera reaches a fixed drawn set and a fixed image,
//                 and keeps them, frame after frame, with the per-frame scene
//                 calls the sample makes;
//   [lod]         every drawn cluster passes the projected-error cut test the
//                 shader runs, recomputed on the CPU from the encoded bounds -
//                 so the projected voxel of every drawn cluster sits inside
//                 one octave of the pixel budget or cannot refine;
//   [replace]     replacing a runtime object between frames - what a terrain
//                 chunk rebuild does - never leaves a frame where neither the
//                 old nor the new geometry draws;
//   [evict]       page-buffer pressure that evicts pages leaves the GPU
//                 hierarchy consistent with the page table, so an evicted page
//                 hands its surface back to the coarser clusters instead of
//                 opening a hole.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
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
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryIdReadbackPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryRendererPass.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

namespace svr = voxel::svraster;

namespace
{

constexpr uint32_t kImageSide = 256u;
constexpr uint32_t kMaxQueueElements = 1u << 20;
constexpr uint32_t kPageNotInstalledBit = 1u << 31u;
constexpr float kNear = 0.5f;
constexpr float kFar = 20000.0f;
constexpr float kErrorBudget = 1.0f;

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
  // A ridge, so the surface has enough relief to occlude itself.
  if (z > side / 3u && z < side / 3u + 4u)
    height += 5u + (x % 3u);
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

struct ChunkAsset
{
  VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  VirtualGeometryEncodedData encoded;
  uint32_t leafClusterCount = 0u;
  uint32_t maximumLevel = 0u;
  bool valid = false;
};

ChunkAsset buildChunkAsset(const uint32_t side, const uint32_t groupsPerPage)
{
  ChunkAsset asset{};
  svr::SparseVoxelBrickHierarchy hierarchy{};
  std::string error;
  if (!buildChunkOctree(side, hierarchy, &error))
  {
    os::Logger::errorf("voxel chunk build failed: %s", error.c_str());
    return asset;
  }
  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  octreeSettings.originOffset = {
      static_cast<double>(side) * 0.5, 0.0, static_cast<double>(side) * 0.5};
  VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = groupsPerPage;
  buildSettings.maxRootPageGroups = 1u;
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          hierarchy, 0u, static_cast<uint32_t>(hierarchy.nodes.size()),
          octreeSettings, buildSettings, asset.build, &asset.stats, &error))
  {
    os::Logger::errorf("octree virtual geometry build failed: %s", error.c_str());
    return asset;
  }
  QuantizationConfig quantization{};
  quantization.quantization_factor = 4u;
  quantization.unit_scale = 100.0f;
  quantization.padMeshlets = true;
  asset.encoded = VirtualGeometryCompressor::encode(asset.build, quantization);
  for (uint32_t cluster = 0u; cluster < asset.build.clusterDAG.size(); ++cluster)
    asset.leafClusterCount += asset.build.clusterDAG[cluster].empty() ? 1u : 0u;
  for (const auto &source : asset.stats.clusterSources)
    asset.maximumLevel = std::max(asset.maximumLevel, source.level);
  asset.valid = !asset.encoded.pages.empty() && !asset.encoded.hierarchy.empty();
  return asset;
}

// ── the CPU side of the shader's cut test ───────────────────────────────────

struct CameraState
{
  math::Mat4f view;
  math::Mat4f projection;
  math::Vec3f eye;
};

CameraState makeCamera(const math::Vec3f &eye, const math::Vec3f &target)
{
  Camera camera(60.0f * 3.14159265f / 180.0f, eye, math::Vec3f(0.0f, 0.0f, -1.0f), true);
  camera.setAspectRatio(1.0f);
  camera.setNearFar(kNear, kFar);
  camera.lookAt(target);
  camera.updateMatrices();
  return CameraState{camera.getViewMatrix(), camera.getProjectionMatrix(), camera.getPosition()};
}

// calculateProjectedError in culling_multipass.wgsl, identity model matrix.
float projectedError(
    const CameraState &camera,
    const float center[3],
    const float radius,
    const float error)
{
  const float dx = center[0] - camera.eye[0];
  const float dy = center[1] - camera.eye[1];
  const float dz = center[2] - camera.eye[2];
  const float distance = std::max(std::sqrt(dx * dx + dy * dy + dz * dz) - radius, kNear);
  // proj[1][1] in WGSL is column 1, row 1 of the column-major matrix.
  const float proj11 = camera.projection.data[5];
  return (error / distance) * proj11 * 0.5f * static_cast<float>(kImageSide);
}

struct FrameProbe
{
  uint32_t phaseOneClusters = 0u;
  uint32_t phaseTwoClusters = 0u;
  uint32_t phaseOneQueue = 0u;
  uint32_t phaseTwoQueue = 0u;
  std::set<uint64_t> drawn; // (pageIndex << 32) | pageLocalClusterIndex, both phases
  uint32_t duplicateEntries = 0u;
  uint32_t coveredPixels = 0u;
  uint64_t imageHash = 0u;
  // Hash of which pixels are covered, independent of the cluster ids that
  // won them: a replacement that lands in fresh cluster slots changes every
  // id while drawing the identical surface.
  uint64_t coverageHash = 0u;
  uint32_t lastFrameBits = 0u;
  uint32_t drawnBits = 0u;
  uint32_t installedPages = 0u;
};

uint64_t clusterKey(const VisibleClusterInfo &info)
{
  return (static_cast<uint64_t>(info.pageIndex) << 32u) | info.pageLocalClusterIndex;
}

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Sparse voxel virtual geometry cut stability (GPU, two-pass occlusion)");
  os::Logger::log("=====================================================================");

  constexpr uint32_t kSide = 64u;
  const std::string objectName = "voxel_chunk";

  ChunkAsset asset = buildChunkAsset(kSide, 8u);
  check(asset.valid, "the chunk builds a streamable virtual geometry asset");
  if (!asset.valid)
  {
    os::Logger::shutdown();
    return 1;
  }
  const uint32_t pageCount = static_cast<uint32_t>(asset.encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(asset.encoded.hierarchy.size());
  os::Logger::logf(
      "  chunk side=%u levels=%u clusters=%zu leaves=%u pages=%u hierarchyNodes=%u",
      kSide, asset.stats.levelCount, asset.build.clusters.size(), asset.leafClusterCount,
      pageCount, hierarchySize);

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
    // Room for several copies of the object: the replacement test installs
    // the new object before the old one's allocations are recycled.
    const uint32_t hierarchyBytes = std::max(
        hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 8u, 65536u);
    auto graphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *graph = graphStorage.get();
    VirtualGeometryScene scene(graph, hierarchyBytes, 256ull * 1024 * 1024);

    const auto installObject = [&](const std::string &name, const uint64_t generation) -> bool
    {
      VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
      install.name = name;
      install.asset = &asset.encoded;
      install.basePageGeneration = generation;
      install.geometryRevision = generation;
      install.materialRevision = 1u;
      install.position = math::Vec3f(0.0f, 0.0f, 0.0f);
      install.hasTracePayload = true;
      install.enqueuePageDecode = true;
      std::string installError;
      const bool installed = scene.installRuntimeEncodedObject(install, nullptr, &installError);
      if (!installed)
        os::Logger::errorf("install of %s failed: %s", name.c_str(), installError.c_str());
      scene.updateInstanceBuffer();
      return installed;
    };
    check(installObject(objectName, 1u), "the chunk installs as a runtime object");

    // ── passes: the sample's two-pass chain ─────────────────────────────────
    const auto makeColorTexture = [&](const char *name, const Format format)
    {
      return graph->createTexture(TextureInfo{
        .name = name,
        .format = format,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
        .width = kImageSide,
        .height = kImageSide,
        .mipLevels = 1u,
      });
    };
    const auto colorView = [](const Texture texture)
    {
      return TextureView{
        .texture = texture,
        .index = 0u,
        .flags = ImageAspectFlags::Color,
        .baseMipLevel = 0u,
        .levelCount = 1u,
        .baseArrayLayer = 0u,
        .layerCount = 1u,
        .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
        .layout = ResourceLayout::COLOR_ATTACHMENT,
      };
    };
    Texture depthTexture = graph->createTexture(TextureInfo{
      .name = "Cut_Depth.texture",
      .format = Format::Format_Depth32Float,
      .memoryProperties = BufferUsage::BufferUsage_None,
      .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = 1u,
    });
    const TextureView depthView{
      .texture = depthTexture,
      .index = 0u,
      .flags = ImageAspectFlags::Depth,
      .baseMipLevel = 0u,
      .levelCount = 1u,
      .baseArrayLayer = 0u,
      .layerCount = 1u,
      .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE |
          AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
      .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    };
    Texture idsLo = makeColorTexture("Cut_IdsLo.texture", Format::Format_R32Uint);
    Texture idsHi = makeColorTexture("Cut_IdsHi.texture", Format::Format_R32Uint);
    Texture materialId = makeColorTexture("Cut_MaterialId.texture", Format::Format_R32Uint);
    Texture materialUV = makeColorTexture("Cut_MaterialUV.texture", Format::Format_RG32Float);
    Texture motionVector = makeColorTexture("Cut_Motion.texture", Format::Format_RG16Float);
    Texture baseColor = makeColorTexture("Cut_BaseColor.texture", Format::Format_RGBA16Float);
    Texture materialProperties = makeColorTexture("Cut_MaterialProperties.texture", Format::Format_RGBA16Float);
    Texture emissive = makeColorTexture("Cut_Emissive.texture", Format::Format_RGBA16Float);
    Texture geometryNormal = makeColorTexture("Cut_GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture normal = makeColorTexture("Cut_Normal.texture", Format::Format_RGBA16Float);
    Texture color = makeColorTexture("Cut_Color.texture", Format::Format_RGBA16Float);

    uint32_t hzbMipCount = 1u;
    for (uint32_t dimension = kImageSide; dimension > 1u; dimension >>= 1u)
      ++hzbMipCount;
    hzbMipCount = std::min(hzbMipCount, 7u);
    Texture hzb = graph->createTexture(TextureInfo{
      .name = "Cut_HZB.texture",
      .format = Format::Format_R32Float,
      .memoryProperties = BufferUsage::BufferUsage_Storage,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
      .width = kImageSide,
      .height = kImageSide,
      .mipLevels = hzbMipCount,
    });

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
    VirtualGeometryDepthPrePassDrawPass::FrameTarget prePassTarget{
      .depthView = depthView,
      .depthTexture = depthTexture,
    };

    VirtualGeometryRendererPass::Settings settings{};
    settings.prepassCullingSettings.maxHierarchyLevels = 32u;
    settings.prepassCullingSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    settings.prepassCullingSettings.errorTreshhold = kErrorBudget;
    settings.prepassCullingSettings.viewPortWidth = kImageSide;
    settings.prepassCullingSettings.viewPortHeight = kImageSide;
    settings.prepassCullingSettings.hiZMipLevels = hzbMipCount;
    settings.prepassCullingSettings.useExternalHiZTexture = true;
    settings.prepassCullingSettings.enableFrustumCulling = true;
    settings.prepassCullingSettings.enableOcclusionCulling = true;
    settings.prepassCullingSettings.enableStreamingPriorityUpdates = false;
    settings.prepassCullingSettings.swTriangleEdgePixelThreshold = 0.0f;
    settings.finalCullingSettings = settings.prepassCullingSettings;
    settings.finalCullingSettings.enableStreamingPriorityUpdates = true;
    settings.depthPrePassSettings.viewPortWidth = kImageSide;
    settings.depthPrePassSettings.viewPortHeight = kImageSide;
    settings.depthPrePassSettings.cullMode = CullMode::None;
    settings.depthPrePassSettings.directDrawWhenIndirectCountUnavailable = true;
    settings.depthPyramidSettings.width = kImageSide;
    settings.depthPyramidSettings.height = kImageSide;
    settings.depthPyramidSettings.outputWidth = kImageSide;
    settings.depthPyramidSettings.outputHeight = kImageSide;
    settings.depthPyramidSettings.mipCount = hzbMipCount;
    settings.depthPyramidSettings.outputFormat = Format::Format_R32Float;
    settings.hardwareDrawSettings.viewPortWidth = kImageSide;
    settings.hardwareDrawSettings.viewPortHeight = kImageSide;
    settings.hardwareDrawSettings.colorFormat = Format::Format_RGBA16Float;
    settings.hardwareDrawSettings.motionVectorFormat = Format::Format_RG16Float;
    settings.hardwareDrawSettings.depthFormat = Format::Format_Depth32Float;
    settings.hardwareDrawSettings.cullMode = CullMode::None;
    settings.hardwareDrawSettings.directDrawWhenIndirectCountUnavailable = true;
    settings.computeRasterSettings.enabled = false;
    settings.twoPhaseOcclusion = true;
    settings.registerScreenSpaceRayTracePass = false;
    settings.registerMaterialPass = false;

    uint32_t passIndex = 0u;
    auto *renderer = graph->registerPass<VirtualGeometryRendererPass>(
        "cut.renderer", passIndex++, scene, frameTarget, prePassTarget, Texture{}, hzb, 0.0f, settings);
    passIndex += VirtualGeometryRendererPass::childPassCount(settings);

    const uint64_t visibleClusterBytes =
        static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
    const auto makeStaging = [&](const char *name, const uint64_t bytes)
    {
      return graph->createBuffer(BufferInfo{
        .name = name,
        .size = bytes,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
      });
    };
    Buffer phaseOneVisibleStaging = makeStaging("Cut_PhaseOneVisible.staging", visibleClusterBytes);
    Buffer phaseTwoVisibleStaging = makeStaging("Cut_PhaseTwoVisible.staging", visibleClusterBytes);
    Buffer phaseOneStatsStaging = makeStaging("Cut_PhaseOneStats.staging", 64u);
    Buffer phaseTwoStatsStaging = makeStaging("Cut_PhaseTwoStats.staging", 64u);
    Buffer visibilityStaging = makeStaging("Cut_Visibility.staging", scene.clusterVisibilityBufferSize);
    const uint64_t idBytes = static_cast<uint64_t>(kImageSide) * kImageSide * sizeof(uint32_t) * 2u;
    Buffer idBuffer = graph->createBuffer(BufferInfo{
      .name = "Cut_Ids.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    Buffer idStaging = makeStaging("Cut_Ids.staging", idBytes);

    auto *preCulling = renderer->getPreCullingPass();
    auto *finalCulling = renderer->getFinalCullingPass();
    auto *phaseOneDraw = renderer->getPhaseOneDrawPass();
    auto *hardwareDraw = renderer->getHardwareDrawPass();
    // The children are created when the parent records; the graph compiles
    // them below and the accessors are valid after that.

    graph->registerPass<VirtualGeometryIdReadbackPass>(
        "cut.idReadback", passIndex++, idsLo, idsHi, idBuffer, kImageSide, kImageSide);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyIds", passIndex++, idBuffer, 0, idBytes, idStaging, 0, idBytes);
    graph->compile();

    preCulling = renderer->getPreCullingPass();
    finalCulling = renderer->getFinalCullingPass();
    phaseOneDraw = renderer->getPhaseOneDrawPass();
    hardwareDraw = renderer->getHardwareDrawPass();
    check(preCulling != nullptr && finalCulling != nullptr && phaseOneDraw != nullptr && hardwareDraw != nullptr,
          "the two-pass renderer registered both culling passes and both draws");
    if (preCulling == nullptr || finalCulling == nullptr || phaseOneDraw == nullptr || hardwareDraw == nullptr)
      throw std::runtime_error("renderer children missing");

    // The copies of the culling outputs have to be graph passes; they are
    // registered after compile so they can name the child passes' buffers, so
    // recompile once.
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyPhaseOneVisible", passIndex++, preCulling->getHWVisibleClusterInfosBuffer(), 0,
        visibleClusterBytes, phaseOneVisibleStaging, 0, visibleClusterBytes);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyPhaseTwoVisible", passIndex++, finalCulling->getHWVisibleClusterInfosBuffer(), 0,
        visibleClusterBytes, phaseTwoVisibleStaging, 0, visibleClusterBytes);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyPhaseOneStats", passIndex++, preCulling->getCullingStatisticsBuffer(), 0,
        64u, phaseOneStatsStaging, 0, 64u);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyPhaseTwoStats", passIndex++, finalCulling->getCullingStatisticsBuffer(), 0,
        64u, phaseTwoStatsStaging, 0, 64u);
    graph->registerPass<rendering::gpgpu::CopyBufferPass>(
        "cut.copyVisibility", passIndex++, scene.clusterVisibilityBuffer, 0,
        scene.clusterVisibilityBufferSize, visibilityStaging, 0, scene.clusterVisibilityBufferSize);
    graph->compile();

    const math::Vec3f chunkCenter(0.0f, 8.0f, 0.0f);
    uint32_t frameSlot = 0u;
    uint64_t frameNumber = 0u;

    const auto runFrame = [&](const CameraState &camera) -> FrameProbe
    {
      ++frameNumber;
      // The per-frame scene calls the sample makes.
      scene.setRuntimeStreamingFrame(frameNumber);
      scene.touchRuntimePagesFromGPUPriorities();

      const float *viewArray = camera.view.data;
      const float *projectionArray = camera.projection.data;
      float viewPosition[4] = {camera.eye[0], camera.eye[1], camera.eye[2], 1.0f};
      preCulling->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount);
      finalCulling->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount);
      phaseOneDraw->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount, kErrorBudget);
      hardwareDraw->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount, kErrorBudget);

      scene.prepareFrame(frameSlot);
      RenderGraph::Overrides overrides;
      renderer->appendFrameOverrides(overrides, frameSlot);
      RenderGraph::Frame frame;
      graph->run(frame, overrides);
      graph->waitFrame(frame);
      frameSlot = (frameSlot + 1u) % graph->getMaxFramesInFlight();

      FrameProbe probe{};
      CullingCounters phaseOne{};
      CullingCounters phaseTwo{};
      graph->bufferRead(phaseOneStatsStaging, 0, sizeof(CullingCounters),
                        [&](const void *data) { std::memcpy(&phaseOne, data, sizeof(CullingCounters)); });
      graph->bufferRead(phaseTwoStatsStaging, 0, sizeof(CullingCounters),
                        [&](const void *data) { std::memcpy(&phaseTwo, data, sizeof(CullingCounters)); });
      probe.phaseOneClusters = phaseOne.visibleClusterHardwareCount;
      probe.phaseTwoClusters = phaseTwo.visibleClusterHardwareCount;
      probe.phaseOneQueue = phaseOne.clusterQueueSize;
      probe.phaseTwoQueue = phaseTwo.clusterQueueSize;

      const auto readVisible = [&](const Buffer &staging, const uint32_t count)
      {
        if (count == 0u || count > VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
          return;
        std::vector<VisibleClusterInfo> visible(count);
        graph->bufferRead(staging, 0, count * sizeof(VisibleClusterInfo),
                          [&](const void *data) { std::memcpy(visible.data(), data, count * sizeof(VisibleClusterInfo)); });
        for (const auto &info : visible)
          if (!probe.drawn.insert(clusterKey(info)).second)
            ++probe.duplicateEntries;
      };
      readVisible(phaseOneVisibleStaging, probe.phaseOneClusters);
      readVisible(phaseTwoVisibleStaging, probe.phaseTwoClusters);

      std::vector<uint32_t> packed(kImageSide * kImageSide * 2u, 0u);
      graph->bufferRead(idStaging, 0, idBytes, [&](const void *data) { std::memcpy(packed.data(), data, idBytes); });
      uint64_t hash = 1469598103934665603ull;
      uint64_t coverageHash = 1469598103934665603ull;
      for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
      {
        const uint32_t id = packed[pixel * 2u + 1u];
        hash = (hash ^ id) * 1099511628211ull;
        coverageHash = (coverageHash ^ (id != 0u ? 1u : 0u)) * 1099511628211ull;
        probe.coveredPixels += id != 0u ? 1u : 0u;
      }
      probe.imageHash = hash;
      probe.coverageHash = coverageHash;

      const uint64_t visibilityWords = scene.clusterVisibilityBufferSize / sizeof(uint32_t);
      std::vector<uint32_t> bits(visibilityWords, 0u);
      graph->bufferRead(visibilityStaging, 0, scene.clusterVisibilityBufferSize,
                        [&](const void *data) { std::memcpy(bits.data(), data, scene.clusterVisibilityBufferSize); });
      for (uint64_t word = 0u; word + 1u < visibilityWords; ++word)
      {
        probe.lastFrameBits += static_cast<uint32_t>(__builtin_popcount(bits[word] & 0x55555555u));
        probe.drawnBits += static_cast<uint32_t>(__builtin_popcount(bits[word] & 0xAAAAAAAAu));
      }

      std::vector<PageTableEntry> pageTable(scene.nextPageTableSlot);
      if (!pageTable.empty())
        graph->bufferRead(scene.pageTableBuffer, 0, pageTable.size() * sizeof(PageTableEntry),
                          [&](const void *data) { std::memcpy(pageTable.data(), data, pageTable.size() * sizeof(PageTableEntry)); });
      for (const auto &entry : pageTable)
        probe.installedPages += entry.isInstalled != 0u ? 1u : 0u;
      return probe;
    };

    const auto describe = [](const FrameProbe &probe)
    {
      std::ostringstream text;
      text << "p1=" << probe.phaseOneClusters << " p2=" << probe.phaseTwoClusters
           << " drawn=" << probe.drawn.size() << " px=" << probe.coveredPixels
           << " last=" << probe.lastFrameBits << " drawnBits=" << probe.drawnBits
           << " q1=" << probe.phaseOneQueue << " q2=" << probe.phaseTwoQueue;
      return text.str();
    };

    // ── [settle] ────────────────────────────────────────────────────────────
    os::Logger::log("[settle] a static camera converges on one drawn set and keeps it");
    const CameraState farCamera = makeCamera(math::Vec3f(0.0f, 30.0f, 70.0f), chunkCenter);
    std::vector<FrameProbe> settle;
    constexpr uint32_t kSettleFrames = 24u;
    for (uint32_t frame = 0u; frame < kSettleFrames; ++frame)
      settle.push_back(runFrame(farCamera));
    for (uint32_t frame = 0u; frame < 4u; ++frame)
      os::Logger::logf("  frame %u: %s", frame, describe(settle[frame]).c_str());
    os::Logger::logf("  frame %u: %s", kSettleFrames - 1u, describe(settle.back()).c_str());

    check(settle[0].phaseOneClusters == 0u && settle[0].phaseTwoClusters > 0u,
          "the first frame draws everything in phase two: nothing was visible before it");
    check(settle[1].phaseOneClusters > 0u,
          "the second frame redraws the previous frame's set in phase one");
    const FrameProbe &reference = settle[2];
    uint32_t unstableFrames = 0u;
    uint32_t phaseTwoRedraws = 0u;
    uint32_t imageChanges = 0u;
    uint32_t emptyPhaseOne = 0u;
    for (uint32_t frame = 2u; frame < kSettleFrames; ++frame)
    {
      const FrameProbe &probe = settle[frame];
      if (probe.drawn != reference.drawn)
        ++unstableFrames;
      if (probe.phaseTwoClusters != 0u)
        ++phaseTwoRedraws;
      if (probe.imageHash != reference.imageHash)
        ++imageChanges;
      if (probe.phaseOneClusters == 0u)
        ++emptyPhaseOne;
    }
    check(reference.drawn.size() > 0u && reference.coveredPixels > 0u,
          "the settled frame draws clusters and covers pixels");
    check(unstableFrames == 0u,
          "the drawn cluster set is identical on every frame after settling (" +
              std::to_string(unstableFrames) + " unstable frames)");
    check(phaseTwoRedraws == 0u,
          "once settled phase two never re-emits a cluster phase one already drew (" +
              std::to_string(phaseTwoRedraws) + " frames re-emitted)");
    check(emptyPhaseOne == 0u,
          "phase one never loses the whole last-frame set (" +
              std::to_string(emptyPhaseOne) + " frames with an empty phase one)");
    check(imageChanges == 0u,
          "the rendered image is bit-identical on every settled frame (" +
              std::to_string(imageChanges) + " frames differed)");
    check(reference.drawn.size() <= asset.leafClusterCount,
          "the settled cut never exceeds the leaf count: no level is drawn twice");
    uint32_t duplicateFrames = 0u;
    for (const auto &probe : settle)
      duplicateFrames += probe.duplicateEntries != 0u ? 1u : 0u;
    check(duplicateFrames == 0u, "no frame emits the same cluster from both phases");

    // ── [lod] ───────────────────────────────────────────────────────────────
    //
    // The shader draws a cluster when projSelf < budget <= projParent. For the
    // voxel DAG the self error of an interior cluster is its own voxel and the
    // parent error is twice that, so every drawn cluster's voxel projects to
    // [budget/2, budget) pixels unless it is a leaf (no upper bound: nothing
    // finer exists) or a root (no lower bound: nothing coarser exists). That is
    // the "constant triangle size" contract, one octave wide.
    os::Logger::log("[lod] every drawn cluster passes the projected-error cut test");
    finalCulling->settings.enableOcclusionCulling = false;
    const std::array<float, 5> distances{600.0f, 200.0f, 70.0f, 30.0f, 14.0f};
    uint32_t lodViolations = 0u;
    uint32_t octaveViolations = 0u;
    uint32_t sweepClusters = 0u;
    std::ostringstream sweepText;
    for (const float distance : distances)
    {
      const CameraState camera =
          makeCamera(math::Vec3f(0.0f, distance * 0.35f + 8.0f, distance), chunkCenter);
      FrameProbe probe{};
      for (uint32_t frame = 0u; frame < 3u; ++frame)
        probe = runFrame(camera);
      std::map<uint32_t, uint32_t> perLevel;
      for (const uint64_t key : probe.drawn)
      {
        const uint32_t page = static_cast<uint32_t>(key >> 32u);
        const uint32_t local = static_cast<uint32_t>(key & 0xffffffffu);
        if (page >= asset.build.pages.size() || local >= asset.build.pages[page].clusterCount)
        {
          ++lodViolations;
          continue;
        }
        const uint32_t cluster = asset.build.pages[page].clusterOffset + local;
        const VirtualGeometryCluster &data = asset.build.clusters[cluster];
        const float projSelf = projectedError(camera, data.self.center, data.self.radius, data.self.error);
        const bool root = data.parent.error == std::numeric_limits<float>::max();
        const float projParent = root ? std::numeric_limits<float>::max()
                                      : projectedError(camera, data.parent.center, data.parent.radius, data.parent.error);
        if (!(projSelf < kErrorBudget) || !(projParent >= kErrorBudget))
          ++lodViolations;
        const auto &source = asset.stats.clusterSources[cluster];
        ++perLevel[source.level];
        const bool leaf = data.self.error == 0.0f;
        const float voxel = std::ldexp(1.0f, static_cast<int>(source.level));
        const float projVoxel = projectedError(camera, data.self.center, data.self.radius, voxel);
        if (!leaf && !(projVoxel < kErrorBudget))
          ++octaveViolations;
        if (!root && !(projVoxel >= kErrorBudget * 0.5f * 0.999f))
          ++octaveViolations;
        ++sweepClusters;
      }
      sweepText << "\n    d=" << distance << " drawn=" << probe.drawn.size() << " px=" << probe.coveredPixels << " byLevel:";
      for (const auto &[level, count] : perLevel)
        sweepText << " L" << level << "=" << count;
    }
    os::Logger::logf("  sweep:%s", sweepText.str().c_str());
    check(sweepClusters > 0u, "the sweep drew clusters to test");
    check(lodViolations == 0u,
          "every drawn cluster satisfies projSelf < budget <= projParent (" +
              std::to_string(lodViolations) + " violations)");
    check(octaveViolations == 0u,
          "every drawn cluster's voxel projects inside one octave of the budget, or cannot refine (" +
              std::to_string(octaveViolations) + " violations)");
    finalCulling->settings.enableOcclusionCulling = true;

    // ── [replace] ───────────────────────────────────────────────────────────
    //
    // A terrain chunk rebuild is unregister + install between two frames. The
    // frame that follows must draw the replacement: the old clusters are gone
    // from the last-frame set, so phase two has to admit the new ones the
    // same frame, or the chunk is missing for a frame.
    os::Logger::log("[replace] replacing the object between frames never leaves a frame without it");
    std::vector<FrameProbe> before;
    for (uint32_t frame = 0u; frame < 4u; ++frame)
      before.push_back(runFrame(farCamera));
    const FrameProbe steady = before.back();
    os::Logger::logf("  steady: %s", describe(steady).c_str());

    uint32_t replacementHoles = 0u;
    uint32_t replacementImageMismatch = 0u;
    uint32_t replacementsMissingQueue = 0u;
    std::ostringstream replaceText;
    for (uint32_t replacement = 0u; replacement < 6u; ++replacement)
    {
      std::string unregisterError;
      check(scene.unregisterRuntimeObject(objectName, &unregisterError),
            "the object unregisters: " + unregisterError);
      check(installObject(objectName, 10u + replacement * 100u), "the replacement object installs");
      const FrameProbe first = runFrame(farCamera);
      const FrameProbe second = runFrame(farCamera);
      const FrameProbe third = runFrame(farCamera);
      replaceText << "\n    replacement " << replacement << ": frame+0 " << describe(first)
                  << " | frame+1 " << describe(second) << " | frame+2 " << describe(third);
      // The replacement carries the same geometry, so the image must not move
      // and the drawn count must not dip.
      if (first.coveredPixels < steady.coveredPixels)
        ++replacementHoles;
      if (first.imageHash != steady.imageHash)
        ++replacementImageMismatch;
      if (first.phaseOneQueue + first.phaseTwoQueue < steady.phaseOneQueue + steady.phaseTwoQueue)
        ++replacementsMissingQueue;
      if (second.coveredPixels < steady.coveredPixels || third.coveredPixels < steady.coveredPixels)
        ++replacementHoles;
    }
    os::Logger::logf("  replacements:%s", replaceText.str().c_str());
    check(replacementsMissingQueue == 0u,
          "the culler traverses the replacement on the frame it is installed (" +
              std::to_string(replacementsMissingQueue) + " replacements traversed fewer clusters)");
    check(replacementHoles == 0u,
          "no replacement frame covers fewer pixels than the steady frame (" +
              std::to_string(replacementHoles) + " holes)");
    check(replacementImageMismatch == 0u,
          "a replacement with identical geometry renders the identical image (" +
              std::to_string(replacementImageMismatch) + " mismatches)");

    // The scene must have recycled everything: the replacement loop installs
    // seven copies of the object in total, and the page table only ever holds
    // one of them.
    {
      FrameProbe probe = runFrame(farCamera);
      check(probe.installedPages == pageCount,
            "after six replacements exactly one copy of the object is resident (" +
                std::to_string(probe.installedPages) + " pages installed for " +
                std::to_string(pageCount) + ")");
    }

    // ── [replace-deferred] ──────────────────────────────────────────────────
    //
    // The sample renders with frames in flight, so the scene cannot release a
    // replaced object's GPU resources the moment it is unregistered: the frame
    // still executing would lose its page table entries and hierarchy. With a
    // release delay the old object stays resident on the GPU for that many
    // streaming frames while the new one draws from fresh ranges.
    os::Logger::log("[replace-deferred] a release delay keeps the old object's GPU state until frames retire");
    {
      constexpr uint32_t kDelay = 2u;
      scene.setRuntimeReleaseDelayFrames(kDelay);
      const FrameProbe base = runFrame(farCamera);
      uint32_t deferredHoles = 0u;
      uint32_t deferredMismatches = 0u;
      uint32_t framesWithBothResident = 0u;
      uint32_t framesReleasedEarly = 0u;
      std::ostringstream deferredText;
      for (uint32_t replacement = 0u; replacement < 4u; ++replacement)
      {
        std::string unregisterError;
        check(scene.unregisterRuntimeObject(objectName, &unregisterError),
              "the object unregisters under a release delay: " + unregisterError);
        check(installObject(objectName, 1000u + replacement * 100u),
              "the replacement installs into fresh ranges while the old ranges are held");
        check(scene.pendingDeferredRuntimeReleaseCount() > 0u,
              "the old object's resources are queued for deferred release");
        std::vector<FrameProbe> frames;
        for (uint32_t frame = 0u; frame < kDelay + 2u; ++frame)
          frames.push_back(runFrame(farCamera));
        deferredText << "\n    replacement " << replacement << ":";
        for (uint32_t frame = 0u; frame < frames.size(); ++frame)
        {
          const FrameProbe &probe = frames[frame];
          deferredText << " [f+" << frame << " pages=" << probe.installedPages
                       << " drawn=" << probe.drawn.size() << " px=" << probe.coveredPixels << "]";
          if (probe.coveredPixels < base.coveredPixels)
            ++deferredHoles;
          if (probe.coverageHash != base.coverageHash)
            ++deferredMismatches;
          // runFrame advances the streaming frame before recording, so the
          // release lands on the kDelay-th frame after the unregister.
          if (frame + 1u < kDelay && probe.installedPages == 2u * pageCount)
            ++framesWithBothResident;
          if (frame + 1u < kDelay && probe.installedPages < 2u * pageCount)
            ++framesReleasedEarly;
        }
      }
      os::Logger::logf("  deferred replacements:%s", deferredText.str().c_str());
      check(framesWithBothResident > 0u,
            "while the release is pending both the old and the new object stay resident in the page table");
      check(framesReleasedEarly == 0u,
            "the old object's page table entries are never cleared before the release delay elapses (" +
                std::to_string(framesReleasedEarly) + " early)");
      check(deferredHoles == 0u,
            "no frame around a deferred replacement covers fewer pixels (" +
                std::to_string(deferredHoles) + " holes)");
      check(deferredMismatches == 0u,
            "every frame around a deferred replacement covers the identical pixels (" +
                std::to_string(deferredMismatches) + " mismatches)");
      const FrameProbe drained = runFrame(farCamera);
      check(drained.installedPages == pageCount && scene.pendingDeferredRuntimeReleaseCount() == 0u,
            "after the delay only the new object is resident and nothing awaits release (" +
                std::to_string(drained.installedPages) + " pages, " +
                std::to_string(scene.pendingDeferredRuntimeReleaseCount()) + " pending)");
      scene.setRuntimeReleaseDelayFrames(0u);
    }

    // ── [pipelined] ─────────────────────────────────────────────────────────
    //
    // The sample keeps two frames in flight and per-slot instance and uniform
    // buffers. Replace the object while the previous frame is still on the
    // GPU, record the next frame without waiting, and read what that frame's
    // culler saw: the replacement must be traversed and drawn in the frame
    // that installed it, and the frame in flight must keep the old one.
    os::Logger::log("[pipelined] replacing while a frame is in flight, with per-slot buffers");
    {
      auto pipelinedGraphStorage = std::make_unique<RenderGraph>(
          rhi, RenderGraph::Settings{.maxFramesInFlight = 2u});
      RenderGraph *pipelinedGraph = pipelinedGraphStorage.get();
      VirtualGeometryScene pipelined(pipelinedGraph, hierarchyBytes, 256ull * 1024 * 1024);
      pipelined.setRuntimeReleaseDelayFrames(2u);
      const auto installPipelined = [&](const uint64_t generation)
      {
        VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
        install.name = objectName;
        install.asset = &asset.encoded;
        install.basePageGeneration = generation;
        install.geometryRevision = generation;
        install.materialRevision = 1u;
        install.hasTracePayload = true;
        install.enqueuePageDecode = true;
        std::string installError;
        const bool installed = pipelined.installRuntimeEncodedObject(install, nullptr, &installError);
        if (!installed)
          os::Logger::errorf("pipelined install failed: %s", installError.c_str());
        pipelined.updateInstanceBuffer();
        return installed;
      };
      check(installPipelined(1u), "the object installs into the two-frame scene");

      Texture pDepth = pipelinedGraph->createTexture(TextureInfo{
        .name = "Pipe_Depth.texture", .format = Format::Format_Depth32Float,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
        .width = kImageSide, .height = kImageSide, .mipLevels = 1u});
      const auto pColor = [&](const char *name, const Format format)
      {
        return pipelinedGraph->createTexture(TextureInfo{
          .name = name, .format = format, .memoryProperties = BufferUsage::BufferUsage_None,
          .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
          .width = kImageSide, .height = kImageSide, .mipLevels = 1u});
      };
      Texture pIdsLo = pColor("Pipe_IdsLo.texture", Format::Format_R32Uint);
      Texture pIdsHi = pColor("Pipe_IdsHi.texture", Format::Format_R32Uint);
      Texture pMaterialId = pColor("Pipe_MaterialId.texture", Format::Format_R32Uint);
      Texture pMaterialUV = pColor("Pipe_MaterialUV.texture", Format::Format_RG32Float);
      Texture pMotion = pColor("Pipe_Motion.texture", Format::Format_RG16Float);
      Texture pBaseColor = pColor("Pipe_BaseColor.texture", Format::Format_RGBA16Float);
      Texture pMaterialProperties = pColor("Pipe_MaterialProperties.texture", Format::Format_RGBA16Float);
      Texture pEmissive = pColor("Pipe_Emissive.texture", Format::Format_RGBA16Float);
      Texture pGeometryNormal = pColor("Pipe_GeometryNormal.texture", Format::Format_RGBA16Float);
      Texture pNormal = pColor("Pipe_Normal.texture", Format::Format_RGBA16Float);
      Texture pColorTex = pColor("Pipe_Color.texture", Format::Format_RGBA16Float);
      Texture pHzb = pipelinedGraph->createTexture(TextureInfo{
        .name = "Pipe_HZB.texture", .format = Format::Format_R32Float,
        .memoryProperties = BufferUsage::BufferUsage_Storage,
        .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
        .width = kImageSide, .height = kImageSide, .mipLevels = hzbMipCount});
      const TextureView pDepthView{
        .texture = pDepth, .index = 0u, .flags = ImageAspectFlags::Depth,
        .baseMipLevel = 0u, .levelCount = 1u, .baseArrayLayer = 0u, .layerCount = 1u,
        .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
        .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT};
      VirtualGeometryHardwareDrawPass::FrameTarget pTarget{
        .packedGeometryIdsLoView = colorView(pIdsLo), .packedGeometryIdsHiView = colorView(pIdsHi),
        .materialIdView = colorView(pMaterialId), .materialUVView = colorView(pMaterialUV),
        .baseColorView = colorView(pBaseColor), .materialPropertiesView = colorView(pMaterialProperties),
        .emissiveView = colorView(pEmissive), .geometryNormalView = colorView(pGeometryNormal),
        .normalView = colorView(pNormal), .motionVectorView = colorView(pMotion),
        .depthView = pDepthView, .depthTexture = pDepth, .baseColorTexture = pBaseColor,
        .materialPropertiesTexture = pMaterialProperties, .emissiveTexture = pEmissive,
        .geometryNormalTexture = pGeometryNormal, .normalTexture = pNormal, .motionVectorTexture = pMotion,
        .colorView = colorView(pColorTex), .colorTexture = pColorTex};
      VirtualGeometryDepthPrePassDrawPass::FrameTarget pPrePassTarget{.depthView = pDepthView, .depthTexture = pDepth};
      uint32_t pPassIndex = 0u;
      auto *pRenderer = pipelinedGraph->registerPass<VirtualGeometryRendererPass>(
          "pipe.renderer", pPassIndex++, pipelined, pTarget, pPrePassTarget, Texture{}, pHzb, 0.0f, settings);
      pPassIndex += VirtualGeometryRendererPass::childPassCount(settings);
      Buffer pIdBuffer = pipelinedGraph->createBuffer(BufferInfo{
        .name = "Pipe_Ids.buffer", .size = idBytes,
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc});
      // One staging copy per frame slot, so both in-flight frames keep their
      // own readback.
      std::array<Buffer, 2> pIdStaging{};
      std::array<Buffer, 2> pStatsStaging{};
      std::array<Buffer, 2> pPreStatsStaging{};
      for (uint32_t slot = 0u; slot < 2u; ++slot)
      {
        pIdStaging[slot] = pipelinedGraph->createBuffer(BufferInfo{
          .name = "Pipe_Ids.staging" + std::to_string(slot), .size = idBytes,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        pStatsStaging[slot] = pipelinedGraph->createBuffer(BufferInfo{
          .name = "Pipe_Stats.staging" + std::to_string(slot), .size = 64u,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        pPreStatsStaging[slot] = pipelinedGraph->createBuffer(BufferInfo{
          .name = "Pipe_PreStats.staging" + std::to_string(slot), .size = 64u,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
      }
      pipelinedGraph->registerPass<VirtualGeometryIdReadbackPass>(
          "pipe.idReadback", pPassIndex++, pIdsLo, pIdsHi, pIdBuffer, kImageSide, kImageSide);
      // The graph resolves the copy destination through a frame override
      // supplied per run, so the same pass serves both slots.
      Buffer pIdStagingVirtual = pipelinedGraph->createBuffer(BufferInfo{
        .name = "Pipe_Ids.stagingVirtual", .size = idBytes,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
      Buffer pStatsStagingVirtual = pipelinedGraph->createBuffer(BufferInfo{
        .name = "Pipe_Stats.stagingVirtual", .size = 64u,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
      Buffer pPreStatsStagingVirtual = pipelinedGraph->createBuffer(BufferInfo{
        .name = "Pipe_PreStats.stagingVirtual", .size = 64u,
        .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
      pipelinedGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "pipe.copyIds", pPassIndex++, pIdBuffer, 0, idBytes, pIdStagingVirtual, 0, idBytes);
      auto *pPre = pRenderer->getPreCullingPass();
      auto *pFinal = pRenderer->getFinalCullingPass();
      auto *pPhaseOne = pRenderer->getPhaseOneDrawPass();
      auto *pDraw = pRenderer->getHardwareDrawPass();
      pipelinedGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "pipe.copyStats", pPassIndex++, pFinal->getCullingStatisticsBuffer(), 0, 64u, pStatsStagingVirtual, 0, 64u);
      pipelinedGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "pipe.copyPreStats", pPassIndex++, pPre->getCullingStatisticsBuffer(), 0, 64u, pPreStatsStagingVirtual, 0, 64u);
      pipelinedGraph->compile();

      uint64_t pFrameNumber = 0u;
      struct PipelinedFrame
      {
        RenderGraph::Frame frame{};
        uint32_t slot = 0u;
      };
      const auto recordPipelined = [&](const uint32_t slot) -> PipelinedFrame
      {
        ++pFrameNumber;
        pipelined.setRuntimeStreamingFrame(pFrameNumber);
        pipelinedGraph->setCurrentFrameIndex(slot);
        const float *viewArray = farCamera.view.data;
        const float *projectionArray = farCamera.projection.data;
        float viewPosition[4] = {farCamera.eye[0], farCamera.eye[1], farCamera.eye[2], 1.0f};
        pPre->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount);
        pFinal->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount);
        pPhaseOne->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount, kErrorBudget);
        pDraw->updateUniforms(viewArray, projectionArray, viewPosition, kImageSide, kImageSide, kNear, kFar, hzbMipCount, kErrorBudget);
        pipelined.prepareFrame(slot);
        RenderGraph::Overrides overrides;
        pRenderer->appendFrameOverrides(overrides, slot);
        overrides.bufferOverrides.emplace(pIdStagingVirtual.name, RenderGraphBufferOverride{.bufferId = pipelinedGraph->resolveBufferId(pIdStaging[slot])});
        overrides.bufferOverrides.emplace(pStatsStagingVirtual.name, RenderGraphBufferOverride{.bufferId = pipelinedGraph->resolveBufferId(pStatsStaging[slot])});
        overrides.bufferOverrides.emplace(pPreStatsStagingVirtual.name, RenderGraphBufferOverride{.bufferId = pipelinedGraph->resolveBufferId(pPreStatsStaging[slot])});
        PipelinedFrame result{};
        result.slot = slot;
        result.frame.frameIndex = slot;
        pipelinedGraph->run(result.frame, overrides);
        return result;
      };
      struct PipelinedProbe
      {
        uint32_t phaseOne = 0u;
        uint32_t phaseTwo = 0u;
        uint32_t queue = 0u;
        uint32_t coveredPixels = 0u;
      };
      const auto readPipelined = [&](PipelinedFrame &frame) -> PipelinedProbe
      {
        pipelinedGraph->waitFrame(frame.frame);
        PipelinedProbe probe{};
        CullingCounters phaseOne{};
        CullingCounters phaseTwo{};
        pipelinedGraph->bufferRead(pPreStatsStaging[frame.slot], 0, sizeof(CullingCounters),
                                   [&](const void *data) { std::memcpy(&phaseOne, data, sizeof(CullingCounters)); });
        pipelinedGraph->bufferRead(pStatsStaging[frame.slot], 0, sizeof(CullingCounters),
                                   [&](const void *data) { std::memcpy(&phaseTwo, data, sizeof(CullingCounters)); });
        probe.phaseOne = phaseOne.visibleClusterHardwareCount;
        probe.phaseTwo = phaseTwo.visibleClusterHardwareCount;
        probe.queue = phaseTwo.clusterQueueSize;
        std::vector<uint32_t> packed(kImageSide * kImageSide * 2u, 0u);
        pipelinedGraph->bufferRead(pIdStaging[frame.slot], 0, idBytes,
                                   [&](const void *data) { std::memcpy(packed.data(), data, idBytes); });
        for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
          probe.coveredPixels += packed[pixel * 2u + 1u] != 0u ? 1u : 0u;
        return probe;
      };

      // Settle with waits.
      PipelinedProbe baseProbe{};
      for (uint32_t frame = 0u; frame < 4u; ++frame)
      {
        PipelinedFrame f = recordPipelined(frame % 2u);
        baseProbe = readPipelined(f);
      }
      os::Logger::logf("  pipelined steady: p1=%u p2=%u queue=%u px=%u",
                       baseProbe.phaseOne, baseProbe.phaseTwo, baseProbe.queue, baseProbe.coveredPixels);

      uint32_t pipelinedHoles = 0u;
      uint32_t pipelinedQueueLoss = 0u;
      uint32_t inFlightHoles = 0u;
      std::ostringstream pipelinedText;
      for (uint32_t replacement = 0u; replacement < 4u; ++replacement)
      {
        // Frame A is submitted and left in flight; the replacement happens
        // while it may still be executing; frame B is recorded after it.
        PipelinedFrame frameA = recordPipelined(0u);
        std::string unregisterError;
        check(pipelined.unregisterRuntimeObject(objectName, &unregisterError),
              "the object unregisters while a frame is in flight: " + unregisterError);
        check(installPipelined(100u + replacement), "the replacement installs while a frame is in flight");
        PipelinedFrame frameB = recordPipelined(1u);
        const PipelinedProbe probeA = readPipelined(frameA);
        const PipelinedProbe probeB = readPipelined(frameB);
        PipelinedFrame frameC = recordPipelined(0u);
        const PipelinedProbe probeC = readPipelined(frameC);
        pipelinedText << "\n    replacement " << replacement
                      << ": inFlight[p1=" << probeA.phaseOne << " p2=" << probeA.phaseTwo << " q=" << probeA.queue << " px=" << probeA.coveredPixels << "]"
                      << " install[p1=" << probeB.phaseOne << " p2=" << probeB.phaseTwo << " q=" << probeB.queue << " px=" << probeB.coveredPixels << "]"
                      << " next[p1=" << probeC.phaseOne << " p2=" << probeC.phaseTwo << " q=" << probeC.queue << " px=" << probeC.coveredPixels << "]";
        if (probeA.coveredPixels < baseProbe.coveredPixels)
          ++inFlightHoles;
        if (probeB.coveredPixels < baseProbe.coveredPixels || probeC.coveredPixels < baseProbe.coveredPixels)
          ++pipelinedHoles;
        if (probeB.queue < baseProbe.queue)
          ++pipelinedQueueLoss;
      }
      os::Logger::logf("  pipelined replacements:%s", pipelinedText.str().c_str());

      // The sample's shape of a replacement: several objects resident, one
      // replaced (its dense slot refilled by the last object) and a new one
      // appended in the same iteration, so the instance count grows and the
      // packed array is reordered while a frame is in flight.
      {
        const auto installNamed = [&](const std::string &name, const uint64_t generation, const math::Vec3f &position)
        {
          VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
          install.name = name;
          install.asset = &asset.encoded;
          install.basePageGeneration = generation;
          install.geometryRevision = generation;
          install.materialRevision = 1u;
          install.position = position;
          install.hasTracePayload = true;
          install.enqueuePageDecode = true;
          std::string installError;
          const bool installed = pipelined.installRuntimeEncodedObject(install, nullptr, &installError);
          if (!installed)
            os::Logger::errorf("pipelined install of %s failed: %s", name.c_str(), installError.c_str());
          pipelined.updateInstanceBuffer();
          return installed;
        };
        check(installNamed("chunk_b", 5000u, math::Vec3f(0.0f, 12.0f, 0.0f)), "a second object installs above the first");
        PipelinedProbe grownBase{};
        for (uint32_t frame = 0u; frame < 4u; ++frame)
        {
          PipelinedFrame f = recordPipelined(frame % 2u);
          grownBase = readPipelined(f);
        }
        os::Logger::logf("  two-object steady: p1=%u p2=%u queue=%u px=%u",
                         grownBase.phaseOne, grownBase.phaseTwo, grownBase.queue, grownBase.coveredPixels);
        uint32_t growQueueLoss = 0u;
        uint32_t growHoles = 0u;
        std::ostringstream growText;
        uint32_t expectedQueue = grownBase.queue;
        for (uint32_t replacement = 0u; replacement < 4u; ++replacement)
        {
          PipelinedFrame frameA = recordPipelined(0u);
          std::string unregisterError;
          check(pipelined.unregisterRuntimeObject(objectName, &unregisterError),
                "the first object unregisters with a frame in flight: " + unregisterError);
          check(installNamed(objectName, 6000u + replacement, math::Vec3f(0.0f, 0.0f, 0.0f)),
                "the first object reinstalls behind the second");
          check(installNamed("extra_" + std::to_string(replacement), 7000u + replacement,
                             math::Vec3f(0.0f, 6.0f + 3.0f * static_cast<float>(replacement), 0.0f)),
                "an additional object installs in the same iteration");
          PipelinedFrame frameB = recordPipelined(1u);
          const PipelinedProbe probeA = readPipelined(frameA);
          const PipelinedProbe probeB = readPipelined(frameB);
          PipelinedFrame frameC = recordPipelined(0u);
          const PipelinedProbe probeC = readPipelined(frameC);
          // Every object shares the asset, so each adds the same cluster
          // queue contribution; the install frame must carry the reinstalled
          // object and the new one.
          expectedQueue += baseProbe.queue;
          growText << "\n    grow " << replacement << ": inFlight[q=" << probeA.queue << " px=" << probeA.coveredPixels << "]"
                   << " install[q=" << probeB.queue << " p2=" << probeB.phaseTwo << " px=" << probeB.coveredPixels << "]"
                   << " next[q=" << probeC.queue << " px=" << probeC.coveredPixels << "] expectedQueue=" << expectedQueue;
          if (probeB.queue < expectedQueue)
            ++growQueueLoss;
          if (probeB.coveredPixels < grownBase.coveredPixels || probeC.coveredPixels < grownBase.coveredPixels)
            ++growHoles;
        }
        os::Logger::logf("  growing replacements:%s", growText.str().c_str());
        check(growQueueLoss == 0u,
              "with the instance list reordered and grown, the install frame traverses every object (" +
                  std::to_string(growQueueLoss) + " frames traversed fewer)");
        check(growHoles == 0u,
              "no frame around a growing replacement covers fewer pixels (" + std::to_string(growHoles) + " holes)");
      }
      check(inFlightHoles == 0u,
            "the frame in flight during a replacement keeps drawing the old object (" +
                std::to_string(inFlightHoles) + " holes)");
      check(pipelinedQueueLoss == 0u,
            "the frame recorded after the replacement traverses the new object's clusters (" +
                std::to_string(pipelinedQueueLoss) + " frames traversed fewer)");
      check(pipelinedHoles == 0u,
            "no frame after a pipelined replacement covers fewer pixels (" +
                std::to_string(pipelinedHoles) + " holes)");
    }

    // ── [evict] ─────────────────────────────────────────────────────────────
    //
    // Under page-buffer pressure the scene retires least-recently-used pages
    // to make room. A retired page's hierarchy leaves must be marked not
    // installed and the coarser clusters it replaced must be enabled again, or
    // the culler walks into a page that is gone and draws nothing there.
    os::Logger::log("[evict] page eviction keeps the hierarchy consistent with the page table");
    {
      uint64_t pageBytes = 0u;
      for (const auto &page : asset.encoded.pages)
        pageBytes += PageBuffer::encode(page).data.size() * sizeof(uint32_t);
      // Room for one object and a little more than half of a second one.
      const uint64_t tightPagesBytes = pageBytes + pageBytes / 2u + 4096u;
      auto tightGraphStorage = std::make_unique<RenderGraph>(rhi);
      RenderGraph *tightGraph = tightGraphStorage.get();
      VirtualGeometryScene tight(tightGraph, hierarchyBytes, tightPagesBytes);
      tightGraph->registerPass<VirtualGeometryPageDecodePass>("evict.pageDecode", 0, tight);
      tightGraph->compile();

      const auto installTight = [&](const std::string &name, const uint64_t generation)
      {
        VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
        install.name = name;
        install.asset = &asset.encoded;
        install.basePageGeneration = generation;
        install.geometryRevision = generation;
        install.materialRevision = 1u;
        install.hasTracePayload = true;
        install.enqueuePageDecode = true;
        std::string installError;
        const bool installed = tight.installRuntimeEncodedObject(install, nullptr, &installError);
        if (!installed)
          os::Logger::logf("  install of %s: %s", name.c_str(), installError.c_str());
        tight.updateInstanceBuffer();
        return installed;
      };
      check(installTight("first", 1u), "the first object fits the tight page buffer");
      tight.setRuntimeStreamingFrame(1u);
      tight.prepareFrame(0u);
      {
        RenderGraph::Overrides overrides;
        tight.appendFrameOverrides(overrides, 0u);
        RenderGraph::Frame frame;
        tightGraph->run(frame, overrides);
        tightGraph->waitFrame(frame);
      }
      tight.setRuntimeStreamingFrame(2u);
      const bool secondInstalled = installTight("second", 1000u);
      os::Logger::logf("  second object install %s", secondInstalled ? "succeeded" : "failed");

      // Read the page table and hierarchy back and compare, object by object.
      std::vector<PageTableEntry> pageTable(tight.nextPageTableSlot);
      tightGraph->bufferRead(tight.pageTableBuffer, 0, pageTable.size() * sizeof(PageTableEntry),
                             [&](const void *data) { std::memcpy(pageTable.data(), data, pageTable.size() * sizeof(PageTableEntry)); });
      uint32_t inconsistentLeaves = 0u;
      uint32_t evictedPages = 0u;
      uint32_t objectsChecked = 0u;
      for (const char *name : {"first", "second"})
      {
        VirtualGeometryScene::RuntimeObjectDiagnostics diagnostics{};
        if (!tight.getRuntimeObjectDiagnostics(name, diagnostics) || !diagnostics.registered)
          continue;
        ++objectsChecked;
        std::vector<VirtualGeometryHierarchy> nodes(diagnostics.hierarchyNodeCount);
        tightGraph->bufferRead(
            tight.hierarchyBuffer,
            static_cast<uint64_t>(diagnostics.hierarchyStartOffset) * sizeof(VirtualGeometryHierarchy),
            nodes.size() * sizeof(VirtualGeometryHierarchy),
            [&](const void *data) { std::memcpy(nodes.data(), data, nodes.size() * sizeof(VirtualGeometryHierarchy)); });
        uint32_t installedFromTable = 0u;
        for (uint32_t page = 0u; page < diagnostics.pageCount; ++page)
          installedFromTable += pageTable[diagnostics.pageTableOffset + page].isInstalled != 0u ? 1u : 0u;
        evictedPages += diagnostics.pageCount - installedFromTable;
        for (const auto &node : nodes)
        {
          if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u || node.pageIndex == UINT32_MAX)
            continue;
          const uint32_t page = node.pageIndex & ~kPageNotInstalledBit;
          if (page >= diagnostics.pageCount)
            continue;
          const bool tableInstalled = pageTable[diagnostics.pageTableOffset + page].isInstalled != 0u;
          const bool hierarchyInstalled = (node.pageIndex & kPageNotInstalledBit) == 0u;
          if (tableInstalled != hierarchyInstalled)
            ++inconsistentLeaves;
        }
        os::Logger::logf(
            "  %s: pages=%u installed(table)=%u installed(record)=%u",
            name, diagnostics.pageCount, installedFromTable, diagnostics.installedPageCount);
      }
      check(objectsChecked == 2u, "both objects are registered after the pressured install");
      check(evictedPages > 0u, "the tight page buffer forced pages to be evicted (" +
                                   std::to_string(evictedPages) + " evicted)");
      check(inconsistentLeaves == 0u,
            "every hierarchy leaf agrees with the page table about its page's residency (" +
                std::to_string(inconsistentLeaves) + " leaves disagree)");
    }

    // ── [scale] ─────────────────────────────────────────────────────────────
    //
    // The sample's cut settles around fifteen thousand clusters, far past the
    // few thousand the bootstrap asset set holds. A draw capacity taken from
    // that bootstrap count silently dropped every cluster the culler emitted
    // past it, and since the atomic emission order changes from frame to
    // frame, which clusters were dropped changed too: a settled scene whose
    // coverage moved by several percent every frame. Both draw paths - the
    // single indirect draw with the culler's vertex count, and the flat
    // fixed-size draw - must draw the whole cut, identically every frame, and
    // cover the same pixels as each other.
    os::Logger::log("[scale] a cut far past any bootstrap capacity draws completely and identically every frame");
    {
      constexpr uint32_t kGrid = 12u;
      // Four times the pixels of the other sections: the cut is bounded by
      // pixels, and the sample's settles near fifteen thousand clusters.
      constexpr uint32_t kScaleSide = 512u;
      const uint64_t scaleIdBytes = static_cast<uint64_t>(kScaleSide) * kScaleSide * sizeof(uint32_t) * 2u;
      uint32_t scaleHzbMips = 1u;
      for (uint32_t dimension = kScaleSide; dimension > 1u; dimension >>= 1u)
        ++scaleHzbMips;
      scaleHzbMips = std::min(scaleHzbMips, 8u);
      // A quarter-pixel budget: every chunk in view refines to its leaves, so
      // the cut is as large as the scene allows rather than as large as the
      // image allows.
      constexpr float kScaleBudget = 0.25f;
      struct ScaleResult
      {
        uint32_t drawn = 0u;
        uint32_t coveredPixels = 0u;
        uint64_t coverageHash = 0u;
        uint32_t unstableFrames = 0u;
        uint32_t phaseTwoRedraws = 0u;
        bool ok = false;
      };
      const auto runScale = [&](const bool flatDraw, const bool occlusion, const bool frustum) -> ScaleResult
      {
        ScaleResult result{};
        auto scaleGraphStorage = std::make_unique<RenderGraph>(rhi);
        RenderGraph *scaleGraph = scaleGraphStorage.get();
        const uint32_t scaleHierarchyBytes =
            hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * (kGrid * kGrid + 4u);
        VirtualGeometryScene scaleScene(scaleGraph, scaleHierarchyBytes, 512ull * 1024 * 1024);
        for (uint32_t gx = 0u; gx < kGrid; ++gx)
          for (uint32_t gz = 0u; gz < kGrid; ++gz)
          {
            VirtualGeometryScene::RuntimeEncodedObjectInstallInfo install{};
            install.name = "scale_" + std::to_string(gx) + "_" + std::to_string(gz);
            install.asset = &asset.encoded;
            install.basePageGeneration = 1u + (gx * kGrid + gz) * 64u;
            install.geometryRevision = install.basePageGeneration;
            install.materialRevision = 1u;
            install.position = math::Vec3f(
                (static_cast<float>(gx) - static_cast<float>(kGrid) * 0.5f + 0.5f) * static_cast<float>(kSide),
                0.0f,
                (static_cast<float>(gz) - static_cast<float>(kGrid) * 0.5f + 0.5f) * static_cast<float>(kSide));
            install.hasTracePayload = true;
            install.enqueuePageDecode = true;
            std::string installError;
            if (!scaleScene.installRuntimeEncodedObject(install, nullptr, &installError))
            {
              os::Logger::errorf("scale install of %s failed: %s", install.name.c_str(), installError.c_str());
              return result;
            }
          }
        scaleScene.updateInstanceBuffer();

        const std::string prefix = std::string(flatDraw ? "ScaleFlat" : "ScaleIndirect") +
            (occlusion ? "" : "NoOcc") + (frustum ? "" : "NoFrustum") + "_";
        Texture sDepth = scaleGraph->createTexture(TextureInfo{
          .name = prefix + "Depth.texture", .format = Format::Format_Depth32Float,
          .memoryProperties = BufferUsage::BufferUsage_None,
          .usage = ImageUsage::ImageUsage_DepthStencilAttachment | ImageUsage::ImageUsage_Sampled,
          .width = kScaleSide, .height = kScaleSide, .mipLevels = 1u});
        const auto sColor = [&](const char *name, const Format format)
        {
          return scaleGraph->createTexture(TextureInfo{
            .name = prefix + name, .format = format, .memoryProperties = BufferUsage::BufferUsage_None,
            .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
            .width = kScaleSide, .height = kScaleSide, .mipLevels = 1u});
        };
        Texture sIdsLo = sColor("IdsLo.texture", Format::Format_R32Uint);
        Texture sIdsHi = sColor("IdsHi.texture", Format::Format_R32Uint);
        Texture sMaterialId = sColor("MaterialId.texture", Format::Format_R32Uint);
        Texture sMaterialUV = sColor("MaterialUV.texture", Format::Format_RG32Float);
        Texture sMotion = sColor("Motion.texture", Format::Format_RG16Float);
        Texture sBaseColor = sColor("BaseColor.texture", Format::Format_RGBA16Float);
        Texture sMaterialProperties = sColor("MaterialProperties.texture", Format::Format_RGBA16Float);
        Texture sEmissive = sColor("Emissive.texture", Format::Format_RGBA16Float);
        Texture sGeometryNormal = sColor("GeometryNormal.texture", Format::Format_RGBA16Float);
        Texture sNormal = sColor("Normal.texture", Format::Format_RGBA16Float);
        Texture sColorTex = sColor("Color.texture", Format::Format_RGBA16Float);
        Texture sHzb = scaleGraph->createTexture(TextureInfo{
          .name = prefix + "HZB.texture", .format = Format::Format_R32Float,
          .memoryProperties = BufferUsage::BufferUsage_Storage,
          .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
          .width = kScaleSide, .height = kScaleSide, .mipLevels = scaleHzbMips});
        const TextureView sDepthView{
          .texture = sDepth, .index = 0u, .flags = ImageAspectFlags::Depth,
          .baseMipLevel = 0u, .levelCount = 1u, .baseArrayLayer = 0u, .layerCount = 1u,
          .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
          .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT};
        VirtualGeometryHardwareDrawPass::FrameTarget sTarget{
          .packedGeometryIdsLoView = colorView(sIdsLo), .packedGeometryIdsHiView = colorView(sIdsHi),
          .materialIdView = colorView(sMaterialId), .materialUVView = colorView(sMaterialUV),
          .baseColorView = colorView(sBaseColor), .materialPropertiesView = colorView(sMaterialProperties),
          .emissiveView = colorView(sEmissive), .geometryNormalView = colorView(sGeometryNormal),
          .normalView = colorView(sNormal), .motionVectorView = colorView(sMotion),
          .depthView = sDepthView, .depthTexture = sDepth, .baseColorTexture = sBaseColor,
          .materialPropertiesTexture = sMaterialProperties, .emissiveTexture = sEmissive,
          .geometryNormalTexture = sGeometryNormal, .normalTexture = sNormal, .motionVectorTexture = sMotion,
          .colorView = colorView(sColorTex), .colorTexture = sColorTex};
        VirtualGeometryDepthPrePassDrawPass::FrameTarget sPrePassTarget{.depthView = sDepthView, .depthTexture = sDepth};

        VirtualGeometryRendererPass::Settings scaleSettings = settings;
        scaleSettings.hardwareDrawSettings.directDrawWhenIndirectCountUnavailable = flatDraw;
        scaleSettings.hardwareDrawSettings.maxDrawnClusters = VirtualGeometryScene::MAX_VISIBLE_CLUSTERS;
        scaleSettings.depthPrePassSettings.directDrawWhenIndirectCountUnavailable = flatDraw;
        scaleSettings.depthPrePassSettings.maxDrawnClusters = VirtualGeometryScene::MAX_VISIBLE_CLUSTERS;
        scaleSettings.prepassCullingSettings.errorTreshhold = kScaleBudget;
        scaleSettings.finalCullingSettings.errorTreshhold = kScaleBudget;
        scaleSettings.prepassCullingSettings.viewPortWidth = kScaleSide;
        scaleSettings.prepassCullingSettings.viewPortHeight = kScaleSide;
        scaleSettings.prepassCullingSettings.hiZMipLevels = scaleHzbMips;
        scaleSettings.finalCullingSettings.viewPortWidth = kScaleSide;
        scaleSettings.finalCullingSettings.viewPortHeight = kScaleSide;
        scaleSettings.finalCullingSettings.hiZMipLevels = scaleHzbMips;
        scaleSettings.depthPrePassSettings.viewPortWidth = kScaleSide;
        scaleSettings.depthPrePassSettings.viewPortHeight = kScaleSide;
        scaleSettings.depthPyramidSettings.width = kScaleSide;
        scaleSettings.depthPyramidSettings.height = kScaleSide;
        scaleSettings.depthPyramidSettings.outputWidth = kScaleSide;
        scaleSettings.depthPyramidSettings.outputHeight = kScaleSide;
        scaleSettings.depthPyramidSettings.mipCount = scaleHzbMips;
        scaleSettings.hardwareDrawSettings.viewPortWidth = kScaleSide;
        scaleSettings.hardwareDrawSettings.viewPortHeight = kScaleSide;
        scaleSettings.prepassCullingSettings.enableFrustumCulling = frustum;
        scaleSettings.finalCullingSettings.enableFrustumCulling = frustum;
        scaleSettings.finalCullingSettings.enableOcclusionCulling = occlusion;
        uint32_t sPassIndex = 0u;
        auto *sRenderer = scaleGraph->registerPass<VirtualGeometryRendererPass>(
            prefix + "renderer", sPassIndex++, scaleScene, sTarget, sPrePassTarget, Texture{}, sHzb, 0.0f, scaleSettings);
        sPassIndex += VirtualGeometryRendererPass::childPassCount(scaleSettings);
        Buffer sIdBuffer = scaleGraph->createBuffer(BufferInfo{
          .name = prefix + "Ids.buffer", .size = scaleIdBytes,
          .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc});
        Buffer sIdStaging = scaleGraph->createBuffer(BufferInfo{
          .name = prefix + "Ids.staging", .size = scaleIdBytes,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        Buffer sPreStatsStaging = scaleGraph->createBuffer(BufferInfo{
          .name = prefix + "PreStats.staging", .size = 64u,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        Buffer sStatsStaging = scaleGraph->createBuffer(BufferInfo{
          .name = prefix + "Stats.staging", .size = 64u,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        Buffer sVisibleStaging = scaleGraph->createBuffer(BufferInfo{
          .name = prefix + "Visible.staging", .size = visibleClusterBytes,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull});
        scaleGraph->registerPass<VirtualGeometryIdReadbackPass>(
            prefix + "idReadback", sPassIndex++, sIdsLo, sIdsHi, sIdBuffer, kScaleSide, kScaleSide);
        scaleGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
            prefix + "copyIds", sPassIndex++, sIdBuffer, 0, scaleIdBytes, sIdStaging, 0, scaleIdBytes);
        scaleGraph->compile();
        auto *sPre = sRenderer->getPreCullingPass();
        auto *sFinal = sRenderer->getFinalCullingPass();
        auto *sPhaseOne = sRenderer->getPhaseOneDrawPass();
        auto *sDraw = sRenderer->getHardwareDrawPass();
        scaleGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
            prefix + "copyPreStats", sPassIndex++, sPre->getCullingStatisticsBuffer(), 0, 64u, sPreStatsStaging, 0, 64u);
        scaleGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
            prefix + "copyStats", sPassIndex++, sFinal->getCullingStatisticsBuffer(), 0, 64u, sStatsStaging, 0, 64u);
        scaleGraph->registerPass<rendering::gpgpu::CopyBufferPass>(
            prefix + "copyVisible", sPassIndex++, sPre->getHWVisibleClusterInfosBuffer(), 0,
            visibleClusterBytes, sVisibleStaging, 0, visibleClusterBytes);
        scaleGraph->compile();

        // Close and low over the grid: every chunk in view refines to its
        // leaves, so the cut is as large as the asset allows.
        // A low, grazing view down the length of the grid: chunks at every
        // distance from a few units to the far edge, so the cut mixes leaf
        // clusters near the camera with coarser levels behind them and is
        // bounded by pixels rather than by the frustum.
        const CameraState scaleCamera = makeCamera(
            math::Vec3f(0.0f, 40.0f, 340.0f), math::Vec3f(0.0f, 0.0f, 0.0f));
        uint64_t sFrameNumber = 0u;
        struct ScaleProbe
        {
          uint32_t phaseOne = 0u;
          uint32_t phaseTwo = 0u;
          uint32_t coveredPixels = 0u;
          uint64_t imageHash = 0u;
          uint64_t coverageHash = 0u;
          uint32_t instancesDrawn = 0u;
          uint32_t minPerInstance = 0u;
          uint32_t maxPerInstance = 0u;
          uint32_t minInstance = 0u;
          uint32_t maxInstance = 0u;
        };
        const auto runScaleFrame = [&]() -> ScaleProbe
        {
          ++sFrameNumber;
          scaleScene.setRuntimeStreamingFrame(sFrameNumber);
          scaleScene.touchRuntimePagesFromGPUPriorities();
          const float *viewArray = scaleCamera.view.data;
          const float *projectionArray = scaleCamera.projection.data;
          float viewPosition[4] = {scaleCamera.eye[0], scaleCamera.eye[1], scaleCamera.eye[2], 1.0f};
          sPre->updateUniforms(viewArray, projectionArray, viewPosition, kScaleSide, kScaleSide, kNear, kFar, scaleHzbMips);
          sFinal->updateUniforms(viewArray, projectionArray, viewPosition, kScaleSide, kScaleSide, kNear, kFar, scaleHzbMips);
          sPhaseOne->updateUniforms(viewArray, projectionArray, viewPosition, kScaleSide, kScaleSide, kNear, kFar, scaleHzbMips, kScaleBudget);
          sDraw->updateUniforms(viewArray, projectionArray, viewPosition, kScaleSide, kScaleSide, kNear, kFar, scaleHzbMips, kScaleBudget);
          scaleScene.prepareFrame(0u);
          RenderGraph::Overrides overrides;
          sRenderer->appendFrameOverrides(overrides, 0u);
          RenderGraph::Frame frame;
          scaleGraph->run(frame, overrides);
          scaleGraph->waitFrame(frame);
          ScaleProbe probe{};
          CullingCounters phaseOne{};
          CullingCounters phaseTwo{};
          scaleGraph->bufferRead(sPreStatsStaging, 0, sizeof(CullingCounters),
                                 [&](const void *data) { std::memcpy(&phaseOne, data, sizeof(CullingCounters)); });
          scaleGraph->bufferRead(sStatsStaging, 0, sizeof(CullingCounters),
                                 [&](const void *data) { std::memcpy(&phaseTwo, data, sizeof(CullingCounters)); });
          probe.phaseOne = phaseOne.visibleClusterHardwareCount;
          probe.phaseTwo = phaseTwo.visibleClusterHardwareCount;
          std::vector<uint32_t> packed(kScaleSide * kScaleSide * 2u, 0u);
          scaleGraph->bufferRead(sIdStaging, 0, scaleIdBytes, [&](const void *data) { std::memcpy(packed.data(), data, scaleIdBytes); });
          uint64_t hash = 1469598103934665603ull;
          uint64_t coverageHash = 1469598103934665603ull;
          for (uint32_t pixel = 0u; pixel < kScaleSide * kScaleSide; ++pixel)
          {
            const uint32_t id = packed[pixel * 2u + 1u];
            hash = (hash ^ id) * 1099511628211ull;
            coverageHash = (coverageHash ^ (id != 0u ? 1u : 0u)) * 1099511628211ull;
            probe.coveredPixels += id != 0u ? 1u : 0u;
          }
          probe.imageHash = hash;
          probe.coverageHash = coverageHash;
          if (sFrameNumber == 3u)
          {
            // Coverage map, 16x16 pixel cells, for the log.
            std::ostringstream map;
            for (uint32_t cy = 0u; cy < kScaleSide / 16u; ++cy)
            {
              map << "\n    ";
              for (uint32_t cx = 0u; cx < kScaleSide / 16u; ++cx)
              {
                uint32_t covered = 0u;
                for (uint32_t y = 0u; y < 16u; ++y)
                  for (uint32_t x = 0u; x < 16u; ++x)
                    covered += packed[((cy * 16u + y) * kScaleSide + cx * 16u + x) * 2u + 1u] != 0u ? 1u : 0u;
                map << (covered == 0u ? '.' : covered < 128u ? '+' : '#');
              }
            }
            os::Logger::logf("  coverage map:%s", map.str().c_str());
          }
          if (probe.phaseOne != 0u && probe.phaseOne <= VirtualGeometryScene::MAX_VISIBLE_CLUSTERS)
          {
            std::vector<VisibleClusterInfo> visible(probe.phaseOne);
            scaleGraph->bufferRead(sVisibleStaging, 0, probe.phaseOne * sizeof(VisibleClusterInfo),
                                   [&](const void *data) { std::memcpy(visible.data(), data, probe.phaseOne * sizeof(VisibleClusterInfo)); });
            std::map<uint32_t, uint32_t> perInstance;
            for (const auto &info : visible)
              ++perInstance[info.instanceIndex];
            uint32_t minCount = 0xffffffffu;
            uint32_t maxCount = 0u;
            for (const auto &[instance, count] : perInstance)
            {
              minCount = std::min(minCount, count);
              maxCount = std::max(maxCount, count);
            }
            probe.instancesDrawn = static_cast<uint32_t>(perInstance.size());
            probe.minPerInstance = minCount;
            probe.maxPerInstance = maxCount;
            probe.minInstance = perInstance.begin()->first;
            probe.maxInstance = perInstance.rbegin()->first;
          }
          return probe;
        };
        constexpr uint32_t kScaleFrames = 10u;
        std::vector<ScaleProbe> frames;
        for (uint32_t frame = 0u; frame < kScaleFrames; ++frame)
          frames.push_back(runScaleFrame());
        const ScaleProbe &reference = frames[2];
        for (uint32_t frame = 2u; frame < kScaleFrames; ++frame)
        {
          if (frames[frame].imageHash != reference.imageHash)
            ++result.unstableFrames;
          if (frames[frame].phaseTwo != 0u)
            ++result.phaseTwoRedraws;
        }
        result.drawn = reference.phaseOne + reference.phaseTwo;
        result.coveredPixels = reference.coveredPixels;
        result.coverageHash = reference.coverageHash;
        result.ok = true;
        std::vector<PageTableEntry> scalePageTable(scaleScene.nextPageTableSlot);
        uint32_t scaleInstalledPages = 0u;
        if (!scalePageTable.empty())
          scaleGraph->bufferRead(scaleScene.pageTableBuffer, 0, scalePageTable.size() * sizeof(PageTableEntry),
                                 [&](const void *data) { std::memcpy(scalePageTable.data(), data, scalePageTable.size() * sizeof(PageTableEntry)); });
        for (const auto &entry : scalePageTable)
          scaleInstalledPages += entry.isInstalled != 0u ? 1u : 0u;
        os::Logger::logf("  %s: frame0 p1=%u p2=%u | settled p1=%u p2=%u px=%u instances=%u/%u [%u..%u] perInstance=[%u,%u] pages=%u/%u unstable=%u p2redraws=%u",
                         (prefix.substr(0, prefix.size() - 1u)).c_str(),
                         frames[0].phaseOne, frames[0].phaseTwo, reference.phaseOne, reference.phaseTwo,
                         reference.coveredPixels, reference.instancesDrawn, kGrid * kGrid,
                         reference.minInstance, reference.maxInstance,
                         reference.minPerInstance, reference.maxPerInstance,
                         scaleInstalledPages, kGrid * kGrid * pageCount,
                         result.unstableFrames, result.phaseTwoRedraws);
        return result;
      };
      const ScaleResult indirect = runScale(false, true, true);
      const ScaleResult flat = runScale(true, true, true);
      const ScaleResult noOcclusion = runScale(false, false, true);
      const ScaleResult noFrustum = runScale(false, true, false);
      os::Logger::logf("  variants: occlusion off drawn=%u px=%u | frustum off drawn=%u px=%u",
                       noOcclusion.drawn, noOcclusion.coveredPixels, noFrustum.drawn, noFrustum.coveredPixels);
      // Culling is a pure optimisation: with either test disabled the frame
      // draws more clusters and the identical pixels.
      check(noFrustum.drawn > indirect.drawn && noFrustum.coverageHash == indirect.coverageHash,
            "frustum culling removes clusters and never a pixel (" + std::to_string(noFrustum.drawn) + " vs " +
                std::to_string(indirect.drawn) + " clusters)");
      check(noOcclusion.coverageHash == indirect.coverageHash,
            "occlusion culling never removes a pixel");
      check(indirect.ok && flat.ok, "both draw paths render the 64-chunk scene");
      check(indirect.drawn > 6000u,
            "the scene draws far more clusters than any bootstrap capacity (" + std::to_string(indirect.drawn) + ")");
      check(indirect.unstableFrames == 0u,
            "the indirect draw renders the identical image on every settled frame (" +
                std::to_string(indirect.unstableFrames) + " differed)");
      check(flat.unstableFrames == 0u,
            "the flat draw renders the identical image on every settled frame (" +
                std::to_string(flat.unstableFrames) + " differed)");
      check(indirect.phaseTwoRedraws == 0u && flat.phaseTwoRedraws == 0u,
            "the large cut settles with phase two re-emitting nothing");
      check(indirect.coveredPixels == flat.coveredPixels && indirect.coverageHash == flat.coverageHash,
            "both draw paths cover the identical pixels (" + std::to_string(indirect.coveredPixels) + " vs " +
                std::to_string(flat.coveredPixels) + ")");
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
    os::Logger::errorf("%d sparse voxel VG cut stability checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all sparse voxel VG cut stability checks passed");
  os::Logger::shutdown();
  return 0;
}
