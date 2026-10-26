#pragma once

#include "editor/virtualgeometry/VoxelOctreeVirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/SparseVoxelTerrainTriangleMesh.hpp"

#include <array>
#include <cstdint>
#include <string>

namespace voxel::svraster
{

struct SparseVoxelTerrainTriangleVirtualGeometryBuildStats
{
  uint32_t sourceClusterCount = 0u;
  uint32_t sourceTriangleCount = 0u;
  uint32_t encodedPageCount = 0u;
  uint32_t encodedMeshletCount = 0u;
  uint32_t encodedHierarchyNodeCount = 0u;
  uint32_t encodedGroupCount = 0u;
  uint64_t encodedVertexPositionWords = 0u;
  uint64_t encodedIndexBytes = 0u;
};

// Legacy path: LOD0 exposed-face clusters through the meshoptimizer builder.
//
// Kept as the A/B baseline. It cannot produce a LOD chain - see the note on
// buildSparseVoxelOctreeVirtualGeometry - so it is not the route the renderer
// should take.
bool buildSparseVoxelTerrainTriangleVirtualGeometryAsset(
    const SparseVoxelTerrainTrianglePageSidecar &sidecar,
    const SparseVoxelTerrainTriangleMeshSettings &meshSettings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    const virtualgeometry::QuantizationConfig &quantization,
    virtualgeometry::VirtualGeometryEncodedData &result,
    SparseVoxelTerrainTriangleVirtualGeometryBuildStats *stats = nullptr,
    std::string *error = nullptr);

// Octree path: every brick level meshed, the DAG taken from the octree.
//
// This is the terrain-facing wrapper around buildSparseVoxelOctreeVirtualGeometry.
// It resolves the chunk's brick node span and page origin, builds positions
// relative to that origin so a planet-scale coordinate never reaches a float,
// and encodes the result into streamable virtual geometry pages.
struct SparseVoxelTerrainOctreeVirtualGeometryBuildStats
{
  SparseVoxelOctreeVirtualGeometryStats build;
  uint32_t encodedPageCount = 0u;
  uint32_t encodedMeshletCount = 0u;
  uint32_t encodedHierarchyNodeCount = 0u;
  uint32_t encodedGroupCount = 0u;
  uint64_t encodedVertexPositionWords = 0u;
  uint64_t encodedIndexBytes = 0u;
  double encodeMilliseconds = 0.0;
};

bool buildSparseVoxelTerrainOctreeVirtualGeometryAsset(
    const PlanetVoxelChunkSet &chunks,
    uint32_t chunkIndex,
    const SparseVoxelOctreeVirtualGeometrySettings &octreeSettings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    const virtualgeometry::QuantizationConfig &quantization,
    virtualgeometry::VirtualGeometryEncodedData &result,
    std::array<double, 3> *pageOrigin = nullptr,
    SparseVoxelTerrainOctreeVirtualGeometryBuildStats *stats = nullptr,
    std::string *error = nullptr);

} // namespace voxel::svraster
