#include "editor/voxel/SparseVoxelOBJAnalysis.hpp"
#include "editor/voxel/SparseVoxelOBJBuilder.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelSceneFile.hpp"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>

#undef assert
#define assert(condition)                                                                                                                                        \
  do                                                                                                                                                             \
  {                                                                                                                                                              \
    if (!(condition))                                                                                                                                            \
    {                                                                                                                                                            \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);                                                           \
      std::abort();                                                                                                                                              \
    }                                                                                                                                                            \
  } while (false)

namespace
{

using namespace voxel::svraster;

bool near(const double a, const double b, const double epsilon = 1.0e-6)
{
  return std::fabs(a - b) <= epsilon;
}

struct TemporaryDirectory
{
  std::filesystem::path path;

  TemporaryDirectory()
  {
    std::random_device random;
    for (uint32_t attempt = 0u; attempt < 32u; ++attempt)
    {
      path = std::filesystem::temp_directory_path() /
             ("sparse-voxel-obj-analysis-" + std::to_string(random()) + "-" + std::to_string(random()));
      if (std::filesystem::create_directory(path))
        return;
    }
    std::abort();
  }

  ~TemporaryDirectory()
  {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

std::filesystem::path writeFile(const TemporaryDirectory &directory, const char *name, const char *contents)
{
  const std::filesystem::path path = directory.path / name;
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  assert(stream);
  stream << contents;
  assert(stream.good());
  return path;
}

SparseVoxelOBJAnalysis analyzeFixture(const std::filesystem::path &path)
{
  SparseVoxelOBJAnalysis analysis{};
  std::string error;
  assert(analyzeSparseVoxelOBJ(path, analysis, &error));
  assert(error.empty());
  return analysis;
}

std::filesystem::path sourceRoot()
{
  return std::filesystem::path(ENGINE_SOURCE_DIR);
}

std::filesystem::path stanfordBunnyPath()
{
  return sourceRoot() / "assets/meshes/obj/stanford-bunny.obj";
}

std::filesystem::path suzannePath()
{
  return sourceRoot() / "assets/meshes/obj/suzanne.obj";
}

void testMeasurementAndDiagnostics(const TemporaryDirectory &directory)
{
  const auto material = writeFile(
      directory,
      "analytic.mtl",
      "newmtl red\n"
      "Kd 1 0 0\n");
  (void)material;
  const auto path = writeFile(
      directory,
      "analytic.obj",
      "mtllib analytic.mtl\n"
      "o measured\n"
      "v -2 -1 3\n"
      "v 4 -1 3\n"
      "v -2 5 9\n"
      "v -2 -1 3\n"
      "usemtl red\n"
      "f 1 2 3\n"
      "f 1 4 2\n");

  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);
  assert(analysis.sourcePath == path);
  assert(analysis.sourceContentHash != 0u);
  assert(analysis.vertexCount == 4u);
  assert(analysis.triangleCount == 2u);
  assert(analysis.shapeCount == 1u);
  assert(analysis.materialCount == 1u);
  assert(analysis.degenerateTriangleCount == 1u);
  assert((analysis.sourceBounds.min == std::array<double, 3>{-2.0, -1.0, 3.0}));
  assert((analysis.sourceBounds.max == std::array<double, 3>{4.0, 5.0, 9.0}));
  assert((analysis.sourceBounds.center == std::array<double, 3>{1.0, 2.0, 6.0}));
  assert((analysis.sourceBounds.extent == std::array<double, 3>{6.0, 6.0, 6.0}));
  assert(near(analysis.sourceBounds.diagonal, std::sqrt(108.0)));

  std::atomic_bool cancelled{true};
  SparseVoxelOBJAnalysis unchanged{};
  unchanged.vertexCount = 99u;
  std::string error;
  assert(!analyzeSparseVoxelOBJ(path, unchanged, &error, &cancelled));
  assert(error.find("cancelled") != std::string::npos);
  assert(unchanged.vertexCount == 99u);

  const auto empty = writeFile(directory, "empty.obj", "# no geometry\n");
  assert(!analyzeSparseVoxelOBJ(empty, unchanged, &error));
  assert(error.find("no triangles") != std::string::npos);

  const auto degenerate = writeFile(
      directory,
      "degenerate.obj",
      "v 0 0 0\n"
      "v 1 0 0\n"
      "v 2 0 0\n"
      "f 1 2 3\n");
  assert(!analyzeSparseVoxelOBJ(degenerate, unchanged, &error));
  assert(error.find("no non-degenerate") != std::string::npos);
}

void testUnitsAndSnappedGrid(const TemporaryDirectory &directory)
{
  const auto path = writeFile(
      directory,
      "grid.obj",
      "v 0.1 -0.9 0.2\n"
      "v 2.1 -0.9 0.2\n"
      "v 0.1 1.1 1.2\n"
      "f 1 2 3\n");
  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);

  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Meters), 1.0));
  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Centimeters), 0.01));
  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Millimeters), 0.001));
  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Inches), 0.0254));
  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Feet), 0.3048));
  assert(near(metersPerSourceUnit(SparseVoxelLengthUnit::Custom, 2.5), 2.5));

  SparseVoxelOBJImportSettings settings{};
  settings.voxelSizeMeters = 1.0;
  settings.maximumEstimatedCells = 1024u;
  settings.maximumEstimatedTemporaryBytes = 1024u * 1024u;
  SparseVoxelOBJGridEstimate estimate{};
  std::string error;
  assert(deriveSparseVoxelOBJGrid(analysis, settings, estimate, &error));
  assert(error.empty());
  assert((estimate.occupiedCellsPerAxis == std::array<uint64_t, 3>{3u, 3u, 2u}));
  assert((estimate.rootMinMeters == std::array<double, 3>{0.0, -1.0, 0.0}));
  assert(estimate.maximumLevel == 2u);
  assert(estimate.resolution == 4u);
  assert(near(estimate.rootExtentMeters, 4.0));
  assert((estimate.rootCenterMeters == std::array<double, 3>{2.0, 1.0, 2.0}));
  assert(estimate.conservativeCellCount == 18u);
  assert(estimate.conservativeTemporaryBytes == 576u);
  assert(estimate.conservativePageCount == 1u);

  settings.sourceUnit = SparseVoxelLengthUnit::Centimeters;
  settings.voxelSizeMeters = 0.01;
  assert(deriveSparseVoxelOBJGrid(analysis, settings, estimate, &error));
  assert(near(estimate.meterBounds.extent[0], analysis.sourceBounds.extent[0] * 0.01));
  assert((estimate.occupiedCellsPerAxis == std::array<uint64_t, 3>{3u, 3u, 2u}));

  settings.sourceUnit = SparseVoxelLengthUnit::Custom;
  settings.metersPerSourceUnit = 0.5;
  settings.voxelSizeMeters = 0.5;
  assert(deriveSparseVoxelOBJGrid(analysis, settings, estimate, &error));
  assert(near(estimate.meterBounds.extent[0], 1.0));
}

void testInvalidSettingsAndBudgets(const TemporaryDirectory &directory)
{
  const auto path = writeFile(
      directory,
      "limits.obj",
      "v 0 0 0\n"
      "v 16 0 0\n"
      "v 0 16 1\n"
      "f 1 2 3\n");
  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);
  SparseVoxelOBJImportSettings settings{};
  SparseVoxelOBJGridEstimate unchanged{};
  unchanged.resolution = 777u;
  std::string error;

  settings.voxelSizeMeters = 0.0;
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));
  assert(unchanged.resolution == 777u);
  settings.voxelSizeMeters = std::numeric_limits<double>::quiet_NaN();
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));
  settings.voxelSizeMeters = 1.0;
  settings.sourceUnit = SparseVoxelLengthUnit::Custom;
  settings.metersPerSourceUnit = -1.0;
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));

  settings.sourceUnit = SparseVoxelLengthUnit::Meters;
  settings.maximumLevel = 3u;
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));
  assert(error.find("deeper") != std::string::npos);

  settings.maximumLevel = 16u;
  settings.maximumEstimatedCells = 10u;
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));
  assert(error.find("cell budget") != std::string::npos);

  settings.maximumEstimatedCells = std::numeric_limits<uint64_t>::max();
  settings.maximumEstimatedTemporaryBytes = 64u;
  assert(!deriveSparseVoxelOBJGrid(analysis, settings, unchanged, &error));
  assert(error.find("memory") != std::string::npos);
}

void testSurfaceVoxelBuilder(const TemporaryDirectory &directory)
{
  writeFile(
      directory,
      "builder.mtl",
      "newmtl surface\n"
      "Kd 0.2 0.4 0.8\n"
      "Pr 0.25\n"
      "Pm 0.75\n"
      "Ke 1 2 3\n");
  const auto path = writeFile(
      directory,
      "builder.obj",
      "mtllib builder.mtl\n"
      "v 0 0 0\n"
      "v 2.1 0 0\n"
      "v 0 2.1 0\n"
      "usemtl surface\n"
      "f 1 2 3\n");
  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);
  SparseVoxelOBJImportSettings settings{};
  settings.voxelSizeMeters = 1.0;
  settings.maximumEstimatedCells = 1024u;
  settings.maximumEstimatedTemporaryBytes = 1024u * 1024u;

  SparseVoxelScene first{};
  SparseVoxelOBJBuildStatistics firstStatistics{};
  std::string error;
  if (!buildSparseVoxelOBJScene(analysis, settings, first, &firstStatistics, &error))
  {
    std::fprintf(stderr, "builder failed: %s\n", error.c_str());
    std::abort();
  }
  assert(error.empty());
  assert(first.models.size() == 1u);
  assert(first.instances.size() == 1u);
  assert(firstStatistics.testedCells > 0u);
  assert(firstStatistics.occupiedLeafCount > 0u);
  assert(firstStatistics.hierarchyNodeCount == first.models[0].hierarchyNodes.size());
  assert(first.models[0].maxLevel == 2u);
  assert(first.models[0].rootCount > 0u && first.models[0].rootCount <= 8u);
  assert(validateSparseVoxelSceneSource(first, &error));
  BootstrapSparseVoxelModel previewModel{};
  BootstrapSparseVoxelLOD previewHierarchy{};
  assert(makeBootstrapSparseVoxelPreview(first, previewModel, previewHierarchy, &error));
  assert(previewHierarchy.nodes.size() == first.models[0].hierarchyNodes.size());
  assert(std::memcmp(previewHierarchy.nodes.data(), first.models[0].hierarchyNodes.data(), previewHierarchy.nodes.size() * sizeof(SparseVoxelLODNodeGPU)) == 0);
  assert(previewHierarchy.centersLengths.size() == first.models[0].centersLengths.size());
  assert(previewHierarchy.materials.size() == first.models[0].materials.size());
  assert(previewHierarchy.firstRoot == first.models[0].firstRoot);
  assert(previewHierarchy.rootCount == first.models[0].rootCount);
  assert(previewModel.blas.voxelCount == firstStatistics.occupiedLeafCount);
  assert(validateBootstrapSparseVoxelModel(previewModel, &error));

  bool foundLeaf = false;
  for (uint32_t nodeIndex = 0u; nodeIndex < first.models[0].hierarchyNodes.size(); ++nodeIndex)
  {
    const auto &node = first.models[0].hierarchyNodes[nodeIndex];
    const auto &material = first.models[0].materials[nodeIndex];
    assert((material.albedoRGBA8 >> 24u) == 0xffu);
    if (node.childCount == 0u)
    {
      foundLeaf = true;
      assert(node.level == first.models[0].maxLevel);
      assert((material.albedoRGBA8 & 0xffu) == 51u);
      assert(((material.albedoRGBA8 >> 8u) & 0xffu) == 102u);
      assert(((material.albedoRGBA8 >> 16u) & 0xffu) == 204u);
      assert((material.roughnessMetallicUNorm8 & 0xffu) == 64u);
      assert(((material.roughnessMetallicUNorm8 >> 8u) & 0xffu) == 191u);
      assert(near(halfToFloat(static_cast<uint16_t>(material.emissiveRG16F)), 1.0));
      assert(near(halfToFloat(static_cast<uint16_t>(material.emissiveRG16F >> 16u)), 2.0));
      assert(near(halfToFloat(static_cast<uint16_t>(material.emissiveB16F)), 3.0));
    }
  }
  assert(foundLeaf);

  SparseVoxelScene second{};
  SparseVoxelOBJBuildStatistics secondStatistics{};
  assert(buildSparseVoxelOBJScene(analysis, settings, second, &secondStatistics, &error));
  assert(second.models[0].octreePaths == first.models[0].octreePaths);
  assert(std::memcmp(second.models[0].materials.data(), first.models[0].materials.data(), first.models[0].materials.size() * sizeof(SparseVoxelMaterialGPU)) == 0);

  writeFile(
      directory,
      "builder.mtl",
      "newmtl surface\n"
      "Kd 1 0 0\n");
  SparseVoxelScene staleResult{};
  assert(!buildSparseVoxelOBJScene(analysis, settings, staleResult, nullptr, &error));
  assert(error.find("changed after analysis") != std::string::npos);

  std::atomic_bool cancelled{true};
  SparseVoxelScene unchanged{};
  unchanged.sourceContentHash = 1234u;
  assert(!buildSparseVoxelOBJScene(analysis, settings, unchanged, nullptr, &error, &cancelled));
  assert(error.find("cancelled") != std::string::npos);
  assert(unchanged.sourceContentHash == 1234u);
}

void testExactFullSiblingCoalescing(const TemporaryDirectory &directory)
{
  const auto path = writeFile(
      directory,
      "coalesce.obj",
      "v 0 0 0\n"
      "v 1 0 0\n"
      "v 1 1 0\n"
      "v 0 1 0\n"
      "v 0 0 1\n"
      "v 1 0 1\n"
      "v 1 1 1\n"
      "v 0 1 1\n"
      "v 1.1 1.1 1.1\n"
      "v 1.11 1.1 1.1\n"
      "v 1.1 1.11 1.1\n"
      "f 1 3 2\n"
      "f 1 4 3\n"
      "f 5 6 7\n"
      "f 5 7 8\n"
      "f 1 2 6\n"
      "f 1 6 5\n"
      "f 4 8 7\n"
      "f 4 7 3\n"
      "f 1 5 8\n"
      "f 1 8 4\n"
      "f 2 3 7\n"
      "f 2 7 6\n"
      "f 9 10 11\n");
  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);
  SparseVoxelOBJImportSettings settings{};
  settings.voxelSizeMeters = 0.5;
  settings.maximumEstimatedCells = 4096u;
  settings.maximumEstimatedTemporaryBytes = 1024u * 1024u;
  SparseVoxelScene scene{};
  std::string error;
  assert(buildSparseVoxelOBJScene(analysis, settings, scene, nullptr, &error));
  assert(scene.models[0].maxLevel == 2u);
  const uint64_t coalescedPath = encodeOctreePath(0u, 0u, 0u, 1u);
  bool foundCoalescedLeaf = false;
  for (uint32_t nodeIndex = 0u; nodeIndex < scene.models[0].hierarchyNodes.size(); ++nodeIndex)
  {
    if (scene.models[0].octreeLevels[nodeIndex] == 1u && scene.models[0].octreePaths[nodeIndex] == coalescedPath)
    {
      foundCoalescedLeaf = scene.models[0].hierarchyNodes[nodeIndex].childCount == 0u;
      break;
    }
  }
  assert(foundCoalescedLeaf);
  assert(validateSparseVoxelSceneSource(scene, &error));
}

void testRepositoryModelAnalysis()
{
  const SparseVoxelOBJAnalysis bunny = analyzeFixture(stanfordBunnyPath());
  assert(bunny.vertexCount == 35947u);
  assert(bunny.triangleCount == 69451u);
  assert(bunny.shapeCount > 0u);
  assert(bunny.degenerateTriangleCount < bunny.triangleCount);
  assert(near(bunny.sourceBounds.extent[0], 0.155699, 1.0e-5));
  assert(near(bunny.sourceBounds.extent[1], 0.154334, 1.0e-5));
  assert(near(bunny.sourceBounds.extent[2], 0.120674, 1.0e-5));

  const SparseVoxelOBJAnalysis suzanne = analyzeFixture(suzannePath());
  assert(suzanne.vertexCount == 507u);
  assert(suzanne.triangleCount == 968u);
  assert(suzanne.shapeCount > 0u);
  assert(suzanne.degenerateTriangleCount < suzanne.triangleCount);
  assert(near(suzanne.sourceBounds.extent[0], 2.734375, 1.0e-5));
  assert(near(suzanne.sourceBounds.extent[1], 1.96875, 1.0e-5));
  assert(near(suzanne.sourceBounds.extent[2], 1.703125, 1.0e-5));
}

SparseVoxelScene buildRepositoryModel(const std::filesystem::path &path, const double voxelSize, SparseVoxelOBJBuildStatistics &statistics)
{
  const SparseVoxelOBJAnalysis analysis = analyzeFixture(path);
  SparseVoxelOBJImportSettings settings{};
  settings.voxelSizeMeters = voxelSize;
  settings.maximumEstimatedCells = 8u * 1024u * 1024u;
  settings.maximumEstimatedTemporaryBytes = 512u * 1024u * 1024u;
  SparseVoxelScene scene{};
  std::string error;
  if (!buildSparseVoxelOBJScene(analysis, settings, scene, &statistics, &error))
  {
    std::fprintf(stderr, "repository model build failed for %s: %s\n", path.string().c_str(), error.c_str());
    std::abort();
  }
  assert(validateSparseVoxelSceneSource(scene, &error));
  assert(statistics.testedCells > 0u);
  assert(statistics.occupiedLeafCount > 0u);
  assert(statistics.hierarchyNodeCount >= statistics.occupiedLeafCount);
  assert(scene.sourceInfo.sourceRelativePath == path.filename().generic_string());
  BootstrapSparseVoxelModel previewModel{};
  BootstrapSparseVoxelLOD previewHierarchy{};
  assert(makeBootstrapSparseVoxelPreview(scene, previewModel, previewHierarchy, &error));
  assert(previewHierarchy.nodes.size() == scene.models[0].hierarchyNodes.size());
  return scene;
}

void testRepositoryModelBuilds()
{
  SparseVoxelOBJBuildStatistics bunnyStatistics{};
  SparseVoxelOBJBuildStatistics suzanneStatistics{};
  const SparseVoxelScene bunny = buildRepositoryModel(stanfordBunnyPath(), 0.01, bunnyStatistics);
  const SparseVoxelScene suzanne = buildRepositoryModel(suzannePath(), 0.2, suzanneStatistics);
  assert(bunny.models[0].maxLevel >= 4u);
  assert(suzanne.models[0].maxLevel >= 4u);
  assert(bunny.sourceContentHash != suzanne.sourceContentHash);
}

void testRepositoryModelExplicitSave(const TemporaryDirectory &directory)
{
  SparseVoxelOBJBuildStatistics statistics{};
  const SparseVoxelScene direct = buildRepositoryModel(stanfordBunnyPath(), 0.01, statistics);
  const std::filesystem::path savedPath = directory.path / "stanford-bunny-explicit-save.vsvo";
  assert(!std::filesystem::exists(savedPath));
  std::string error;
  if (!writeVirtualSparseVoxelScene(savedPath.string(), direct, {}, &error))
  {
    std::fprintf(stderr, "Stanford Bunny VSVO save failed: %s\n", error.c_str());
    std::abort();
  }
  SparseVoxelScene loaded{};
  assert(readVirtualSparseVoxelScene(savedPath.string(), loaded, &error));
  assert(loaded.sourceContentHash == direct.sourceContentHash);
  assert(loaded.buildSettingsHash == direct.buildSettingsHash);
  assert(loaded.models[0].octreePaths == direct.models[0].octreePaths);
  assert(std::memcmp(loaded.models[0].materials.data(), direct.models[0].materials.data(), direct.models[0].materials.size() * sizeof(SparseVoxelMaterialGPU)) == 0);

  BootstrapSparseVoxelModel directPreview{};
  BootstrapSparseVoxelModel reloadedPreview{};
  BootstrapSparseVoxelLOD directHierarchy{};
  BootstrapSparseVoxelLOD reloadedHierarchy{};
  assert(makeBootstrapSparseVoxelPreview(direct, directPreview, directHierarchy, &error));
  assert(makeBootstrapSparseVoxelPreview(loaded, reloadedPreview, reloadedHierarchy, &error));
  assert(directHierarchy.nodes.size() == reloadedHierarchy.nodes.size());
  assert(std::memcmp(directHierarchy.nodes.data(), reloadedHierarchy.nodes.data(), directHierarchy.nodes.size() * sizeof(SparseVoxelLODNodeGPU)) == 0);
  assert(std::memcmp(directHierarchy.pathsLevels.data(), reloadedHierarchy.pathsLevels.data(), directHierarchy.pathsLevels.size() * sizeof(SparseVoxelPathLevelGPU)) == 0);
  assert(std::memcmp(directHierarchy.centersLengths.data(), reloadedHierarchy.centersLengths.data(), directHierarchy.centersLengths.size() * sizeof(SparseVoxelCenterLengthGPU)) == 0);
  assert(std::memcmp(directHierarchy.materials.data(), reloadedHierarchy.materials.data(), directHierarchy.materials.size() * sizeof(SparseVoxelMaterialGPU)) == 0);
}

} // namespace

int main(const int argc, char **argv)
{
  const TemporaryDirectory directory;
  const std::string gate = argc > 1 ? argv[1] : "--all";
  if (gate == "--g17a" || gate == "--all")
  {
    testMeasurementAndDiagnostics(directory);
    testUnitsAndSnappedGrid(directory);
    testInvalidSettingsAndBudgets(directory);
    testRepositoryModelAnalysis();
  }
  if (gate == "--g17b" || gate == "--all")
  {
    testSurfaceVoxelBuilder(directory);
    testExactFullSiblingCoalescing(directory);
    testRepositoryModelBuilds();
  }
  if (gate == "--g17c" || gate == "--all")
    testRepositoryModelExplicitSave(directory);
  assert(gate == "--g17a" || gate == "--g17b" || gate == "--g17c" || gate == "--all");
  std::puts("SparseVoxelOBJAnalysisTests: passed");
  return 0;
}
