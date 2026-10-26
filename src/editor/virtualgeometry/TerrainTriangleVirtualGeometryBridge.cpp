#include "editor/virtualgeometry/TerrainTriangleVirtualGeometryBridge.hpp"
#include "editor/virtualgeometry/VirtualGeometryPacker.hpp"

#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyBuilder.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace voxel::svraster
{
namespace
{

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

virtualgeometry::LODBounds boundsForCluster(
    const SparseVoxelTerrainTriangleCluster &source)
{
  virtualgeometry::LODBounds result{};
  result.center[0] = (source.boundsMin[0] + source.boundsMax[0]) * 0.5f;
  result.center[1] = (source.boundsMin[1] + source.boundsMax[1]) * 0.5f;
  result.center[2] = (source.boundsMin[2] + source.boundsMax[2]) * 0.5f;
  const float dx = source.boundsMax[0] - source.boundsMin[0];
  const float dy = source.boundsMax[1] - source.boundsMin[1];
  const float dz = source.boundsMax[2] - source.boundsMin[2];
  result.radius = std::sqrt(dx * dx + dy * dy + dz * dz) * 0.5f;
  result.error = source.selfError;
  return result;
}

void accumulateEncodedStats(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const virtualgeometry::VirtualGeometryEncodedData &asset,
    SparseVoxelTerrainTriangleVirtualGeometryBuildStats &stats)
{
  stats = {};
  stats.sourceClusterCount = static_cast<uint32_t>(sidecar.clusters.size());
  for (const auto &cluster : sidecar.clusters)
    stats.sourceTriangleCount += cluster.indexCount / 3u;
  stats.encodedPageCount = static_cast<uint32_t>(asset.pages.size());
  stats.encodedHierarchyNodeCount = static_cast<uint32_t>(asset.hierarchy.size());
  for (const auto &page : asset.pages)
  {
    stats.encodedMeshletCount += static_cast<uint32_t>(page.meshlets.size());
    stats.encodedGroupCount += page.groupCount;
    stats.encodedVertexPositionWords += page.vertexBuffers.positions.size();
    stats.encodedIndexBytes += page.vertexBuffers.indices.size();
  }
}

} // namespace

bool buildSparseVoxelTerrainTriangleVirtualGeometryAsset(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &meshSettings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    const virtualgeometry::QuantizationConfig &quantization,
    virtualgeometry::VirtualGeometryEncodedData &result,
    SparseVoxelTerrainTriangleVirtualGeometryBuildStats *stats,
    std::string *error)
{
  result = {};
  if (stats != nullptr)
    *stats = {};
  if (error != nullptr)
    error->clear();

  if (!validateSparseVoxelTerrainTrianglePageSidecar(sidecar, meshSettings, error))
    return false;
  if (sidecar.empty())
    return true;

  const auto preview =
      summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview(sidecar);
  if (!preview.virtualGeometryCompatible)
    return fail(error, "Triangle terrain sidecar has clusters too large for virtual geometry");

  std::vector<virtualgeometry::VirtualGeometryCluster> clusters;
  clusters.reserve(sidecar.clusters.size());
  for (const auto &source : sidecar.clusters)
  {
    virtualgeometry::VirtualGeometryCluster cluster{};
    cluster.groupId = source.lodGroup == kSparseVoxelBrickLODGroupInvalidIndex
        ? UINT32_MAX
        : source.lodGroup;
    cluster.meshPartIndex = 0u;
    cluster.self = boundsForCluster(source);
    cluster.parent = cluster.self;
    cluster.parent.error = std::numeric_limits<float>::max();
    cluster.vertices.reserve(source.vertexCount);
    for (uint32_t vertexIndex = 0u; vertexIndex < source.vertexCount; ++vertexIndex)
    {
      const auto &vertex = sidecar.vertices[source.firstVertex + vertexIndex];
      virtualgeometry::Vertex out{};
      out.pos[0] = vertex.position[0];
      out.pos[1] = vertex.position[1];
      out.pos[2] = vertex.position[2];
      out.norm[0] = static_cast<float>(vertex.normal[0]);
      out.norm[1] = static_cast<float>(vertex.normal[1]);
      out.norm[2] = static_cast<float>(vertex.normal[2]);
      out.uv[0] = vertex.uv[0];
      out.uv[1] = vertex.uv[1];
      cluster.vertices.push_back(std::move(out));
    }
    cluster.indices.reserve(source.indexCount);
    for (uint32_t indexOffset = 0u; indexOffset < source.indexCount; ++indexOffset)
    {
      const uint32_t globalIndex = sidecar.indices[source.firstIndex + indexOffset];
      cluster.indices.push_back(globalIndex - source.firstVertex);
    }
    clusters.push_back(std::move(cluster));
  }

  virtualgeometry::VirtualGeometryBuildSettings terrainBuildSettings = buildSettings;
  terrainBuildSettings.maxGroupsPerPage =
      std::max<uint32_t>(1u, terrainBuildSettings.maxGroupsPerPage);
  virtualgeometry::VirtualGeometryBuildData build =
      virtualgeometry::VirtualGeometryBuilder::build(std::move(clusters), terrainBuildSettings);
  if (build.clusters.empty() || build.pages.empty() || build.lodLevelHierarchy.empty())
    return fail(error, "Virtual geometry builder produced no terrain triangle pages");

  result = virtualgeometry::VirtualGeometryCompressor::encode(build, quantization);
  if (result.pages.empty() || result.hierarchy.empty() || result.shapes.empty())
    return fail(error, "Virtual geometry compressor produced an empty terrain triangle asset");

  if (stats != nullptr)
    accumulateEncodedStats(sidecar, result, *stats);
  return true;
}

bool buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
    const PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex,
    const SparseVoxelOctreeVirtualGeometrySettings &octreeSettings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    const virtualgeometry::QuantizationConfig &quantization,
    virtualgeometry::VirtualGeometryEncodedData &result,
    std::array<double, 3> *pageOrigin,
    SparseVoxelTerrainOctreeVirtualGeometryBuildStats *stats,
    std::string *error)
{
  result = {};
  if (stats != nullptr)
    *stats = {};
  if (pageOrigin != nullptr)
    *pageOrigin = {};
  if (error != nullptr)
    error->clear();

  if (chunkIndex >= chunks.chunks.size())
    return fail(error, "Terrain octree VG build needs a valid chunk index");

  const auto &record = chunks.chunks[chunkIndex];
  if (!(record.voxelSizeMetres > 0.0) || !(record.edgeMetres > 0.0) ||
      record.brickNodeOffset > chunks.bricks.nodes.size() ||
      record.brickNodeCapacity >
          chunks.bricks.nodes.size() - record.brickNodeOffset)
    return fail(error, "Terrain octree VG build found invalid page ranges");
  if (record.brickNodeCapacity == 0u)
    return true;

  // Same frame the raster/trace payloads use: the page's minimum corner, with
  // elevation measured against the chunk set's shared origin.
  const std::array<double, 3> origin{
      record.worldCenterXMetres - record.edgeMetres * 0.5,
      record.elevationOriginMetres - chunks.elevationOriginMetres,
      record.worldCenterZMetres - record.edgeMetres * 0.5};
  if (pageOrigin != nullptr)
    *pageOrigin = origin;

  SparseVoxelOctreeVirtualGeometrySettings settings = octreeSettings;
  settings.originOffset =
      octreeSettings.absoluteOrigin ? std::array<double, 3>{0.0, 0.0, 0.0} : origin;
  if (chunks.canonicalWorldGrid[3] > 0.0f)
  {
    std::copy(chunks.canonicalWorldGrid.begin(), chunks.canonicalWorldGrid.end(),
              settings.canonicalWorldGrid.begin());
    settings.canonicalWorldGrid[3] = std::ldexp(
        record.voxelSizeMetres, -static_cast<int>(record.clipmapLOD));
  }

  virtualgeometry::VirtualGeometryBuildData build{};
  SparseVoxelOctreeVirtualGeometryStats buildStats{};
  if (!buildSparseVoxelOctreeVirtualGeometry(
          chunks.bricks,
          record.brickNodeOffset,
          record.brickNodeCapacity,
          settings,
          buildSettings,
          build,
          &buildStats,
          error))
    return false;
  if (build.clusters.empty())
    return true;
  if (build.pages.empty() || build.lodLevelHierarchy.empty())
    return fail(error, "Terrain octree VG build produced no pages");

  const auto encodeStart = std::chrono::steady_clock::now();
  // Serial encode: a chunk is 4-32 pages and this already runs on one of the
  // page cache's workers, so the parallelism is per chunk, not per page.
  result = virtualgeometry::VirtualGeometryPacker::encode(build, quantization, nullptr, 1u);
  const double encodeMilliseconds =
      std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - encodeStart)
          .count();
  if (result.pages.empty() || result.hierarchy.empty() || result.shapes.empty())
    return fail(error, "Terrain octree VG encode produced an empty asset");

  if (stats != nullptr)
  {
    stats->build = std::move(buildStats);
    stats->encodeMilliseconds = encodeMilliseconds;
    stats->encodedPageCount = static_cast<uint32_t>(result.pages.size());
    stats->encodedHierarchyNodeCount =
        static_cast<uint32_t>(result.hierarchy.size());
    for (const auto &page : result.pages)
    {
      stats->encodedMeshletCount += static_cast<uint32_t>(page.meshlets.size());
      stats->encodedGroupCount += page.groupCount;
      stats->encodedVertexPositionWords += page.vertexBuffers.positions.size();
      stats->encodedIndexBytes += page.vertexBuffers.indices.size();
    }
  }
  return true;
}

bool buildSparseVoxelTerrainSurfaceVirtualGeometryAsset(
    const PlanetVoxelChunkSet &chunks,
    const uint32_t chunkIndex,
    const SparseVoxelOctreeVirtualGeometrySettings &octreeSettings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    const virtualgeometry::QuantizationConfig &quantization,
    virtualgeometry::VirtualGeometryEncodedData &result,
    std::array<double, 3> *pageOrigin,
    SparseVoxelTerrainOctreeVirtualGeometryBuildStats *stats,
    std::string *error)
{
  namespace vg = virtualgeometry;
  result = {};
  if (stats != nullptr)
    *stats = {};
  if (pageOrigin != nullptr)
    *pageOrigin = {};
  if (error != nullptr)
    error->clear();
  if (chunkIndex >= chunks.chunks.size())
    return fail(error, "Terrain surface VG build needs a valid chunk index");
  const auto &record = chunks.chunks[chunkIndex];
  if (!(record.voxelSizeMetres > 0.0) || !(record.edgeMetres > 0.0) ||
      record.brickNodeOffset > chunks.bricks.nodes.size() ||
      record.brickNodeCapacity > chunks.bricks.nodes.size() - record.brickNodeOffset)
    return fail(error, "Terrain surface VG build found invalid page ranges");
  if (record.brickNodeCapacity == 0u)
    return true;
  const std::array<double, 3> origin{
      record.worldCenterXMetres - record.edgeMetres * 0.5,
      record.elevationOriginMetres - chunks.elevationOriginMetres,
      record.worldCenterZMetres - record.edgeMetres * 0.5};
  if (pageOrigin != nullptr)
    *pageOrigin = origin;
  if (!(chunks.canonicalWorldGrid[3] > 0.0f))
    return fail(error, "Terrain surface VG build needs the canonical world grid");
  const std::array<double, 3> originOffset =
      octreeSettings.absoluteOrigin ? std::array<double, 3>{0.0, 0.0, 0.0} : origin;
  const double unit = std::ldexp(record.voxelSizeMetres, -static_cast<int>(record.clipmapLOD));

  // The chunk's finest bricks, on the global lattice in their own scale.
  const auto *nodes = chunks.bricks.nodes.data() + record.brickNodeOffset;
  uint32_t finestLevel = UINT32_MAX;
  for (uint32_t i = 0u; i < record.brickNodeCapacity; ++i)
    if (nodes[i].occupiedCount != 0u && nodes[i].childCount == 0u)
      finestLevel = std::min(finestLevel, nodes[i].level & 0xffu);
  if (finestLevel == UINT32_MAX)
    return true;
  int32_t scale = 0;
  std::array<int32_t, 2> low{INT32_MAX, INT32_MAX};
  std::vector<vg::VoxelSurfaceCell> cells;
  for (uint32_t i = 0u; i < record.brickNodeCapacity; ++i)
  {
    const auto &node = nodes[i];
    if (node.occupiedCount == 0u || node.childCount != 0u || (node.level & 0xffu) != finestLevel)
      continue;
    if (node.occupancyIndex >= chunks.bricks.occupancy.size())
      return fail(error, "Terrain surface VG brick occupancy is out of range");
    const int32_t nodeScale = node.canonicalGridOriginScale[3];
    if (nodeScale <= 0 || (scale != 0 && nodeScale != scale))
      return fail(error, "Terrain surface VG finest bricks differ in scale");
    scale = nodeScale;
    std::array<int32_t, 3> base{};
    for (int axis = 0; axis < 3; ++axis)
    {
      if (node.canonicalGridOriginScale[axis] % scale != 0)
        return fail(error, "Terrain surface VG brick is off its own lattice");
      base[axis] = node.canonicalGridOriginScale[axis] / scale;
    }
    low[0] = std::min(low[0], base[0]);
    low[1] = std::min(low[1], base[2]);
    const auto &occupancy = chunks.bricks.occupancy[node.occupancyIndex];
    for (uint32_t bit = 0u; bit < 512u; ++bit)
      if (occupancy.cells[bit / 32u].bits & (1u << (bit % 32u)))
        cells.push_back({{base[0] + int32_t(bit % 8u), base[1] + int32_t((bit / 8u) % 8u), base[2] + int32_t(bit / 64u)}, 0u});
  }
  if (cells.empty())
    return true;

  vg::VoxelSurfaceTriangleMeshSettings meshSettings;
  meshSettings.pitchMetres = double(scale) * unit;
  for (int axis = 0; axis < 3; ++axis)
    meshSettings.origin[axis] = double(chunks.canonicalWorldGrid[axis]) - originOffset[axis];
  const int32_t side = std::max(2, int32_t(std::lround(record.edgeMetres / meshSettings.pitchMetres)));
  const int32_t half = side / 2;

  // Four closed quadrant surfaces in one shape: the terrain runtime installs
  // a chunk through a single hierarchy root, so separate shapes drew one
  // quadrant per chunk. LOD0 clusters stay inside their quadrant (the meshes
  // are not connected); a coarse cluster that spans quadrants reports them
  // all through its block box and is replaced once all are resident.
  std::array<std::vector<vg::VoxelSurfaceCell>, 4> quadrantCells;
  for (const auto &cell : cells)
    quadrantCells[(cell.position[0] - low[0] >= half ? 1u : 0u) + (cell.position[2] - low[1] >= half ? 2u : 0u)].push_back(cell);
  std::vector<vg::Vertex> vertices;
  std::vector<vg::Shape> shapes(1);
  uint64_t triangles = 0u;
  for (uint32_t quadrant = 0u; quadrant < 4u; ++quadrant)
  {
    if (quadrantCells[quadrant].empty())
      continue;
    vg::VoxelSurfaceTriangleMesh mesh;
    if (!vg::buildVoxelSurfaceTriangleMesh(quadrantCells[quadrant], meshSettings, mesh, error))
      return false;
    vg::mergeVoxelSurfaceCoplanar(mesh, meshSettings);
    const uint32_t base = static_cast<uint32_t>(vertices.size());
    vertices.insert(vertices.end(), mesh.vertices.begin(), mesh.vertices.end());
    for (uint32_t index : mesh.indices)
      shapes[0].indices.push_back(base + index);
    triangles += mesh.indices.size() / 3u;
  }

  vg::VirtualGeometryBuildSettings settings = buildSettings;
  settings.clusterizer = vg::VirtualGeometryClusterizer::Meshopt;
  settings.simplifyPositionsOnly = true;
  settings.finestClusterError = std::max(octreeSettings.leafSelfError, 0.0f);
  settings.finestClustersAreStreamingLeaves = octreeSettings.leafSelfError > 0.0f;
  settings.frontThreads = 1u;
  vg::setVirtualGeometryBuildLogging(false);
  // Flat normals per view, as voxel faces shade (the welded mesh's are
  // smoothed). The per-view vertex copies need room under MaxVertices; a
  // chunk whose clusters still overflow is rebuilt with smaller ones, and
  // keeps smooth normals only if even 32 do not fit.
  vg::VirtualGeometryBuildData build;
  bool flat = false;
  for (const uint32_t cap : {64u, 48u, 32u})
  {
    std::vector<vg::Vertex> attempt = vertices;
    settings.clusterMaxVertices = cap;
    build = vg::VirtualGeometryBuilder::build(attempt, shapes, settings);
    if (build.clusters.empty() || build.pages.empty() || build.lodLevelHierarchy.empty())
      return fail(error, "Terrain surface VG build produced no pages");
    vg::VirtualGeometryBuildData withNormals = build;
    if (vg::assignVoxelSurfaceViewUVs(withNormals, meshSettings))
    {
      build = std::move(withNormals);
      flat = true;
      break;
    }
  }
  (void)flat;

  SparseVoxelOctreeVirtualGeometryStats buildStats{};
  buildStats.triangleCount = static_cast<uint32_t>(triangles);
  buildStats.clusterSources.resize(build.clusters.size());
  for (size_t c = 0; c < build.clusters.size(); ++c)
  {
    const auto &cluster = build.clusters[c];
    auto &source = buildStats.clusterSources[c];
    source.triangleCount = static_cast<uint32_t>(cluster.indices.size() / 3u);
    std::array<float, 3> lo{FLT_MAX, FLT_MAX, FLT_MAX}, hi{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto &v : cluster.vertices)
      for (int axis = 0; axis < 3; ++axis)
      {
        lo[axis] = std::min(lo[axis], v.pos[axis]);
        hi[axis] = std::max(hi[axis], v.pos[axis]);
      }
    source.surfaceMinimum = lo;
    source.surfaceMaximum = hi;
    source.surfaceValid = !cluster.vertices.empty();
    // Its own bounds as its block, so the quadrants it touches are the ones
    // that must all be resident before it is replaced.
    source.blockMinimum = lo;
    source.blockExtent = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], float(meshSettings.pitchMetres)});
    source.level = 0u;
  }

  result = vg::VirtualGeometryPacker::encode(build, quantization, nullptr, 1u);
  if (result.pages.empty() || result.hierarchy.empty() || result.shapes.empty())
    return fail(error, "Terrain surface VG encode produced an empty asset");
  if (stats != nullptr)
  {
    stats->build = std::move(buildStats);
    stats->encodedPageCount = static_cast<uint32_t>(result.pages.size());
    stats->encodedHierarchyNodeCount = static_cast<uint32_t>(result.hierarchy.size());
    for (const auto &page : result.pages)
    {
      stats->encodedMeshletCount += static_cast<uint32_t>(page.meshlets.size());
      stats->encodedGroupCount += page.groupCount;
      stats->encodedVertexPositionWords += page.vertexBuffers.positions.size();
      stats->encodedIndexBytes += page.vertexBuffers.indices.size();
    }
  }
  return true;
}

} // namespace voxel::svraster
