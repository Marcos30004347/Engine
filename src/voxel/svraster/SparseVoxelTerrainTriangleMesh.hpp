#pragma once

#include "voxel/svraster/PlanetVoxelChunk.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{
struct VirtualGeometryBuildData;
}

namespace voxel::svraster
{

inline constexpr uint32_t kSparseVoxelTerrainTriangleInvalidIndex = 0xffffffffu;
// Face-local quads cost four private vertices each, so the binding constraint is
// virtualgeometry::MaxVertices (192), not the 128-triangle cluster budget. 64
// quads is 256 vertices, and Meshlet::vertex_count is a uint8: the count wrapped
// to zero and the encoded meshlet decoded as empty. 48 quads is 192 vertices and
// 96 triangles, which fits both budgets exactly.
inline constexpr uint32_t kSparseVoxelTerrainTriangleClusterDefaultQuadCapacity = 48u;

struct SparseVoxelTerrainTriangleMeshSettings
{
  // Keep clusters inside both virtual geometry budgets: every quad is two
  // triangles and, with face-local corners, four vertices. The vertex budget
  // binds first.
  uint32_t maxQuadsPerCluster = kSparseVoxelTerrainTriangleClusterDefaultQuadCapacity;
  // Default terrain VG payloads are face-local: every exposed voxel face emits
  // one quad with four private vertices.  This keeps the future cube-unwrap
  // texture assignment, per-face colour and hard face normal independent from
  // neighbouring voxels.  Greedy quads remain available as an explicit
  // compression experiment by setting this to false.
  bool emitOneQuadPerExposedFace = true;
  // Start with the natural terrain streaming unit. Page-wide merging is useful
  // as an offline ceiling, but brick-local clusters preserve simple page/brick
  // ownership and make missing sidecars easy to fall back from.
  bool brickLocalGreedy = true;
};

struct SparseVoxelTerrainTriangleVertex
{
  std::array<float, 3> position{};
  std::array<int8_t, 3> normal{};
  uint8_t face = 0u;
  std::array<float, 2> uv{};
  SparseVoxelBrickMaterialGPU material{};
};

struct SparseVoxelTerrainTriangleCluster
{
  uint32_t firstVertex = 0u;
  uint32_t vertexCount = 0u;
  uint32_t firstIndex = 0u;
  uint32_t indexCount = 0u;
  uint32_t firstQuad = 0u;
  uint32_t quadCount = 0u;
  uint32_t sourceBrickNode = kSparseVoxelTerrainTriangleInvalidIndex;
  uint32_t lodGroup = kSparseVoxelBrickLODGroupInvalidIndex;
  std::array<float, 3> boundsMin{};
  std::array<float, 3> boundsMax{};
  float selfError = 0.0f;
  float parentError = 0.0f;
};

struct SparseVoxelTerrainTrianglePageSidecar
{
  uint32_t chunkIndex = 0u;
  uint64_t brickPayloadRevision = 0u;
  uint64_t tree64Revision = 0u;
  uint32_t clipmapLOD = 0u;
  uint32_t regionMappingId = 0u;
  double voxelSizeMetres = 0.0;
  double edgeMetres = 0.0;
  std::array<double, 3> pageMinimum{};
  std::vector<SparseVoxelTerrainTriangleVertex> vertices;
  std::vector<uint32_t> indices;
  std::vector<SparseVoxelTerrainTriangleCluster> clusters;
  uint64_t occupiedCells = 0u;
  uint64_t exposedFaces = 0u;
  uint64_t greedyQuads = 0u;

  bool empty() const { return clusters.empty(); }
};

struct SparseVoxelTerrainTriangleVirtualGeometryPreview
{
  uint32_t pageCount = 0u;
  uint32_t hierarchyNodeCount = 0u;
  uint32_t meshletCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t maxTrianglesPerMeshlet = 0u;
  uint32_t maxVerticesPerMeshlet = 0u;
  uint64_t estimatedVertexBytes = 0u;
  uint64_t estimatedIndexBytes = 0u;
  uint64_t estimatedClusterBytes = 0u;
  uint64_t estimatedTotalBytes = 0u;
  bool virtualGeometryCompatible = false;
};

bool buildSparseVoxelTerrainTrianglePageSidecar(
    const PlanetVoxelChunkSet &chunks,
    uint32_t chunkIndex,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    SparseVoxelTerrainTrianglePageSidecar &result,
    std::string *error = nullptr);

SparseVoxelTerrainTriangleVirtualGeometryPreview
summarizeSparseVoxelTerrainTriangleVirtualGeometryPreview(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar);

bool buildSparseVoxelTerrainTriangleVirtualGeometryPreview(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    virtualgeometry::VirtualGeometryBuildData &result,
    std::string *error = nullptr);

bool validateSparseVoxelTerrainTrianglePageSidecar(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &settings,
    std::string *error = nullptr);

uint64_t sparseVoxelTerrainTrianglePageSidecarHash(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar);

} // namespace voxel::svraster
