// Voxel-native virtual geometry build: the octree is the LOD DAG.
//
// The previous terrain triangle bridge handed exposed-face clusters to the
// generic virtual geometry builder, which simplifies with meshoptimizer. Voxel
// face soup has no shared edges to collapse, so that build produced exactly as
// many "simplified" meshlets as it was given source clusters and left every
// parent error at infinity - a renderer that can only ever draw LOD0. These
// tests pin the replacement: mesh every octree level, take the DAG and the
// errors from the brick hierarchy, and never call a triangle simplifier.

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "TerrainGenerationPhase0Utils.hpp"
#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
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

struct Fixture
{
  svr::PlanetVoxelChunkSet chunks;
  bool valid = false;
};

Fixture buildFixture(const terrain_phase0::FixtureKind kind)
{
  Fixture fixture{};
  std::string error;
  const auto bundle = terrain_phase0::makeFixture(kind);
  const auto settings = terrain_phase0::fixtureSettings(kind, 0u);
  svr::WorldMapVoxelGenerationMetrics metrics{};
  fixture.valid = svr::buildWorldMapVoxelTerrain(
      bundle, settings, fixture.chunks, &error, {}, nullptr, &metrics);
  if (!fixture.valid)
    std::cout << "  fixture build failed: " << error << '\n';
  return fixture;
}

// Node span of one chunk inside the shared hierarchy.
struct ChunkSpan
{
  uint32_t first = 0u;
  uint32_t count = 0u;
};

ChunkSpan chunkSpan(const svr::PlanetVoxelChunkSet &chunks, const uint32_t chunkIndex)
{
  const auto &record = chunks.chunks[chunkIndex];
  return {record.brickNodeOffset, record.brickNodeCapacity};
}

// ── mesher unit checks ──────────────────────────────────────────────────────

svr::SparseVoxelBrickOccupancyGPU occupancyFromCells(
    const std::vector<uint32_t> &cells)
{
  svr::SparseVoxelBrickOccupancyGPU occupancy{};
  for (const uint32_t cell : cells)
    occupancy.cells[cell / 32u].bits |= uint32_t{1u} << (cell % 32u);
  uint32_t prefix = 0u;
  for (uint32_t word = 0u; word < svr::kSparseVoxelBrickOccupancyWords; ++word)
  {
    occupancy.cells[word].prefix = prefix;
    prefix += static_cast<uint32_t>(__builtin_popcount(occupancy.cells[word].bits));
  }
  return occupancy;
}

uint32_t linearCell(const uint32_t x, const uint32_t y, const uint32_t z)
{
  return x + svr::kSparseVoxelBrickSide * y +
      svr::kSparseVoxelBrickSide * svr::kSparseVoxelBrickSide * z;
}

void testExposedFaceMasks()
{
  std::cout << "brick exposed-face masks\n";

  const auto single = occupancyFromCells({linearCell(3u, 4u, 5u)});
  svr::SparseVoxelBrickFaceMasks exposed{};
  svr::SparseVoxelBrickNeighbourOccupancy neighbours{};
  check(
      svr::sparseVoxelBrickExposedFaces(single, neighbours, exposed) == 6u,
      "an isolated voxel exposes six faces");

  // A 2x2x2 block: 8 voxels, 24 exposed faces, 24 hidden.
  std::vector<uint32_t> block;
  for (uint32_t z = 0u; z < 2u; ++z)
    for (uint32_t y = 0u; y < 2u; ++y)
      for (uint32_t x = 0u; x < 2u; ++x)
        block.push_back(linearCell(x + 2u, y + 2u, z + 2u));
  const auto cube = occupancyFromCells(block);
  check(
      svr::sparseVoxelBrickExposedFaces(cube, neighbours, exposed) == 24u,
      "a 2x2x2 block hides its interior faces");

  // A full brick against empty space: only the six outer 8x8 faces.
  std::vector<uint32_t> everyCell;
  for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    everyCell.push_back(cell);
  const auto solid = occupancyFromCells(everyCell);
  check(
      svr::sparseVoxelBrickExposedFaces(solid, neighbours, exposed) == 6u * 64u,
      "a solid brick exposes only its six outer faces");

  // The same brick with a solid neighbour on +X loses that whole face.
  svr::SparseVoxelBrickNeighbourOccupancy withNeighbour{};
  withNeighbour.neighbours[1] = &solid;
  check(
      svr::sparseVoxelBrickExposedFaces(solid, withNeighbour, exposed) == 5u * 64u,
      "a same-level neighbour closes the shared face");

  // -X of a solid neighbour, to prove both signs of the cross-brick test.
  svr::SparseVoxelBrickNeighbourOccupancy minusX{};
  minusX.neighbours[0] = &solid;
  check(
      svr::sparseVoxelBrickExposedFaces(solid, minusX, exposed) == 5u * 64u,
      "the negative-direction neighbour test is symmetric");

  svr::SparseVoxelBrickNeighbourOccupancy plusZ{};
  plusZ.neighbours[5] = &solid;
  check(
      svr::sparseVoxelBrickExposedFaces(solid, plusZ, exposed) == 5u * 64u,
      "the z-slab neighbour test crosses bricks");
}

void testQuadWindingAndMerging()
{
  std::cout << "greedy quads wind outward and merge inside blocks\n";

  std::vector<uint32_t> everyCell;
  for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    everyCell.push_back(cell);
  const auto solid = occupancyFromCells(everyCell);
  svr::SparseVoxelBrickFaceMasks exposed{};
  svr::SparseVoxelBrickNeighbourOccupancy neighbours{};
  svr::sparseVoxelBrickExposedFaces(solid, neighbours, exposed);

  std::array<uint8_t, svr::kSparseVoxelBrickVoxelCount> materials{};
  materials.fill(7u);

  std::vector<svr::SparseVoxelBrickSurfaceQuad> merged;
  svr::sparseVoxelBrickGreedyQuads(
      exposed, materials, svr::kSparseVoxelBrickSide, true, merged);
  check(merged.size() == 6u, "a solid brick merges to one quad per outer face");

  std::vector<svr::SparseVoxelBrickSurfaceQuad> unmerged;
  svr::sparseVoxelBrickGreedyQuads(exposed, materials, 1u, true, unmerged);
  check(unmerged.size() == 6u * 64u, "block side one disables merging");

  std::vector<svr::SparseVoxelBrickSurfaceQuad> halfBlocks;
  svr::sparseVoxelBrickGreedyQuads(exposed, materials, 4u, true, halfBlocks);
  check(
      halfBlocks.size() == 6u * 4u,
      "merging never crosses a block boundary");
  bool insideBlocks = true;
  for (const auto &quad : halfBlocks)
  {
    const auto axes = svr::sparseVoxelBrickFaceAxes(quad.direction);
    insideBlocks = insideBlocks &&
        (quad.origin[axes[0]] % 4u) + quad.width <= 4u &&
        (quad.origin[axes[1]] % 4u) + quad.height <= 4u;
  }
  check(insideBlocks, "every quad stays inside one block");

  // Different materials must not merge. Splitting the top slab in x halves the
  // +Z face and leaves the untouched -Z face whole.
  materials.fill(7u);
  for (uint32_t x = 0u; x < 4u; ++x)
    for (uint32_t y = 0u; y < svr::kSparseVoxelBrickSide; ++y)
      materials[linearCell(x, y, svr::kSparseVoxelBrickSide - 1u)] = 9u;
  std::vector<svr::SparseVoxelBrickSurfaceQuad> split;
  svr::sparseVoxelBrickGreedyQuads(
      exposed, materials, svr::kSparseVoxelBrickSide, true, split);
  std::array<uint32_t, svr::kSparseVoxelBrickFaceDirections> quadsPerDirection{};
  for (const auto &quad : split)
    ++quadsPerDirection[quad.direction];
  check(quadsPerDirection[5] == 2u, "a material split halves the +Z face");
  check(quadsPerDirection[4] == 1u, "the untouched -Z face stays one quad");
  check(split.size() > merged.size(), "a material split adds quads");

  std::vector<svr::SparseVoxelBrickSurfaceQuad> ignoringMaterial;
  svr::sparseVoxelBrickGreedyQuads(
      exposed, materials, svr::kSparseVoxelBrickSide, false, ignoringMaterial);
  check(
      ignoringMaterial.size() == 6u,
      "merging without the material test recovers one quad per face");
  std::vector<svr::SparseVoxelBrickSurfaceQuad> geometryOnly;
  svr::sparseVoxelBrickGreedyGeometryQuads(
      exposed, svr::kSparseVoxelBrickSide, geometryOnly);
  check(geometryOnly.size() == 6u &&
        std::all_of(geometryOnly.begin(), geometryOnly.end(), [](const auto &q) {
          return q.material == svr::kSparseVoxelBrickEmptyCellMaterial;
        }), "VG geometry merges across material changes without a palette");
  bool recoveredCells = true;
  for (const auto &quad : geometryOnly)
  {
    const auto axes = svr::sparseVoxelBrickFaceAxes(quad.direction);
    for (uint32_t v = 0; v < quad.height; ++v)
      for (uint32_t u = 0; u < quad.width; ++u)
      {
        const auto cell = svr::sparseVoxelBrickSourceCell(
            quad, float(quad.origin[axes[0]] + u) + 0.5f,
            float(quad.origin[axes[1]] + v) + 0.5f);
        recoveredCells &= cell[axes[0]] == quad.origin[axes[0]] + u &&
            cell[axes[1]] == quad.origin[axes[1]] + v &&
            cell[quad.direction >> 1u] == quad.origin[quad.direction >> 1u];
      }
  }
  check(recoveredCells, "merged-quad UV and normal recover every source cell");

  // Winding: the triangle normal of every quad must match its face normal.
  bool windingOk = true;
  for (const auto &quad : unmerged)
  {
    const auto corners = svr::sparseVoxelBrickQuadCorners(quad);
    const auto normal = svr::sparseVoxelBrickQuadNormal(quad);
    float edge0[3];
    float edge1[3];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      edge0[axis] = static_cast<float>(corners[1][axis]) -
          static_cast<float>(corners[0][axis]);
      edge1[axis] = static_cast<float>(corners[2][axis]) -
          static_cast<float>(corners[0][axis]);
    }
    const float cross[3] = {
        edge0[1] * edge1[2] - edge0[2] * edge1[1],
        edge0[2] * edge1[0] - edge0[0] * edge1[2],
        edge0[0] * edge1[1] - edge0[1] * edge1[0]};
    const float dot = cross[0] * normal[0] + cross[1] * normal[1] + cross[2] * normal[2];
    windingOk = windingOk && dot > 0.0f;
  }
  check(windingOk, "quad winding is counter-clockwise seen from outside");
}

void testClosedSelectableBlocks()
{
  std::cout << "every selectable octree block is closed\n";
  std::vector<uint32_t> cells;
  for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    cells.push_back(cell);
  const auto solid = occupancyFromCells(cells);
  for (const uint32_t side : {1u, 2u, 4u, 8u})
  {
    svr::SparseVoxelBrickFaceMasks faces{};
    const uint32_t count = svr::sparseVoxelBrickClosedBlockFaces(solid, side, faces);
    check(count == 6u * 64u * (8u / side),
          "solid blocks retain both sides of every selectable interface");
    bool exact = true;
    for (uint32_t z = 0u; z < 8u; ++z)
      for (uint32_t y = 0u; y < 8u; ++y)
        for (uint32_t x = 0u; x < 8u; ++x)
          for (uint32_t direction = 0u; direction < 6u; ++direction)
          {
            const uint32_t coordinate = std::array<uint32_t, 3>{x, y, z}[direction / 2u];
            const bool boundary = coordinate % side == ((direction & 1u) ? side - 1u : 0u);
            const bool emitted = (faces[direction][z] & (uint64_t{1u} << (x + 8u * y))) != 0u;
            exact &= emitted == boundary;
          }
    check(exact, "all six block sides are present, with no interior voxel faces");
  }

  svr::SparseVoxelBrickHierarchy hierarchy{};
  hierarchy.occupancy = {solid, solid};
  hierarchy.nodes.resize(2u);
  for (uint32_t i = 0u; i < 2u; ++i)
  {
    auto &node = hierarchy.nodes[i];
    node.occupancyIndex = i;
    node.minimumVoxelSize = {static_cast<float>(i * 8u), 0.0f, 0.0f, 1.0f};
    node.canonicalGridOriginScale = {static_cast<int32_t>(i * 8u), 0, 0, 1};
  }
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  settings.mergeCoplanarFaces = false;
  settings.maxTrianglesPerCluster = 12u;
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      hierarchy, 0u, 2u, settings, {}, build, &stats, &error);
  check(built, "closed adjacent bricks build: " + error);
  check(built && stats.blockDepth == 3u && build.clusters.size() == 1024u,
        "subdivision retains every occupied block, including interior blocks");
  check(built && std::all_of(build.clusters.begin(), build.clusters.end(),
            [](const auto &cluster) { return cluster.indices.size() == 36u; }),
        "each selectable voxel has six complete quad faces");

  // Independent bricks at different LODs share a plane far from the lattice
  // origin. Their rounded float minima are deliberately slightly different.
  hierarchy.nodes[0].canonicalGridOriginScale = {999992, 0, 0, 1};
  hierarchy.nodes[1].canonicalGridOriginScale = {1000000, 0, 0, 2};
  hierarchy.nodes[0].minimumVoxelSize = {-0.80001f, 0.0f, 0.0f, 0.1f};
  hierarchy.nodes[1].minimumVoxelSize = {0.00001f, 0.0f, 0.0f, 0.2f};
  hierarchy.nodes[1].level = 1u;
  settings = {};
  settings.canonicalWorldGrid = {-100000.0, 0.0, 0.0, 0.1};
  vg::VirtualGeometryBuildSettings rootPages{};
  rootPages.maxGroupsPerPage = 1u;
  rootPages.maxRootPageGroups = 1u;
  const bool aligned = svr::buildSparseVoxelOctreeVirtualGeometry(
      hierarchy, 0u, 2u, settings, rootPages, build, &stats, &error);
  uint32_t sharedVertices = 0u;
  bool sharedPlaneExact = aligned;
  float sharedPlane = 0.0f;
  for (size_t cluster = 0u; aligned && cluster < build.clusters.size(); ++cluster)
  {
    const bool fine = stats.clusterSources[cluster].brickNodeIndex == 0u;
    for (const auto &vertex : build.clusters[cluster].vertices)
      if (fine ? vertex.norm[0] > 0.5f : vertex.norm[0] < -0.5f)
      {
        if (sharedVertices++ == 0u)
          sharedPlane = vertex.pos[0];
        sharedPlaneExact &= vertex.pos[0] == sharedPlane &&
            std::fabs(vertex.pos[0]) < 1.0e-6f;
      }
  }
  check(sharedPlaneExact && sharedVertices == 8u,
        "mixed-LOD shared planes are bit-identical despite rounded brick origins");
  // Put the coarsest root first: selecting just the page with the largest
  // self error would leave the independent finer root on a missing page.
  std::swap(hierarchy.nodes[0], hierarchy.nodes[1]);
  const bool rootsBuilt = svr::buildSparseVoxelOctreeVirtualGeometry(
      hierarchy, 0u, 2u, settings, rootPages, build, &stats, &error);
  bool rootCoverage = rootsBuilt && build.pages.size() > 1u && !build.shapes.empty();
  for (const auto &node : build.lodLevelHierarchy)
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u &&
        node.max_parent_lod_error == std::numeric_limits<float>::max())
      rootCoverage &= (node.pageIndex & ~vg::PAGE_NOT_INSTALLED_BIT) <=
          build.shapes.front().root_page_index;
  check(rootCoverage, "the root page prefix covers every independent DAG root");
  if (rootCoverage)
  {
    const uint32_t root = build.shapes.front().root_page_index;
    const auto &dependencies = build.pages[root].dependencies;
    check(std::find(dependencies.begin(), dependencies.end(), 0u) != dependencies.end(),
          "the normal dependency installer reaches the earlier independent root");
  }
}

// ── octree virtual geometry build ───────────────────────────────────────────

void testOctreeBuildProducesRealLODChain()
{
  std::cout << "octree virtual geometry build over a terrain page\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;

  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  // Streamed pages translate their local octree onto the canonical lattice.
  // The translation need not align with every coarser brick's extent.
  auto translated = fixture.chunks.bricks;
  for (auto &node : translated.nodes)
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const int32_t delta = std::array<int32_t, 3>{24, 40, -8}[axis];
      node.canonicalGridOriginScale[axis] += delta;
      node.minimumVoxelSize[axis] += static_cast<float>(delta) *
          node.minimumVoxelSize[3] / node.canonicalGridOriginScale[3];
    }
  vg::VirtualGeometryBuildData translatedBuild{};
  svr::SparseVoxelOctreeVirtualGeometryStats translatedStats{};
  const bool translatedBuilt = svr::buildSparseVoxelOctreeVirtualGeometry(
      translated, span.first, span.count, settings, buildSettings,
      translatedBuild, &translatedStats, &error);
  const auto edges = [](const auto &data, const auto &sourceStats)
  {
    std::set<std::array<uint32_t, 4>> result;
    for (uint32_t from = 0u; from < data.clusterDAG.size(); ++from)
      for (const uint32_t to : data.clusterDAG[from])
      {
        const auto &a = sourceStats.clusterSources[from];
        const auto &b = sourceStats.clusterSources[to];
        result.insert({a.brickNodeIndex, a.blockIndex, b.brickNodeIndex, b.blockIndex});
      }
    return result;
  };
  check(translatedBuilt && edges(build, stats) == edges(translatedBuild, translatedStats) &&
            stats.rootClusterCount == translatedStats.rootClusterCount,
        "world translation preserves the octree DAG and every replacement volume");

  check(stats.levelCount > 1u, "the page hierarchy has more than one level");
  check(!build.clusters.empty(), "the build emits clusters");
  check(!build.pages.empty(), "the build emits pages");

  // The whole point: clusters exist above LOD0.
  uint32_t coarseClusters = 0u;
  for (uint32_t level = 1u; level < stats.clustersPerLevel.size(); ++level)
    coarseClusters += stats.clustersPerLevel[level];
  check(coarseClusters > 0u, "coarse levels carry their own clusters");
  check(
      stats.clustersPerLevel.empty() || stats.clustersPerLevel[0] > coarseClusters,
      "the chain narrows: LOD0 has more clusters than every coarse level");

  // A real LOD chain means most clusters have a finite parent error.
  uint32_t finiteParents = 0u;
  for (const auto &cluster : build.clusters)
    if (cluster.parent.error < std::numeric_limits<float>::max())
      ++finiteParents;
  check(
      finiteParents * 2u > static_cast<uint32_t>(build.clusters.size()),
      "most clusters can be coarsened: finite parent error");

  // Errors must be monotonic along every DAG edge or the cut can crack.
  uint32_t monotonicityBreaks = 0u;
  for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
    for (const uint32_t child : build.clusterDAG[parent])
      if (build.clusters[parent].self.error + 1e-5f < build.clusters[child].self.error)
        ++monotonicityBreaks;
  check(monotonicityBreaks == 0u, "cluster errors are monotonic along the DAG");

  // Parent/self bounds agreement is what the cut selects on.
  uint32_t boundsMismatches = 0u;
  for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
    for (const uint32_t child : build.clusterDAG[parent])
    {
      const auto &parentSelf = build.clusters[parent].self;
      const auto &childParent = build.clusters[child].parent;
      const bool same = std::fabs(parentSelf.radius - childParent.radius) < 1e-4f &&
          std::fabs(parentSelf.error - childParent.error) < 1e-4f &&
          std::fabs(parentSelf.center[0] - childParent.center[0]) < 1e-4f &&
          std::fabs(parentSelf.center[1] - childParent.center[1]) < 1e-4f &&
          std::fabs(parentSelf.center[2] - childParent.center[2]) < 1e-4f;
      if (!same)
        ++boundsMismatches;
    }
  check(boundsMismatches == 0u, "child parent bounds equal the coarse cluster's self bounds");

  // Every group has to fit a hierarchy leaf.
  uint32_t oversizedGroups = 0u;
  for (const auto &group : build.groupInfos)
    if (group.originalClusterIndices.size() > vg::MaxBVHChildren)
      ++oversizedGroups;
  check(oversizedGroups == 0u, "no group exceeds the hierarchy leaf child budget");

  // Every cluster belongs to exactly one group as an original.
  std::vector<uint32_t> groupOwners(build.clusters.size(), 0u);
  for (const auto &group : build.groupInfos)
    for (const int32_t cluster : group.originalClusterIndices)
      ++groupOwners[static_cast<size_t>(cluster)];
  const bool everyClusterOwnedOnce =
      std::all_of(groupOwners.begin(), groupOwners.end(), [](const uint32_t count)
                  { return count == 1u; });
  check(everyClusterOwnedOnce, "every cluster is an original of exactly one group");

  uint32_t oversizedClusters = 0u;
  uint32_t oversizedVertexClusters = 0u;
  for (const auto &cluster : build.clusters)
  {
    if (cluster.indices.size() / 3u > vg::ClusterSize)
      ++oversizedClusters;
    if (cluster.vertices.size() > vg::MaxVertices)
      ++oversizedVertexClusters;
  }
  check(oversizedClusters == 0u, "clusters respect the triangle budget");
  check(oversizedVertexClusters == 0u, "clusters respect the vertex budget");

  std::cout << "  octree build levels=" << stats.levelCount
            << " nodes=" << stats.nodeCount
            << " blockDepth=" << stats.blockDepth
            << " clusters=" << stats.clusterCount
            << " groups=" << stats.groupCount
            << " triangles=" << stats.triangleCount
            << " faces=" << stats.exposedFaceCount
            << " quads=" << stats.quadCount
            << " vanishingGroups=" << stats.vanishingGroupCount
            << " meshing_ms=" << std::fixed << std::setprecision(3)
            << stats.meshingMilliseconds
            << " hierarchy_ms=" << stats.hierarchyMilliseconds << '\n';
  std::cout << "  clusters per level:";
  for (uint32_t level = 0u; level < stats.clustersPerLevel.size(); ++level)
    std::cout << ' ' << level << '=' << stats.clustersPerLevel[level];
  std::cout << '\n';
}

// A cluster's error is what the culler projects to pixels and compares against
// its budget. For a voxel mesh the deviation from drawing a block instead of the
// eight blocks under it is bounded by that block's own cell, so a coarsened
// cluster has to carry at least its voxel size.
//
// The brick hierarchy's selfError floors at a quarter of the voxel
// (kSparseVoxelBrickErrorFloor), which is what the DDA raster wants. Inheriting
// that here kept voxel faces on screen at four times the intended pixel size -
// two LOD levels too coarse - and flat ground was the worst case, because its
// measured deviation is zero and the floor became the whole metric.
void testClusterErrorIsAtLeastItsOwnVoxel()
{
  std::cout << "coarsened clusters carry at least their voxel size as error\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;

  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks, span.first, span.count, settings, buildSettings,
      build, &stats, &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;
  check(
      stats.clusterSources.size() == build.clusters.size(),
      "every cluster has a recorded source");
  if (stats.clusterSources.size() != build.clusters.size())
    return;

  uint32_t coarsened = 0u;
  uint32_t belowVoxel = 0u;
  uint32_t nonMonotonic = 0u;
  float worstRatio = 1.0f;
  for (uint32_t cluster = 0u; cluster < build.clusters.size(); ++cluster)
  {
    const auto &source = stats.clusterSources[cluster];
    const auto &bounds = build.clusters[cluster];
    if (source.brickNodeIndex >= fixture.chunks.bricks.nodes.size())
      continue;
    const float voxelSize =
        fixture.chunks.bricks.nodes[source.brickNodeIndex].minimumVoxelSize[3];
    if (!(voxelSize > 0.0f))
      continue;
    // A leaf introduces no approximation, so zero is its correct self error.
    if (bounds.self.error <= 0.0f)
      continue;
    ++coarsened;
    if (bounds.self.error + 1.0e-4f < voxelSize)
    {
      ++belowVoxel;
      worstRatio = std::min(worstRatio, bounds.self.error / voxelSize);
    }
    if (bounds.parent.error < std::numeric_limits<float>::max() &&
        bounds.parent.error + 1.0e-4f < bounds.self.error)
      ++nonMonotonic;
  }

  std::cout << "  coarsened clusters=" << coarsened
            << " belowVoxel=" << belowVoxel
            << " worstRatio=" << worstRatio << '\n';
  check(coarsened > 0u, "the build produced coarsened clusters to measure");
  check(
      belowVoxel == 0u,
      "no coarsened cluster carries less error than its own voxel");
  check(nonMonotonic == 0u, "parent error never falls below self error");
}

void testGeometryMatchesVoxelSurface()
{
  std::cout << "emitted geometry reproduces the voxel surface exactly\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Canyon);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  // Total quad area must equal the exposed face count: merging changes how the
  // surface is packed into quads, never how much surface there is.
  check(
      stats.quadCount <= stats.exposedFaceCount,
      "merging never invents faces");
  check(stats.exposedFaceCount > 0u, "the page has an exposed surface");

  // Every triangle is degenerate-free and axis aligned.
  uint32_t degenerate = 0u;
  uint32_t nonAxisAligned = 0u;
  for (const auto &cluster : build.clusters)
    for (size_t triangle = 0u; triangle + 2u < cluster.indices.size(); triangle += 3u)
    {
      const auto &a = cluster.vertices[cluster.indices[triangle]];
      const auto &b = cluster.vertices[cluster.indices[triangle + 1u]];
      const auto &c = cluster.vertices[cluster.indices[triangle + 2u]];
      const float edge0[3] = {b.pos[0] - a.pos[0], b.pos[1] - a.pos[1], b.pos[2] - a.pos[2]};
      const float edge1[3] = {c.pos[0] - a.pos[0], c.pos[1] - a.pos[1], c.pos[2] - a.pos[2]};
      const float cross[3] = {
          edge0[1] * edge1[2] - edge0[2] * edge1[1],
          edge0[2] * edge1[0] - edge0[0] * edge1[2],
          edge0[0] * edge1[1] - edge0[1] * edge1[0]};
      const float area = std::sqrt(
          cross[0] * cross[0] + cross[1] * cross[1] + cross[2] * cross[2]);
      if (!(area > 0.0f))
      {
        ++degenerate;
        continue;
      }
      uint32_t zeroComponents = 0u;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        zeroComponents += std::fabs(cross[axis]) < area * 1e-3f ? 1u : 0u;
      if (zeroComponents != 2u)
        ++nonAxisAligned;
      // Winding must agree with the stored normal.
      const float dot = cross[0] * a.norm[0] + cross[1] * a.norm[1] + cross[2] * a.norm[2];
      if (!(dot > 0.0f))
        ++nonAxisAligned;
    }
  check(degenerate == 0u, "no degenerate triangles");
  check(nonAxisAligned == 0u, "every triangle is an outward axis-aligned face");
}

void testNoSimplifierAndSpeedAgainstTheOldBridge()
{
  std::cout << "octree build against the meshopt bridge\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;

  const auto start = std::chrono::steady_clock::now();
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  const double octreeMs =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
          .count();
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  // The old path: LOD0 sidecar meshing plus the meshoptimizer builder.
  svr::SparseVoxelTerrainTriangleMeshSettings meshSettings{};
  svr::SparseVoxelTerrainTrianglePageSidecar sidecar{};
  const auto legacyStart = std::chrono::steady_clock::now();
  const bool sidecarBuilt = svr::buildSparseVoxelTerrainTrianglePageSidecar(
      fixture.chunks, 0u, meshSettings, sidecar, &error);
  const double legacyMeshMs =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - legacyStart)
          .count();
  check(sidecarBuilt, "legacy sidecar builds for comparison: " + error);

  std::cout << "  octree_total_ms=" << std::fixed << std::setprecision(3) << octreeMs
            << " octree_meshing_ms=" << stats.meshingMilliseconds
            << " legacy_lod0_meshing_ms=" << legacyMeshMs << '\n';

  // The legacy sidecar only ever decodes the finest level, so it cannot even
  // describe the coarse geometry the octree path emits.
  check(
      stats.clustersPerLevel.size() > 1u &&
          stats.clustersPerLevel[0] > 0u,
      "the octree build covers LOD0");
  uint32_t coarse = 0u;
  for (uint32_t level = 1u; level < stats.clustersPerLevel.size(); ++level)
    coarse += stats.clustersPerLevel[level];
  check(coarse > 0u, "the octree build additionally covers every coarse level");

  // Meshing every level of the page must still be far cheaper than the hash-map
  // LOD0-only mesher it replaces.
  check(
      stats.meshingMilliseconds < legacyMeshMs,
      "octree meshing of all levels beats hash-map meshing of LOD0 alone");
}

void testEveryFixtureBuilds()
{
  std::cout << "every terrain fixture builds a valid DAG\n";

  for (const auto kind : terrain_phase0::kFixtureKinds)
  {
    Fixture fixture = buildFixture(kind);
    if (!fixture.valid || fixture.chunks.chunks.empty())
    {
      check(false, std::string("fixture builds: ") + terrain_phase0::fixtureName(kind));
      continue;
    }

    double totalMs = 0.0;
    uint32_t totalClusters = 0u;
    uint32_t totalGroups = 0u;
    uint32_t maximumBlockDepth = 0u;
    bool allBuilt = true;
    bool allValid = true;

    for (uint32_t chunkIndex = 0u; chunkIndex < fixture.chunks.chunks.size(); ++chunkIndex)
    {
      const ChunkSpan span = chunkSpan(fixture.chunks, chunkIndex);
      if (span.count == 0u)
        continue;

      svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
      vg::VirtualGeometryBuildSettings buildSettings{};
      vg::VirtualGeometryBuildData build{};
      svr::SparseVoxelOctreeVirtualGeometryStats stats{};
      std::string error;
      const auto start = std::chrono::steady_clock::now();
      const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
          fixture.chunks.bricks,
          span.first,
          span.count,
          settings,
          buildSettings,
          build,
          &stats,
          &error);
      totalMs += std::chrono::duration<double, std::milli>(
                     std::chrono::steady_clock::now() - start)
                     .count();
      if (!built)
      {
        allBuilt = false;
        std::cout << "    build failed: " << error << '\n';
        continue;
      }

      totalClusters += stats.clusterCount;
      totalGroups += stats.groupCount;
      maximumBlockDepth = std::max(maximumBlockDepth, stats.blockDepth);

      for (const auto &group : build.groupInfos)
        allValid = allValid && group.originalClusterIndices.size() <= vg::MaxBVHChildren;
      for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
        for (const uint32_t child : build.clusterDAG[parent])
          allValid = allValid &&
              build.clusters[parent].self.error + 1e-5f >= build.clusters[child].self.error;
    }

    check(allBuilt, std::string("all pages build: ") + terrain_phase0::fixtureName(kind));
    check(allValid, std::string("DAG invariants hold: ") + terrain_phase0::fixtureName(kind));
    std::cout << "    " << terrain_phase0::fixtureName(kind)
              << " pages=" << fixture.chunks.chunks.size()
              << " clusters=" << totalClusters << " groups=" << totalGroups
              << " maxBlockDepth=" << maximumBlockDepth
              << " total_ms=" << std::fixed << std::setprecision(3) << totalMs << '\n';
  }
}


// ── independent reference: the exposed surface of one octree level ──────────
//
// Rebuilt straight from the occupancy words, with no shared code with the
// mesher, so the comparison is a real oracle and not a restatement.

uint64_t packCell(const int64_t x, const int64_t y, const int64_t z)
{
  constexpr int64_t kBias = 1 << 20;
  return (static_cast<uint64_t>(x + kBias) << 42u) |
      (static_cast<uint64_t>(y + kBias) << 21u) |
      static_cast<uint64_t>(z + kBias);
}

// Occupied cells of one octree level, in that level's own integer cell grid.
using LevelCells = std::set<std::array<int64_t, 3>>;

std::map<uint32_t, LevelCells> levelOccupancy(
    const svr::SparseVoxelBrickHierarchy &hierarchy,
    const ChunkSpan &span)
{
  std::map<uint32_t, LevelCells> levels;
  for (uint32_t node = span.first; node < span.first + span.count; ++node)
  {
    const auto &source = hierarchy.nodes[node];
    const uint32_t level = source.level & 0xffu;
    const int64_t scale = std::max<int64_t>(1, source.canonicalGridOriginScale[3]);
    std::array<int64_t, 3> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      base[axis] = static_cast<int64_t>(source.canonicalGridOriginScale[axis]) / scale;

    const auto &occupancy = hierarchy.occupancy[source.occupancyIndex];
    for (uint32_t cell = 0u; cell < svr::kSparseVoxelBrickVoxelCount; ++cell)
    {
      if ((occupancy.cells[cell / 32u].bits & (uint32_t{1u} << (cell % 32u))) == 0u)
        continue;
      const int64_t x = cell % svr::kSparseVoxelBrickSide;
      const int64_t y = (cell / svr::kSparseVoxelBrickSide) % svr::kSparseVoxelBrickSide;
      const int64_t z = cell / (svr::kSparseVoxelBrickSide * svr::kSparseVoxelBrickSide);
      levels[level].insert({base[0] + x, base[1] + y, base[2] + z});
    }
  }
  return levels;
}

// (cell, direction) of every face an occupied cell shows to empty space.
std::set<std::pair<uint64_t, uint32_t>> expectedFaces(
    const LevelCells &cells, const uint32_t blockSide = 0u)
{
  static constexpr std::array<std::array<int64_t, 3>, 6> kOffsets{{
      {{-1, 0, 0}},
      {{1, 0, 0}},
      {{0, -1, 0}},
      {{0, 1, 0}},
      {{0, 0, -1}},
      {{0, 0, 1}},
  }};
  std::set<std::pair<uint64_t, uint32_t>> faces;
  for (const auto &cell : cells)
    for (uint32_t direction = 0u; direction < 6u; ++direction)
    {
      const std::array<int64_t, 3> neighbour{
          cell[0] + kOffsets[direction][0],
          cell[1] + kOffsets[direction][1],
          cell[2] + kOffsets[direction][2]};
      const int64_t coordinate = cell[direction / 2u];
      const int64_t side = blockSide;
      const int64_t offset = side != 0 ? (coordinate % side + side) % side : -1;
      const bool blockBoundary = side != 0 &&
          offset == ((direction & 1u) ? side - 1 : 0);
      if (blockBoundary || cells.count(neighbour) == 0u)
        faces.emplace(packCell(cell[0], cell[1], cell[2]), direction);
    }
  return faces;
}

// Expand the emitted clusters back into unit voxel faces.
//
// Returns false when a cluster is not a run of quads, which is itself a
// contract the raster path depends on.
bool emittedFaces(
    const vg::VirtualGeometryBuildData &build,
    const svr::SparseVoxelOctreeVirtualGeometryStats &stats,
    const svr::SparseVoxelBrickHierarchy &hierarchy,
    std::map<uint32_t, std::map<std::pair<uint64_t, uint32_t>, uint32_t>> &faces)
{
  faces.clear();
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    const auto &cluster = build.clusters[clusterIndex];
    const auto &origin = stats.clusterSources[clusterIndex];
    if (origin.brickNodeIndex >= hierarchy.nodes.size())
      return false;
    const auto &node = hierarchy.nodes[origin.brickNodeIndex];
    const double cellSize = static_cast<double>(node.minimumVoxelSize[3]);
    const int64_t scale = std::max<int64_t>(1, node.canonicalGridOriginScale[3]);
    std::array<int64_t, 3> base{};
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      base[axis] = static_cast<int64_t>(node.canonicalGridOriginScale[axis]) / scale;

    if (cluster.indices.size() % 6u != 0u)
      return false;
    for (size_t quad = 0u; quad * 6u < cluster.indices.size(); ++quad)
    {
      const uint32_t *triangle = &cluster.indices[quad * 6u];
      if (triangle[0] != triangle[3] || triangle[2] != triangle[4])
        return false;
      const std::array<uint32_t, 4> corners{
          triangle[0], triangle[1], triangle[2], triangle[5]};

      // Cell-grid corner coordinates, relative to the brick's own origin.
      std::array<int64_t, 3> minimum{};
      std::array<int64_t, 3> maximum{};
      for (uint32_t axis = 0u; axis < 3u; ++axis)
      {
        minimum[axis] = std::numeric_limits<int64_t>::max();
        maximum[axis] = std::numeric_limits<int64_t>::lowest();
      }
      for (const uint32_t corner : corners)
        for (uint32_t axis = 0u; axis < 3u; ++axis)
        {
          const double local =
              (static_cast<double>(cluster.vertices[corner].pos[axis]) -
               static_cast<double>(node.minimumVoxelSize[axis])) /
              cellSize;
          const int64_t rounded = static_cast<int64_t>(std::llround(local));
          if (std::fabs(local - static_cast<double>(rounded)) > 1e-2)
            return false;
          minimum[axis] = std::min(minimum[axis], rounded);
          maximum[axis] = std::max(maximum[axis], rounded);
        }

      // The constant axis is the face axis; the normal gives the side.
      uint32_t axis = 4u;
      for (uint32_t candidate = 0u; candidate < 3u; ++candidate)
        if (minimum[candidate] == maximum[candidate])
          axis = axis == 4u ? candidate : 4u;
      if (axis == 4u)
        return false;

      const float normalComponent = cluster.vertices[corners[0]].norm[axis];
      if (std::fabs(std::fabs(normalComponent) - 1.0f) > 1e-3f)
        return false;
      const bool positive = normalComponent > 0.0f;
      const uint32_t direction = axis * 2u + (positive ? 1u : 0u);

      const std::array<uint32_t, 2> tangents{
          axis == 0u ? 1u : 0u, axis == 2u ? 1u : 2u};
      for (int64_t u = minimum[tangents[0]]; u < maximum[tangents[0]]; ++u)
        for (int64_t v = minimum[tangents[1]]; v < maximum[tangents[1]]; ++v)
        {
          std::array<int64_t, 3> cell{};
          cell[axis] = minimum[axis] - (positive ? 1 : 0);
          cell[tangents[0]] = u;
          cell[tangents[1]] = v;
          const uint64_t key = packCell(
              base[0] + cell[0], base[1] + cell[1], base[2] + cell[2]);
          faces[origin.level][{key, direction}] += 1u;
        }
    }
  }
  return true;
}

void testEveryExteriorVoxelFaceIsMeshedPerLevel()
{
  std::cout << "every exterior voxel face is meshed once, at every octree level\n";

  for (const auto kind : terrain_phase0::kFixtureKinds)
  {
    Fixture fixture = buildFixture(kind);
    if (!fixture.valid || fixture.chunks.chunks.empty())
    {
      check(false, std::string("fixture builds: ") + terrain_phase0::fixtureName(kind));
      continue;
    }

    const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
    svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
    vg::VirtualGeometryBuildSettings buildSettings{};
    vg::VirtualGeometryBuildData build{};
    svr::SparseVoxelOctreeVirtualGeometryStats stats{};
    std::string error;
    if (!svr::buildSparseVoxelOctreeVirtualGeometry(
            fixture.chunks.bricks,
            span.first,
            span.count,
            settings,
            buildSettings,
            build,
            &stats,
            &error))
    {
      check(false, std::string("build: ") + terrain_phase0::fixtureName(kind) + ": " + error);
      continue;
    }

    std::map<uint32_t, std::map<std::pair<uint64_t, uint32_t>, uint32_t>> emitted;
    const bool quadShaped =
        emittedFaces(build, stats, fixture.chunks.bricks, emitted);
    check(
        quadShaped,
        std::string("clusters are runs of well formed quads: ") +
            terrain_phase0::fixtureName(kind));
    if (!quadShaped)
      continue;

    const auto occupancy = levelOccupancy(fixture.chunks.bricks, span);
    uint32_t missingTotal = 0u;
    uint32_t extraTotal = 0u;
    uint32_t duplicateTotal = 0u;
    uint32_t levelsChecked = 0u;
    for (const auto &[level, cells] : occupancy)
    {
      const auto expected = expectedFaces(cells, 8u >> stats.blockDepth);
      const auto &actual = emitted[level];
      ++levelsChecked;

      uint32_t missing = 0u;
      for (const auto &face : expected)
        if (actual.count(face) == 0u)
          ++missing;
      uint32_t extra = 0u;
      uint32_t duplicates = 0u;
      for (const auto &[face, count] : actual)
      {
        if (expected.count(face) == 0u)
          ++extra;
        if (count != 1u)
          ++duplicates;
      }
      missingTotal += missing;
      extraTotal += extra;
      duplicateTotal += duplicates;
    }

    check(
        levelsChecked > 1u,
        std::string("more than one octree level was meshed: ") +
            terrain_phase0::fixtureName(kind));
    check(
        missingTotal == 0u,
        std::string("no exterior voxel face is missing: ") +
            terrain_phase0::fixtureName(kind) + " missing=" +
            std::to_string(missingTotal));
    check(
        extraTotal == 0u,
        std::string("only exterior and selectable block-boundary faces are emitted: ") +
            terrain_phase0::fixtureName(kind) + " extra=" + std::to_string(extraTotal));
    check(
        duplicateTotal == 0u,
        std::string("no voxel face is covered twice: ") +
            terrain_phase0::fixtureName(kind) + " duplicates=" +
            std::to_string(duplicateTotal));
  }
}

void testOctreeLevelsAreValidCoarsenings()
{
  std::cout << "each octree level is a valid coarsening of the level below\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Mountain);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  const auto occupancy = levelOccupancy(fixture.chunks.bricks, span);
  check(occupancy.size() > 1u, "the page carries more than one octree level");

  uint32_t coarseningBreaks = 0u;
  uint32_t coverageBreaks = 0u;
  for (const auto &[level, cells] : occupancy)
  {
    if (level == 0u)
      continue;
    const auto finer = occupancy.find(level - 1u);
    if (finer == occupancy.end())
      continue;

    // Every fine cell must be represented by its parent cell: the coarse level
    // is an OR reduction, so coarsening can never drop occupied space.
    for (const auto &cell : finer->second)
    {
      const std::array<int64_t, 3> parent{
          cell[0] >> 1, cell[1] >> 1, cell[2] >> 1};
      if (cells.count(parent) == 0u)
        ++coverageBreaks;
    }
    // And no coarse cell may be invented out of empty space.
    for (const auto &cell : cells)
    {
      bool anyChild = false;
      for (uint32_t octant = 0u; octant < 8u && !anyChild; ++octant)
        anyChild = finer->second.count({
                       cell[0] * 2 + static_cast<int64_t>(octant & 1u),
                       cell[1] * 2 + static_cast<int64_t>((octant >> 1u) & 1u),
                       cell[2] * 2 + static_cast<int64_t>((octant >> 2u) & 1u),
                   }) != 0u;
      if (!anyChild)
        ++coarseningBreaks;
    }
  }
  check(coverageBreaks == 0u, "every fine cell has an occupied parent cell");
  check(coarseningBreaks == 0u, "no coarse cell is invented from empty space");

  // Each level must lose surface: a level with the same face count as the one
  // below it would make the whole LOD chain pointless.
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  bool trianglesShrink = true;
  for (uint32_t level = 1u; level < stats.trianglesPerLevel.size(); ++level)
    if (stats.trianglesPerLevel[level] == 0u ||
        stats.trianglesPerLevel[level] >= stats.trianglesPerLevel[level - 1u])
      trianglesShrink = false;
  check(trianglesShrink, "every coarser level carries strictly fewer triangles");

  // A cluster that coarsens something must carry at least its brick's measured
  // error. A leaf cluster coarsens nothing, so its error is exactly zero - that
  // is the LOD0 convention the culler needs in order to always have something
  // it can draw at close range.
  uint32_t coarseningBelowMeasured = 0u;
  uint32_t leafWithError = 0u;
  uint32_t leafClusters = 0u;
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    const auto &origin = stats.clusterSources[clusterIndex];
    if (origin.brickNodeIndex >= fixture.chunks.bricks.nodes.size())
      continue;
    const auto &node = fixture.chunks.bricks.nodes[origin.brickNodeIndex];
    if (build.clusterDAG[clusterIndex].empty())
    {
      ++leafClusters;
      if (build.clusters[clusterIndex].self.error != 0.0f)
        ++leafWithError;
      continue;
    }
    if (build.clusters[clusterIndex].self.error + 1e-5f < node.selfError)
      ++coarseningBelowMeasured;
  }
  check(leafClusters > 0u, "the page has leaf clusters");
  check(leafWithError == 0u, "every leaf cluster has zero self error");
  check(
      coarseningBelowMeasured == 0u,
      "a coarsening cluster never claims less error than its brick measured");
}

void testClusterCutsAreBrickBlockBounds()
{
  std::cout << "cluster cuts are brick block bounds\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::CrossLOD);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  check(
      stats.clusterSources.size() == build.clusters.size(),
      "every cluster reports the brick block it meshes");

  uint32_t outsideBlock = 0u;
  uint32_t wrongExtent = 0u;
  uint32_t unmappedClusters = 0u;
  const float expectedExtent = static_cast<float>(
      svr::kSparseVoxelBrickSide >> stats.blockDepth);
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    const auto &origin = stats.clusterSources[clusterIndex];
    if (origin.brickNodeIndex >= fixture.chunks.bricks.nodes.size())
    {
      ++unmappedClusters;
      continue;
    }
    const auto &node = fixture.chunks.bricks.nodes[origin.brickNodeIndex];
    const float cellSize = node.minimumVoxelSize[3];
    if (std::fabs(origin.blockExtent - expectedExtent * cellSize) > cellSize * 1e-3f)
      ++wrongExtent;

    const float tolerance = cellSize * 1e-3f;
    for (const auto &vertex : build.clusters[clusterIndex].vertices)
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (vertex.pos[axis] < origin.blockMinimum[axis] - tolerance ||
            vertex.pos[axis] > origin.blockMinimum[axis] + origin.blockExtent + tolerance)
          ++outsideBlock;
  }
  check(unmappedClusters == 0u, "no cluster is missing its brick mapping");
  check(wrongExtent == 0u, "every block box is one octree block wide");
  check(outsideBlock == 0u, "every cluster vertex lies on its brick block box");

  // Every cluster meshes exactly one brick node, so a cut that keeps a cluster
  // keeps a whole brick block and never half of one.
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> blockOwners;
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    const auto &origin = stats.clusterSources[clusterIndex];
    ++blockOwners[{origin.brickNodeIndex, origin.blockIndex}];
  }
  const bool oneClusterPerBlock =
      std::all_of(blockOwners.begin(), blockOwners.end(), [](const auto &entry)
                  { return entry.second == 1u; });
  check(oneClusterPerBlock, "one cluster per brick block, never split");

  // Group volume correspondence: the coarse cluster's block must contain every
  // one of the fine blocks it replaces, at exactly twice the side.
  uint32_t badGroups = 0u;
  uint32_t checkedGroups = 0u;
  for (const auto &group : build.groupInfos)
  {
    if (group.simplifiedClusterIndices.empty())
      continue;
    const auto &parent =
        stats.clusterSources[static_cast<size_t>(group.simplifiedClusterIndices[0])];
    ++checkedGroups;
    for (const int32_t child : group.originalClusterIndices)
    {
      const auto &origin = stats.clusterSources[static_cast<size_t>(child)];
      if (std::fabs(parent.blockExtent - origin.blockExtent * 2.0f) >
          origin.blockExtent * 1e-3f)
      {
        ++badGroups;
        break;
      }
      bool contained = true;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        contained = contained &&
            origin.blockMinimum[axis] >= parent.blockMinimum[axis] - origin.blockExtent * 1e-3f &&
            origin.blockMinimum[axis] + origin.blockExtent <=
                parent.blockMinimum[axis] + parent.blockExtent + origin.blockExtent * 1e-3f;
      if (!contained)
      {
        ++badGroups;
        break;
      }
    }
  }
  check(checkedGroups > 0u, "the DAG has coarsening groups to check");
  check(badGroups == 0u, "each group's fine blocks tile its coarse block exactly");
}


void testEncodedTerrainPageHasAStreamableLODChain()
{
  std::cout << "encoded terrain page carries a streamable LOD chain\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;
  vg::QuantizationConfig quantization{};
  quantization.unit_scale = 1.0f;
  quantization.padMeshlets = true;

  vg::VirtualGeometryEncodedData encoded{};
  std::array<double, 3> pageOrigin{};
  svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
      fixture.chunks,
      0u,
      octreeSettings,
      buildSettings,
      quantization,
      encoded,
      &pageOrigin,
      &stats,
      &error);
  check(built, "terrain octree VG asset builds: " + error);
  if (!built)
    return;
  // Phase 2 of docs/terrain-vg-unified-hierarchy-plan.md needs to know the
  // shape of a real terrain asset: how many pages, how many hierarchy nodes,
  // and whether the builder supplied the per-cluster install rewrites that a
  // slot install has to forward.
  {
    size_t installUpdates = 0u;
    size_t uninstallUpdates = 0u;
    size_t pagesWithUpdates = 0u;
    for (const auto &page : encoded.pages)
    {
      installUpdates += page.installUpdates.hierarchyUpdates.size();
      uninstallUpdates += page.uninstallUpdates.hierarchyUpdates.size();
      if (!page.installUpdates.hierarchyUpdates.empty())
        ++pagesWithUpdates;
    }
    std::cout << "  [shape] pages=" << encoded.pages.size()
              << " hierarchyNodes=" << encoded.hierarchy.size()
              << " installUpdates=" << installUpdates
              << " uninstallUpdates=" << uninstallUpdates
              << " pagesWithUpdates=" << pagesWithUpdates << "\n";
    // The builder must supply per-cluster install rewrites. Installing a page
    // without them marks every cluster on it a streaming leaf, which forces its
    // self error to zero in the culler: the cluster then draws at every
    // distance, the parent test is skipped, and every LOD level draws at once.
    // See VirtualGeometryScene.hpp's RuntimePageInstallInfo comment.
    check(installUpdates != 0u, "the terrain octree build supplies install rewrites");
    check(
        pagesWithUpdates == encoded.pages.size(),
        "every encoded terrain page carries its rewrites");
    // A chunk's hierarchy is much richer than one node per page, which is why a
    // shared runtime object cannot use a flat one-leaf-per-slot hierarchy for
    // CPU-meshed terrain. See docs/terrain-vg-unified-hierarchy-plan.md Phase 2.
    check(
        encoded.hierarchy.size() > encoded.pages.size() + 1u,
        "a terrain chunk's DAG has more nodes than it has pages");
  }

  check(!encoded.pages.empty(), "the encoded asset has pages");
  check(!encoded.hierarchy.empty(), "the encoded asset has hierarchy nodes");
  check(!encoded.shapes.empty(), "the encoded asset has a shape");

  uint32_t meshlets = 0u;
  uint32_t emptyMeshlets = 0u;
  uint32_t oversizedMeshlets = 0u;
  uint32_t coarsenableMeshlets = 0u;
  for (const auto &page : encoded.pages)
    for (const auto &meshlet : page.meshlets)
    {
      ++meshlets;
      if (meshlet.vertex_count == 0u || meshlet.triangle_count == 0u)
        ++emptyMeshlets;
      if (meshlet.triangle_count > vg::ClusterSize)
        ++oversizedMeshlets;
      if (meshlet.parent.error < std::numeric_limits<float>::max())
        ++coarsenableMeshlets;
    }
  check(meshlets > 0u, "the encoded asset has meshlets");
  check(emptyMeshlets == 0u, "every encoded meshlet has vertices and triangles");
  check(oversizedMeshlets == 0u, "every encoded meshlet fits the cluster budget");
  check(
      coarsenableMeshlets * 2u > meshlets,
      "most encoded meshlets carry a finite parent error");

  // Positions must be page relative: a planet-scale absolute coordinate would
  // quantize to nothing.
  bool insidePage = true;
  const double edge = fixture.chunks.chunks[0].edgeMetres;
  for (const auto &page : encoded.pages)
    for (const auto &meshlet : page.meshlets)
      for (const int32_t channel : {meshlet.min_vertex_position_channel_x,
                                    meshlet.min_vertex_position_channel_y,
                                    meshlet.min_vertex_position_channel_z})
        insidePage = insidePage &&
            std::fabs(static_cast<double>(channel)) <=
                edge * static_cast<double>(quantization.unit_scale) *
                    static_cast<double>(1u << quantization.quantization_factor) * 4.0;
  check(insidePage, "encoded positions stay inside the page frame");

  std::cout << "  encoded pages=" << stats.encodedPageCount
            << " meshlets=" << stats.encodedMeshletCount
            << " groups=" << stats.encodedGroupCount
            << " hierarchyNodes=" << stats.encodedHierarchyNodeCount
            << " positionWords=" << stats.encodedVertexPositionWords
            << " indexBytes=" << stats.encodedIndexBytes
            << " build_ms=" << std::fixed << std::setprecision(3)
            << (stats.build.meshingMilliseconds + stats.build.hierarchyMilliseconds)
            << " encode_ms=" << stats.encodeMilliseconds << '\n';
}


// ── the DAG cut the renderer actually performs ──────────────────────────────
//
// Virtual geometry selects a cluster with two independent tests: my own error
// is small enough at this distance, and my parent's was not. That is crack-free
// only when exactly one cluster on every root-to-leaf path passes both. These
// tests run that selection on the CPU over the built DAG, which is what decides
// whether terrain refines with distance and whether a cut can open a hole.

std::vector<uint32_t> parentClusterMap(const vg::VirtualGeometryBuildData &build)
{
  std::vector<uint32_t> parents(build.clusters.size(), UINT32_MAX);
  for (uint32_t parent = 0u; parent < build.clusterDAG.size(); ++parent)
    for (const uint32_t child : build.clusterDAG[parent])
      parents[child] = parent;
  return parents;
}

bool clusterSelected(
    const vg::VirtualGeometryCluster &cluster,
    const float threshold)
{
  return cluster.self.error <= threshold && cluster.parent.error > threshold;
}

// One runtime object holds every terrain chunk, and a chunk's finest clusters
// carry its voxel size as error so a resident finer chunk can replace them. That
// only works if the builder also marks them streaming leaves - the mark is what
// keeps the frontier drawing until the finer chunk arrives - and if a mark that
// never clears inside the build does not hold its group pending forever (the
// first attempt looped in buildPagesAndRewrites emitting empty pages).
void testFinestClustersAreStreamingLeavesWhenTheyCarryError()
{
  std::cout << "finest clusters are streaming leaves when they carry an error\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const auto build = [&](const float leafSelfError, vg::VirtualGeometryEncodedData &encoded) -> bool
  {
    svr::SparseVoxelOctreeVirtualGeometrySettings octreeSettings{};
    octreeSettings.leafSelfError = leafSelfError;
    vg::VirtualGeometryBuildSettings buildSettings{};
    buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;
    vg::QuantizationConfig quantization{};
    quantization.unit_scale = 1.0f;
    quantization.padMeshlets = true;
    std::array<double, 3> pageOrigin{};
    svr::SparseVoxelTerrainOctreeVirtualGeometryBuildStats stats{};
    std::string error;
    const bool built = svr::buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
        fixture.chunks, 0u, octreeSettings, buildSettings, quantization,
        encoded, &pageOrigin, &stats, &error);
    check(built, "terrain octree VG asset builds: " + error);
    return built;
  };
  // Leaf nodes whose install update leaves some cluster unmarked, and leaf
  // nodes that are fully marked, over every page's install rewrites.
  // Every group is a hierarchy leaf node, at every LOD level; the finest
  // level's nodes are the ones whose self error is the finest cluster error.
  const auto countLeafMarks = [&](const vg::VirtualGeometryEncodedData &encoded, const float finestError, size_t &fullyMarked, size_t &partlyUnmarked)
  {
    fullyMarked = 0u;
    partlyUnmarked = 0u;
    for (const auto &page : encoded.pages)
      for (const auto &update : page.installUpdates.hierarchyUpdates)
      {
        if (update.hierarchyNodeIndex >= encoded.hierarchy.size())
          continue;
        const auto &node = encoded.hierarchy[update.hierarchyNodeIndex];
        if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u ||
            node.min_lod_error > finestError + 1.0e-6f)
          continue;
        if ((update.enabledClustersBitset & ~update.streamingLeafsBitset) == 0u)
          ++fullyMarked;
        else
          ++partlyUnmarked;
      }
  };

  vg::VirtualGeometryEncodedData plain{};
  vg::VirtualGeometryEncodedData withError{};
  if (!build(0.0f, plain))
    return;
  const float voxel = static_cast<float>(fixture.chunks.chunks[0].voxelSizeMetres);
  check(voxel > 0.0f, "the fixture chunk has a voxel size");
  if (!build(voxel, withError))
    return;
  check(withError.pages.size() == plain.pages.size(),
      "a real finest error does not change the page count (the build converged)");
  size_t plainFull = 0u, plainPartial = 0u, errorFull = 0u, errorPartial = 0u;
  countLeafMarks(plain, 0.0f, plainFull, plainPartial);
  countLeafMarks(withError, voxel, errorFull, errorPartial);
  std::cout << "  [marks] plain fully=" << plainFull << " partly=" << plainPartial
            << " withError fully=" << errorFull << " partly=" << errorPartial << "\n";
  check(plainPartial != 0u, "with the LOD0 convention some finest clusters are true leaves, not streaming leaves");
  check(errorPartial == 0u, "with a real finest error every enabled cluster of a leaf node is a streaming leaf");
  check(errorFull != 0u, "the build with a real finest error produced leaf-node rewrites");
  // The finest clusters really carry the error: the leaf nodes' self error is
  // no longer zero.
  size_t zeroLeaves = 0u, nonZeroLeaves = 0u;
  for (const auto &node : withError.hierarchy)
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) != 0u)
      (node.min_lod_error > 0.0f ? nonZeroLeaves : zeroLeaves)++;
  check(zeroLeaves == 0u && nonZeroLeaves != 0u, "leaf nodes carry the finest cluster error");
}

void testDAGCutIsCrackFreeAndRefinesWithThreshold()
{
  std::cout << "the DAG cut refines with the error threshold and never cracks\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Mountain);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  const std::vector<uint32_t> parents = parentClusterMap(build);

  // Leaves are the finest clusters: nothing in the DAG refines them further.
  std::vector<uint32_t> leaves;
  float finestError = 0.0f;
  float coarsestError = 0.0f;
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
  {
    if (build.clusterDAG[clusterIndex].empty())
    {
      leaves.push_back(clusterIndex);
      finestError = std::max(finestError, build.clusters[clusterIndex].self.error);
    }
    if (build.clusters[clusterIndex].parent.error == std::numeric_limits<float>::max())
      coarsestError = std::max(coarsestError, build.clusters[clusterIndex].self.error);
  }
  check(!leaves.empty(), "the DAG has leaf clusters");
  check(coarsestError > finestError, "the chain spans a real error range");

  uint32_t multipleSelected = 0u;
  uint32_t noneSelected = 0u;
  uint32_t thresholdsChecked = 0u;
  std::vector<uint32_t> selectedPerThreshold;
  std::vector<float> thresholds;
  for (float scale = 1.0f; scale <= 64.0f; scale *= 2.0f)
    thresholds.push_back(finestError * scale);
  thresholds.push_back(coarsestError * 2.0f);

  for (const float threshold : thresholds)
  {
    ++thresholdsChecked;
    uint32_t selectedClusters = 0u;
    for (const auto &cluster : build.clusters)
      selectedClusters += clusterSelected(cluster, threshold) ? 1u : 0u;
    selectedPerThreshold.push_back(selectedClusters);

    // One cluster per root-to-leaf path is the crack-free condition.
    for (const uint32_t leaf : leaves)
    {
      uint32_t onPath = 0u;
      uint32_t cursor = leaf;
      while (cursor != UINT32_MAX)
      {
        onPath += clusterSelected(build.clusters[cursor], threshold) ? 1u : 0u;
        cursor = parents[cursor];
      }
      if (onPath > 1u)
        ++multipleSelected;
      else if (onPath == 0u)
        ++noneSelected;
    }
  }

  check(thresholdsChecked > 1u, "the sweep covers several thresholds");
  check(multipleSelected == 0u, "no path ever selects two clusters: no double geometry");
  check(
      noneSelected == 0u,
      "no path ever selects zero clusters: no holes, count=" +
          std::to_string(noneSelected));

  // Refinement must actually respond to the threshold: a tighter budget draws
  // more clusters than a looser one.
  bool refinesMonotonically = true;
  for (size_t index = 1u; index < selectedPerThreshold.size(); ++index)
    refinesMonotonically =
        refinesMonotonically && selectedPerThreshold[index] <= selectedPerThreshold[index - 1u];
  check(refinesMonotonically, "a looser threshold never draws more clusters");
  check(
      selectedPerThreshold.front() > selectedPerThreshold.back(),
      "the tightest threshold draws strictly more clusters than the loosest");
  check(
      selectedPerThreshold.back() > 0u,
      "even the loosest threshold still draws the coarse page");

  std::cout << "  clusters selected per threshold:";
  for (size_t index = 0u; index < thresholds.size(); ++index)
    std::cout << ' ' << std::fixed << std::setprecision(3) << thresholds[index]
              << "->" << selectedPerThreshold[index];
  std::cout << '\n';
}

void testSelectedCutCoversTheWholeSurface()
{
  std::cout << "every cut covers the whole page volume\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  const std::vector<uint32_t> parents = parentClusterMap(build);

  // Volume coverage: for every finest-level cluster, exactly one cluster in its
  // ancestor chain is drawn, and the drawn one's block box contains it.
  float finestError = 0.0f;
  for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
    if (build.clusterDAG[clusterIndex].empty())
      finestError = std::max(finestError, build.clusters[clusterIndex].self.error);

  uint32_t uncovered = 0u;
  uint32_t notContained = 0u;
  for (float scale = 1.0f; scale <= 32.0f; scale *= 2.0f)
  {
    const float threshold = finestError * scale;
    for (uint32_t clusterIndex = 0u; clusterIndex < build.clusters.size(); ++clusterIndex)
    {
      if (!build.clusterDAG[clusterIndex].empty())
        continue;
      const auto &leafOrigin = stats.clusterSources[clusterIndex];

      uint32_t drawn = UINT32_MAX;
      uint32_t cursor = clusterIndex;
      while (cursor != UINT32_MAX)
      {
        if (clusterSelected(build.clusters[cursor], threshold))
        {
          drawn = cursor;
          break;
        }
        cursor = parents[cursor];
      }
      if (drawn == UINT32_MAX)
      {
        ++uncovered;
        continue;
      }

      const auto &drawnOrigin = stats.clusterSources[drawn];
      const float tolerance = drawnOrigin.blockExtent * 1e-3f;
      for (uint32_t axis = 0u; axis < 3u; ++axis)
        if (leafOrigin.blockMinimum[axis] < drawnOrigin.blockMinimum[axis] - tolerance ||
            leafOrigin.blockMinimum[axis] + leafOrigin.blockExtent >
                drawnOrigin.blockMinimum[axis] + drawnOrigin.blockExtent + tolerance)
        {
          ++notContained;
          break;
        }
    }
  }
  check(uncovered == 0u, "every finest block is covered by some drawn cluster");
  check(
      notContained == 0u,
      "the drawn cluster's block always contains the block it stands in for");
}

void testClusterConesNeverCullAVisibleFace()
{
  std::cout << "cluster normal cones never claim more than the geometry allows\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Canyon);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildSettings buildSettings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks,
      span.first,
      span.count,
      settings,
      buildSettings,
      build,
      &stats,
      &error);
  check(built, "octree virtual geometry builds: " + error);
  if (!built)
    return;

  // A cone with cutoff below one lets the culler drop the cluster. That is only
  // sound when every face in the cluster really points the same way: the
  // shader's test is dot(centerToEye, axis) >= cutoff * distance + radius, so a
  // tight cone on a multi-directional block would cull faces that are visible.
  uint32_t unsoundCones = 0u;
  uint32_t tightCones = 0u;
  uint32_t conservativeCones = 0u;
  for (const auto &cluster : build.clusters)
  {
    std::set<std::array<int, 3>> normals;
    for (const auto &vertex : cluster.vertices)
      normals.insert({
          static_cast<int>(std::lround(vertex.norm[0])),
          static_cast<int>(std::lround(vertex.norm[1])),
          static_cast<int>(std::lround(vertex.norm[2])),
      });

    if (cluster.cone.cutoff >= 1.0f)
    {
      ++conservativeCones;
      continue;
    }
    ++tightCones;
    if (normals.size() != 1u)
    {
      ++unsoundCones;
      continue;
    }
    const auto &normal = *normals.begin();
    const bool axisMatches =
        std::fabs(cluster.cone.axis[0] - static_cast<float>(normal[0])) < 1e-3f &&
        std::fabs(cluster.cone.axis[1] - static_cast<float>(normal[1])) < 1e-3f &&
        std::fabs(cluster.cone.axis[2] - static_cast<float>(normal[2])) < 1e-3f;
    if (!axisMatches)
      ++unsoundCones;
  }
  check(unsoundCones == 0u, "no cluster claims a tighter cone than its faces support");
  check(tightCones == 0u, "closed block surfaces cannot be rejected by a one-sided cone");
  check(conservativeCones > 0u, "mixed clusters fall back to no cone culling");
}

} // namespace

// ---------------------------------------------------------------------------
// The error partition the cut rests on. Phase 10 found roughly ten defects in
// this structure and none of them had a CPU test, because the existing cut
// tests all run with the default settings where `parentChunkError` is infinite.
// These cover the invariants the triangle builder enforces by construction and
// the ones the cross-chunk clamp adds on top.
// ---------------------------------------------------------------------------

bool sameBounds(const vg::LODBounds &left, const vg::LODBounds &right)
{
  return left.center[0] == right.center[0] && left.center[1] == right.center[1] &&
      left.center[2] == right.center[2] && left.radius == right.radius &&
      left.error == right.error;
}

// Build one chunk of a fixture with the given octree settings.
bool buildChunkWith(
    Fixture &fixture,
    const svr::SparseVoxelOctreeVirtualGeometrySettings &settings,
    vg::VirtualGeometryBuildData &build,
    svr::SparseVoxelOctreeVirtualGeometryStats &stats)
{
  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  vg::VirtualGeometryBuildSettings buildSettings{};
  buildSettings.maxGroupsPerPage = vg::MAX_GROUPS_PER_PAGE;
  std::string error;
  const bool built = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks, span.first, span.count, settings, buildSettings,
      build, &stats, &error);
  check(built, "octree virtual geometry builds: " + error);
  return built;
}

// A group replaces its children with its own coarse cluster, so the band the
// children end at and the band the parent starts at have to be the *same*
// bound - the triangle builder gets this by assigning one `groupBounds` object
// to `clusters[child].parent` and `simplified.self` alike. If they ever drift
// apart the cut gains a gap or an overlap exactly one group wide.
void testGroupBoundsAreSharedByChildrenAndParent()
{
  std::cout << "a group's bound is shared by its children and its coarse cluster\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!buildChunkWith(fixture, settings, build, stats))
    return;

  uint32_t groupsChecked = 0u;
  uint32_t mismatched = 0u;
  uint32_t nonMonotonic = 0u;
  for (const auto &group : build.groupInfos)
  {
    if (group.originalClusterIndices.empty() ||
        group.simplifiedClusterIndices.empty())
      continue;
    ++groupsChecked;
    const auto &coarse =
        build.clusters[static_cast<size_t>(group.simplifiedClusterIndices[0])];
    for (const int32_t childIndex : group.originalClusterIndices)
    {
      const auto &child = build.clusters[static_cast<size_t>(childIndex)];
      if (!sameBounds(child.parent, coarse.self))
        ++mismatched;
      if (!(child.parent.error >= child.self.error))
        ++nonMonotonic;
    }
  }
  check(groupsChecked != 0u, "the build has groups with a coarse replacement");
  check(
      mismatched == 0u,
      "every child's parent bound is its group's coarse cluster's self bound (" +
          std::to_string(mismatched) + " differ)");
  check(
      nonMonotonic == 0u,
      "a cluster's parent error is never finer than its own (" +
          std::to_string(nonMonotonic) + " inverted)");
}

// The triangle builder asserts `parent.error == max()` before consuming a
// cluster, which is how it guarantees one group per cluster. The octree builder
// has no such assert; a cluster consumed twice would carry the second group's
// bound and silently leave the first group's cut with a hole.
void testEveryClusterIsConsumedByAtMostOneGroup()
{
  std::cout << "no cluster is consumed by two groups\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Canyon);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!buildChunkWith(fixture, settings, build, stats))
    return;

  std::vector<uint32_t> consumedBy(build.clusters.size(), UINT32_MAX);
  uint32_t doubleConsumed = 0u;
  for (uint32_t groupIndex = 0u; groupIndex < build.groupInfos.size(); ++groupIndex)
    for (const int32_t childIndex : build.groupInfos[groupIndex].originalClusterIndices)
    {
      const size_t child = static_cast<size_t>(childIndex);
      if (child >= consumedBy.size())
        continue;
      if (consumedBy[child] != UINT32_MAX)
        ++doubleConsumed;
      consumedBy[child] = groupIndex;
    }
  check(
      doubleConsumed == 0u,
      "each cluster belongs to at most one group (" +
          std::to_string(doubleConsumed) + " consumed twice)");
}

// Phase 10c. A terrain chunk is handed the parent chunk's finest error, and
// every cluster's parent error is clamped to it so the chunk owns exactly the
// octave below its parent chunk. Two chunks then partition the error axis with
// no coverage test at all, which is the whole point - so the shape of that
// clamp is worth pinning.
void testParentChunkErrorClampsTheChunkToOneOctave()
{
  std::cout << "the parent chunk error clamps a chunk to the octave below it\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const float leafError =
      static_cast<float>(fixture.chunks.chunks[0].voxelSizeMetres);
  check(leafError > 0.0f, "the fixture chunk has a voxel size");
  // What the terrain page cache passes: the parent chunk is exactly one octave
  // coarser, because the spatial quadtree holds `horizontalVoxelCount` fixed.
  const float parentChunkError = leafError * 2.0f;

  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  settings.leafSelfError = leafError;
  settings.parentChunkError = parentChunkError;
  vg::VirtualGeometryBuildData build{};
  svr::SparseVoxelOctreeVirtualGeometryStats stats{};
  if (!buildChunkWith(fixture, settings, build, stats))
    return;

  uint32_t overClamp = 0u;
  uint32_t finestClusters = 0u;
  uint32_t finestWithEmptyBand = 0u;
  for (const auto &cluster : build.clusters)
  {
    if (cluster.parent.error > parentChunkError)
      ++overClamp;
    if (cluster.self.error == leafError)
    {
      ++finestClusters;
      // The band this chunk owns. It must be non-empty and must end exactly
      // where the parent chunk's finest clusters begin, or the two chunks
      // leave a gap between them.
      if (!(cluster.parent.error > cluster.self.error))
        ++finestWithEmptyBand;
      else if (cluster.parent.error != parentChunkError)
        ++finestWithEmptyBand;
    }
  }
  check(
      overClamp == 0u,
      "no cluster's parent error survives above the parent chunk's (" +
          std::to_string(overClamp) + " do)");
  check(finestClusters != 0u, "the chunk has clusters at its own voxel size");
  check(
      finestWithEmptyBand == 0u,
      "every finest cluster owns [leafError, parentChunkError) exactly (" +
          std::to_string(finestWithEmptyBand) + " do not)");

  // Disjointness, which is the property the coverage test used to stand in for:
  // at any budget the parent chunk owns, this chunk must select nothing.
  uint32_t selectedAtParentBudget = 0u;
  for (const auto &cluster : build.clusters)
    selectedAtParentBudget += clusterSelected(cluster, parentChunkError) ? 1u : 0u;
  check(
      selectedAtParentBudget == 0u,
      "at the parent chunk's own error this chunk selects nothing (" +
          std::to_string(selectedAtParentBudget) + " selected)");

  // Coverage, the other half: just under that budget this chunk must select
  // something on every leaf path, or the pair leaves a hole.
  const float ownBudget = std::nextafter(parentChunkError, 0.0f);
  uint32_t selectedAtOwnBudget = 0u;
  for (const auto &cluster : build.clusters)
    selectedAtOwnBudget += clusterSelected(cluster, ownBudget) ? 1u : 0u;
  check(
      selectedAtOwnBudget != 0u,
      "just under the parent chunk's error this chunk still draws");
}

// The clamp must not reach the roots. A root cluster's infinite parent error is
// the chunk's "nothing coarser than me" fallback; whether something coarser is
// resident is a runtime fact, answered by the chunk root node's
// max_parent_lod_error. This pins which clusters the flag governs.
void testRootParentErrorFollowsItsFlag()
{
  std::cout << "the root clamp flag governs exactly the root clusters\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;

  const float leafError =
      static_cast<float>(fixture.chunks.chunks[0].voxelSizeMetres);
  const float parentChunkError = leafError * 2.0f;
  const auto countInfiniteParents = [](const vg::VirtualGeometryBuildData &build)
  {
    uint32_t infinite = 0u;
    for (const auto &cluster : build.clusters)
      if (cluster.parent.error == std::numeric_limits<float>::max())
        ++infinite;
    return infinite;
  };

  svr::SparseVoxelOctreeVirtualGeometrySettings clamped{};
  clamped.leafSelfError = leafError;
  clamped.parentChunkError = parentChunkError;
  clamped.parentChunkErrorAppliesToRoots = true;
  vg::VirtualGeometryBuildData clampedBuild{};
  svr::SparseVoxelOctreeVirtualGeometryStats clampedStats{};
  if (!buildChunkWith(fixture, clamped, clampedBuild, clampedStats))
    return;

  svr::SparseVoxelOctreeVirtualGeometrySettings open = clamped;
  open.parentChunkErrorAppliesToRoots = false;
  vg::VirtualGeometryBuildData openBuild{};
  svr::SparseVoxelOctreeVirtualGeometryStats openStats{};
  if (!buildChunkWith(fixture, open, openBuild, openStats))
    return;

  check(
      countInfiniteParents(clampedBuild) == 0u,
      "with the flag on, no cluster keeps an infinite parent error");
  check(
      countInfiniteParents(openBuild) == openStats.rootClusterCount &&
          openStats.rootClusterCount != 0u,
      "with the flag off, exactly the root clusters keep it");
  check(
      clampedStats.rootClusterCount == openStats.rootClusterCount,
      "the flag changes errors, never which clusters are roots");
}

// ---------------------------------------------------------------------------
// Pages, residency and the cut.
//
// The property these pin is the one the whole streaming design rests on: a page
// holds whole groups, a page declares the pages it depends on, and
// `streamingBitsetForGroup` marks a cluster a streaming leaf exactly while any
// of its DAG children sits on a page that is not resident yet. Together that
// means **every prefix of the install order is a valid cut** - no hole, no
// overlap - at any error budget. Nothing tested that, and it is the invariant
// the cross-chunk work kept trying to reproduce by other means.
// ---------------------------------------------------------------------------

struct PagedBuild
{
  vg::VirtualGeometryBuildData build;
  svr::SparseVoxelOctreeVirtualGeometryStats stats;
  std::vector<uint32_t> clusterToPage;
  bool valid = false;
};

PagedBuild buildPagedChunk(
    Fixture &fixture,
    const float leafSelfError,
    const bool splitGroupsAcrossPages = false,
    const uint64_t maxPageBytes = 0u)
{
  PagedBuild paged{};
  const ChunkSpan span = chunkSpan(fixture.chunks, 0u);
  svr::SparseVoxelOctreeVirtualGeometrySettings settings{};
  settings.leafSelfError = leafSelfError;
  vg::VirtualGeometryBuildSettings buildSettings{};
  // Small pages, so one chunk produces a real install sequence to walk.
  buildSettings.maxGroupsPerPage = 4u;
  buildSettings.splitGroupsAcrossPages = splitGroupsAcrossPages;
  buildSettings.maxPageBytes = maxPageBytes;
  std::string error;
  paged.valid = svr::buildSparseVoxelOctreeVirtualGeometry(
      fixture.chunks.bricks, span.first, span.count, settings, buildSettings,
      paged.build, &paged.stats, &error);
  check(paged.valid, "octree virtual geometry builds: " + error);
  if (!paged.valid)
    return paged;

  // A cluster lives on the page of the group that *consumes* it - the packer
  // builds its group lists from `originalClusterIndices` and the builder's own
  // residency test is `groupToPage[clusters[c].groupId] <= pageIndex`. A
  // cluster is also the simplification of some other group, and that group sits
  // on a different page; mapping through `simplifiedClusterIndices` as well
  // overwrites the answer with the wrong page.
  //
  // A span carries only part of its group once `splitGroupsAcrossPages` is on,
  // so the page is taken per cluster from the span's own range rather than
  // from the whole group - mapping the whole group would let the last page a
  // group touches claim all of its clusters.
  paged.clusterToPage.assign(paged.build.clusters.size(), UINT32_MAX);
  // Carried across pages: a split group's later span continues where its
  // earlier one stopped, so this must not reset per page.
  std::map<uint32_t, uint32_t> placedPerGroup;
  for (uint32_t pageIndex = 0u; pageIndex < paged.build.pages.size(); ++pageIndex)
  {
    for (const auto &group : paged.build.pages[pageIndex].groups)
    {
      if (group.globalGroupId >= paged.build.groupInfos.size())
        continue;
      const auto &originals =
          paged.build.groupInfos[group.globalGroupId].originalClusterIndices;
      // Spans of one group arrive in page order, so the clusters this page
      // holds start where the previous pages left off.
      uint32_t &placed = placedPerGroup[group.globalGroupId];
      for (uint32_t offset = 0u; offset < group.clusterCount; ++offset)
      {
        const size_t index = static_cast<size_t>(placed) + offset;
        if (index >= originals.size())
          break;
        const int32_t cluster = originals[index];
        if (static_cast<size_t>(cluster) < paged.clusterToPage.size())
          paged.clusterToPage[static_cast<size_t>(cluster)] = pageIndex;
      }
      placed += group.clusterCount;
    }
  }
  return paged;
}

// A page is closed rather than split a group, so every cluster of a group lands
// on one page. If a group straddled two pages, installing the first would make
// half a substitution resident and the cut could take it.
void testPagesHoldWholeGroups()
{
  std::cout << "a page holds whole groups\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;
  PagedBuild paged = buildPagedChunk(fixture, 0.0f);
  if (!paged.valid)
    return;

  check(paged.build.pages.size() > 1u, "the chunk packs into several pages");

  uint32_t splitGroups = 0u;
  for (const auto &group : paged.build.groupInfos)
  {
    uint32_t page = UINT32_MAX;
    bool split = false;
    const auto visit = [&](const int32_t cluster)
    {
      if (static_cast<size_t>(cluster) >= paged.clusterToPage.size())
        return;
      const uint32_t clusterPage = paged.clusterToPage[static_cast<size_t>(cluster)];
      if (clusterPage == UINT32_MAX)
        return;
      if (page == UINT32_MAX)
        page = clusterPage;
      else if (page != clusterPage)
        split = true;
    };
    for (const int32_t cluster : group.originalClusterIndices)
      visit(cluster);
    if (split)
      ++splitGroups;
  }
  check(
      splitGroups == 0u,
      "no group's clusters straddle two pages (" + std::to_string(splitGroups) +
          " do)");
}

// The same chunk with `splitGroupsAcrossPages`, which is what makes the page a
// byte budget rather than a group boundary.
//
// Splitting is only safe because a partly resident group is *disabled*: its
// clusters share one parent bound and stand in for their DAG children as a
// unit, so the coarse cluster they replace keeps its streaming mark until the
// last of them lands. Enabling the resident half would draw it over that
// coarse cluster - half a substitution taken, which is the overlap the test
// above exists to prevent. So the invariant is not "pages hold whole groups"
// any more; it is "a page holds whole groups, or the part it holds draws
// nothing until the rest arrives".
void testSplitGroupsAreDisabledUntilComplete()
{
  std::cout << "a group split across pages draws nothing until it is complete\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;
  // A budget that lands *inside* a group rather than between two.
  //
  // Where it falls depends on the fixture's cluster sizes, so rather than
  // hard-coding one, walk a range and take the first that actually straddles -
  // a budget that happens to align with a group boundary would make this test
  // pass while exercising nothing.
  const auto pagesPerGroup = [](const PagedBuild &build)
  {
    std::map<uint32_t, std::vector<uint32_t>> result;
    for (uint32_t groupIndex = 0u;
         groupIndex < build.build.groupInfos.size(); ++groupIndex)
    {
      std::vector<uint32_t> pagesOfGroup;
      for (const int32_t cluster :
           build.build.groupInfos[groupIndex].originalClusterIndices)
      {
        if (static_cast<size_t>(cluster) >= build.clusterToPage.size())
          continue;
        const uint32_t page = build.clusterToPage[static_cast<size_t>(cluster)];
        if (page != UINT32_MAX)
          pagesOfGroup.push_back(page);
      }
      if (!pagesOfGroup.empty())
        result[groupIndex] = pagesOfGroup;
    }
    return result;
  };
  const auto straddlingCount = [](const std::map<uint32_t, std::vector<uint32_t>> &groups)
  {
    uint32_t count = 0u;
    for (const auto &[groupIndex, pagesOfGroup] : groups)
    {
      (void)groupIndex;
      if (*std::min_element(pagesOfGroup.begin(), pagesOfGroup.end()) !=
          *std::max_element(pagesOfGroup.begin(), pagesOfGroup.end()))
        ++count;
    }
    return count;
  };

  PagedBuild paged{};
  std::map<uint32_t, std::vector<uint32_t>> groupPages;
  uint32_t straddling = 0u;
  uint64_t chosenBudget = 0u;
  for (uint64_t budget = 2048u; budget <= 64u * 1024u && straddling == 0u; budget += 512u)
  {
    PagedBuild candidate = buildPagedChunk(fixture, 0.0f, true, budget);
    if (!candidate.valid || candidate.build.pages.size() <= 1u)
      continue;
    auto candidateGroups = pagesPerGroup(candidate);
    const uint32_t candidateStraddling = straddlingCount(candidateGroups);
    if (candidateStraddling == 0u)
      continue;
    paged = std::move(candidate);
    groupPages = std::move(candidateGroups);
    straddling = candidateStraddling;
    chosenBudget = budget;
  }
  check(
      straddling != 0u,
      "some byte budget makes groups straddle pages (" +
          std::to_string(straddling) + " at " + std::to_string(chosenBudget) +
          " bytes)");
  if (straddling == 0u)
    return;
  check(paged.build.pages.size() > 1u, "the chunk packs into several pages");

  // A group's clusters must run in non-decreasing page order: the packer
  // places a prefix and resumes on a later page, never out of order.
  uint32_t outOfOrder = 0u;
  for (const auto &[groupIndex, pagesOfGroup] : groupPages)
  {
    (void)groupIndex;
    if (!std::is_sorted(pagesOfGroup.begin(), pagesOfGroup.end()))
      ++outOfOrder;
  }
  check(outOfOrder == 0u, "every group's clusters run in page order");

  // The enabled mask is the safety property. For each group, find the page
  // that completes it; no update before that page may enable any of its
  // clusters, and the completing page must enable them.
  uint32_t enabledEarly = 0u;
  uint32_t completedGroups = 0u;
  for (const auto &[groupIndex, pagesOfGroup] : groupPages)
  {
    const uint32_t lastPage =
        *std::max_element(pagesOfGroup.begin(), pagesOfGroup.end());
    if (*std::min_element(pagesOfGroup.begin(), pagesOfGroup.end()) == lastPage)
      continue; // not split
    bool enabledAtCompletion = false;
    for (uint32_t pageIndex = 0u; pageIndex < paged.build.pages.size(); ++pageIndex)
    {
      // Updates naming this group's nodes: a split group owns several leaves,
      // so match by node through the group's own span nodes is not available
      // here - instead check every update on pages before completion carries a
      // zero enabled mask for nodes that belong to this group's leaves.
      for (const auto &update : paged.build.pages[pageIndex].installUpdates.hierarchyUpdates)
      {
        if (update.hierarchyNodeIndex >= paged.build.lodLevelHierarchy.size())
          continue;
        const auto &node = paged.build.lodLevelHierarchy[update.hierarchyNodeIndex];
        if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u || node.child_count == 0u)
          continue;
        // The leaf's first cluster identifies the group it serves.
        // The high bit marks "not installed"; the low bits are the page.
        const uint32_t page = node.pageIndex & ~(1u << 31);
        if (page >= paged.build.pages.size())
          continue;
        const uint32_t globalCluster =
            paged.build.pages[page].clusterOffset + node.child_start;
        if (globalCluster >= paged.build.clusters.size() ||
            paged.build.clusters[globalCluster].groupId != groupIndex)
          continue;
        if (pageIndex < lastPage && update.enabledClustersBitset != 0u)
          ++enabledEarly;
        if (pageIndex >= lastPage && update.enabledClustersBitset != 0u)
          enabledAtCompletion = true;
      }
    }
    if (enabledAtCompletion)
      ++completedGroups;
  }
  check(enabledEarly == 0u, "no split group is enabled before its last page (" + std::to_string(enabledEarly) + " were)");
  check(completedGroups != 0u, "and split groups are enabled once complete (" + std::to_string(completedGroups) + ")");
}

// Splitting must not change what geometry exists, only where it lives.
void testSplitPreservesTheClusterSet()
{
  std::cout << "splitting groups across pages preserves the cluster set\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;
  PagedBuild whole = buildPagedChunk(fixture, 0.0f, false, 6u * 1024u);
  PagedBuild split = buildPagedChunk(fixture, 0.0f, true, 6u * 1024u);
  if (!whole.valid || !split.valid)
    return;

  check(
      whole.build.clusters.size() == split.build.clusters.size(),
      "the same cluster count either way (" +
          std::to_string(whole.build.clusters.size()) + " vs " +
          std::to_string(split.build.clusters.size()) + ")");
  uint64_t wholeTriangles = 0u;
  uint64_t splitTriangles = 0u;
  for (const auto &cluster : whole.build.clusters)
    wholeTriangles += cluster.indices.size() / 3u;
  for (const auto &cluster : split.build.clusters)
    splitTriangles += cluster.indices.size() / 3u;
  check(wholeTriangles == splitTriangles, "and the same triangle count");
  check(
      split.build.pages.size() <= whole.build.pages.size(),
      "splitting never needs more pages (" +
          std::to_string(split.build.pages.size()) + " vs " +
          std::to_string(whole.build.pages.size()) + ")");

  // Every cluster lands on exactly one page, both ways.
  const auto placedOnce = [](const PagedBuild &build)
  {
    for (const uint32_t page : build.clusterToPage)
      if (page == UINT32_MAX)
        return false;
    return true;
  };
  check(placedOnce(whole), "every cluster is placed without splitting");
  check(placedOnce(split), "every cluster is placed with splitting");

  // A split group becomes one leaf per page under a synthetic internal node,
  // so the hierarchy gains nodes but every leaf still names one page.
  check(
      split.build.lodLevelHierarchy.size() >= whole.build.lodLevelHierarchy.size(),
      "the split hierarchy is no smaller");
  uint32_t leavesWithoutPage = 0u;
  for (const auto &node : split.build.lodLevelHierarchy)
  {
    if ((node.flags & vg::HIERARCHY_LEAF_FLAG) == 0u)
      continue;
    if (node.pageIndex == UINT32_MAX)
      ++leavesWithoutPage;
  }
  check(leavesWithoutPage == 0u, "every leaf names a page after the split");
}

// Dependencies must let the pages install in index order, and a coarse cluster
// must never land on a later page than the children it stands in for - the
// streaming mark is derived from "child's page <= this page", so an inverted
// pair would clear a mark for geometry that is not there.
void testPageDependenciesInstallInOrder()
{
  std::cout << "page dependencies admit an in-order install\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Canyon);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;
  PagedBuild paged = buildPagedChunk(fixture, 0.0f);
  if (!paged.valid)
    return;

  uint32_t forwardDependencies = 0u;
  for (uint32_t pageIndex = 0u; pageIndex < paged.build.pages.size(); ++pageIndex)
    for (const uint32_t dependency : paged.build.pages[pageIndex].dependencies)
      if (dependency >= pageIndex)
        ++forwardDependencies;
  check(
      forwardDependencies == 0u,
      "every page depends only on earlier pages (" +
          std::to_string(forwardDependencies) + " do not)");

  uint32_t invertedEdges = 0u;
  for (uint32_t cluster = 0u; cluster < paged.build.clusterDAG.size(); ++cluster)
  {
    const uint32_t coarsePage = paged.clusterToPage[cluster];
    if (coarsePage == UINT32_MAX)
      continue;
    for (const uint32_t child : paged.build.clusterDAG[cluster])
    {
      if (child >= paged.clusterToPage.size())
        continue;
      const uint32_t childPage = paged.clusterToPage[child];
      if (childPage != UINT32_MAX && childPage < coarsePage)
        ++invertedEdges;
    }
  }
  check(
      invertedEdges == 0u,
      "a cluster's DAG children never precede it in the install order (" +
          std::to_string(invertedEdges) + " do)");
}

// The one that matters. Install the pages in order and, after every prefix,
// check the cut over exactly what is resident: a cluster whose children are not
// all resident is a streaming leaf and draws at any budget below its parent's,
// so every root-to-leaf path must select exactly one cluster - never two (an
// overlap) and never none (a hole).
// Walks every prefix of the install order at a range of budgets and requires
// that each resident path is drawn exactly once.
//
// Two residency questions, and splitting is why they are different. A cluster's
// *geometry* is resident once its page is installed, and that is what decides
// whether a coarser cluster is still a streaming leaf. Whether it may be
// *selected* is stricter: its whole group must be resident, because the group
// substitutes for its DAG children as a unit and the packer disables a
// partly-landed group's clusters until the last of them arrives. With
// splitting off the two coincide, since a group never straddles a page.
void checkInstallPrefixesAreValidCuts(
    PagedBuild &paged, const float leafError, const std::string &label)
{
  const std::vector<uint32_t> parents = parentClusterMap(paged.build);
  const uint32_t pageCount = static_cast<uint32_t>(paged.build.pages.size());
  check(pageCount > 1u, "the chunk packs into several pages");

  std::vector<float> budgets;
  for (float scale = 0.5f; scale <= 64.0f; scale *= 2.0f)
    budgets.push_back(leafError * scale);

  uint32_t prefixesChecked = 0u;
  uint32_t holes = 0u;
  uint32_t overlaps = 0u;
  for (uint32_t installed = 1u; installed <= pageCount; ++installed)
  {
    const auto resident = [&](const uint32_t cluster)
    {
      return cluster < paged.clusterToPage.size() &&
          paged.clusterToPage[cluster] != UINT32_MAX &&
          paged.clusterToPage[cluster] < installed;
    };
    // A cluster may only be selected once every cluster of its group is
    // resident: that is the enabled mask the packer emits.
    const auto selectable = [&](const uint32_t cluster)
    {
      if (!resident(cluster))
        return false;
      if (cluster >= paged.build.clusters.size())
        return false;
      const uint32_t groupId = paged.build.clusters[cluster].groupId;
      if (groupId >= paged.build.groupInfos.size())
        return true;
      for (const int32_t sibling :
           paged.build.groupInfos[groupId].originalClusterIndices)
        if (!resident(static_cast<uint32_t>(sibling)))
          return false;
      return true;
    };
    // The builder's own rule, evaluated at this residency.
    const auto isStreamingLeaf = [&](const uint32_t cluster)
    {
      if (cluster >= paged.build.clusterDAG.size())
        return true;
      if (paged.build.clusterDAG[cluster].empty())
        return true;
      for (const uint32_t child : paged.build.clusterDAG[cluster])
        if (!resident(child))
          return true;
      return false;
    };

    // Roots of what is resident: every path starts at a cluster nothing
    // resident refines into.
    for (const float budget : budgets)
    {
      ++prefixesChecked;
      for (uint32_t cluster = 0u; cluster < paged.build.clusters.size(); ++cluster)
      {
        if (!resident(cluster))
          continue;
        // Walk from a resident leaf up to the root, counting selections.
        if (!paged.build.clusterDAG[cluster].empty() &&
            !isStreamingLeaf(cluster))
          continue; // not the bottom of a resident path
        if (!selectable(cluster))
          continue; // a partly-landed group draws nothing; its parent covers it
        uint32_t selected = 0u;
        uint32_t cursor = cluster;
        while (cursor != UINT32_MAX && resident(cursor))
        {
          const auto &entry = paged.build.clusters[cursor];
          const float self = isStreamingLeaf(cursor) ? 0.0f : entry.self.error;
          if (selectable(cursor) && self <= budget && entry.parent.error > budget)
            ++selected;
          cursor = parents[cursor];
        }
        if (selected == 0u)
          ++holes;
        else if (selected > 1u)
          ++overlaps;
      }
    }
  }
  check(prefixesChecked != 0u, label + ": the install sequence was walked");
  check(
      holes == 0u,
      label + ": no resident path is left undrawn at any budget (" +
          std::to_string(holes) + " holes)");
  check(
      overlaps == 0u,
      label + ": no resident path draws twice at any budget (" +
          std::to_string(overlaps) + " overlaps)");
}

void testEveryInstallPrefixIsAValidCut()
{
  std::cout << "every prefix of the install order is a valid cut\n";

  Fixture fixture = buildFixture(terrain_phase0::FixtureKind::Cliff);
  check(fixture.valid, "CPU terrain fixture builds");
  if (!fixture.valid || fixture.chunks.chunks.empty())
    return;
  // Terrain's convention: a real finest error plus streaming marks, which is
  // the configuration where a wrong mark shows as a hole rather than as an
  // always-drawn leaf.
  const float leafError =
      static_cast<float>(fixture.chunks.chunks[0].voxelSizeMetres);
  PagedBuild paged = buildPagedChunk(fixture, leafError);
  if (!paged.valid)
    return;
  checkInstallPrefixesAreValidCuts(paged, leafError, "whole groups");

  // And with groups straddling pages, which is the configuration the property
  // could plausibly break in: half a substitution resident at some prefix.
  uint32_t straddling = 0u;
  for (uint64_t budget = 2048u; budget <= 64u * 1024u && straddling == 0u; budget += 512u)
  {
    PagedBuild split = buildPagedChunk(fixture, leafError, true, budget);
    if (!split.valid || split.build.pages.size() <= 1u)
      continue;
    for (const auto &group : split.build.groupInfos)
    {
      uint32_t first = UINT32_MAX;
      for (const int32_t cluster : group.originalClusterIndices)
      {
        const uint32_t page = static_cast<size_t>(cluster) < split.clusterToPage.size()
            ? split.clusterToPage[static_cast<size_t>(cluster)]
            : UINT32_MAX;
        if (page == UINT32_MAX)
          continue;
        if (first == UINT32_MAX)
          first = page;
        else if (page != first)
          ++straddling;
      }
    }
    if (straddling != 0u)
      checkInstallPrefixesAreValidCuts(split, leafError, "split groups");
  }
  check(straddling != 0u, "a split configuration was found to check");
}

int main()
{
  std::cout << "Sparse voxel octree virtual geometry tests\n";
  testExposedFaceMasks();
  testClosedSelectableBlocks();
  testQuadWindingAndMerging();
  testOctreeBuildProducesRealLODChain();
  testClusterErrorIsAtLeastItsOwnVoxel();
  testGeometryMatchesVoxelSurface();
  testNoSimplifierAndSpeedAgainstTheOldBridge();
  testEveryExteriorVoxelFaceIsMeshedPerLevel();
  testOctreeLevelsAreValidCoarsenings();
  testClusterCutsAreBrickBlockBounds();
  testDAGCutIsCrackFreeAndRefinesWithThreshold();
  testSelectedCutCoversTheWholeSurface();
  testClusterConesNeverCullAVisibleFace();
  testEncodedTerrainPageHasAStreamableLODChain();
  testFinestClustersAreStreamingLeavesWhenTheyCarryError();
  testGroupBoundsAreSharedByChildrenAndParent();
  testSplitGroupsAreDisabledUntilComplete();
  testSplitPreservesTheClusterSet();
  testEveryClusterIsConsumedByAtMostOneGroup();
  testParentChunkErrorClampsTheChunkToOneOctave();
  testRootParentErrorFollowsItsFlag();
  testPagesHoldWholeGroups();
  testPageDependenciesInstallInOrder();
  testEveryInstallPrefixIsAValidCut();
  testEveryFixtureBuilds();

  if (failures != 0)
  {
    std::cout << failures << " sparse voxel octree virtual geometry checks failed\n";
    return 1;
  }
  std::cout << "all sparse voxel octree virtual geometry checks passed\n";
  return 0;
}
