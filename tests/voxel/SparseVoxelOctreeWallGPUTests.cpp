// A wall of voxels, rendered on the GPU, inspected pixel by pixel as it streams.
//
// The camera looks straight at a flat wall of voxels. The render target is
// exactly as wide as the wall is in finest voxels, and the error budget is set
// just above zero so the cut wants the finest level everywhere it can have it.
// Pages are then installed one at a time and the frame is rendered and read back
// after every install.
//
// The channel inspected is packedGeometryIdsHi, which the virtual geometry
// fragment shader packs from page, group, cluster and triangle - a unique
// identity per rendered surface, which is the "unique colour per voxel" this
// test needs without depending on a material palette the virtual geometry vertex
// format does not carry. Each frame is also written out as a PPM so the picture
// can be looked at directly.
//
// Only virtual geometry passes take part: page decode, culling, hardware draw,
// and a one-dispatch id readback. There is no swapchain and no window.
//
// STATUS: NOT WORKING - this target builds and runs but renders nothing, and it
// is deliberately not registered as a ctest.
//
// The scene uploads correctly: 112 hierarchy nodes, root at index 0 with
// childCount 3, AABB (-32,-32,0)-(32,32,4), maxParentLodError = FLT_MAX, one
// instance, and the root page installs. But the culling pass reports
// hierarchyQueueSize == 0 on every frame - the traversal never seeds a root
// node, so no cluster is selected and the draw has nothing to rasterise.
//
// Bisected against SparseVoxelOctreeStreamingGPUTests, which uses the same scene
// wiring and does select clusters. Ruled out, each by direct experiment:
//   - the hardware draw pass (removing it changes nothing);
//   - sharing the culling statistics buffer with the draw pass (the draw pass
//     only reads it, with AccessPattern::SHADER_READ);
//   - the error budget (1e-4 and 1.0 behave identically);
//   - the 64x64 viewport (1920x1080 behaves identically);
//   - the camera orientation (looking down -Z from +Z, as the working test
//     does, behaves identically).
//
// So the difference is in the asset or the scene setup, not the culling
// configuration, and it has not been found yet.
//
// The deterministic version of this test - same wall, same one-voxel-per-pixel
// framing, same streaming sequence - lives in SparseVoxelOctreeWallImageTests
// and passes. That one rasterises on the CPU from the selected cut.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "virtualgeometry/rendering/VirtualGeometryIdReadbackPass.hpp"
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

// The wall is this many finest voxels on a side; the image is that many pixels.
constexpr uint32_t kWallSide = 64u;
constexpr uint32_t kImageSide = 64u;
// Just above zero: leaf clusters carry self error 0 and pass, every coarse
// cluster carries at least the brick error floor and does not.
constexpr float kFinestErrorBudget = 1.0f;
constexpr uint32_t kMaxQueueElements = 1u << 20;
// EXPERIMENT: isolate whether the draw pass breaks culling.
constexpr bool kDrawPassEnabled = true;
// EXPERIMENT: is the custom readback pass what stops the culler running?
constexpr bool kReadbackEnabled = true;
constexpr bool kBuildChunkInsteadOfWall = false;
constexpr uint32_t kWallThickness = 1u;

int failures = 0;

void check(const bool condition, const std::string &message)
{
  os::Logger::logf("  %s %s", condition ? "PASSED" : "FAILED", message.c_str());
  failures += condition ? 0 : 1;
}

uint32_t wallMaterial(const uint32_t x, const uint32_t y)
{
  return (x ^ y) & 1u;
}

// EXPERIMENT: build the working test's terrain-shaped chunk instead of the wall,
// to isolate whether the asset or the harness is at fault.
uint32_t experimentColumnHeight(const uint32_t x, const uint32_t z, const uint32_t side)
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

bool buildWallOctree(svr::SparseVoxelBrickHierarchy &hierarchy, std::string *error)
{
  std::vector<svr::SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<svr::SparseVoxelMaterialGPU> materials;
  std::vector<uint32_t> leaves;

  if (kBuildChunkInsteadOfWall)
  {
    constexpr uint32_t kShellDepth = 3u;
    for (uint32_t x = 0u; x < kWallSide; ++x)
      for (uint32_t z = 0u; z < kWallSide; ++z)
      {
        const uint32_t top = experimentColumnHeight(x, z, kWallSide);
        uint32_t lowestNeighbour = top;
        const std::array<std::array<int32_t, 2>, 4> neighbours{{
            {{-1, 0}}, {{1, 0}}, {{0, -1}}, {{0, 1}},
        }};
        for (const auto &offset : neighbours)
        {
          const int32_t nx = static_cast<int32_t>(x) + offset[0];
          const int32_t nz = static_cast<int32_t>(z) + offset[1];
          if (nx < 0 || nz < 0 || nx >= static_cast<int32_t>(kWallSide) ||
              nz >= static_cast<int32_t>(kWallSide))
            continue;
          lowestNeighbour = std::min(
              lowestNeighbour,
              experimentColumnHeight(
                  static_cast<uint32_t>(nx), static_cast<uint32_t>(nz), kWallSide));
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
  }
  else
  {
    for (uint32_t x = 0u; x < kWallSide; ++x)
      for (uint32_t y = 0u; y < kWallSide; ++y)
        for (uint32_t z = 0u; z < kWallThickness; ++z)
      {
        svr::SparseVoxelCenterLengthGPU cell{};
        cell.center = {
            static_cast<float>(x) + 0.5f,
            static_cast<float>(y) + 0.5f,
            static_cast<float>(z) + 0.5f};
        cell.length = 1.0f;
        svr::SparseVoxelMaterialGPU material{};
        const uint32_t band = wallMaterial(x, y);
        material.albedoRGBA8 = band == 0u ? 0xff3070c0u : 0xffc07030u;
        material.materialId = band;
        leaves.push_back(static_cast<uint32_t>(centersLengths.size()));
        centersLengths.push_back(cell);
        materials.push_back(material);
      }
  }

  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = 1.0f;
  settings.maximumBricks = 1u << 18u;
  return svr::buildSparseVoxelBrickHierarchy(
      centersLengths, materials, leaves, settings, hierarchy, error);
}

struct RenderedImage
{
  std::vector<uint32_t> ids;
  uint32_t coveredPixels = 0u;
  uint32_t distinctIds = 0u;
  uint32_t visibleClusters = 0u;
  uint32_t installedPages = 0u;
  uint32_t hierarchyQueue = 0u;
  uint32_t clusterQueue = 0u;
  uint64_t hash = 0u;
};

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

RenderedImage summarize(const std::vector<uint32_t> &packed)
{
  RenderedImage image{};
  image.ids.assign(kImageSide * kImageSide, 0u);
  std::set<uint32_t> distinct;
  for (uint32_t pixel = 0u; pixel < kImageSide * kImageSide; ++pixel)
  {
    const uint32_t id = packed[pixel * 2u + 1u];
    image.ids[pixel] = id;
    if (id != 0u)
    {
      ++image.coveredPixels;
      distinct.insert(id);
    }
  }
  image.distinctIds = static_cast<uint32_t>(distinct.size());
  uint64_t hash = 1469598103934665603ull;
  for (const uint32_t id : image.ids)
    hash = (hash ^ id) * 1099511628211ull;
  image.hash = hash;
  return image;
}

// A stable colour per surface id, so the dumped image is legible.
void writePPM(const RenderedImage &image, const fs::path &path)
{
  std::ofstream file(path, std::ios::binary);
  if (!file)
    return;
  file << "P6\n" << kImageSide << " " << kImageSide << "\n255\n";
  for (uint32_t y = 0u; y < kImageSide; ++y)
    for (uint32_t x = 0u; x < kImageSide; ++x)
    {
      // Flip vertically so +Y is up in the written image.
      const uint32_t id = image.ids[(kImageSide - 1u - y) * kImageSide + x];
      std::array<unsigned char, 3> rgb{0u, 0u, 0u};
      if (id != 0u)
      {
        uint64_t hash = id;
        hash = (hash ^ (hash >> 33u)) * 0xff51afd7ed558ccdull;
        hash = (hash ^ (hash >> 33u)) * 0xc4ceb9fe1a85ec53ull;
        rgb[0] = static_cast<unsigned char>(64u + (hash & 0xbfu));
        rgb[1] = static_cast<unsigned char>(64u + ((hash >> 16u) & 0xbfu));
        rgb[2] = static_cast<unsigned char>(64u + ((hash >> 32u) & 0xbfu));
      }
      file.write(reinterpret_cast<const char *>(rgb.data()), 3);
    }
}

} // namespace

int main()
{
  os::Logger::start();
  os::Logger::log("Sparse voxel octree wall, rendered on the GPU");
  os::Logger::log("=============================================");

  svr::SparseVoxelBrickHierarchy octree{};
  std::string error;
  if (!buildWallOctree(octree, &error))
  {
    os::Logger::errorf("wall octree build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  octreeSettings.originOffset = kBuildChunkInsteadOfWall
      ? std::array<double, 3>{
            static_cast<double>(kWallSide) * 0.5, 0.0, static_cast<double>(kWallSide) * 0.5}
      : std::array<double, 3>{
            static_cast<double>(kWallSide) * 0.5,
            static_cast<double>(kWallSide) * 0.5,
            0.0};
  VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.maxRootPageGroups = 1u;
  VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!svr::buildSparseVoxelOctreeVirtualGeometry(
          octree, 0u, static_cast<uint32_t>(octree.nodes.size()),
          octreeSettings, buildSettings, build, &stats, &error))
  {
    os::Logger::errorf("wall virtual geometry build failed: %s", error.c_str());
    os::Logger::shutdown();
    return 1;
  }

  QuantizationConfig quantization{};
  quantization.quantization_factor = 4u;
  quantization.unit_scale = 100.0f;
  quantization.padMeshlets = true;
  VirtualGeometryEncodedData encoded =
      VirtualGeometryCompressor::encode(build, quantization);

  const fs::path outputDirectory =
      fs::path(os::io::getExecutableDirectory()) / "wall-gpu";
  std::error_code directoryError;
  fs::create_directories(outputDirectory, directoryError);
  const fs::path assetPath = outputDirectory / "wall.virtualgeometry";
  {
    VirtualGeometryFile writer(assetPath.string(), /*write=*/true);
    if (!writer.isOpen() || !writer.write(encoded, build.pages, MESHLET_LZ4))
    {
      os::Logger::errorf("could not write %s", assetPath.string().c_str());
      os::Logger::shutdown();
      return 1;
    }
  }

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf(
      "  wall %ux%u bricks=%zu levels=%u clusters=%zu pages=%u blockDepth=%u",
      kWallSide, kWallSide, octree.nodes.size(), stats.levelCount,
      build.clusters.size(), pageCount, stats.blockDepth);
  check(pageCount > 4u, "the wall is split over several pages");

  DeviceRequiredLimits limits{};
  // A headless device with no surface can come up without a graphics queue, and
  // the virtual geometry raster then records a render pass that produces nothing
  // while every compute pass keeps working. Ask for graphics explicitly, and for
  // the clip distances the meshlet vertex shader uses - this is the feature set
  // VirtualGeometryRenderer asks for.
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
    const std::string objectName = "wall";
    const uint32_t hierarchyBytes = std::max(
        hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
    auto graphStorage = std::make_unique<RenderGraph>(rhi);
    RenderGraph *graph = graphStorage.get();
    VirtualGeometryScene scene(graph, hierarchyBytes, 256ull * 1024 * 1024);
    scene.registerObjectForStreaming(objectName, assetPath.string());
    scene.instantiateObjectInstance(
        objectName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
    scene.updateInstanceBuffer();

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
      .name = "Wall_Depth.texture",
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

    Texture idsLo = makeColorTexture("Wall_PackedGeometryIdsLo.texture", Format::Format_R32Uint);
    Texture idsHi = makeColorTexture("Wall_PackedGeometryIdsHi.texture", Format::Format_R32Uint);
    Texture materialId = makeColorTexture("Wall_MaterialId.texture", Format::Format_R32Uint);
    Texture materialUV = makeColorTexture("Wall_MaterialUV.texture", Format::Format_RG32Float);
    Texture motionVector = makeColorTexture("Wall_MotionVector.texture", Format::Format_RG16Float);
    Texture baseColor = makeColorTexture("Wall_BaseColor.texture", Format::Format_RGBA16Float);
    Texture materialProperties = makeColorTexture("Wall_MaterialProperties.texture", Format::Format_RGBA16Float);
    Texture emissive = makeColorTexture("Wall_Emissive.texture", Format::Format_RGBA16Float);
    Texture geometryNormal = makeColorTexture("Wall_GeometryNormal.texture", Format::Format_RGBA16Float);
    Texture normal = makeColorTexture("Wall_Normal.texture", Format::Format_RGBA16Float);
    Texture color = makeColorTexture("Wall_Color.texture", Format::Format_RGBA16Float);

    VirtualGeometryCullingMultipleDispatchesPass::Settings cullingSettings;
    cullingSettings.maxHierarchyLevels = 32u;
    cullingSettings.clustersQueueBufferSize = kMaxQueueElements * sizeof(uint32_t) * 4u;
    cullingSettings.errorTreshhold = kFinestErrorBudget;
    cullingSettings.viewPortWidth = kImageSide;
    cullingSettings.viewPortHeight = kImageSide;
    // No depth pre-pass here, so the HiZ pyramid holds nothing usable.
    cullingSettings.enableOcclusionCulling = false;

    graph->registerPass<VirtualGeometryPageDecodePass>("wall_pageDecode", 0, scene);
    auto *culling = graph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(
        "wall_culling", 1, scene, cullingSettings, depthTexture);

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
    // The wall is a single quad layer seen head on; which side counts as front
    // depends on the winding convention, and this test is about what the cut
    // selects rather than about backface culling.
    drawSettings.cullMode = CullMode::None;
    // MoltenVK has no vkCmdDrawIndirectCount, so the pass has to fall back to a
    // direct draw and the DRAW_INDIRECT_COUNT_DISABLED shader variant. Without
    // this the culler selects clusters and the raster emits nothing.
    drawSettings.directDrawWhenIndirectCountUnavailable = true;
    auto *hardwareDraw = kDrawPassEnabled ? graph->registerPass<VirtualGeometryHardwareDrawPass>(
        "wall_hardwareDraw",
        2,
        scene,
        culling->getHWDrawIndirectBuffer(),
        culling->getHWVisibleClusterInfosBuffer(),
        culling->getCullingStatisticsBuffer(),
        frameTarget,
        /*shouldClearDepth=*/true,
        0.0f,
        drawSettings) : nullptr;
    (void)hardwareDraw;

    const uint64_t idBytes =
        static_cast<uint64_t>(kImageSide) * kImageSide * sizeof(uint32_t) * 2u;
    Buffer idBuffer = graph->createBuffer(BufferInfo{
      .name = "Wall_Ids.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc,
    });
    Buffer idStaging = graph->createBuffer(BufferInfo{
      .name = "Wall_IdsStaging.buffer",
      .size = idBytes,
      .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
    });
    if (kReadbackEnabled)
    {
      graph->registerPass<virtualgeometry::gpgpu::VirtualGeometryIdReadbackPass>(
          "wall_idReadback", 3, idsLo, idsHi, idBuffer, kImageSide, kImageSide);
      graph->registerPass<rendering::gpgpu::CopyBufferPass>(
          "wall_copyIds", 4, idBuffer, 0, idBytes, idStaging, 0, idBytes);
    }
    graph->compile();

    // Frame the wall: a perspective camera far enough back that the 64-voxel
    // wall fills the 64 pixel target almost exactly.
    const float halfWall = static_cast<float>(kWallSide) * 0.5f;
    const float fovY = 60.0f * 3.14159265f / 180.0f;
    const float distance = halfWall / std::tan(fovY * 0.5f);
    uint32_t frameSlot = 0u;

    const auto renderFrame = [&]() -> RenderedImage
    {
      // Head on: the wall fills the square target, one finest voxel per pixel.
      const math::Vec3f eyePosition(0.0f, 0.0f, distance);
      const math::Vec3f lookTarget(0.0f, 0.0f, 0.0f);
      Camera camera(fovY, eyePosition, math::Vec3f(0.0f, 0.0f, -1.0f), true);
      camera.setAspectRatio(1.0f);
      camera.setNearFar(0.5f, 4000.0f);
      camera.lookAt(lookTarget);
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
      culling->updateUniforms(
          viewArray, projectionArray, viewPosition,
          kImageSide, kImageSide, 0.5f, 4000.0f, 1u);
      if (hardwareDraw != nullptr)
        // The error threshold is a defaulted trailing parameter here and the
        // meshlet vertex shader re-runs LOD selection with it. Leaving it at its
        // 0.0 default makes every cluster fail `projSelfError >= error`, so the
        // culler selects clusters and the raster discards all of them.
        hardwareDraw->updateUniforms(
            viewArray, projectionArray, viewPosition,
            kImageSide, kImageSide, 0.5f, 4000.0f, 1u, kFinestErrorBudget);

      scene.prepareFrame(frameSlot);
      RenderGraph::Overrides overrides;
      scene.appendFrameOverrides(overrides, frameSlot);
      culling->appendFrameOverrides(overrides, frameSlot);
      if (hardwareDraw != nullptr)
        hardwareDraw->appendFrameOverrides(overrides, frameSlot);
      RenderGraph::Frame frame;
      graph->run(frame, overrides);
      graph->waitFrame(frame);
      frameSlot = (frameSlot + 1u) % graph->getMaxFramesInFlight();

      std::vector<uint32_t> packed(kImageSide * kImageSide * 2u, 0u);
      if (kReadbackEnabled)
        graph->bufferRead(
            idStaging, 0, idBytes,
            [&](const void *data) { std::memcpy(packed.data(), data, idBytes); });
      RenderedImage image = summarize(packed);
      CullingCounters counters{};
      graph->bufferRead(
          culling->getCullingStatisticsBuffer(), 0, sizeof(CullingCounters),
          [&](const void *data) { std::memcpy(&counters, data, sizeof(CullingCounters)); });
      image.visibleClusters = counters.visibleClusterHardwareCount;
      std::vector<PageTableEntry> pageTable(pageCount);
      graph->bufferRead(
          scene.pageTableBuffer, 0, pageCount * sizeof(PageTableEntry),
          [&](const void *data)
          { std::memcpy(pageTable.data(), data, pageCount * sizeof(PageTableEntry)); });
      for (const auto &entry : pageTable)
        image.installedPages += entry.isInstalled != 0u ? 1u : 0u;
      image.hierarchyQueue = counters.hierarchyQueueSize;
      image.clusterQueue = counters.clusterQueueSize;
      return image;
    };

    {
      std::vector<VirtualGeometryHierarchy> gpuHierarchy(hierarchySize);
      graph->bufferRead(
          scene.hierarchyBuffer, 0,
          hierarchySize * sizeof(VirtualGeometryHierarchy),
          [&](const void *data)
          { std::memcpy(gpuHierarchy.data(), data, hierarchySize * sizeof(VirtualGeometryHierarchy)); });
      std::vector<VirtualGeometryInstanceGPUData> instances(
          std::max<size_t>(1u, scene.getInstanceCount()));
      graph->bufferRead(
          scene.instanceBuffer, 0,
          instances.size() * sizeof(VirtualGeometryInstanceGPUData),
          [&](const void *data)
          {
            std::memcpy(
                instances.data(), data,
                instances.size() * sizeof(VirtualGeometryInstanceGPUData));
          });
      for (size_t index = 0u; index < instances.size(); ++index)
        os::Logger::logf(
            "  instance %zu: hierarchyStart=%u pageTableOffset=%u quant=%u "
            "unitScaleBits=0x%x material=%u model=(%.2f,%.2f,%.2f)",
            index, instances[index].hierarchyStartOffset,
            instances[index].pageTableOffset, instances[index].quantization_factor,
            instances[index].unit_scale_bits, instances[index].materialIndex,
            instances[index].modelMatrix.at(0, 3), instances[index].modelMatrix.at(1, 3),
            instances[index].modelMatrix.at(2, 3));

      uint32_t nonZeroNodes = 0u;
      for (const auto &node : gpuHierarchy)
        nonZeroNodes += (node.child_count != 0u || node.max_radius != 0.0f) ? 1u : 0u;
      const auto &root = gpuHierarchy[0];
      os::Logger::logf(
          "  hierarchy nodes=%u nonZero=%u rootNodeIndex=%u instances=%zu",
          hierarchySize, nonZeroNodes, encoded.root_node_index, scene.getInstanceCount());
      os::Logger::logf(
          "  root: flags=0x%x childStart=%u childCount=%u page=0x%x radius=%.3f "
          "center=(%.2f,%.2f,%.2f) minErr=%.4f maxParentErr=%.4f aabb=(%.1f,%.1f,%.1f)-(%.1f,%.1f,%.1f)",
          root.flags, root.child_start, root.child_count, root.pageIndex, root.max_radius,
          root.max_center_x, root.max_center_y, root.max_center_z,
          root.min_lod_error, root.max_parent_lod_error,
          root.min_x, root.min_y, root.min_z, root.max_x, root.max_y, root.max_z);
    }

    os::Logger::log("streaming the wall in, one page per update");
    std::vector<RenderedImage> frames;
    frames.push_back(renderFrame());
    for (uint32_t update = 0u; update < pageCount * 4u; ++update)
    {
      scene.updatePageStreaming(objectName, 1u);
      scene.updateInstanceBuffer();
      frames.push_back(renderFrame());
    }

    std::ostringstream coverage;
    std::ostringstream detail;
    std::ostringstream selected;
    for (const RenderedImage &frame : frames)
    {
      coverage << ' ' << frame.coveredPixels;
      detail << ' ' << frame.distinctIds;
      selected << ' ' << frame.visibleClusters;
    }
    std::ostringstream resident;
    std::ostringstream queues;
    for (const RenderedImage &frame : frames)
    {
      resident << ' ' << frame.installedPages;
      queues << ' ' << frame.hierarchyQueue << '/' << frame.clusterQueue;
    }
    os::Logger::logf("  installed pages:%s", resident.str().c_str());
    os::Logger::logf("  hierarchy/cluster queue:%s", queues.str().c_str());
    os::Logger::logf("  visible clusters:%s", selected.str().c_str());
    os::Logger::logf("  covered pixels:%s", coverage.str().c_str());
    os::Logger::logf("  distinct surfaces:%s", detail.str().c_str());

    // Write the first, middle and settled frames out so the picture can be
    // looked at rather than only counted.
    writePPM(frames.front(), outputDirectory / "wall-000-root-page.ppm");
    writePPM(frames[frames.size() / 2u], outputDirectory / "wall-001-half-streamed.ppm");
    writePPM(frames.back(), outputDirectory / "wall-002-settled.ppm");
    os::Logger::logf("  wrote images to %s", outputDirectory.string().c_str());

    // The wall fills the target, and the root page's streaming-leaf clusters
    // already stretch over all of it, so coverage is complete from the first
    // frame and stays complete. What grows is the detail inside it.
    const uint32_t targetPixels = kImageSide * kImageSide;
    check(
        frames.front().coveredPixels == targetPixels,
        "the first frame already covers the whole wall from the root page");
    check(
        frames.back().coveredPixels == targetPixels,
        "the settled wall still covers every pixel");
    check(
        frames.back().distinctIds > frames.front().distinctIds * 4u,
        "streaming multiplies the number of distinct surfaces on screen");

    bool coverageAlwaysComplete = true;
    bool detailNeverRegresses = true;
    for (size_t index = 0u; index < frames.size(); ++index)
    {
      coverageAlwaysComplete =
          coverageAlwaysComplete && frames[index].coveredPixels == targetPixels;
      if (index > 0u)
        detailNeverRegresses =
            detailNeverRegresses && frames[index].distinctIds >= frames[index - 1u].distinctIds;
    }
    check(
        coverageAlwaysComplete,
        "no streaming step ever leaves a hole in the wall");
    check(detailNeverRegresses, "the number of distinct surfaces never falls while streaming");
    // Every voxel face is two triangles and the target is one pixel per voxel,
    // so a pixel can only ever show one of them: the settled image approaches
    // one distinct surface per pixel from below.
    check(
        frames.back().distinctIds * 2u >= targetPixels,
        "the settled image approaches one distinct voxel surface per pixel: " +
            std::to_string(frames.back().distinctIds) + " over " +
            std::to_string(targetPixels) + " pixels");

    const RenderedImage repeat = renderFrame();
    check(repeat.hash == frames.back().hash, "the settled frame is deterministic");
  }
  catch (const std::exception &exception)
  {
    os::Logger::errorf("EXCEPTION: %s", exception.what());
    exitCode = 1;
  }

  delete rhi;
  if (failures != 0 || exitCode != 0)
  {
    os::Logger::errorf("%d sparse voxel octree wall GPU checks failed", failures);
    os::Logger::shutdown();
    return 1;
  }
  os::Logger::log("all sparse voxel octree wall GPU checks passed");
  os::Logger::shutdown();
  return 0;
}
