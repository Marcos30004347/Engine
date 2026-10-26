#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace virtualgeometry
{

// LOD0 of one voxel object instance as a single welded triangle mesh, so the
// normal virtual geometry builder (meshopt cluster, partition, simplify,
// repeat) makes every coarser level by pixel error instead of following the
// voxel octree.
//
// The mesh is a closed, consistently wound 2-manifold with one vertex per
// surface point and no attribute seams: no UV charts, no per-face normals, no
// per-material split. Every exposed voxel face is a quad whose corners are
// shared with every face around them, so the simplifier sees one connected
// surface, coplanar collapses cost exactly nothing, and nothing but a group
// border ever locks a vertex. Colour and shading normals are baked afterwards
// against the finished DAG; they never constrain the geometry.
//
// Voxel surfaces are not manifold as they stand: two cells that touch only
// along an edge or at a corner meet four (or more) faces at one edge. Solid
// cells are taken as 6-connected - across such an edge each face pairs with
// the face of its own cell - and a lattice point the surface passes through
// more than once becomes one vertex per pass, each moved a thousandth of a
// voxel into its own side so no two vertices share a position. An edge passed
// twice gets a midpoint per pass the same way.
struct VoxelSurfaceCell
{
  std::array<int32_t, 3> position{};
  uint32_t rgba = 0u;
};

struct VoxelSurfaceTriangleMeshSettings
{
  double pitchMetres = 0.1;
  // Position of lattice point (0, 0, 0).
  std::array<double, 3> origin{};
  // A chunk of something larger (terrain): only cells in [domainMin,
  // domainMax) get faces; cells outside are context - they hide the faces
  // they touch, as a neighbouring chunk's halo does. Edges on the domain's
  // boundary stay open, and simplification keeps them where they are, so two
  // chunks meshed from the same cells meet exactly at any LOD.
  bool hasDomain = false;
  std::array<int32_t, 3> domainMin{};
  std::array<int32_t, 3> domainMax{};
};

struct VoxelSurfaceTriangleMesh
{
  std::vector<Vertex> vertices; // Smooth area-weighted normals, zero UVs.
  std::vector<uint32_t> indices;
  uint64_t faceCount = 0u;
  // Vertices beyond one per lattice point: where the surface passed through a
  // lattice point more than once (edge- or corner-touching cells).
  uint32_t splitVertices = 0u;
  // Lattice edges passed twice by the surface (two diagonal cells joined round
  // both of its ends), each given one midpoint vertex per pass.
  uint32_t splitEdges = 0u;
  uint64_t borderEdges = 0u; // Open edges on the domain boundary.
  // Per vertex: 1 for a split vertex or edge midpoint, which sits a
  // thousandth of a voxel off the lattice beside another. The lossless merge
  // never moves these; its threshold would otherwise fold the sheets together.
  std::vector<uint8_t> pinned;
};

bool buildVoxelSurfaceTriangleMesh(
    const std::vector<VoxelSurfaceCell> &cells,
    const VoxelSurfaceTriangleMeshSettings &settings,
    VoxelSurfaceTriangleMesh &result,
    std::string *error = nullptr);

// LOD0 without the per-voxel quads: every collapse that costs no geometric
// error (coplanar faces, straight runs of edge), so the finest level a
// renderer draws is the voxel surface exactly, in a fraction of the
// triangles. Colour is the atlas's job, so nothing per voxel is lost.
// Vertices stay on the lattice (no vertex moves), and the mesh stays closed.
void mergeVoxelSurfaceCoplanar(VoxelSurfaceTriangleMesh &mesh, const VoxelSurfaceTriangleMeshSettings &settings);

// Build settings for a welded voxel surface: meshoptimizer clusters and
// groups, simplification by geometry alone.
VirtualGeometryBuildSettings voxelSurfaceBuildSettings();

// Colour by lattice instead of by atlas: every triangle belongs to the view
// its normal faces most, each vertex is copied once per view it touches, and
// a copy's UV is its position on that view's plane in voxels (u = axis + 1,
// v = axis + 2, mod 3) with that view's own area-weighted normal - so a
// renderer can find the voxel under any pixel from the UV and the depth along
// the normal. Returns the largest cluster vertex count; fails over
// MaxVertices.
// `storeUVs` false leaves UVs zero (they compress to nothing) for a renderer
// that derives the lattice cell from the position itself, as the city resolve
// does; the per-view copies and normals are made either way.
bool assignVoxelSurfaceViewUVs(VirtualGeometryBuildData &build, const VoxelSurfaceTriangleMeshSettings &settings,
                               uint32_t *maxClusterVertices = nullptr, std::string *error = nullptr, bool storeUVs = true);

// The triangles a DAG cut draws at world-space error `cutError`: every
// cluster with self.error <= cut < parent.error. Three corners per triangle,
// and their UVs when `uvs` is given.
std::vector<std::array<float, 3>> voxelSurfaceCutTriangles(const VirtualGeometryBuildData &build, float cutError,
                                                           std::vector<std::array<float, 2>> *uvs = nullptr);

// Topology of a triangle soup welded by exact position: the facts a closed
// seamless surface must satisfy.
struct VoxelSurfaceTopology
{
  uint64_t triangles = 0u;
  uint64_t vertices = 0u;      // Distinct positions.
  uint64_t edges = 0u;         // Distinct undirected edges.
  uint64_t borderEdges = 0u;   // Edges with one triangle.
  uint64_t nonManifoldEdges = 0u; // Edges with three or more.
  uint64_t misorientedEdges = 0u; // A directed edge used twice.
  uint64_t degenerateTriangles = 0u; // Two corners at one position.
  uint64_t zeroAreaTriangles = 0u;
  double area = 0.0;
  double signedVolume = 0.0;   // Divergence theorem; positive when outward.
  int64_t eulerCharacteristic() const { return int64_t(vertices) - int64_t(edges) + int64_t(triangles); }
  bool closedManifold() const { return borderEdges == 0u && nonManifoldEdges == 0u && misorientedEdges == 0u; }
};

VoxelSurfaceTopology measureVoxelSurfaceTopology(const std::vector<std::array<float, 3>> &triangleCorners);
VoxelSurfaceTopology measureVoxelSurfaceTopology(const VoxelSurfaceTriangleMesh &mesh);

// Debug preview: orthographic, z-buffered, each pixel coloured by the voxel
// found marching inward from the surface point along the triangle normal -
// what an ideal bake would show - and lit by that normal.
struct VoxelSurfacePreviewView
{
  uint32_t size = 512u;
  float yaw = 0.6f;
  float pitch = -0.45f;
  std::array<float, 3> boundsMin{};
  std::array<float, 3> boundsMax{};
};

struct VoxelSurfacePreviewImage
{
  uint32_t size = 0u;
  std::vector<uint32_t> rgba;
  uint64_t coveredPixels = 0u;
  uint64_t missedPixels = 0u; // Covered, but no voxel within reach.
};

VoxelSurfacePreviewImage renderVoxelSurfacePreview(
    const std::vector<std::array<float, 3>> &triangleCorners,
    const std::vector<VoxelSurfaceCell> &cells,
    const VoxelSurfaceTriangleMeshSettings &settings,
    const VoxelSurfacePreviewView &view,
    float reachMetres);

// The same view, each pixel sampled (nearest) from a texture at the
// interpolated UV: what the renderer draws from a baked atlas.
VoxelSurfacePreviewImage renderVoxelSurfaceTexturedPreview(
    const std::vector<std::array<float, 3>> &triangleCorners,
    const std::vector<std::array<float, 2>> &cornerUVs,
    const std::vector<uint32_t> &textureRGBA,
    uint32_t textureWidth,
    uint32_t textureHeight,
    const VoxelSurfacePreviewView &view);

// Panels left to right, one row, as a PNG.
bool writeVoxelSurfacePreviewSheet(const std::vector<VoxelSurfacePreviewImage> &panels, const std::string &path, std::string *error = nullptr);

// A .gltf + .bin pair (positions, normals) the virtual geometry renderer loads
// with `--asset`. `basePath` has no extension.
bool writeVoxelSurfaceTriangleMeshGLTF(
    const VoxelSurfaceTriangleMesh &mesh,
    const std::string &basePath,
    std::string *error = nullptr);

} // namespace virtualgeometry
