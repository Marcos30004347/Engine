#pragma once

#include "editor/virtualgeometry/VoxelSurfaceTriangleMesh.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

// Colour for a voxel surface's virtual geometry: UVs and one atlas, made
// after the DAG is built so they never constrain the geometry.
//
// The object is unwrapped by view. Every exposed voxel face belongs to the
// view it faces (+x, -x, +y, -y, +z, -z); the faces of one view are cut into
// flat connected patches, and patches are merged, largest first, into
// regions that do not overlap in projection - a facade with its recesses and
// trims seen as one picture. What would overlap (a wall behind an eave, the
// far side of a hole, a hole inside that) opens a further region of the same
// view. A region is one chart at one texel per voxel face, coloured straight
// from the faces.
//
// Those regions are shared by every level of the DAG: a triangle whose every
// covered texel finds, in its view's columns, a face within reach in depth
// and all in one region, draws from that region - LOD0 exactly, and the fine
// levels that stay on the surface. Only triangles that leave it (a coarse
// triangle bridging a window, a slope cut across steps) get charts of their
// cluster's own: projected by view, peeled into layers where a projection
// folds over itself, texels max(pitch, self error) wide, coloured from the
// voxel found along the triangle normal and averaged over subsamples.
// Texels with no voxel within reach and every chart's gutter are dilated
// from the chart's own texels.
struct VoxelSurfaceAtlasSettings
{
  uint32_t gutterTexels = 1u;
  uint32_t maxAtlasWidth = 8192u;
  uint32_t maxSubsamples = 4u; // Per texel side.
};

struct VoxelSurfaceAtlasChart
{
  uint32_t cluster = 0u; // UINT32_MAX for a view region every level shares.
  uint32_t view = 0u;  // 0 +x, 1 -x, 2 +y, 3 -y, 4 +z, 5 -z.
  uint32_t layer = 0u; // 0 for the first unwrap of that view in the cluster.
  uint32_t x = 0u, y = 0u, width = 0u, height = 0u; // Rectangle, gutter included.
};

struct VoxelSurfaceAtlas
{
  uint32_t width = 0u;
  uint32_t height = 0u;
  std::vector<uint32_t> rgba; // Row 0 is the top row; v = row / height.
  std::vector<VoxelSurfaceAtlasChart> charts;

  uint32_t sharedRegions = 0u;     // View regions charted (cluster UINT32_MAX in `charts`).
  uint64_t sharedTriangles = 0u;   // Triangles drawing from a view region.
  uint64_t ownTriangles = 0u;      // Triangles on their cluster's own charts.
  uint32_t peeledCharts = 0u;      // Own charts with layer > 0.
  uint32_t addedVertices = 0u;     // Vertex copies made where charts meet.
  uint32_t maxClusterVertices = 0u;
  uint32_t mergedClusters = 0u;    // Clusters on their own charts only, by view, folded, to fit MaxVertices.
  uint64_t coveredTexels = 0u;     // Texel centres inside a triangle.
  uint64_t missedTexels = 0u;      // Of those, no voxel within reach.
};

bool bakeVoxelSurfaceAtlas(
    VirtualGeometryBuildData &build,
    const std::vector<VoxelSurfaceCell> &cells,
    const VoxelSurfaceTriangleMeshSettings &meshSettings,
    const VoxelSurfaceAtlasSettings &settings,
    VoxelSurfaceAtlas &atlas,
    std::string *error = nullptr);

// The baked asset on disk: `<basePath>.png` (the atlas), `<basePath>.vmat`
// (its material, baseColor) and `<basePath>.vg` (the DAG, LZ4 pages).
bool writeVoxelSurfaceAsset(
    const VirtualGeometryBuildData &build,
    const VoxelSurfaceAtlas &atlas,
    const std::string &basePath,
    std::string *error = nullptr);

} // namespace virtualgeometry
