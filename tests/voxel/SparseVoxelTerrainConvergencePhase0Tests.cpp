#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelSceneFile.hpp"
#include "voxel/svraster/SparseVoxelStreaming.hpp"
#include "voxel/svraster/SparseVoxelTerrainTree64.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace
{

using namespace voxel::svraster;

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

SparseVoxelCenterLengthGPU centerLength(
    const SparseVoxelBLAS &model,
    const uint64_t path,
    const uint32_t level)
{
  const auto coordinate = decodeOctreePath(path, level);
  const float length =
      model.rootExtent / static_cast<float>(uint32_t{1} << level);
  return {{
            model.rootCenter[0] - model.rootExtent * 0.5f +
                (static_cast<float>(coordinate[0]) + 0.5f) * length,
            model.rootCenter[1] - model.rootExtent * 0.5f +
                (static_cast<float>(coordinate[1]) + 0.5f) * length,
            model.rootCenter[2] - model.rootExtent * 0.5f +
                (static_cast<float>(coordinate[2]) + 0.5f) * length,
          },
          length};
}

SparseVoxelScene syntheticTerrainChunkScene()
{
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.0f, 0.0f, 0.0f};
  model.rootExtent = 64.0f;
  model.maxLevel = 2u;
  model.firstRoot = 0u;
  model.rootCount = 8u;

  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff507030u;
  material.roughnessMetallicUNorm8 = 0x7f00u;
  material.materialId = 11u;

  for (uint32_t level = 1u; level <= 2u; ++level)
  {
    const uint32_t resolution = 1u << level;
    std::vector<uint64_t> paths;
    for (uint32_t z = 0u; z < resolution; ++z)
      for (uint32_t y = 0u; y < resolution; ++y)
        for (uint32_t x = 0u; x < resolution; ++x)
          paths.push_back(encodeOctreePath(x, y, z, level));
    std::sort(paths.begin(), paths.end());
    for (const uint64_t path : paths)
    {
      const uint32_t nodeIndex =
          static_cast<uint32_t>(model.hierarchyNodes.size());
      const bool parent = level == 1u;
      model.hierarchyNodes.push_back(
          {parent ? 8u + nodeIndex * 8u : 0u,
              parent ? 8u : 0u, nodeIndex, level});
      model.octreePaths.push_back(path);
      model.octreeLevels.push_back(static_cast<uint8_t>(level));
      model.centersLengths.push_back(centerLength(model, path, level));
      model.materials.push_back(material);
    }
  }

  scene.models.push_back(std::move(model));
  scene.instances.push_back({0u, 0u, identityMatrix()});
  return scene;
}

struct Fixture
{
  std::filesystem::path path;
  VirtualSparseVoxelSceneMetadata metadata;

  Fixture()
  {
    const auto stamp = std::chrono::steady_clock::now()
                           .time_since_epoch()
                           .count();
    path = std::filesystem::temp_directory_path() /
        ("engine-svraster-convergence-phase0-" +
            std::to_string(stamp) + ".vsvo");
    std::string error;
    assert(writeVirtualSparseVoxelScene(
        path.string(), syntheticTerrainChunkScene(),
        {VirtualSparseVoxelFileCompression::None, 704u}, &error));
    assert(readVirtualSparseVoxelSceneMetadata(path.string(), metadata, &error));
    assert(metadata.modelCount == 1u);
    assert(metadata.pageCount == 9u);
    assert(metadata.pages[metadata.models[0].rootPage].flags &
        kVirtualSparseVoxelPageRoot);
  }

  ~Fixture()
  {
    if (!path.empty())
      std::filesystem::remove(path);
  }
};

void installPage(
    const Fixture &fixture,
    SparseVoxelPageResidency &residency,
    const uint32_t pageIndex,
    SparseVoxelPageInstallBatch *installed = nullptr)
{
  std::string error;
  assert(residency.markReading(pageIndex, &error));
  VirtualSparseVoxelPageData page{};
  assert(readVirtualSparseVoxelPage(
      fixture.path.string(), fixture.metadata, pageIndex, page, &error));
  assert(residency.markDecoded(pageIndex, &error));
  SparseVoxelPageInstallBatch batch{};
  assert(residency.prepareInstall(page, batch, &error));
  assert(residency.commitInstall(batch, &error));
  if (installed != nullptr)
    *installed = std::move(batch);
}

void testSyntheticChunkCutDiagnostics()
{
  Fixture fixture;
  SparseVoxelPageResidency residency;
  std::string error;
  assert(residency.initialize(fixture.metadata, 72u, &error));

  SparseVoxelCutDiagnostics diagnostics{};
  assert(!residency.diagnoseCutCoverage(0u, diagnostics, &error));
  assert(diagnostics.selectedNeitherParentNorChildCount ==
      fixture.metadata.models[0].rootCount);
  assert(classifySparseVoxelMissingRegion(diagnostics) ==
      SparseVoxelMissingRegionCategory::CutSelection);

  auto admissions = residency.planAdmissions(
      {1u, 72u, std::numeric_limits<uint64_t>::max()});
  assert(admissions == std::vector<uint32_t>{fixture.metadata.models[0].rootPage});
  installPage(fixture, residency, admissions.front());
  assert(residency.diagnostics().pageInstallCount == 1u);
  assert(residency.diagnoseCutCoverage(0u, diagnostics, &error));
  assert(diagnostics.parentFallbackCount == fixture.metadata.models[0].rootCount);
  assert(diagnostics.childSelectionCount == 0u);
  assert(diagnostics.selectedNeitherParentNorChildCount == 0u);
  assert(classifySparseVoxelMissingRegion(diagnostics) ==
      SparseVoxelMissingRegionCategory::PageResidency);

  const uint32_t priority =
      quantizeSparseVoxelPagePriority(true, 64.0f, 2.0f);
  for (uint32_t pageIndex = 0u; pageIndex < fixture.metadata.pageCount;
       ++pageIndex)
  {
    if ((fixture.metadata.pages[pageIndex].flags &
            kVirtualSparseVoxelPageRoot) == 0u)
      assert(residency.requestPage(pageIndex, priority, 0u, 10u, &error));
  }
  admissions = residency.planAdmissions(
      {8u, 64u, std::numeric_limits<uint64_t>::max()});
  assert(admissions.size() == 8u);
  SparseVoxelPageInstallBatch lastInstall{};
  for (const uint32_t pageIndex : admissions)
    installPage(fixture, residency, pageIndex, &lastInstall);
  assert(residency.diagnostics().pageInstallCount == 9u);
  assert(residency.diagnoseCutCoverage(0u, diagnostics, &error));
  assert(diagnostics.childSelectionCount == fixture.metadata.models[0].rootCount);
  assert(diagnostics.parentFallbackCount == 0u);
  assert(diagnostics.pageResidencyMissCount == 0u);
  assert(classifySparseVoxelMissingRegion(diagnostics) ==
      SparseVoxelMissingRegionCategory::None);

  assert(residency.initialize(fixture.metadata, 72u, &error));
  assert(!residency.commitInstall(lastInstall, &error));
  assert(residency.diagnostics().invalidGenerationFallbackCount == 1u);
  assert(classifySparseVoxelMissingRegion(residency.diagnostics()) ==
      SparseVoxelMissingRegionCategory::CutSelection);
}

void testHZBRejectionCounterBridge()
{
  SparseVoxelFrameCountersGPU frameCounters{};
  frameCounters.occlusionCulledVoxelCount = 7u;
  const auto diagnostics =
      sparseVoxelCutDiagnosticsFromFrameCounters(frameCounters);
  assert(diagnostics.hzbRejectionCount == 7u);
  assert(classifySparseVoxelMissingRegion(diagnostics) ==
      SparseVoxelMissingRegionCategory::HZBOrFrustum);
}

void testTree64LODSidecarVersions()
{
  SparseVoxelTerrainTree64VirtualPagePayload lod0{};
  lod0.internalLOD = 0u;
  lod0.regionMappingId = 3u;
  lod0.geometryRevision = 91u;
  lod0.modelSide = 64u;
  lod0.modelPayloadRevision = 1001u;
  lod0.modelNodes.resize(8u);
  lod0.cellScale = 1u;

  auto lod1 = lod0;
  lod1.internalLOD = 1u;
  lod1.modelSide = 16u;
  lod1.modelPayloadRevision = 1002u;
  lod1.modelNodes.resize(2u);
  lod1.cellScale = 4u;

  const auto lod0Bytes =
      serializeSparseVoxelTerrainTree64VirtualPagePayload(lod0);
  const auto lod1Bytes =
      serializeSparseVoxelTerrainTree64VirtualPagePayload(lod1);
  assert(lod0Bytes != lod1Bytes);

  SparseVoxelTerrainTree64VirtualPagePayload restored{};
  std::string error;
  assert(deserializeSparseVoxelTerrainTree64VirtualPagePayload(
      lod1Bytes, restored, &error));
  assert(restored.internalLOD == 1u);
  assert(restored.geometryRevision == lod1.geometryRevision);
  assert(restored.modelPayloadRevision == lod1.modelPayloadRevision);
  assert(restored.modelNodes.size() == lod1.modelNodes.size());
  assert(restored.cellScale == 4u);
}

void testTree64LODVariantSelectionAndFallback()
{
  std::vector<SparseVoxelTerrainTree64LODVariant> variants{
      {7u, 0u, 51u, 128.0f, 64u, 4096u,
          SparseVoxelTerrainTree64LODVariantState::Resident},
      {7u, 1u, 51u, 32.0f, 16u, 1024u,
          SparseVoxelTerrainTree64LODVariantState::Resident},
      {7u, 2u, 51u, 8.0f, 4u, 256u,
          SparseVoxelTerrainTree64LODVariantState::Missing},
      {7u, 0u, 50u, 128.0f, 64u, 4096u,
          SparseVoxelTerrainTree64LODVariantState::Resident},
  };

  const uint32_t near =
      selectSparseVoxelTerrainTree64LODVariant(variants, 7u, 51u, 80.0f);
  assert(near == 0u);
  const uint32_t farResident =
      selectSparseVoxelTerrainTree64LODVariant(variants, 7u, 51u, 4.0f);
  assert(farResident == 1u);
  const uint32_t farDesired =
      selectSparseVoxelTerrainTree64LODVariant(
          variants, 7u, 51u, 4.0f, false);
  assert(farDesired == 2u);
  assert(variants[farDesired].state ==
      SparseVoxelTerrainTree64LODVariantState::Missing);
  const uint32_t stale =
      selectSparseVoxelTerrainTree64LODVariant(variants, 7u, 52u, 4.0f);
  assert(stale == std::numeric_limits<uint32_t>::max());
}

} // namespace

int main()
{
  testSyntheticChunkCutDiagnostics();
  testHZBRejectionCounterBridge();
  testTree64LODSidecarVersions();
  testTree64LODVariantSelectionAndFallback();
  std::cout << "Sparse voxel terrain convergence Phase 0 tests passed\n";
  return 0;
}
