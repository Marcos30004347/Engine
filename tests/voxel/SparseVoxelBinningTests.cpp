#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/rendering/SparseVoxelBinningPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDebugPresentPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelGBufferPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelGBufferReadbackPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDepthPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelDepthReadbackPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelResolvePass.hpp"

#include "reference/SparseVoxelRasterReference.hpp"

#include <algorithm>
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
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#undef assert
#define assert(condition)                                                                                                                                        \
  do                                                                                                                                                             \
  {                                                                                                                                                              \
    if (!(condition))                                                                                                                                             \
    {                                                                                                                                                            \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);                                                            \
      std::abort();                                                                                                                                              \
    }                                                                                                                                                            \
  } while (false)

namespace
{
using namespace voxel::svraster;
namespace ref = voxel::svraster::reference;
namespace svrRendering = voxel::svraster::rendering;
using ::rendering::Buffer;
using ::rendering::BufferInfo;
using ::rendering::BufferUsage;
using ::rendering::RenderGraph;
using ::rendering::Texture;

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f};
}

void setViewport(
    SparseVoxelRasterUniformsGPU &uniforms,
    const uint32_t width,
    const uint32_t height)
{
  const uint32_t tilesX =
      (width + kSparseVoxelTileSize - 1u) / kSparseVoxelTileSize;
  const uint32_t tilesY =
      (height + kSparseVoxelTileSize - 1u) / kSparseVoxelTileSize;
  uniforms.viewportTiles = {width, height, tilesX, tilesY};
  uniforms.tileRangeCapacity = tilesX * tilesY;
}

ref::ProjectionFixture makeProjectionFixture()
{
  ref::ProjectionFixture fixture{};
  fixture.uniforms.worldToCamera = identityMatrix();
  fixture.uniforms.cameraToWorld = identityMatrix();
  fixture.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  fixture.uniforms.focalPrincipal = {32.0f, 32.0f, 32.0f, 32.0f};
  setViewport(fixture.uniforms, 64u, 64u);
  fixture.uniforms.visibleVoxelCapacity = 7u;
  fixture.uniforms.projectedVoxelCapacity = 7u;
  fixture.uniforms.candidateCapacity = 4096u;
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  instance.previousModelToWorld = identityMatrix();
  fixture.instances.push_back(instance);
  fixture.centersLengths = {
    {{{0.0f, 0.0f, 2.0f}}, 0.5f},
    {{{1.8f, 0.0f, 2.0f}}, 0.5f},
    {{{0.0f, 0.0f, -0.5f}}, 0.25f},
    {{{0.0f, 0.0f, 0.12f}}, 0.20f},
    {{{-1.8f, -1.8f, 2.0f}}, 0.5f},
    // The center is inside the near-distance sphere, but the box crosses the
    // actual camera-space near plane and must survive conservative clipping.
    {{{0.0f, 0.0f, 0.08f}}, 0.08f},
    {{{0.1f, 0.0f, 0.1f}}, 0.04f},
  };
  for (uint32_t index = 0u; index < fixture.centersLengths.size(); ++index)
  {
    const uint64_t path = encodeOctreePath(index, index ^ 3u, index ^ 5u, 7u);
    fixture.pathsLevels.push_back({static_cast<uint32_t>(path), static_cast<uint32_t>(path >> 32u), 7u, 0u});
  }
  fixture.visibleInstances.push_back({0u, 0u, static_cast<uint32_t>(fixture.centersLengths.size()), 0u});
  return fixture;
}

template <typename T> bool equalBytes(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

void assertCandidatesEqual(const std::vector<TileCandidateGPU> &actual, const std::vector<TileCandidateGPU> &expected)
{
  assert(actual.size() == expected.size());
  for (size_t index = 0u; index < actual.size(); ++index)
  {
    if (std::memcmp(&actual[index], &expected[index], sizeof(TileCandidateGPU)) != 0)
    {
      std::fprintf(
          stderr,
          "first candidate mismatch at %zu: actual key=%08x:%08x ordinal=%u expected key=%08x:%08x ordinal=%u\n",
          index,
          actual[index].keyHi,
          actual[index].keyLo,
          actual[index].reserved0,
          expected[index].keyHi,
          expected[index].keyLo,
          expected[index].reserved0);
      assert(false);
    }
  }
}

bool nearFloat(const float actual, const float expected)
{
  const float tolerance = 2.0e-5f * std::max(1.0f, std::max(std::abs(actual), std::abs(expected)));
  return std::abs(actual - expected) <= tolerance;
}

bool projectedEqual(
    const std::vector<ProjectedVoxelGPU> &actual,
    const std::vector<ProjectedVoxelGPU> &expected)
{
  if (actual.size() != expected.size())
    return false;
  for (size_t index = 0u; index < actual.size(); ++index)
  {
    ProjectedVoxelGPU actualComparable = actual[index];
    ProjectedVoxelGPU expectedComparable = expected[index];
    float actualMinimumDistance = 0.0f;
    float expectedMinimumDistance = 0.0f;
    std::memcpy(
        &actualMinimumDistance, &actualComparable.reserved0,
        sizeof(actualMinimumDistance));
    std::memcpy(
        &expectedMinimumDistance, &expectedComparable.reserved0,
        sizeof(expectedMinimumDistance));
    actualComparable.reserved0 = 0u;
    expectedComparable.reserved0 = 0u;
    if (std::memcmp(
            &actualComparable, &expectedComparable,
            sizeof(ProjectedVoxelGPU)) != 0 ||
        !nearFloat(actualMinimumDistance, expectedMinimumDistance))
      return false;
  }
  return true;
}

struct GpuInput
{
  SparseVoxelRasterUniformsGPU uniforms{};
  SparseVoxelFrameCountersGPU counters{};
  std::vector<VisibleInstanceGPU> visibleInstances{{}};
  std::vector<SparseVoxelInstanceGPU> instances{{}};
  std::vector<SparseVoxelCenterLengthGPU> centersLengths{{}};
  std::vector<SparseVoxelPathLevelGPU> pathsLevels{{}};
  std::vector<SparseVoxelMaterialGPU> materials{{}};
  std::vector<ProjectedVoxelGPU> projected;
  std::vector<TileCandidateGPU> candidates;
  std::vector<SparseVoxelTileRangeGPU> inputRanges;
  std::vector<ResolvedVoxelSurfaceGPU> inputResolved;
  std::vector<uint32_t> inputCoverage;
  std::vector<float> inputLinearDepth;
  std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> indirect{};
  uint32_t candidateCount = 0u;
  bool preprocess = false;
  bool scan = false;
  bool emit = false;
  bool sort = false;
  uint32_t sortingBits = 64u;
  bool ranges = false;
  bool resolve = false;
  bool gbuffer = false;
  bool depthOnly = false;
  ::rendering::LoadOp depthLoadOp = ::rendering::LoadOp::LoadOp_Clear;
  ::rendering::ComparisonOp depthComparison = ::rendering::ComparisonOp::ComparisonOp_Always;
  bool debugPresent = false;
  SparseVoxelDebugMode debugMode = SparseVoxelDebugMode::BaseColor;
  float debugLinearDepthScale = 1.0f;
};

struct GpuOutput
{
  SparseVoxelFrameCountersGPU counters{};
  std::vector<ProjectedVoxelGPU> projected;
  std::vector<uint32_t> offsets;
  std::vector<TileCandidateGPU> candidates;
  std::vector<SparseVoxelTileRangeGPU> ranges;
  std::vector<ResolvedVoxelSurfaceGPU> resolved;
  std::vector<uint32_t> coverage;
  std::vector<float> linearDepth;
  std::vector<VoxelMaterialSampleAddressGPU> materialSampleAddresses;
  std::vector<SparseVoxelGBufferPixelGPU> gbuffer;
  std::vector<float> depthOnly;
  std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> indirect{};
  uint32_t candidateCount = 0u;
};

void assertResolveEqual(const GpuOutput &actual, const ref::ResolveResult &expected)
{
  assert(actual.resolved.size() == expected.surfaces.size());
  assert(actual.coverage == expected.coverage);
  assert(actual.linearDepth.size() == expected.linearDepth.size());
  assert(actual.materialSampleAddresses.size() == expected.surfaces.size());
  for (const auto &address : actual.materialSampleAddresses)
    assert(address == VoxelMaterialSampleAddressGPU{});
  for (size_t index = 0u; index < expected.surfaces.size(); ++index)
  {
    const ResolvedVoxelSurfaceGPU &gpu = actual.resolved[index];
    const ResolvedVoxelSurfaceGPU &cpu = expected.surfaces[index];
    assert(gpu.instanceIndex == cpu.instanceIndex);
    assert(gpu.voxelIndex == cpu.voxelIndex);
    assert(gpu.packedLocalFace == cpu.packedLocalFace);
    assert(gpu.hitFlags == cpu.hitFlags);
    assert(gpu.reserved0 == 0u && gpu.reserved1 == 0u && gpu.reserved2 == 0u);
    assert(nearFloat(gpu.surfaceT, cpu.surfaceT));
    assert(nearFloat(actual.linearDepth[index], expected.linearDepth[index]));
    assert(std::isfinite(gpu.surfaceT));
    assert(std::isfinite(actual.linearDepth[index]));
  }
}

void assertGBufferEqual(const std::vector<SparseVoxelGBufferPixelGPU> &actual, const std::vector<SparseVoxelGBufferPixelGPU> &expected)
{
  assert(actual.size() == expected.size());
  for (size_t pixel = 0u; pixel < expected.size(); ++pixel)
  {
    assert(actual[pixel].ids == expected[pixel].ids);
    const float *gpu = actual[pixel].materialUv.data();
    const float *cpu = expected[pixel].materialUv.data();
    constexpr size_t floatCount = 7u * 4u;
    for (size_t component = 0u; component < floatCount; ++component)
    {
      assert(std::isfinite(gpu[component]));
      assert(std::isfinite(cpu[component]));
      const float tolerance = 2.0e-3f * std::max(1.0f, std::max(std::abs(gpu[component]), std::abs(cpu[component])));
      if (std::abs(gpu[component] - cpu[component]) > tolerance)
      {
        std::fprintf(stderr, "G-buffer mismatch pixel=%zu component=%zu gpu=%g cpu=%g\n", pixel, component, gpu[component], cpu[component]);
        assert(false);
      }
    }
  }
}

std::array<float, 3> debugIdColor(const uint32_t value)
{
  uint32_t hash = value + 0x9e3779b9u;
  hash = (hash ^ (hash >> 16u)) * 0x85ebca6bu;
  hash = (hash ^ (hash >> 13u)) * 0xc2b2ae35u;
  hash ^= hash >> 16u;
  return {
    static_cast<float>(hash & 0xffu) / 255.0f,
    static_cast<float>((hash >> 8u) & 0xffu) / 255.0f,
    static_cast<float>((hash >> 16u) & 0xffu) / 255.0f,
  };
}

std::array<float, 4> expectedDebugColor(
    const SparseVoxelGBufferPixelGPU &pixel,
    const SparseVoxelDebugMode mode,
    const uint32_t coverage,
    const float linearDepth,
    const float linearDepthScale)
{
  if (coverage == 0u)
    return {0.0f, 0.0f, 0.0f, 1.0f};
  switch (mode)
  {
  case SparseVoxelDebugMode::BaseColor:
    return pixel.baseColor;
  case SparseVoxelDebugMode::GeometryNormal:
    return {pixel.geometryNormal[0] * 0.5f + 0.5f, pixel.geometryNormal[1] * 0.5f + 0.5f, pixel.geometryNormal[2] * 0.5f + 0.5f, 1.0f};
  case SparseVoxelDebugMode::ShadingNormal:
    return {pixel.normal[0] * 0.5f + 0.5f, pixel.normal[1] * 0.5f + 0.5f, pixel.normal[2] * 0.5f + 0.5f, 1.0f};
  case SparseVoxelDebugMode::Roughness:
    return {pixel.materialProperties[0], pixel.materialProperties[0], pixel.materialProperties[0], 1.0f};
  case SparseVoxelDebugMode::Metallic:
    return {pixel.materialProperties[1], pixel.materialProperties[1], pixel.materialProperties[1], 1.0f};
  case SparseVoxelDebugMode::Emissive:
    return {
      pixel.emissive[0] / (1.0f + pixel.emissive[0]),
      pixel.emissive[1] / (1.0f + pixel.emissive[1]),
      pixel.emissive[2] / (1.0f + pixel.emissive[2]),
      1.0f,
    };
  case SparseVoxelDebugMode::MotionVector:
    return {pixel.motionDepth[0] + 0.5f, pixel.motionDepth[1] + 0.5f, 0.5f, 1.0f};
  case SparseVoxelDebugMode::HardwareDepth:
    return {pixel.motionDepth[2], pixel.motionDepth[2], pixel.motionDepth[2], 1.0f};
  case SparseVoxelDebugMode::LinearDepth:
  {
    const float value = std::clamp(linearDepth * linearDepthScale, 0.0f, 1.0f);
    return {value, value, value, 1.0f};
  }
  case SparseVoxelDebugMode::Coverage:
    return {1.0f, 1.0f, 1.0f, 1.0f};
  case SparseVoxelDebugMode::InstanceId:
  {
    const auto color = debugIdColor(pixel.ids[0]);
    return {color[0], color[1], color[2], 1.0f};
  }
  case SparseVoxelDebugMode::VoxelId:
  {
    const auto color = debugIdColor(pixel.ids[1]);
    return {color[0], color[1], color[2], 1.0f};
  }
  case SparseVoxelDebugMode::OccupancyShadow:
    return {1.0f, 1.0f, 1.0f, 1.0f};
  case SparseVoxelDebugMode::OccupancyHitCell:
    return {0.0f, 0.0f, 0.0f, 1.0f};
  }
  return {};
}

template <typename T> void writeVector(RenderGraph &graph, const Buffer buffer, const std::vector<T> &values)
{
  if (!values.empty())
    graph.bufferWrite(buffer, 0u, values.size() * sizeof(T), const_cast<T *>(values.data()));
}

template <typename T> void readVector(RenderGraph &graph, const Buffer buffer, std::vector<T> &values)
{
  if (!values.empty())
  {
    graph.bufferRead(
        buffer,
        0u,
        values.size() * sizeof(T),
        [&](const void *data)
        {
          std::memcpy(values.data(), data, values.size() * sizeof(T));
        });
  }
}

GpuOutput runGpu(::rendering::backend::vulkan::VulkanRHI &rhi, GpuInput input)
{
  const uint32_t visibleCapacity = std::max(1u, input.uniforms.visibleVoxelCapacity);
  const uint32_t projectedCapacity = std::max(1u, input.uniforms.projectedVoxelCapacity);
  const uint32_t candidateCapacity = std::max(1u, input.uniforms.candidateCapacity);
  const uint32_t rangeCapacity = std::max(1u, input.uniforms.tileRangeCapacity);
  const uint32_t pixelCapacity = std::max(1u, input.uniforms.viewportTiles[0] * input.uniforms.viewportTiles[1]);
  const uint32_t scanBlocks = std::max(1u, (visibleCapacity + 255u) / 256u);
  input.projected.resize(projectedCapacity);
  input.candidates.resize(candidateCapacity);
  input.inputRanges.resize(rangeCapacity);
  input.visibleInstances.resize(std::max<size_t>(1u, input.visibleInstances.size()));
  input.instances.resize(std::max<size_t>(1u, input.instances.size()));
  input.centersLengths.resize(visibleCapacity);
  input.pathsLevels.resize(visibleCapacity);
  input.materials.resize(std::max<size_t>(1u, input.materials.size()));
  input.inputResolved.resize(pixelCapacity);
  input.inputCoverage.resize(pixelCapacity);
  input.inputLinearDepth.resize(pixelCapacity);
  std::vector<VisibleVoxelGPU> visibleVoxels;
  for (const VisibleInstanceGPU &record : input.visibleInstances)
  {
    for (uint32_t localIndex = 0u; localIndex < record.visibleVoxelCount; ++localIndex)
      visibleVoxels.push_back({record.instanceIndex, record.firstVisibleVoxel + localIndex, 0u, 0u});
  }
  visibleVoxels.resize(std::max<size_t>(visibleCapacity, visibleVoxels.size()));

  RenderGraph graph(&rhi);
  auto storage = [&](const std::string &name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer(
        {.name = name,
         .size = size,
         .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc | (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None)});
  };
  auto readback = [&](const std::string &name, const uint64_t size)
  {
    return graph.createBuffer({.name = name, .size = size, .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  };

  const Buffer counters = storage("svr.gpu.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer visible = storage("svr.gpu.visible", input.visibleInstances.size() * sizeof(VisibleInstanceGPU));
  const Buffer instances = storage("svr.gpu.instances", input.instances.size() * sizeof(SparseVoxelInstanceGPU));
  const Buffer centers = storage("svr.gpu.centers", visibleCapacity * sizeof(SparseVoxelCenterLengthGPU));
  const Buffer paths = storage("svr.gpu.paths", visibleCapacity * sizeof(SparseVoxelPathLevelGPU));
  const Buffer materials = storage("svr.gpu.materials", input.materials.size() * sizeof(SparseVoxelMaterialGPU));
  const Buffer microMaterials = storage("svr.gpu.microMaterials", sizeof(SparseVoxelMicroMaterialGPU));
  const Buffer microPayloads = storage("svr.gpu.microPayloads", pixelCapacity * sizeof(uint32_t));
  const Buffer materialSampleAddresses = storage(
      "svr.gpu.materialSampleAddresses",
      pixelCapacity * sizeof(VoxelMaterialSampleAddressGPU));
  const Buffer shadingPrograms = storage("svr.gpu.shadingPrograms", sizeof(voxel::material::VoxelShadingProgramGPU));
  const Buffer shadingInstructions = storage("svr.gpu.shadingInstructions", sizeof(voxel::material::VoxelShadingInstructionGPU));
  const Buffer shadingParameters = storage("svr.gpu.shadingParameters", sizeof(voxel::material::VoxelShadingParameterBlockGPU));
  const Buffer shadingPalettes = storage(
      "svr.gpu.shadingPalettes",
      sizeof(voxel::material::VoxelShadingPaletteGPU));
  const Buffer materialFieldDefaults = storage(
      "svr.gpu.materialFieldDefaults",
      sizeof(voxel::material::VoxelMaterialFieldDefaultsGPU));
  const Buffer materialFieldResolvers = storage(
      "svr.gpu.materialFieldResolvers",
      sizeof(voxel::material::VoxelMaterialFieldResolverGPU));
  const Buffer materialFieldMappings = storage(
      "svr.gpu.materialFieldMappings",
      sizeof(voxel::material::VoxelMaterialFieldMappingGPU));
  const Buffer projected = storage("svr.gpu.projected", projectedCapacity * sizeof(ProjectedVoxelGPU));
  const Buffer offsets = storage("svr.gpu.offsets", visibleCapacity * sizeof(uint32_t));
  const Buffer blockSums = storage("svr.gpu.blockSums", scanBlocks * sizeof(uint32_t));
  const Buffer blockOffsets = storage("svr.gpu.blockOffsets", scanBlocks * sizeof(uint32_t));
  const Buffer candidates = storage("svr.gpu.candidates", candidateCapacity * sizeof(TileCandidateGPU));
  const Buffer candidateScratch = storage("svr.gpu.candidateScratch", candidateCapacity * sizeof(TileCandidateGPU));
  const Buffer ranges = storage("svr.gpu.ranges", rangeCapacity * sizeof(SparseVoxelTileRangeGPU));
  const Buffer indirect = storage("svr.gpu.indirect", sizeof(input.indirect), true);
  const Buffer candidateCount = storage("svr.gpu.candidateCount", sizeof(uint32_t));
  const Buffer visibleVoxelBuffer = storage("svr.gpu.visibleVoxels", visibleCapacity * sizeof(VisibleVoxelGPU));
  const Buffer resolved = storage("svr.gpu.resolved", pixelCapacity * sizeof(ResolvedVoxelSurfaceGPU));
  const Buffer coverage = storage("svr.gpu.coverage", pixelCapacity * sizeof(uint32_t));
  const Buffer linearDepth = storage("svr.gpu.linearDepth", pixelCapacity * sizeof(float));
  const Buffer gbufferPixels = storage("svr.gpu.gbufferPixels", pixelCapacity * sizeof(SparseVoxelGBufferPixelGPU));
  const Buffer depthPixels = storage("svr.gpu.depthPixels", pixelCapacity * sizeof(float));

  graph.bufferWrite(counters, 0u, sizeof(input.counters), &input.counters);
  writeVector(graph, visible, input.visibleInstances);
  writeVector(graph, instances, input.instances);
  writeVector(graph, centers, input.centersLengths);
  writeVector(graph, paths, input.pathsLevels);
  writeVector(graph, materials, input.materials);
  SparseVoxelMicroMaterialGPU disabledMicroMaterial{};
  graph.bufferWrite(microMaterials, 0u, sizeof(disabledMicroMaterial), &disabledMicroMaterial);
  voxel::material::VoxelShadingPaletteGPU defaultShadingPalette{};
  graph.bufferWrite(
      shadingPalettes, 0u, sizeof(defaultShadingPalette),
      &defaultShadingPalette);
  writeVector(graph, projected, input.projected);
  writeVector(graph, candidates, input.candidates);
  writeVector(graph, ranges, input.inputRanges);
  std::vector<uint8_t> resolvedPoison(pixelCapacity * sizeof(ResolvedVoxelSurfaceGPU), 0xcdu);
  std::vector<uint32_t> coveragePoison(pixelCapacity, 0xcdcdcdcdu);
  std::vector<float> depthPoison(pixelCapacity, -1234.0f);
  if ((input.gbuffer || input.depthOnly) && !input.resolve)
    writeVector(graph, resolved, input.inputResolved);
  else
    graph.bufferWrite(resolved, 0u, resolvedPoison.size(), resolvedPoison.data());
  if (input.debugPresent && !input.resolve)
  {
    writeVector(graph, coverage, input.inputCoverage);
    writeVector(graph, linearDepth, input.inputLinearDepth);
  }
  else
  {
    writeVector(graph, coverage, coveragePoison);
    writeVector(graph, linearDepth, depthPoison);
  }
  graph.bufferWrite(indirect, 0u, sizeof(input.indirect), input.indirect.data());
  graph.bufferWrite(candidateCount, 0u, sizeof(input.candidateCount), &input.candidateCount);
  writeVector(graph, visibleVoxelBuffer, visibleVoxels);

  const svrRendering::SparseVoxelBinningPass::Buffers buffers{counters, visible, instances, centers, paths, projected, offsets, blockSums, blockOffsets, candidates, ranges, indirect, candidateCount, visibleVoxelBuffer};
  const svrRendering::SparseVoxelBinningPass::Settings settings{input.uniforms, static_cast<uint32_t>(input.visibleInstances.size()), static_cast<uint32_t>(input.instances.size())};
  uint32_t passIndex = 0u;
  auto add = [&](const char *name, const svrRendering::SparseVoxelBinningPass::Kernel kernel)
  {
    graph.registerPass<svrRendering::SparseVoxelBinningPass>(name, passIndex++, kernel, buffers, settings);
  };
  if (input.preprocess)
    add("svr.Preprocess", svrRendering::SparseVoxelBinningPass::Kernel::PreprocessVisibleVoxels);
  if (input.scan)
  {
    add("svr.ScanBlocks", svrRendering::SparseVoxelBinningPass::Kernel::ScanDuplicateBlocks);
    add("svr.ScanBlockSums", svrRendering::SparseVoxelBinningPass::Kernel::ScanDuplicateBlockSums);
    add("svr.ScanAdd", svrRendering::SparseVoxelBinningPass::Kernel::AddDuplicateBlockOffsets);
  }
  if (input.emit)
    add("svr.Emit", svrRendering::SparseVoxelBinningPass::Kernel::EmitTileCandidates);
  if (input.sort)
  {
    graph.registerPass<svrRendering::SparseVoxelRadixSortPass>("svr.Radix", passIndex++, candidateCapacity, candidates, candidateScratch, candidateCount, input.sortingBits);
    passIndex += svrRendering::SparseVoxelRadixSortPass::kChildPassCount;
  }
  if (input.ranges)
  {
    svrRendering::SparseVoxelBinningPass::Buffers rangeBuffers = buffers;
    if (input.sort)
      rangeBuffers.candidates = candidateScratch;
    graph.registerPass<svrRendering::SparseVoxelBinningPass>("svr.ClearRanges", passIndex++, svrRendering::SparseVoxelBinningPass::Kernel::ClearTileRanges, rangeBuffers, settings);
    graph.registerPass<svrRendering::SparseVoxelBinningPass>("svr.BuildRanges", passIndex++, svrRendering::SparseVoxelBinningPass::Kernel::BuildTileRanges, rangeBuffers, settings);
  }
  if (input.resolve)
  {
    const Buffer sortedCandidates = input.sort ? candidateScratch : candidates;
    const svrRendering::SparseVoxelResolvePass::Buffers resolveBuffers{
      instances, centers, projected, sortedCandidates, ranges, resolved,
      coverage, linearDepth, {}, materialSampleAddresses};
    const svrRendering::SparseVoxelResolvePass::Settings resolveSettings{
      input.uniforms,
      static_cast<uint32_t>(input.instances.size()),
      visibleCapacity,
      projectedCapacity,
      candidateCapacity,
    };
    graph.registerPass<svrRendering::SparseVoxelResolvePass>("svr.Resolve", passIndex++, resolveBuffers, resolveSettings);
  }

  auto texture = [&](const char *name, const ::rendering::Format format, const ::rendering::ImageUsage usage)
  {
    return graph.createTexture(
        {.name = name,
         .format = format,
         .height = input.uniforms.viewportTiles[1],
         .width = input.uniforms.viewportTiles[0],
         .memoryProperties = BufferUsage::BufferUsage_None,
         .mipLevels = 1u,
         .usage = usage});
  };

  svrRendering::SparseVoxelGBufferPass::FrameTarget gbufferTarget{};
  if (input.gbuffer)
  {
    const ::rendering::ImageUsage colorUsage = ::rendering::ImageUsage::ImageUsage_ColorAttachment | ::rendering::ImageUsage::ImageUsage_Sampled;
    const ::rendering::ImageUsage materialUsage = ::rendering::ImageUsage::ImageUsage_Storage | ::rendering::ImageUsage::ImageUsage_Sampled;
    gbufferTarget = {
      texture("svr.gbuffer.instance", ::rendering::Format::Format_R32Uint, colorUsage),
      texture("svr.gbuffer.voxel", ::rendering::Format::Format_R32Uint, colorUsage),
      texture("svr.gbuffer.materialId", ::rendering::Format::Format_R32Uint, colorUsage),
      texture("svr.gbuffer.materialUv", ::rendering::Format::Format_RG32Float, colorUsage),
      texture("svr.gbuffer.baseColor", ::rendering::Format::Format_RGBA16Float, materialUsage),
      texture("svr.gbuffer.materialProperties", ::rendering::Format::Format_RGBA16Float, materialUsage),
      texture("svr.gbuffer.emissive", ::rendering::Format::Format_RGBA16Float, materialUsage),
      texture("svr.gbuffer.geometryNormal", ::rendering::Format::Format_RGBA16Float, colorUsage),
      texture("svr.gbuffer.normal", ::rendering::Format::Format_RGBA16Float, materialUsage),
      texture("svr.gbuffer.motion", ::rendering::Format::Format_RG16Float, colorUsage),
      texture(
          "svr.gbuffer.depth",
          ::rendering::Format::Format_Depth32Float,
          ::rendering::ImageUsage::ImageUsage_DepthStencilAttachment | ::rendering::ImageUsage::ImageUsage_Sampled),
      texture("svr.gbuffer.temporalId", ::rendering::Format::Format_R32Uint, colorUsage),
    };
    const Texture materialFieldAtlas = texture(
        "svr.gbuffer.materialFieldAtlas",
        ::rendering::Format::Format_RGBA8Uint,
        ::rendering::ImageUsage::ImageUsage_Sampled);
    svrRendering::SparseVoxelGBufferPass::Settings gbufferSettings{
      input.uniforms,
      static_cast<uint32_t>(input.instances.size()),
      static_cast<uint32_t>(input.materials.size()),
      input.uniforms.gbufferClearDepth,
    };
    // G9 validates the authored base G-buffer contract. Terrain-only colour
    // variation is covered separately and would intentionally diverge from
    // this reference fixture.
    gbufferSettings.microDetail[0] = 0.0f;
    gbufferSettings.normalFilter[0] = 0.0f;
    gbufferSettings.distantFilter[0] = 0.0f;
    graph.registerPass<svrRendering::SparseVoxelGBufferPass>(
        "svr.GBuffer",
        passIndex++,
        svrRendering::SparseVoxelGBufferPass::Buffers{
          instances, materials, resolved, microMaterials, microPayloads,
          shadingPrograms, shadingInstructions, shadingParameters,
          materialFieldDefaults, materialSampleAddresses, shadingPalettes},
        gbufferTarget,
        svrRendering::SparseVoxelGBufferPass::MaterialFields{
          materialFieldResolvers, materialFieldMappings,
          materialFieldAtlas},
        gbufferSettings);
    svrRendering::SparseVoxelGBufferPass::FrameTarget sampledTarget = gbufferTarget;
    if (input.debugPresent)
    {
      const Texture debugOutput = texture("svr.gbuffer.debugPresent", ::rendering::Format::Format_RGBA16Float, materialUsage);
      graph.registerPass<svrRendering::SparseVoxelDebugPresentPass>(
          "svr.DebugPresent",
          passIndex++,
          gbufferTarget,
          coverage,
          linearDepth,
          debugOutput,
          SparseVoxelDebugPresentUniformsGPU{
            input.uniforms.viewportTiles[0],
            input.uniforms.viewportTiles[1],
            static_cast<uint32_t>(input.debugMode),
            input.debugLinearDepthScale,
          });
      sampledTarget.baseColor = debugOutput;
    }
    graph.registerPass<svrRendering::SparseVoxelGBufferReadbackPass>("svr.GBufferReadback", passIndex++, sampledTarget, gbufferPixels, input.uniforms);
  }

  if (input.depthOnly)
  {
    const Texture depthTarget = texture(
        "svr.depthOnly.depth",
        ::rendering::Format::Format_Depth32Float,
        ::rendering::ImageUsage::ImageUsage_DepthStencilAttachment | ::rendering::ImageUsage::ImageUsage_Sampled);
    graph.registerPass<svrRendering::SparseVoxelDepthPass>(
        "svr.WriteVoxelDepth",
        passIndex++,
        svrRendering::SparseVoxelDepthPass::Buffers{instances, resolved},
        depthTarget,
        svrRendering::SparseVoxelDepthPass::Settings{
          input.uniforms,
          static_cast<uint32_t>(input.instances.size()),
          input.uniforms.gbufferClearDepth,
          input.depthLoadOp,
          ::rendering::StoreOp::StoreOp_Store,
          input.depthComparison,
        });
    graph.registerPass<svrRendering::SparseVoxelDepthReadbackPass>(
        "svr.DepthReadback", passIndex++, depthTarget, depthPixels, input.uniforms);
  }

  const Buffer countersReadback = readback("svr.read.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer projectedReadback = readback("svr.read.projected", projectedCapacity * sizeof(ProjectedVoxelGPU));
  const Buffer offsetsReadback = readback("svr.read.offsets", visibleCapacity * sizeof(uint32_t));
  const Buffer candidatesReadback = readback("svr.read.candidates", candidateCapacity * sizeof(TileCandidateGPU));
  const Buffer rangesReadback = readback("svr.read.ranges", rangeCapacity * sizeof(SparseVoxelTileRangeGPU));
  const Buffer indirectReadback = readback("svr.read.indirect", sizeof(input.indirect));
  const Buffer candidateCountReadback = readback("svr.read.candidateCount", sizeof(uint32_t));
  const Buffer resolvedReadback = readback("svr.read.resolved", pixelCapacity * sizeof(ResolvedVoxelSurfaceGPU));
  const Buffer coverageReadback = readback("svr.read.coverage", pixelCapacity * sizeof(uint32_t));
  const Buffer linearDepthReadback = readback("svr.read.linearDepth", pixelCapacity * sizeof(float));
  const Buffer materialSampleAddressesReadback = readback(
      "svr.read.materialSampleAddresses",
      pixelCapacity * sizeof(VoxelMaterialSampleAddressGPU));
  const Buffer gbufferReadback = readback("svr.read.gbuffer", pixelCapacity * sizeof(SparseVoxelGBufferPixelGPU));
  const Buffer depthReadback = readback("svr.read.depthOnly", pixelCapacity * sizeof(float));
  auto copy = [&](const char *name, const Buffer source, const uint64_t size, const Buffer destination)
  {
    graph.registerPass<::rendering::gpgpu::CopyBufferPass>(name, passIndex++, source, 0u, size, destination, 0u, size);
  };
  copy("svr.CopyCounters", counters, sizeof(SparseVoxelFrameCountersGPU), countersReadback);
  copy("svr.CopyProjected", projected, projectedCapacity * sizeof(ProjectedVoxelGPU), projectedReadback);
  copy("svr.CopyOffsets", offsets, visibleCapacity * sizeof(uint32_t), offsetsReadback);
  copy("svr.CopyCandidates", input.sort ? candidateScratch : candidates, candidateCapacity * sizeof(TileCandidateGPU), candidatesReadback);
  copy("svr.CopyRanges", ranges, rangeCapacity * sizeof(SparseVoxelTileRangeGPU), rangesReadback);
  copy("svr.CopyIndirect", indirect, sizeof(input.indirect), indirectReadback);
  copy("svr.CopyCandidateCount", candidateCount, sizeof(uint32_t), candidateCountReadback);
  if (input.resolve)
  {
    copy("svr.CopyResolved", resolved, pixelCapacity * sizeof(ResolvedVoxelSurfaceGPU), resolvedReadback);
    copy("svr.CopyCoverage", coverage, pixelCapacity * sizeof(uint32_t), coverageReadback);
    copy("svr.CopyLinearDepth", linearDepth, pixelCapacity * sizeof(float), linearDepthReadback);
    copy(
        "svr.CopyMaterialSampleAddresses", materialSampleAddresses,
        pixelCapacity * sizeof(VoxelMaterialSampleAddressGPU),
        materialSampleAddressesReadback);
  }
  if (input.gbuffer)
    copy("svr.CopyGBuffer", gbufferPixels, pixelCapacity * sizeof(SparseVoxelGBufferPixelGPU), gbufferReadback);
  if (input.depthOnly)
    copy("svr.CopyDepthOnly", depthPixels, pixelCapacity * sizeof(float), depthReadback);

  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  GpuOutput output{};
  output.projected.resize(projectedCapacity);
  output.offsets.resize(visibleCapacity);
  output.candidates.resize(candidateCapacity);
  output.ranges.resize(rangeCapacity);
  if (input.resolve)
  {
    output.resolved.resize(pixelCapacity);
    output.coverage.resize(pixelCapacity);
    output.linearDepth.resize(pixelCapacity);
    output.materialSampleAddresses.resize(pixelCapacity);
  }
  if (input.gbuffer)
    output.gbuffer.resize(pixelCapacity);
  if (input.depthOnly)
    output.depthOnly.resize(pixelCapacity);
  graph.bufferRead(
      countersReadback,
      0u,
      sizeof(output.counters),
      [&](const void *data)
      {
        std::memcpy(&output.counters, data, sizeof(output.counters));
      });
  readVector(graph, projectedReadback, output.projected);
  readVector(graph, offsetsReadback, output.offsets);
  readVector(graph, candidatesReadback, output.candidates);
  readVector(graph, rangesReadback, output.ranges);
  if (input.resolve)
  {
    readVector(graph, resolvedReadback, output.resolved);
    readVector(graph, coverageReadback, output.coverage);
    readVector(graph, linearDepthReadback, output.linearDepth);
    readVector(
        graph, materialSampleAddressesReadback,
        output.materialSampleAddresses);
  }
  if (input.gbuffer)
    readVector(graph, gbufferReadback, output.gbuffer);
  if (input.depthOnly)
    readVector(graph, depthReadback, output.depthOnly);
  graph.bufferRead(
      indirectReadback,
      0u,
      sizeof(output.indirect),
      [&](const void *data)
      {
        std::memcpy(output.indirect.data(), data, sizeof(output.indirect));
      });
  graph.bufferRead(
      candidateCountReadback,
      0u,
      sizeof(output.candidateCount),
      [&](const void *data)
      {
        std::memcpy(&output.candidateCount, data, sizeof(output.candidateCount));
      });
  return output;
}

GpuInput inputFromFixture(const ref::ProjectionFixture &fixture)
{
  GpuInput input{};
  input.uniforms = fixture.uniforms;
  input.instances = fixture.instances;
  input.visibleInstances = fixture.visibleInstances;
  input.centersLengths = fixture.centersLengths;
  input.pathsLevels = fixture.pathsLevels;
  for (const auto &visible : fixture.visibleInstances)
    input.counters.visibleVoxelCount += visible.visibleVoxelCount;
  input.counters.visibleInstanceCount = static_cast<uint32_t>(fixture.visibleInstances.size());
  input.indirect[0] = {(input.counters.visibleVoxelCount + 255u) / 256u, 1u, 1u, 0u};
  return input;
}

struct ResolveSelection
{
  uint32_t instanceIndex = 0u;
  uint32_t voxelIndex = 0u;
};

ref::ResolveFixture makeSinglePixelResolveFixture(
    const std::array<float, 3> &camera,
    const std::array<float, 3> &forward,
    std::vector<SparseVoxelInstanceGPU> instances,
    std::vector<SparseVoxelCenterLengthGPU> centers,
    const std::vector<ResolveSelection> &selections)
{
  ref::ResolveFixture fixture{};
  fixture.uniforms.worldToCamera = identityMatrix();
  fixture.uniforms.cameraToWorld = identityMatrix();
  fixture.uniforms.cameraToWorld[8] = forward[0];
  fixture.uniforms.cameraToWorld[9] = forward[1];
  fixture.uniforms.cameraToWorld[10] = forward[2];
  fixture.uniforms.cameraWorldPositionNear = {camera[0], camera[1], camera[2], 0.01f};
  fixture.uniforms.focalPrincipal = {1.0f, 1.0f, 0.5f, 0.5f};
  setViewport(fixture.uniforms, 1u, 1u);
  fixture.uniforms.visibleVoxelCapacity = std::max<uint32_t>(1u, static_cast<uint32_t>(centers.size()));
  fixture.uniforms.projectedVoxelCapacity = std::max<uint32_t>(1u, static_cast<uint32_t>(selections.size()));
  fixture.uniforms.candidateCapacity = std::max<uint32_t>(1u, static_cast<uint32_t>(selections.size()));
  fixture.instances = std::move(instances);
  fixture.centersLengths = std::move(centers);
  const ref::Ray worldRay = ref::cameraRay(fixture.uniforms, 0u, 0u);
  for (uint32_t index = 0u; index < selections.size(); ++index)
  {
    const ResolveSelection selection = selections[index];
    ProjectedVoxelGPU projected{};
    projected.packedInclusivePixelBounds = {ref::packPixel(0u, 0u), ref::packPixel(0u, 0u)};
    projected.instanceIndex = selection.instanceIndex;
    projected.voxelIndex = selection.voxelIndex;
    fixture.projected.push_back(projected);
    const SparseVoxelInstanceGPU &instance = fixture.instances.at(selection.instanceIndex);
    const std::array<float, 3> localDirection = ref::transformVector(instance.worldToModel, worldRay.direction);
    TileCandidateGPU candidate{};
    candidate.instanceIndex = selection.instanceIndex;
    candidate.voxelIndex = selection.voxelIndex;
    candidate.quadrant = ref::rayQuadrant(localDirection);
    candidate.projectedVoxelIndex = index;
    candidate.reserved0 = index;
    fixture.candidates.push_back(candidate);
  }
  fixture.ranges.push_back({0u, static_cast<uint32_t>(fixture.candidates.size()), 0u, 0u});
  return fixture;
}

GpuInput inputFromResolveFixture(const ref::ResolveFixture &fixture)
{
  GpuInput input{};
  input.uniforms = fixture.uniforms;
  input.instances = fixture.instances;
  input.centersLengths = fixture.centersLengths;
  input.projected = fixture.projected;
  input.candidates = fixture.candidates;
  input.inputRanges = fixture.ranges;
  input.candidateCount = static_cast<uint32_t>(fixture.candidates.size());
  input.counters.candidateCount = input.candidateCount;
  input.resolve = true;
  return input;
}

void testG3(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const ref::ProjectionFixture fixture = makeProjectionFixture();
  const std::vector<ProjectedVoxelGPU> expected = ref::preprocess(fixture);
  assert(expected.size() == 7u);
  assert(expected[0].quadrantBitset == 0x55u);
  assert(expected[0].duplicateCount == 64u);
  assert(expected[2].duplicateCount == 0u);
  assert(expected[2].instanceIndex == kInvalidSparseVoxelIndex);
  assert(expected[3].duplicateCount > 0u);  // near-plane corner clipping remains conservative
  assert(expected[3].packedInclusivePixelBounds == (std::array<uint32_t, 2>{ref::packPixel(0u, 0u), ref::packPixel(64u, 64u)}));
  assert(expected[5].packedInclusivePixelBounds == (std::array<uint32_t, 2>{ref::packPixel(18u, 18u), ref::packPixel(45u, 45u)}));
  assert(expected[5].duplicateCount == 256u); // center distance must not reject a near-plane-crossing box
  assert(expected[6].packedInclusivePixelBounds == (std::array<uint32_t, 2>{ref::packPixel(52u, 25u), ref::packPixel(64u, 38u)}));
  assert(expected[6].duplicateCount == 24u); // clipped projection remains bounded to its small screen rectangle

  GpuInput input = inputFromFixture(fixture);
  input.preprocess = true;
  GpuOutput gpu = runGpu(rhi, input);
  gpu.projected.resize(expected.size());
  assert(projectedEqual(gpu.projected, expected));

  input = inputFromFixture(fixture);
  input.uniforms.projectedVoxelCapacity = static_cast<uint32_t>(expected.size()) - 1u;
  input.preprocess = true;
  gpu = runGpu(rhi, input);
  assert(gpu.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::ProjectedVoxels));
  assert(gpu.counters.requiredCount == expected.size());
  assert(gpu.counters.capacity + 1u == expected.size());
  assert(gpu.counters.visibleVoxelCount == 0u && gpu.candidateCount == 0u);
  for (const IndirectDispatchArgsGPU &args : gpu.indirect)
    assert(args.x == 0u);
}

void testG4(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  for (const uint32_t count : {0u, 1u, 255u, 256u, 257u})
  {
    std::vector<ProjectedVoxelGPU> edgeProjected(count);
    for (uint32_t index = 0u; index < count; ++index)
      edgeProjected[index].duplicateCount = (index % 5u) + 1u;
    const ref::ScanResult edgeExpected = ref::exclusiveScan(edgeProjected, 4096u);
    GpuInput edgeInput{};
    edgeInput.uniforms.visibleVoxelCapacity = std::max(1u, count);
    edgeInput.uniforms.projectedVoxelCapacity = std::max(1u, count);
    edgeInput.uniforms.candidateCapacity = 4096u;
    edgeInput.uniforms.tileRangeCapacity = 1u;
    edgeInput.counters.visibleVoxelCount = count;
    edgeInput.projected = edgeProjected;
    edgeInput.indirect[0] = {(count + 255u) / 256u, 1u, 1u, 0u};
    edgeInput.scan = true;
    GpuOutput edgeGpu = runGpu(rhi, edgeInput);
    edgeGpu.offsets.resize(count);
    assert(edgeGpu.offsets == edgeExpected.offsets);
    assert(edgeGpu.candidateCount == edgeExpected.requiredCount);
    assert(edgeGpu.indirect[1].x == (count + 255u) / 256u);
  }

  std::mt19937 random(0x5ca11ab1u);
  std::vector<ProjectedVoxelGPU> projected(513u);
  for (ProjectedVoxelGPU &voxel : projected)
    voxel.duplicateCount = random() % 9u;
  projected[0].duplicateCount = 0u;
  projected[255].duplicateCount = 7u;
  projected[256].duplicateCount = 3u;
  const ref::ScanResult expected = ref::exclusiveScan(projected, 4096u);

  GpuInput input{};
  input.uniforms.visibleVoxelCapacity = projected.size();
  input.uniforms.projectedVoxelCapacity = projected.size();
  input.uniforms.candidateCapacity = 4096u;
  input.uniforms.tileRangeCapacity = 1u;
  input.counters.visibleVoxelCount = projected.size();
  input.projected = projected;
  input.indirect[0] = {(static_cast<uint32_t>(projected.size()) + 255u) / 256u, 1u, 1u, 0u};
  input.scan = true;
  GpuOutput gpu = runGpu(rhi, input);
  gpu.offsets.resize(projected.size());
  assert(gpu.offsets == expected.offsets);
  assert(gpu.candidateCount == expected.requiredCount);
  assert(gpu.counters.candidateCount == expected.requiredCount);
  assert(gpu.indirect[1].x == (projected.size() + 255u) / 256u);
  assert(gpu.indirect[2].x == (expected.requiredCount + 255u) / 256u);

  input.uniforms.candidateCapacity = expected.requiredCount - 1u;
  gpu = runGpu(rhi, input);
  assert(gpu.candidateCount == input.uniforms.candidateCapacity);
  assert(gpu.counters.candidateCount == input.uniforms.candidateCapacity);
  assert(gpu.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::TileCandidates));
  assert(gpu.counters.requiredCount == expected.requiredCount);
  assert(gpu.indirect[1].x == (projected.size() + 255u) / 256u);
  assert(gpu.indirect[2].x ==
      (input.uniforms.candidateCapacity + 255u) / 256u);
  assert(gpu.indirect[3].x ==
      (input.uniforms.candidateCapacity + 255u) / 256u);
}

void testG5(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  ref::ProjectionFixture fixture = makeProjectionFixture();
  fixture.pathsLevels[0].pathLo = 0xffffffffu;
  fixture.pathsLevels[0].pathHi = 0x0000ffffu;
  std::vector<ProjectedVoxelGPU> projected = ref::preprocess(fixture);
  const ref::ScanResult scan = ref::exclusiveScan(projected, fixture.uniforms.candidateCapacity);
  const std::vector<TileCandidateGPU> expected = ref::emit(projected, scan.offsets, fixture.pathsLevels, fixture.uniforms);
  assert(!expected.empty());
  bool highPathBitsObserved = false;
  for (const TileCandidateGPU &candidate : expected)
    highPathBitsObserved |= (candidate.keyHi & 0xffffu) != 0u;
  assert(highPathBitsObserved);

  GpuInput input = inputFromFixture(fixture);
  input.projected = projected;
  input.scan = true;
  input.emit = true;
  GpuOutput gpu = runGpu(rhi, input);
  gpu.candidates.resize(expected.size());
  assertCandidatesEqual(gpu.candidates, expected);
  for (uint32_t index = 0u; index < gpu.candidates.size(); ++index)
    assert(gpu.candidates[index].reserved0 == index); // stable emission ordinal
}

void testG6(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  std::mt19937 random(0xa64bad5eu);
  std::vector<TileCandidateGPU> candidates(1301u);
  for (uint32_t index = 0u; index < candidates.size(); ++index)
  {
    candidates[index].keyLo = random();
    candidates[index].keyHi = random();
    candidates[index].voxelIndex = index;
    candidates[index].reserved0 = index;
  }
  candidates[7].keyLo = candidates[8].keyLo = 0x12345678u;
  candidates[7].keyHi = candidates[8].keyHi = 0x9abcdef0u;
  candidates[9].keyLo = candidates[7].keyLo;
  candidates[9].keyHi = candidates[7].keyHi + 1u;
  std::vector<TileCandidateGPU> expected = candidates;
  ref::stableSort(expected);

  GpuInput input{};
  input.uniforms.visibleVoxelCapacity = 1u;
  input.uniforms.projectedVoxelCapacity = 1u;
  input.uniforms.candidateCapacity = candidates.size();
  input.uniforms.tileRangeCapacity = 1u;
  input.counters.candidateCount = candidates.size();
  input.candidateCount = candidates.size();
  input.candidates = candidates;
  input.sort = true;
  GpuOutput gpu = runGpu(rhi, input);
  gpu.candidates.resize(expected.size());
  assertCandidatesEqual(gpu.candidates, expected);
  const auto firstEqual = std::find_if(
      gpu.candidates.begin(),
      gpu.candidates.end(),
      [&](const TileCandidateGPU &candidate)
      {
        return ref::candidateKey(candidate) == ref::candidateKey(candidates[7]);
      });
  assert(firstEqual != gpu.candidates.end());
  assert(firstEqual->reserved0 == 7u);
  assert((firstEqual + 1)->reserved0 == 8u);

  candidates.assign(103871u, {});
  for (uint32_t index = 0u; index < candidates.size(); ++index)
  {
    candidates[index].keyLo = 0xdeadbeefu;
    candidates[index].keyHi = 0x0012db35u;
    candidates[index].voxelIndex = index;
    candidates[index].reserved0 = index;
  }
  input.uniforms.candidateCapacity = candidates.size();
  input.counters.candidateCount = candidates.size();
  input.candidateCount = candidates.size();
  input.candidates = candidates;
  gpu = runGpu(rhi, input);
  gpu.candidates.resize(candidates.size());
  assertCandidatesEqual(gpu.candidates, candidates);

  for (uint32_t index = 0u; index < candidates.size(); ++index)
    candidates[index].keyLo = (index * 0x9e3779b9u) ^ (index >> 3u);
  expected = candidates;
  std::stable_sort(
      expected.begin(),
      expected.end(),
      [](const TileCandidateGPU &left, const TileCandidateGPU &right)
      {
        return left.keyLo < right.keyLo;
      });
  input.sortingBits = 32u;
  input.candidates = candidates;
  gpu = runGpu(rhi, input);
  gpu.candidates.resize(expected.size());
  assertCandidatesEqual(gpu.candidates, expected);

  for (uint32_t index = 0u; index < candidates.size(); ++index)
    candidates[index].keyHi = (index * 0x85ebca6bu) ^ (index >> 7u);
  expected = candidates;
  ref::stableSort(expected);
  input.sortingBits = 64u;
  input.candidates = candidates;
  gpu = runGpu(rhi, input);
  gpu.candidates.resize(expected.size());
  assertCandidatesEqual(gpu.candidates, expected);
}

void testG7(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  ref::ProjectionFixture fixture = makeProjectionFixture();
  const std::vector<ProjectedVoxelGPU> projected = ref::preprocess(fixture);
  const ref::ScanResult scan = ref::exclusiveScan(projected, fixture.uniforms.candidateCapacity);
  std::vector<TileCandidateGPU> expectedCandidates = ref::emit(projected, scan.offsets, fixture.pathsLevels, fixture.uniforms);
  ref::stableSort(expectedCandidates);
  const std::vector<SparseVoxelTileRangeGPU> expectedRanges = ref::buildRanges(expectedCandidates, fixture.uniforms.tileRangeCapacity);

  GpuInput input = inputFromFixture(fixture);
  input.preprocess = true;
  input.scan = true;
  input.emit = true;
  input.sort = true;
  input.ranges = true;
  GpuOutput gpu = runGpu(rhi, input);
  gpu.projected.resize(projected.size());
  gpu.offsets.resize(scan.offsets.size());
  gpu.candidates.resize(expectedCandidates.size());
  assert(projectedEqual(gpu.projected, projected));
  assert(gpu.offsets == scan.offsets);
  assertCandidatesEqual(gpu.candidates, expectedCandidates);
  assert(equalBytes(gpu.ranges, expectedRanges));

  // A second, smaller frame must clear stale ranges before rebuilding.
  input.counters.visibleVoxelCount = 0u;
  input.counters.candidateCount = 0u;
  input.candidateCount = 0u;
  input.preprocess = input.scan = input.emit = input.sort = false;
  input.indirect[3] = {0u, 1u, 1u, 0u};
  gpu = runGpu(rhi, input);
  const std::vector<SparseVoxelTileRangeGPU> empty(fixture.uniforms.tileRangeCapacity);
  assert(equalBytes(gpu.ranges, empty));

  // Required fixed-camera synthetic-shell prefix: this exercises all 96,776
  // resident voxels without a scene, TLAS, file, or CPU visibility readback.
  const BootstrapSparseVoxelModel shell = generateExteriorShell128();
  ref::ProjectionFixture shellFixture{};
  shellFixture.uniforms.worldToCamera = identityMatrix();
  shellFixture.uniforms.cameraToWorld = identityMatrix();
  shellFixture.uniforms.worldToCamera[14] = 4.0f;
  shellFixture.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, -4.0f, 0.1f};
  shellFixture.uniforms.focalPrincipal = {64.0f, 64.0f, 64.0f, 64.0f};
  setViewport(shellFixture.uniforms, 128u, 128u);
  shellFixture.uniforms.visibleVoxelCapacity = kExteriorShellVoxelCount;
  shellFixture.uniforms.projectedVoxelCapacity = kExteriorShellVoxelCount;
  shellFixture.uniforms.candidateCapacity = 1000000u;
  shellFixture.instances.push_back(shell.instance);
  shellFixture.visibleInstances.push_back({0u, 0u, kExteriorShellVoxelCount, 0u});
  shellFixture.centersLengths = shell.centersLengths;
  shellFixture.pathsLevels = shell.pathsLevels;
  const std::vector<ProjectedVoxelGPU> shellProjected = ref::preprocess(shellFixture);
  const ref::ScanResult shellScan = ref::exclusiveScan(shellProjected, shellFixture.uniforms.candidateCapacity);
  assert(!shellScan.overflow && shellScan.candidateCount > kExteriorShellVoxelCount);
  std::vector<TileCandidateGPU> shellCandidates = ref::emit(shellProjected, shellScan.offsets, shellFixture.pathsLevels, shellFixture.uniforms);
  const std::vector<TileCandidateGPU> shellCandidatesUnsorted = shellCandidates;
  ref::stableSort(shellCandidates);
  const std::vector<SparseVoxelTileRangeGPU> shellRanges = ref::buildRanges(shellCandidates, shellFixture.uniforms.tileRangeCapacity);
  input = inputFromFixture(shellFixture);
  input.preprocess = input.scan = input.emit = true;
  gpu = runGpu(rhi, input);
  gpu.candidates.resize(shellCandidatesUnsorted.size());
  assertCandidatesEqual(gpu.candidates, shellCandidatesUnsorted);

  input = {};
  input.uniforms.visibleVoxelCapacity = 1u;
  input.uniforms.projectedVoxelCapacity = 1u;
  input.uniforms.candidateCapacity = shellScan.candidateCount;
  input.uniforms.tileRangeCapacity = 1u;
  input.counters.candidateCount = shellScan.candidateCount;
  input.candidateCount = shellScan.candidateCount;
  input.candidates = shellCandidatesUnsorted;
  input.sort = true;
  gpu = runGpu(rhi, input);
  gpu.candidates.resize(shellCandidates.size());
  assertCandidatesEqual(gpu.candidates, shellCandidates);

  input = inputFromFixture(shellFixture);
  input.preprocess = input.scan = input.emit = input.sort = input.ranges = true;
  gpu = runGpu(rhi, std::move(input));
  gpu.projected.resize(shellProjected.size());
  gpu.offsets.resize(shellScan.offsets.size());
  gpu.candidates.resize(shellCandidates.size());
  assert(projectedEqual(gpu.projected, shellProjected));
  assert(gpu.offsets == shellScan.offsets);
  assertCandidatesEqual(gpu.candidates, shellCandidates);
  assert(equalBytes(gpu.ranges, shellRanges));
}

void testG8(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelInstanceGPU identityInstance{};
  identityInstance.modelToWorld = identityMatrix();
  identityInstance.worldToModel = identityMatrix();
  identityInstance.previousModelToWorld = identityMatrix();
  const SparseVoxelCenterLengthGPU unitBox{{0.0f, 0.0f, 0.0f}, 1.0f};

  struct FaceCase
  {
    std::array<float, 3> origin;
    std::array<float, 3> direction;
    uint32_t face;
  };
  const std::array<FaceCase, 6> faceCases = {
    FaceCase{{-2.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, ref::packLocalFace(0u, false)},
    FaceCase{{2.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, ref::packLocalFace(0u, true)},
    FaceCase{{0.0f, -2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, ref::packLocalFace(1u, false)},
    FaceCase{{0.0f, 2.0f, 0.0f}, {0.0f, -1.0f, 0.0f}, ref::packLocalFace(1u, true)},
    FaceCase{{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}, ref::packLocalFace(2u, false)},
    FaceCase{{0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}, ref::packLocalFace(2u, true)},
  };
  for (const FaceCase &faceCase : faceCases)
  {
    const ref::VoxelSegment segment = ref::voxelSegmentForPixel({faceCase.origin, faceCase.direction}, unitBox);
    assert(segment.hit && !segment.cameraInside);
    assert(nearFloat(segment.tEnter, 1.5f));
    assert(segment.enterFace == faceCase.face);

    const ref::ResolveFixture fixture = makeSinglePixelResolveFixture(faceCase.origin, faceCase.direction, {identityInstance}, {unitBox}, {{0u, 0u}});
    const ref::ResolveResult expected = ref::resolveSolidVoxels(fixture);
    assert(expected.coverage[0] == 1u);
    assert(expected.surfaces[0].packedLocalFace == faceCase.face);
    assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);
  }

  ref::ResolveFixture fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {identityInstance}, {unitBox}, {{0u, 0u}});
  ref::ResolveResult expected = ref::resolveSolidVoxels(fixture);
  assert(expected.surfaces[0].hitFlags == (kSparseVoxelSurfaceHit | kSparseVoxelSurfaceCameraInside));
  assert(expected.surfaces[0].packedLocalFace == ref::packLocalFace(2u, true));
  assert(nearFloat(expected.surfaces[0].surfaceT, 0.5f));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // A camera exactly on a shared face remains covered from either side.
  const std::vector<SparseVoxelCenterLengthGPU> adjacentBoxes = {{{0.0f, 0.0f, 1.5f}, 1.0f}, {{0.0f, 0.0f, 2.5f}, 1.0f}};
  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 1.0f}, {identityInstance}, adjacentBoxes, {{0u, 0u}, {0u, 1u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 1u && nearFloat(expected.surfaces[0].surfaceT, 0.0f));
  assert(expected.surfaces[0].voxelIndex == 0u && expected.surfaces[0].packedLocalFace == ref::packLocalFace(2u, true));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);
  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, -1.0f}, {identityInstance}, adjacentBoxes, {{0u, 0u}, {0u, 1u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 1u && nearFloat(expected.surfaces[0].surfaceT, 0.0f));
  assert(expected.surfaces[0].voxelIndex == 1u && expected.surfaces[0].packedLocalFace == ref::packLocalFace(2u, false));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // The exhaustive scan must ignore candidate order and select the nearest box.
  fixture = makeSinglePixelResolveFixture(
      {0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
      {identityInstance},
      {{{0.0f, 0.0f, 3.0f}, 1.0f}, {{0.0f, 0.0f, 2.0f}, 1.0f}},
      {{0u, 0u}, {0u, 1u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.surfaces[0].voxelIndex == 1u);
  assert(nearFloat(expected.surfaces[0].surfaceT, 1.5f));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // Epsilon ties resolve by instance, face incidence, then voxel ID, regardless of enumeration.
  fixture = makeSinglePixelResolveFixture(
      {0.0f, 0.0f, 0.0f},
      {0.0f, 0.0f, 1.0f},
      {identityInstance, identityInstance},
      {{{0.0f, 0.0f, 2.000005f}, 1.0f}, {{0.0f, 0.0f, 2.0f}, 1.0f}},
      {{1u, 1u}, {0u, 1u}, {0u, 0u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.surfaces[0].instanceIndex == 0u && expected.surfaces[0].voxelIndex == 0u);
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // At a shared voxel edge, choose the more front-facing exterior face. A
  // sub-epsilon depth difference must not expose the lower voxel's top face as
  // a one-pixel normal stripe, even when its lower ID wins the surface tie.
  const std::vector<SparseVoxelCenterLengthGPU> verticalColumn = {
    {{0.0f, -0.5f, 2.0f}, 1.0f},
    {{0.0f, 0.5f, 2.0f}, 1.0f},
  };
  const std::array<float, 3> edgeCamera = {-2.0f, 0.750002f, 2.0f};
  const std::array<float, 3> edgeDirection = {1.0f, -0.5f, 0.0f};
  const ref::VoxelSegment edgeSegment = ref::voxelSegmentForPixel(
      {edgeCamera, ref::normalized(edgeDirection)}, verticalColumn[0]);
  assert(edgeSegment.hit && edgeSegment.enterFace == ref::packLocalFace(0u, false));
  for (const std::vector<ResolveSelection> order : {
         std::vector<ResolveSelection>{{0u, 0u}, {0u, 1u}},
         std::vector<ResolveSelection>{{0u, 1u}, {0u, 0u}},
       })
  {
    fixture = makeSinglePixelResolveFixture(edgeCamera, edgeDirection, {identityInstance}, verticalColumn, order);
    expected = ref::resolveSolidVoxels(fixture);
    assert(expected.coverage[0] == 1u);
    assert(expected.surfaces[0].voxelIndex == 0u);
    assert(expected.surfaces[0].packedLocalFace == ref::packLocalFace(0u, false));
    assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);
  }

  // A non-power-of-two octree length can leave a real float gap between
  // independently reconstructed center +/- radius bounds. The one-ULP
  // endpoint expansion must close that gap and keep the wall's front normal.
  constexpr float importedRootMinimum = -1.37f;
  constexpr float importedRootExtent = 2.56f;
  constexpr uint32_t importedResolution = 256u;
  const float importedLength = importedRootExtent / static_cast<float>(importedResolution);
  std::array<SparseVoxelCenterLengthGPU, 2> importedAdjacent{};
  bool foundFloatGap = false;
  for (uint32_t coordinate = 0u; coordinate + 1u < importedResolution; ++coordinate)
  {
    const float lowerCenter = importedRootMinimum + (static_cast<float>(coordinate) + 0.5f) * importedLength;
    const float upperCenter = importedRootMinimum + (static_cast<float>(coordinate + 1u) + 0.5f) * importedLength;
    if (lowerCenter + importedLength * 0.5f < upperCenter - importedLength * 0.5f)
    {
      importedAdjacent = {
        SparseVoxelCenterLengthGPU{{0.0f, lowerCenter, 2.0f}, importedLength},
        SparseVoxelCenterLengthGPU{{0.0f, upperCenter, 2.0f}, importedLength},
      };
      foundFloatGap = true;
      break;
    }
  }
  assert(foundFloatGap);
  assert(std::nextafter(
             importedAdjacent[0].center[1] + importedLength * 0.5f,
             std::numeric_limits<float>::infinity()) >=
         std::nextafter(
             importedAdjacent[1].center[1] - importedLength * 0.5f,
             -std::numeric_limits<float>::infinity()));
  const float floatGapMiddle = 0.5f * (
      importedAdjacent[0].center[1] + importedLength * 0.5f +
      importedAdjacent[1].center[1] - importedLength * 0.5f);
  const std::array<float, 3> importedCamera = {0.0f, floatGapMiddle + 0.1f, 0.0f};
  const std::array<float, 3> importedDirection = {0.0f, -0.1f, 2.0f - importedLength * 0.5f};
  fixture = makeSinglePixelResolveFixture(
      importedCamera,
      importedDirection,
      {identityInstance},
      std::vector<SparseVoxelCenterLengthGPU>(importedAdjacent.begin(), importedAdjacent.end()),
      {{0u, 0u}, {0u, 1u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 1u);
  assert((expected.surfaces[0].packedLocalFace & 3u) == 2u);
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // Bounding-box and quadrant filters are both mandatory before the exact test.
  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {identityInstance}, {{{0.0f, 0.0f, 2.0f}, 1.0f}}, {{0u, 0u}});
  fixture.projected[0].packedInclusivePixelBounds = {ref::packPixel(1u, 1u), ref::packPixel(1u, 1u)};
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 0u);
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);
  fixture.projected[0].packedInclusivePixelBounds = {ref::packPixel(0u, 0u), ref::packPixel(0u, 0u)};
  fixture.candidates[0].quadrant ^= 1u;
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 0u);
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {identityInstance}, {unitBox}, {});
  expected = ref::resolveSolidVoxels(fixture);
  assert(expected.coverage[0] == 0u);
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // Keep world-space t under a translated, reflected-capable, non-uniform transform.
  SparseVoxelInstanceGPU transformed = identityInstance;
  transformed.modelToWorld[0] = 2.0f;
  transformed.modelToWorld[5] = 1.0f;
  transformed.modelToWorld[10] = 0.5f;
  transformed.modelToWorld[14] = 2.0f;
  transformed.worldToModel[0] = 0.5f;
  transformed.worldToModel[5] = 1.0f;
  transformed.worldToModel[10] = 2.0f;
  transformed.worldToModel[14] = -4.0f;
  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {transformed}, {unitBox}, {{0u, 0u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(nearFloat(expected.surfaces[0].surfaceT, 1.75f));
  assert(expected.surfaces[0].packedLocalFace == ref::packLocalFace(2u, false));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  transformed.modelToWorld[10] = -0.5f;
  transformed.worldToModel[10] = -2.0f;
  transformed.worldToModel[14] = 4.0f;
  fixture = makeSinglePixelResolveFixture({0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {transformed}, {unitBox}, {{0u, 0u}});
  expected = ref::resolveSolidVoxels(fixture);
  assert(nearFloat(expected.surfaces[0].surfaceT, 1.75f));
  assert(expected.surfaces[0].packedLocalFace == ref::packLocalFace(2u, true));
  assertResolveEqual(runGpu(rhi, inputFromResolveFixture(fixture)), expected);

  // Chained G1-G8 shell: every accepted intermediate and every pixel winner is checked.
  const BootstrapSparseVoxelModel shell = generateExteriorShell128();
  ref::ProjectionFixture shellProjection{};
  shellProjection.uniforms.worldToCamera = identityMatrix();
  shellProjection.uniforms.worldToCamera[14] = 4.0f;
  shellProjection.uniforms.cameraToWorld = identityMatrix();
  shellProjection.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, -4.0f, 0.1f};
  shellProjection.uniforms.focalPrincipal = {64.0f, 64.0f, 64.0f, 64.0f};
  setViewport(shellProjection.uniforms, 128u, 128u);
  shellProjection.uniforms.visibleVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.projectedVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.candidateCapacity = 1000000u;
  shellProjection.instances.push_back(shell.instance);
  shellProjection.visibleInstances.push_back({0u, 0u, kExteriorShellVoxelCount, 0u});
  shellProjection.centersLengths = shell.centersLengths;
  shellProjection.pathsLevels = shell.pathsLevels;
  const std::vector<ProjectedVoxelGPU> shellProjected = ref::preprocess(shellProjection);
  const ref::ScanResult shellScan = ref::exclusiveScan(shellProjected, shellProjection.uniforms.candidateCapacity);
  assert(!shellScan.overflow);
  std::vector<TileCandidateGPU> shellCandidates = ref::emit(shellProjected, shellScan.offsets, shellProjection.pathsLevels, shellProjection.uniforms);
  ref::stableSort(shellCandidates);
  const std::vector<SparseVoxelTileRangeGPU> shellRanges = ref::buildRanges(shellCandidates, shellProjection.uniforms.tileRangeCapacity);
  const ref::ResolveFixture shellResolve{
    shellProjection.uniforms,
    shellProjection.instances,
    shellProjection.centersLengths,
    shellProjected,
    shellCandidates,
    shellRanges,
  };
  const ref::ResolveResult shellExpected = ref::resolveSolidVoxels(shellResolve);
  GpuInput shellInput = inputFromFixture(shellProjection);
  shellInput.preprocess = shellInput.scan = shellInput.emit = shellInput.sort = shellInput.ranges = shellInput.resolve = true;
  GpuOutput shellGpu = runGpu(rhi, std::move(shellInput));
  shellGpu.projected.resize(shellProjected.size());
  shellGpu.offsets.resize(shellScan.offsets.size());
  shellGpu.candidates.resize(shellCandidates.size());
  assert(projectedEqual(shellGpu.projected, shellProjected));
  assert(shellGpu.offsets == shellScan.offsets);
  assertCandidatesEqual(shellGpu.candidates, shellCandidates);
  assert(equalBytes(shellGpu.ranges, shellRanges));
  assertResolveEqual(shellGpu, shellExpected);
  const uint32_t coveredPixels = static_cast<uint32_t>(std::count(shellGpu.coverage.begin(), shellGpu.coverage.end(), 1u));
  assert(coveredPixels > 0u && coveredPixels < shellGpu.coverage.size());
}

void testG9(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  instance.previousModelToWorld = identityMatrix();
  instance.previousModelToWorld[12] = -0.25f;
  // Sheared inverse transform proves the inverse-transpose normal path rather
  // than a model-matrix direction transform.
  instance.worldToModel[0] = 0.5f;
  instance.worldToModel[4] = -0.5f;

  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff4080ffu;
  material.roughnessMetallicUNorm8 = 64u | (128u << 8u);
  material.emissiveRG16F = 0x38003c00u; // r=1, g=0.5
  material.emissiveB16F = 0x00004000u;  // b=2
  material.materialId = 123u;
  material.flags = 3u;

  ref::GBufferFixture fixture{};
  fixture.uniforms.worldToCamera = identityMatrix();
  fixture.uniforms.cameraToWorld = identityMatrix();
  fixture.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.1f};
  fixture.uniforms.focalPrincipal = {2.0f, 2.0f, 0.5f, 0.5f};
  setViewport(fixture.uniforms, 2u, 2u);
  fixture.uniforms.worldToClip = identityMatrix();
  fixture.uniforms.previousWorldToClip = identityMatrix();
  fixture.uniforms.gbufferFlags = kSparseVoxelGBufferHasPreviousFrame;
  fixture.instances = {instance};
  fixture.materials = {material};
  fixture.surfaces.resize(4u);
  for (ResolvedVoxelSurfaceGPU &surface : fixture.surfaces)
  {
    surface.instanceIndex = kInvalidSparseVoxelIndex;
    surface.voxelIndex = kInvalidSparseVoxelIndex;
  }
  fixture.surfaces[0] = {0.25f, 0u, 0u, ref::packLocalFace(0u, true), kSparseVoxelSurfaceHit, 0u, 0u, 0u};
  const std::vector<SparseVoxelGBufferPixelGPU> expected = ref::writeVoxelGBuffer(fixture);
  assert(expected[0].ids[0] == 0u && expected[0].ids[1] == 0u && expected[0].ids[2] == 123u);
  assert(
      nearFloat(expected[0].emissive[0], 0.0f) &&
      nearFloat(expected[0].emissive[1], 0.0f) &&
      nearFloat(expected[0].emissive[2], 0.0f));
  assert(nearFloat(expected[0].motionDepth[0], 0.125f));
  assert(expected[0].geometryNormal[0] > 0.7f && expected[0].geometryNormal[1] < -0.7f);

  GpuInput input{};
  input.uniforms = fixture.uniforms;
  input.instances = fixture.instances;
  input.materials = fixture.materials;
  input.inputResolved = fixture.surfaces;
  input.gbuffer = true;
  GpuOutput gpu = runGpu(rhi, input);
  assertGBufferEqual(gpu.gbuffer, expected);
  for (size_t pixel = 1u; pixel < gpu.gbuffer.size(); ++pixel)
    assert(equalBytes(
        std::vector<SparseVoxelGBufferPixelGPU>{gpu.gbuffer[pixel]},
        std::vector<SparseVoxelGBufferPixelGPU>{SparseVoxelGBufferPixelGPU{}}));

  // Camera translation along the view direction must generate radial motion
  // in top-left image coordinates. A missing clip-space Y flip makes the top
  // and bottom components point inward while X points outward, which appears
  // as a vertical squeeze/stretch during temporal reprojection.
  ref::GBufferFixture radialFixture{};
  radialFixture.uniforms.worldToCamera = identityMatrix();
  radialFixture.uniforms.cameraToWorld = identityMatrix();
  radialFixture.uniforms.cameraToWorld[5] = -1.0f;
  radialFixture.uniforms.cameraToWorld[10] = -1.0f;
  radialFixture.uniforms.cameraWorldPositionNear =
      {0.0f, 0.0f, 0.0f, 0.1f};
  radialFixture.uniforms.focalPrincipal = {1.0f, 1.0f, 1.0f, 1.0f};
  setViewport(radialFixture.uniforms, 2u, 2u);
  radialFixture.uniforms.worldToClip = {};
  radialFixture.uniforms.worldToClip[0] = 1.0f;
  radialFixture.uniforms.worldToClip[5] = 1.0f;
  radialFixture.uniforms.worldToClip[11] = -1.0f;
  radialFixture.uniforms.previousWorldToClip =
      radialFixture.uniforms.worldToClip;
  radialFixture.uniforms.previousWorldToClip[15] = 0.5f;
  radialFixture.uniforms.gbufferFlags =
      kSparseVoxelGBufferHasPreviousFrame;
  radialFixture.instances = {SparseVoxelInstanceGPU{}};
  radialFixture.instances[0].modelToWorld = identityMatrix();
  radialFixture.instances[0].worldToModel = identityMatrix();
  radialFixture.instances[0].previousModelToWorld = identityMatrix();
  radialFixture.materials = {material};
  radialFixture.surfaces.resize(4u);
  for (ResolvedVoxelSurfaceGPU &surface : radialFixture.surfaces)
    surface = {
      2.0f, 0u, 0u, ref::packLocalFace(2u, true),
      kSparseVoxelSurfaceHit, 0u, 0u, 0u,
    };

  const std::vector<SparseVoxelGBufferPixelGPU> radialExpected =
      ref::writeVoxelGBuffer(radialFixture);
  assert(radialExpected[0].motionDepth[0] < 0.0f);
  assert(radialExpected[0].motionDepth[1] < 0.0f);
  assert(radialExpected[1].motionDepth[0] > 0.0f);
  assert(radialExpected[1].motionDepth[1] < 0.0f);
  assert(radialExpected[2].motionDepth[0] < 0.0f);
  assert(radialExpected[2].motionDepth[1] > 0.0f);
  assert(radialExpected[3].motionDepth[0] > 0.0f);
  assert(radialExpected[3].motionDepth[1] > 0.0f);

  GpuInput radialInput{};
  radialInput.uniforms = radialFixture.uniforms;
  radialInput.instances = radialFixture.instances;
  radialInput.materials = radialFixture.materials;
  radialInput.inputResolved = radialFixture.surfaces;
  radialInput.gbuffer = true;
  const GpuOutput radialGpu = runGpu(rhi, radialInput);
  assertGBufferEqual(radialGpu.gbuffer, radialExpected);

  input.debugPresent = true;
  input.inputCoverage = {1u, 0u, 0u, 0u};
  input.inputLinearDepth = {0.25f, 0.0f, 0.0f, 0.0f};
  input.debugLinearDepthScale = 2.0f;
  for (uint32_t modeIndex = static_cast<uint32_t>(SparseVoxelDebugMode::BaseColor);
       modeIndex <= static_cast<uint32_t>(SparseVoxelDebugMode::VoxelId);
       ++modeIndex)
  {
    input.debugMode = static_cast<SparseVoxelDebugMode>(modeIndex);
    gpu = runGpu(rhi, input);
    const std::array<float, 4> debugExpected = expectedDebugColor(
        expected[0], input.debugMode, input.inputCoverage[0], input.inputLinearDepth[0], input.debugLinearDepthScale);
    for (uint32_t component = 0u; component < 4u; ++component)
    {
      const float tolerance = 2.0e-3f * std::max(1.0f, std::abs(debugExpected[component]));
      assert(std::abs(gpu.gbuffer[0].baseColor[component] - debugExpected[component]) <= tolerance);
      assert(std::abs(gpu.gbuffer[1].baseColor[component] - (component == 3u ? 1.0f : 0.0f)) <= tolerance);
    }
  }
  input.debugPresent = false;

  // Every local face and a disabled temporal history path get isolated checks.
  setViewport(fixture.uniforms, 6u, 1u);
  fixture.uniforms.focalPrincipal = {8.0f, 8.0f, 0.5f, 0.5f};
  fixture.uniforms.gbufferFlags = 0u;
  fixture.surfaces.resize(6u);
  for (uint32_t face = 0u; face < 6u; ++face)
    fixture.surfaces[face] = {0.2f, 0u, 0u, ref::packLocalFace(face / 2u, (face & 1u) != 0u), kSparseVoxelSurfaceHit, 0u, 0u, 0u};
  input.uniforms = fixture.uniforms;
  input.inputResolved = fixture.surfaces;
  gpu = runGpu(rhi, input);
  const std::vector<SparseVoxelGBufferPixelGPU> faceExpected = ref::writeVoxelGBuffer(fixture);
  assertGBufferEqual(gpu.gbuffer, faceExpected);
  for (const SparseVoxelGBufferPixelGPU &pixel : gpu.gbuffer)
    assert(pixel.motionDepth[0] == 0.0f && pixel.motionDepth[1] == 0.0f);

  // Exact heightfield micro-voxels retain their staircase in depth, identity,
  // and material selection, but the receiver normal follows the continuous
  // terrain plane. Otherwise every one-cell riser becomes a dark cliff and a
  // self-shadow origin, producing the contour artifacts seen at distance.
  constexpr uint32_t terrainWidth = 5u;
  constexpr uint32_t terrainHeight = 5u;
  SparseVoxelRasterUniformsGPU terrainUniforms{};
  terrainUniforms.cameraToWorld = identityMatrix();
  terrainUniforms.worldToCamera = identityMatrix();
  terrainUniforms.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.01f};
  terrainUniforms.focalPrincipal = {8.0f, 8.0f, 2.5f, 2.5f};
  terrainUniforms.worldToClip = identityMatrix();
  terrainUniforms.previousWorldToClip = identityMatrix();
  setViewport(terrainUniforms, terrainWidth, terrainHeight);
  std::vector<ResolvedVoxelSurfaceGPU> terrainSurfaces(
      terrainWidth * terrainHeight);
  constexpr float terrainNormalY = 0.4472135955f;
  constexpr float terrainNormalZ = -0.8944271910f;
  constexpr float terrainPlane = -0.2f;
  constexpr uint32_t terrainContext = 0x80u | 32u;
  for (uint32_t y = 0u; y < terrainHeight; ++y)
  {
    for (uint32_t x = 0u; x < terrainWidth; ++x)
    {
      const float cameraX =
          (static_cast<float>(x) + 0.5f - 2.5f) / 8.0f;
      const float cameraY =
          (static_cast<float>(y) + 0.5f - 2.5f) / 8.0f;
      const float inverseLength = 1.0f /
          std::sqrt(cameraX * cameraX + cameraY * cameraY + 1.0f);
      const float directionY = cameraY * inverseLength;
      const float directionZ = inverseLength;
      const float distance = terrainPlane /
          (terrainNormalY * directionY + terrainNormalZ * directionZ);
      terrainSurfaces[y * terrainWidth + x] = {
        distance,
        0u,
        y * terrainWidth + x,
        // Deliberately report a vertical staircase riser. The reconstructed
        // macro normal must replace this +X lighting normal.
        ref::packLocalFace(0u, true),
        kSparseVoxelSurfaceHit | kSparseVoxelSurfacePackedMaterial,
        0xff808080u,
        kSparseVoxelExactMicroMaterialFlag,
        terrainContext << kSparseVoxelResolvedContextShift,
      };
    }
  }
  GpuInput terrainInput{};
  terrainInput.uniforms = terrainUniforms;
  terrainInput.instances = {SparseVoxelInstanceGPU{}};
  terrainInput.instances[0].modelToWorld = identityMatrix();
  terrainInput.instances[0].worldToModel = identityMatrix();
  terrainInput.instances[0].previousModelToWorld = identityMatrix();
  terrainInput.inputResolved = std::move(terrainSurfaces);
  terrainInput.gbuffer = true;
  GpuInput parentTerrainInput = terrainInput;
  const GpuOutput terrainGpu = runGpu(rhi, std::move(terrainInput));
  const SparseVoxelGBufferPixelGPU &terrainCenter =
      terrainGpu.gbuffer[(terrainHeight / 2u) * terrainWidth +
          terrainWidth / 2u];
  assert(std::abs(terrainCenter.geometryNormal[0]) < 0.05f);
  assert(terrainCenter.geometryNormal[1] > 0.40f);
  assert(terrainCenter.geometryNormal[2] < -0.85f);
  assert(std::abs(terrainCenter.normal[0]) < 0.05f);
  assert(terrainCenter.normal[1] > 0.40f);
  assert(terrainCenter.normal[2] < -0.85f);

  // Parent-only generated terrain has no exact micro payload. Its voxel face
  // is the authored surface, so the terrain macro reconstruction must not
  // round or overlap a horizontal square through a neighbour-derived shading
  // normal. Vertical sides remain heightfield closures and are covered by the
  // legacy macro-normal assertion above.
  for (ResolvedVoxelSurfaceGPU &surface : parentTerrainInput.inputResolved)
  {
    surface.reserved1 = 0u;
    surface.packedLocalFace = ref::packLocalFace(1u, true);
  }
  const GpuOutput parentTerrainGpu =
      runGpu(rhi, std::move(parentTerrainInput));
  const SparseVoxelGBufferPixelGPU &parentTerrainCenter =
      parentTerrainGpu.gbuffer[(terrainHeight / 2u) * terrainWidth +
          terrainWidth / 2u];
  assert(std::abs(parentTerrainCenter.geometryNormal[0]) < 0.01f);
  assert(parentTerrainCenter.geometryNormal[1] > 0.99f);
  assert(std::abs(parentTerrainCenter.geometryNormal[2]) < 0.01f);
  assert(std::abs(parentTerrainCenter.normal[0]) < 0.01f);
  assert(parentTerrainCenter.normal[1] > 0.99f);
  assert(std::abs(parentTerrainCenter.normal[2]) < 0.01f);

  // Non-workgroup-aligned empty frames must clear every native attachment and
  // every storage-texture material channel without stale pixels.
  setViewport(fixture.uniforms, 17u, 9u);
  fixture.uniforms.gbufferClearDepth = 1.0f;
  fixture.surfaces.assign(17u * 9u, {});
  for (ResolvedVoxelSurfaceGPU &surface : fixture.surfaces)
  {
    surface.instanceIndex = kInvalidSparseVoxelIndex;
    surface.voxelIndex = kInvalidSparseVoxelIndex;
  }
  input.uniforms = fixture.uniforms;
  input.inputResolved = fixture.surfaces;
  gpu = runGpu(rhi, input);
  assertGBufferEqual(gpu.gbuffer, ref::writeVoxelGBuffer(fixture));

  // Full G1-G9 chain on the required 128^3 exterior shell.
  const BootstrapSparseVoxelModel shell = generateExteriorShell128();
  ref::ProjectionFixture shellProjection{};
  shellProjection.uniforms.worldToCamera = identityMatrix();
  shellProjection.uniforms.worldToCamera[14] = 4.0f;
  shellProjection.uniforms.cameraToWorld = identityMatrix();
  shellProjection.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, -4.0f, 0.1f};
  shellProjection.uniforms.focalPrincipal = {64.0f, 64.0f, 64.0f, 64.0f};
  setViewport(shellProjection.uniforms, 128u, 128u);
  shellProjection.uniforms.visibleVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.projectedVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.candidateCapacity = 1000000u;
  shellProjection.uniforms.worldToClip = {};
  shellProjection.uniforms.worldToClip[11] = 1.0f;
  shellProjection.uniforms.worldToClip[14] = 0.1f;
  shellProjection.uniforms.worldToClip[15] = 4.0f;
  shellProjection.uniforms.previousWorldToClip = shellProjection.uniforms.worldToClip;
  shellProjection.instances = {shell.instance};
  shellProjection.visibleInstances = {{0u, 0u, kExteriorShellVoxelCount, 0u}};
  shellProjection.centersLengths = shell.centersLengths;
  shellProjection.pathsLevels = shell.pathsLevels;
  const std::vector<ProjectedVoxelGPU> shellProjected = ref::preprocess(shellProjection);
  const ref::ScanResult shellScan = ref::exclusiveScan(shellProjected, shellProjection.uniforms.candidateCapacity);
  assert(!shellScan.overflow);
  std::vector<TileCandidateGPU> shellCandidates = ref::emit(shellProjected, shellScan.offsets, shellProjection.pathsLevels, shellProjection.uniforms);
  ref::stableSort(shellCandidates);
  const std::vector<SparseVoxelTileRangeGPU> shellRanges = ref::buildRanges(shellCandidates, shellProjection.uniforms.tileRangeCapacity);
  const ref::ResolveFixture shellResolve{
    shellProjection.uniforms,
    shellProjection.instances,
    shellProjection.centersLengths,
    shellProjected,
    shellCandidates,
    shellRanges,
  };
  const ref::ResolveResult shellResolved = ref::resolveSolidVoxels(shellResolve);
  const ref::GBufferFixture shellGBuffer{
    shellProjection.uniforms,
    shellProjection.instances,
    shell.materials,
    shellResolved.surfaces,
  };
  const std::vector<SparseVoxelGBufferPixelGPU> shellExpected = ref::writeVoxelGBuffer(shellGBuffer);
  input = inputFromFixture(shellProjection);
  input.materials = shell.materials;
  input.preprocess = input.scan = input.emit = input.sort = input.ranges = input.resolve = input.gbuffer = true;
  gpu = runGpu(rhi, std::move(input));
  assertGBufferEqual(gpu.gbuffer, shellExpected);
  const size_t hitCount = static_cast<size_t>(std::count_if(
      gpu.gbuffer.begin(), gpu.gbuffer.end(), [](const SparseVoxelGBufferPixelGPU &pixel) { return pixel.ids[2] != 0u; }));
  assert(hitCount > 0u && hitCount < gpu.gbuffer.size());
}

void assertDepthParity(const GpuOutput &gpu)
{
  assert(gpu.depthOnly.size() == gpu.gbuffer.size());
  for (size_t pixel = 0u; pixel < gpu.depthOnly.size(); ++pixel)
  {
    assert(std::isfinite(gpu.depthOnly[pixel]));
    assert(gpu.depthOnly[pixel] == gpu.gbuffer[pixel].motionDepth[2]);
  }
}

std::vector<float> runDepthLoadFixture(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const ::rendering::ComparisonOp comparison)
{
  SparseVoxelRasterUniformsGPU uniforms{};
  uniforms.cameraToWorld = identityMatrix();
  uniforms.worldToClip = identityMatrix();
  uniforms.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.01f};
  uniforms.focalPrincipal = {1000.0f, 1000.0f, 0.5f, 0.5f};
  setViewport(uniforms, 2u, 1u);
  uniforms.gbufferClearDepth = comparison == ::rendering::ComparisonOp::ComparisonOp_LessOrEqual ? 1.0f : 0.0f;

  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  std::vector<ResolvedVoxelSurfaceGPU> first(2u);
  std::vector<ResolvedVoxelSurfaceGPU> second(2u);
  for (uint32_t pixel = 0u; pixel < 2u; ++pixel)
  {
    first[pixel] = {0.5f, 0u, 0u, ref::packLocalFace(2u, false), kSparseVoxelSurfaceHit, 0u, 0u, 0u};
    second[pixel] = {pixel == 0u ? 0.4f : 0.6f, 0u, 0u, ref::packLocalFace(2u, false), kSparseVoxelSurfaceHit, 0u, 0u, 0u};
  }

  RenderGraph graph(&rhi);
  const auto storage = [&](const char *name, const uint64_t size)
  {
    return graph.createBuffer(
        {.name = name,
         .size = size,
         .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc});
  };
  const Buffer instances = storage("svr.g19.load.instances", sizeof(instance));
  const Buffer firstResolved = storage("svr.g19.load.first", first.size() * sizeof(first[0]));
  const Buffer secondResolved = storage("svr.g19.load.second", second.size() * sizeof(second[0]));
  const Buffer depthPixels = storage("svr.g19.load.depthPixels", 2u * sizeof(float));
  const Buffer readback = graph.createBuffer(
      {.name = "svr.g19.load.readback", .size = 2u * sizeof(float), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  graph.bufferWrite(instances, 0u, sizeof(instance), &instance);
  writeVector(graph, firstResolved, first);
  writeVector(graph, secondResolved, second);
  const Texture depth = graph.createTexture(
      {.name = "svr.g19.load.depth",
       .format = ::rendering::Format::Format_Depth32Float,
       .height = 1u,
       .width = 2u,
       .memoryProperties = BufferUsage::BufferUsage_None,
       .mipLevels = 1u,
       .usage = ::rendering::ImageUsage::ImageUsage_DepthStencilAttachment | ::rendering::ImageUsage::ImageUsage_Sampled});

  graph.registerPass<svrRendering::SparseVoxelDepthPass>(
      "svr.g19.DepthClear",
      0u,
      svrRendering::SparseVoxelDepthPass::Buffers{instances, firstResolved},
      depth,
      svrRendering::SparseVoxelDepthPass::Settings{
        uniforms, 1u, uniforms.gbufferClearDepth, ::rendering::LoadOp::LoadOp_Clear,
        ::rendering::StoreOp::StoreOp_Store, ::rendering::ComparisonOp::ComparisonOp_Always});
  graph.registerPass<svrRendering::SparseVoxelDepthPass>(
      "svr.g19.DepthLoad",
      1u,
      svrRendering::SparseVoxelDepthPass::Buffers{instances, secondResolved},
      depth,
      svrRendering::SparseVoxelDepthPass::Settings{
        uniforms, 1u, uniforms.gbufferClearDepth, ::rendering::LoadOp::LoadOp_Load,
        ::rendering::StoreOp::StoreOp_Store, comparison});
  graph.registerPass<svrRendering::SparseVoxelDepthReadbackPass>("svr.g19.DepthReadback", 2u, depth, depthPixels, uniforms);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "svr.g19.CopyDepth", 3u, depthPixels, 0u, 2u * sizeof(float), readback, 0u, 2u * sizeof(float));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);
  std::vector<float> output(2u);
  readVector(graph, readback, output);
  return output;
}

void testG19(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  SparseVoxelInstanceGPU instance{};
  instance.modelToWorld = identityMatrix();
  instance.worldToModel = identityMatrix();
  instance.previousModelToWorld = identityMatrix();

  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff80c040u;
  material.roughnessMetallicUNorm8 = 192u;
  material.materialId = 77u;

  GpuInput input{};
  input.uniforms.worldToCamera = identityMatrix();
  input.uniforms.cameraToWorld = identityMatrix();
  input.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, 0.0f, 0.05f};
  input.uniforms.focalPrincipal = {8.0f, 8.0f, 0.5f, 0.5f};
  setViewport(input.uniforms, 7u, 3u);
  input.uniforms.worldToClip = identityMatrix();
  input.uniforms.previousWorldToClip = identityMatrix();
  input.instances = {instance};
  input.materials = {material};
  input.inputResolved.assign(21u, {});
  for (ResolvedVoxelSurfaceGPU &surface : input.inputResolved)
  {
    surface.instanceIndex = kInvalidSparseVoxelIndex;
    surface.voxelIndex = kInvalidSparseVoxelIndex;
  }
  for (uint32_t face = 0u; face < 6u; ++face)
    input.inputResolved[face] = {0.2f + 0.01f * static_cast<float>(face), 0u, 0u, ref::packLocalFace(face / 2u, (face & 1u) != 0u), kSparseVoxelSurfaceHit, 0u, 0u, 0u};
  input.gbuffer = true;
  input.depthOnly = true;

  // Forward-Z clear, every local face, odd viewport, and empty pixels.
  input.uniforms.gbufferClearDepth = 1.0f;
  GpuOutput gpu = runGpu(rhi, input);
  assertDepthParity(gpu);
  for (size_t pixel = 0u; pixel < input.inputResolved.size(); ++pixel)
  {
    const bool hit = (input.inputResolved[pixel].hitFlags & kSparseVoxelSurfaceHit) != 0u;
    assert((gpu.gbuffer[pixel].ids[2] == material.materialId) == hit);
    if (hit)
    {
      assert(gpu.gbuffer[pixel].ids[0] == input.inputResolved[pixel].instanceIndex);
      assert(gpu.gbuffer[pixel].ids[1] == input.inputResolved[pixel].voxelIndex);
    }
    else
    {
      assert(gpu.depthOnly[pixel] == 1.0f);
    }
  }

  // Reverse-Z clear uses the same resolved contract and exact device depth.
  input.uniforms.gbufferClearDepth = 0.0f;
  gpu = runGpu(rhi, input);
  assertDepthParity(gpu);
  for (size_t pixel = 6u; pixel < gpu.depthOnly.size(); ++pixel)
    assert(gpu.depthOnly[pixel] == 0.0f);

  // Native attachment Load and both depth conventions retain the first pass
  // wherever the second pass fails its comparison.
  const std::vector<float> forwardLoad = runDepthLoadFixture(rhi, ::rendering::ComparisonOp::ComparisonOp_LessOrEqual);
  const std::vector<float> reverseLoad = runDepthLoadFixture(rhi, ::rendering::ComparisonOp::ComparisonOp_GreaterOrEqual);
  assert(forwardLoad[0] < forwardLoad[1]);
  assert(reverseLoad[0] < reverseLoad[1]);
  assert(nearFloat(forwardLoad[1], reverseLoad[0]));

  // Full passes 0-7 plus both terminal writers on the required 128^3 shell.
  const BootstrapSparseVoxelModel shell = generateExteriorShell128();
  ref::ProjectionFixture shellProjection{};
  shellProjection.uniforms.worldToCamera = identityMatrix();
  shellProjection.uniforms.worldToCamera[14] = 4.0f;
  shellProjection.uniforms.cameraToWorld = identityMatrix();
  shellProjection.uniforms.cameraWorldPositionNear = {0.0f, 0.0f, -4.0f, 0.1f};
  shellProjection.uniforms.focalPrincipal = {64.0f, 64.0f, 64.0f, 64.0f};
  setViewport(shellProjection.uniforms, 128u, 128u);
  shellProjection.uniforms.visibleVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.projectedVoxelCapacity = kExteriorShellVoxelCount;
  shellProjection.uniforms.candidateCapacity = 1000000u;
  shellProjection.uniforms.worldToClip = {};
  shellProjection.uniforms.worldToClip[11] = 1.0f;
  shellProjection.uniforms.worldToClip[14] = 0.1f;
  shellProjection.uniforms.worldToClip[15] = 4.0f;
  shellProjection.uniforms.previousWorldToClip = shellProjection.uniforms.worldToClip;
  shellProjection.instances = {shell.instance};
  shellProjection.visibleInstances = {{0u, 0u, kExteriorShellVoxelCount, 0u}};
  shellProjection.centersLengths = shell.centersLengths;
  shellProjection.pathsLevels = shell.pathsLevels;

  input = inputFromFixture(shellProjection);
  input.materials = shell.materials;
  input.preprocess = input.scan = input.emit = input.sort = input.ranges = input.resolve = true;
  input.gbuffer = input.depthOnly = true;
  gpu = runGpu(rhi, std::move(input));
  assertDepthParity(gpu);
  assert(gpu.resolved.size() == gpu.coverage.size());
  for (size_t pixel = 0u; pixel < gpu.coverage.size(); ++pixel)
  {
    const bool hit = gpu.coverage[pixel] != 0u;
    assert(((gpu.resolved[pixel].hitFlags & kSparseVoxelSurfaceHit) != 0u) == hit);
    if (hit)
    {
      assert(gpu.gbuffer[pixel].ids[0] == gpu.resolved[pixel].instanceIndex);
      assert(gpu.gbuffer[pixel].ids[1] == gpu.resolved[pixel].voxelIndex);
      assert(std::isfinite(gpu.linearDepth[pixel]) && gpu.linearDepth[pixel] > 0.0f);
    }
  }
}

} // namespace

int main(int argc, char **argv)
{
  os::Logger::start();
  assert(argc == 2);
  ::rendering::DeviceRequiredLimits limits{.minimumMemory = 0u, .minimumComputeSharedMemory = 10u * 1024u, .minimumComputeWorkGroupInvocations = 256u};
  ::rendering::backend::vulkan::VulkanRHI rhi(::rendering::backend::vulkan::Vulkan_1_2, limits, ::rendering::DeviceFeatures::DeviceFeatures_Compute, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  const std::string gate = argv[1];
  if (gate == "--g3")
    testG3(rhi);
  else if (gate == "--g4")
    testG4(rhi);
  else if (gate == "--g5")
    testG5(rhi);
  else if (gate == "--g6")
    testG6(rhi);
  else if (gate == "--g7")
    testG7(rhi);
  else if (gate == "--g8")
  {
    testG8(rhi);
  }
  else if (gate == "--g9")
    testG9(rhi);
  else
  {
    assert(gate == "--g19");
    testG19(rhi);
  }
  os::Logger::shutdown();
  return 0;
}
