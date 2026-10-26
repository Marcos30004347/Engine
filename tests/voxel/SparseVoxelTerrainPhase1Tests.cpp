#include "TerrainGenerationPhase0Utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
int failures = 0;

void check(const bool condition, const std::string &message)
{
  std::cout << "  " << (condition ? "PASSED " : "FAILED ") << message << '\n';
  failures += condition ? 0 : 1;
}

bool buildFixture(
    const terrain_phase0::FixtureKind kind, const uint32_t lod,
    svr::PlanetVoxelChunkSet &page, svr::WorldMapTerrainColumnPage &columns,
    std::string &error,
    const std::array<double, 4> *bounds = nullptr)
{
  const auto bundle = terrain_phase0::makeFixture(kind);
  auto settings = terrain_phase0::fixtureSettings(kind, lod);
  if (bounds != nullptr)
    settings.normalizedBounds = *bounds;
  return svr::buildWorldMapVoxelTerrain(
      bundle, settings, page, &error, {}, nullptr, nullptr, &columns);
}

terrain_phase0::SemanticTerrainPage semanticPageFromColumns(
    const svr::WorldMapTerrainColumnPage &columns, std::string &error)
{
  terrain_phase0::SemanticTerrainPage result{};
  result.edgeMetres = columns.edgeMetres;
  result.voxelSizeMetres = columns.voxelSizeMetres;
  result.internalLOD = columns.internalLOD;
  svr::WorldMapTerrainColumnLeafPayload payload{};
  if (!svr::buildWorldMapTerrainColumnLeafPayload(columns, payload, &error))
    return result;
  for (size_t index = 0u; index < payload.leaves.size(); ++index)
  {
    const auto &leaf = payload.leaves[index];
    terrain_phase0::SemanticTerrainCell cell{};
    cell.coordinate = {
      static_cast<int32_t>(std::llround(
          (leaf.center[0] + columns.edgeMetres * 0.5) /
              columns.voxelSizeMetres - 0.5)),
      static_cast<int32_t>(std::llround(
          leaf.center[1] / columns.voxelSizeMetres - 0.5)),
      static_cast<int32_t>(std::llround(
          (leaf.center[2] + columns.edgeMetres * 0.5) /
              columns.voxelSizeMetres - 0.5)),
    };
    cell.material = svr::packSparseVoxelBrickMaterial(payload.materials[index]);
    cell.context = payload.materials[index].flags;
    result.cells.push_back(cell);
  }
  std::sort(
      result.cells.begin(), result.cells.end(),
      [](const auto &left, const auto &right)
      {
        return left.coordinate < right.coordinate;
      });
  return result;
}

uint64_t voxelHeightUnits(const svr::WorldMapTerrainColumnPage &page)
{
  return static_cast<uint64_t>(std::llround(
      page.voxelSizeMetres / page.heightUnitMetres));
}

int64_t elevationOriginUnits(const svr::WorldMapTerrainColumnPage &page)
{
  return svr::quantizeWorldMapTerrainHeight(
      page.elevationOriginMetres, page.heightUnitMetres);
}

int64_t ownedTopFaceUnits(
    const svr::WorldMapTerrainColumnPage &page, const int64_t height)
{
  const int64_t origin = elevationOriginUnits(page);
  const uint64_t units = voxelHeightUnits(page);
  const uint64_t relative = static_cast<uint64_t>(std::max<int64_t>(
      height - origin, 0));
  const uint64_t cells = relative == 0u
      ? 1u : (relative + units - 1u) / units;
  return origin + static_cast<int64_t>(cells * units);
}

int64_t stitchFloorUnits(
    const svr::WorldMapTerrainColumnPage &page, const int64_t height,
    const uint32_t closure)
{
  const int64_t origin = elevationOriginUnits(page);
  const uint64_t units = voxelHeightUnits(page);
  const uint64_t relative = static_cast<uint64_t>(std::max<int64_t>(
      height - origin, 0));
  const uint64_t boundaryTop = relative / units;
  const uint64_t floor = boundaryTop > closure
      ? boundaryTop - closure : 0u;
  return origin + static_cast<int64_t>(floor * units);
}

bool coarseEnvelopeContainsFineShell(
    const svr::WorldMapTerrainColumnPage &fine,
    const svr::PlanetVoxelChunkSet &coarse)
{
  if (coarse.surfaceHeightSide == 0u || coarse.surfaceEnvelope.empty() ||
      coarse.surfaceHeightSide <= coarse.surfaceHeightHalo * 2u)
    return false;
  const uint32_t coarseSide =
      coarse.surfaceHeightSide - coarse.surfaceHeightHalo * 2u;
  if (fine.side <= coarseSide || fine.side % coarseSide != 0u)
    return false;
  const uint32_t scale = fine.side / coarseSide;
  const int64_t fineOrigin = elevationOriginUnits(fine);
  const int64_t fineUnits = static_cast<int64_t>(voxelHeightUnits(fine));
  for (uint32_t z = 0u; z < fine.side; ++z)
    for (uint32_t x = 0u; x < fine.side; ++x)
    {
      const auto &fineColumn = fine.columns[
          static_cast<size_t>(z) * fine.side + x];
      const int64_t fineTop = fineOrigin +
          static_cast<int64_t>(fineColumn.topY + 1u) * fineUnits;
      const size_t coarseIndex =
          static_cast<size_t>(z / scale + coarse.surfaceHeightHalo) *
              coarse.surfaceHeightSide + x / scale + coarse.surfaceHeightHalo;
      const int64_t coarseEnvelope = svr::quantizeWorldMapTerrainHeight(
          coarse.surfaceEnvelope[coarseIndex], fine.heightUnitMetres);
      if (coarseEnvelope < fineTop)
        return false;
    }
  return true;
}

svr::WorldMapTerrainColumnPage randomizedPage()
{
  svr::WorldMapTerrainColumnPage page{};
  page.side = 8u;
  page.maximumVerticalVoxels = 128u;
  page.heightUnitMetres = 1.0 /
      svr::kWorldMapTerrainHeightUnitsPerVoxel;
  page.voxelSizeMetres = 1.0;
  page.edgeMetres = 8.0;
  page.columns.resize(64u);
  page.walls.resize(64u);
  page.canonicalCornerHeights.resize(81u);
  page.canonicalBoundaryMinimumHeights.resize(32u);
  page.canonicalBoundaryMaximumHeights.resize(32u);
  std::mt19937 random(0x4d595df4u);
  for (size_t index = 0u; index < page.columns.size(); ++index)
  {
    const uint32_t top = random() % page.maximumVerticalVoxels;
    const uint32_t first = random() % (top + 1u);
    const uint32_t unstiched = first + random() % (top - first + 1u);
    page.columns[index] = {
      top,
      first,
      uint32_t{0u} |
          (static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface) << 8u),
      first < unstiched ? svr::WorldMapTerrainColumnBoundaryStitched : 0u,
    };
    page.walls[index] = {
      unstiched,
      random() % page.maximumVerticalVoxels,
      static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeX) |
          (static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeZ) << 8u),
      0u,
    };
  }
  return page;
}

} // namespace

int main()
{
  std::cout << "GPU terrain generation Phase 1 canonical columns\n";
  std::cout << "column-fed compatibility\n";
  for (const auto kind : terrain_phase0::kFixtureKinds)
    for (uint32_t lod = 0u; lod < 3u; ++lod)
    {
      svr::PlanetVoxelChunkSet raster{};
      svr::WorldMapTerrainColumnPage columns{};
      std::string error;
      const bool built = buildFixture(kind, lod, raster, columns, error);
      terrain_phase0::SemanticTerrainPage rasterSemantic{};
      const bool decoded = built && terrain_phase0::decodeSemanticTerrainPage(
          raster, 0u, rasterSemantic, &error);
      auto columnSemantic = semanticPageFromColumns(columns, error);
      columnSemantic.edgeMetres = rasterSemantic.edgeMetres;
      columnSemantic.voxelSizeMetres = rasterSemantic.voxelSizeMetres;
      columnSemantic.internalLOD = rasterSemantic.internalLOD;
      columnSemantic.geometryRevision = rasterSemantic.geometryRevision;
      // This differential isolates the canonical leaf contract. Hierarchy
      // node/error parity remains covered by the Phase 0 semantic fixture
      // suite and the production hierarchy is built from this same payload.
      columnSemantic.nodes = rasterSemantic.nodes;
      std::string difference;
      const bool equal = decoded && svr::validateWorldMapTerrainColumns(
          columns, &error) && terrain_phase0::compareSemanticTerrainPages(
              rasterSemantic, columnSemantic, &difference);
      check(equal,
          std::string(terrain_phase0::fixtureName(kind)) + " LOD " +
              std::to_string(lod) +
              " column-fed occupancy/material/context matches raster: " +
              (difference.empty() ? error : difference));
      svr::SparseVoxelTerrainPageTree64 columnTree{};
      bool treeEqual = svr::buildSparseVoxelTerrainPageTree64FromColumns(
          columns, columnTree, &error);
      for (uint32_t z = 0u; treeEqual && z < columns.side; ++z)
        for (uint32_t x = 0u; treeEqual && x < columns.side; ++x)
        {
          const auto &column = columns.columns[
              static_cast<size_t>(z) * columns.side + x];
          for (uint32_t y = 0u; y < columns.maximumVerticalVoxels; ++y)
          {
            const bool expected =
                y >= column.firstSolidY && y <= column.topY;
            if (svr::sparseVoxelTree64Occupied(
                    columnTree.tree, {x, y, z}) != expected)
            {
              treeEqual = false;
              break;
            }
          }
        }
      check(treeEqual,
          std::string(terrain_phase0::fixtureName(kind)) + " LOD " +
              std::to_string(lod) +
              " column-fed Tree64 occupancy matches canonical runs: " + error);
    }

  std::cout << "adjacent page ownership\n";
  const std::array<double, 4> leftBounds{0.375, 0.4375, 0.5, 0.5625};
  const std::array<double, 4> rightBounds{0.5, 0.4375, 0.625, 0.5625};
  svr::PlanetVoxelChunkSet leftRaster{};
  svr::PlanetVoxelChunkSet rightRaster{};
  svr::WorldMapTerrainColumnPage left{};
  svr::WorldMapTerrainColumnPage right{};
  std::string error;
  const bool adjacentBuilt =
      buildFixture(terrain_phase0::FixtureKind::CrossLOD, 0u,
          leftRaster, left, error, &leftBounds) &&
      buildFixture(terrain_phase0::FixtureKind::CrossLOD, 0u,
          rightRaster, right, error, &rightBounds);
  bool cornersEqual = adjacentBuilt && left.side == right.side &&
      left.heightUnitMetres == right.heightUnitMetres;
  bool boundariesEqual = cornersEqual;
  bool ownershipEqual = cornersEqual;
  if (cornersEqual)
    for (uint32_t z = 0u; z <= left.side; ++z)
      cornersEqual = cornersEqual &&
          left.canonicalCornerHeights[
              static_cast<size_t>(z) * (left.side + 1u) + left.side] ==
          right.canonicalCornerHeights[
              static_cast<size_t>(z) * (right.side + 1u)];
  if (boundariesEqual)
    for (uint32_t segment = 0u; segment < left.side; ++segment)
    {
      const size_t leftIndex = static_cast<size_t>(1u) * left.side + segment;
      const size_t rightIndex = segment;
      const int64_t leftMinimum =
          left.canonicalBoundaryMinimumHeights[leftIndex];
      const int64_t rightMinimum =
          right.canonicalBoundaryMinimumHeights[rightIndex];
      const int64_t leftMaximum =
          left.canonicalBoundaryMaximumHeights[leftIndex];
      const int64_t rightMaximum =
          right.canonicalBoundaryMaximumHeights[rightIndex];
      boundariesEqual = boundariesEqual && leftMinimum == rightMinimum &&
          leftMaximum == rightMaximum;
      ownershipEqual = ownershipEqual &&
          ownedTopFaceUnits(left, leftMaximum) ==
              ownedTopFaceUnits(right, rightMaximum) &&
          stitchFloorUnits(left, leftMinimum, 2u) ==
              stitchFloorUnits(right, rightMinimum, 2u);
    }
  check(cornersEqual, "adjacent pages share exact canonical corner heights");
  check(boundariesEqual,
      "adjacent pages share exact boundary minima and maxima");
  check(ownershipEqual,
      "adjacent pages derive identical top and closure ownership");

  // Decimal voxel sizes and page centres far from renderer zero used to take
  // two independent float roundings (local brick + page translation). The
  // nominally shared plane then became a tiny gap on one edge and an overlap
  // on the other, large enough to cover pixels with a centimetre-high camera.
  const auto precisionBundle =
      terrain_phase0::makeFixture(terrain_phase0::FixtureKind::CrossLOD);
  auto precisionSettings = terrain_phase0::fixtureSettings(
      terrain_phase0::FixtureKind::CrossLOD, 0u);
  precisionSettings.finestVoxelSizeMetres = 0.1;
  precisionSettings.finestHorizontalVoxelCount = 32u;
  precisionSettings.voxel.patchEdgeMetres = 3.2;
  precisionSettings.voxel.horizontalVoxelCount = 32u;
  const double normalizedEdge =
      precisionSettings.voxel.patchEdgeMetres / precisionBundle.extentMetres;
  const double sharedU = 0.7625;
  precisionSettings.normalizedBounds = {
    sharedU - normalizedEdge, 0.55,
    sharedU, 0.55 + normalizedEdge};
  svr::PlanetVoxelChunkSet precisionLeft{};
  const bool precisionLeftBuilt = svr::buildWorldMapVoxelTerrain(
      precisionBundle, precisionSettings, precisionLeft, &error);
  precisionSettings.normalizedBounds = {
    sharedU, 0.55,
    sharedU + normalizedEdge, 0.55 + normalizedEdge};
  svr::PlanetVoxelChunkSet precisionRight{};
  const bool precisionRightBuilt = svr::buildWorldMapVoxelTerrain(
      precisionBundle, precisionSettings, precisionRight, &error);
  const float sharedWorldX = static_cast<float>(
      (sharedU - 0.5) * precisionBundle.extentMetres);
  bool leftPlaneFound = false;
  bool rightPlaneFound = false;
  bool legacyFloatMismatchExercised = false;
  float canonicalSharedPlane = 0.0f;
  bool packedPlanesExact = precisionLeftBuilt && precisionRightBuilt;
  if (packedPlanesExact)
  {
    for (const auto &node : precisionLeft.bricks.nodes)
    {
      const float legacyMaximumX = node.minimumVoxelSize[0] +
          node.minimumVoxelSize[3] *
              static_cast<float>(svr::kSparseVoxelBrickSide);
      const int32_t canonicalMaximumX =
          node.canonicalGridOriginScale[0] +
          static_cast<int32_t>(svr::kSparseVoxelBrickSide) *
              node.canonicalGridOriginScale[3];
      const float maximumX = precisionLeft.canonicalWorldGrid[0] +
          static_cast<float>(canonicalMaximumX) *
              precisionLeft.canonicalWorldGrid[3];
      if (std::abs(maximumX - sharedWorldX) <=
          node.minimumVoxelSize[3] * 0.01f)
      {
        if (!leftPlaneFound)
          canonicalSharedPlane = maximumX;
        leftPlaneFound = true;
        packedPlanesExact &= maximumX == canonicalSharedPlane;
        legacyFloatMismatchExercised |= legacyMaximumX != maximumX;
      }
    }
    for (const auto &node : precisionRight.bricks.nodes)
    {
      const float minimumX = precisionRight.canonicalWorldGrid[0] +
          static_cast<float>(node.canonicalGridOriginScale[0]) *
              precisionRight.canonicalWorldGrid[3];
      if (std::abs(minimumX - sharedWorldX) <=
          node.minimumVoxelSize[3] * 0.01f)
      {
        rightPlaneFound = true;
        packedPlanesExact &= minimumX == canonicalSharedPlane;
      }
    }
  }
  check(packedPlanesExact && leftPlaneFound && rightPlaneFound &&
          legacyFloatMismatchExercised,
      "far 10 cm pages pack their shared raster plane to identical floats "
      "(exact=" + std::to_string(packedPlanesExact) +
      " left=" + std::to_string(leftPlaneFound) +
      " right=" + std::to_string(rightPlaneFound) +
      " legacyMismatch=" + std::to_string(legacyFloatMismatchExercised) +
      ")");

  svr::PlanetVoxelChunkSet fineRaster{};
  svr::PlanetVoxelChunkSet coarseRaster{};
  svr::WorldMapTerrainColumnPage fineColumns{};
  svr::WorldMapTerrainColumnPage coarseColumns{};
  const bool lodsBuilt =
      buildFixture(terrain_phase0::FixtureKind::CrossLOD, 0u,
          fineRaster, fineColumns, error) &&
      buildFixture(terrain_phase0::FixtureKind::CrossLOD, 2u,
          coarseRaster, coarseColumns, error);
  check(lodsBuilt && coarseEnvelopeContainsFineShell(
          fineColumns, coarseRaster),
      "coarse ownership envelope conservatively contains the fine shell");

  std::cout << "randomized properties and tie-breaking\n";
  auto randomPage = randomizedPage();
  check(svr::validateWorldMapTerrainColumns(randomPage, &error),
      "random representable columns satisfy the ABI invariants: " + error);
  svr::WorldMapTerrainColumnLeafPayload firstPayload{};
  svr::WorldMapTerrainColumnLeafPayload secondPayload{};
  const bool deterministic =
      svr::buildWorldMapTerrainColumnLeafPayload(
          randomPage, firstPayload, &error) &&
      svr::buildWorldMapTerrainColumnLeafPayload(
          randomPage, secondPayload, &error) &&
      firstPayload.indices == secondPayload.indices &&
      firstPayload.surfaceMaterials == secondPayload.surfaceMaterials &&
      firstPayload.leaves.size() == secondPayload.leaves.size() &&
      firstPayload.materials.size() == secondPayload.materials.size() &&
      std::equal(firstPayload.leaves.begin(), firstPayload.leaves.end(),
          secondPayload.leaves.begin(),
          [](const auto &a, const auto &b)
          {
            return a.center == b.center && a.length == b.length;
          }) &&
      std::equal(firstPayload.materials.begin(), firstPayload.materials.end(),
          secondPayload.materials.begin(),
          [](const auto &a, const auto &b)
          {
            return a.albedoRGBA8 == b.albedoRGBA8 &&
                a.roughnessMetallicUNorm8 == b.roughnessMetallicUNorm8 &&
                a.emissiveRG16F == b.emissiveRG16F &&
                a.emissiveB16F == b.emissiveB16F &&
                a.materialId == b.materialId && a.flags == b.flags &&
                a.reserved0 == b.reserved0 && a.reserved1 == b.reserved1;
          });
  check(deterministic,
      "random columns emit byte-semantic deterministic leaves");

  auto tiePage = randomizedPage();
  tiePage.columns.resize(1u);
  tiePage.walls.resize(1u);
  tiePage.side = 1u;
  tiePage.edgeMetres = 1.0;
  tiePage.canonicalCornerHeights.resize(4u);
  tiePage.canonicalBoundaryMinimumHeights.resize(4u);
  tiePage.canonicalBoundaryMaximumHeights.resize(4u);
  tiePage.columns[0] = {
    10u, 2u,
    uint32_t{0u} |
        (static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface) << 8u),
    svr::WorldMapTerrainColumnExteriorShell |
        svr::WorldMapTerrainColumnBoundaryStitched,
  };
  tiePage.walls[0] = {
    4u, 3u,
    static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeX) |
        (static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeZ) << 8u),
    0u,
  };
  svr::WorldMapTerrainColumnLeafPayload tiePayload{};
  const bool tieBuilt = svr::buildWorldMapTerrainColumnLeafPayload(
      tiePage, tiePayload, &error);
  const bool tieCorrect = tieBuilt && tiePayload.materials.size() == 9u &&
      tiePayload.materials[0].flags ==
          static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeZ) &&
      tiePayload.materials[1].flags ==
          static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeZ) &&
      tiePayload.materials[2].flags ==
          static_cast<uint32_t>(svr::SparseVoxelMicroContext::WallNegativeX) &&
      tiePayload.materials.back().flags ==
          static_cast<uint32_t>(svr::SparseVoxelMicroContext::FlatSurface);
  check(tieCorrect,
      "wall and boundary contexts use stable threshold and face ownership");

  auto invalid = randomPage;
  invalid.columns.front().firstSolidY = invalid.columns.front().topY + 1u;
  check(!svr::validateWorldMapTerrainColumns(invalid, &error),
      "invalid vertical ranges fail closed");
  invalid = randomPage;
  ++invalid.abiVersion;
  check(!svr::validateWorldMapTerrainColumns(invalid, &error),
      "unknown column ABI versions fail closed");

  return failures == 0 ? 0 : 1;
}
