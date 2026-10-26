#include "globalIllumination/worldgi/WorldGITypes.hpp"
#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpgpu/LightingTemporalAccumulationPass.hpp"
#include "rendering/gpgpu/SvgfAtrousFilterPass.hpp"
#include "rendering/gpgpu/SvgfDenoisePass.hpp"
#include "rendering/gpgpu/TemporalSuperResolutionPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGPU.hpp"
#include "voxel/svraster/rendering/SparseVoxelColorReadbackPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDebugPresentPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelGBufferReadbackPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDIReservoirPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDISpatialReservoirPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelLightFieldPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneHitTemporalPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayCompactionPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayCreatePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayWorklistPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayStatisticsPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayStatisticsResetPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneScreenTracePass.hpp"
#include "voxel/svraster/rendering/SparseVoxelSceneRayTree64Passes.hpp"

#include <array>
// These tests use assert() as their check mechanism, and Release builds
// define NDEBUG. Without this the assertions vanish - and because many of
// them wrap the call they are checking, the call under test vanishes with
// them, leaving objects unconfigured and the suite crashing or silently
// passing. cassert re-reads NDEBUG on every include, so undefining it here
// restores assert regardless of what included cassert earlier.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{

using namespace ::rendering;
using namespace globalillumination::worldgi;
using namespace voxel::svraster;
namespace svrRendering = voxel::svraster::rendering;

constexpr uint32_t kGeneration = 17u;

uint32_t reachabilityMix(uint32_t value)
{
  value = (value ^ (value >> 16u)) * 0x7feb352du;
  value = (value ^ (value >> 15u)) * 0x846ca68bu;
  return value ^ (value >> 16u);
}

uint32_t positiveYOriginReachabilityKey()
{
  // Region cell (0,0,0), dominant receiver face +Y (face 3).
  const uint32_t key = reachabilityMix(3u * 0xc2b2ae35u);
  return key == 0u ? 1u : key;
}

std::array<float, 16> identityMatrix()
{
  return {
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    1.0f,
  };
}

template <typename T> Buffer uploadBuffer(RenderGraph &graph, const std::string &name, const std::vector<T> &values, BufferUsage usage)
{
  BufferUsage resolvedUsage = usage;
  if ((usage & BufferUsage::BufferUsage_Pull) == 0u)
    resolvedUsage = resolvedUsage | BufferUsage::BufferUsage_Push;
  const Buffer result = graph.createBuffer({
    .name = name,
    .size = std::max<uint64_t>(sizeof(T), values.size() * sizeof(T)),
    .usage = resolvedUsage,
  });
  if (!values.empty() && (usage & BufferUsage::BufferUsage_Pull) == 0u)
    graph.bufferWrite(result, 0u, values.size() * sizeof(T), const_cast<T *>(values.data()));
  return result;
}

uint32_t textureComponentCount(const Format format)
{
  switch (format)
  {
  case Format::Format_RG32Float:
  case Format::Format_RG32Uint:
    return 2u;
  case Format::Format_RGBA32Float:
  case Format::Format_RGBA32Uint:
    return 4u;
  default:
    return 1u;
  }
}

Texture uploadFloatTexture(RenderGraph &graph, const std::string &name, Format format, uint32_t width, uint32_t height, const std::vector<float> &values)
{
  const uint64_t pixelCount = uint64_t{width} * height;
  const uint64_t expectedValueCount =
      pixelCount * textureComponentCount(format);
  std::vector<float> expandedValues;
  const std::vector<float> *uploadValues = &values;
  if (values.empty())
  {
    expandedValues.assign(expectedValueCount, 0.0f);
    uploadValues = &expandedValues;
  }
  else if (values.size() == pixelCount && expectedValueCount != pixelCount)
  {
    expandedValues.assign(expectedValueCount, 0.0f);
    const uint32_t componentCount = textureComponentCount(format);
    for (uint64_t pixel = 0u; pixel < pixelCount; ++pixel)
      expandedValues[pixel * componentCount] = values[pixel];
    uploadValues = &expandedValues;
  }
  if (uploadValues->size() < expectedValueCount)
    throw std::runtime_error(
        "uploadFloatTexture fixture payload is smaller than " + name);
  const Texture result = graph.createTexture({
    .name = name,
    .format = format,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Sampled,
  });
  try
  {
    graph.textureWrite(
        result, uploadValues->size() * sizeof(float),
        const_cast<float *>(uploadValues->data()));
  }
  catch (const std::exception &exception)
  {
    throw std::runtime_error(
        "uploadFloatTexture failed for " + name + ": " + exception.what());
  }
  return result;
}

Texture uploadUintTexture(
    RenderGraph &graph, const std::string &name, uint32_t width,
    uint32_t height, const std::vector<uint32_t> &values,
    const Format format = Format::Format_R32Uint)
{
  const uint64_t pixelCount = uint64_t{width} * height;
  const uint64_t expectedValueCount =
      pixelCount * textureComponentCount(format);
  std::vector<uint32_t> expandedValues;
  const std::vector<uint32_t> *uploadValues = &values;
  if (values.empty())
  {
    expandedValues.assign(expectedValueCount, 0u);
    uploadValues = &expandedValues;
  }
  else if (values.size() == pixelCount && expectedValueCount != pixelCount)
  {
    expandedValues.assign(expectedValueCount, 0u);
    const uint32_t componentCount = textureComponentCount(format);
    for (uint64_t pixel = 0u; pixel < pixelCount; ++pixel)
      expandedValues[pixel * componentCount] = values[pixel];
    uploadValues = &expandedValues;
  }
  if (uploadValues->size() < expectedValueCount)
    throw std::runtime_error(
        "uploadUintTexture fixture payload is smaller than " + name);
  const Texture result = graph.createTexture({
    .name = name,
    .format = format,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Sampled,
  });
  try
  {
    graph.textureWrite(
        result, uploadValues->size() * sizeof(uint32_t),
        const_cast<uint32_t *>(uploadValues->data()));
  }
  catch (const std::exception &exception)
  {
    throw std::runtime_error(
        "uploadUintTexture failed for " + name + ": " + exception.what());
  }
  return result;
}

Texture reservoirSidebandTexture(
    RenderGraph &graph, const std::string &name, uint32_t width,
    uint32_t height, const std::vector<uint32_t> &values = {})
{
  const uint64_t pixelCount = uint64_t{width} * height;
  std::vector<uint32_t> expandedValues;
  const std::vector<uint32_t> *uploadValues = &values;
  if (values.empty())
  {
    expandedValues.assign(pixelCount * 4u, 0u);
    uploadValues = &expandedValues;
  }
  else if (values.size() == pixelCount)
  {
    expandedValues.assign(pixelCount * 4u, 0u);
    for (uint64_t pixel = 0u; pixel < pixelCount; ++pixel)
      expandedValues[pixel * 4u] = values[pixel];
    uploadValues = &expandedValues;
  }
  if (uploadValues->size() < pixelCount * 4u)
    throw std::runtime_error(
        "reservoirSidebandTexture fixture payload is smaller than " + name);
  const Texture result = graph.createTexture({
    .name = name,
    .format = Format::Format_RGBA32Uint,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });
  try
  {
    graph.textureWrite(
        result, uploadValues->size() * sizeof(uint32_t),
        const_cast<uint32_t *>(uploadValues->data()));
  }
  catch (const std::exception &exception)
  {
    throw std::runtime_error(
        "reservoirSidebandTexture failed for " + name + ": " +
        exception.what());
  }
  return result;
}

uint32_t floatBits(const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

uint32_t packOctahedralNormal(const std::array<float, 3u> &normal)
{
  const float length = std::sqrt(
      normal[0] * normal[0] + normal[1] * normal[1] +
      normal[2] * normal[2]);
  const std::array<float, 3u> n =
      length > 1.0e-6f
          ? std::array<float, 3u>{
                normal[0] / length, normal[1] / length, normal[2] / length}
          : std::array<float, 3u>{0.0f, 1.0f, 0.0f};
  const float denominator =
      std::max(std::abs(n[0]) + std::abs(n[1]) + std::abs(n[2]), 1.0e-6f);
  float octX = n[0] / denominator;
  float octY = n[1] / denominator;
  if (n[2] < 0.0f)
  {
    const float foldedX =
        (1.0f - std::abs(octY)) * (octX >= 0.0f ? 1.0f : -1.0f);
    const float foldedY =
        (1.0f - std::abs(octX)) * (octY >= 0.0f ? 1.0f : -1.0f);
    octX = foldedX;
    octY = foldedY;
  }
  const auto quantize = [](const float value)
  {
    const float normalized =
        std::max(0.0f, std::min(1.0f, value * 0.5f + 0.5f));
    return static_cast<uint32_t>(std::round(normalized * 65535.0f));
  };
  return quantize(octX) | (quantize(octY) << 16u);
}

uint32_t reservoirCoreStateWord()
{
  return 1u | (1u << 8u) | ((kGeneration & 0x007fffffu) << 9u);
}

SparseVoxelSceneRayGPU activeRay(uint32_t pixel, float originX = 0.0f)
{
  SparseVoxelSceneRayGPU ray{};
  ray.originMinimumDistance = {originX, 0.0f, 0.0f, 0.0f};
  ray.directionMaximumDistance = {0.0f, 1.0f, 0.0f, 64.0f};
  ray.pixelRayLight = {pixel, 0u, 0u, 0u};
  ray.seedFlags = {
    100u + pixel,
    kSparseVoxelSceneRayFlagActive | kSparseVoxelSceneRayFlagShadow,
    kGeneration,
    0u,
  };
  ray.lightSamplePdfWeight = {1.0f, 1.0f, 1.0f, 0.0f};
  return ray;
}

WorldGILightGPU directionalLight()
{
  WorldGILightGPU light{};
  light.directionType[1] = -1.0f;
  for (uint32_t channel = 0u; channel < 4u; ++channel)
    light.colorIntensity[channel] = 1.0f;
  light.flags[0] = WorldGILightFlag_Enabled;
  light.flags[1] = WorldGILightType_Directional;
  light.flags[2] = 0u;
  light.flags[3] = kGeneration;
  return light;
}

WorldGILightGPU skyLight()
{
  WorldGILightGPU light{};
  light.directionType[1] = 1.0f;
  for (uint32_t channel = 0u; channel < 4u; ++channel)
    light.colorIntensity[channel] = 1.0f;
  light.flags[0] = WorldGILightFlag_Enabled;
  light.flags[1] = WorldGILightType_Sky;
  light.flags[2] = 0u;
  light.flags[3] = kGeneration;
  return light;
}

SparseVoxelDIReservoirGPU previousReservoir(std::array<float, 3> normal, float depth, uint32_t instanceId, uint32_t voxelId)
{
  SparseVoxelDIReservoirGPU reservoir{};
  reservoir.identity = {0u, 0u, 3u, kGeneration};
  reservoir.weights = {3.0f, 1.0f, 1.0f, 1.0f};
  reservoir.state = {700u, 2u, 1u, 0u};
  reservoir.receiverNormalDepth = {normal[0], normal[1], normal[2], depth};
  reservoir.receiverIdentity = {instanceId, voxelId, 1u, 0u};
  return reservoir;
}

void runGraph(RenderGraph &graph)
{
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
}

void testCombinedSceneRayWorklistReset(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  RenderGraph graph(&rhi);
  const Buffer candidates = uploadBuffer(
      graph, "CombinedReset.Candidates",
      std::vector<SparseVoxelSceneRayGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer indices = uploadBuffer(
      graph, "CombinedReset.Indices", std::vector<uint32_t>(1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer primary = uploadBuffer(
      graph, "CombinedReset.Primary", std::vector<uint32_t>(4u, 9u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect |
          BufferUsage::BufferUsage_CopySrc);
  const Buffer secondary = uploadBuffer(
      graph, "CombinedReset.Secondary", std::vector<uint32_t>(4u, 7u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect |
          BufferUsage::BufferUsage_CopySrc);
  const Buffer rays = uploadBuffer(
      graph, "CombinedReset.Rays",
      std::vector<SparseVoxelSceneRayGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer reservoirs = uploadBuffer(
      graph, "CombinedReset.Reservoirs",
      std::vector<SparseVoxelDIReservoirGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(
      graph, "CombinedReset.Readback", std::vector<uint32_t>(8u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelSceneRayWorklistPass>(
      "CombinedReset.Reset", passIndex++,
      svrRendering::SparseVoxelSceneRayWorklistPass::Stage::Reset,
      svrRendering::SparseVoxelSceneRayWorklistPass::Buffers{
        candidates, indices, primary, rays, reservoirs, secondary},
      1u, 1u);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "CombinedReset.CopyPrimary", passIndex++, primary, 0u,
      4u * sizeof(uint32_t), readback, 0u, 4u * sizeof(uint32_t));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "CombinedReset.CopySecondary", passIndex++, secondary, 0u,
      4u * sizeof(uint32_t), readback, 4u * sizeof(uint32_t),
      4u * sizeof(uint32_t));
  runGraph(graph);

  std::array<uint32_t, 8u> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data) { std::memcpy(actual.data(), data, sizeof(actual)); });
  constexpr std::array<uint32_t, 4u> expected{0u, 1u, 1u, 0u};
  assert(std::equal(expected.begin(), expected.end(), actual.begin()));
  assert(std::equal(expected.begin(), expected.end(), actual.begin() + 4u));
}

void testTerrainRayOriginIgnoresPageLODAndBias(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 2u;
  constexpr float finestCell = 0.1f;
  constexpr float biasCells = 0.125f;
  RenderGraph graph(&rhi);

  const Buffer coverage = uploadBuffer(
      graph, "TerrainBias.Coverage", std::vector<uint32_t>(width, 1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph, "TerrainBias.Depth", std::vector<float>(width, 10.0f),
      BufferUsage::BufferUsage_Storage);
  const Buffer rays = uploadBuffer(
      graph, "TerrainBias.Rays",
      std::vector<SparseVoxelSceneRayGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);

  WorldGILightGPU sky{};
  sky.directionType[1] = 1.0f;
  for (float &channel : sky.colorIntensity)
    channel = 1.0f;
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[3] = kGeneration;
  const Buffer lights = uploadBuffer(
      graph, "TerrainBias.Lights", std::vector<WorldGILightGPU>{sky},
      BufferUsage::BufferUsage_Storage);
  const Buffer bvhNodes = uploadBuffer(
      graph, "TerrainBias.BVHNodes",
      std::vector<WorldGILightBVHNodeGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  WorldGILightBVHInfoGPU info{};
  info.lightCount = 1u;
  info.infiniteLightCount = 1u;
  const Buffer bvhInfo = uploadBuffer(
      graph, "TerrainBias.BVHInfo",
      std::vector<WorldGILightBVHInfoGPU>{info},
      BufferUsage::BufferUsage_Storage);

  const Texture normalTexture = uploadFloatTexture(
      graph, "TerrainBias.Normal", Format::Format_RGBA32Float, width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 0.25f});
  const Texture lightFieldTexture = graph.createTexture({
    .name = "TerrainBias.LightField",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  constexpr std::array<uint32_t, width> mappingIds{101u, 202u};
  const Buffer ownershipIds = uploadBuffer(
      graph, "TerrainBias.OwnershipIds",
      std::vector<uint32_t>(mappingIds.begin(), mappingIds.end()),
      BufferUsage::BufferUsage_Storage);
  std::vector<SparseVoxelTerrainOwnershipGPU> ownership(8u);
  for (auto &entry : ownership)
    entry.physicalSlot = kInvalidSparseVoxelIndex;
  for (uint32_t index = 0u; index < mappingIds.size(); ++index)
  {
    SparseVoxelTerrainOwnershipGPU owner{};
    owner.regionMappingId = mappingIds[index];
    owner.spatialLOD = index == 0u ? 0u : 5u;
    owner.physicalSlot = index;
    uint32_t bucket = reachabilityMix(owner.regionMappingId) &
        (static_cast<uint32_t>(ownership.size()) - 1u);
    while (ownership[bucket].physicalSlot != kInvalidSparseVoxelIndex)
      bucket = (bucket + 1u) &
          (static_cast<uint32_t>(ownership.size()) - 1u);
    ownership[bucket] = owner;
  }
  const Buffer ownershipTable = uploadBuffer(
      graph, "TerrainBias.OwnershipTable", ownership,
      BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(
      graph, "TerrainBias.Readback",
      std::vector<SparseVoxelSceneRayGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  raster.focalPrincipal = {65536.0f, 65536.0f, 0.0f, 0.0f};
  raster.viewportTiles = {width, 1u, 1u, 1u};

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
      "TerrainBias.Create", passIndex++,
      svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
        .coverage = coverage,
        .linearDepth = depth,
        .rays = rays,
        .lights = lights,
        .lightBVHNodes = bvhNodes,
        .lightBVHInfo = bvhInfo,
        .geometryNormal = normalTexture,
        .lightFieldHistory = lightFieldTexture,
        .terrainOwnershipIds = ownershipIds,
        .terrainOwnershipTable = ownershipTable,
      },
      svrRendering::SparseVoxelSceneRayCreatePass::Settings{
        .raster = raster,
        .maximumDistance = 64.0f,
        .originBias = finestCell * biasCells,
        .width = width,
        .height = 1u,
        .candidatesPerPixel = 1u,
        .lightCapacity = 1u,
        .generation = kGeneration,
        .sourceWidth = width,
        .sourceHeight = 1u,
        .skyMode = 1u,
        .traceCellSize = finestCell,
        .terrainFinestCellSize = finestCell,
      });
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "TerrainBias.Copy", passIndex++, rays, 0u,
      uint64_t{width} * sizeof(SparseVoxelSceneRayGPU), readback, 0u,
      uint64_t{width} * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::array<SparseVoxelSceneRayGPU, width> result{};
  graph.bufferRead(
      readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(result.data(), data, sizeof(result)); });
  // Even a legacy nonzero bias setting and two very different terrain LODs
  // must leave the ray on the reconstructed receiver surface.
  assert(result[0].originMinimumDistance[3] == 0.0f);
  assert(result[1].originMinimumDistance[3] == 0.0f);
  // Normal-buffer alpha is material sidecar data, not an analytic-visibility
  // sentinel. Low-alpha terrain receivers must still emit real shadow rays;
  // Tree64 skips the authored receiver cell instead of disabling visibility.
  assert(result[0].directionMaximumDistance[3] == 64.0f);
  assert(result[1].directionMaximumDistance[3] == 64.0f);
  for (uint32_t pixel = 0u; pixel < width; ++pixel)
  {
    const std::array<float, 3> cameraDirection{
      (static_cast<float>(pixel) + 0.5f) / raster.focalPrincipal[0],
      0.5f / raster.focalPrincipal[1],
      1.0f,
    };
    const float inverseLength = 1.0f / std::sqrt(
        cameraDirection[0] * cameraDirection[0] +
        cameraDirection[1] * cameraDirection[1] + 1.0f);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      assert(std::abs(
          result[pixel].originMinimumDistance[axis] -
          cameraDirection[axis] * inverseLength * 10.0f) < 1.0e-5f);
  }
}

void testReachabilityCacheDoesNotBiasReSTIR(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 1024u;
  constexpr uint32_t lightCapacity = 2u;
  constexpr uint32_t cacheEntries = 64u;
  RenderGraph graph(&rhi);

  const Buffer coverage = uploadBuffer(
      graph, "Reachability.Coverage", std::vector<uint32_t>(width, 1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph, "Reachability.Depth", std::vector<float>(width, 1.0f),
      BufferUsage::BufferUsage_Storage);
  const Buffer rays = uploadBuffer(
      graph, "Reachability.Rays",
      std::vector<SparseVoxelSceneRayGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);

  WorldGILightGPU point{};
  point.positionRange[1] = 10.0f;
  point.positionRange[2] = 1.0f;
  point.positionRange[3] = 100.0f;
  for (float &channel : point.colorIntensity)
    channel = 1.0f;
  point.flags[0] = WorldGILightFlag_Enabled;
  point.flags[1] = WorldGILightType_Point;
  point.flags[3] = kGeneration;
  WorldGILightGPU sky{};
  for (float &channel : sky.positionRange)
    channel = 1.0f;
  sky.positionRange[3] = 0.0f;
  sky.directionType[1] = 1.0f;
  for (float &channel : sky.colorIntensity)
    channel = 1.0f;
  for (float &channel : sky.shapeParams)
    channel = 1.0f;
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[2] = 1u;
  sky.flags[3] = kGeneration;
  const Buffer lights = uploadBuffer(
      graph, "Reachability.Lights",
      std::vector<WorldGILightGPU>{point, sky},
      BufferUsage::BufferUsage_Storage);
  const Buffer bvhNodes = uploadBuffer(
      graph, "Reachability.BVHNodes",
      std::vector<WorldGILightBVHNodeGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  WorldGILightBVHInfoGPU info{};
  info.lightCount = lightCapacity;
  info.infiniteLightCount = 1u;
  const Buffer bvhInfo = uploadBuffer(
      graph, "Reachability.BVHInfo",
      std::vector<WorldGILightBVHInfoGPU>{info},
      BufferUsage::BufferUsage_Storage);

  const uint32_t stride = lightCapacity + 2u;
  std::vector<uint32_t> cache(cacheEntries * stride, 0u);
  const uint32_t key = positiveYOriginReachabilityKey();
  const uint32_t base = (key % cacheEntries) * stride;
  cache[base] = kGeneration;
  cache[base + 1u] = key;
  cache[base + 2u] = 100u;                  // point: 0 / 100 visible
  cache[base + 3u] = (100u << 16u) | 100u; // sky: 100 / 100 visible
  const Buffer reachability = uploadBuffer(
      graph, "Reachability.Cache", cache, BufferUsage::BufferUsage_Storage);

  std::vector<float> normals(static_cast<size_t>(width) * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < width; ++pixel)
    normals[static_cast<size_t>(pixel) * 4u + 1u] = 1.0f;
  const Texture normalTexture = uploadFloatTexture(
      graph, "Reachability.Normal", Format::Format_RGBA32Float,
      width, 1u, normals);
  const Texture lightFieldTexture = graph.createTexture({
    .name = "Reachability.LightField",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });
  const Buffer readback = uploadBuffer(
      graph, "Reachability.Readback",
      std::vector<SparseVoxelSceneRayGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  raster.focalPrincipal = {
    65536.0f, 65536.0f, 0.0f, 0.0f};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  svrRendering::SparseVoxelSceneRayCreatePass::Settings settings{};
  settings.raster = raster;
  settings.maximumDistance = 64.0f;
  settings.originBias = 0.001f;
  settings.width = width;
  settings.height = 1u;
  // Match the production-quality initial RIS count while retaining one
  // streamed output reservoir per pixel.
  settings.candidatesPerPixel = 8u;
  settings.lightCapacity = lightCapacity;
  settings.generation = kGeneration;
  settings.sourceWidth = width;
  settings.sourceHeight = 1u;
  settings.samplingDivisor = 1u;
  settings.reachabilityEntryCount = cacheEntries;
  settings.reachabilityRegionSize = 64.0f;
  settings.reachabilityExplorationFloor = 1.0f / 64.0f;

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
      "Reachability.Create", passIndex++,
      svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
        coverage, depth, rays, lights, bvhNodes, bvhInfo, normalTexture,
        lightFieldTexture, reachability},
      settings);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "Reachability.Copy", passIndex++, rays, 0u,
      uint64_t{width} * sizeof(SparseVoxelSceneRayGPU), readback, 0u,
      uint64_t{width} * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::vector<SparseVoxelSceneRayGPU> results(width);
  graph.bufferRead(
      readback, 0u, uint64_t{width} * sizeof(SparseVoxelSceneRayGPU),
      [&](const void *data)
      {
        std::memcpy(
            results.data(), data,
            results.size() * sizeof(SparseVoxelSceneRayGPU));
      });
  uint32_t skySelections = 0u;
  for (const auto &ray : results)
  {
    assert(ray.seedFlags[3] == key);
    skySelections += ray.pixelRayLight[3] == 1u ? 1u : 0u;
  }
  // The simple ReSTIR proposal distribution does not consume cached binary
  // visibility. This fixture's ordinary target function strongly favours the
  // nearby point; the cache must not override it and force the sky selection.
  assert(skySelections < width / 20u);
}

void testMotionReprojectionAndTemporalValidation(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 5u;
  constexpr uint32_t height = 1u;
  constexpr uint32_t pixelCount = width * height;
  RenderGraph graph(&rhi);
  std::vector<SparseVoxelSceneRayGPU> candidates(pixelCount);
  std::vector<SparseVoxelSceneRayGPU> previousRays(pixelCount);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    candidates[pixel] = activeRay(pixel);
    previousRays[pixel] = activeRay(pixel);
  }
  candidates[0].originMinimumDistance = {0.25f, 0.0f, 0.0f, 0.25f};
  candidates[0].directionMaximumDistance = {1.0f, 0.0f, 0.0f, 64.0f};
  previousRays[1].originMinimumDistance = {0.0f, 0.25f, 0.0f, 0.25f};
  previousRays[1].directionMaximumDistance = {0.0f, 1.0f, 0.0f, 64.0f};
  std::vector<SparseVoxelDIReservoirGPU> previous(pixelCount);
  previous[0] = previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 50u, 50u);
  previous[1] = previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 7u, 11u);
  previous[1].weights[3] = 100.0f;
  previous[2] = previousReservoir({1.0f, 0.0f, 0.0f}, 10.0f, 7u, 11u);
  previous[3] = previousReservoir({0.0f, 1.0f, 0.0f}, 100.0f, 7u, 11u);
  previous[4] = previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 7u, 11u);
  previous[4].identity[0] = 1u;
  previous[4].weights[3] = 100.0f;

  const Buffer candidateBuffer = uploadBuffer(graph, "SceneRayTemporal.Candidates", candidates, BufferUsage::BufferUsage_Storage);
  const Buffer previousRayBuffer = uploadBuffer(graph, "SceneRayTemporal.PreviousRays", previousRays, BufferUsage::BufferUsage_Storage);
  const Buffer previousReservoirBuffer = uploadBuffer(graph, "SceneRayTemporal.PreviousReservoirs", previous, BufferUsage::BufferUsage_Storage);
  const Buffer selectedRayBuffer = uploadBuffer(graph, "SceneRayTemporal.SelectedRays", std::vector<SparseVoxelSceneRayGPU>(pixelCount), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer reservoirBuffer = uploadBuffer(graph, "SceneRayTemporal.Reservoirs", std::vector<SparseVoxelDIReservoirGPU>(pixelCount), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer lightBuffer = uploadBuffer(
      graph,
      "SceneRayTemporal.Lights",
      std::vector<WorldGILightGPU>{directionalLight(), skyLight()},
      BufferUsage::BufferUsage_Storage);
  const Buffer depthBuffer = uploadBuffer(graph, "SceneRayTemporal.Depth", std::vector<float>(pixelCount, 10.0f), BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(graph, "SceneRayTemporal.Readback", std::vector<SparseVoxelDIReservoirGPU>(pixelCount), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer selectedReadback = uploadBuffer(
      graph,
      "SceneRayTemporal.SelectedReadback",
      std::vector<SparseVoxelSceneRayGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  const std::vector<float> normals{
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
    0.0f,
    1.0f,
  };
  const std::vector<float> motion{
    -0.25f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
    0.0f,
  };
  const Texture normalTexture = uploadFloatTexture(graph, "SceneRayTemporal.Normals", Format::Format_RGBA32Float, width, height, normals);
  const Texture motionTexture = uploadFloatTexture(graph, "SceneRayTemporal.Motion", Format::Format_RG32Float, width, height, motion);
  const Texture instanceTexture = uploadUintTexture(graph, "SceneRayTemporal.Instance", width, height, {7u, 99u, 7u, 7u, 7u});
  const Texture voxelTexture = uploadUintTexture(graph, "SceneRayTemporal.Voxel", width, height, {11u, 11u, 11u, 11u, 11u});
  const Texture reservoirReceiverTexture =
      reservoirSidebandTexture(graph, "SceneRayTemporal.ReservoirReceiver", width, height);
  const Texture reservoirCoreTexture =
      reservoirSidebandTexture(graph, "SceneRayTemporal.ReservoirCore", width, height);

  uint32_t passIndex = 0u;
  auto *reservoirPass = graph.registerPass<svrRendering::SparseVoxelDIReservoirPass>(
      "SceneRayTemporal.Resample",
      passIndex++,
      svrRendering::SparseVoxelDIReservoirPass::Buffers{
        candidateBuffer,
        previousRayBuffer,
        previousReservoirBuffer,
        selectedRayBuffer,
        reservoirBuffer,
        lightBuffer,
        depthBuffer,
        normalTexture,
        motionTexture,
        instanceTexture,
        voxelTexture,
        {},
        reservoirReceiverTexture,
        reservoirCoreTexture,
      },
      svrRendering::SparseVoxelDIReservoirPass::Settings{
        pixelCount,
        1u,
        kGeneration,
        true,
        1u,
        width,
        height,
        0.85f,
        0.05f,
        width,
        height,
        2u,
      });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRayTemporal.Copy", passIndex++, reservoirBuffer, 0u, pixelCount * sizeof(SparseVoxelDIReservoirGPU), readback, 0u, pixelCount * sizeof(SparseVoxelDIReservoirGPU));
  auto *selectedCopyPass =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "SceneRayTemporal.CopySelected",
          passIndex++,
          selectedRayBuffer,
          0u,
          pixelCount * sizeof(SparseVoxelSceneRayGPU),
          selectedReadback,
          0u,
          pixelCount * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::vector<SparseVoxelDIReservoirGPU> actual(pixelCount);
  graph.bufferRead(
      readback,
      0u,
      actual.size() * sizeof(actual[0]),
      [&](const void *data)
      {
        std::memcpy(actual.data(), data, actual.size() * sizeof(actual[0]));
      });
  // Reprojected history is accepted solely by normal/depth and its effective
  // sample count is bounded before the current candidate is merged. Pixel 1
  // deliberately changes instance identity but retains compatible geometry.
  assert(actual[0].identity[2] == 3u);
  assert(actual[1].identity[2] == 3u);
  assert(actual[2].identity[2] == 1u);
  assert(actual[3].identity[2] == 1u);
  // Sky directions are stochastic samples of a hemispheric integral. The
  // light field accumulates their resolved visibility, so ReSTIR must not add
  // previous sky samples a second time even when normal/depth history matches.
  assert(actual[4].identity[2] == 1u);
  assert(actual[0].receiverIdentity[0] == 7u);
  assert(actual[1].receiverIdentity[0] == 99u);
  assert(std::abs(actual[0].receiverNormalDepth[3] - 10.0f) < 1.0e-6f);
  std::vector<SparseVoxelSceneRayGPU> selected(pixelCount);
  graph.bufferRead(
      selectedReadback,
      0u,
      selected.size() * sizeof(selected[0]),
      [&](const void *data)
      {
        std::memcpy(
            selected.data(), data, selected.size() * sizeof(selected[0]));
      });
  assert(actual[0].state[1] != 0u);
  // Temporal reuse preserves the sampled light direction, but reconnects the
  // ray to the current receiver without restoring the removed origin bias.
  assert(
      std::abs(selected[0].originMinimumDistance[0] - 0.25f) < 1.0e-6f);
  assert(
      std::abs(selected[0].originMinimumDistance[1]) < 1.0e-6f);
  assert(std::abs(selected[0].originMinimumDistance[3]) < 1.0e-6f);
  assert(
      std::abs(selected[0].directionMaximumDistance[1] - 1.0f) < 1.0e-6f);

  delete selectedCopyPass;
  delete copyPass;
  delete reservoirPass;
}

void testMultipleReservoirLanesUseLargerCandidatePool(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 1u;
  constexpr uint32_t height = 1u;
  constexpr uint32_t candidateCount = 4u;
  constexpr uint32_t raysPerPixel = 2u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> candidates(raysPerPixel);
  for (uint32_t slot = 0u; slot < raysPerPixel; ++slot)
  {
    candidates[slot] = activeRay(0u, static_cast<float>(slot));
    candidates[slot].seedFlags[0] += slot * 101u;
    candidates[slot].pixelRayLight[2] = (2u << 16u) | slot;
    candidates[slot].lightSamplePdfWeight[3] = 2.0f;
  }
  const Buffer candidateBuffer = uploadBuffer(
      graph, "SceneRayLanes.Candidates", candidates,
      BufferUsage::BufferUsage_Storage);
  const Buffer previousRayBuffer = uploadBuffer(
      graph, "SceneRayLanes.PreviousRays",
      std::vector<SparseVoxelSceneRayGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Storage);
  const Buffer previousReservoirBuffer = uploadBuffer(
      graph, "SceneRayLanes.PreviousReservoirs",
      std::vector<SparseVoxelDIReservoirGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Storage);
  const Buffer temporalRayBuffer = uploadBuffer(
      graph, "SceneRayLanes.TemporalRays",
      std::vector<SparseVoxelSceneRayGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Storage);
  const Buffer temporalReservoirBuffer = uploadBuffer(
      graph, "SceneRayLanes.TemporalReservoirs",
      std::vector<SparseVoxelDIReservoirGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Storage);
  const Buffer spatialRayBuffer = uploadBuffer(
      graph, "SceneRayLanes.SpatialRays",
      candidates,
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer spatialReservoirBuffer = uploadBuffer(
      graph, "SceneRayLanes.SpatialReservoirs",
      std::vector<SparseVoxelDIReservoirGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer lightBuffer = uploadBuffer(
      graph, "SceneRayLanes.Lights",
      std::vector<WorldGILightGPU>{directionalLight()},
      BufferUsage::BufferUsage_Storage);
  const Buffer depthBuffer = uploadBuffer(
      graph, "SceneRayLanes.Depth", std::vector<float>{10.0f},
      BufferUsage::BufferUsage_Storage);
  const Buffer rayReadback = uploadBuffer(
      graph, "SceneRayLanes.RayReadback",
      std::vector<SparseVoxelSceneRayGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer reservoirReadback = uploadBuffer(
      graph, "SceneRayLanes.ReservoirReadback",
      std::vector<SparseVoxelDIReservoirGPU>(raysPerPixel),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture normalTexture = uploadFloatTexture(
      graph, "SceneRayLanes.Normal", Format::Format_RGBA32Float,
      width, height, {0.0f, 1.0f, 0.0f, 1.0f});
  const Texture motionTexture = uploadFloatTexture(
      graph, "SceneRayLanes.Motion", Format::Format_RG32Float,
      width, height, {0.0f, 0.0f});
  const Texture instanceTexture = uploadUintTexture(
      graph, "SceneRayLanes.Instance", width, height, {7u});
  const Texture voxelTexture = uploadUintTexture(
      graph, "SceneRayLanes.Voxel", width, height, {11u});
  const Texture reservoirReceiverTexture =
      reservoirSidebandTexture(graph, "SceneRayLanes.ReservoirReceiver", width, height);
  const Texture reservoirCoreTexture =
      reservoirSidebandTexture(graph, "SceneRayLanes.ReservoirCore", width, height);
  const Buffer activeRayIndices = uploadBuffer(
      graph, "SceneRayLanes.ActiveRayIndices", std::vector<uint32_t>{0u, 1u},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeRayState = uploadBuffer(
      graph, "SceneRayLanes.ActiveRayState", std::vector<uint32_t>{1u, 1u, 1u, raysPerPixel},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelDIReservoirPass>(
      "SceneRayLanes.Temporal", passIndex++,
      svrRendering::SparseVoxelDIReservoirPass::Buffers{
        candidateBuffer, previousRayBuffer, previousReservoirBuffer,
        temporalRayBuffer, temporalReservoirBuffer, lightBuffer, depthBuffer,
        normalTexture, motionTexture, instanceTexture, voxelTexture, {},
        reservoirReceiverTexture, reservoirCoreTexture},
      svrRendering::SparseVoxelDIReservoirPass::Settings{
        width * height, candidateCount, kGeneration, false, 1u,
        width, height, 0.85f, 0.05f, width, height, 8u, 0u, 64.0f,
        kSparseVoxelLightReachabilityEntryCount, 1.0f / 64.0f,
        raysPerPixel});
  graph.registerPass<svrRendering::SparseVoxelDISpatialReservoirPass>(
      "SceneRayLanes.Spatial", passIndex++,
      svrRendering::SparseVoxelDISpatialReservoirPass::Buffers{
        candidateBuffer, temporalRayBuffer, temporalReservoirBuffer,
        spatialRayBuffer, spatialReservoirBuffer, lightBuffer, depthBuffer,
        normalTexture, instanceTexture, voxelTexture, {},
        reservoirReceiverTexture, reservoirCoreTexture, activeRayIndices,
        activeRayState},
      svrRendering::SparseVoxelDISpatialReservoirPass::Settings{
        width, height, candidateCount, kGeneration, 1u, 0.5f, 0.85f,
        0.05f, width, height, 0u, 64.0f,
        kSparseVoxelLightReachabilityEntryCount, 1.0f / 64.0f,
        raysPerPixel});
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRayLanes.CopyRays", passIndex++, spatialRayBuffer, 0u,
      raysPerPixel * sizeof(SparseVoxelSceneRayGPU), rayReadback, 0u,
      raysPerPixel * sizeof(SparseVoxelSceneRayGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRayLanes.CopyReservoirs", passIndex++,
      spatialReservoirBuffer, 0u,
      raysPerPixel * sizeof(SparseVoxelDIReservoirGPU), reservoirReadback, 0u,
      raysPerPixel * sizeof(SparseVoxelDIReservoirGPU));
  runGraph(graph);

  std::array<SparseVoxelSceneRayGPU, raysPerPixel> selected{};
  std::array<SparseVoxelDIReservoirGPU, raysPerPixel> reservoirs{};
  graph.bufferRead(
      rayReadback, 0u, sizeof(selected),
      [&](const void *data) { std::memcpy(selected.data(), data, sizeof(selected)); });
  graph.bufferRead(
      reservoirReadback, 0u, sizeof(reservoirs),
      [&](const void *data) { std::memcpy(reservoirs.data(), data, sizeof(reservoirs)); });

  assert((selected[0].seedFlags[1] &
      kSparseVoxelSceneRayFlagActive) != 0u);
  assert((selected[1].seedFlags[1] &
      kSparseVoxelSceneRayFlagActive) != 0u);
  // Lane zero owns the reusable reservoir. Fresh lanes bypass ReSTIR and
  // remain in the final tracing buffer written by ray creation.
  assert((selected[0].pixelRayLight[2] & 1u) == 0u);
  assert((selected[1].pixelRayLight[2] & 1u) == 1u);
  assert(reservoirs[0].identity[2] == 2u);
  assert(reservoirs[1].state[2] == 0u);
}

void testSpatialReceiverValidation(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 4u;
  constexpr uint32_t height = 1u;
  RenderGraph graph(&rhi);
  std::vector<SparseVoxelSceneRayGPU> candidates(width);
  std::vector<SparseVoxelSceneRayGPU> inputRays(width);
  for (uint32_t pixel = 0u; pixel < width; ++pixel)
  {
    candidates[pixel] = activeRay(pixel, 0.1f * static_cast<float>(pixel));
    inputRays[pixel] = candidates[pixel];
  }
  std::vector<SparseVoxelDIReservoirGPU> inputReservoirs{
    previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 7u, 11u),
    previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 7u, 11u),
    previousReservoir({0.0f, 1.0f, 0.0f}, 10.0f, 99u, 11u),
  };
  for (auto &reservoir : inputReservoirs)
  {
    reservoir.identity[2] = 1u;
    reservoir.weights[0] = 1.0f;
    reservoir.state[2] = 1u;
  }

  const Buffer candidateBuffer = uploadBuffer(graph, "SceneRaySpatial.Candidates", candidates, BufferUsage::BufferUsage_Storage);
  const Buffer inputRayBuffer = uploadBuffer(graph, "SceneRaySpatial.InputRays", inputRays, BufferUsage::BufferUsage_Storage);
  const Buffer inputReservoirBuffer = uploadBuffer(graph, "SceneRaySpatial.InputReservoirs", inputReservoirs, BufferUsage::BufferUsage_Storage);
  const Buffer outputRayBuffer = uploadBuffer(graph, "SceneRaySpatial.OutputRays", std::vector<SparseVoxelSceneRayGPU>(width), BufferUsage::BufferUsage_Storage);
  const Buffer outputReservoirBuffer = uploadBuffer(graph, "SceneRaySpatial.OutputReservoirs", std::vector<SparseVoxelDIReservoirGPU>(width), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer lightBuffer = uploadBuffer(graph, "SceneRaySpatial.Lights", std::vector<WorldGILightGPU>{directionalLight()}, BufferUsage::BufferUsage_Storage);
  const Buffer depthBuffer = uploadBuffer(graph, "SceneRaySpatial.Depth", std::vector<float>(width, 10.0f), BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(graph, "SceneRaySpatial.Readback", std::vector<SparseVoxelDIReservoirGPU>(width), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture normalTexture = uploadFloatTexture(
      graph,
      "SceneRaySpatial.Normals",
      Format::Format_RGBA32Float,
      width,
      height,
      {
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
        0.0f,
        1.0f,
      });
  const Texture instanceTexture = uploadUintTexture(graph, "SceneRaySpatial.Instance", width, height, {7u, 7u, 99u, 7u});
  const Texture voxelTexture = uploadUintTexture(graph, "SceneRaySpatial.Voxel", width, height, {11u, 11u, 11u, 11u});
  const uint32_t packedUpNormal =
      packOctahedralNormal({0.0f, 1.0f, 0.0f});
  const uint32_t receiverDepth = floatBits(10.0f);
  const Texture reservoirReceiverTexture =
      reservoirSidebandTexture(
          graph, "SceneRaySpatial.ReservoirReceiver", width, height,
          {
            7u, 11u, packedUpNormal, receiverDepth,
            7u, 11u, packedUpNormal, receiverDepth,
            99u, 11u, packedUpNormal, receiverDepth,
            7u, 11u, packedUpNormal, receiverDepth,
          });
  const Texture reservoirCoreTexture =
      reservoirSidebandTexture(
          graph, "SceneRaySpatial.ReservoirCore", width, height,
          {
            0u, 1u, floatBits(1.0f), reservoirCoreStateWord(),
            0u, 1u, floatBits(1.0f), reservoirCoreStateWord(),
            0u, 1u, floatBits(1.0f), reservoirCoreStateWord(),
            0u, 1u, floatBits(1.0f), reservoirCoreStateWord(),
          });
  const Buffer activeRayIndices = uploadBuffer(
      graph, "SceneRaySpatial.ActiveRayIndices",
      std::vector<uint32_t>{0u, 1u, 2u},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeRayState = uploadBuffer(
      graph, "SceneRaySpatial.ActiveRayState",
      std::vector<uint32_t>{1u, 1u, 1u, width},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);

  uint32_t passIndex = 0u;
  auto *spatialPass = graph.registerPass<svrRendering::SparseVoxelDISpatialReservoirPass>(
      "SceneRaySpatial.Resample",
      passIndex++,
      svrRendering::SparseVoxelDISpatialReservoirPass::Buffers{
        candidateBuffer,
        inputRayBuffer,
        inputReservoirBuffer,
        outputRayBuffer,
        outputReservoirBuffer,
        lightBuffer,
        depthBuffer,
        normalTexture,
        instanceTexture,
        voxelTexture,
        {},
        reservoirReceiverTexture,
        reservoirCoreTexture,
        activeRayIndices,
        activeRayState,
      },
      svrRendering::SparseVoxelDISpatialReservoirPass::Settings{
        width,
        height,
        1u,
        kGeneration,
        1u,
        0.5f,
        0.85f,
        0.05f,
        width,
        height,
      });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRaySpatial.Copy", passIndex++, outputReservoirBuffer, 0u, width * sizeof(SparseVoxelDIReservoirGPU), readback, 0u, width * sizeof(SparseVoxelDIReservoirGPU));
  runGraph(graph);

  std::vector<SparseVoxelDIReservoirGPU> actual(width);
  graph.bufferRead(
      readback,
      0u,
      actual.size() * sizeof(actual[0]),
      [&](const void *data)
      {
        std::memcpy(actual.data(), data, actual.size() * sizeof(actual[0]));
      });
  assert(actual[0].identity[2] == 2u);
  // All three receivers have compatible normal/depth. The different instance
  // at pixel 2 must therefore remain available to spatial ReSTIR.
  assert(actual[1].identity[2] == 3u);
  assert(actual[2].identity[2] == 3u);

  delete copyPass;
  delete spatialPass;
}

void testHitVisibilityMotionReprojection(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 4u;
  constexpr uint32_t height = 1u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> rays(width);
  std::vector<SparseVoxelSceneRayHitGPU> hits(width);
  for (uint32_t pixel = 0u; pixel < width; ++pixel)
  {
    rays[pixel] = activeRay(pixel);
    hits[pixel].status =
        static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit);
    hits[pixel].identity = {
      rays[pixel].seedFlags[0],
      0u,
      static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64),
      kGeneration,
    };
  }
  hits[2].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::UnknownResidency);
  // Pixel one samples a different light than its stored history so reuse must
  // be rejected on the stable light handle.
  rays[1].pixelRayLight[3] = 1u;
  hits[1].identity[1] = 1u;

  std::vector<SparseVoxelTemporalSceneRayHitGPU> previous(width);
  previous[1].hit.status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax);
  previous[1].hit.identity = {
    rays[0].seedFlags[0],
    0u,
    static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64),
    kGeneration,
  };
  previous[1].receiverVisibility = {
    rays[0].originMinimumDistance[0],
    rays[0].originMinimumDistance[1],
    rays[0].originMinimumDistance[2],
    1.0f,
  };
  previous[1].momentsConfidence = {1.0f, 0.75f, 0.0f, 0.0f};
  previous[1].state = {3u, kGeneration, 0u, 0u};
  previous[3].hit.status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax);
  previous[3].hit.identity = {
    rays[3].seedFlags[0], 0u,
    static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64), kGeneration};
  previous[3].receiverVisibility = {
    rays[3].originMinimumDistance[0],
    rays[3].originMinimumDistance[1],
    rays[3].originMinimumDistance[2],
    0.625f,
  };
  previous[3].momentsConfidence = {0.625f, 0.75f, 0.0f, 0.0f};
  previous[3].state = {5u, kGeneration, 0u, 0u};
  // Checkerboard-inactive lanes only guarantee ACTIVE=0. Model a stale
  // payload coordinate from pixel zero; temporal carry must still use this
  // slot's canonical pixel (three), not pixelRayLight.
  rays[3].seedFlags[1] = 0u;
  rays[3].pixelRayLight[0] = 0u;

  const Buffer hitBuffer = uploadBuffer(
      graph, "SceneHitTemporal.Hits", hits, BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = uploadBuffer(
      graph, "SceneHitTemporal.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer previousBuffer = uploadBuffer(
      graph, "SceneHitTemporal.Previous", previous,
      BufferUsage::BufferUsage_Storage);
  const Buffer historyBuffer = uploadBuffer(
      graph, "SceneHitTemporal.History",
      std::vector<SparseVoxelTemporalSceneRayHitGPU>(width),
      BufferUsage::BufferUsage_Storage);
  const Buffer stableHitBuffer = uploadBuffer(
      graph, "SceneHitTemporal.StableHits",
      std::vector<SparseVoxelSceneRayHitGPU>(width),
      BufferUsage::BufferUsage_Storage);
  const Buffer visibilityBuffer = uploadBuffer(
      graph, "SceneHitTemporal.Visibility", std::vector<float>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "SceneHitTemporal.Readback", std::vector<float>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture motionTexture = uploadFloatTexture(
      graph,
      "SceneHitTemporal.Motion",
      Format::Format_RG32Float,
      width,
      height,
      {
        -1.0f / 4.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f,
      });

  uint32_t passIndex = 0u;
  auto *temporalPass =
      graph.registerPass<svrRendering::SparseVoxelSceneHitTemporalPass>(
          "SceneHitTemporal.Accumulate",
          passIndex++,
          svrRendering::SparseVoxelSceneHitTemporalPass::Buffers{
            hitBuffer,
            rayBuffer,
            previousBuffer,
            historyBuffer,
            stableHitBuffer,
            visibilityBuffer,
            motionTexture,
          },
          svrRendering::SparseVoxelSceneHitTemporalPass::Settings{
            width, 8u, kGeneration, true, 0.25f, 0.05f, width, height});
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneHitTemporal.Copy",
      passIndex++,
      visibilityBuffer,
      0u,
      width * sizeof(float),
      readback,
      0u,
      width * sizeof(float));
  runGraph(graph);

  std::vector<float> actual(width);
  graph.bufferRead(
      readback,
      0u,
      actual.size() * sizeof(actual[0]),
      [&](const void *data)
      {
        std::memcpy(actual.data(), data, actual.size() * sizeof(actual[0]));
      });
  // Pixel zero reprojects to previous pixel one and retains three visible
  // samples before blending the current occluded result.
  assert(std::abs(actual[0] - 0.75f) < 1.0e-6f);
  // Pixel one must not reuse its history because the stable light handle
  // differs. Ray seeds are deliberately not compared: fresh reservoir
  // candidates re-sample the same light with new seeds every frame, and
  // motion-reprojected history always comes from another ray index, so a
  // seed comparison would discard all accumulation while the camera moves.
  assert(std::abs(actual[1]) < 1.0e-6f);
  // Incomplete traversal is fail-closed for shadow visibility.
  assert(std::abs(actual[2]) < 1.0e-6f);
  // Inactive pixel three carries its own settled history even though the
  // inactive ray payload still names pixel zero.
  assert(std::abs(actual[3] - 0.625f) < 1.0e-6f);

  delete copyPass;
  delete temporalPass;
}

void testHitVisibilityKeepsViewportEdgeGuardBand(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 4u;
  constexpr uint32_t height = 1u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> rays(width);
  std::vector<SparseVoxelSceneRayHitGPU> hits(width);
  for (uint32_t pixel = 0u; pixel < width; ++pixel)
  {
    rays[pixel] = activeRay(pixel);
    hits[pixel].status =
        static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit);
    hits[pixel].identity = {
      rays[pixel].seedFlags[0],
      0u,
      static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64),
      kGeneration,
    };
  }

  std::vector<SparseVoxelTemporalSceneRayHitGPU> previous(width);
  previous[0].hit.status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax);
  previous[0].hit.identity = {
    rays[0].seedFlags[0],
    0u,
    static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64),
    kGeneration,
  };
  previous[0].receiverVisibility = {
    rays[0].originMinimumDistance[0],
    rays[0].originMinimumDistance[1],
    rays[0].originMinimumDistance[2],
    1.0f,
  };
  previous[0].momentsConfidence = {1.0f, 0.75f, 0.0f, 0.0f};
  previous[0].state = {3u, kGeneration, 0u, 0u};

  const Buffer hitBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.Hits", hits, BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer previousBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.Previous", previous,
      BufferUsage::BufferUsage_Storage);
  const Buffer historyBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.History",
      std::vector<SparseVoxelTemporalSceneRayHitGPU>(width),
      BufferUsage::BufferUsage_Storage);
  const Buffer stableHitBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.StableHits",
      std::vector<SparseVoxelSceneRayHitGPU>(width),
      BufferUsage::BufferUsage_Storage);
  const Buffer visibilityBuffer = uploadBuffer(
      graph, "SceneHitTemporalEdge.Visibility", std::vector<float>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "SceneHitTemporalEdge.Readback", std::vector<float>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture motionTexture = uploadFloatTexture(
      graph,
      "SceneHitTemporalEdge.Motion",
      Format::Format_RG32Float,
      width,
      height,
      {
        1.0f / 4.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f,
        0.0f, 0.0f,
      });

  uint32_t passIndex = 0u;
  auto *temporalPass =
      graph.registerPass<svrRendering::SparseVoxelSceneHitTemporalPass>(
          "SceneHitTemporalEdge.Accumulate",
          passIndex++,
          svrRendering::SparseVoxelSceneHitTemporalPass::Buffers{
            hitBuffer,
            rayBuffer,
            previousBuffer,
            historyBuffer,
            stableHitBuffer,
            visibilityBuffer,
            motionTexture,
          },
          svrRendering::SparseVoxelSceneHitTemporalPass::Settings{
            width, 8u, kGeneration, true, 0.25f, 0.05f, width, height});
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneHitTemporalEdge.Copy",
      passIndex++,
      visibilityBuffer,
      0u,
      width * sizeof(float),
      readback,
      0u,
      width * sizeof(float));
  runGraph(graph);

  std::vector<float> actual(width);
  graph.bufferRead(
      readback,
      0u,
      actual.size() * sizeof(actual[0]),
      [&](const void *data)
      {
        std::memcpy(actual.data(), data, actual.size() * sizeof(actual[0]));
      });
  // The motion vector puts pixel zero just outside the previous viewport. A
  // two-pixel guard band should clamp to the edge and then rely on receiver
  // validation, preserving the settled visible history before blending the
  // current occluded shadow sample.
  assert(std::abs(actual[0] - 0.75f) < 1.0e-6f);

  delete copyPass;
  delete temporalPass;
}

void testSkyRayGeneration(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  RenderGraph graph(&rhi);
  // Exercise sky selection through the normal GI/ReSTIR path together with
  // the canonical-slot contract and quarter-rate scheduling: storage remains
  // full raster size, every slot stays attached to its pixel, and frame three
  // refreshes only offset (1, 1) in each 2x2 quad.
  const Buffer coverage = uploadBuffer(
      graph,
      "SceneRaySky.Coverage",
      std::vector<uint32_t>{1u, 1u, 1u, 1u},
      BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph,
      "SceneRaySky.Depth",
      std::vector<float>{1.0f, 1.0f, 1.0f, 1.0f},
      BufferUsage::BufferUsage_Storage);
  const Buffer rays = uploadBuffer(graph, "SceneRaySky.Rays", std::vector<SparseVoxelSceneRayGPU>(4u), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  WorldGILightGPU sky{};
  sky.positionRange[0] = 0.5f;
  sky.positionRange[1] = 0.7f;
  sky.positionRange[2] = 1.0f;
  sky.directionType[1] = 1.0f;
  sky.shapeParams[0] = 0.05f;
  sky.shapeParams[1] = 0.04f;
  sky.shapeParams[2] = 0.03f;
  sky.shapeParams[3] = 0.35f;
  sky.colorIntensity[0] = 0.08f;
  sky.colorIntensity[1] = 0.24f;
  sky.colorIntensity[2] = 0.55f;
  sky.colorIntensity[3] = 1.0f;
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[2] = 0u;
  sky.flags[3] = kGeneration;
  const Buffer lights = uploadBuffer(graph, "SceneRaySky.Lights", std::vector<WorldGILightGPU>{sky}, BufferUsage::BufferUsage_Storage);
  const Buffer bvhNodes = uploadBuffer(graph, "SceneRaySky.BVHNodes", std::vector<WorldGILightBVHNodeGPU>(1u), BufferUsage::BufferUsage_Storage);
  WorldGILightBVHInfoGPU info{};
  info.lightCount = 1u;
  info.infiniteLightCount = 1u;
  const Buffer bvhInfo = uploadBuffer(graph, "SceneRaySky.BVHInfo", std::vector<WorldGILightBVHInfoGPU>{info}, BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(graph, "SceneRaySky.Readback", std::vector<SparseVoxelSceneRayGPU>(4u), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer adaptiveRays = uploadBuffer(
      graph, "SceneRaySky.AdaptiveRays",
      std::vector<SparseVoxelSceneRayGPU>(4u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer adaptiveReadback = uploadBuffer(
      graph, "SceneRaySky.AdaptiveReadback",
      std::vector<SparseVoxelSceneRayGPU>(4u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture normalTexture = uploadFloatTexture(
      graph,
      "SceneRaySky.Normal",
      Format::Format_RGBA32Float,
      2u,
      2u,
      {
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
      });

  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  raster.focalPrincipal = {1.0f, 1.0f, 0.5f, 0.5f};
  raster.viewportTiles = {2u, 2u, 1u, 1u};

  uint32_t passIndex = 0u;
  const Texture lightFieldTexture = graph.createTexture({
    .name = "SceneRaySky.LightField",
    .format = Format::Format_RGBA16Float,
    .height = 2u,
    .width = 2u,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });
  const Texture instanceTexture = uploadUintTexture(
      graph, "SceneRaySky.Instance", 2u, 2u,
      {1u, 1u, 1u, 1u});
  const Texture voxelTexture = uploadUintTexture(
      graph, "SceneRaySky.Voxel", 2u, 2u,
      {2u, 2u, 2u, 2u});
  const Texture motionTexture = uploadFloatTexture(
      graph, "SceneRaySky.Motion", Format::Format_RG32Float, 2u, 2u,
      {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
  const Texture lightFieldIdTexture = uploadUintTexture(
      graph, "SceneRaySky.LightFieldId", 2u, 2u,
      {1u, 2u, 1u, 2u, 1u, 2u, 1u, 2u},
      Format::Format_RG32Uint);
  auto *createPass = graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
      "SceneRaySky.Create",
      passIndex++,
      svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
        coverage,
        depth,
        rays,
        lights,
        bvhNodes,
        bvhInfo,
        normalTexture,
        // Demand budgeting is off here (refresh interval 1), so the field is
        // never read; it only has to be bindable.
        lightFieldTexture,
      },
      // skyMode = 0: with sky as the only enabled light, the regular light
      // selector must still produce the sky candidate in the single-lane
      // compatibility path. A 2x2 viewport reaches quarter-rate sampling with
      // a divisor of 2; sample index 1 selects offset (1, 1) in the hashed
      // schedule.
      svrRendering::SparseVoxelSceneRayCreatePass::Settings{
        raster,
        {-0.408248f, 0.816497f, 0.408248f},
        64.0f,
        0.01f,
        2u,
        2u,
        1u,
        1u,
        kGeneration,
        1u,
        2u,
        2u,
        0u,
        0.0f,
        32.0f,
        1.0f,
        1u,
        2u,
      });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>("SceneRaySky.Copy", passIndex++, rays, 0u, 4u * sizeof(SparseVoxelSceneRayGPU), readback, 0u, 4u * sizeof(SparseVoxelSceneRayGPU));
  auto *adaptiveCreatePass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
          "SceneRaySky.AdaptiveCreate", passIndex++,
          svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
            coverage, depth, adaptiveRays, lights, bvhNodes, bvhInfo,
            normalTexture, lightFieldTexture, {}, instanceTexture,
            voxelTexture, motionTexture, lightFieldIdTexture},
          svrRendering::SparseVoxelSceneRayCreatePass::Settings{
            raster,
            {-0.408248f, 0.816497f, 0.408248f},
            64.0f,
            0.01f,
            2u,
            2u,
            1u,
            1u,
            kGeneration,
            1u,
            2u,
            2u,
            0u,
            0.0f,
            256.0f,
            1.0f,
            1u,
            2u,
          });
  // No valid previous frame: every covered receiver has maximum demand and
  // must bypass the quarter-rate phase for an immediate first sample.
  adaptiveCreatePass->updateAdaptiveDemand(true);
  adaptiveCreatePass->update(raster, kGeneration, 1u, false);
  auto *adaptiveCopyPass =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "SceneRaySky.AdaptiveCopy", passIndex++, adaptiveRays, 0u,
          4u * sizeof(SparseVoxelSceneRayGPU), adaptiveReadback, 0u,
          4u * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::array<SparseVoxelSceneRayGPU, 4u> actual{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(actual),
      [&](const void *data)
      {
        std::memcpy(&actual, data, sizeof(actual));
      });
  for (uint32_t pixel = 0u; pixel < actual.size(); ++pixel)
  {
    assert(((actual[pixel].seedFlags[1] & kSparseVoxelSceneRayFlagActive) != 0u) ==
           (pixel == 3u));
  }
  assert(actual[3].pixelRayLight[0] == 1u);
  assert(actual[3].pixelRayLight[1] == 1u);
  const auto &direction = actual[3].directionMaximumDistance;
  const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] + direction[2] * direction[2]);
  assert(actual[3].pixelRayLight[3] == 0u);
  assert(std::abs(length - 1.0f) < 1.0e-5f);
  assert(direction[1] >= 0.0f);
  assert(std::abs(actual[3].lightSamplePdfWeight[1] - direction[1] / 3.14159265359f) < 1.0e-5f);
  assert(actual[3].lightSamplePdfWeight[2] > 0.0f);
  assert(std::abs(direction[3] - 64.0f) < 1.0e-5f);

  std::array<SparseVoxelSceneRayGPU, 4u> adaptive{};
  graph.bufferRead(
      adaptiveReadback, 0u, sizeof(adaptive),
      [&](const void *data)
      {
        std::memcpy(&adaptive, data, sizeof(adaptive));
      });
  uint32_t adaptiveActive = 0u;
  for (const SparseVoxelSceneRayGPU &ray : adaptive)
    adaptiveActive +=
        (ray.seedFlags[1] & kSparseVoxelSceneRayFlagActive) != 0u ? 1u : 0u;
  // Demand promotion is budgeted: the scheduled quarter-rate phase is
  // guaranteed, while newly visible off-phase pixels are stochastic extras.
  assert(
      (adaptive[3].seedFlags[1] &
       kSparseVoxelSceneRayFlagActive) != 0u);
  assert(adaptiveActive >= 1u && adaptiveActive <= adaptive.size());

  delete adaptiveCopyPass;
  delete adaptiveCreatePass;
  delete copyPass;
  delete createPass;
}

void testViewportEdgeUsesFullRateSchedule(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 8u;
  constexpr uint32_t height = 8u;
  constexpr uint32_t pixelCount = width * height;
  RenderGraph graph(&rhi);
  const Buffer coverage = uploadBuffer(
      graph, "EdgeSchedule.Coverage", std::vector<uint32_t>(pixelCount, 1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph, "EdgeSchedule.Depth", std::vector<float>(pixelCount, 1.0f),
      BufferUsage::BufferUsage_Storage);
  const Buffer rays = uploadBuffer(
      graph, "EdgeSchedule.Rays",
      std::vector<SparseVoxelSceneRayGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  WorldGILightGPU sky{};
  sky.directionType[1] = 1.0f;
  sky.colorIntensity[0] = 1.0f;
  sky.colorIntensity[1] = 1.0f;
  sky.colorIntensity[2] = 1.0f;
  sky.colorIntensity[3] = 1.0f;
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[2] = 0u;
  sky.flags[3] = kGeneration;
  const Buffer lights = uploadBuffer(
      graph, "EdgeSchedule.Lights", std::vector<WorldGILightGPU>{sky},
      BufferUsage::BufferUsage_Storage);
  const Buffer bvhNodes = uploadBuffer(
      graph, "EdgeSchedule.BVHNodes",
      std::vector<WorldGILightBVHNodeGPU>(1u),
      BufferUsage::BufferUsage_Storage);
  WorldGILightBVHInfoGPU info{};
  info.lightCount = 1u;
  info.infiniteLightCount = 1u;
  const Buffer bvhInfo = uploadBuffer(
      graph, "EdgeSchedule.BVHInfo",
      std::vector<WorldGILightBVHInfoGPU>{info},
      BufferUsage::BufferUsage_Storage);
  std::vector<float> normals(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    normals[pixel * 4u + 1u] = 1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture normalTexture = uploadFloatTexture(
      graph, "EdgeSchedule.Normal", Format::Format_RGBA32Float,
      width, height, normals);
  std::vector<float> motion(pixelCount * 2u, 0.0f);
  constexpr uint32_t motionDisocclusionX = 3u;
  constexpr uint32_t motionDisocclusionY = 3u;
  motion[(motionDisocclusionY * width + motionDisocclusionX) * 2u] = 0.5f;
  const Texture motionTexture = uploadFloatTexture(
      graph, "EdgeSchedule.Motion", Format::Format_RG32Float,
      width, height, motion);
  const Texture lightFieldTexture = graph.createTexture({
    .name = "EdgeSchedule.LightField",
    .format = Format::Format_RGBA16Float,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });
  const Buffer readback = uploadBuffer(
      graph, "EdgeSchedule.Readback",
      std::vector<SparseVoxelSceneRayGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  raster.focalPrincipal = {64.0f, 64.0f, 4.0f, 4.0f};
  raster.viewportTiles = {width, height, 1u, 1u};
  uint32_t passIndex = 0u;
  auto *createPass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
          "EdgeSchedule.Create", passIndex++,
          svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
            .coverage = coverage,
            .linearDepth = depth,
            .rays = rays,
            .lights = lights,
            .lightBVHNodes = bvhNodes,
            .lightBVHInfo = bvhInfo,
            .geometryNormal = normalTexture,
            .lightFieldHistory = lightFieldTexture,
            .motionVector = motionTexture,
          },
          svrRendering::SparseVoxelSceneRayCreatePass::Settings{
            .raster = raster,
            .maximumDistance = 64.0f,
            .width = width,
            .height = height,
            .candidatesPerPixel = 1u,
            .lightCapacity = 1u,
            .generation = kGeneration,
            .sampleIndex = 0u,
            .sourceWidth = width,
            .sourceHeight = height,
            .skyMode = 1u,
            .samplingDivisor = 4u,
            .fullRateEdgeWidth = 1u,
          });
  createPass->update(raster, kGeneration, 0u, true);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "EdgeSchedule.Copy", passIndex++, rays, 0u,
      uint64_t{pixelCount} * sizeof(SparseVoxelSceneRayGPU), readback, 0u,
      uint64_t{pixelCount} * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::array<SparseVoxelSceneRayGPU, pixelCount> result{};
  graph.bufferRead(
      readback, 0u, sizeof(result),
      [&](const void *data) { std::memcpy(result.data(), data, sizeof(result)); });
  uint32_t activeCount = 0u;
  uint32_t expectedActiveCount = 0u;
  constexpr std::array<uint32_t, 64> blueNoiseCoordinates{
      0u, 36u, 32u, 4u, 18u, 54u, 50u, 22u,
      16u, 52u, 48u, 20u, 2u, 38u, 34u, 6u,
      9u, 45u, 41u, 13u, 27u, 63u, 59u, 31u,
      25u, 61u, 57u, 29u, 11u, 47u, 43u, 15u,
      8u, 44u, 40u, 12u, 26u, 62u, 58u, 30u,
      24u, 60u, 56u, 28u, 10u, 46u, 42u, 14u,
      1u, 37u, 33u, 5u, 19u, 55u, 51u, 23u,
      17u, 53u, 49u, 21u, 3u, 39u, 35u, 7u};
  const auto hashBits = [](const uint32_t value)
  {
    uint32_t bits = value * 747796405u + 2891336453u;
    bits = (bits >> ((bits >> 28u) + 4u)) ^ bits;
    return bits * 277803737u;
  };
  for (uint32_t y = 0u; y < height; ++y)
    for (uint32_t x = 0u; x < width; ++x)
    {
      const bool edge = x == 0u || y == 0u || x + 1u == width ||
          y + 1u == height;
      constexpr uint32_t window = 4u;
      const uint32_t blockX = x / window;
      const uint32_t blockY = y / window;
      const uint32_t blockSeed = hashBits(
          blockX * 0x8da6b343u ^ blockY * 0xd8163841u);
      const uint32_t phase = blockSeed & (window * window - 1u);
      const uint32_t packedCoordinate = blueNoiseCoordinates[phase];
      const uint32_t selectedX = (packedCoordinate & 7u) / 2u;
      const uint32_t selectedY = (packedCoordinate >> 3u) / 2u;
      const bool sparseWindowPhase =
          x % window == selectedX && y % window == selectedY;
      const bool motionDisocclusion =
          x == motionDisocclusionX && y == motionDisocclusionY;
      const bool active =
          (result[y * width + x].seedFlags[1] &
           kSparseVoxelSceneRayFlagActive) != 0u;
      const bool expected = edge || sparseWindowPhase || motionDisocclusion;
      assert(active == expected);
      activeCount += active ? 1u : 0u;
      expectedActiveCount += expected ? 1u : 0u;
    }
  assert(activeCount == expectedActiveCount);
}

void testLightFieldKeepsResolvedVisibilityReceiverLocal(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 2u;
  constexpr uint32_t height = 2u;
  constexpr uint32_t pixelCount = width * height;
  RenderGraph graph(&rhi);

  const Buffer coverage = uploadBuffer(
      graph, "LightField.Coverage",
      std::vector<uint32_t>(pixelCount, 1u), BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph, "LightField.Depth",
      std::vector<float>(pixelCount, 1.0f), BufferUsage::BufferUsage_Storage);

  std::vector<SparseVoxelSceneRayGPU> rays(pixelCount);
  rays[0] = activeRay(0u);
  rays[0].directionMaximumDistance = {0.0f, 1.0f, 0.0f, 64.0f};
  rays[0].pixelRayLight = {0u, 0u, 0u, 0u};
  const Buffer rayBuffer = uploadBuffer(
      graph, "LightField.Rays", rays, BufferUsage::BufferUsage_Storage);

  std::vector<SparseVoxelDIReservoirGPU> reservoirs(pixelCount);
  for (auto &reservoir : reservoirs)
  {
    reservoir.identity = {0u, 0u, 1u, kGeneration};
    reservoir.weights = {1.0f, 1.0f, 1.0f, 3.14159265359f};
    reservoir.state = {1u, 0u, 1u, 0u};
  }
  const Buffer reservoirBuffer = uploadBuffer(
      graph, "LightField.Reservoirs", reservoirs,
      BufferUsage::BufferUsage_Storage);

  WorldGILightGPU sky{};
  sky.positionRange[0] = 1.0f;
  sky.positionRange[1] = 1.0f;
  sky.positionRange[2] = 1.0f;
  sky.directionType[1] = 1.0f;
  sky.shapeParams[0] = 1.0f;
  sky.shapeParams[1] = 1.0f;
  sky.shapeParams[2] = 1.0f;
  sky.shapeParams[3] = 1.0f;
  sky.colorIntensity[0] = 1.0f;
  sky.colorIntensity[1] = 1.0f;
  sky.colorIntensity[2] = 1.0f;
  sky.colorIntensity[3] = 1.0f;
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[3] = kGeneration;
  const Buffer lights = uploadBuffer(
      graph, "LightField.Lights",
      std::vector<WorldGILightGPU>{sky, directionalLight()},
      BufferUsage::BufferUsage_Storage);

  std::vector<SparseVoxelSceneRayHitGPU> resolvedHits(pixelCount);
  resolvedHits[0].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax);
  const Buffer resolvedHitBuffer = uploadBuffer(
      graph, "LightField.ResolvedHits", resolvedHits,
      BufferUsage::BufferUsage_Storage);
  std::vector<SparseVoxelSceneRayHitGPU> unresolvedHits(pixelCount);
  unresolvedHits[0].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::QueryLimit);
  const Buffer unresolvedHitBuffer = uploadBuffer(
      graph, "LightField.UnresolvedHits", unresolvedHits,
      BufferUsage::BufferUsage_Storage);
  constexpr uint32_t independentRaysPerPixel = 2u;
  std::vector<SparseVoxelSceneRayGPU> independentRays(
      pixelCount * independentRaysPerPixel);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const uint32_t base = pixel * independentRaysPerPixel;
    independentRays[base] = activeRay(pixel);
    independentRays[base + 1u] = activeRay(pixel);
    independentRays[base].pixelRayLight =
        {pixel % width, pixel / width, 0u, 0u};
    independentRays[base + 1u].pixelRayLight =
        {pixel % width, pixel / width, 1u, 1u};
    independentRays[base].lightSamplePdfWeight[1] =
        1.0f / 3.14159265359f;
    independentRays[base + 1u].lightSamplePdfWeight[1] =
        1.0f / 3.14159265359f;
  }
  const Buffer independentRayBuffer = uploadBuffer(
      graph, "LightField.IndependentRays", independentRays,
      BufferUsage::BufferUsage_Storage);
  std::vector<SparseVoxelSceneRayHitGPU> independentHits(
      independentRays.size());
  for (auto &hit : independentHits)
    hit.status = static_cast<uint32_t>(
        SparseVoxelSceneRayStatus::MissTMax);
  const Buffer independentHitBuffer = uploadBuffer(
      graph, "LightField.IndependentHits", independentHits,
      BufferUsage::BufferUsage_Storage);

  std::vector<float> normals(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    normals[pixel * 4u + 1u] = 1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture normal = uploadFloatTexture(
      graph, "LightField.Normal", Format::Format_RGBA32Float,
      width, height, normals);
  const Texture identity = uploadUintTexture(
      graph, "LightField.Identity", width, height,
      std::vector<uint32_t>(pixelCount, 1u));
  const Texture motion = uploadFloatTexture(
      graph, "LightField.Motion", Format::Format_RG32Float,
      width, height, std::vector<float>(pixelCount * 2u, 0.0f));
  const Texture history = uploadFloatTexture(
      graph, "LightField.History", Format::Format_RGBA32Float,
      width, height, std::vector<float>(pixelCount * 4u, 0.0f));
  std::vector<float> brightHistory(pixelCount * 4u, 1.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
    brightHistory[pixel * 4u + 3u] = 256.0f;
  const Texture antiLagHistory = uploadFloatTexture(
      graph, "LightField.AntiLagHistory", Format::Format_RGBA32Float,
      width, height, brightHistory);
  const Texture historyId = graph.createTexture({
    .name = "LightField.HistoryId",
    .format = Format::Format_RG32Uint,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Sampled,
  });
  std::vector<uint32_t> emptyHistoryIds(pixelCount * 2u, 0u);
  graph.textureWrite(
      historyId, emptyHistoryIds.size() * sizeof(uint32_t),
      emptyHistoryIds.data());
  const Texture antiLagHistoryId = graph.createTexture({
    .name = "LightField.AntiLagHistoryId",
    .format = Format::Format_RG32Uint,
    .height = height,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Sampled,
  });
  std::vector<uint32_t> antiLagHistoryIds(pixelCount * 2u, 1u);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
    antiLagHistoryIds[pixel * 2u] = (1u << 29u) | 1u;
  graph.textureWrite(
      antiLagHistoryId,
      antiLagHistoryIds.size() * sizeof(uint32_t),
      antiLagHistoryIds.data());
  const Texture antiLagMotion = uploadFloatTexture(
      graph, "LightField.AntiLagMotion", Format::Format_RG32Float,
      width, height,
      std::vector<float>{
        0.0f, 0.2f, 0.0f, 0.2f,
        0.0f, 0.2f, 0.0f, 0.2f,
      });
  std::vector<float> awakeHistoryValues(pixelCount * 4u, 0.75f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
    awakeHistoryValues[pixel * 4u + 3u] = 32.0f;
  const Texture awakeHistory = uploadFloatTexture(
      graph, "LightField.AwakeHistory", Format::Format_RGBA32Float,
      width, height, awakeHistoryValues);
  const uint32_t skySignature = reachabilityMix(
      sky.flags[2] ^ (sky.flags[1] * 0x85ebca6bu) ^
      (sky.flags[3] * 0xc2b2ae35u));
  uint32_t awakeLightHash = reachabilityMix(0x9e3779b9u ^ skySignature);
  awakeLightHash = reachabilityMix(awakeLightHash ^ 1u);
  if (awakeLightHash == 0u)
    awakeLightHash = 1u;
  const Texture awakeHistoryLightHash = uploadUintTexture(
      graph, "LightField.AwakeHistoryLightHash", width, height,
      std::vector<uint32_t>(pixelCount, awakeLightHash));

  const auto outputTexture = [&](const std::string &name, const Format format)
  {
    return graph.createTexture({
      .name = name,
      .format = format,
      .height = height,
      .width = width,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture reconstructed =
      outputTexture("LightField.Reconstructed", Format::Format_RGBA16Float);
  const Texture unresolved =
      outputTexture("LightField.Unresolved", Format::Format_RGBA16Float);
  const Texture reconstructedId =
      outputTexture("LightField.ReconstructedId", Format::Format_RG32Uint);
  const Texture unresolvedId =
      outputTexture("LightField.UnresolvedId", Format::Format_RG32Uint);
  const Texture reconstructedReactive =
      outputTexture("LightField.ReconstructedReactive", Format::Format_R16Float);
  const Texture unresolvedReactive =
      outputTexture("LightField.UnresolvedReactive", Format::Format_R16Float);
  const Texture independent =
      outputTexture("LightField.Independent", Format::Format_RGBA16Float);
  const Texture independentId =
      outputTexture("LightField.IndependentId", Format::Format_RG32Uint);
  const Texture independentReactive =
      outputTexture("LightField.IndependentReactive", Format::Format_R16Float);
  const Texture antiLag =
      outputTexture("LightField.AntiLag", Format::Format_RGBA16Float);
  const Texture antiLagId =
      outputTexture("LightField.AntiLagId", Format::Format_RG32Uint);
  const Texture antiLagReactive =
      outputTexture("LightField.AntiLagReactive", Format::Format_R16Float);
  const Texture cutSeed =
      outputTexture("LightField.CutSeed", Format::Format_RGBA16Float);
  const Texture cutSeedId =
      outputTexture("LightField.CutSeedId", Format::Format_RG32Uint);
  const Texture cutSeedReactive =
      outputTexture("LightField.CutSeedReactive", Format::Format_R16Float);
  const Texture cutSeedDepth =
      outputTexture("LightField.CutSeedDepth", Format::Format_R32Float);
  const Texture cutSeedNormal =
      outputTexture("LightField.CutSeedNormal", Format::Format_RG16Float);
  const Texture cutSeedLightHash =
      outputTexture("LightField.CutSeedLightHash", Format::Format_R32Uint);
  const Texture cutRetained =
      outputTexture("LightField.CutRetained", Format::Format_RGBA16Float);
  const Texture cutRetainedId =
      outputTexture("LightField.CutRetainedId", Format::Format_RG32Uint);
  const Texture cutRetainedReactive =
      outputTexture("LightField.CutRetainedReactive", Format::Format_R16Float);
  const Texture cutRetainedDepth =
      outputTexture("LightField.CutRetainedDepth", Format::Format_R32Float);
  const Texture cutRetainedNormal =
      outputTexture("LightField.CutRetainedNormal", Format::Format_RG16Float);
  const Texture cutRejected =
      outputTexture("LightField.CutRejected", Format::Format_RGBA16Float);
  const Texture cutRejectedId =
      outputTexture("LightField.CutRejectedId", Format::Format_RG32Uint);
  const Texture cutRejectedReactive =
      outputTexture("LightField.CutRejectedReactive", Format::Format_R16Float);
  const Texture cutRejectedDepth =
      outputTexture("LightField.CutRejectedDepth", Format::Format_R32Float);
  const Texture cutRejectedNormal =
      outputTexture("LightField.CutRejectedNormal", Format::Format_RG16Float);
  const Texture reducedFeedback =
      outputTexture("LightField.ReducedFeedback", Format::Format_RGBA16Float);
  const Texture reducedFeedbackId =
      outputTexture("LightField.ReducedFeedbackId", Format::Format_RG32Uint);
  const Texture reducedFeedbackReactive =
      outputTexture("LightField.ReducedFeedbackReactive", Format::Format_R16Float);
  const Texture awake =
      outputTexture("LightField.Awake", Format::Format_RGBA16Float);
  const Texture awakeId =
      outputTexture("LightField.AwakeId", Format::Format_RG32Uint);
  const Texture awakeReactive =
      outputTexture("LightField.AwakeReactive", Format::Format_R16Float);
  const Texture awakeLightHashOutput =
      outputTexture("LightField.AwakeLightHash", Format::Format_R32Uint);
  const Texture awakeDepth =
      outputTexture("LightField.AwakeDepth", Format::Format_R32Float);
  const Texture awakeNormal =
      outputTexture("LightField.AwakeNormal", Format::Format_RG16Float);
  const Buffer reachability = uploadBuffer(
      graph, "LightField.Reachability", std::vector<uint32_t>(64u, 0u),
      BufferUsage::BufferUsage_Storage);

  const auto makeLightFieldPass =
      [&](const std::string &name, Buffer hits, Texture output, Texture outputId,
          Texture reactive, uint32_t index)
  {
    return graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
        name, index,
        svrRendering::SparseVoxelLightFieldPass::Buffers{
          coverage, depth, rayBuffer, reservoirBuffer, lights, hits,
          reachability},
        svrRendering::SparseVoxelLightFieldPass::Textures{
          normal, normal, identity, identity, motion, history, historyId,
          output, outputId, reactive},
        svrRendering::SparseVoxelLightFieldPass::Settings{
          width, height, width, height, false, 32u, 0u, 1u});
  };
  uint32_t passIndex = 0u;
  auto *resolvedPass = makeLightFieldPass(
      "LightField.ResolveSparse", resolvedHitBuffer, reconstructed,
      reconstructedId, reconstructedReactive, passIndex++);
  auto *unresolvedPass = makeLightFieldPass(
      "LightField.RejectUnresolved", unresolvedHitBuffer, unresolved,
      unresolvedId, unresolvedReactive, passIndex++);
  auto *independentPass =
      graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
          "LightField.AverageIndependent", passIndex++,
          svrRendering::SparseVoxelLightFieldPass::Buffers{
            coverage, depth, independentRayBuffer, reservoirBuffer, lights,
            independentHitBuffer, reachability},
          svrRendering::SparseVoxelLightFieldPass::Textures{
            normal, normal, identity, identity, motion, history, historyId,
            independent, independentId, independentReactive},
          svrRendering::SparseVoxelLightFieldPass::Settings{
            width, height, width, height, false, 32u, 0u, 1u,
            0.85f, 0.05f, 2.0f, 0u, 0u, 1.0f,
            independentRaysPerPixel, true});
  std::vector<SparseVoxelSceneRayHitGPU> darkHits(pixelCount);
  darkHits[0].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit);
  const Buffer darkHitBuffer = uploadBuffer(
      graph, "LightField.AntiLagHits", darkHits,
      BufferUsage::BufferUsage_Storage);
  auto *antiLagPass =
      graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
          "LightField.RejectMovingBrightHistory", passIndex++,
          svrRendering::SparseVoxelLightFieldPass::Buffers{
            coverage, depth, rayBuffer, reservoirBuffer, lights,
            darkHitBuffer, reachability},
          svrRendering::SparseVoxelLightFieldPass::Textures{
            normal, normal, identity, identity, antiLagMotion,
            antiLagHistory, antiLagHistoryId, antiLag, antiLagId,
            antiLagReactive},
          svrRendering::SparseVoxelLightFieldPass::Settings{
            width, height, width, height, true, 256u, 0u, 1u,
            0.85f, 0.05f, 2.0f, 0u, 0u, 1.0f, 1u, false,
            0.05f, 0.25f, 8.0f, 0.9f});

  const Buffer terrainOwnership = uploadBuffer(
      graph, "LightField.TerrainOwnership",
      std::vector<uint32_t>(pixelCount, 1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer inactiveRayBuffer = uploadBuffer(
      graph, "LightField.InactiveRays",
      std::vector<SparseVoxelSceneRayGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage);
  const Texture replacedTerrainIdentity = uploadUintTexture(
      graph, "LightField.ReplacedTerrainIdentity", width, height,
      std::vector<uint32_t>(pixelCount, 2u));
  const Texture cutRetainedLightHash =
      outputTexture("LightField.CutRetainedLightHash", Format::Format_R32Uint);
  const Texture cutRejectedLightHash =
      outputTexture("LightField.CutRejectedLightHash", Format::Format_R32Uint);
  svrRendering::SparseVoxelLightFieldPass::Settings cutHistorySettings{};
  cutHistorySettings.width = width;
  cutHistorySettings.height = height;
  cutHistorySettings.rayWidth = width;
  cutHistorySettings.rayHeight = height;
  cutHistorySettings.historyValid = false;
  cutHistorySettings.maximumSamples = 32u;
  cutHistorySettings.lightCapacity = 1u;
  cutHistorySettings.raster.cameraToWorld[0] = 1.0f;
  cutHistorySettings.raster.cameraToWorld[5] = 1.0f;
  cutHistorySettings.raster.cameraToWorld[10] = 1.0f;
  cutHistorySettings.raster.cameraToWorld[15] = 1.0f;
  cutHistorySettings.raster.focalPrincipal = {1.0f, 1.0f, 0.0f, 0.0f};
  cutHistorySettings.raster.worldToClip =
      cutHistorySettings.raster.cameraToWorld;
  cutHistorySettings.raster.previousWorldToClip =
      cutHistorySettings.raster.cameraToWorld;
  auto *cutSeedPass =
      graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
          "LightField.SeedTerrainCutHistory", passIndex++,
          svrRendering::SparseVoxelLightFieldPass::Buffers{
            coverage, depth, rayBuffer, reservoirBuffer, lights,
            resolvedHitBuffer, reachability, {}, {}, {}, {},
            terrainOwnership},
          svrRendering::SparseVoxelLightFieldPass::Textures{
            normal, normal, identity, identity, motion, history, historyId,
            cutSeed, cutSeedId, cutSeedReactive, {}, {}, {}, cutSeedLightHash,
            {}, cutSeedDepth, {}, cutSeedNormal},
          cutHistorySettings);
  cutHistorySettings.historyValid = true;
  const auto makeCutHistoryPass =
      [&](const std::string &name,
          const svrRendering::SparseVoxelLightFieldPass::Settings &settings,
          const Texture output, const Texture outputId,
          const Texture reactive, const Texture outputDepth,
          const Texture outputNormal, const Texture outputLightHash,
          const uint32_t index)
  {
    return graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
        name, index,
        svrRendering::SparseVoxelLightFieldPass::Buffers{
          coverage, depth, inactiveRayBuffer, reservoirBuffer, lights,
          unresolvedHitBuffer, reachability, {}, {}, {}, {},
          terrainOwnership},
        svrRendering::SparseVoxelLightFieldPass::Textures{
          normal, normal, replacedTerrainIdentity, replacedTerrainIdentity,
          motion, cutSeed, cutSeedId, output, outputId, reactive, {}, {},
          cutSeedLightHash,
          outputLightHash, cutSeedDepth, outputDepth, cutSeedNormal,
          outputNormal},
        settings);
  };
  auto *cutRetainedPass = makeCutHistoryPass(
      "LightField.RetainAcrossTerrainCut", cutHistorySettings, cutRetained,
      cutRetainedId, cutRetainedReactive, cutRetainedDepth,
      cutRetainedNormal, cutRetainedLightHash, passIndex++);
  auto rejectedCutSettings = cutHistorySettings;
  // Exercise the former publication exception explicitly. Even inside its
  // diagnostic window, terrain history at a different depth is a different
  // receiver and must not be blended into the new voxel face.
  rejectedCutSettings.terrainCommitted = true;
  rejectedCutSettings.terrainHistoryDepthThreshold = 0.01f;
  rejectedCutSettings.raster.previousWorldToClip[15] = 2.0f;
  auto *cutRejectedPass = makeCutHistoryPass(
      "LightField.RejectDifferentTerrainDepth", rejectedCutSettings, cutRejected,
      cutRejectedId, cutRejectedReactive, cutRejectedDepth,
      cutRejectedNormal, cutRejectedLightHash, passIndex++);

  std::vector<SparseVoxelSceneRayGPU> reducedRays(1u);
  reducedRays[0] = activeRay(0u);
  reducedRays[0].pixelRayLight = {1u, 1u, 0u, 0u};
  reducedRays[0].seedFlags[1] |= kSparseVoxelSceneRayFlagRegionSentinel;
  const Buffer reducedRayBuffer = uploadBuffer(
      graph, "LightField.ReducedFeedbackRays", reducedRays,
      BufferUsage::BufferUsage_Storage);
  const Buffer reducedReservoirBuffer = uploadBuffer(
      graph, "LightField.ReducedFeedbackReservoirs",
      std::vector<SparseVoxelDIReservoirGPU>{reservoirs[0]},
      BufferUsage::BufferUsage_Storage);
  const Buffer reducedHitBuffer = uploadBuffer(
      graph, "LightField.ReducedFeedbackHits",
      std::vector<SparseVoxelSceneRayHitGPU>{resolvedHits[0]},
      BufferUsage::BufferUsage_Storage);
  const Texture reducedIdentity = uploadUintTexture(
      graph, "LightField.ReducedFeedbackIdentity", width, height,
      std::vector<uint32_t>{1u, 1u, 1u, 3u});
  constexpr uint32_t reducedRegionWords =
      kSparseVoxelRegionStabilityWordsPerTile;
  const Buffer reducedRegionStability = uploadBuffer(
      graph, "LightField.ReducedRegionStability",
      std::vector<uint32_t>(reducedRegionWords, 0u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  auto reducedFeedbackSettings = cutHistorySettings;
  reducedFeedbackSettings.historyValid = false;
  reducedFeedbackSettings.rayWidth = 1u;
  reducedFeedbackSettings.rayHeight = 1u;
  reducedFeedbackSettings.regionTileSize = 4u;
  reducedFeedbackSettings.regionSoftPrefillStride = 1u;
  auto *reducedFeedbackPass =
      graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
          "LightField.ReducedGridFeedbackOwnership", passIndex++,
          svrRendering::SparseVoxelLightFieldPass::Buffers{
            coverage, depth, reducedRayBuffer, reducedReservoirBuffer, lights,
            reducedHitBuffer, reachability, {}, reducedRegionStability},
          svrRendering::SparseVoxelLightFieldPass::Textures{
            normal, normal, reducedIdentity, reducedIdentity, motion, history,
            historyId, reducedFeedback, reducedFeedbackId,
            reducedFeedbackReactive},
          reducedFeedbackSettings);

  std::vector<uint32_t> awakeRegionWords(
      kSparseVoxelRegionStabilityWordsPerTile, 0u);
  constexpr uint32_t awakeFrame = 7u;
  awakeRegionWords[2] = awakeFrame + 2u;
  const Buffer awakeRegionStability = uploadBuffer(
      graph, "LightField.AwakeRegionStability", awakeRegionWords,
      BufferUsage::BufferUsage_Storage);
  auto awakeSettings = cutHistorySettings;
  awakeSettings.historyValid = true;
  awakeSettings.frameIndex = awakeFrame;
  awakeSettings.regionTileSize = 4u;
  auto *awakePass =
      graph.registerPass<svrRendering::SparseVoxelLightFieldPass>(
          "LightField.PreserveAwakeRegionHistory", passIndex++,
          svrRendering::SparseVoxelLightFieldPass::Buffers{
            coverage, depth, inactiveRayBuffer, reservoirBuffer, lights,
            unresolvedHitBuffer, reachability, {}, awakeRegionStability},
          svrRendering::SparseVoxelLightFieldPass::Textures{
            normal, normal, identity, identity, motion, awakeHistory,
            antiLagHistoryId, awake, awakeId, awakeReactive, {}, {},
            awakeHistoryLightHash, awakeLightHashOutput, cutSeedDepth,
            awakeDepth, cutSeedNormal, awakeNormal},
          awakeSettings);

  const Texture uintTexture = uploadUintTexture(
      graph, "LightField.Uint", width, height,
      std::vector<uint32_t>(pixelCount, 0u));
  const Texture rgTexture = uploadFloatTexture(
      graph, "LightField.RG", Format::Format_RG32Float, width, height,
      std::vector<float>(pixelCount * 2u, 0.0f));
  const Texture depthTexture = uploadFloatTexture(
      graph, "LightField.DepthTexture", Format::Format_Depth32Float,
      width, height, std::vector<float>(pixelCount, 0.5f));
  std::vector<::rendering::Pass *> outputReadPasses;
  const auto readOutput =
      [&](const std::string &name, Texture source, Buffer pixels,
          Buffer readback, uint32_t index)
  {
    const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
      uintTexture, uintTexture, uintTexture, rgTexture, source, normal, normal,
      normal, normal, motion, depthTexture, uintTexture,
    };
    outputReadPasses.push_back(
        graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
        name + ".Read", index, target, pixels,
        SparseVoxelRasterUniformsGPU{
          .viewportTiles = {width, height, 1u, 1u}}));
    outputReadPasses.push_back(
        graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
        name + ".Copy", index + 1u, pixels, 0u,
        uint64_t{pixelCount} * sizeof(SparseVoxelGBufferPixelGPU), readback,
        0u, uint64_t{pixelCount} * sizeof(SparseVoxelGBufferPixelGPU)));
  };
  const Buffer reconstructedPixels = uploadBuffer(
      graph, "LightField.ReconstructedPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer unresolvedPixels = uploadBuffer(
      graph, "LightField.UnresolvedPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer reconstructedReadback = uploadBuffer(
      graph, "LightField.ReconstructedReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer unresolvedReadback = uploadBuffer(
      graph, "LightField.UnresolvedReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer independentPixels = uploadBuffer(
      graph, "LightField.IndependentPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer independentReadback = uploadBuffer(
      graph, "LightField.IndependentReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer antiLagPixels = uploadBuffer(
      graph, "LightField.AntiLagPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer antiLagReadback = uploadBuffer(
      graph, "LightField.AntiLagReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer cutSeedPixels = uploadBuffer(
      graph, "LightField.CutSeedPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer cutSeedReadback = uploadBuffer(
      graph, "LightField.CutSeedReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer cutRetainedPixels = uploadBuffer(
      graph, "LightField.CutRetainedPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer cutRetainedReadback = uploadBuffer(
      graph, "LightField.CutRetainedReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer cutRejectedPixels = uploadBuffer(
      graph, "LightField.CutRejectedPixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer cutRejectedReadback = uploadBuffer(
      graph, "LightField.CutRejectedReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer reducedRegionReadback = uploadBuffer(
      graph, "LightField.ReducedRegionReadback",
      std::vector<uint32_t>(reducedRegionWords, 0u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer awakePixels = uploadBuffer(
      graph, "LightField.AwakePixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer awakeReadback = uploadBuffer(
      graph, "LightField.AwakeReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer awakeReactivePixels = uploadBuffer(
      graph, "LightField.AwakeReactivePixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer awakeReactiveReadback = uploadBuffer(
      graph, "LightField.AwakeReactiveReadback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  readOutput(
      "LightField.Reconstructed", reconstructed, reconstructedPixels,
      reconstructedReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.Unresolved", unresolved, unresolvedPixels,
      unresolvedReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.Independent", independent, independentPixels,
      independentReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.AntiLag", antiLag, antiLagPixels,
      antiLagReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.CutSeed", cutSeed, cutSeedPixels,
      cutSeedReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.CutRetained", cutRetained, cutRetainedPixels,
      cutRetainedReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.CutRejected", cutRejected, cutRejectedPixels,
      cutRejectedReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.Awake", awake, awakePixels, awakeReadback, passIndex);
  passIndex += 2u;
  readOutput(
      "LightField.AwakeReactive", awakeReactive, awakeReactivePixels,
      awakeReactiveReadback, passIndex);
  passIndex += 2u;
  auto *reducedRegionCopy =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "LightField.ReducedRegion.Copy", passIndex++,
          reducedRegionStability, 0u,
          uint64_t{reducedRegionWords} * sizeof(uint32_t),
          reducedRegionReadback, 0u,
          uint64_t{reducedRegionWords} * sizeof(uint32_t));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, pixelCount> reconstructedValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> unresolvedValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> independentValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> antiLagValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> cutSeedValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> cutRetainedValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> cutRejectedValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> awakeValues{};
  std::array<SparseVoxelGBufferPixelGPU, pixelCount> awakeReactiveValues{};
  std::array<uint32_t, reducedRegionWords> reducedRegionValues{};
  graph.bufferRead(
      reconstructedReadback, 0u, sizeof(reconstructedValues),
      [&](const void *data)
      { std::memcpy(reconstructedValues.data(), data, sizeof(reconstructedValues)); });
  graph.bufferRead(
      unresolvedReadback, 0u, sizeof(unresolvedValues),
      [&](const void *data)
      { std::memcpy(unresolvedValues.data(), data, sizeof(unresolvedValues)); });
  graph.bufferRead(
      independentReadback, 0u, sizeof(independentValues),
      [&](const void *data)
      {
        std::memcpy(
            independentValues.data(), data, sizeof(independentValues));
      });
  graph.bufferRead(
      antiLagReadback, 0u, sizeof(antiLagValues),
      [&](const void *data)
      {
        std::memcpy(antiLagValues.data(), data, sizeof(antiLagValues));
      });
  graph.bufferRead(
      cutSeedReadback, 0u, sizeof(cutSeedValues),
      [&](const void *data)
      { std::memcpy(cutSeedValues.data(), data, sizeof(cutSeedValues)); });
  graph.bufferRead(
      cutRetainedReadback, 0u, sizeof(cutRetainedValues),
      [&](const void *data)
      {
        std::memcpy(
            cutRetainedValues.data(), data, sizeof(cutRetainedValues));
      });
  graph.bufferRead(
      cutRejectedReadback, 0u, sizeof(cutRejectedValues),
      [&](const void *data)
      {
        std::memcpy(
            cutRejectedValues.data(), data, sizeof(cutRejectedValues));
      });
  graph.bufferRead(
      awakeReadback, 0u, sizeof(awakeValues),
      [&](const void *data)
      { std::memcpy(awakeValues.data(), data, sizeof(awakeValues)); });
  graph.bufferRead(
      awakeReactiveReadback, 0u, sizeof(awakeReactiveValues),
      [&](const void *data)
      {
        std::memcpy(
            awakeReactiveValues.data(), data, sizeof(awakeReactiveValues));
      });
  graph.bufferRead(
      reducedRegionReadback, 0u, sizeof(reducedRegionValues),
      [&](const void *data)
      {
        std::memcpy(
            reducedRegionValues.data(), data, sizeof(reducedRegionValues));
      });
  for (uint32_t channel = 0u; channel < 3u; ++channel)
  {
    assert(std::abs(reconstructedValues[0].baseColor[channel] - 1.0f) <
           0.02f);
    assert(std::abs(independentValues[0].baseColor[channel] - 2.0f) <
           0.02f);
  }
  assert(std::abs(reconstructedValues[0].baseColor[3] - 1.0f) < 0.02f);
  // A scheduled receiver whose own trace came back inconclusive has no
  // measurement, so it must carry no confidence - but it must still publish a
  // *valid* estimate. The zero-confidence marker is -1, not a sample count of
  // zero: zero is the uncovered-pixel encoding, and both SVGF stages replace
  // an uncovered pixel with black. Publishing zero here painted every
  // inconclusive receiver, and after a-trous alpha averaging its neighbours
  // too, solid black on lit ground.
  assert(unresolvedValues[0].baseColor[3] < 0.0f);
  assert(unresolvedValues[0].baseColor[0] > 0.01f);
  assert(unresolvedValues[0].baseColor[0] < 0.5f);
  // Only pixel zero traced. The other coplanar pixels must receive a
  // zero-confidence analytic prior, not pixel zero's already-resolved
  // visibility. Copying that visibility here is the stationary shadow-spread
  // regression this fixture is intended to catch.
  for (uint32_t pixel = 1u; pixel < pixelCount; ++pixel)
  {
    assert(reconstructedValues[pixel].baseColor[3] < 0.0f);
    assert(reconstructedValues[pixel].baseColor[0] > 0.01f);
    assert(reconstructedValues[pixel].baseColor[0] < 0.5f);
    assert(unresolvedValues[pixel].baseColor[3] < 0.0f);
    for (uint32_t channel = 0u; channel < 3u; ++channel)
      assert(
          // The sky and directional lanes are disjoint estimator strata.
          // Each contributes one; they must be added, not averaged to one.
          std::abs(independentValues[pixel].baseColor[channel] - 2.0f) <
          0.02f);
  }
  assert(
      std::abs(independentValues[0].baseColor[3] -
               static_cast<float>(independentRaysPerPixel)) <
      0.02f);
  for (uint32_t pixel = 1u; pixel < pixelCount; ++pixel)
    assert(
        std::abs(independentValues[pixel].baseColor[3] -
                 static_cast<float>(independentRaysPerPixel)) <
        0.02f);
  // A fully converged bright value must not dominate a current dark sample
  // while the receiver is moving. Before anti-lag this remained ~0.99 with a
  // sample count around 128, then SVGF accumulated that stale value again.
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    assert(antiLagValues[0].baseColor[channel] < 0.2f);
  assert(antiLagValues[0].baseColor[3] <= 9.1f);
  assert(cutSeedValues[0].baseColor[3] > 0.9f);
  assert(cutSeedValues[0].baseColor[0] > 0.9f);
  assert(cutRetainedValues[0].baseColor[3] > 0.9f);
  assert(cutRetainedValues[0].baseColor[0] > 0.9f);
  // A page publication may move the independently quantized surface farther
  // than the ordinary depth tolerance. That is a new receiver, not permission
  // to drag the old shadow across the new face. With no scheduled ray it gets
  // the bounded zero-confidence prior, which a real observation replaces.
  assert(cutRejectedValues[0].baseColor[3] < 0.0f);
  assert(cutRejectedValues[0].baseColor[0] > 0.01f);
  assert(cutRejectedValues[0].baseColor[0] < 0.5f);
  // Region wake-up requests fresh rays but is not evidence that every receiver
  // in the tile changed. Stable neighbours must retain both their light-field
  // confidence and downstream temporal history. The old blanket invalidation
  // reduced alpha from 32 to 8 and wrote a fully reactive tile.
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    assert(std::abs(awakeValues[pixel].baseColor[0] - 0.75f) < 0.02f);
    assert(std::abs(awakeValues[pixel].baseColor[3] - 32.0f) < 0.1f);
    assert(std::abs(awakeReactiveValues[pixel].baseColor[0]) < 0.02f);
  }
  // A 1x1 ray grid representing raster pixel (1,1) must publish that source's
  // instance signature, not the instance at raster pixel (0,0).
  const uint32_t reducedSourceState =
      reducedRegionValues[kSparseVoxelRegionStabilityHeaderWords];
  const uint32_t sourceIdentity = (1u << 29u) | 3u;
  const uint32_t expectedSignature =
      reachabilityMix(
          sky.flags[2] ^ (sky.flags[3] * 0xc2b2ae35u) ^
          (sky.flags[1] * 0x85ebca6bu) ^
          (sourceIdentity * 0x9e3779b9u)) &
      7u;
  assert(reducedSourceState != 0u);
  assert((reducedSourceState >> 29u) == expectedSignature);

  for (auto *pass : outputReadPasses)
    delete pass;
  delete reducedRegionCopy;
  delete awakePass;
  delete reducedFeedbackPass;
  delete antiLagPass;
  delete cutRejectedPass;
  delete cutRetainedPass;
  delete cutSeedPass;
  delete independentPass;
  delete unresolvedPass;
  delete resolvedPass;
}

void testDisabledScreenTraceRoutesToWorld(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  RenderGraph graph(&rhi);
  // A very wide, one-row target reaches mip 11 without allocating a large
  // image. This guards the high-mip hierarchy path that previously made
  // MoltenVK fail while translating the multi-entry-point shader module.
  constexpr uint32_t hierarchyWidth = 2048u;
  const std::vector<float> depthValues(hierarchyWidth, 0.5f);
  std::vector<float> normalValues(hierarchyWidth * 4u, 0.0f);
  for (uint32_t x = 0u; x < hierarchyWidth; ++x)
  {
    normalValues[x * 4u + 2u] = -1.0f;
    normalValues[x * 4u + 3u] = 1.0f;
  }
  const Buffer rays = uploadBuffer(
      graph, "ScreenTraceDisabled.Rays",
      std::vector<SparseVoxelSceneRayGPU>{activeRay(0u)},
      BufferUsage::BufferUsage_Storage);
  const Buffer hits = uploadBuffer(
      graph, "ScreenTraceDisabled.Hits",
      std::vector<SparseVoxelSceneRayHitGPU>(1u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "ScreenTraceDisabled.Readback",
      std::vector<SparseVoxelSceneRayHitGPU>(1u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture depth = uploadFloatTexture(
      graph, "ScreenTraceDisabled.Depth", Format::Format_Depth32Float,
      hierarchyWidth, 1u, depthValues);
  const Texture normal = uploadFloatTexture(
      graph, "ScreenTraceDisabled.Normal", Format::Format_RGBA32Float,
      hierarchyWidth, 1u, normalValues);
  const Texture instanceId = uploadUintTexture(
      graph, "ScreenTraceDisabled.InstanceId", hierarchyWidth, 1u,
      std::vector<uint32_t>(hierarchyWidth, 7u));
  const Texture voxelId = uploadUintTexture(
      graph, "ScreenTraceDisabled.VoxelId", hierarchyWidth, 1u,
      std::vector<uint32_t>(hierarchyWidth, 19u));
  const Buffer activeIndices = uploadBuffer(
      graph, "ScreenTraceDisabled.ActiveIndices",
      std::vector<uint32_t>{0u}, BufferUsage::BufferUsage_Storage);
  const Buffer activeState = uploadBuffer(
      graph, "ScreenTraceDisabled.ActiveState",
      std::vector<uint32_t>{1u, 1u, 1u, 1u},
      BufferUsage::BufferUsage_Storage |
          BufferUsage::BufferUsage_Indirect);

  uint32_t passIndex = 0u;
  auto *tracePass =
      graph.registerPass<svrRendering::SparseVoxelSceneScreenTracePass>(
          "ScreenTraceDisabled.Trace",
          passIndex++,
          depth,
          normal,
          instanceId,
          voxelId,
          svrRendering::SparseVoxelSceneScreenTracePass::Buffers{
            rays, hits, activeIndices, activeState},
          svrRendering::SparseVoxelSceneScreenTracePass::Settings{
            .width = hierarchyWidth,
            .height = 1u,
            .rayCount = 1u,
            .maximumSteps = 8u,
            .reverseZ = true,
            .enabled = false,
          });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "ScreenTraceDisabled.Copy",
      passIndex++,
      hits,
      0u,
      sizeof(SparseVoxelSceneRayHitGPU),
      readback,
      0u,
      sizeof(SparseVoxelSceneRayHitGPU));
  runGraph(graph);

  SparseVoxelSceneRayHitGPU actual{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(actual),
      [&](const void *data)
      {
        std::memcpy(&actual, data, sizeof(actual));
      });
  assert(
      actual.status ==
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::Pending));
  assert(
      actual.identity[2] ==
      static_cast<uint32_t>(SparseVoxelSceneRayBackend::None));

  delete copyPass;
  delete tracePass;
}

// A pinhole camera at the origin looking down +Z, with the world-to-clip matrix
// that exactly inverts the depth reconstruction the trace performs. Column
// major, so each group of four is one column:
//
//   pixel.x = x * fx / z + cx  requires ndc.x =  2x/z, hence clip.x =  2x
//   pixel.y = y * fy / z + cy  requires ndc.y = -2y/z, hence clip.y = -2y
//
// with clip.w = z. The depth channel only has to stay inside [0, 1].
std::array<float, 16> screenTraceClipMatrix()
{
  return {
    2.0f, 0.0f, 0.0f, 0.0f,
    0.0f, -2.0f, 0.0f, 0.0f,
    0.0f, 0.0f, 0.5f, 1.0f,
    0.0f, 0.0f, 0.0f, 0.0f,
  };
}

SparseVoxelRasterUniformsGPU screenTraceRaster(const uint32_t side)
{
  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.worldToClip = screenTraceClipMatrix();
  raster.previousWorldToClip = screenTraceClipMatrix();
  // Camera at the origin; the near plane is the reverse-Z linearization
  // constant, so viewDepth = 1 / deviceDepth.
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 1.0f};
  raster.focalPrincipal = {
    static_cast<float>(side), static_cast<float>(side),
    static_cast<float>(side) * 0.5f, static_cast<float>(side) * 0.5f};
  // Reverse Z clears to zero, which is what marks a pixel as background.
  raster.gbufferClearDepth = 0.0f;
  return raster;
}

// The near-field span the world tracer cannot see.
//
// The world ray's origin is pushed out of its containing trace cell, which on
// terrain is metres, so an occluder inside that displacement is invisible to it
// - and that is exactly where contact shadows and ambient occlusion come from.
// This pass exists to answer that span from the depth buffer, and these two
// cases are the whole contract: an occluder a few pixels away is found, and a
// flat surface with nothing on it is not mistaken for one.
//
// The trace is deliberately given a ray whose recorded source pixel is the
// receiver, because it reconstructs its start from that pixel rather than from
// the ray's own biased origin. A regression to the biased origin would put the
// traversal past the step below and report a miss.
void runScreenTraceNearFieldCase(
    ::rendering::backend::vulkan::VulkanRHI &rhi, const bool withOccluder,
    const bool distinctOccluderIdentity, const bool coplanarEdgeJitter,
    const uint32_t expectedStatus, const char *label)
{
  // At this resolution the old pixel-only thickness is about 0.16 world
  // units, smaller than the half-unit voxel step below. It therefore jumped
  // straight from "not behind" to "too far behind" and missed the blocker.
  constexpr uint32_t side = 256u;
  constexpr float receiverViewDepth = 10.0f;
  // Half a world unit nearer the camera: comfortably more than the trace's
  // two-pixel self-intersection bias at this depth and inside one receiver
  // trace cell, but outside the old pixel-only thickness window.
  constexpr float occluderViewDepth = 9.5f;

  RenderGraph graph(&rhi);
  const std::string name = std::string("ScreenTraceNearField.") + label;

  std::vector<float> depths(side * side, 1.0f / receiverViewDepth);
  constexpr uint32_t blockerX = side / 2u + 15u;
  if (coplanarEdgeJitter)
  {
    // Adjacent coplanar voxel faces can reconstruct a fraction of a pixel
    // apart after depth quantization. An outward grazing ray intersects that
    // numerical plane even though the two cells form one continuous sheet.
    constexpr float adjacentViewDepth = receiverViewDepth - 0.02f;
    for (uint32_t y = 0u; y < side; ++y)
      for (uint32_t x = blockerX; x < side; ++x)
        depths[y * side + x] = 1.0f / adjacentViewDepth;
  }
  else if (withOccluder)
  {
    // A one-pixel-wide foreground strip. It lies between two samples of the
    // old geometric point march, but a cell traversal must visit it.
    for (uint32_t y = 0u; y < side; ++y)
      depths[y * side + blockerX] = 1.0f / occluderViewDepth;
  }
  const Texture depth = uploadFloatTexture(
      graph, name + ".Depth", Format::Format_Depth32Float, side, side, depths);
  std::vector<float> normals(static_cast<size_t>(side) * side * 4u, 0.0f);
  for (size_t pixel = 0u; pixel < static_cast<size_t>(side) * side; ++pixel)
  {
    normals[pixel * 4u + 2u] = -1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture normal = uploadFloatTexture(
      graph, name + ".Normal", Format::Format_RGBA32Float,
      side, side, normals);
  std::vector<uint32_t> instances(side * side, 7u);
  std::vector<uint32_t> voxels(side * side, 19u);
  if (coplanarEdgeJitter)
  {
    for (uint32_t y = 0u; y < side; ++y)
      for (uint32_t x = blockerX; x < side; ++x)
      {
        // A streamed terrain page seam changes both identities even though
        // the visible faces remain one geometric sheet.
        instances[y * side + x] = 8u;
        voxels[y * side + x] = 20u;
      }
  }
  else if (withOccluder && distinctOccluderIdentity)
  {
    for (uint32_t y = 0u; y < side; ++y)
      voxels[y * side + blockerX] = 20u;
  }
  const Texture instanceId = uploadUintTexture(
      graph, name + ".InstanceId", side, side, instances);
  const Texture voxelId = uploadUintTexture(
      graph, name + ".VoxelId", side, side, voxels);

  // The receiver starts on the flat surface and the projected ray crosses the
  // deliberately thin strip above.
  constexpr uint32_t receiverX = side / 2u - 1u;
  constexpr uint32_t receiverY = side / 2u;
  const float cameraX =
      (static_cast<float>(receiverX) + 0.5f - static_cast<float>(side) * 0.5f) /
      static_cast<float>(side);
  const float cameraY =
      (static_cast<float>(receiverY) + 0.5f - static_cast<float>(side) * 0.5f) /
      static_cast<float>(side);
  const float rayLength =
      std::sqrt(cameraX * cameraX + cameraY * cameraY + 1.0f);
  const float scale = receiverViewDepth * rayLength / rayLength;

  SparseVoxelSceneRayGPU ray = activeRay(0u);
  // Origin equal to the reconstructed receiver keeps the reported distance in
  // the same parameterisation as the trace, so the assertion is on status only.
  ray.originMinimumDistance = {
    cameraX * scale, cameraY * scale, receiverViewDepth, 0.0f};
  // Straight along +X is screen right. The coplanar-edge case adds the small
  // outward (-Z) component that intersects its quantized neighbouring plane.
  ray.directionMaximumDistance = coplanarEdgeJitter
      ? std::array<float, 4u>{1.0f, 0.0f, -0.032f, 100.0f}
      : std::array<float, 4u>{1.0f, 0.0f, 0.0f, 100.0f};
  ray.pixelRayLight = {receiverX, receiverY, 0u, 0u};

  const Buffer rays = uploadBuffer(
      graph, name + ".Rays", std::vector<SparseVoxelSceneRayGPU>{ray},
      BufferUsage::BufferUsage_Storage);
  const Buffer hits = uploadBuffer(
      graph, name + ".Hits", std::vector<SparseVoxelSceneRayHitGPU>(1u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, name + ".Readback", std::vector<SparseVoxelSceneRayHitGPU>(1u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer activeIndices = uploadBuffer(
      graph, name + ".ActiveIndices", std::vector<uint32_t>{0u},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeState = uploadBuffer(
      graph, name + ".ActiveState", std::vector<uint32_t>{1u, 1u, 1u, 1u},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);

  uint32_t passIndex = 0u;
  auto *tracePass =
      graph.registerPass<svrRendering::SparseVoxelSceneScreenTracePass>(
          name + ".Trace", passIndex++, depth,
          normal,
          instanceId,
          voxelId,
          svrRendering::SparseVoxelSceneScreenTracePass::Buffers{
            rays, hits, activeIndices, activeState},
          svrRendering::SparseVoxelSceneScreenTracePass::Settings{
            .width = side,
            .height = side,
            .rayCount = 1u,
            .maximumSteps = 8u,
            .reverseZ = true,
            .enabled = true,
            .thicknessSteps = 2.0f,
            // A near field, not the scene diagonal. This is the whole point.
            .maximumDistance = 4.0f,
            .startPixels = 2.0f,
            // Model one receiver trace cell. This is the production floor
            // that keeps voxel-scale contact blockers resolvable at high
            // raster resolutions.
            .minimumThickness = 0.75f,
            .depthLinearA = 0.0f,
            .depthLinearB = 1.0f,
            .raster = screenTraceRaster(side),
          });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      name + ".Copy", passIndex++, hits, 0u, sizeof(SparseVoxelSceneRayHitGPU),
      readback, 0u, sizeof(SparseVoxelSceneRayHitGPU));
  runGraph(graph);

  SparseVoxelSceneRayHitGPU actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data) { std::memcpy(&actual, data, sizeof(actual)); });
  assert(actual.status == expectedStatus);
  assert(
      actual.identity[2] ==
      static_cast<uint32_t>(SparseVoxelSceneRayBackend::Screen));

  delete copyPass;
  delete tracePass;
}

void testScreenTraceLeavesParallelNearFieldOccluderForTree64(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  // A single depth layer cannot prove a side face for a plane-parallel ray.
  // The screen pass must not invent a slab here; the overlapping Tree64 pass
  // is responsible for the real near-field blocker.
  runScreenTraceNearFieldCase(
      rhi, true, true, false,
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax), "Occluded");
}

void testScreenTracePassesFlatSurface(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  runScreenTraceNearFieldCase(
      rhi, false, false, false,
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax), "Open");
}

void testScreenTraceRejectsReceiverVoxel(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  // The depth step is deliberately present, but its pixels carry the same
  // instance/voxel identity as the receiver. It is another raster sample of
  // the receiver itself, not an occluder.
  runScreenTraceNearFieldCase(
      rhi, true, false, false,
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax),
      "ReceiverVoxel");
}

void testScreenTraceRejectsCoplanarVoxelEdge(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  runScreenTraceNearFieldCase(
      rhi, false, true, true,
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax),
      "CoplanarVoxelEdge");
}

SparseVoxelSceneRayHitGPU sceneRayHit(
    const SparseVoxelSceneRayStatus status,
    const SparseVoxelSceneRayBackend backend,
    const float distance)
{
  SparseVoxelSceneRayHitGPU hit{};
  hit.hitPositionDistance = {distance, 0.0f, 0.0f, distance};
  hit.status = static_cast<uint32_t>(status);
  hit.identity = {123u, 0u, static_cast<uint32_t>(backend), kGeneration};
  return hit;
}

SparseVoxelTree64StateGPU sceneRayTestTreeState()
{
  SparseVoxelTree64StateGPU state{};
  state.logicalChunkResolution = {1u, 1u, 1u};
  state.chunkSide = 32u;
  state.valid = 1u;
  state.contentGeneration = kGeneration;
  state.originCell = {-16, -16, -16};
  state.logicalCellResolution = {32u, 32u, 32u};
  state.cellSize = {1.0f, 1.0f, 1.0f};
  state.worldExtent = {32.0f, 32.0f, 32.0f};
  return state;
}

void expectNear(const float actual, const float expected)
{
  assert(std::abs(actual - expected) < 1.0e-4f);
}

// The screen trace only knows about visible depth-buffer geometry. If a ray
// leaves the screen, the world trace must still cover the original full span:
// otherwise an occluder just outside the viewport stops casting a shadow.
void testScreenTraceCompactionPreservesWorldFallback(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t rayCount = 3u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> rays;
  rays.reserve(rayCount);
  for (uint32_t index = 0u; index < rayCount; ++index)
  {
    SparseVoxelSceneRayGPU ray = activeRay(index);
    ray.originMinimumDistance = {0.0f, 0.0f, 0.0f, 0.0f};
    ray.directionMaximumDistance = {1.0f, 0.0f, 0.0f, 10.0f};
    rays.push_back(ray);
  }
  // +Y receiver face in the high flag bits. At a large-world coordinate the
  // compacted visibility ray must preserve the exact receiver: even one f32
  // ULP is a visible centimetre-scale shadow offset here.
  rays[0u].originMinimumDistance[1] = 884.0f;
  rays[0u].seedFlags[1] |= 3u << 4u;

  const std::vector<SparseVoxelSceneRayHitGPU> screenHits{
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Screen, 2.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::MissTMax, SparseVoxelSceneRayBackend::Screen, 3.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::EscapedScene, SparseVoxelSceneRayBackend::Screen, 3.0f),
  };
  const SparseVoxelTree64StateGPU treeState = sceneRayTestTreeState();

  const Buffer rayBuffer = uploadBuffer(
      graph, "SceneRayCompact.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer screenBuffer = uploadBuffer(
      graph, "SceneRayCompact.ScreenHits", screenHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer compactRays = uploadBuffer(
      graph, "SceneRayCompact.CompactRays",
      std::vector<SparseVoxelOccupancyDDARayGPU>(rayCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer compactToCanonical = uploadBuffer(
      graph, "SceneRayCompact.CompactToCanonical",
      std::vector<uint32_t>(rayCount, 0xffffffffu),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer canonicalToCompact = uploadBuffer(
      graph, "SceneRayCompact.CanonicalToCompact",
      std::vector<uint32_t>(rayCount, 0xffffffffu),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer treeDispatch = uploadBuffer(
      graph, "SceneRayCompact.TreeDispatch",
      std::vector<SparseVoxelTree64TraceDispatchGPU>(1u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer indirectArgs = uploadBuffer(
      graph, "SceneRayCompact.IndirectArgs", std::vector<uint32_t>(4u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);
  const Buffer treeStateBuffer = uploadBuffer(
      graph, "SceneRayCompact.TreeState",
      std::vector<SparseVoxelTree64StateGPU>{treeState},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeIndices = uploadBuffer(
      graph, "SceneRayCompact.ActiveIndices",
      std::vector<uint32_t>{0u, 1u, 2u}, BufferUsage::BufferUsage_Storage);
  const Buffer activeState = uploadBuffer(
      graph, "SceneRayCompact.ActiveState",
      std::vector<uint32_t>{1u, 1u, 1u, rayCount},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);
  const Buffer compactReadback = uploadBuffer(
      graph, "SceneRayCompact.CompactReadback",
      std::vector<SparseVoxelOccupancyDDARayGPU>(rayCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer canonicalReadback = uploadBuffer(
      graph, "SceneRayCompact.CanonicalReadback",
      std::vector<uint32_t>(rayCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Buffer dispatchReadback = uploadBuffer(
      graph, "SceneRayCompact.DispatchReadback",
      std::vector<SparseVoxelTree64TraceDispatchGPU>(1u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  const svrRendering::SparseVoxelSceneRayCompactionPass::Buffers buffers{
    rayBuffer, screenBuffer, compactRays, compactToCanonical,
    canonicalToCompact, treeDispatch, indirectArgs, treeStateBuffer,
    activeIndices, activeState};
  uint32_t passIndex = 0u;
  auto *resetPass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayCompactionPass>(
          "SceneRayCompact.Reset", passIndex++,
          svrRendering::SparseVoxelSceneRayCompactionPass::Stage::Reset,
          buffers, rayCount, true, true);
  auto *compactPass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayCompactionPass>(
          "SceneRayCompact.Compact", passIndex++,
          svrRendering::SparseVoxelSceneRayCompactionPass::Stage::Compact,
          buffers, rayCount, true, true);
  auto *copyCompact =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "SceneRayCompact.CopyRays", passIndex++, compactRays, 0u,
          rayCount * sizeof(SparseVoxelOccupancyDDARayGPU), compactReadback,
          0u, rayCount * sizeof(SparseVoxelOccupancyDDARayGPU));
  auto *copyCanonical =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "SceneRayCompact.CopyCanonical", passIndex++, canonicalToCompact, 0u,
          rayCount * sizeof(uint32_t), canonicalReadback, 0u,
          rayCount * sizeof(uint32_t));
  auto *copyDispatch =
      graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
          "SceneRayCompact.CopyDispatch", passIndex++, treeDispatch, 0u,
          sizeof(SparseVoxelTree64TraceDispatchGPU), dispatchReadback, 0u,
          sizeof(SparseVoxelTree64TraceDispatchGPU));
  runGraph(graph);

  std::array<SparseVoxelOccupancyDDARayGPU, rayCount> compacted{};
  std::array<uint32_t, rayCount> canonical{};
  SparseVoxelTree64TraceDispatchGPU dispatch{};
  graph.bufferRead(
      compactReadback, 0u, sizeof(compacted),
      [&](const void *data) { std::memcpy(compacted.data(), data, sizeof(compacted)); });
  graph.bufferRead(
      canonicalReadback, 0u, sizeof(canonical),
      [&](const void *data) { std::memcpy(canonical.data(), data, sizeof(canonical)); });
  graph.bufferRead(
      dispatchReadback, 0u, sizeof(dispatch),
      [&](const void *data) { std::memcpy(&dispatch, data, sizeof(dispatch)); });

  assert(dispatch.rayCount == rayCount);
  for (const uint32_t compactIndex : canonical)
    assert(compactIndex < rayCount);

  const auto &screenHitRay = compacted[canonical[0u]];
  // Receiver rejection belongs to Tree64, where the authored cell size and
  // face are known. Compaction must not move the traversal origin.
  expectNear(screenHitRay.originMaximumDistance[0], 0.0f);
  expectNear(screenHitRay.originMaximumDistance[1], 884.0f);
  expectNear(screenHitRay.originMaximumDistance[3], 0.0f);
  // direction.w preserves the face metadata Tree64 needs to select and skip
  // the receiver cell without an origin bias. +Y is face 3.
  expectNear(screenHitRay.direction[3], -(1.0f + 3.0f * 128.0f));

  const auto &screenMissRay = compacted[canonical[1u]];
  expectNear(screenMissRay.originMaximumDistance[0], 3.0f);
  expectNear(screenMissRay.originMaximumDistance[3], 7.0f);
  expectNear(screenMissRay.direction[3], 3.0f);

  const auto &escapedScreenRay = compacted[canonical[2u]];
  // Leaving the screen re-traces the complete world span from the exact
  // receiver; an off-screen occluder may begin immediately beyond its cell.
  expectNear(escapedScreenRay.originMaximumDistance[0], 0.0f);
  expectNear(escapedScreenRay.originMaximumDistance[3], 10.0f);
  expectNear(escapedScreenRay.direction[3], 0.0f);

  delete copyDispatch;
  delete copyCanonical;
  delete copyCompact;
  delete compactPass;
  delete resetPass;
}

void testScreenAndWorldHitMergeContracts(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t rayCount = 4u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> rays;
  rays.reserve(rayCount);
  for (uint32_t index = 0u; index < rayCount; ++index)
  {
    SparseVoxelSceneRayGPU ray = activeRay(index);
    ray.originMinimumDistance = {0.0f, 0.0f, 0.0f, 0.0f};
    ray.directionMaximumDistance = {1.0f, 0.0f, 0.0f, 10.0f};
    if (index == 2u)
      ray.seedFlags[1] = 0u;
    rays.push_back(ray);
  }
  const std::vector<SparseVoxelSceneRayHitGPU> screenHits{
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Screen, 2.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::MissTMax, SparseVoxelSceneRayBackend::Screen, 3.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::EscapedScene, SparseVoxelSceneRayBackend::Screen, 3.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Screen, 2.0f),
  };
  std::vector<SparseVoxelOccupancyDDAResultGPU> treeHits(rayCount);
  for (uint32_t index = 0u; index < rayCount; ++index)
  {
    treeHits[index].globalCell = {static_cast<int32_t>(index), 0, 0};
    treeHits[index].status = 2u;
    treeHits[index].distance = index == 1u ? 4.0f : 2.0f;
    treeHits[index].contentGeneration = kGeneration;
  }
  // A coplanar screen-space thickness false positive. Tree64 is authoritative
  // for binary visibility and reports no authored occluder for this ray.
  treeHits[3u].status = 1u;
  std::vector<SparseVoxelOccupancyDDARayGPU> treeRays(rayCount);
  treeRays[1u].direction[3] = 3.0f;

  const SparseVoxelTree64StateGPU treeState = sceneRayTestTreeState();
  const Buffer treeStateBuffer = uploadBuffer(
      graph, "SceneRayMerge.TreeState",
      std::vector<SparseVoxelTree64StateGPU>{treeState},
      BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = uploadBuffer(
      graph, "SceneRayMerge.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer screenBuffer = uploadBuffer(
      graph, "SceneRayMerge.ScreenHits", screenHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer treeHitBuffer = uploadBuffer(
      graph, "SceneRayMerge.TreeHits", treeHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer canonicalToCompact = uploadBuffer(
      graph, "SceneRayMerge.CanonicalToCompact",
      std::vector<uint32_t>{0u, 1u, 2u, 3u}, BufferUsage::BufferUsage_Storage);
  const Buffer mergedHits = uploadBuffer(
      graph, "SceneRayMerge.MergedHits",
      std::vector<SparseVoxelSceneRayHitGPU>(rayCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer treeRayBuffer = uploadBuffer(
      graph, "SceneRayMerge.TreeRays", treeRays,
      BufferUsage::BufferUsage_Storage);
  const Buffer activeIndices = uploadBuffer(
      graph, "SceneRayMerge.ActiveIndices",
      std::vector<uint32_t>{0u, 1u, 2u, 3u}, BufferUsage::BufferUsage_Storage);
  const Buffer activeState = uploadBuffer(
      graph, "SceneRayMerge.ActiveState",
      std::vector<uint32_t>{1u, 1u, 1u, rayCount},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);
  const Buffer readback = uploadBuffer(
      graph, "SceneRayMerge.Readback",
      std::vector<SparseVoxelSceneRayHitGPU>(rayCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  uint32_t passIndex = 0u;
  auto *mergePass = graph.registerPass<svrRendering::SparseVoxelSceneHitMergePass>(
      "SceneRayMerge.Merge", passIndex++,
      svrRendering::SparseVoxelSceneHitMergePass::Buffers{
        treeStateBuffer, rayBuffer, screenBuffer, treeHitBuffer,
        canonicalToCompact, mergedHits, treeRayBuffer, activeIndices,
        activeState},
      rayCount, std::array<float, 3u>{-8.0f, -8.0f, -8.0f},
      std::array<float, 3u>{8.0f, 8.0f, 8.0f}, true, true);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRayMerge.Copy", passIndex++, mergedHits, 0u,
      rayCount * sizeof(SparseVoxelSceneRayHitGPU), readback, 0u,
      rayCount * sizeof(SparseVoxelSceneRayHitGPU));
  runGraph(graph);

  std::array<SparseVoxelSceneRayHitGPU, rayCount> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data) { std::memcpy(actual.data(), data, sizeof(actual)); });

  assert(actual[0u].status == static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit));
  assert(actual[0u].identity[2] == static_cast<uint32_t>(SparseVoxelSceneRayBackend::Screen));
  expectNear(actual[0u].hitPositionDistance[3], 2.0f);

  assert(actual[1u].status == static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit));
  assert(actual[1u].identity[2] == static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64));
  expectNear(actual[1u].hitPositionDistance[0], 7.0f);
  expectNear(actual[1u].hitPositionDistance[3], 7.0f);

  assert(actual[2u].status == static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit));
  assert(actual[2u].identity[2] == static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64));
  expectNear(actual[2u].hitPositionDistance[0], 2.0f);
  expectNear(actual[2u].hitPositionDistance[3], 2.0f);

  assert(actual[3u].status == static_cast<uint32_t>(
      SparseVoxelSceneRayStatus::EscapedScene));
  assert(actual[3u].identity[2] == static_cast<uint32_t>(
      SparseVoxelSceneRayBackend::Tree64));

  delete copyPass;
  delete mergePass;
}

void testSceneRayStatisticsCountsMergedScreenAndWorldContracts(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t rayCount = 4u;
  RenderGraph graph(&rhi);

  std::vector<SparseVoxelSceneRayGPU> rays;
  rays.reserve(rayCount);
  for (uint32_t index = 0u; index < rayCount; ++index)
  {
    SparseVoxelSceneRayGPU ray = activeRay(index);
    ray.pixelRayLight[3] = index == 2u ? 1u : 0u;
    if (index == 3u)
      ray.seedFlags[1] = 0u;
    rays.push_back(ray);
  }
  const std::vector<SparseVoxelSceneRayHitGPU> screenHits{
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Screen, 2.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::MissTMax, SparseVoxelSceneRayBackend::Screen, 3.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::EscapedScene, SparseVoxelSceneRayBackend::Screen, 3.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::Invalid, SparseVoxelSceneRayBackend::None, 0.0f),
  };
  const std::vector<SparseVoxelSceneRayHitGPU> mergedHits{
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Screen, 2.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Tree64, 7.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::Hit, SparseVoxelSceneRayBackend::Tree64, 2.0f),
    sceneRayHit(SparseVoxelSceneRayStatus::Invalid, SparseVoxelSceneRayBackend::None, 0.0f),
  };
  std::vector<SparseVoxelDIReservoirGPU> reservoirs(rayCount);
  reservoirs[0u].identity[2] = 2u;
  reservoirs[0u].state[1] = 1u;
  reservoirs[1u].identity[2] = 3u;
  reservoirs[1u].state[2] = 2u;
  reservoirs[2u].identity[2] = 4u;
  reservoirs[3u].identity[2] = 8u;

  WorldGILightGPU direct = directionalLight();
  WorldGILightGPU sky{};
  sky.flags[0] = WorldGILightFlag_Enabled;
  sky.flags[1] = WorldGILightType_Sky;
  sky.flags[3] = kGeneration;
  const Buffer rayBuffer = uploadBuffer(
      graph, "SceneRayStats.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer screenBuffer = uploadBuffer(
      graph, "SceneRayStats.ScreenHits", screenHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer mergedBuffer = uploadBuffer(
      graph, "SceneRayStats.MergedHits", mergedHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer reservoirBuffer = uploadBuffer(
      graph, "SceneRayStats.Reservoirs", reservoirs,
      BufferUsage::BufferUsage_Storage);
  const Buffer lightBuffer = uploadBuffer(
      graph, "SceneRayStats.Lights", std::vector<WorldGILightGPU>{direct, sky},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeIndices = uploadBuffer(
      graph, "SceneRayStats.ActiveIndices",
      std::vector<uint32_t>{0u, 1u, 2u, 3u},
      BufferUsage::BufferUsage_Storage);
  const Buffer activeState = uploadBuffer(
      graph, "SceneRayStats.ActiveState",
      std::vector<uint32_t>{1u, 1u, 1u, rayCount},
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect);
  std::vector<SparseVoxelOccupancyDDAResultGPU> treeHits(rayCount);
  treeHits[0u].status = 2u; // Tree64 hit.
  treeHits[1u].status = 2u;
  treeHits[2u].status = 1u; // Tree64 miss.
  const Buffer treeHitBuffer = uploadBuffer(
      graph, "SceneRayStats.TreeHits", treeHits,
      BufferUsage::BufferUsage_Storage);
  const Buffer canonicalToCompact = uploadBuffer(
      graph, "SceneRayStats.CanonicalToCompact",
      std::vector<uint32_t>{0u, 1u, 2u, 0xffffffffu},
      BufferUsage::BufferUsage_Storage);
  const Buffer statistics = uploadBuffer(
      graph, "SceneRayStats.Statistics",
      std::vector<SparseVoxelSceneRayStatisticsGPU>(1u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "SceneRayStats.Readback",
      std::vector<SparseVoxelSceneRayStatisticsGPU>(1u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  uint32_t passIndex = 0u;
  auto *resetPass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayStatisticsResetPass>(
          "SceneRayStats.Reset", passIndex++, statistics);
  auto *statsPass =
      graph.registerPass<svrRendering::SparseVoxelSceneRayStatisticsPass>(
          "SceneRayStats.Collect", passIndex++,
          svrRendering::SparseVoxelSceneRayStatisticsPass::Buffers{
            rayBuffer, screenBuffer, mergedBuffer, reservoirBuffer, lightBuffer,
            activeIndices, activeState, treeHitBuffer, canonicalToCompact,
            statistics},
          rayCount, 2u, true, true);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SceneRayStats.Copy", passIndex++, statistics, 0u,
      sizeof(SparseVoxelSceneRayStatisticsGPU), readback, 0u,
      sizeof(SparseVoxelSceneRayStatisticsGPU));
  runGraph(graph);

  SparseVoxelSceneRayStatisticsGPU actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data) { std::memcpy(&actual, data, sizeof(actual)); });

  assert(actual.rayCount == 4u);
  assert(actual.screenHits == 1u);
  assert(actual.screenMisses == 2u);
  assert(actual.worldHits == 2u);
  assert(actual.worldMisses == 0u);
  assert(actual.invalidRays == 1u);
  assert(actual.reservoirM == 9u);
  assert(actual.temporalReservoirs == 1u);
  assert(actual.spatialReservoirs == 1u);
  assert(actual.skyRayHits == 1u);
  assert(actual.skyRayCount == 1u);
  assert(actual.screenWorldBothHit == 1u);
  assert(actual.screenHitWorldMiss == 0u);
  assert(actual.screenHitWorldUnknown == 0u);
  assert(actual.screenMissWorldHit == 1u);

  delete copyPass;
  delete statsPass;
  delete resetPass;
}

void testSvgfAccumulatesAlternatingSkySamples(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 2u;
  RenderGraph graph(&rhi);
  const Texture bright = uploadFloatTexture(
      graph, "SVGF.Bright", Format::Format_RGBA32Float, width, 1u,
      {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f});
  const Texture current = uploadFloatTexture(
      graph, "SVGF.Current", Format::Format_RGBA32Float, width, 1u,
      {
        0.0f, 0.0f, 0.0f, 1.0f,
        // Negative alpha is the light field's zero-count reconstructed marker.
        // It is valid denoiser input even though it must not enter the light
        // field's measured running mean on the next frame.
        0.25f, 0.25f, 0.25f, -1.0f,
      });
  const Texture motion = uploadFloatTexture(
      graph, "SVGF.Motion", Format::Format_RG32Float, width, 1u,
      {0.0f, 0.0f, 0.0f, 0.0f});
  const Texture reactive = uploadFloatTexture(
      graph, "SVGF.Reactive", Format::Format_R32Float, width, 1u,
      {1.0f, 1.0f});
  const Texture currentDepth = uploadFloatTexture(
      graph, "SVGF.CurrentDepth", Format::Format_Depth32Float, width, 1u,
      {0.5f, 0.5f});
  const Texture historyDepth = uploadFloatTexture(
      graph, "SVGF.HistoryDepth", Format::Format_Depth32Float, width, 1u,
      {0.9f, 0.5f});
  const Texture normal = uploadFloatTexture(
      graph, "SVGF.Normal", Format::Format_RGBA32Float, width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f});
  const Texture currentIdentity = uploadUintTexture(
      graph, "SVGF.CurrentIdentity", width, 1u, {7u, 8u});
  const Texture historyIdentity = uploadUintTexture(
      graph, "SVGF.HistoryIdentity", width, 1u, {7u, 9u});
  const auto outputTexture = [&](const char *name)
  {
    return graph.createTexture({
      .name = name,
      .format = Format::Format_RGBA16Float,
      .height = 1u,
      .width = width,
      .mipLevels = 1u,
      .usage =
          ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture firstOutput = outputTexture("SVGF.FirstOutput");
  const Texture secondOutput = outputTexture("SVGF.SecondOutput");

  ::rendering::gpgpu::SvgfDenoisePass::Settings firstSettings{};
  firstSettings.width = width;
  firstSettings.height = 1u;
  firstSettings.historyWeight = 0.96f;
  firstSettings.disocclusionWeight = 0.0f;
  firstSettings.historyClipStrength = 0.0f;
  firstSettings.historyValid = false;
  uint32_t passIndex = 0u;
  auto *firstPass = graph.registerPass<::rendering::gpgpu::SvgfDenoisePass>(
      "SVGF.First",
      passIndex++,
      bright,
      current,
      motion,
      reactive,
      historyDepth,
      normal,
      historyDepth,
      normal,
      historyIdentity,
      historyIdentity,
      historyIdentity,
      historyIdentity,
      firstOutput,
      firstSettings);
  auto secondSettings = firstSettings;
  secondSettings.historyValid = true;
  auto *secondPass = graph.registerPass<::rendering::gpgpu::SvgfDenoisePass>(
      "SVGF.Second",
      passIndex++,
      current,
      firstOutput,
      motion,
      reactive,
      currentDepth,
      normal,
      historyDepth,
      normal,
      currentIdentity,
      currentIdentity,
      historyIdentity,
      historyIdentity,
      secondOutput,
      secondSettings);

  const Texture uintTexture = uploadUintTexture(
      graph, "SVGF.Uint", width, 1u, {0u, 0u});
  const Texture rgTexture = uploadFloatTexture(
      graph, "SVGF.RG", Format::Format_RG32Float, width, 1u,
      {0.0f, 0.0f, 0.0f, 0.0f});
  const Buffer pixels = uploadBuffer(
      graph,
      "SVGF.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph,
      "SVGF.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    uintTexture,
    uintTexture,
    uintTexture,
    rgTexture,
    secondOutput,
    normal,
    normal,
    normal,
    normal,
    motion,
    currentDepth,
    uintTexture,
  };
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "SVGF.ReadOutput", passIndex++, target, pixels, raster);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SVGF.CopyOutput",
      passIndex++,
      pixels,
      0u,
      width * sizeof(SparseVoxelGBufferPixelGPU),
      readback,
      0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(actual),
      [&](const void *data)
      {
        std::memcpy(&actual, data, sizeof(actual));
      });
  // Stable instance/voxel identity allows camera-motion reuse even though
  // reverse-Z device depth changed. A different identity at the same depth
  // rejects history, but the reconstructed current signal must survive rather
  // than being mistaken for an uncovered black pixel.
  for (uint32_t channel = 0u; channel < 3u; ++channel)
  {
    assert(std::abs(actual[0].baseColor[channel] - 0.5f) < 0.02f);
    assert(std::abs(actual[1].baseColor[channel] - 0.25f) < 0.02f);
  }
  assert(std::abs(actual[0].baseColor[3] - 2.0f) < 0.02f);
  assert(std::abs(actual[1].baseColor[3] - 0.25f) < 0.02f);

  delete copyPass;
  delete textureReadback;
  delete secondPass;
  delete firstPass;
}

void testLightingAccumulationIsIndependentOfSvgf(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 3u;
  RenderGraph graph(&rhi);
  const Texture first = uploadFloatTexture(
      graph, "LightingAccumulation.First", Format::Format_RGBA32Float,
      width, 1u,
      {2.0f, 1.0f, 0.5f, 1.0f,
       0.5f, 0.25f, 0.125f, 1.0f,
       2.0f, 1.0f, 0.5f, 1.0f});
  const Texture second = uploadFloatTexture(
      graph, "LightingAccumulation.Second", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 0.0f, 0.0f, 1.0f,
       0.0f, 0.0f, 0.0f, -1.0f,
       0.0f, 0.0f, 0.0f, 1.0f});
  const Texture historyDepth = uploadFloatTexture(
      graph, "LightingAccumulation.HistoryDepth", Format::Format_Depth32Float,
      width, 1u, {0.5f, 0.5f, 0.5f});
  const Texture currentDepth = uploadFloatTexture(
      graph, "LightingAccumulation.CurrentDepth", Format::Format_Depth32Float,
      width, 1u, {0.5f, 0.5f, 0.9f});
  const Texture normal = uploadFloatTexture(
      graph, "LightingAccumulation.Normal", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f});
  const Texture currentVoxel = uploadUintTexture(
      graph, "LightingAccumulation.CurrentVoxel", width, 1u,
      {8u, 9u, 10u});
  const Texture currentInstance = uploadUintTexture(
      graph, "LightingAccumulation.CurrentInstance", width, 1u,
      {1u, 1u, 2u});
  const Texture firstMotion = uploadFloatTexture(
      graph, "LightingAccumulation.FirstMotion", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Texture secondMotion = uploadFloatTexture(
      graph, "LightingAccumulation.SecondMotion", Format::Format_RG32Float,
      width, 1u,
      {-1.0f / static_cast<float>(width), 0.0f,
       -1.0f / static_cast<float>(width), 0.0f,
       0.0f, 0.0f});
  const auto outputTexture = [&](const char *name)
  {
    return graph.createTexture({
      .name = name,
      .format = Format::Format_RGBA16Float,
      .height = 1u,
      .width = width,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_Storage |
          ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture firstOutput =
      outputTexture("LightingAccumulation.FirstOutput");
  const Texture secondOutput =
      outputTexture("LightingAccumulation.SecondOutput");

  using Accumulator =
      ::rendering::gpgpu::LightingTemporalAccumulationPass;
  Accumulator::Settings firstSettings{};
  firstSettings.width = width;
  firstSettings.height = 1u;
  firstSettings.maxHistoryFrames = 32.0f;
  firstSettings.surfaceTolerancePixels = 0.5f;
  firstSettings.cameraStationary = true;
  uint32_t passIndex = 0u;
  auto *firstPass = graph.registerPass<Accumulator>(
      "LightingAccumulation.FirstPass", passIndex++,
      Accumulator::Textures{
        first, first, historyDepth, normal, historyDepth, normal, firstMotion,
        firstOutput},
      firstSettings);
  auto secondSettings = firstSettings;
  secondSettings.historyValid = true;
  secondSettings.cameraStationary = false;
  // A one-pixel camera translation changes NDC by 2/width. Express it around
  // the previous camera origin, matching the runtime's large-world-safe
  // current-view to history-view transform.
  secondSettings.currentClipToHistoryView[12] =
      2.0f / static_cast<float>(width);
  secondSettings.currentClipToHistoryClip[12] =
      2.0f / static_cast<float>(width);
  auto *secondPass = graph.registerPass<Accumulator>(
      "LightingAccumulation.SecondPass", passIndex++,
      Accumulator::Textures{
        second, firstOutput, currentDepth, normal, historyDepth, normal,
        secondMotion,
        secondOutput},
      secondSettings);

  const Texture rgTexture = uploadFloatTexture(
      graph, "LightingAccumulation.RG", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "LightingAccumulation.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "LightingAccumulation.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    currentInstance, currentVoxel, currentVoxel, rgTexture,
    secondOutput, normal, normal, normal, normal, rgTexture, currentDepth,
    currentVoxel};
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "LightingAccumulation.ReadOutput", passIndex++, target, pixels,
          raster);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "LightingAccumulation.CopyOutput", passIndex++, pixels, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // Camera motion moves current pixel zero onto previous pixel one. The
  // reprojected history and current dark estimate form a running mean.
  assert(std::abs(actual[0].baseColor[0] - 0.25f) < 0.02f);
  assert(std::abs(actual[0].baseColor[1] - 0.125f) < 0.02f);
  assert(std::abs(actual[0].baseColor[2] - 0.0625f) < 0.02f);
  assert(std::abs(actual[0].baseColor[3] - 2.0f) < 0.02f);
  // An unresolved receiver keeps the compatible estimate reprojected from
  // previous pixel two instead of resetting during camera motion.
  assert(std::abs(actual[1].baseColor[0] - 2.0f) < 0.02f);
  assert(std::abs(actual[1].baseColor[1] - 1.0f) < 0.02f);
  assert(std::abs(actual[1].baseColor[2] - 0.5f) < 0.02f);
  assert(std::abs(actual[1].baseColor[3] - 1.0f) < 0.02f);
  // Pixel two has no matching reprojected identity and rejects history.
  assert(std::abs(actual[2].baseColor[0]) < 0.02f);
  assert(std::abs(actual[2].baseColor[3] - 1.0f) < 0.02f);

  delete copyPass;
  delete textureReadback;
  delete secondPass;
  delete firstPass;
}

void testStationaryLightingIgnoresSpuriousMotion(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 2u;
  RenderGraph graph(&rhi);
  const Texture current = uploadFloatTexture(
      graph, "LightingStationary.Current", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 0.0f, 0.0f, 1.0f,
       0.0f, 0.0f, 0.0f, 1.0f});
  const Texture history = uploadFloatTexture(
      graph, "LightingStationary.History", Format::Format_RGBA32Float,
      width, 1u,
      {1.0f, 1.0f, 1.0f, 1.0f,
       0.0f, 0.0f, 0.0f, 1.0f});
  const Texture depth = uploadFloatTexture(
      graph, "LightingStationary.Depth", Format::Format_Depth32Float,
      width, 1u, {0.5f, 0.5f});
  const Texture normal = uploadFloatTexture(
      graph, "LightingStationary.Normal", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f});
  // Pixel zero falsely claims it came from pixel one. Both surfaces have the
  // same depth and normal, so geometry rejection alone cannot identify this
  // stationary-camera drift.
  const Texture motion = uploadFloatTexture(
      graph, "LightingStationary.Motion", Format::Format_RG32Float,
      width, 1u, {-0.5f, 0.0f, 0.0f, 0.0f});
  const Texture output = graph.createTexture({
    .name = "LightingStationary.Output",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  using Accumulator =
      ::rendering::gpgpu::LightingTemporalAccumulationPass;
  Accumulator::Settings settings{};
  settings.width = width;
  settings.height = 1u;
  settings.historyValid = true;
  settings.cameraStationary = true;
  uint32_t passIndex = 0u;
  auto *accumulator = graph.registerPass<Accumulator>(
      "LightingStationary.Accumulate", passIndex++,
      Accumulator::Textures{
        current, history, depth, normal, depth, normal, motion, output},
      settings);

  const Texture identity = uploadUintTexture(
      graph, "LightingStationary.Identity", width, 1u, {1u, 1u});
  const Texture rg = uploadFloatTexture(
      graph, "LightingStationary.RG", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "LightingStationary.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "LightingStationary.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, rg, output, normal, normal, normal, normal,
    rg, depth, identity};
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "LightingStationary.ReadOutput", passIndex++, target, pixels,
          raster);
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "LightingStationary.CopyOutput", passIndex++, pixels, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // One current dark sample plus one exact-pixel bright history sample.
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    assert(std::abs(actual[0].baseColor[channel] - 0.5f) < 0.02f);
  assert(std::abs(actual[0].baseColor[3] - 2.0f) < 0.02f);

  delete copy;
  delete textureReadback;
  delete accumulator;
}

void testRotatingCameraPrefersGeometricReprojection(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 4u;
  RenderGraph graph(&rhi);
  const Texture current = uploadFloatTexture(
      graph, "LightingRotation.Current", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 0.0f, 0.0f, 1.0f,
       0.0f, 0.0f, 0.0f, -1.0f,
       0.0f, 0.0f, 0.0f, 1.0f,
       0.0f, 0.0f, 0.0f, 1.0f});
  const Texture history = uploadFloatTexture(
      graph, "LightingRotation.History", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 0.0f, 0.0f, 24.0f,
       0.0f, 0.0f, 0.0f, 24.0f,
       1.0f, 1.0f, 1.0f, 24.0f,
       1.0f, 1.0f, 1.0f, 24.0f});
  const Texture depth = uploadFloatTexture(
      graph, "LightingRotation.Depth", Format::Format_Depth32Float,
      width, 1u, {0.5f, 0.5f, 0.5f, 0.5f});
  const Texture normal = uploadFloatTexture(
      graph, "LightingRotation.Normal", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f});
  // Camera reprojection maps pixel one a quarter pixel across the sharp edge
  // between history pixels one and two. This stale motion vector instead points at
  // dark pixel two; all taps deliberately have compatible depth and normals,
  // reproducing the rotating flat-terrain failure mode.
  const Texture motion = uploadFloatTexture(
      graph, "LightingRotation.Motion", Format::Format_RG32Float,
      width, 1u,
      {-2.0f / static_cast<float>(width), 0.0f,
       0.0f, 0.0f,
       0.0f, 0.0f,
       0.0f, 0.0f});
  const Texture output = graph.createTexture({
    .name = "LightingRotation.Output",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  using Accumulator =
      ::rendering::gpgpu::LightingTemporalAccumulationPass;
  Accumulator::Settings settings{};
  settings.width = width;
  settings.height = 1u;
  settings.historyValid = true;
  settings.cameraStationary = false;
  settings.surfaceTolerancePixels = 0.5f;
  settings.currentClipToHistoryView[12] =
      0.5f / static_cast<float>(width);
  settings.currentClipToHistoryClip[12] =
      0.5f / static_cast<float>(width);
  uint32_t passIndex = 0u;
  auto *accumulator = graph.registerPass<Accumulator>(
      "LightingRotation.Accumulate", passIndex++,
      Accumulator::Textures{
        current, history, depth, normal, depth, normal, motion, output},
      settings);

  const Texture identity = uploadUintTexture(
      graph, "LightingRotation.Identity", width, 1u, {1u, 1u, 1u, 1u});
  const Texture rg = uploadFloatTexture(
      graph, "LightingRotation.RG", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "LightingRotation.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "LightingRotation.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, rg, output, normal, normal, normal, normal,
    rg, depth, identity};
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "LightingRotation.ReadOutput", passIndex++, target, pixels,
          raster);
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "LightingRotation.CopyOutput", passIndex++, pixels, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // Pixel zero remains on the settled dark side of the shadow.
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    assert(std::abs(actual[0].baseColor[channel]) < 0.02f);
  // Camera motion retains the converged history count. The old four-frame cap
  // reduced this to five immediately, causing the moving image's variance and
  // spatial-filter blur to jump despite successful reprojection.
  assert(std::abs(actual[0].baseColor[3] - 25.0f) < 0.05f);
  // Pixel one only has an SVGF reconstruction. Catmull-Rom reconstructs the
  // quarter-pixel step at 0.203125; recursive bilinear reprojection would
  // produce 0.25 and progressively soften the edge on each rotation frame.
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    assert(std::abs(actual[1].baseColor[channel] - 0.203125f) < 0.03f);
  assert(std::abs(actual[1].baseColor[3] - 24.0f) < 0.05f);

  delete copy;
  delete textureReadback;
  delete accumulator;
}

void testLightingReprojectionRecoversNearbyNoisyLightfieldHistory(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 5u;
  RenderGraph graph(&rhi);
  const Texture current = uploadFloatTexture(
      graph, "LightingNearbyHistory.Current", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 0.0f, 0.0f, 1.0f,
       0.0f, 0.0f, 0.0f, -2.0f,
       0.0f, 0.0f, 0.0f, -2.0f,
       0.0f, 0.0f, 0.0f, -2.0f,
       0.0f, 0.0f, 0.0f, -2.0f});
  const Texture history = uploadFloatTexture(
      graph, "LightingNearbyHistory.NoisyLightfieldHistory",
      Format::Format_RGBA32Float, width, 1u,
      {0.2f, 0.1f, 0.0f, 16.0f,
       0.0f, 0.0f, 0.0f, 0.0f,
       0.0f, 0.0f, 0.0f, 0.0f,
       0.8f, 0.6f, 0.2f, 16.0f,
       0.0f, 0.0f, 0.0f, 0.0f});
  const Texture depth = uploadFloatTexture(
      graph, "LightingNearbyHistory.Depth", Format::Format_Depth32Float,
      width, 1u, std::vector<float>(width, 0.5f));
  const Texture normal = uploadFloatTexture(
      graph, "LightingNearbyHistory.Normal", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f});
  const Texture motion = uploadFloatTexture(
      graph, "LightingNearbyHistory.Motion", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Texture output = graph.createTexture({
    .name = "LightingNearbyHistory.Output",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  using Accumulator =
      ::rendering::gpgpu::LightingTemporalAccumulationPass;
  Accumulator::Settings settings{};
  settings.width = width;
  settings.height = 1u;
  settings.historyValid = true;
  settings.cameraStationary = false;
  settings.surfaceTolerancePixels = 3.0f;
  uint32_t passIndex = 0u;
  auto *accumulator = graph.registerPass<Accumulator>(
      "LightingNearbyHistory.Accumulate", passIndex++,
      Accumulator::Textures{
        current, history, depth, normal, depth, normal, motion, output},
      settings);

  const Texture identity = uploadUintTexture(
      graph, "LightingNearbyHistory.Identity", width, 1u,
      {1u, 1u, 1u, 1u, 1u});
  const Texture rg = uploadFloatTexture(
      graph, "LightingNearbyHistory.RG", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "LightingNearbyHistory.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "LightingNearbyHistory.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, rg, output, normal, normal, normal, normal,
    rg, depth, identity};
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "LightingNearbyHistory.ReadOutput", passIndex++, target, pixels,
          raster);
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "LightingNearbyHistory.CopyOutput", passIndex++, pixels, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // Pixel one is covered this frame but its sparse ReSTIR light sample has not
  // arrived. The bilinear reprojection footprint contains no valid accumulated
  // rays, but the same receiver still has noisy accumulated lightfield samples
  // nearby after a small camera rotation. Gather those compatible taps with a
  // compact distance kernel instead of dropping to an unresolved black texel or
  // selecting only the nearest sample.
  const float nearWeight = std::exp(-0.5f);
  const float farWeight = std::exp(-2.0f);
  const float weightSum = nearWeight + farWeight;
  const float expectedR = (0.2f * nearWeight + 0.8f * farWeight) / weightSum;
  const float expectedG = (0.1f * nearWeight + 0.6f * farWeight) / weightSum;
  const float expectedB = (0.0f * nearWeight + 0.2f * farWeight) / weightSum;
  assert(std::abs(actual[1].baseColor[0] - expectedR) < 0.03f);
  assert(std::abs(actual[1].baseColor[1] - expectedG) < 0.03f);
  assert(std::abs(actual[1].baseColor[2] - expectedB) < 0.03f);
  assert(std::abs(actual[1].baseColor[3] - 16.0f) < 0.05f);

  delete copy;
  delete textureReadback;
  delete accumulator;
}

void testLightingAccumulationFillsUnresolvedCurrentSparseWindow(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 5u;
  RenderGraph graph(&rhi);
  const Texture current = uploadFloatTexture(
      graph, "LightingCurrentFallback.Current", Format::Format_RGBA32Float,
      width, 1u,
      {0.25f, 0.25f, 0.25f, 1.0f,
       0.0f, 0.0f, 0.0f, -2.0f,
       0.0f, 0.0f, 0.0f, -2.0f,
       1.0f, 1.0f, 1.0f, 1.0f,
       0.0f, 0.0f, 0.0f, -2.0f});
  const Texture history = uploadFloatTexture(
      graph, "LightingCurrentFallback.History", Format::Format_RGBA32Float,
      width, 1u, std::vector<float>(width * 4u, 0.0f));
  const Texture depth = uploadFloatTexture(
      graph, "LightingCurrentFallback.Depth", Format::Format_Depth32Float,
      width, 1u, std::vector<float>(width, 0.5f));
  const Texture normal = uploadFloatTexture(
      graph, "LightingCurrentFallback.Normal", Format::Format_RGBA32Float,
      width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f,
       0.0f, 1.0f, 0.0f, 1.0f});
  const Texture motion = uploadFloatTexture(
      graph, "LightingCurrentFallback.Motion", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Texture output = graph.createTexture({
    .name = "LightingCurrentFallback.Output",
    .format = Format::Format_RGBA16Float,
    .height = 1u,
    .width = width,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  using Accumulator =
      ::rendering::gpgpu::LightingTemporalAccumulationPass;
  Accumulator::Settings settings{};
  settings.width = width;
  settings.height = 1u;
  settings.enabled = false;
  settings.historyValid = false;
  settings.cameraStationary = true;
  settings.currentFallbackRadiusPixels = 4.0f;
  settings.surfaceTolerancePixels = 3.0f;
  uint32_t passIndex = 0u;
  auto *accumulator = graph.registerPass<Accumulator>(
      "LightingCurrentFallback.Accumulate", passIndex++,
      Accumulator::Textures{
        current, history, depth, normal, depth, normal, motion, output},
      settings);

  const Texture identity = uploadUintTexture(
      graph, "LightingCurrentFallback.Identity", width, 1u,
      {1u, 1u, 1u, 1u, 1u});
  const Texture rg = uploadFloatTexture(
      graph, "LightingCurrentFallback.RG", Format::Format_RG32Float,
      width, 1u, std::vector<float>(width * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "LightingCurrentFallback.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "LightingCurrentFallback.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, rg, output, normal, normal, normal, normal,
    rg, depth, identity};
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "LightingCurrentFallback.ReadOutput", passIndex++, target, pixels,
          raster);
  auto *copy = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "LightingCurrentFallback.CopyOutput", passIndex++, pixels, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // Pixel one is covered but unresolved in the sparse current frame. Even when
  // temporal blending is disabled, same-surface current lightfield samples are
  // enough to reconstruct a valid one-sample estimate, preventing compose from
  // multiplying the surface by black.
  const float nearWeight = std::exp(-0.125f);
  const float farWeight = std::exp(-0.5f);
  const float expected = (0.25f * nearWeight + 1.0f * farWeight) /
      (nearWeight + farWeight);
  for (uint32_t channel = 0u; channel < 3u; ++channel)
    assert(std::abs(actual[1].baseColor[channel] - expected) < 0.03f);
  assert(std::abs(actual[1].baseColor[3] - 1.0f) < 0.02f);

  delete copy;
  delete textureReadback;
  delete accumulator;
}

void testSvgfReconstructsUnresolvedSparseWindow(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 4u;
  constexpr uint32_t height = 4u;
  constexpr uint32_t pixelCount = width * height;
  RenderGraph graph(&rhi);

  std::vector<float> sparse(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
    sparse[pixel * 4u + 3u] = -2.0f;
  sparse[0u] = 1.0f;
  sparse[1u] = 0.75f;
  sparse[2u] = 0.5f;
  sparse[3u] = 1.0f;
  const Texture source = uploadFloatTexture(
      graph, "SVGF.SparseWindow.Source", Format::Format_RGBA32Float,
      width, height, sparse);
  const Texture depth = uploadFloatTexture(
      graph, "SVGF.SparseWindow.Depth", Format::Format_Depth32Float,
      width, height, std::vector<float>(pixelCount, 0.5f));
  std::vector<float> normals(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    normals[pixel * 4u + 1u] = 1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture normal = uploadFloatTexture(
      graph, "SVGF.SparseWindow.Normal", Format::Format_RGBA32Float,
      width, height, normals);
  std::vector<uint32_t> identities(pixelCount);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
    identities[pixel] = pixel + 1u;
  const Texture instance = uploadUintTexture(
      graph, "SVGF.SparseWindow.Instance", width, height, identities);
  const Texture voxel = uploadUintTexture(
      graph, "SVGF.SparseWindow.Voxel", width, height, identities);
  const auto storageTexture = [&](const char *name)
  {
    return graph.createTexture({
      .name = name,
      .format = Format::Format_RGBA16Float,
      .height = height,
      .width = width,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture intermediate = storageTexture("SVGF.SparseWindow.Intermediate");
  const Texture output = storageTexture("SVGF.SparseWindow.Output");

  ::rendering::gpgpu::SvgfAtrousFilterPass::Settings settings{};
  settings.width = width;
  settings.height = height;
  settings.stepWidth = 1u;
  settings.spatialStrength = 1.0f;
  settings.maxHistoryFrames = 1.0f;
  uint32_t passIndex = 0u;
  auto *firstPass =
      graph.registerPass<::rendering::gpgpu::SvgfAtrousFilterPass>(
          "SVGF.SparseWindow.ATrous1", passIndex++, source, depth, normal,
          intermediate, settings);
  settings.stepWidth = 2u;
  settings.outputHitMask = true;
  auto *secondPass =
      graph.registerPass<::rendering::gpgpu::SvgfAtrousFilterPass>(
          "SVGF.SparseWindow.ATrous2", passIndex++, intermediate, depth,
          normal, output, settings);

  const Texture materialUv = uploadFloatTexture(
      graph, "SVGF.SparseWindow.MaterialUv", Format::Format_RG32Float,
      width, height, std::vector<float>(pixelCount * 2u, 0.0f));
  const Texture motion = uploadFloatTexture(
      graph, "SVGF.SparseWindow.Motion", Format::Format_RG32Float,
      width, height, std::vector<float>(pixelCount * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "SVGF.SparseWindow.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "SVGF.SparseWindow.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, height, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    instance, voxel, instance, materialUv, output, normal, normal, normal,
    normal, motion, depth, instance,
  };
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "SVGF.SparseWindow.ReadOutput", passIndex++, target, pixels, raster);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SVGF.SparseWindow.CopyOutput", passIndex++, pixels, 0u,
      pixelCount * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      pixelCount * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, pixelCount> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  for (const auto &pixel : actual)
  {
    assert(std::abs(pixel.baseColor[0] - 1.0f) < 0.03f);
    assert(std::abs(pixel.baseColor[1] - 0.75f) < 0.03f);
    assert(std::abs(pixel.baseColor[2] - 0.5f) < 0.03f);
    assert(std::abs(pixel.baseColor[3] - 1.0f) < 0.03f);
  }

  delete copyPass;
  delete textureReadback;
  delete secondPass;
  delete firstPass;
}

void testSvgfFiltersCoplanarLightingOutlier(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 8u;
  constexpr uint32_t pixelCount = width;
  RenderGraph graph(&rhi);

  std::vector<float> lighting(pixelCount * 4u, 0.0f);
  std::vector<float> depth(pixelCount, 0.5f);
  std::vector<float> normals(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    const bool outlier = pixel == width / 2u;
    lighting[pixel * 4u + 0u] = outlier ? 0.0f : 1.0f;
    lighting[pixel * 4u + 1u] = outlier ? 0.0f : 1.0f;
    lighting[pixel * 4u + 2u] = outlier ? 0.0f : 1.0f;
    lighting[pixel * 4u + 3u] = 32.0f;
    normals[pixel * 4u + 1u] = 1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture source = uploadFloatTexture(
      graph, "SVGF.DepthEdge.Source", Format::Format_RGBA32Float,
      width, 1u, lighting);
  const Texture depthTexture = uploadFloatTexture(
      graph, "SVGF.DepthEdge.Depth", Format::Format_Depth32Float,
      width, 1u, depth);
  const Texture normal = uploadFloatTexture(
      graph, "SVGF.DepthEdge.Normal", Format::Format_RGBA32Float,
      width, 1u, normals);
  const auto storageTexture = [&](const char *name)
  {
    return graph.createTexture({
      .name = name,
      .format = Format::Format_RGBA16Float,
      .height = 1u,
      .width = width,
      .mipLevels = 1u,
      .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture intermediate = storageTexture("SVGF.DepthEdge.Intermediate");
  const Texture output = storageTexture("SVGF.DepthEdge.Output");

  ::rendering::gpgpu::SvgfAtrousFilterPass::Settings settings{};
  settings.width = width;
  settings.height = 1u;
  settings.stepWidth = 1u;
  settings.spatialStrength = 1.0f;
  // Depth and normals are deliberately identical. A single mature black
  // visibility outlier must be filtered instead of protected as a boundary.
  settings.maxHistoryFrames = 32.0f;
  uint32_t passIndex = 0u;
  auto *firstPass =
      graph.registerPass<::rendering::gpgpu::SvgfAtrousFilterPass>(
          "SVGF.DepthEdge.ATrous1", passIndex++, source, depthTexture,
          normal, intermediate, settings);
  settings.stepWidth = 2u;
  settings.outputHitMask = true;
  auto *secondPass =
      graph.registerPass<::rendering::gpgpu::SvgfAtrousFilterPass>(
          "SVGF.DepthEdge.ATrous2", passIndex++, intermediate, depthTexture,
          normal, output, settings);

  std::vector<uint32_t> identities(pixelCount, 1u);
  const Texture identity = uploadUintTexture(
      graph, "SVGF.DepthEdge.Identity", width, 1u, identities);
  const Texture materialUv = uploadFloatTexture(
      graph, "SVGF.DepthEdge.MaterialUv", Format::Format_RG32Float,
      width, 1u, std::vector<float>(pixelCount * 2u, 0.0f));
  const Texture motion = uploadFloatTexture(
      graph, "SVGF.DepthEdge.Motion", Format::Format_RG32Float,
      width, 1u, std::vector<float>(pixelCount * 2u, 0.0f));
  const Buffer pixels = uploadBuffer(
      graph, "SVGF.DepthEdge.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "SVGF.DepthEdge.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(pixelCount),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, materialUv, output, normal, normal, normal,
    normal, motion, depthTexture, identity,
  };
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "SVGF.DepthEdge.ReadOutput", passIndex++, target, pixels, raster);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SVGF.DepthEdge.CopyOutput", passIndex++, pixels, 0u,
      pixelCount * sizeof(SparseVoxelGBufferPixelGPU), readback, 0u,
      pixelCount * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, pixelCount> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    if (pixel == width / 2u)
    {
      assert(actual[pixel].baseColor[0] > 0.50f);
      assert(actual[pixel].baseColor[1] > 0.50f);
      assert(actual[pixel].baseColor[2] > 0.50f);
    }
  }

  delete copyPass;
  delete textureReadback;
  delete secondPass;
  delete firstPass;
}

void testTsrReprojectsStochasticLightingWithoutCurrentFrameClipping(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 2u;
  RenderGraph graph(&rhi);
  const Texture currentColor = uploadFloatTexture(
      graph, "TSR.CurrentColor", Format::Format_RGBA32Float, width, 1u,
      {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f});
  const Texture currentNormal = uploadFloatTexture(
      graph, "TSR.CurrentNormal", Format::Format_RGBA32Float, width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f});
  const Texture currentDepth = uploadFloatTexture(
      graph, "TSR.CurrentDepth", Format::Format_Depth32Float, width, 1u,
      {0.5f, 0.5f});
  // The raw G-buffer velocity contains a +0.25 UV jitter delta. Once TSR
  // removes it, both pixels should reproject to their own history texel.
  const Texture rawMotion = uploadFloatTexture(
      graph, "TSR.RawMotion", Format::Format_RG32Float, width, 1u,
      {0.25f, 0.0f, 0.25f, 0.0f});
  const Texture inputDisocclusion = uploadFloatTexture(
      graph, "TSR.InputDisocclusion", Format::Format_R32Float, width, 1u,
      {0.0f, 0.0f});
  const Texture historyColor = uploadFloatTexture(
      graph, "TSR.HistoryColor", Format::Format_RGBA32Float, width, 1u,
      {1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f});
  // Pixel zero represents the same receiver after camera translation, so its
  // device depth changes while its normal remains stable. Pixel one has a
  // conflicting historical normal and must not receive full history weight.
  const Texture historyDepth = uploadFloatTexture(
      graph, "TSR.HistoryDepth", Format::Format_R32Float, width, 1u,
      {0.9f, 0.5f});
  const Texture historyNormal = uploadFloatTexture(
      graph, "TSR.HistoryNormal", Format::Format_RGBA32Float, width, 1u,
      {0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f});
  const Texture historyLock = uploadFloatTexture(
      graph, "TSR.HistoryLock", Format::Format_RGBA32Float, width, 1u,
      std::vector<float>(width * 4u, 0.0f));
  const Texture historyLuma = uploadFloatTexture(
      graph, "TSR.HistoryLuma", Format::Format_RGBA32Float, width, 1u,
      std::vector<float>(width * 4u, 0.0f));
  const Texture historyMotion = uploadFloatTexture(
      graph, "TSR.HistoryMotion", Format::Format_RGBA32Float, width, 1u,
      std::vector<float>(width * 4u, 0.0f));
  // Identity rejection is left off for this case, so these only have to exist
  // and be bindable; the shader never reads them.
  const Texture currentInstance = uploadUintTexture(
      graph, "TSR.CurrentInstance", width, 1u, {0u, 0u});
  const Texture currentVoxel = uploadUintTexture(
      graph, "TSR.CurrentVoxel", width, 1u, {0u, 0u});

  const auto storageTexture = [&](const char *name, const Format format)
  {
    return graph.createTexture({
      .name = name,
      .format = format,
      .height = 1u,
      .width = width,
      .mipLevels = 1u,
      .usage =
          ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    });
  };
  const Texture output = storageTexture(
      "TSR.Output", Format::Format_RGBA16Float);
  const Texture outputHistoryColor = storageTexture(
      "TSR.OutputHistoryColor", Format::Format_RGBA16Float);
  const Texture outputHistoryDepth = storageTexture(
      "TSR.OutputHistoryDepth", Format::Format_R32Float);
  const Texture outputHistoryNormal = storageTexture(
      "TSR.OutputHistoryNormal", Format::Format_RGBA16Float);
  const Texture historyId = storageTexture(
      "TSR.HistoryId", Format::Format_RG32Uint);
  const Texture outputHistoryId = storageTexture(
      "TSR.OutputHistoryId", Format::Format_RG32Uint);
  const Texture outputLock = storageTexture(
      "TSR.OutputLock", Format::Format_RGBA16Float);
  const Texture outputLuma = storageTexture(
      "TSR.OutputLuma", Format::Format_RGBA16Float);
  const Texture outputMotion = storageTexture(
      "TSR.OutputMotion", Format::Format_RGBA16Float);
  const Texture outputDisocclusion = storageTexture(
      "TSR.OutputDisocclusion", Format::Format_R16Float);
  // Diagnostic targets. Settings::debugTerms is left off here, so the
  // accumulator never writes them; they only have to exist to bind.
  const Texture outputDebugTermsA = storageTexture(
      "TSR.OutputDebugTermsA", Format::Format_RGBA16Float);
  const Texture outputDebugTermsB = storageTexture(
      "TSR.OutputDebugTermsB", Format::Format_RGBA16Float);
  // Settings::accumulationWeighting is left off, so the accumulator neither
  // reads nor writes these; they only have to exist to bind.
  const Texture historyAccumulation = storageTexture(
      "TSR.HistoryAccumulation", Format::Format_R16Float);
  const Texture outputAccumulation = storageTexture(
      "TSR.OutputAccumulation", Format::Format_R16Float);
  // Settings::shadingChangeStrength is left at zero, so the accumulator never
  // consults these; the coarse-luma stage still writes its target.
  const Texture historyCoarseLuma = storageTexture(
      "TSR.HistoryCoarseLuma", Format::Format_R32Float);
  const Texture outputCoarseLuma = storageTexture(
      "TSR.OutputCoarseLuma", Format::Format_R32Float);

  ::rendering::gpgpu::TemporalSuperResolutionPass::Settings settings{};
  settings.sourceViewPortWidth = width;
  settings.sourceViewPortHeight = 1u;
  settings.outputViewPortWidth = width;
  settings.outputViewPortHeight = 1u;
  settings.currentJitter[0] = 0.25f;
  settings.previousJitter[0] = -0.25f;
  settings.historyValid = true;
  settings.reverseDepth = true;
  settings.stochasticInput = true;
  settings.accumulationFrames = 16.0f;
  settings.sharpenStrength = 0.0f;
  uint32_t passIndex = 0u;
  auto *tsrPass =
      graph.registerPass<::rendering::gpgpu::TemporalSuperResolutionPass>(
          "TSR.Resolve",
          passIndex++,
          currentColor,
          currentNormal,
          currentDepth,
          rawMotion,
          inputDisocclusion,
          currentInstance,
          currentVoxel,
          historyColor,
          historyDepth,
          historyNormal,
          historyId,
          historyLock,
          historyLuma,
          historyMotion,
          historyAccumulation,
          historyCoarseLuma,
          output,
          outputHistoryColor,
          outputHistoryDepth,
          outputHistoryNormal,
          outputHistoryId,
          outputLock,
          outputLuma,
          outputMotion,
          outputDisocclusion,
          outputDebugTermsA,
          outputDebugTermsB,
          outputAccumulation,
          outputCoarseLuma,
          settings);

  const Texture uintTexture = uploadUintTexture(
      graph, "TSR.Uint", width, 1u, {0u, 0u});
  const Texture rgTexture = uploadFloatTexture(
      graph, "TSR.RG", Format::Format_RG32Float, width, 1u,
      {0.0f, 0.0f, 0.0f, 0.0f});
  const Buffer pixels = uploadBuffer(
      graph,
      "TSR.Pixels",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph,
      "TSR.Readback",
      std::vector<SparseVoxelGBufferPixelGPU>(width),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.viewportTiles = {width, 1u, 1u, 1u};
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    uintTexture,
    uintTexture,
    uintTexture,
    rgTexture,
    outputHistoryColor,
    currentNormal,
    currentNormal,
    currentNormal,
    currentNormal,
    rawMotion,
    currentDepth,
    uintTexture,
  };
  auto *textureReadback =
      graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>(
          "TSR.ReadOutput", passIndex++, target, pixels, raster);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "TSR.CopyOutput",
      passIndex++,
      pixels,
      0u,
      width * sizeof(SparseVoxelGBufferPixelGPU),
      readback,
      0u,
      width * sizeof(SparseVoxelGBufferPixelGPU));
  runGraph(graph);

  std::array<SparseVoxelGBufferPixelGPU, width> actual{};
  graph.bufferRead(
      readback,
      0u,
      sizeof(actual),
      [&](const void *data)
      {
        std::memcpy(&actual, data, sizeof(actual));
      });
  for (uint32_t channel = 0u; channel < 3u; ++channel)
  {
    assert(actual[0].baseColor[channel] > 0.85f);
    assert(actual[1].baseColor[channel] < 0.45f);
  }

  delete copyPass;
  delete textureReadback;
  delete tsrPass;
}


// Soft shadows come from an area light being sampled across its extent: if
// every ray went to the same point the light would be a point light and every
// shadow edge would be hard. These cases build synthetic scenes with a single
// area light and assert the three properties that make a penumbra possible and
// well behaved.
//
// 1. Sampled emitter points actually lie on the light's surface.
// 2. Those points are spread over the emitter rather than collapsing to one.
// 3. The RIS candidate weight target / (selectionPdf * emitterPdf) stays
//    bounded across the emitter.
//
// (3) is the regression guard for the firefly work. Uniform *area* sampling of
// an emitter gives a weight carrying a raw cos_emitter * area / d^2, which
// diverges as a receiver approaches the light; sampling the subtended solid
// angle instead cancels both terms and the weight flattens out. A revert to
// area sampling shows up here as a large max/mean ratio.
struct AreaLightSampleStats
{
  uint32_t activeCount = 0u;
  // Rays that were emitted but cannot contribute (target 0). Uniform area
  // sampling of a sphere puts half its samples on the far hemisphere, where
  // the emitter faces away and the sample shades to black; solid-angle
  // sampling only ever generates points on the visible cap, so this stays
  // near zero. It is the sharpest signal that the sampler regressed.
  uint32_t wastedCount = 0u;
  float maximumSurfaceError = 0.0f;
  float pointSpread = 0.0f;
  float weightMaximum = 0.0f;
  float weightMean = 0.0f;
};

AreaLightSampleStats measureAreaLightSampling(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const std::string &name,
    const WorldGILightGPU &light,
    const std::array<float, 3> &lightCenter,
    const float sphereRadius,
    const std::array<float, 3> &quadTangent,
    const std::array<float, 3> &quadBitangent,
    const float quadHalfWidth,
    const float quadHalfHeight,
    const bool isSphere,
    const uint32_t gridSide,
    const float receiverDepth)
{
  RenderGraph graph(&rhi, RenderGraph::Settings{.maxTimers = 16u, .maxFramesInFlight = 1u});
  const uint32_t rayCount = gridSide * gridSide;

  const Buffer coverage = uploadBuffer(
      graph, name + ".Coverage", std::vector<uint32_t>(rayCount, 1u), BufferUsage::BufferUsage_Storage);
  const Buffer depth = uploadBuffer(
      graph, name + ".Depth", std::vector<float>(rayCount, receiverDepth), BufferUsage::BufferUsage_Storage);
  const Buffer rays = uploadBuffer(
      graph, name + ".Rays", std::vector<SparseVoxelSceneRayGPU>(rayCount), BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer lights = uploadBuffer(
      graph, name + ".Lights", std::vector<WorldGILightGPU>{light}, BufferUsage::BufferUsage_Storage);

  // A single leaf covering everything, so selection always resolves to this
  // light and the emitter sampling is what is under test.
  WorldGILightBVHNodeGPU leaf{};
  leaf.boundsMinNodeType[0] = -1.0e6f;
  leaf.boundsMinNodeType[1] = -1.0e6f;
  leaf.boundsMinNodeType[2] = -1.0e6f;
  leaf.boundsMinNodeType[3] = static_cast<float>(WorldGILightBVHNodeType_Leaf);
  leaf.boundsMaxImportance[0] = 1.0e6f;
  leaf.boundsMaxImportance[1] = 1.0e6f;
  leaf.boundsMaxImportance[2] = 1.0e6f;
  leaf.boundsMaxImportance[3] = 1.0f;
  leaf.childOrLightCount[0] = kWorldGILightBVHInvalidNode;
  leaf.childOrLightCount[1] = kWorldGILightBVHInvalidNode;
  leaf.childOrLightCount[2] = 0u;
  leaf.childOrLightCount[3] = 1u;
  const Buffer bvhNodes = uploadBuffer(
      graph, name + ".BVHNodes", std::vector<WorldGILightBVHNodeGPU>{leaf}, BufferUsage::BufferUsage_Storage);
  WorldGILightBVHInfoGPU info{};
  info.nodeCount = 1u;
  info.rootNodeIndex = 0u;
  info.lightCount = 1u;
  const Buffer bvhInfo = uploadBuffer(
      graph, name + ".BVHInfo", std::vector<WorldGILightBVHInfoGPU>{info}, BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(
      graph, name + ".Readback", std::vector<SparseVoxelSceneRayGPU>(rayCount), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  // Receivers face +Y, toward the light.
  std::vector<float> normals(static_cast<size_t>(rayCount) * 4u, 0.0f);
  for (uint32_t index = 0u; index < rayCount; ++index)
  {
    normals[static_cast<size_t>(index) * 4u + 1u] = 1.0f;
    normals[static_cast<size_t>(index) * 4u + 3u] = 1.0f;
  }
  const Texture normalTexture = uploadFloatTexture(
      graph, name + ".Normal", Format::Format_RGBA32Float, gridSide, gridSide, normals);
  const Texture lightFieldTexture = graph.createTexture({
    .name = name + ".LightField",
    .format = Format::Format_RGBA16Float,
    .height = gridSide,
    .width = gridSide,
    .mipLevels = 1u,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
  });

  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToCamera = identityMatrix();
  raster.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  // A long focal length keeps every receiver in a small patch directly under
  // the emitter. A unit focal sprays them across a wide cone, where some end
  // up with the light behind them and the case under test is diluted.
  const float focal = 64.0f * static_cast<float>(gridSide);
  raster.focalPrincipal = {focal, focal, 0.5f * static_cast<float>(gridSide), 0.5f * static_cast<float>(gridSide)};
  raster.viewportTiles = {gridSide, gridSide, 1u, 1u};

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelSceneRayCreatePass>(
      name + ".Create",
      passIndex++,
      svrRendering::SparseVoxelSceneRayCreatePass::Buffers{
        coverage, depth, rays, lights, bvhNodes, bvhInfo, normalTexture, lightFieldTexture},
      svrRendering::SparseVoxelSceneRayCreatePass::Settings{
        raster,
        {0.0f, 1.0f, 0.0f},
        64.0f,
        0.001f,
        gridSide,
        gridSide,
        1u,
        1u,
        kGeneration,
        5u,
        gridSide,
        gridSide,
        0u,
      });
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      name + ".Copy", passIndex++, rays, 0u,
      uint64_t{rayCount} * sizeof(SparseVoxelSceneRayGPU), readback, 0u,
      uint64_t{rayCount} * sizeof(SparseVoxelSceneRayGPU));
  runGraph(graph);

  std::vector<SparseVoxelSceneRayGPU> results(rayCount);
  graph.bufferRead(
      readback, 0u, uint64_t{rayCount} * sizeof(SparseVoxelSceneRayGPU),
      [&](const void *data) { std::memcpy(results.data(), data, results.size() * sizeof(SparseVoxelSceneRayGPU)); });

  AreaLightSampleStats stats{};
  std::array<double, 3> pointSum{0.0, 0.0, 0.0};
  std::array<double, 3> pointSquareSum{0.0, 0.0, 0.0};
  double weightSum = 0.0;
  for (const SparseVoxelSceneRayGPU &ray : results)
  {
    if ((ray.seedFlags[1] & kSparseVoxelSceneRayFlagActive) == 0u)
      continue;
    std::array<float, 3> point{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      point[axis] = ray.originMinimumDistance[axis] +
          ray.directionMaximumDistance[axis] * ray.directionMaximumDistance[3];

    float surfaceError = 0.0f;
    if (isSphere)
    {
      float distanceSquared = 0.0f;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const float delta = point[axis] - lightCenter[axis];
        distanceSquared += delta * delta;
      }
      surfaceError = std::abs(std::sqrt(distanceSquared) - sphereRadius);
    }
    else
    {
      float alongTangent = 0.0f;
      float alongBitangent = 0.0f;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        const float delta = point[axis] - lightCenter[axis];
        alongTangent += delta * quadTangent[axis];
        alongBitangent += delta * quadBitangent[axis];
      }
      surfaceError = std::max(
          std::max(0.0f, std::abs(alongTangent) - quadHalfWidth),
          std::max(0.0f, std::abs(alongBitangent) - quadHalfHeight));
    }
    const float selectionPdf = ray.lightSamplePdfWeight[0];
    const float emitterPdf = ray.lightSamplePdfWeight[1];
    const float target = ray.lightSamplePdfWeight[2];
    assert(std::isfinite(selectionPdf) && selectionPdf > 0.0f);
    assert(std::isfinite(emitterPdf) && emitterPdf > 0.0f);
    assert(std::isfinite(target) && target >= 0.0f);
    // A zero target means this sample cannot contribute - the emitter point
    // faces away, or the light is behind the receiver. The reservoir drops
    // those, so they are not part of the weight distribution under test.
    if (target <= 0.0f)
    {
      ++stats.wastedCount;
      continue;
    }
    const float weight = target / (selectionPdf * emitterPdf);
    assert(std::isfinite(weight));
    ++stats.activeCount;
    stats.weightMaximum = std::max(stats.weightMaximum, weight);
    weightSum += weight;
    stats.maximumSurfaceError = std::max(stats.maximumSurfaceError, surfaceError);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      pointSum[axis] += point[axis];
      pointSquareSum[axis] += static_cast<double>(point[axis]) * point[axis];
    }
  }

  if (stats.activeCount != 0u)
  {
    stats.weightMean = static_cast<float>(weightSum / stats.activeCount);
    double varianceSum = 0.0;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const double mean = pointSum[axis] / stats.activeCount;
      varianceSum += std::max(0.0, pointSquareSum[axis] / stats.activeCount - mean * mean);
    }
    stats.pointSpread = static_cast<float>(std::sqrt(varianceSum));
  }
  return stats;
}

void testSphereAreaLightProducesBoundedSoftShadowSamples(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  // Receivers sit on the plane y = 0 looking up at a sphere light one unit
  // above them; the radius is large relative to that distance so the emitter
  // subtends a wide cone. This is the configuration that used to fire: with
  // uniform area sampling half the samples land on the far hemisphere and the
  // inverse-square term is at its worst.
  constexpr float kRadius = 0.5f;
  const std::array<float, 3> center{0.0f, 1.0f, 0.0f};
  WorldGILightGPU light{};
  light.positionRange[0] = center[0];
  light.positionRange[1] = center[1];
  light.positionRange[2] = center[2];
  light.positionRange[3] = 64.0f;
  light.shapeParams[0] = kRadius;
  light.colorIntensity[0] = 1.0f;
  light.colorIntensity[1] = 1.0f;
  light.colorIntensity[2] = 1.0f;
  light.colorIntensity[3] = 4.0f;
  light.flags[0] = WorldGILightFlag_Enabled;
  light.flags[1] = WorldGILightType_Sphere;
  light.flags[3] = kGeneration;

  const AreaLightSampleStats stats = measureAreaLightSampling(
      rhi, "SoftShadowSphere", light, center, kRadius, {}, {}, 0.0f, 0.0f,
      true, 16u, 1.0f);

  assert(stats.activeCount > 0u);
  // Cone sampling only generates points on the cap facing the receiver, so
  // almost nothing is wasted. Uniform area sampling would put roughly half the
  // samples on the far hemisphere and fail this.
  assert(stats.wastedCount * 10u < stats.activeCount);
  // Every sampled point lies on the emitter surface.
  assert(stats.maximumSurfaceError < 1.0e-2f);
  // And they are spread across it, which is what gives a penumbra rather than
  // a hard point-light edge.
  assert(stats.pointSpread > 0.05f);
  // Bounded candidate weights: the property solid-angle sampling buys.
  assert(stats.weightMean > 0.0f);
  assert(stats.weightMaximum <= stats.weightMean * 8.0f);
}

void testPlanarAreaLightProducesBoundedSoftShadowSamples(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  // A quad emitter facing down at receivers one unit below it. Its normal
  // points toward the receivers so the one-sided emitter test passes.
  constexpr float kHalfWidth = 0.6f;
  constexpr float kHalfHeight = 0.4f;
  const std::array<float, 3> center{0.0f, 1.0f, 0.0f};
  WorldGILightGPU light{};
  light.positionRange[0] = center[0];
  light.positionRange[1] = center[1];
  light.positionRange[2] = center[2];
  light.positionRange[3] = 64.0f;
  light.directionType[1] = -1.0f;
  light.shapeParams[1] = kHalfWidth;
  light.shapeParams[2] = kHalfHeight;
  light.colorIntensity[0] = 1.0f;
  light.colorIntensity[1] = 1.0f;
  light.colorIntensity[2] = 1.0f;
  light.colorIntensity[3] = 4.0f;
  light.flags[0] = WorldGILightFlag_Enabled;
  light.flags[1] = WorldGILightType_Planar;
  light.flags[3] = kGeneration;

  // Mirrors worldgi_planar_light_basis for a -Y normal: it picks the +X
  // reference when the normal is near vertical, giving tangent = (0,0,-1) and
  // bitangent = (1,0,0). halfWidth applies along the tangent, halfHeight along
  // the bitangent, so the extents are not simply X and Z.
  const std::array<float, 3> tangent{0.0f, 0.0f, -1.0f};
  const std::array<float, 3> bitangent{1.0f, 0.0f, 0.0f};
  const AreaLightSampleStats stats = measureAreaLightSampling(
      rhi, "SoftShadowPlanar", light, center, 0.0f, tangent, bitangent,
      kHalfWidth, kHalfHeight, false, 16u, 1.0f);

  assert(stats.activeCount > 0u);
  // A quad facing the receivers has no back-facing samples to waste.
  assert(stats.wastedCount * 10u < stats.activeCount);
  // Sampled points stay within the quad; the tolerance only absorbs the
  // reconstruction of the point from origin + direction * distance.
  assert(stats.maximumSurfaceError < 1.0e-2f);
  assert(stats.pointSpread > 0.05f);
  assert(stats.weightMean > 0.0f);
  assert(stats.weightMaximum <= stats.weightMean * 8.0f);
}

} // namespace

// Reproduces the sample's multi-frame-in-flight history plumbing: virtual
// read/write reservoir buffers, one physical instance per frame slot, cached
// per-slot command buffers whose overrides map read -> previous slot and
// write -> current slot, and per-frame settings updates through update().
// The temporal reservoir M count must grow across frames; if it stays at one
// the history chain is broken whenever more than one frame slot exists.
void testDIReservoirHistoryChainAcrossFrameSlots(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 1u;
  constexpr uint32_t height = 1u;
  constexpr uint32_t pixelCount = width * height;
  constexpr uint32_t slotCount = 3u;
  constexpr uint32_t frameCount = 7u;
  RenderGraph graph(&rhi, RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = slotCount});

  const std::vector<SparseVoxelSceneRayGPU> candidates{activeRay(0u)};
  const Buffer candidateBuffer = uploadBuffer(graph, "ChainTest.Candidates", candidates, BufferUsage::BufferUsage_Storage);
  const Buffer lightBuffer = uploadBuffer(graph, "ChainTest.Lights", std::vector<WorldGILightGPU>{directionalLight()}, BufferUsage::BufferUsage_Storage);
  const Buffer depthBuffer = uploadBuffer(graph, "ChainTest.Depth", std::vector<float>(pixelCount, 10.0f), BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(graph, "ChainTest.Readback", std::vector<SparseVoxelDIReservoirGPU>(pixelCount), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);

  const BufferUsage historyUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc;
  auto virtualBuffer = [&](const char *name, const uint64_t size)
  {
    return graph.createBuffer({.name = name, .size = size, .usage = historyUsage, .isVirtual = true});
  };
  const uint64_t raySize = pixelCount * sizeof(SparseVoxelSceneRayGPU);
  const uint64_t reservoirSize = pixelCount * sizeof(SparseVoxelDIReservoirGPU);
  const Buffer previousRaysVirtual = virtualBuffer("ChainTest.Rays.read", raySize);
  const Buffer raysVirtual = virtualBuffer("ChainTest.Rays.write", raySize);
  const Buffer previousReservoirsVirtual = virtualBuffer("ChainTest.Reservoirs.read", reservoirSize);
  const Buffer reservoirsVirtual = virtualBuffer("ChainTest.Reservoirs.write", reservoirSize);
  std::array<BufferId, slotCount> rayInstances{};
  std::array<BufferId, slotCount> reservoirInstances{};
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
  {
    rayInstances[slot] = rhi.createBuffer({.name = "ChainTest.Rays.frame" + std::to_string(slot), .size = raySize, .usage = historyUsage});
    reservoirInstances[slot] = rhi.createBuffer({.name = "ChainTest.Reservoirs.frame" + std::to_string(slot), .size = reservoirSize, .usage = historyUsage});
  }

  const Texture normalTexture = uploadFloatTexture(graph, "ChainTest.Normals", Format::Format_RGBA32Float, width, height, {0.0f, 1.0f, 0.0f, 1.0f});
  const Texture motionTexture = uploadFloatTexture(graph, "ChainTest.Motion", Format::Format_RG32Float, width, height, {0.0f, 0.0f});
  const Texture instanceTexture = uploadUintTexture(graph, "ChainTest.Instance", width, height, {7u});
  const Texture voxelTexture = uploadUintTexture(graph, "ChainTest.Voxel", width, height, {11u});
  const Texture reservoirReceiverTexture =
      reservoirSidebandTexture(graph, "ChainTest.ReservoirReceiver", width, height);
  const Texture reservoirCoreTexture =
      reservoirSidebandTexture(graph, "ChainTest.ReservoirCore", width, height);

  auto makeSettings = [&](const bool historyValid, const uint32_t frameIndex)
  {
    return svrRendering::SparseVoxelDIReservoirPass::Settings{
      pixelCount, 1u, kGeneration, historyValid, 1u, width, height,
      0.85f, 0.05f, width, height, 8u, frameIndex};
  };
  uint32_t passIndex = 0u;
  auto *reservoirPass = graph.registerPass<svrRendering::SparseVoxelDIReservoirPass>(
      "ChainTest.Resample",
      passIndex++,
      svrRendering::SparseVoxelDIReservoirPass::Buffers{
        candidateBuffer,
        previousRaysVirtual,
        previousReservoirsVirtual,
        raysVirtual,
        reservoirsVirtual,
        lightBuffer,
        depthBuffer,
        normalTexture,
        motionTexture,
        instanceTexture,
        voxelTexture,
        {},
        reservoirReceiverTexture,
        reservoirCoreTexture,
      },
      makeSettings(false, 0u));
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "ChainTest.Copy", passIndex++, reservoirsVirtual, 0u, reservoirSize, readback, 0u, reservoirSize);
  graph.compile();

  std::array<RenderGraph::CommandBufferHandle, slotCount> frameHandles{};
  RenderGraph::CommandBufferHandle initialHandle{};
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
  {
    const uint32_t previousSlot = (slot + slotCount - 1u) % slotCount;
    RenderGraph::Overrides overrides{};
    overrides.bufferOverrides.emplace(previousRaysVirtual.name, RenderGraph::BufferOverride{.bufferId = rayInstances[previousSlot]});
    overrides.bufferOverrides.emplace(raysVirtual.name, RenderGraph::BufferOverride{.bufferId = rayInstances[slot]});
    overrides.bufferOverrides.emplace(previousReservoirsVirtual.name, RenderGraph::BufferOverride{.bufferId = reservoirInstances[previousSlot]});
    overrides.bufferOverrides.emplace(reservoirsVirtual.name, RenderGraph::BufferOverride{.bufferId = reservoirInstances[slot]});
    frameHandles[slot] = graph.createCommandBuffer(overrides, slot, true);
    if (slot == 0u)
      initialHandle = graph.createCommandBuffer(overrides, slot, false);
  }

  // Pipeline frames the way the sample does: a slot is only waited on when
  // it is about to be reused, so consecutive frames genuinely overlap on the
  // GPU and the cross-slot history read must be ordered by graph barriers.
  std::array<RenderGraph::Frame, frameCount> frames{};
  for (uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
  {
    const uint32_t slot = frameIndex % slotCount;
    if (frameIndex >= slotCount)
      graph.waitFrame(frames[frameIndex - slotCount]);
    graph.setCurrentFrameIndex(slot);
    reservoirPass->update(makeSettings(frameIndex > 0u, frameIndex));
    frames[frameIndex].frameIndex = slot;
    const RenderGraph::Frame *previousFrame = frameIndex > 0u ? &frames[frameIndex - 1u] : nullptr;
    graph.run(frames[frameIndex], frameIndex == 0u ? initialHandle : frameHandles[slot], previousFrame);
  }
  for (uint32_t frameIndex = frameCount - slotCount; frameIndex < frameCount; ++frameIndex)
    graph.waitFrame(frames[frameIndex]);
  uint32_t finalHistoryCount = 0u;
  graph.bufferRead(
      readback,
      0u,
      sizeof(SparseVoxelDIReservoirGPU),
      [&](const void *data)
      {
        SparseVoxelDIReservoirGPU reservoir{};
        std::memcpy(&reservoir, data, sizeof(reservoir));
        finalHistoryCount = reservoir.identity[2];
      });

  std::cout << "DI reservoir history chain final M after " << frameCount
            << " pipelined frames: " << finalHistoryCount << "\n";
  // Frame N carries N accumulated samples plus the fresh candidate; a broken
  // cross-slot chain shows up as a value stuck at one or two.
  assert(finalHistoryCount == frameCount);

  delete copyPass;
  delete reservoirPass;
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
  {
    rhi.deleteBuffer(rayInstances[slot]);
    rhi.deleteBuffer(reservoirInstances[slot]);
  }
}

// Mirrors the hit-temporal history plumbing across three frame slots. The
// visibility mean count must grow by one per frame; a per-slot chain break
// shows up as counts resetting with the frame-slot period.
void testHitTemporalHistoryChainAcrossFrameSlots(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 1u;
  constexpr uint32_t height = 1u;
  constexpr uint32_t rayCount = width * height;
  constexpr uint32_t slotCount = 3u;
  constexpr uint32_t frameCount = 7u;
  RenderGraph graph(&rhi, RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = slotCount});

  std::vector<SparseVoxelSceneRayGPU> rays{activeRay(0u)};
  std::vector<SparseVoxelSceneRayHitGPU> hits(rayCount);
  hits[0].status = static_cast<uint32_t>(SparseVoxelSceneRayStatus::EscapedScene);
  hits[0].identity = {rays[0].seedFlags[0], 0u, static_cast<uint32_t>(SparseVoxelSceneRayBackend::Tree64), kGeneration};
  const Buffer hitBuffer = uploadBuffer(graph, "HitChain.Hits", hits, BufferUsage::BufferUsage_Storage);
  const Buffer rayBuffer = uploadBuffer(graph, "HitChain.Rays", rays, BufferUsage::BufferUsage_Storage);
  const Buffer stableHitBuffer = uploadBuffer(graph, "HitChain.StableHits", std::vector<SparseVoxelSceneRayHitGPU>(rayCount), BufferUsage::BufferUsage_Storage);
  const Buffer visibilityBuffer = uploadBuffer(graph, "HitChain.Visibility", std::vector<float>(rayCount), BufferUsage::BufferUsage_Storage);
  const Buffer readback = uploadBuffer(graph, "HitChain.Readback", std::vector<SparseVoxelTemporalSceneRayHitGPU>(rayCount), BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  const Texture motionTexture = uploadFloatTexture(graph, "HitChain.Motion", Format::Format_RG32Float, width, height, {0.0f, 0.0f});

  const BufferUsage historyUsage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc;
  const uint64_t historySize = rayCount * sizeof(SparseVoxelTemporalSceneRayHitGPU);
  const Buffer previousHistoryVirtual = graph.createBuffer({.name = "HitChain.History.read", .size = historySize, .usage = historyUsage, .isVirtual = true});
  const Buffer historyVirtual = graph.createBuffer({.name = "HitChain.History.write", .size = historySize, .usage = historyUsage, .isVirtual = true});
  std::array<BufferId, slotCount> historyInstances{};
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
    historyInstances[slot] = rhi.createBuffer({.name = "HitChain.History.frame" + std::to_string(slot), .size = historySize, .usage = historyUsage});

  auto makeSettings = [&](const bool historyValid)
  {
    return svrRendering::SparseVoxelSceneHitTemporalPass::Settings{
      rayCount, 32u, kGeneration, historyValid, 0.25f, 0.05f, width, height};
  };
  uint32_t passIndex = 0u;
  auto *temporalPass = graph.registerPass<svrRendering::SparseVoxelSceneHitTemporalPass>(
      "HitChain.Accumulate",
      passIndex++,
      svrRendering::SparseVoxelSceneHitTemporalPass::Buffers{
        hitBuffer,
        rayBuffer,
        previousHistoryVirtual,
        historyVirtual,
        stableHitBuffer,
        visibilityBuffer,
        motionTexture,
      },
      makeSettings(false));
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "HitChain.Copy", passIndex++, historyVirtual, 0u, historySize, readback, 0u, historySize);
  graph.compile();

  std::array<RenderGraph::CommandBufferHandle, slotCount> frameHandles{};
  RenderGraph::CommandBufferHandle initialHandle{};
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
  {
    const uint32_t previousSlot = (slot + slotCount - 1u) % slotCount;
    RenderGraph::Overrides overrides{};
    overrides.bufferOverrides.emplace(previousHistoryVirtual.name, RenderGraph::BufferOverride{.bufferId = historyInstances[previousSlot]});
    overrides.bufferOverrides.emplace(historyVirtual.name, RenderGraph::BufferOverride{.bufferId = historyInstances[slot]});
    frameHandles[slot] = graph.createCommandBuffer(overrides, slot, true);
    if (slot == 0u)
      initialHandle = graph.createCommandBuffer(overrides, slot, false);
  }

  std::array<RenderGraph::Frame, frameCount> frames{};
  std::array<uint32_t, frameCount> sampleCounts{};
  for (uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
  {
    const uint32_t slot = frameIndex % slotCount;
    if (frameIndex >= slotCount)
      graph.waitFrame(frames[frameIndex - slotCount]);
    graph.setCurrentFrameIndex(slot);
    temporalPass->update(makeSettings(frameIndex > 0u));
    frames[frameIndex].frameIndex = slot;
    const RenderGraph::Frame *previousFrame = frameIndex > 0u ? &frames[frameIndex - 1u] : nullptr;
    graph.run(frames[frameIndex], frameIndex == 0u ? initialHandle : frameHandles[slot], previousFrame);
    graph.waitFrame(frames[frameIndex]);
    graph.bufferRead(
        readback,
        0u,
        sizeof(SparseVoxelTemporalSceneRayHitGPU),
        [&](const void *data)
        {
          SparseVoxelTemporalSceneRayHitGPU history{};
          std::memcpy(&history, data, sizeof(history));
          sampleCounts[frameIndex] = history.state[0];
        });
  }
  std::cout << "Hit temporal history chain counts:";
  for (const uint32_t count : sampleCounts)
    std::cout << " " << count;
  std::cout << "\n";
  for (uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
    assert(sampleCounts[frameIndex] == frameIndex + 1u);

  delete copyPass;
  delete temporalPass;
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
    rhi.deleteBuffer(historyInstances[slot]);
}

// A frameLocal buffer written from the CPU every frame (the sample's light
// list) must expose that frame's contents to that frame's GPU work for every
// frame slot. If the write resolves to the wrong slot instance, frames on
// the other slots render with stale data and per-frame values alternate.
void testFrameLocalBufferWritePerSlot(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t slotCount = 2u;
  constexpr uint32_t frameCount = 5u;
  RenderGraph graph(&rhi, RenderGraph::Settings{.maxTimers = 64u, .maxFramesInFlight = slotCount});
  const Buffer source = graph.createBuffer({
    .name = "SlotWrite.Source",
    .size = sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
    .frameLocal = true,
  });
  const Buffer readback = graph.createBuffer({
    .name = "SlotWrite.Readback",
    .size = sizeof(uint32_t),
    .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst,
  });
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SlotWrite.Copy", 0u, source, 0u, sizeof(uint32_t), readback, 0u, sizeof(uint32_t));
  graph.compile();
  std::array<RenderGraph::CommandBufferHandle, slotCount> handles{};
  for (uint32_t slot = 0u; slot < slotCount; ++slot)
    handles[slot] = graph.createCommandBuffer(RenderGraph::Overrides{}, slot, slot != 0u);

  std::array<uint32_t, frameCount> observed{};
  std::array<RenderGraph::Frame, frameCount> frames{};
  for (uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
  {
    const uint32_t slot = frameIndex % slotCount;
    graph.setCurrentFrameIndex(slot);
    const uint32_t value = 100u + frameIndex;
    graph.bufferWrite(source, 0u, sizeof(value), const_cast<uint32_t *>(&value));
    frames[frameIndex].frameIndex = slot;
    const RenderGraph::Frame *previousFrame = frameIndex > 0u ? &frames[frameIndex - 1u] : nullptr;
    graph.run(frames[frameIndex], handles[slot], previousFrame);
    graph.waitFrame(frames[frameIndex]);
    graph.bufferRead(
        readback,
        0u,
        sizeof(uint32_t),
        [&](const void *data)
        {
          std::memcpy(&observed[frameIndex], data, sizeof(uint32_t));
        });
  }
  std::cout << "frameLocal buffer per-frame reads:";
  for (const uint32_t value : observed)
    std::cout << " " << value;
  std::cout << "\n";
  for (uint32_t frameIndex = 0u; frameIndex < frameCount; ++frameIndex)
    assert(observed[frameIndex] == 100u + frameIndex);
  delete copyPass;
}

void testDirectLightingResolveUsesCurrentReceiverVisibility(
    ::rendering::backend::vulkan::VulkanRHI &rhi)
{
  constexpr uint32_t width = 3u;
  constexpr uint32_t height = 1u;
  constexpr uint32_t pixelCount = width * height;
  RenderGraph graph(&rhi);

  const Texture identity = uploadUintTexture(
      graph, "DirectResolve.Identity", width, height,
      std::vector<uint32_t>(pixelCount, 1u));
  const Texture rg = uploadFloatTexture(
      graph, "DirectResolve.RG", Format::Format_RG32Float, width, height,
      std::vector<float>(pixelCount * 2u, 0.0f));
  const Texture rgba = uploadFloatTexture(
      graph, "DirectResolve.RGBA", Format::Format_RGBA32Float, width, height,
      std::vector<float>(pixelCount * 4u, 0.0f));
  std::vector<float> normals(pixelCount * 4u, 0.0f);
  for (uint32_t pixel = 0u; pixel < pixelCount; ++pixel)
  {
    normals[pixel * 4u + 1u] = 1.0f;
    normals[pixel * 4u + 3u] = 1.0f;
  }
  const Texture normal = uploadFloatTexture(
      graph, "DirectResolve.Normal", Format::Format_RGBA32Float, width,
      height, normals);
  const Texture depthTexture = uploadFloatTexture(
      graph, "DirectResolve.DepthTexture", Format::Format_Depth32Float,
      width, height, std::vector<float>(pixelCount, 0.5f));
  const svrRendering::SparseVoxelGBufferPass::FrameTarget target{
    identity, identity, identity, rg, rgba, rgba, rgba, normal, normal, rg,
    depthTexture, identity,
  };
  const Buffer coverage = uploadBuffer(
      graph, "DirectResolve.Coverage",
      std::vector<uint32_t>(pixelCount, 1u),
      BufferUsage::BufferUsage_Storage);
  const Buffer linearDepth = uploadBuffer(
      graph, "DirectResolve.LinearDepth",
      std::vector<float>(pixelCount, 1.0f),
      BufferUsage::BufferUsage_Storage);

  std::vector<SparseVoxelSceneRayGPU> rays(pixelCount);
  rays[0] = activeRay(0u);
  rays[2] = activeRay(2u);
  const Buffer rayBuffer = uploadBuffer(
      graph, "DirectResolve.Rays", rays,
      BufferUsage::BufferUsage_Storage);
  std::vector<SparseVoxelSceneRayHitGPU> hits(pixelCount);
  hits[0].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::Hit);
  hits[2].status =
      static_cast<uint32_t>(SparseVoxelSceneRayStatus::MissTMax);
  const Buffer hitBuffer = uploadBuffer(
      graph, "DirectResolve.Hits", hits,
      BufferUsage::BufferUsage_Storage);
  std::vector<SparseVoxelDIReservoirGPU> reservoirs(pixelCount);
  reservoirs[0] = previousReservoir({0.0f, 1.0f, 0.0f}, 1.0f, 1u, 1u);
  reservoirs[2] = reservoirs[0];
  const Buffer reservoirBuffer = uploadBuffer(
      graph, "DirectResolve.Reservoirs", reservoirs,
      BufferUsage::BufferUsage_Storage);
  std::vector<WorldGILightGPU> lights(5u);
  lights[0] = directionalLight();
  const Buffer lightBuffer = uploadBuffer(
      graph, "DirectResolve.Lights", lights,
      BufferUsage::BufferUsage_Storage);

  const Texture output = graph.createTexture({
    .name = "DirectResolve.Output",
    .format = Format::Format_RGBA16Float,
    .usage = ImageUsage::ImageUsage_Storage | ImageUsage::ImageUsage_Sampled,
    .width = width,
    .height = height,
    .mipLevels = 1u,
  });
  const Buffer pixels = uploadBuffer(
      graph, "DirectResolve.Pixels",
      std::vector<float>(pixelCount * 4u),
      BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_CopySrc);
  const Buffer readback = uploadBuffer(
      graph, "DirectResolve.Readback",
      std::vector<float>(pixelCount * 4u),
      BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst);
  SparseVoxelRasterUniformsGPU raster{};
  raster.cameraToWorld = identityMatrix();
  raster.worldToClip = identityMatrix();
  raster.previousWorldToClip = identityMatrix();
  raster.focalPrincipal = {1.0f, 1.0f, 0.0f, 0.0f};
  auto *presentPass =
      graph.registerPass<svrRendering::SparseVoxelDebugPresentPass>(
          "DirectResolve.Present", 0u, target, coverage, linearDepth, output,
          SparseVoxelDebugPresentUniformsGPU{
            width,
            height,
            static_cast<uint32_t>(SparseVoxelDebugMode::SceneRayShading),
            1.0f,
            width,
            height,
            width | (1u << 24u),
            height,
          },
          Buffer{}, hitBuffer, Buffer{}, rayBuffer, lightBuffer,
          reservoirBuffer, Buffer{}, hitBuffer, Buffer{}, Texture{}, raster);
  auto *readPass =
      graph.registerPass<svrRendering::SparseVoxelColorReadbackPass>(
          "DirectResolve.Read", 1u, output, pixels, width, height);
  auto *copyPass = graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "DirectResolve.Copy", 2u, pixels, 0u,
      pixelCount * 4u * sizeof(float), readback, 0u,
      pixelCount * 4u * sizeof(float));
  runGraph(graph);

  std::array<float, pixelCount * 4u> actual{};
  graph.bufferRead(
      readback, 0u, sizeof(actual),
      [&](const void *data)
      { std::memcpy(actual.data(), data, sizeof(actual)); });
  // The first receiver is occluded. This specifically runs without the
  // optional stableSceneVisibility buffer, whose fallback value is 1.0.
  assert(std::abs(actual[0u]) < 0.01f);
  assert(std::abs(actual[1u]) < 0.01f);
  assert(std::abs(actual[2u]) < 0.01f);
  assert(std::abs(actual[3u] - 1.0f) < 0.02f);
  // The middle receiver was not traced. It must remain unresolved instead of
  // borrowing either neighbouring receiver's binary visibility result.
  assert(std::abs(actual[4u]) < 0.01f);
  assert(std::abs(actual[7u] + 2.0f) < 0.02f);
  // The final receiver is an unoccluded control and must still receive direct
  // light, proving the zero above is visibility rather than a dead estimator.
  assert(actual[8u] > 0.25f);
  assert(std::abs(actual[11u] - 1.0f) < 0.02f);

  delete copyPass;
  delete readPass;
  delete presentPass;
}

int main(const int argc, const char *const *argv)
{
  os::Logger::start();
  ::rendering::DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  if (argc > 1 &&
      std::string(argv[1]) == "--light-field-visibility-locality-only")
  {
    testLightFieldKeepsResolvedVisibilityReceiverLocal(rhi);
    std::cout << "Sparse voxel light-field visibility locality test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--edge-schedule-only")
  {
    testViewportEdgeUsesFullRateSchedule(rhi);
    std::cout << "Sparse voxel full-rate viewport edge test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 && std::string(argv[1]) == "--screen-trace-only")
  {
    testDisabledScreenTraceRoutesToWorld(rhi);
    testScreenTraceLeavesParallelNearFieldOccluderForTree64(rhi);
    testScreenTracePassesFlatSurface(rhi);
    testScreenTraceRejectsReceiverVoxel(rhi);
    testScreenTraceRejectsCoplanarVoxelEdge(rhi);
    std::cout << "Sparse voxel screen-trace tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--screen-world-merge-only")
  {
    testScreenTraceCompactionPreservesWorldFallback(rhi);
    testScreenAndWorldHitMergeContracts(rhi);
    testSceneRayStatisticsCountsMergedScreenAndWorldContracts(rhi);
    std::cout << "Sparse voxel screen/Tree64 merge tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--world-ray-compaction-only")
  {
    testScreenTraceCompactionPreservesWorldFallback(rhi);
    std::cout << "Sparse voxel world-ray receiver rejection test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--terrain-ray-origin-only")
  {
    testTerrainRayOriginIgnoresPageLODAndBias(rhi);
    std::cout << "Sparse voxel terrain ray-origin/shadow-span test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--restir-history-only")
  {
    testCombinedSceneRayWorklistReset(rhi);
    testMotionReprojectionAndTemporalValidation(rhi);
    testSpatialReceiverValidation(rhi);
    std::cout << "Sparse voxel ReSTIR history tests passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--hit-temporal-only")
  {
    testHitVisibilityMotionReprojection(rhi);
    testHitVisibilityKeepsViewportEdgeGuardBand(rhi);
    std::cout << "Sparse voxel hit-temporal reprojection test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--svgf-spatial-only")
  {
    testSvgfReconstructsUnresolvedSparseWindow(rhi);
    testSvgfFiltersCoplanarLightingOutlier(rhi);
    std::cout << "Sparse voxel SVGF spatial reconstruction test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--lighting-accumulation-only")
  {
    testLightingAccumulationIsIndependentOfSvgf(rhi);
    testStationaryLightingIgnoresSpuriousMotion(rhi);
    testRotatingCameraPrefersGeometricReprojection(rhi);
    testLightingReprojectionRecoversNearbyNoisyLightfieldHistory(rhi);
    testLightingAccumulationFillsUnresolvedCurrentSparseWindow(rhi);
    std::cout << "Sparse voxel temporal lighting accumulation test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  if (argc > 1 &&
      std::string(argv[1]) == "--direct-lighting-resolve-only")
  {
    testDirectLightingResolveUsesCurrentReceiverVisibility(rhi);
    std::cout << "Sparse voxel direct-lighting resolve test passed\n";
    os::Logger::shutdown();
    return 0;
  }
  testCombinedSceneRayWorklistReset(rhi);
  testFrameLocalBufferWritePerSlot(rhi);
  testDirectLightingResolveUsesCurrentReceiverVisibility(rhi);
  testTerrainRayOriginIgnoresPageLODAndBias(rhi);
  testLightFieldKeepsResolvedVisibilityReceiverLocal(rhi);
  testReachabilityCacheDoesNotBiasReSTIR(rhi);
  testDIReservoirHistoryChainAcrossFrameSlots(rhi);
  testHitTemporalHistoryChainAcrossFrameSlots(rhi);
  testMotionReprojectionAndTemporalValidation(rhi);
  testMultipleReservoirLanesUseLargerCandidatePool(rhi);
	  testSpatialReceiverValidation(rhi);
	  testHitVisibilityMotionReprojection(rhi);
	  testHitVisibilityKeepsViewportEdgeGuardBand(rhi);
	  testSkyRayGeneration(rhi);
  testViewportEdgeUsesFullRateSchedule(rhi);
  testDisabledScreenTraceRoutesToWorld(rhi);
  testScreenTraceLeavesParallelNearFieldOccluderForTree64(rhi);
  testScreenTracePassesFlatSurface(rhi);
  testScreenTraceRejectsReceiverVoxel(rhi);
  testScreenTraceRejectsCoplanarVoxelEdge(rhi);
  testScreenTraceCompactionPreservesWorldFallback(rhi);
  testScreenAndWorldHitMergeContracts(rhi);
  testSceneRayStatisticsCountsMergedScreenAndWorldContracts(rhi);
  testSvgfAccumulatesAlternatingSkySamples(rhi);
	  testLightingAccumulationIsIndependentOfSvgf(rhi);
	  testStationaryLightingIgnoresSpuriousMotion(rhi);
	  testRotatingCameraPrefersGeometricReprojection(rhi);
	  testLightingReprojectionRecoversNearbyNoisyLightfieldHistory(rhi);
  testLightingAccumulationFillsUnresolvedCurrentSparseWindow(rhi);
	  testSvgfReconstructsUnresolvedSparseWindow(rhi);
  testSvgfFiltersCoplanarLightingOutlier(rhi);
  testTsrReprojectsStochasticLightingWithoutCurrentFrameClipping(rhi);
  testSphereAreaLightProducesBoundedSoftShadowSamples(rhi);
  testPlanarAreaLightProducesBoundedSoftShadowSamples(rhi);
  std::cout << "Sparse voxel scene-ray GPU validation tests passed\n";
  os::Logger::shutdown();
  return 0;
}
