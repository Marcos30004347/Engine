#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "TerrainGenerationPhase0Utils.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace svr = voxel::svraster;
namespace vg = virtualgeometry;

namespace
{

int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool finitePositions(const std::vector<float> &positions)
{
  return std::all_of(
      positions.begin(),
      positions.end(),
      [](const float value) { return std::isfinite(value); });
}

void testProductionTerrainSidecarBuildsRealVirtualGeometryPages()
{
  std::cout << "terrain triangle sidecar plugs into virtual geometry pages\n";
  std::string error;
  const auto bundle =
      terrain_phase0::makeFixture(terrain_phase0::FixtureKind::Cliff);
  const auto terrainSettings =
      terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::Cliff, 0u);
  svr::PlanetVoxelChunkSet chunks{};
  svr::WorldMapVoxelGenerationMetrics metrics{};
  check(
      svr::buildWorldMapVoxelTerrain(
          bundle,
          terrainSettings,
          chunks,
          &error,
          {},
          nullptr,
          &metrics),
      "CPU terrain fixture builds: " + error);

  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  meshSettings.brickLocalGreedy = true;
  meshSettings.maxQuadsPerCluster =
      svr::kSparseVoxelTerrainTriangleClusterDefaultQuadCapacity;
  svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
  check(
      svr::buildSparseVoxelTerrainTrianglePageSidecar(
          chunks,
          0u,
          meshSettings,
          sidecar,
          &error),
      "terrain triangle sidecar builds: " + error);
  check(!sidecar.empty(), "terrain sidecar emits source triangle clusters");
  check(
      svr::validateSparseVoxelTerrainTrianglePageSidecar(
          sidecar,
          meshSettings,
          &error),
      "terrain sidecar validates before VG build: " + error);

  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;
  vg::QuantizationConfig quantization{};
  quantization.padMeshlets = true;

  vg::VirtualGeometryEncodedData encoded{};
  svr::SparseVoxelTerrainTriangleVirtualGeometryBuildStats stats{};
  const auto bridgeStart = std::chrono::steady_clock::now();
  check(
      svr::buildSparseVoxelTerrainTriangleVirtualGeometryAsset(
          sidecar,
          meshSettings,
          buildSettings,
          quantization,
          encoded,
          &stats,
          &error),
      "terrain sidecar builds encoded virtual geometry asset: " + error);
  const double bridgeMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - bridgeStart)
          .count();

  check(!encoded.shapes.empty(), "encoded VG asset has a shape");
  check(!encoded.hierarchy.empty(), "encoded VG asset has hierarchy nodes");
  check(!encoded.pages.empty(), "encoded VG asset has resident page payloads");
  check(stats.sourceClusterCount == sidecar.clusters.size(), "bridge stats preserve source cluster count");
  check(stats.sourceTriangleCount == sidecar.greedyQuads * 2u, "bridge stats preserve exact source triangle count");
  check(stats.encodedPageCount == encoded.pages.size(), "bridge stats count encoded pages");
  check(stats.encodedHierarchyNodeCount == encoded.hierarchy.size(), "bridge stats count encoded hierarchy nodes");
  check(stats.encodedMeshletCount >= sidecar.clusters.size(), "VG build keeps source LOD0 meshlets and may add simplified meshlets");
  check(stats.encodedGroupCount > 0u, "VG build emits group spans for GPU cut selection");

  uint32_t decodedMeshletCount = 0u;
  uint64_t encodedPageWords = 0u;
  bool pagesHaveMeshlets = true;
  bool pageGroupsAreWellFormed = true;
  bool meshletTrianglesValid = true;
  bool meshletVerticesValid = true;
  bool meshletGroupsValid = true;
  bool positionsDecode = true;
  bool positionsAreFinite = true;
  for (const auto &page : encoded.pages)
  {
    const vg::PageBuffer pageBuffer = vg::PageBuffer::encode(page);
    encodedPageWords += pageBuffer.data.size();
    pagesHaveMeshlets &= !pageBuffer.data.empty();
    pageGroupsAreWellFormed &= page.groupCount == page.groupMeshletOffsets.size();
    pagesHaveMeshlets &= !page.meshlets.empty();
    for (uint32_t meshletIndex = 0u; meshletIndex < page.meshlets.size(); ++meshletIndex)
    {
      const auto &meshlet = page.meshlets[meshletIndex];
      meshletTrianglesValid &=
          meshlet.triangle_count > 0u && meshlet.triangle_count <= vg::ClusterSize;
      meshletVerticesValid &= meshlet.vertex_count > 0u;
      meshletGroupsValid &=
          meshlet.pageLocalGroupIndex < std::max<uint32_t>(1u, page.groupCount);
      std::vector<float> positions;
      vg::VirtualGeometryCompressor::decodePositions(
          page,
          meshletIndex,
          positions,
          quantization);
      positionsDecode &= positions.size() == meshlet.vertex_count * 3u;
      positionsAreFinite &= finitePositions(positions);
      ++decodedMeshletCount;
    }
  }
  std::cout << std::fixed << std::setprecision(3)
            << "  stats source_clusters=" << stats.sourceClusterCount
            << " source_triangles=" << stats.sourceTriangleCount
            << " encoded_pages=" << stats.encodedPageCount
            << " encoded_meshlets=" << stats.encodedMeshletCount
            << " encoded_groups=" << stats.encodedGroupCount
            << " encoded_hierarchy_nodes=" << stats.encodedHierarchyNodeCount
            << " bridge_ms=" << bridgeMs << '\n';
  check(pageGroupsAreWellFormed, "encoded page group tables are well formed");
  check(pagesHaveMeshlets, "encoded pages contain meshlets");
  check(encodedPageWords > 0u, "CPU terrain oracle produces scene-installable encoded VG page words");
  check(meshletTrianglesValid, "encoded meshlets respect VG triangle budget");
  check(meshletVerticesValid, "encoded meshlets have vertices");
  check(meshletGroupsValid, "encoded meshlets reference valid page-local groups");
  check(positionsDecode, "encoded meshlet positions decode");
  check(positionsAreFinite, "decoded meshlet positions are finite");
  check(decodedMeshletCount == stats.encodedMeshletCount, "all encoded meshlets were visited");
}

} // namespace

int main()
{
  testProductionTerrainSidecarBuildsRealVirtualGeometryPages();
  if (failures != 0)
  {
    std::cerr << failures << " terrain triangle VG bridge checks failed\n";
    return 1;
  }
  std::cout << "Sparse voxel terrain triangle virtual geometry bridge tests passed\n";
  return 0;
}
