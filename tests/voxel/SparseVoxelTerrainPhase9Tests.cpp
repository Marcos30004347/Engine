#include "TerrainGenerationPhase0Utils.hpp"
#include "voxel/svraster/TerrainGPURasterBuild.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace
{

namespace svr = voxel::svraster;

template <typename T> bool bytesEqual(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

svr::WorldMapTerrainColumnHierarchyPayload reference(const svr::WorldMapTerrainColumnPage &columns)
{
  svr::WorldMapTerrainColumnLeafPayload leaves{};
  std::string error;
  assert(svr::buildWorldMapTerrainColumnLeafPayload(columns, leaves, &error));
  svr::SparseVoxelBrickBuildSettings settings{};
  settings.voxelSize = static_cast<float>(columns.voxelSizeMetres);
  svr::WorldMapTerrainColumnHierarchyPayload result{};
  assert(svr::buildSparseVoxelBrickHierarchy(leaves.leaves, leaves.materials, leaves.indices, settings, result.hierarchy, &error));
  result.surfaceMaterials = std::move(leaves.surfaceMaterials);
  result.occupiedFineVoxelCount = leaves.leaves.size();
  result.peakTemporaryBrickBytes =
      uint64_t{leaves.leaves.capacity()} * sizeof(leaves.leaves.front()) + uint64_t{leaves.materials.capacity()} * sizeof(leaves.materials.front()) + uint64_t{leaves.indices.capacity()} * sizeof(leaves.indices.front());
  return result;
}

void assertEquivalent(const svr::WorldMapTerrainColumnHierarchyPayload &direct, const svr::WorldMapTerrainColumnHierarchyPayload &oracle)
{
  const auto &left = direct.hierarchy;
  const auto &right = oracle.hierarchy;
  assert(svr::terrainGPURasterSemanticHash(left) == svr::terrainGPURasterSemanticHash(right));
  assert(bytesEqual(left.nodes, right.nodes));
  assert(bytesEqual(left.occupancy, right.occupancy));
  assert(left.materialIndices == right.materialIndices);
  assert(left.microSurfaceData == right.microSurfaceData);
  assert(bytesEqual(left.palette, right.palette));
  assert(left.paletteDescriptorIds == right.paletteDescriptorIds);
  assert(left.childIndices == right.childIndices);
  assert(left.rootIndices == right.rootIndices);
  assert(left.rootCount == right.rootCount);
  assert(left.levelCount == right.levelCount);
  assert(left.finestVoxelSize == right.finestVoxelSize);
  assert(left.paletteStride == right.paletteStride);
  assert(left.paletteInstanceCount == right.paletteInstanceCount);
  assert(left.paletteExactInstances == right.paletteExactInstances);
  assert(left.paletteEmittedVoxels == right.paletteEmittedVoxels);
  assert(direct.surfaceMaterials == oracle.surfaceMaterials);
  assert(direct.occupiedFineVoxelCount == oracle.occupiedFineVoxelCount);
}

void testDirectColumnHierarchyMatrix()
{
  uint64_t directTemporaryBrickBytes = 0u;
  uint64_t referenceTemporaryLeafBytes = 0u;
  for (const auto kind : terrain_phase0::kFixtureKinds)
    for (uint32_t lod = 0u; lod <= 2u; ++lod)
    {
      const auto bundle = terrain_phase0::makeFixture(kind);
      auto settings = terrain_phase0::fixtureSettings(kind, lod);
      svr::PlanetVoxelChunkSet production{};
      svr::WorldMapTerrainColumnPage columns{};
      std::string error;
      assert(svr::buildWorldMapVoxelTerrain(bundle, settings, production, &error, {}, nullptr, nullptr, &columns));

      svr::WorldMapTerrainColumnHierarchyPayload direct{};
      assert(svr::buildWorldMapTerrainColumnHierarchy(columns, settings.voxel.maximumBricks, direct, &error));
      const auto oracle = reference(columns);
      assertEquivalent(direct, oracle);
      assert(direct.fineBrickCount != 0u);
      assert(direct.peakTemporaryBrickBytes != 0u);
      directTemporaryBrickBytes += direct.peakTemporaryBrickBytes;
      referenceTemporaryLeafBytes += oracle.peakTemporaryBrickBytes;
      // The returned page is relocated into the combined world hierarchy, so
      // node positions and links are intentionally different after packing.
      // Counts and semantic column identity remain unchanged.
      assert(production.leafVoxelCount == direct.occupiedFineVoxelCount);
      assert(!production.bricks.empty());

      svr::WorldMapTerrainColumnHierarchyPayload repeated{};
      assert(svr::buildWorldMapTerrainColumnHierarchy(columns, direct.fineBrickCount, repeated, &error));
      assertEquivalent(repeated, oracle);
      if (direct.fineBrickCount > 1u)
      {
        svr::WorldMapTerrainColumnHierarchyPayload overflow{};
        assert(!svr::buildWorldMapTerrainColumnHierarchy(columns, direct.fineBrickCount - 1u, overflow, &error));
      }
    }
  // Very sparse coarse pages can make fixed-capacity brick records larger than
  // their expanded leaf arrays. Across the representative matrix, the direct
  // adapter must still contract the temporary representation it replaces.
  assert(directTemporaryBrickBytes < referenceTemporaryLeafBytes);
  std::cout << "Phase 9 temporary representation: direct bricks " << directTemporaryBrickBytes << " bytes, reference leaves " << referenceTemporaryLeafBytes << " bytes\n";
}

void testReferenceAdapterRemainsIndependent()
{
  const auto bundle = terrain_phase0::makeFixture(terrain_phase0::FixtureKind::Cliff);
  const auto settings = terrain_phase0::fixtureSettings(terrain_phase0::FixtureKind::Cliff, 0u);
  svr::PlanetVoxelChunkSet production{};
  svr::WorldMapTerrainColumnPage columns{};
  std::string error;
  assert(svr::buildWorldMapVoxelTerrain(bundle, settings, production, &error, {}, nullptr, nullptr, &columns));
  svr::WorldMapTerrainColumnLeafPayload leaves{};
  assert(svr::buildWorldMapTerrainColumnLeafPayload(columns, leaves, &error));
  assert(!leaves.leaves.empty());
  assert(leaves.leaves.size() == leaves.materials.size());
  assert(leaves.leaves.size() == leaves.indices.size());
}

} // namespace

int main()
{
  testDirectColumnHierarchyMatrix();
  testReferenceAdapterRemainsIndependent();
  std::cout << "Sparse voxel terrain Phase 9 cleanup tests passed\n";
  return 0;
}
