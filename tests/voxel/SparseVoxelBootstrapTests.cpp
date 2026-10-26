#include "os/Logger.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelSceneManager.hpp"
#include "voxel/svraster/rendering/SparseVoxelBootstrapPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelLODPass.hpp"
#include "voxel/svraster/rendering/SparseVoxelTLASCullingPass.hpp"

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
namespace svrRendering = voxel::svraster::rendering;

uint64_t pathOf(const SparseVoxelPathLevelGPU &pathLevel)
{
  return static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
}

void testAbiLayout()
{
  assert(sizeof(SparseVoxelBLASGPU) == 32u);
  assert(sizeof(SparseVoxelPathLevelGPU) == 16u);
  assert(sizeof(SparseVoxelCenterLengthGPU) == 16u);
  assert(sizeof(SparseVoxelMaterialGPU) == 32u);
  assert(sizeof(SparseVoxelInstanceGPU) == 240u);
  assert(sizeof(SparseVoxelTLASNodeGPU) == 48u);
  assert(sizeof(TLASQueueElementGPU) == 16u);
  assert(sizeof(VisibleInstanceGPU) == 16u);
  assert(sizeof(VisibleVoxelGPU) == 16u);
  assert(sizeof(SparseVoxelLODNodeGPU) == 16u);
  assert(sizeof(SparseVoxelResidentNodeMetadataGPU) == 16u);
  assert(sizeof(SparseVoxelLODQueueElementGPU) == 16u);
  assert(sizeof(SparseVoxelLODRecoveryStateGPU) == 16u);
  assert(sizeof(SparseVoxelLODSeedGPU) == 16u);
  assert(sizeof(SparseVoxelPageRequestGPU) == 16u);
  assert(sizeof(SparseVoxelPhysicalPageRewriteGPU) == 16u);
  assert(sizeof(SparseVoxelPageBoundsGPU) == 32u);
  assert(sizeof(ProjectedVoxelGPU) == 32u);
  assert(sizeof(TileCandidateGPU) == 32u);
  assert(sizeof(ResolvedVoxelSurfaceGPU) == 32u);
  assert(sizeof(SparseVoxelGBufferPixelGPU) == 128u);
  assert(sizeof(SparseVoxelExactOutputPixelGPU) == 192u);
  assert(sizeof(SparseVoxelDebugPresentUniformsGPU) == 96u);
  assert(sizeof(IndirectDispatchArgsGPU) == 16u);
  assert(sizeof(SparseVoxelFrameCountersGPU) == 32u);
  assert(sizeof(SparseVoxelBootstrapUniformsGPU) == 16u);
  assert(sizeof(SparseVoxelLODPassUniformsGPU) == 96u);
  assert(sizeof(SparseVoxelStreamingPassUniformsGPU) == 16u);
  assert(sizeof(SparseVoxelRasterUniformsGPU) == 336u);

  assert(offsetof(SparseVoxelInstanceGPU, worldToModel) == 64u);
  assert(offsetof(SparseVoxelInstanceGPU, previousModelToWorld) == 128u);
  assert(offsetof(SparseVoxelInstanceGPU, worldBoundsMin) == 192u);
  assert(offsetof(SparseVoxelInstanceGPU, worldBoundsMax) == 208u);
  assert(offsetof(SparseVoxelInstanceGPU, blasIndex) == 224u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, cameraToWorld) == 64u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, cameraWorldPositionNear) == 128u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, worldToClip) == 192u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, previousWorldToClip) == 256u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, gbufferFlags) == 320u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, gbufferClearDepth) == 324u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, lodPixelThreshold) == 328u);
  assert(offsetof(SparseVoxelRasterUniformsGPU, lodEnabled) == 332u);
}

void testOctreePathGoldensAndRoundTrips()
{
  assert(encodeOctreePath(0u, 0u, 0u, 7u) == 0u);
  assert(encodeOctreePath(1u, 0u, 0u, 7u) == 0x0000000020000000ull);
  assert(encodeOctreePath(127u, 127u, 127u, 7u) == 0x0000fffff8000000ull);
  assert(encodeOctreePath(128u, 0u, 0u, 7u) == kInvalidOctreePath);
  assert(!isCanonicalOctreePath(1u, 7u));
  assert(!isCanonicalOctreePath(uint64_t{1} << 48u, 7u));

  for (uint32_t level = 1u; level <= kMaxOctreeLevels; ++level)
  {
    const uint32_t limit = (uint32_t{1} << level) - 1u;
    const std::array<std::array<uint32_t, 3>, 4> coordinates = {
      std::array<uint32_t, 3>{0u, 0u, 0u},
      std::array<uint32_t, 3>{limit, limit, limit},
      std::array<uint32_t, 3>{limit, 0u, limit / 2u},
      std::array<uint32_t, 3>{limit / 3u, limit / 2u, limit},
    };
    for (const auto &coordinate : coordinates)
    {
      const uint64_t path = encodeOctreePath(coordinate[0], coordinate[1], coordinate[2], level);
      assert(isCanonicalOctreePath(path, level));
      assert(decodeOctreePath(path, level) == coordinate);
    }
  }
}

void assertMaterial(const SparseVoxelMaterialGPU &material, const uint32_t albedo, const uint32_t properties, const uint32_t emissiveRG, const uint32_t emissiveB, const uint32_t materialId)
{
  assert(material.albedoRGBA8 == albedo);
  assert(material.roughnessMetallicUNorm8 == properties);
  assert(material.emissiveRG16F == emissiveRG);
  assert(material.emissiveB16F == emissiveB);
  assert(material.materialId == materialId);
  assert(material.flags == 0u);
  assert(material.reserved0 == 0u);
  assert(material.reserved1 == 0u);
}

void testHashMaterialAndHalfGoldens()
{
  assert(splitmix64(0u) == 0xe220a8397b1dcdafull);
  assert(splitmix64(1u) == 0x910a2dec89025cc1ull);

  assert(floatToHalfRoundToNearestEven(0.0f) == 0x0000u);
  assert(floatToHalfRoundToNearestEven(1.0f) == 0x3c00u);
  assert(floatToHalfRoundToNearestEven(4.0f) == 0x4400u);
  assert(halfToFloat(0x3c00u) == 1.0f);
  assert(halfToFloat(0x4400u) == 4.0f);

  assertMaterial(generateExteriorShellMaterial(0u, kDefaultExteriorShellMaterialSeed), 0xff2fe4c2u, 0x00007ed0u, 0u, 0u, 0x0000b94au);
  assertMaterial(generateExteriorShellMaterial(0x0000000020000000ull, kDefaultExteriorShellMaterialSeed), 0xffb2286fu, 0x0000c86au, 0u, 0u, 0x0000acb3u);
  assertMaterial(generateExteriorShellMaterial(0x0000000030000000ull, kDefaultExteriorShellMaterialSeed), 0xffaf44e1u, 0x0000c381u, 0x41a03b00u, 0x000040e0u, 0x00006adcu);
  assertMaterial(generateExteriorShellMaterial(0x0000fffff8000000ull, kDefaultExteriorShellMaterialSeed), 0xffefe897u, 0x0000a763u, 0u, 0u, 0x00000ac9u);
}

void testExteriorShellGeneration()
{
  const BootstrapSparseVoxelModel model = generateExteriorShell128();
  assert(model.pathsLevels.size() == kExteriorShellVoxelCount);
  assert(model.centersLengths.size() == kExteriorShellVoxelCount);
  assert(model.materials.size() == kExteriorShellVoxelCount);
  assert(model.blas.voxelCount == kExteriorShellVoxelCount);
  assert(model.blas.maxLevel == kExteriorShellLevel);
  assert(model.blas.rootCenterExtent == (std::array<float, 4>{0.0f, 0.0f, 0.0f, 2.0f}));

  std::vector<uint8_t> occupancy(static_cast<size_t>(kExteriorShellResolution) * kExteriorShellResolution * kExteriorShellResolution, 0u);
  uint64_t previousPath = 0u;
  size_t emissiveCount = 0u;
  uint32_t minimumRoughness = 255u;
  uint32_t maximumRoughness = 0u;
  uint32_t minimumMetallic = 255u;
  uint32_t maximumMetallic = 0u;
  for (size_t voxelIndex = 0u; voxelIndex < model.pathsLevels.size(); ++voxelIndex)
  {
    const SparseVoxelPathLevelGPU &pathLevel = model.pathsLevels[voxelIndex];
    assert(pathLevel.level == kExteriorShellLevel);
    assert(pathLevel.flags == 0u);
    const uint64_t path = pathOf(pathLevel);
    assert(isCanonicalOctreePath(path, pathLevel.level));
    if (voxelIndex > 0u)
      assert(path > previousPath);
    previousPath = path;

    const std::array<uint32_t, 3> coordinate = decodeOctreePath(path, pathLevel.level);
    assert(coordinate[0] < kExteriorShellResolution);
    assert(coordinate[1] < kExteriorShellResolution);
    assert(coordinate[2] < kExteriorShellResolution);
    assert(
        coordinate[0] == 0u || coordinate[0] + 1u == kExteriorShellResolution || coordinate[1] == 0u || coordinate[1] + 1u == kExteriorShellResolution || coordinate[2] == 0u ||
        coordinate[2] + 1u == kExteriorShellResolution);
    const size_t linear = (static_cast<size_t>(coordinate[2]) * kExteriorShellResolution + coordinate[1]) * kExteriorShellResolution + coordinate[0];
    assert(occupancy[linear] == 0u);
    occupancy[linear] = 1u;

    const SparseVoxelCenterLengthGPU &centerLength = model.centersLengths[voxelIndex];
    constexpr float expectedLength = 2.0f / 128.0f;
    assert(centerLength.length == expectedLength);
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      assert(centerLength.center[axis] == -1.0f + (static_cast<float>(coordinate[axis]) + 0.5f) * expectedLength);

    const SparseVoxelMaterialGPU &material = model.materials[voxelIndex];
    assert((material.albedoRGBA8 >> 24u) == 0xffu);
    for (uint32_t shift : {0u, 8u, 16u})
      assert(((material.albedoRGBA8 >> shift) & 0xffu) >= 32u);
    const uint32_t roughness = material.roughnessMetallicUNorm8 & 0xffu;
    const uint32_t metallic = (material.roughnessMetallicUNorm8 >> 8u) & 0xffu;
    minimumRoughness = std::min(minimumRoughness, roughness);
    maximumRoughness = std::max(maximumRoughness, roughness);
    minimumMetallic = std::min(minimumMetallic, metallic);
    maximumMetallic = std::max(maximumMetallic, metallic);
    if (material.emissiveRG16F != 0u || material.emissiveB16F != 0u)
      ++emissiveCount;
  }

  for (uint32_t z = 0u; z < kExteriorShellResolution; ++z)
  {
    for (uint32_t y = 0u; y < kExteriorShellResolution; ++y)
    {
      for (uint32_t x = 0u; x < kExteriorShellResolution; ++x)
      {
        const bool expected = x == 0u || x + 1u == kExteriorShellResolution || y == 0u || y + 1u == kExteriorShellResolution || z == 0u || z + 1u == kExteriorShellResolution;
        const size_t linear = (static_cast<size_t>(z) * kExteriorShellResolution + y) * kExteriorShellResolution + x;
        assert((occupancy[linear] != 0u) == expected);
      }
    }
  }

  assert(emissiveCount > 0u && emissiveCount < model.materials.size());
  assert(minimumRoughness == 20u);
  assert(maximumRoughness == 255u);
  assert(minimumMetallic == 0u);
  assert(maximumMetallic == 255u);

  std::string error;
  assert(validateBootstrapSparseVoxelModel(model, &error));
  assert(error.empty());
}

template <typename T> bool bytesEqual(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

void testDeterminismAndValidationFailures()
{
  const BootstrapSparseVoxelModel first = generateExteriorShell128();
  const BootstrapSparseVoxelModel second = generateExteriorShell128();
  assert(std::memcmp(&first.blas, &second.blas, sizeof(first.blas)) == 0);
  assert(bytesEqual(first.pathsLevels, second.pathsLevels));
  assert(bytesEqual(first.centersLengths, second.centersLengths));
  assert(bytesEqual(first.materials, second.materials));
  assert(std::memcmp(&first.instance, &second.instance, sizeof(first.instance)) == 0);

  const BootstrapSparseVoxelModel otherSeed = generateExteriorShell128(kDefaultExteriorShellMaterialSeed + 1u);
  assert(bytesEqual(first.pathsLevels, otherSeed.pathsLevels));
  assert(bytesEqual(first.centersLengths, otherSeed.centersLengths));
  size_t changedMaterials = 0u;
  for (size_t index = 0u; index < first.materials.size(); ++index)
  {
    if (std::memcmp(&first.materials[index], &otherSeed.materials[index], sizeof(SparseVoxelMaterialGPU)) != 0)
      ++changedMaterials;
  }
  assert(changedMaterials > first.materials.size() * 99u / 100u);

  std::string error;
  BootstrapSparseVoxelModel invalid = first;
  invalid.materials.pop_back();
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));
  assert(!error.empty());

  invalid = first;
  invalid.pathsLevels[1] = invalid.pathsLevels[0];
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));

  invalid = first;
  invalid.pathsLevels[0].pathLo = 1u;
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));

  invalid = first;
  invalid.materials[0].albedoRGBA8 &= 0x00ffffffu;
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));

  invalid = first;
  invalid.materials[0].emissiveB16F = 0x00007c00u;
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));

  invalid = first;
  invalid.materials[0].flags = 1u;
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));

  invalid = first;
  invalid.pathsLevels[0] = SparseVoxelPathLevelGPU{0u, 0u, 1u, 0u};
  invalid.centersLengths[0] = SparseVoxelCenterLengthGPU{{-0.5f, -0.5f, -0.5f}, 1.0f};
  assert(!validateBootstrapSparseVoxelModel(invalid, &error));
}

uint32_t packedByte(const uint32_t value, const uint32_t shift)
{
  return (value >> shift) & 0xffu;
}

void testLODHierarchyAndAveragedMaterials()
{
  const BootstrapSparseVoxelModel leaves = generateExteriorShell128();
  const BootstrapSparseVoxelLOD hierarchy = generateExteriorShellLOD128();
  std::string error;
  assert(validateBootstrapSparseVoxelLOD(hierarchy, &error));
  assert(error.empty());
  assert(hierarchy.rootCount == 8u);
  assert(hierarchy.nodes.size() > leaves.pathsLevels.size());

  uint32_t leafBegin = 0u;
  while (leafBegin < hierarchy.nodes.size() && hierarchy.nodes[leafBegin].level != kExteriorShellLevel)
    ++leafBegin;
  assert(hierarchy.nodes.size() - leafBegin == kExteriorShellVoxelCount);
  assert(std::memcmp(hierarchy.pathsLevels.data() + leafBegin, leaves.pathsLevels.data(), leaves.pathsLevels.size() * sizeof(SparseVoxelPathLevelGPU)) == 0);
  assert(std::memcmp(hierarchy.materials.data() + leafBegin, leaves.materials.data(), leaves.materials.size() * sizeof(SparseVoxelMaterialGPU)) == 0);

  for (uint32_t parentIndex = 0u; parentIndex < leafBegin; ++parentIndex)
  {
    const SparseVoxelLODNodeGPU &parent = hierarchy.nodes[parentIndex];
    assert(parent.childCount > 0u && parent.childCount <= 8u);
    std::array<uint64_t, 2> propertySums{};
    std::array<double, 3> emission{};
    std::vector<SparseVoxelMaterialGPU> childMaterials;
    for (uint32_t childOffset = 0u; childOffset < parent.childCount; ++childOffset)
    {
      const uint32_t childIndex = parent.firstChild + childOffset;
      assert(hierarchy.nodes[childIndex].level == parent.level + 1u);
      const SparseVoxelMaterialGPU &child = hierarchy.materials[childIndex];
      childMaterials.push_back(child);
      propertySums[0] += packedByte(child.roughnessMetallicUNorm8, 0u);
      propertySums[1] += packedByte(child.roughnessMetallicUNorm8, 8u);
      emission[0] += halfToFloat(static_cast<uint16_t>(child.emissiveRG16F));
      emission[1] += halfToFloat(static_cast<uint16_t>(child.emissiveRG16F >> 16u));
      emission[2] += halfToFloat(static_cast<uint16_t>(child.emissiveB16F));
    }
    const SparseVoxelMaterialGPU &material = hierarchy.materials[parentIndex];
    const SparseVoxelMaterialGPU expected = reduceSparseVoxelLODMaterials(childMaterials, material.materialId);
    const auto averageByte = [&](const uint64_t sum) { return static_cast<uint32_t>((sum + parent.childCount / 2u) / parent.childCount); };
    assert(material.albedoRGBA8 == expected.albedoRGBA8);
    assert(packedByte(material.roughnessMetallicUNorm8, 0u) == averageByte(propertySums[0]));
    assert(packedByte(material.roughnessMetallicUNorm8, 8u) == averageByte(propertySums[1]));
    assert(std::abs(halfToFloat(static_cast<uint16_t>(material.emissiveRG16F)) - static_cast<float>(emission[0] / parent.childCount)) < 0.01f);
    assert(std::abs(halfToFloat(static_cast<uint16_t>(material.emissiveRG16F >> 16u)) - static_cast<float>(emission[1] / parent.childCount)) < 0.01f);
    assert(std::abs(halfToFloat(static_cast<uint16_t>(material.emissiveB16F)) - static_cast<float>(emission[2] / parent.childCount)) < 0.01f);
  }

  // Opposing bright hues must not collapse to the gray arithmetic RGB mean.
  std::vector<SparseVoxelMaterialGPU> primaries(3);
  primaries[0].albedoRGBA8 = 0xff0000ffu;
  primaries[1].albedoRGBA8 = 0xff00ff00u;
  primaries[2].albedoRGBA8 = 0xffff0000u;
  primaries[0].roughnessMetallicUNorm8 = 20u | (30u << 8u);
  primaries[1].roughnessMetallicUNorm8 = 40u | (60u << 8u);
  primaries[2].roughnessMetallicUNorm8 = 60u | (90u << 8u);
  primaries[0].flags = 1u;
  primaries[1].flags = 2u;
  primaries[2].flags = 4u;
  const SparseVoxelMaterialGPU vivid = reduceSparseVoxelLODMaterials(primaries, 17u);
  const uint32_t vividMinimum = std::min({packedByte(vivid.albedoRGBA8, 0u), packedByte(vivid.albedoRGBA8, 8u), packedByte(vivid.albedoRGBA8, 16u)});
  const uint32_t vividMaximum = std::max({packedByte(vivid.albedoRGBA8, 0u), packedByte(vivid.albedoRGBA8, 8u), packedByte(vivid.albedoRGBA8, 16u)});
  assert(vividMaximum == 255u && vividMinimum == 0u);
  assert(packedByte(vivid.roughnessMetallicUNorm8, 0u) == 40u);
  assert(packedByte(vivid.roughnessMetallicUNorm8, 8u) == 60u);
  assert(vivid.materialId == 17u && vivid.flags == 7u);
}

void runG1Tests()
{
  testAbiLayout();
  testOctreePathGoldensAndRoundTrips();
  testHashMaterialAndHalfGoldens();
  testExteriorShellGeneration();
  testDeterminismAndValidationFailures();
  testLODHierarchyAndAveragedMaterials();
}

struct BootstrapReadback
{
  SparseVoxelFrameCountersGPU counters{};
  std::array<VisibleInstanceGPU, 4> visible{};
  size_t visibleReadCount = 0u;
  std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> args{};
};

BootstrapReadback runBootstrapGpu(::rendering::backend::vulkan::VulkanRHI &rhi, const svrRendering::SparseVoxelBootstrapPass::Settings settings, const bool emit)
{
  using namespace ::rendering;
  assert(settings.visibleInstanceCapacity <= 4u);
  RenderGraph renderGraph(&rhi);

  const uint64_t visibleBufferSize = std::max<uint64_t>(1u, settings.visibleInstanceCapacity) * sizeof(VisibleInstanceGPU);
  const uint64_t argsBufferSize = kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU);
  const Buffer counters = renderGraph.createBuffer(
      {.name = "SparseVoxelBootstrapTests.counters", .size = sizeof(SparseVoxelFrameCountersGPU), .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc});
  const Buffer visible =
      renderGraph.createBuffer({.name = "SparseVoxelBootstrapTests.visible", .size = visibleBufferSize, .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc});
  const Buffer args = renderGraph.createBuffer(
      {.name = "SparseVoxelBootstrapTests.args", .size = argsBufferSize, .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Indirect | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc});
  const Buffer countersReadback =
      renderGraph.createBuffer({.name = "SparseVoxelBootstrapTests.countersReadback", .size = sizeof(SparseVoxelFrameCountersGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer visibleReadback = renderGraph.createBuffer({.name = "SparseVoxelBootstrapTests.visibleReadback", .size = visibleBufferSize, .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer argsReadback = renderGraph.createBuffer({.name = "SparseVoxelBootstrapTests.argsReadback", .size = argsBufferSize, .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});

  std::array<uint8_t, sizeof(SparseVoxelFrameCountersGPU)> poisonCounters{};
  std::vector<uint8_t> poisonVisible(visibleBufferSize, 0xcdu);
  std::array<uint8_t, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU)> poisonArgs{};
  poisonCounters.fill(0xcdu);
  poisonArgs.fill(0xcdu);
  renderGraph.bufferWrite(counters, 0u, poisonCounters.size(), poisonCounters.data());
  renderGraph.bufferWrite(visible, 0u, poisonVisible.size(), poisonVisible.data());
  renderGraph.bufferWrite(args, 0u, poisonArgs.size(), poisonArgs.data());

  renderGraph.registerPass<svrRendering::SparseVoxelBootstrapPass>("SparseVoxelBootstrapTests.Clear", 0u, svrRendering::SparseVoxelBootstrapPass::Kernel::ClearFrame, counters, visible, args, settings);
  if (emit)
  {
    renderGraph.registerPass<svrRendering::SparseVoxelBootstrapPass>("SparseVoxelBootstrapTests.Emit", 1u, svrRendering::SparseVoxelBootstrapPass::Kernel::EmitBootstrapVisibleInstance, counters, visible, args, settings);
  }
  const uint32_t copyBase = emit ? 2u : 1u;
  renderGraph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "SparseVoxelBootstrapTests.CopyCounters", copyBase, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), countersReadback, 0u, sizeof(SparseVoxelFrameCountersGPU));
  renderGraph.registerPass<::rendering::gpgpu::CopyBufferPass>("SparseVoxelBootstrapTests.CopyVisible", copyBase + 1u, visible, 0u, visibleBufferSize, visibleReadback, 0u, visibleBufferSize);
  renderGraph.registerPass<::rendering::gpgpu::CopyBufferPass>("SparseVoxelBootstrapTests.CopyArgs", copyBase + 2u, args, 0u, argsBufferSize, argsReadback, 0u, argsBufferSize);

  renderGraph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  renderGraph.run(frame, overrides);
  renderGraph.waitFrame(frame);

  BootstrapReadback result{};
  result.visibleReadCount = static_cast<size_t>(visibleBufferSize / sizeof(VisibleInstanceGPU));
  renderGraph.bufferRead(
      countersReadback,
      0u,
      sizeof(result.counters),
      [&](const void *data)
      {
        std::memcpy(&result.counters, data, sizeof(result.counters));
      });
  renderGraph.bufferRead(
      visibleReadback,
      0u,
      visibleBufferSize,
      [&](const void *data)
      {
        std::memcpy(result.visible.data(), data, visibleBufferSize);
      });
  renderGraph.bufferRead(
      argsReadback,
      0u,
      sizeof(result.args),
      [&](const void *data)
      {
        std::memcpy(result.args.data(), data, sizeof(result.args));
      });
  return result;
}

void assertCleared(const BootstrapReadback &result)
{
  const SparseVoxelFrameCountersGPU zeroCounters{};
  assert(std::memcmp(&result.counters, &zeroCounters, sizeof(zeroCounters)) == 0);
  for (size_t index = 0u; index < result.visibleReadCount; ++index)
  {
    assert(result.visible[index].instanceIndex == kInvalidSparseVoxelIndex);
    assert(result.visible[index].firstVisibleVoxel == 0u);
    assert(result.visible[index].visibleVoxelCount == 0u);
    assert(result.visible[index].flags == 0u);
  }
  const std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> zeroArgs{};
  assert(std::memcmp(result.args.data(), zeroArgs.data(), sizeof(zeroArgs)) == 0);
}

struct LODSelectionReadback
{
  SparseVoxelFrameCountersGPU counters{};
  VisibleInstanceGPU visibleInstance{};
  std::vector<VisibleVoxelGPU> visibleVoxels;
  std::array<IndirectDispatchArgsGPU, 8u> lodArgs{};
  std::array<IndirectDispatchArgsGPU, kSparseVoxelIndirectSlotCount> rasterArgs{};
  uint32_t pageRequestBits = 0u;
};

LODSelectionReadback runLODSelection(
    ::rendering::backend::vulkan::VulkanRHI &rhi,
    const BootstrapSparseVoxelLOD &hierarchy,
    const bool enabled,
    const float threshold,
    const bool useTLAS = false,
    const bool moveOutsideFrustum = false,
    const bool cameraInside = false)
{
  using namespace ::rendering;
  RenderGraph graph(&rhi);
  auto storage = [&](const char *name, const uint64_t size, const bool indirect = false)
  {
    return graph.createBuffer(
        {.name = name,
         .size = size,
         .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc |
                  (indirect ? BufferUsage::BufferUsage_Indirect : BufferUsage::BufferUsage_None)});
  };
  const uint32_t capacity = static_cast<uint32_t>(hierarchy.nodes.size());
  const Buffer counters = storage("lod.test.counters", sizeof(SparseVoxelFrameCountersGPU));
  const Buffer visibleInstances = storage("lod.test.instancesVisible", sizeof(VisibleInstanceGPU));
  const Buffer visibleVoxels = storage("lod.test.voxelsVisible", uint64_t{capacity} * sizeof(VisibleVoxelGPU));
  const Buffer instances = storage("lod.test.instances", sizeof(SparseVoxelInstanceGPU));
  const Buffer nodes = storage("lod.test.nodes", uint64_t{capacity} * sizeof(SparseVoxelLODNodeGPU));
  const Buffer centers = storage("lod.test.centers", uint64_t{capacity} * sizeof(SparseVoxelCenterLengthGPU));
  const Buffer queueA = storage("lod.test.queueA", uint64_t{capacity} * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueB = storage("lod.test.queueB", uint64_t{capacity} * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer queueCounts = storage("lod.test.queueCounts", 8u * sizeof(uint32_t));
  const Buffer lodArgs = storage("lod.test.lodArgs", 8u * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer rasterArgs = storage("lod.test.rasterArgs", kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), true);
  const Buffer residentMetadata = storage("lod.test.residentMetadata", uint64_t{capacity} * sizeof(SparseVoxelResidentNodeMetadataGPU));
  const Buffer pageRequestBits = storage("lod.test.pageRequestBits", sizeof(uint32_t));
  const Buffer pageRequestPriorities = storage("lod.test.pageRequestPriorities", sizeof(uint32_t));
  const Buffer pageRequestInstances = storage("lod.test.pageRequestInstances", sizeof(uint32_t));
  const Buffer lodSeeds = storage("lod.test.seeds", sizeof(SparseVoxelLODSeedGPU));
  const Buffer pageBounds = storage("lod.test.pageBounds", sizeof(SparseVoxelPageBoundsGPU));
  const Buffer rejectedNodes = storage("lod.test.rejectedNodes", uint64_t{capacity} * sizeof(SparseVoxelLODQueueElementGPU));
  const Buffer recoveryState = storage("lod.test.recoveryState", sizeof(SparseVoxelLODRecoveryStateGPU));
  const Buffer temporalVisibleVoxels = storage("lod.test.temporalVisibleVoxels", uint64_t{capacity} * sizeof(VisibleVoxelGPU));
  const Buffer preSubmittedFlags = storage("lod.test.preSubmittedFlags", sizeof(uint32_t));
  const Buffer countersReadback = graph.createBuffer(
      {.name = "lod.test.countersReadback", .size = sizeof(SparseVoxelFrameCountersGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer visibleReadback = graph.createBuffer(
      {.name = "lod.test.visibleReadback", .size = uint64_t{capacity} * sizeof(VisibleVoxelGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});

  const Buffer visibleInstanceReadback = graph.createBuffer(
      {.name = "lod.test.visibleInstanceReadback", .size = sizeof(VisibleInstanceGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer rasterArgsReadback = graph.createBuffer(
      {.name = "lod.test.rasterArgsReadback", .size = kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer lodArgsReadback = graph.createBuffer(
      {.name = "lod.test.lodArgsReadback", .size = 8u * sizeof(IndirectDispatchArgsGPU), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});
  const Buffer pageRequestBitsReadback = graph.createBuffer(
      {.name = "lod.test.pageRequestBitsReadback", .size = sizeof(uint32_t), .usage = BufferUsage::BufferUsage_Pull | BufferUsage::BufferUsage_CopyDst});

  const BootstrapSparseVoxelModel model = generateExteriorShell128();
  SparseVoxelSceneManager sceneManager;
  if (useTLAS)
  {
    SparseVoxelScene scene = makeExteriorShellScene(kDefaultExteriorShellMaterialSeed);
    if (moveOutsideFrustum)
      scene.instances[0].modelToWorld[12] = 1000.0f;
    std::string error;
    assert(sceneManager.setScene(scene, &error));
    const SparseVoxelInstanceGPU &instance = sceneManager.runtimeInstances()[0].gpu;
    graph.bufferWrite(instances, 0u, sizeof(instance), const_cast<SparseVoxelInstanceGPU *>(&instance));
  }
  else
  {
    graph.bufferWrite(instances, 0u, sizeof(model.instance), const_cast<SparseVoxelInstanceGPU *>(&model.instance));
  }
  graph.bufferWrite(nodes, 0u, uint64_t{capacity} * sizeof(SparseVoxelLODNodeGPU), const_cast<SparseVoxelLODNodeGPU *>(hierarchy.nodes.data()));
  graph.bufferWrite(centers, 0u, uint64_t{capacity} * sizeof(SparseVoxelCenterLengthGPU), const_cast<SparseVoxelCenterLengthGPU *>(hierarchy.centersLengths.data()));
  std::vector<SparseVoxelResidentNodeMetadataGPU> metadata(capacity);
  graph.bufferWrite(residentMetadata, 0u, uint64_t{capacity} * sizeof(metadata[0]), metadata.data());

  SparseVoxelRasterUniformsGPU raster{};
  raster.worldToCamera = cameraInside
                             ? std::array<float, 16>{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f}
                             : std::array<float, 16>{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 4.0f, 1.0f};
  raster.cameraWorldPositionNear = {0.0f, 0.0f, cameraInside ? 0.0f : -4.0f, 0.01f};
  raster.focalPrincipal = {64.0f, 64.0f, 64.0f, 64.0f};
  raster.viewportTiles = {128u, 128u, 8u, 8u};
  raster.visibleVoxelCapacity = capacity;
  raster.projectedVoxelCapacity = capacity;
  raster.lodEnabled = enabled ? 1u : 0u;
  raster.lodPixelThreshold = threshold;

  uint32_t passIndex = 0u;
  graph.registerPass<svrRendering::SparseVoxelBootstrapPass>(
      "lod.test.clear",
      passIndex++,
      svrRendering::SparseVoxelBootstrapPass::Kernel::ClearFrame,
      counters,
      visibleInstances,
      rasterArgs,
      svrRendering::SparseVoxelBootstrapPass::Settings{capacity, kPreprocessWorkgroupSize, 1u, capacity});

  if (useTLAS)
  {
    const auto &tlas = sceneManager.tlas();
    const uint32_t tlasNodeCount = static_cast<uint32_t>(tlas.nodes.size());
    const uint32_t tlasDepth = std::max(1u, tlas.maxDepth);
    const Buffer tlasNodes = storage("lod.test.tlasNodes", uint64_t{tlasNodeCount} * sizeof(SparseVoxelTLASNodeGPU));
    const Buffer tlasInstanceIndices = storage("lod.test.tlasInstanceIndices", sizeof(uint32_t));
    const Buffer tlasBLASRoots = storage("lod.test.tlasBLASRoots", sizeof(SparseVoxelBLASRootRangeGPU));
    const Buffer tlasQueueA = storage("lod.test.tlasQueueA", uint64_t{tlasNodeCount} * sizeof(TLASQueueElementGPU));
    const Buffer tlasQueueB = storage("lod.test.tlasQueueB", uint64_t{tlasNodeCount} * sizeof(TLASQueueElementGPU));
    const Buffer tlasQueueCounts = storage("lod.test.tlasQueueCounts", uint64_t{tlasDepth} * sizeof(uint32_t));
    const Buffer tlasDispatchArgs = storage("lod.test.tlasDispatchArgs", uint64_t{tlasDepth} * sizeof(IndirectDispatchArgsGPU), true);
    const Buffer tlasVisibilityFlags = storage("lod.test.tlasVisibilityFlags", sizeof(uint32_t));
    const Buffer tlasVisibilityOffsets = storage("lod.test.tlasVisibilityOffsets", sizeof(SparseVoxelVisibilityOffsetGPU));
    const Buffer tlasScanBlocks = storage("lod.test.tlasScanBlocks", sizeof(SparseVoxelTLASScanBlockGPU));
    graph.bufferWrite(tlasNodes, 0u, uint64_t{tlasNodeCount} * sizeof(SparseVoxelTLASNodeGPU), const_cast<SparseVoxelTLASNodeGPU *>(tlas.nodes.data()));
    graph.bufferWrite(tlasInstanceIndices, 0u, sizeof(uint32_t), const_cast<uint32_t *>(tlas.instanceIndices.data()));
    graph.bufferWrite(tlasBLASRoots, 0u, sizeof(SparseVoxelBLASRootRangeGPU), const_cast<SparseVoxelBLASRootRangeGPU *>(sceneManager.blasRootRanges().data()));
    const svrRendering::SparseVoxelTLASCullingPass::Buffers tlasBuffers{
      counters, instances, tlasNodes, tlasInstanceIndices, tlasBLASRoots, tlasQueueA, tlasQueueB, tlasQueueCounts,
      tlasDispatchArgs, tlasVisibilityFlags, tlasVisibilityOffsets, tlasScanBlocks, visibleInstances, lodSeeds,
    };
    SparseVoxelTLASCullingUniformsGPU culling{};
    culling.instanceCount = 1u;
    culling.nodeCount = tlasNodeCount;
    culling.rootNode = tlas.rootNode;
    culling.maxDepth = tlasDepth;
    culling.queueCapacity = tlasNodeCount;
    culling.visibleInstanceCapacity = 1u;
    culling.lodQueueCapacity = capacity;
    culling.scanBlockCount = 1u;
    culling.blasCount = 1u;
    culling.cullingFlags = kSparseVoxelCullingFrustum;
    auto addTLAS = [&](const std::string &name, const svrRendering::SparseVoxelTLASCullingPass::Kernel kernel, const uint32_t depth)
    {
      culling.depth = depth;
      graph.registerPass<svrRendering::SparseVoxelTLASCullingPass>(
          name, passIndex++, kernel, tlasBuffers, svrRendering::SparseVoxelTLASCullingPass::Settings{raster, culling});
    };
    addTLAS("lod.test.tlas.initialize", svrRendering::SparseVoxelTLASCullingPass::Kernel::Initialize, 0u);
    for (uint32_t depth = 0u; depth < tlas.maxDepth; ++depth)
    {
      addTLAS("lod.test.tlas.traverse" + std::to_string(depth), svrRendering::SparseVoxelTLASCullingPass::Kernel::Traverse, depth);
      if (depth + 1u < tlas.maxDepth)
        addTLAS("lod.test.tlas.finalize" + std::to_string(depth), svrRendering::SparseVoxelTLASCullingPass::Kernel::FinalizeLevel, depth);
    }
    addTLAS("lod.test.tlas.scan", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScanVisibility, 0u);
    addTLAS("lod.test.tlas.scanBlocks", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScanBlockSums, 0u);
    addTLAS("lod.test.tlas.scatter", svrRendering::SparseVoxelTLASCullingPass::Kernel::ScatterVisibility, 0u);
  }
  const svrRendering::SparseVoxelLODPass::Buffers buffers{
    counters, visibleInstances, visibleVoxels, instances, nodes, centers, queueA, queueB, queueCounts, lodArgs, rasterArgs,
    residentMetadata, pageRequestBits, pageRequestPriorities, pageRequestInstances,
    lodSeeds,
    pageBounds,
    rejectedNodes,
    recoveryState,
    temporalVisibleVoxels,
    preSubmittedFlags,
  };
  auto add = [&](const std::string &name, const svrRendering::SparseVoxelLODPass::Kernel kernel, const uint32_t depth)
  {
    graph.registerPass<svrRendering::SparseVoxelLODPass>(
        name,
        passIndex++,
        kernel,
        buffers,
        svrRendering::SparseVoxelLODPass::Settings{
          raster,
          {hierarchy.firstRoot, hierarchy.rootCount, capacity, depth, 1u, 1u, 0u,
           useTLAS ? (kSparseVoxelCullingFrustum | kSparseVoxelLODSeedsFromVisibleInstances) : 0u,
           0u, 1u, 0u, 0u}});
  };
  add("lod.test.initialize", svrRendering::SparseVoxelLODPass::Kernel::Initialize, 0u);
  for (uint32_t depth = 0u; depth < kExteriorShellLevel; ++depth)
  {
    add("lod.test.traverse" + std::to_string(depth), svrRendering::SparseVoxelLODPass::Kernel::Traverse, depth);
    if (depth + 1u < kExteriorShellLevel)
      add("lod.test.finalizeLevel" + std::to_string(depth), svrRendering::SparseVoxelLODPass::Kernel::FinalizeLevel, depth);
  }
  add("lod.test.finalizeSelection", svrRendering::SparseVoxelLODPass::Kernel::FinalizeSelection, kExteriorShellLevel - 1u);
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyCounters", passIndex++, counters, 0u, sizeof(SparseVoxelFrameCountersGPU), countersReadback, 0u, sizeof(SparseVoxelFrameCountersGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyVisible", passIndex++, visibleVoxels, 0u, uint64_t{capacity} * sizeof(VisibleVoxelGPU), visibleReadback, 0u, uint64_t{capacity} * sizeof(VisibleVoxelGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyVisibleInstance", passIndex++, visibleInstances, 0u, sizeof(VisibleInstanceGPU), visibleInstanceReadback, 0u, sizeof(VisibleInstanceGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyRasterArgs", passIndex++, rasterArgs, 0u, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU), rasterArgsReadback, 0u, kSparseVoxelIndirectSlotCount * sizeof(IndirectDispatchArgsGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyLODArgs", passIndex++, lodArgs, 0u, 8u * sizeof(IndirectDispatchArgsGPU), lodArgsReadback, 0u, 8u * sizeof(IndirectDispatchArgsGPU));
  graph.registerPass<::rendering::gpgpu::CopyBufferPass>(
      "lod.test.copyPageRequestBits", passIndex++, pageRequestBits, 0u, sizeof(uint32_t), pageRequestBitsReadback, 0u, sizeof(uint32_t));
  graph.compile();
  RenderGraph::Frame frame{};
  RenderGraph::Overrides overrides{};
  graph.run(frame, overrides);
  graph.waitFrame(frame);

  LODSelectionReadback result{};
  graph.bufferRead(countersReadback, 0u, sizeof(result.counters), [&](const void *data) { std::memcpy(&result.counters, data, sizeof(result.counters)); });
  assert(result.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  result.visibleVoxels.resize(result.counters.visibleVoxelCount);
  if (!result.visibleVoxels.empty())
    graph.bufferRead(visibleReadback, 0u, result.visibleVoxels.size() * sizeof(result.visibleVoxels[0]), [&](const void *data) { std::memcpy(result.visibleVoxels.data(), data, result.visibleVoxels.size() * sizeof(result.visibleVoxels[0])); });
  graph.bufferRead(visibleInstanceReadback, 0u, sizeof(result.visibleInstance), [&](const void *data) { std::memcpy(&result.visibleInstance, data, sizeof(result.visibleInstance)); });
  graph.bufferRead(lodArgsReadback, 0u, sizeof(result.lodArgs), [&](const void *data) { std::memcpy(result.lodArgs.data(), data, sizeof(result.lodArgs)); });
  graph.bufferRead(rasterArgsReadback, 0u, sizeof(result.rasterArgs), [&](const void *data) { std::memcpy(result.rasterArgs.data(), data, sizeof(result.rasterArgs)); });
  graph.bufferRead(pageRequestBitsReadback, 0u, sizeof(result.pageRequestBits), [&](const void *data) { std::memcpy(&result.pageRequestBits, data, sizeof(result.pageRequestBits)); });
  return result;
}

void testLODTraversalGpu(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const BootstrapSparseVoxelLOD hierarchy = generateExteriorShellLOD128();
  const auto roots = runLODSelection(rhi, hierarchy, true, 1000000.0f).visibleVoxels;
  assert(roots.size() == hierarchy.rootCount);
  std::vector<bool> seen(hierarchy.nodes.size(), false);
  for (const auto &visible : roots)
  {
    assert(visible.instanceIndex == 0u);
    assert(visible.voxelIndex < hierarchy.rootCount);
    assert(!seen[visible.voxelIndex]);
    seen[visible.voxelIndex] = true;
    assert(hierarchy.nodes[visible.voxelIndex].level == 1u);
  }
  for (uint32_t root = 0u; root < hierarchy.rootCount; ++root)
    assert(seen[root]);

  // A hollow shell must not turn into a coarse solid cube around the camera.
  // Camera-containing and near-plane-crossing nodes refine until they no
  // longer contain the camera or reach an actual leaf.
  const auto inside = runLODSelection(rhi, hierarchy, true, 64.0f, false, false, true).visibleVoxels;
  assert(inside.size() > hierarchy.rootCount);
  for (const auto &visible : inside)
  {
    const SparseVoxelCenterLengthGPU &bounds = hierarchy.centersLengths[visible.voxelIndex];
    const float radius = bounds.length * 0.5f;
    const bool containsCamera =
        std::abs(bounds.center[0]) <= radius &&
        std::abs(bounds.center[1]) <= radius &&
        std::abs(bounds.center[2]) <= radius;
    assert(!containsCamera || hierarchy.nodes[visible.voxelIndex].childCount == 0u);
  }

  const auto leaves = runLODSelection(rhi, hierarchy, false, 8.0f).visibleVoxels;
  assert(leaves.size() == kExteriorShellVoxelCount);
  std::fill(seen.begin(), seen.end(), false);
  for (const auto &visible : leaves)
  {
    assert(visible.instanceIndex == 0u);
    assert(visible.voxelIndex < hierarchy.nodes.size());
    assert(!seen[visible.voxelIndex]);
    seen[visible.voxelIndex] = true;
    assert(hierarchy.nodes[visible.voxelIndex].level == kExteriorShellLevel);
  }
  for (uint32_t index = 0u; index < hierarchy.nodes.size(); ++index)
    assert(seen[index] == (hierarchy.nodes[index].level == kExteriorShellLevel));
}

void testTLASProducerSwapGpu(::rendering::backend::vulkan::VulkanRHI &rhi)
{
  const BootstrapSparseVoxelLOD hierarchy = generateExteriorShellLOD128();
  const LODSelectionReadback bootstrap = runLODSelection(rhi, hierarchy, true, 1000000.0f);
  const LODSelectionReadback tlas = runLODSelection(rhi, hierarchy, true, 1000000.0f, true);

  assert(std::memcmp(&bootstrap.counters, &tlas.counters, sizeof(bootstrap.counters)) == 0);
  assert(std::memcmp(&bootstrap.visibleInstance, &tlas.visibleInstance, sizeof(bootstrap.visibleInstance)) == 0);
  assert(bootstrap.visibleVoxels.size() == tlas.visibleVoxels.size());
  assert(std::memcmp(
             bootstrap.visibleVoxels.data(),
             tlas.visibleVoxels.data(),
             bootstrap.visibleVoxels.size() * sizeof(VisibleVoxelGPU)) == 0);
  assert(std::memcmp(bootstrap.lodArgs.data(), tlas.lodArgs.data(), sizeof(bootstrap.lodArgs)) == 0);
  assert(std::memcmp(bootstrap.rasterArgs.data(), tlas.rasterArgs.data(), sizeof(bootstrap.rasterArgs)) == 0);
  assert(bootstrap.pageRequestBits == tlas.pageRequestBits);
  assert(tlas.counters.visibleInstanceCount == 1u);
  assert(tlas.counters.visibleVoxelCount == hierarchy.rootCount);

  const LODSelectionReadback culled = runLODSelection(rhi, hierarchy, true, 1000000.0f, true, true);
  assert(culled.counters.visibleInstanceCount == 0u);
  assert(culled.counters.visibleVoxelCount == 0u);
  assert(culled.counters.candidateCount == 0u);
  assert(culled.counters.duplicateCount == 0u);
  assert(culled.visibleVoxels.empty());
  assert(culled.visibleInstance.instanceIndex == kInvalidSparseVoxelIndex);
  assert(culled.pageRequestBits == 0u);
  for (const IndirectDispatchArgsGPU &args : culled.lodArgs)
    assert(args.x == 0u);
  assert(culled.rasterArgs[0].x == 0u);
  for (size_t slot = 1u; slot < culled.rasterArgs.size(); ++slot)
  {
    const IndirectDispatchArgsGPU zero{};
    assert(std::memcmp(&culled.rasterArgs[slot], &zero, sizeof(zero)) == 0);
  }
}

void runG2Tests()
{
  using namespace ::rendering;
  using namespace ::rendering::backend;
  DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 64u,
  };
  const DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);

  testLODTraversalGpu(rhi);

  svrRendering::SparseVoxelBootstrapPass::Settings settings{};
  assertCleared(runBootstrapGpu(rhi, settings, false));
  settings.visibleInstanceCapacity = 4u;
  assertCleared(runBootstrapGpu(rhi, settings, false));
  settings = {};

  const BootstrapReadback emitted = runBootstrapGpu(rhi, settings, true);
  assert(emitted.counters.visibleInstanceCount == 1u);
  assert(emitted.counters.visibleVoxelCount == kExteriorShellVoxelCount);
  assert(emitted.counters.candidateCount == 0u);
  assert(emitted.counters.duplicateCount == 0u);
  assert(emitted.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::None));
  assert(emitted.visible[0].instanceIndex == 0u);
  assert(emitted.visible[0].firstVisibleVoxel == 0u);
  assert(emitted.visible[0].visibleVoxelCount == kExteriorShellVoxelCount);
  assert(emitted.visible[0].flags == 0u);
  assert(emitted.args[0].x == (kExteriorShellVoxelCount + kPreprocessWorkgroupSize - 1u) / kPreprocessWorkgroupSize);
  assert(emitted.args[0].y == 1u);
  assert(emitted.args[0].z == 1u);
  assert(emitted.args[0].reserved == 0u);
  for (size_t index = 1u; index < emitted.args.size(); ++index)
  {
    const IndirectDispatchArgsGPU zero{};
    assert(std::memcmp(&emitted.args[index], &zero, sizeof(zero)) == 0);
  }

  settings.voxelCount = 0u;
  assertCleared(runBootstrapGpu(rhi, settings, true));

  settings = {};
  settings.visibleVoxelCapacity = kExteriorShellVoxelCount - 1u;
  const BootstrapReadback voxelOverflow = runBootstrapGpu(rhi, settings, true);
  assert(voxelOverflow.counters.visibleInstanceCount == 0u);
  assert(voxelOverflow.counters.visibleVoxelCount == 0u);
  assert(voxelOverflow.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::VisibleVoxels));
  assert(voxelOverflow.counters.requiredCount == kExteriorShellVoxelCount);
  assert(voxelOverflow.counters.capacity == kExteriorShellVoxelCount - 1u);
  assert(voxelOverflow.args[0].x == 0u);

  settings = {};
  settings.visibleInstanceCapacity = 0u;
  const BootstrapReadback instanceOverflow = runBootstrapGpu(rhi, settings, true);
  assert(instanceOverflow.counters.overflowStage == static_cast<uint32_t>(SparseVoxelOverflowStage::VisibleInstances));
  assert(instanceOverflow.counters.requiredCount == 1u);
  assert(instanceOverflow.counters.capacity == 0u);
  assert(instanceOverflow.args[0].x == 0u);
}

void runG12Tests()
{
  using namespace ::rendering;
  using namespace ::rendering::backend;
  DeviceRequiredLimits limits{
    .minimumMemory = 0u,
    .minimumComputeSharedMemory = 0u,
    .minimumComputeWorkGroupInvocations = 256u,
  };
  const DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute;
  vulkan::VulkanRHI rhi(vulkan::Vulkan_1_2, limits, features, {});
  std::vector<VkSurfaceKHR> surfaces;
  rhi.init(surfaces);
  testTLASProducerSwapGpu(rhi);
}

} // namespace

int main(int argc, char **argv)
{
  os::Logger::start();
  assert(argc == 2);
  const std::string gate = argv[1];
  if (gate == "--g1")
  {
    runG1Tests();
  }
  else if (gate == "--g2")
  {
    runG1Tests();
    runG2Tests();
  }
  else
  {
    assert(gate == "--g12");
    runG12Tests();
  }
  os::Logger::shutdown();
  return 0;
}
