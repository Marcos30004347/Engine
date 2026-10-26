#include "TerrainGenerationPhase0Utils.hpp"

#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelTerrainTriangleMesh.hpp"

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace svr = voxel::svraster;

namespace
{
struct CellKey
{
  int32_t x = 0;
  int32_t y = 0;
  int32_t z = 0;

  bool operator==(const CellKey &other) const
  {
    return x == other.x && y == other.y && z == other.z;
  }
};

struct CellKeyHash
{
  size_t operator()(const CellKey &key) const
  {
    const auto mix = [](uint64_t value)
    {
      value ^= value >> 30u;
      value *= 0xbf58476d1ce4e5b9ull;
      value ^= value >> 27u;
      value *= 0x94d049bb133111ebull;
      value ^= value >> 31u;
      return value;
    };
    uint64_t hash = mix(static_cast<uint32_t>(key.x));
    hash ^= mix(static_cast<uint32_t>(key.y) + 0x9e3779b9u) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    hash ^= mix(static_cast<uint32_t>(key.z) + 0x85ebca6bu) + 0x9e3779b97f4a7c15ull + (hash << 6u) + (hash >> 2u);
    return static_cast<size_t>(hash);
  }
};

struct MaterialKey
{
  uint32_t lo = 0u;
  uint32_t hi = 0u;

  bool operator==(const MaterialKey &other) const
  {
    return lo == other.lo && hi == other.hi;
  }
};

struct Bounds
{
  std::array<int32_t, 3> min{{std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max(), std::numeric_limits<int32_t>::max()}};
  std::array<int32_t, 3> max{{std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::min()}};
};

struct MeshStats
{
  uint64_t occupiedCells = 0u;
  uint64_t naiveQuads = 0u;
  uint64_t greedyQuads = 0u;
  uint64_t greedyArea = 0u;
  uint32_t clusters64 = 0u;
  double elapsedMs = 0.0;
};

using Occupancy = std::unordered_map<CellKey, MaterialKey, CellKeyHash>;

void check(const bool condition, const std::string &message)
{
  if (!condition)
  {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

Bounds computeBounds(const terrain_phase0::SemanticTerrainPage &page)
{
  Bounds bounds{};
  for (const auto &cell : page.cells)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      bounds.min[axis] = std::min(bounds.min[axis], cell.coordinate[axis]);
      bounds.max[axis] = std::max(bounds.max[axis], cell.coordinate[axis]);
    }
  }
  return bounds;
}

Occupancy buildOccupancy(const terrain_phase0::SemanticTerrainPage &page)
{
  Occupancy occupancy;
  occupancy.reserve(page.cells.size() * 2u + 1u);
  for (const auto &cell : page.cells)
  {
    occupancy.insert_or_assign(
        CellKey{cell.coordinate[0], cell.coordinate[1], cell.coordinate[2]},
        MaterialKey{cell.material.packed[0], cell.material.packed[1]});
  }
  return occupancy;
}

bool contains(const Occupancy &occupancy, const int32_t x, const int32_t y, const int32_t z)
{
  return occupancy.find(CellKey{x, y, z}) != occupancy.end();
}

uint64_t countExposedFaces(const Occupancy &occupancy)
{
  static constexpr std::array<std::array<int32_t, 3>, 6> kDirections{{
      {{1, 0, 0}},
      {{-1, 0, 0}},
      {{0, 1, 0}},
      {{0, -1, 0}},
      {{0, 0, 1}},
      {{0, 0, -1}},
  }};
  uint64_t faces = 0u;
  for (const auto &[key, material] : occupancy)
  {
    (void)material;
    for (const auto &direction : kDirections)
      if (!contains(occupancy, key.x + direction[0], key.y + direction[1], key.z + direction[2]))
        ++faces;
  }
  return faces;
}

std::array<int32_t, 3> makeCoord(
    const int axis,
    const int uAxis,
    const int vAxis,
    const int32_t axisValue,
    const int32_t u,
    const int32_t v)
{
  std::array<int32_t, 3> coord{};
  coord[axis] = axisValue;
  coord[uAxis] = u;
  coord[vAxis] = v;
  return coord;
}

uint64_t greedyQuadsForBounds(const Occupancy &occupancy, const Bounds &bounds)
{
  uint64_t quads = 0u;
  for (int axis = 0; axis < 3; ++axis)
  {
    const int uAxis = (axis + 1) % 3;
    const int vAxis = (axis + 2) % 3;
    const int32_t uMin = bounds.min[uAxis];
    const int32_t uMax = bounds.max[uAxis];
    const int32_t vMin = bounds.min[vAxis];
    const int32_t vMax = bounds.max[vAxis];
    const int32_t uCount = uMax >= uMin ? uMax - uMin + 1 : 0;
    const int32_t vCount = vMax >= vMin ? vMax - vMin + 1 : 0;
    if (uCount <= 0 || vCount <= 0)
      continue;

    struct MaskCell
    {
      bool filled = false;
      MaterialKey material{};
    };
    std::vector<MaskCell> mask(static_cast<size_t>(uCount) * static_cast<size_t>(vCount));

    for (int signIndex = 0; signIndex < 2; ++signIndex)
    {
      const int32_t normal = signIndex == 0 ? -1 : 1;
      for (int32_t axisValue = bounds.min[axis]; axisValue <= bounds.max[axis]; ++axisValue)
      {
        std::fill(mask.begin(), mask.end(), MaskCell{});
        for (int32_t v = vMin; v <= vMax; ++v)
        {
          for (int32_t u = uMin; u <= uMax; ++u)
          {
            const auto coord = makeCoord(axis, uAxis, vAxis, axisValue, u, v);
            const auto cell = occupancy.find(CellKey{coord[0], coord[1], coord[2]});
            if (cell == occupancy.end())
              continue;
            auto neighbour = coord;
            neighbour[axis] += normal;
            if (contains(occupancy, neighbour[0], neighbour[1], neighbour[2]))
              continue;
            mask[static_cast<size_t>(v - vMin) * static_cast<size_t>(uCount) + static_cast<size_t>(u - uMin)] = MaskCell{true, cell->second};
          }
        }

        for (int32_t v = 0; v < vCount; ++v)
        {
          for (int32_t u = 0; u < uCount;)
          {
            auto &origin = mask[static_cast<size_t>(v) * static_cast<size_t>(uCount) + static_cast<size_t>(u)];
            if (!origin.filled)
            {
              ++u;
              continue;
            }

            int32_t width = 1;
            while (u + width < uCount)
            {
              const auto &candidate = mask[static_cast<size_t>(v) * static_cast<size_t>(uCount) + static_cast<size_t>(u + width)];
              if (!candidate.filled || !(candidate.material == origin.material))
                break;
              ++width;
            }

            int32_t height = 1;
            for (; v + height < vCount; ++height)
            {
              bool rowMatches = true;
              for (int32_t x = 0; x < width; ++x)
              {
                const auto &candidate = mask[static_cast<size_t>(v + height) * static_cast<size_t>(uCount) + static_cast<size_t>(u + x)];
                if (!candidate.filled || !(candidate.material == origin.material))
                {
                  rowMatches = false;
                  break;
                }
              }
              if (!rowMatches)
                break;
            }

            for (int32_t y = 0; y < height; ++y)
              for (int32_t x = 0; x < width; ++x)
                mask[static_cast<size_t>(v + y) * static_cast<size_t>(uCount) + static_cast<size_t>(u + x)].filled = false;
            ++quads;
            u += width;
          }
        }
      }
    }
  }
  return quads;
}

Bounds brickBoundsFromKey(const CellKey &brickKey)
{
  Bounds bounds{};
  bounds.min = {{brickKey.x * 8, brickKey.y * 8, brickKey.z * 8}};
  bounds.max = {{brickKey.x * 8 + 7, brickKey.y * 8 + 7, brickKey.z * 8 + 7}};
  return bounds;
}

CellKey brickKeyForCell(const terrain_phase0::SemanticTerrainCell &cell)
{
  const auto floorDiv8 = [](const int32_t value)
  {
    return value >= 0 ? value / 8 : -((7 - value) / 8);
  };
  return CellKey{
      floorDiv8(cell.coordinate[0]),
      floorDiv8(cell.coordinate[1]),
      floorDiv8(cell.coordinate[2])};
}

MeshStats measureGreedyMesh(
    const terrain_phase0::SemanticTerrainPage &page,
    const bool brickLocal)
{
  const auto occupancy = buildOccupancy(page);
  MeshStats stats{};
  stats.occupiedCells = occupancy.size();
  const auto start = std::chrono::steady_clock::now();
  stats.naiveQuads = countExposedFaces(occupancy);
  if (brickLocal)
  {
    std::unordered_map<CellKey, uint32_t, CellKeyHash> bricks;
    bricks.reserve(page.cells.size() / 8u + 1u);
    for (const auto &cell : page.cells)
      bricks[brickKeyForCell(cell)]++;
    for (const auto &[brickKey, count] : bricks)
    {
      (void)count;
      stats.greedyQuads += greedyQuadsForBounds(occupancy, brickBoundsFromKey(brickKey));
    }
  }
  else
  {
    stats.greedyQuads = greedyQuadsForBounds(occupancy, computeBounds(page));
  }
  stats.greedyArea = stats.naiveQuads;
  stats.clusters64 = static_cast<uint32_t>((stats.greedyQuads + 63u) / 64u);
  stats.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return stats;
}

uint64_t estimatePackedClusterBytes(const uint64_t quads)
{
  // Simple first path: four unshared vertices per quad. Keep the layout GPU
  // friendly: quantized local position + packed normal/face + uv + material.
  constexpr uint64_t kVertexBytes = 16u;
  constexpr uint64_t kIndexBytesPerQuad = 6u * sizeof(uint16_t);
  constexpr uint64_t kClusterHeaderBytesPer64Quads = 64u;
  const uint64_t clusters = (quads + 63u) / 64u;
  return quads * (4u * kVertexBytes + kIndexBytesPerQuad) + clusters * kClusterHeaderBytesPer64Quads;
}

svr::PlanetVoxelChunkSet isolatedSolidBrickChunk()
{
  svr::PlanetVoxelChunkSet chunks{};
  chunks.edgeMetres = 8.0;
  chunks.voxelSizeMetres = 1.0f;
  chunks.minimumWorldMetres = {0.0, 0.0, 0.0};
  chunks.maximumWorldMetres = {8.0, 8.0, 8.0};
  chunks.bricks.finestVoxelSize = 1.0f;
  chunks.bricks.levelCount = 1u;
  chunks.bricks.rootCount = 1u;
  chunks.bricks.rootIndices = {0u};
  chunks.bricks.paletteStride = 1u;
  chunks.bricks.paletteInstanceCount = 1u;
  chunks.bricks.paletteExactInstances = 1u;

  svr::SparseVoxelBrickNodeGPU node{};
  node.minimumVoxelSize = {0.0f, 0.0f, 0.0f, 1.0f};
  node.occupancyIndex = 0u;
  node.materialBase = 0u;
  node.paletteBase = 0u;
  node.instanceIndex = 0u;
  node.occupiedCount = svr::kSparseVoxelBrickVoxelCount;
  node.selfError = 0.25f;
  node.parentError = 0.5f;
  chunks.bricks.nodes.push_back(node);

  svr::SparseVoxelBrickOccupancyGPU occupancy{};
  uint32_t prefix = 0u;
  for (auto &word : occupancy.cells)
  {
    word.bits = 0xffffffffu;
    word.prefix = prefix;
    prefix += 32u;
  }
  chunks.bricks.occupancy.push_back(occupancy);
  chunks.bricks.materialIndices.assign(
      svr::kSparseVoxelBrickVoxelCount, 0u);
  chunks.bricks.microSurfaceData.assign(
      svr::kSparseVoxelBrickVoxelCount, 0u);
  chunks.bricks.palette.push_back({{0x80ffffffu, 0u}});
  chunks.bricks.paletteDescriptorIds.push_back(0u);

  svr::PlanetVoxelChunkRecord record{};
  record.clipmapLOD = 0u;
  record.clipmapSlot = 0u;
  record.regionMappingId = 1u;
  record.worldCenterXMetres = 4.0;
  record.worldCenterZMetres = 4.0;
  record.edgeMetres = 8.0;
  record.voxelSizeMetres = 1.0;
  record.brickNodeOffset = 0u;
  record.brickNodeCapacity = 1u;
  record.brickMaterialOffset = 0u;
  record.brickMaterialCapacity = svr::kSparseVoxelBrickVoxelCount;
  record.brickPaletteOffset = 0u;
  record.brickPaletteCapacity = 1u;
  record.brickPayloadRevision = 1u;
  record.brickNodeRevision = 1u;
  record.tree64Revision = 1u;
  record.firstBrickRoot = 0u;
  record.brickRootCapacity = 1u;
  record.brickRootCount = 1u;
  record.elevationOriginMetres = 0.0;
  record.minimumSurfaceMetres = 0.0;
  record.maximumSurfaceMetres = 8.0;
  record.centerSurfaceMetres = 4.0;
  record.leafVoxelCount = svr::kSparseVoxelBrickVoxelCount;
  record.brickCount = 1u;
  record.residencyGeneration = 1u;
  chunks.chunks.push_back(record);
  return chunks;
}

void testIsolatedSolidBrickEmitsAllSixVoxelFaces()
{
  const auto chunks = isolatedSolidBrickChunk();
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  meshSettings.emitOneQuadPerExposedFace = true;
  meshSettings.brickLocalGreedy = true;
  meshSettings.maxQuadsPerCluster =
      svr::kSparseVoxelTerrainTriangleClusterDefaultQuadCapacity;
  svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
  std::string error;
  check(
      svr::buildSparseVoxelTerrainTrianglePageSidecar(
          chunks, 0u, meshSettings, sidecar, &error),
      "isolated solid brick triangle sidecar builds: " + error);
  check(
      svr::validateSparseVoxelTerrainTrianglePageSidecar(
          sidecar, meshSettings, &error),
      "isolated solid brick triangle sidecar validates: " + error);
  check(
      sidecar.occupiedCells == svr::kSparseVoxelBrickVoxelCount,
      "isolated solid brick preserves all occupied voxels");
  check(
      sidecar.exposedFaces == 6u * 8u * 8u,
      "isolated solid brick exposes exactly six 8x8 sides");
  check(
      sidecar.greedyQuads == sidecar.exposedFaces,
      "isolated solid brick emits one quad for every exposed voxel face");
  check(
      sidecar.vertices.size() == sidecar.exposedFaces * 4u,
      "isolated solid brick uses four private vertices per face");
  check(
      sidecar.indices.size() == sidecar.exposedFaces * 6u,
      "isolated solid brick emits two triangles per face");
  std::array<uint32_t, 6u> faceQuads{};
  for (uint32_t vertex = 0u;
       vertex + 3u < static_cast<uint32_t>(sidecar.vertices.size());
       vertex += 4u)
  {
    const uint8_t face = sidecar.vertices[vertex].face;
    check(face < faceQuads.size(), "isolated solid brick face id is in range");
    for (uint32_t corner = 1u; corner < 4u; ++corner)
      check(
          sidecar.vertices[vertex + corner].face == face,
          "isolated solid brick quad keeps one face id");
    ++faceQuads[face];
  }
  for (uint32_t face = 0u; face < faceQuads.size(); ++face)
    check(
        faceQuads[face] == 64u,
        "isolated solid brick emits all 64 quads for face " +
            std::to_string(face));
}

} // namespace

int main()
{
  std::cout << std::fixed << std::setprecision(3);
  std::cout << "Sparse voxel terrain triangle meshing feasibility\n";
  std::cout << "fixture,lod,cells,naive_quads,brick_greedy_quads,page_greedy_quads,brick_reduction,page_reduction,brick_clusters64,brick_bytes_kib,page_bytes_kib,brick_ms,page_ms\n";

  testIsolatedSolidBrickEmitsAllSixVoxelFaces();

  uint64_t totalNaive = 0u;
  uint64_t totalBrickGreedy = 0u;
  uint64_t totalPageGreedy = 0u;
  double totalBrickMs = 0.0;
  double totalPageMs = 0.0;
  std::string error;

  for (const auto kind : terrain_phase0::kFixtureKinds)
  {
    const auto bundle = terrain_phase0::makeFixture(kind);
    const auto settings = terrain_phase0::fixtureSettings(kind, 0u);
    svr::PlanetVoxelChunkSet chunks{};
    svr::WorldMapVoxelGenerationMetrics metrics{};
    check(svr::buildWorldMapVoxelTerrain(bundle, settings, chunks, &error, {}, nullptr, &metrics), "CPU terrain build failed for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);

    terrain_phase0::SemanticTerrainPage page{};
    check(terrain_phase0::decodeSemanticTerrainPage(chunks, 0u, page, &error), "semantic decode failed for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    check(!page.cells.empty(), "fixture emitted terrain cells");

    const MeshStats brick = measureGreedyMesh(page, true);
    const MeshStats pageWide = measureGreedyMesh(page, false);
    svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
    meshSettings.brickLocalGreedy = true;
    meshSettings.maxQuadsPerCluster =
      svr::kSparseVoxelTerrainTriangleClusterDefaultQuadCapacity;
    svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
    check(svr::buildSparseVoxelTerrainTrianglePageSidecar(chunks, 0u, meshSettings, sidecar, &error), "production triangle sidecar builds for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    svr::SparseVoxelTerrainTrianglePageSidecar repeatedSidecar{};
    check(svr::buildSparseVoxelTerrainTrianglePageSidecar(chunks, 0u, meshSettings, repeatedSidecar, &error), "production triangle sidecar rebuilds for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    check(svr::validateSparseVoxelTerrainTrianglePageSidecar(sidecar, meshSettings, &error), "production triangle sidecar validates for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    check(svr::validateSparseVoxelTerrainTrianglePageSidecar(repeatedSidecar, meshSettings, &error), "repeated production triangle sidecar validates for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    const auto vgSummary = svr::summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview(sidecar);
    virtualgeometry::VirtualGeometryBuildData vgPreview{};
    check(svr::buildSparseVoxelTerrainTriangleVirtualGeometryPreview(sidecar, vgPreview, &error), "production triangle sidecar converts to a virtual-geometry preview for " + std::string(terrain_phase0::fixtureName(kind)) + ": " + error);
    check(brick.naiveQuads > 0u, "naive face extraction sees a surface");
    check(brick.greedyQuads > 0u && brick.greedyQuads <= brick.naiveQuads, "brick-local greedy never emits more quads than exposed faces");
    check(pageWide.greedyQuads > 0u && pageWide.greedyQuads <= brick.greedyQuads, "page-wide greedy is the optimistic lower bound");
    check(sidecar.occupiedCells == brick.occupiedCells, "production triangle sidecar preserves occupied cell count for " + std::string(terrain_phase0::fixtureName(kind)) + ": sidecar=" + std::to_string(sidecar.occupiedCells) + " oracle=" + std::to_string(brick.occupiedCells));
    check(sidecar.exposedFaces == brick.naiveQuads, "production triangle sidecar matches exposed-face oracle");
    check(sidecar.greedyQuads == sidecar.exposedFaces, "production triangle sidecar emits one quad per exposed voxel face");
    check(sidecar.vertices.size() == sidecar.exposedFaces * 4u, "production triangle sidecar emits four private vertices per voxel face");
    check(sidecar.indices.size() == sidecar.exposedFaces * 6u, "production triangle sidecar emits two triangles per voxel face");
    check(!sidecar.clusters.empty(), "production triangle sidecar emits clusters");
    for (const auto &cluster : sidecar.clusters)
      check(cluster.quadCount > 0u && cluster.quadCount <= meshSettings.maxQuadsPerCluster, "production triangle sidecar keeps each cluster within the 64-quad meshlet budget");
    check(svr::sparseVoxelTerrainTrianglePageSidecarHash(sidecar) == svr::sparseVoxelTerrainTrianglePageSidecarHash(repeatedSidecar), "production triangle sidecar output is deterministic");
    check(vgSummary.virtualGeometryCompatible && vgSummary.maxTrianglesPerMeshlet <= virtualgeometry::ClusterSize, "production triangle sidecar is virtual-geometry meshlet compatible");
    check(vgPreview.clusters.size() == sidecar.clusters.size() && vgPreview.lodLevelHierarchy.size() == 1u && vgPreview.pages.size() == 1u, "virtual-geometry preview mirrors sidecar clusters into one page");
    if (kind == terrain_phase0::FixtureKind::Flat)
    {
      auto corrupted = sidecar;
      corrupted.indices.back() = static_cast<uint32_t>(corrupted.vertices.size());
      check(!svr::validateSparseVoxelTerrainTrianglePageSidecar(corrupted, meshSettings, &error), "triangle sidecar validator rejects out-of-range indices");
      corrupted = sidecar;
      corrupted.clusters.front().quadCount = meshSettings.maxQuadsPerCluster + 1u;
      check(!svr::validateSparseVoxelTerrainTrianglePageSidecar(corrupted, meshSettings, &error), "triangle sidecar validator rejects over-budget clusters");
      corrupted = sidecar;
      corrupted.vertices.front().normal = {1, 1, 0};
      check(!svr::validateSparseVoxelTerrainTrianglePageSidecar(corrupted, meshSettings, &error), "triangle sidecar validator rejects invalid normals");
      corrupted = sidecar;
      if (corrupted.indices.size() >= 12u)
      {
        corrupted.indices[6u] = corrupted.indices[0u];
        check(!svr::validateSparseVoxelTerrainTrianglePageSidecar(corrupted, meshSettings, &error), "triangle sidecar validator rejects cross-face vertex sharing");
      }
      corrupted = sidecar;
      if (corrupted.vertices.size() >= 4u)
      {
        corrupted.vertices[1u].uv = {0.5f, 0.5f};
        check(!svr::validateSparseVoxelTerrainTrianglePageSidecar(corrupted, meshSettings, &error), "triangle sidecar validator rejects non-canonical face UV unwrap");
      }
    }

    const double brickReduction = 1.0 - static_cast<double>(brick.greedyQuads) / static_cast<double>(brick.naiveQuads);
    const double pageReduction = 1.0 - static_cast<double>(pageWide.greedyQuads) / static_cast<double>(brick.naiveQuads);
    std::cout << terrain_phase0::fixtureName(kind) << ",0,"
              << brick.occupiedCells << ','
              << brick.naiveQuads << ','
              << brick.greedyQuads << ','
              << pageWide.greedyQuads << ','
              << brickReduction << ','
              << pageReduction << ','
              << brick.clusters64 << ','
              << static_cast<double>(estimatePackedClusterBytes(brick.greedyQuads)) / 1024.0 << ','
              << static_cast<double>(estimatePackedClusterBytes(pageWide.greedyQuads)) / 1024.0 << ','
              << brick.elapsedMs << ','
              << pageWide.elapsedMs << '\n';

    totalNaive += brick.naiveQuads;
    totalBrickGreedy += brick.greedyQuads;
    totalPageGreedy += pageWide.greedyQuads;
    totalBrickMs += brick.elapsedMs;
    totalPageMs += pageWide.elapsedMs;
  }

  check(totalBrickGreedy < totalNaive, "brick-local greedy reduces terrain face count across fixtures");
  check(totalPageGreedy <= totalBrickGreedy, "page-wide greedy remains an upper-bound target for cross-brick merging");
  std::cout << "total,0,0,"
            << totalNaive << ','
            << totalBrickGreedy << ','
            << totalPageGreedy << ','
            << (1.0 - static_cast<double>(totalBrickGreedy) / static_cast<double>(totalNaive)) << ','
            << (1.0 - static_cast<double>(totalPageGreedy) / static_cast<double>(totalNaive)) << ','
            << ((totalBrickGreedy + 63u) / 64u) << ','
            << static_cast<double>(estimatePackedClusterBytes(totalBrickGreedy)) / 1024.0 << ','
            << static_cast<double>(estimatePackedClusterBytes(totalPageGreedy)) / 1024.0 << ','
            << totalBrickMs << ','
            << totalPageMs << '\n';
  return 0;
}
