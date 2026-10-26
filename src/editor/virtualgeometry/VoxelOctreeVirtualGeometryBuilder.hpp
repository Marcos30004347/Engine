#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"
#include "voxel/svraster/SparseVoxelBrick.hpp"
#include "voxel/svraster/SparseVoxelBrickSurfaceMesh.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace voxel::svraster
{

// Virtual geometry LOD chain built from the voxel brick octree instead of from
// triangle simplification.
//
// The normal virtual geometry builder partitions LOD0 clusters, merges them into
// groups, and runs meshoptimizer over each group to produce the coarser
// clusters. That does not work for voxel surfaces: exposed-face meshing emits a
// quad soup whose vertices are not shared across faces, so edge collapse has no
// connected edges to collapse and the LOD chain collapses to a single level.
// Measured on the Cliff fixture the existing bridge produced 164 clusters and
// 164 "simplified" meshlets - every cluster stayed at LOD0 and every parent
// error stayed at infinity, which is exactly a renderer that always draws the
// finest geometry.
//
// The brick octree already carries everything the DAG needs:
//   - a node is 8^3 cells and its eight children tile exactly the same volume;
//   - a parent cell is the OR reduction of the eight child cells under it;
//   - selfError/parentError are *measured* deviations and are monotonic.
//
// So the coarse representation of a group is not a simplification at all - it is
// the parent brick, meshed the same way. This builder walks the octree, meshes
// every level, and emits the group/DAG structure directly.
//
// Blocks
// ------
// A virtual geometry cluster holds at most ClusterSize triangles and MaxVertices
// vertices, and a group holds at most MaxBVHChildren clusters. A brick's surface
// does not always fit one cluster, so a node is split into octree-aligned
// *blocks* of `8 >> blockDepth` cells per side. One block is one cluster.
//
// The block depth is global, which is what keeps the DAG exact: a parent block
// covers precisely the volume of eight child blocks, so
//   group(parent block) = { the eight child blocks under it }
// and the coarse replacement of that group is the parent block itself. Volume
// correspondence is exact, so a DAG cut can never open a hole. `blockDepth` is
// raised until every block fits inside the cluster budget; depth 3 is a single
// cell, which always fits, so the search always terminates.
struct SparseVoxelOctreeVirtualGeometrySettings
{
  // Merge coplanar exposed faces inside a block using occupancy alone.
  // Appearance is resolved per source cell after rasterization, so material
  // changes must never split a geometry asset's quads.
  bool mergeCoplanarFaces = true;
  // City assets share topology across exact appearance variants. Legacy
  // terrain keeps its existing material-aware quad partition until its
  // per-cell material resolve migrates to the same source-address contract.
  bool geometryOnly = false;
  // Optional per-node exposed-face suppression, indexed by hierarchy node.
  // Used by the offline city compiler after connector/party ownership has
  // been resolved. A set bit removes that cell face at every selected LOD.
  const std::vector<SparseVoxelBrickFaceMasks> *suppressedFaces = nullptr;
  // Share quad corners inside a cluster. Face-local corners cost four vertices
  // per quad, which busts MaxVertices long before the triangle budget.
  bool weldClusterVertices = true;
  uint32_t maxTrianglesPerCluster = virtualgeometry::ClusterSize;
  uint32_t maxVerticesPerCluster = static_cast<uint32_t>(virtualgeometry::MaxVertices);
  uint32_t maxClustersPerGroup = virtualgeometry::MaxBVHChildren;
  // Positions are emitted relative to this origin. Terrain pages pass their page
  // minimum so a planet-scale absolute coordinate never reaches a float.
  std::array<double, 3> originOffset{};
  // xyz lattice origin and w finest spacing. When supplied, evaluate shared
  // planes from integer coordinates once, rather than adding rounded brick
  // origins to rounded cell offsets independently on each side of a seam.
  std::array<double, 4> canonicalWorldGrid{};
  // Emit positions in the renderer's world space instead of relative to the
  // page origin. One runtime object holds one instance transform, so chunks
  // sharing an object cannot each be placed by their own; their geometry has to
  // arrive already in the object's space.
  //
  // The page-relative default exists so a planet-scale coordinate never reaches
  // a float. That limit is not reached here: a 128 km world map at 64 km from
  // the origin resolves about 4 mm in float32, forty times finer than the 10 cm
  // voxel. A genuinely planet-scale world would need per-chunk placement back,
  // and one object per chunk with it.
  bool absoluteOrigin = false;
  // Self error given to the finest clusters. Zero (the default) is the LOD0
  // convention: a cluster with nothing finer under it must never be rejected,
  // or the camera opens a hole approaching it. Chunks sharing one runtime
  // object get their "always draw" from the streaming-leaf flag residency sets
  // on the frontier instead, and pass their voxel size here so the culler can
  // reject a coarse chunk's finest clusters where a resident finer chunk draws.
  float leafSelfError = 0.0f;
  // Error of the coarser chunk that covers this chunk's whole volume - the
  // parent chunk's finest cluster self error. Phase 10c: the four child chunks
  // tile their parent's volume exactly, so clamping every cluster's *parent*
  // error to it makes the two chunks' draw ranges disjoint by construction.
  //
  // A cluster draws iff `self.error <= budget < parent.error`. A chunk carries
  // its own LOD chain (cell sizes v, 2v, 4v, 8v) and its parent chunk carries
  // the same ground one level coarser (2v, 4v, 8v, 16v), so every level of
  // this chunk at or above the parent's finest is a duplicate of a level the
  // parent chunk already draws. Clamping collapses those duplicates to an
  // empty range and leaves each chunk drawing exactly the band below its
  // parent chunk: no overlap, and no gap, with no coverage test at all.
  //
  // Infinite (the default) means "nothing coarser covers this": the chunk is
  // the coarsest one over its ground, keeps its whole chain, and its roots
  // keep the usual infinite parent error.
  float parentChunkError = std::numeric_limits<float>::max();
  // Whether the clamp reaches the chunk's *root* clusters as well as the
  // duplicate levels under them.
  //
  // **On**, and this is a correction. The triangle builder ends every
  // leaf-to-root chain in `[selfRoot, infinity)`, so leaving the roots
  // unclamped is the faithful port, and on a first comparison it looked better
  // on both axes: 65,462 -> 54,564 drawn clusters at an unchanged 0.18 gap
  // pixels. That comparison was incomplete. Measured again with the LOD
  // profile included, on the same 5 km walk:
  //
  //   roots clamped:  lod0 chunks 40, worstPx 2.89, worstLevels 0.95,
  //                   rootRepairs 20, drawn clusters 66,661
  //   roots infinite: lod0 chunks 20, worstPx 3.32, worstLevels 1.15,
  //                   rootRepairs 276, drawn clusters 54,557
  //
  // Half the finest chunks and a measurably coarser profile. The saving is not
  // overlap being removed - it is refinement demand being suppressed, because
  // an unclamped root cluster draws at every coarse budget and the ground it
  // covers stops asking to refine. That is the same mechanism as the demand
  // queue being eaten by ancestors, and paying for fewer clusters with less
  // detail is what raising the LOD threshold does for free.
  bool parentChunkErrorAppliesToRoots = true;
  // Match the normal VG builder: children with no coarse replacement remain
  // roots. Giving them a finite parent error would leave a hole at coarse LOD.
  bool consumeVanishingGroups = false;
  // Suppress the virtual geometry builder's per-cluster stdout logging. Runtime
  // page builds go through this path once per streamed page, where the logging
  // alone dominates the build.
  bool quiet = true;
};

// Where one emitted cluster came from. Indexed by the *linearized* cluster index
// used by VirtualGeometryBuildData::clusters, so a consumer can tie a meshlet
// back to the brick node and the octree-aligned block it meshes.
//
// The block box is the cluster's cut: every vertex of the cluster lies on the
// closed box, because a quad is a face of a cell inside that block.
struct SparseVoxelOctreeVirtualGeometryClusterSource
{
  uint32_t brickNodeIndex = 0xffffffffu;
  uint32_t blockIndex = 0u;
  uint32_t level = 0u;
  uint32_t triangleCount = 0u;
  std::array<float, 3> blockMinimum{};
  float blockExtent = 0.0f;
  // Tight axis-aligned bounds of the cluster's own emitted geometry. The block
  // box above is the octree cell the cluster was cut from - eight cells on a
  // side, most of it air or underground - so anything asking "what does this
  // cluster actually cover" has to use these instead. Comparing the *cube*
  // against a neighbouring chunk's geometry bounds is a category error: it
  // fails whenever the cube reaches into empty sky, which is nearly always.
  std::array<float, 3> surfaceMinimum{};
  std::array<float, 3> surfaceMaximum{};
  bool surfaceValid = false;
};

struct SparseVoxelOctreeVirtualGeometryStats
{
  uint32_t levelCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t blockDepth = 0u;
  uint32_t clusterCount = 0u;
  uint32_t groupCount = 0u;
  uint32_t triangleCount = 0u;
  uint32_t vertexCount = 0u;
  uint32_t quadCount = 0u;
  uint32_t exposedFaceCount = 0u;
  std::vector<uint64_t> exposedFacesPerLevel;
  uint32_t rootClusterCount = 0u;
  // Groups whose parent block carries no surface at all: the children's geometry
  // is interior once coarsened, so the cut legitimately drops it.
  uint32_t vanishingGroupCount = 0u;
  std::vector<SparseVoxelOctreeVirtualGeometryClusterSource> clusterSources;
  std::vector<uint32_t> clustersPerLevel;
  std::vector<uint32_t> trianglesPerLevel;
  double meshingMilliseconds = 0.0;
  double hierarchyMilliseconds = 0.0;
};

// Build a virtual geometry LOD chain for the brick nodes in
// [firstNode, firstNode + nodeCount) of `hierarchy`.
//
// Nodes outside that range are ignored, including parents: a page builds its own
// self-contained DAG whose top level becomes the page roots. Child links that
// leave the range are dropped the same way, so the caller can hand in one
// streamed page without copying the hierarchy.
bool buildSparseVoxelOctreeVirtualGeometry(
    const SparseVoxelBrickHierarchy &hierarchy,
    uint32_t firstNode,
    uint32_t nodeCount,
    const SparseVoxelOctreeVirtualGeometrySettings &settings,
    const virtualgeometry::VirtualGeometryBuildSettings &buildSettings,
    virtualgeometry::VirtualGeometryBuildData &result,
    SparseVoxelOctreeVirtualGeometryStats *stats = nullptr,
    std::string *error = nullptr);

} // namespace voxel::svraster
