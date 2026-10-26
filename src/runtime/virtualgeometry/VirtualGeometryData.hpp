// VirtualGeometryData.hpp
#pragma once
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "rendering/animation/Skeleton.hpp"

namespace virtualgeometry
{
constexpr uint32_t ClusterSize = 128;
constexpr uint32_t GroupSize = 7;
constexpr float SimplifyThreshold = 0.85f;
constexpr uint32_t UseMetis = 1;
constexpr uint64_t MaxVertices = 192;
constexpr uint64_t MaxTriangles = ClusterSize;
constexpr int MetisSlop = 2;
constexpr uint32_t MaxBVHChildren = 8u;

// ---------------------------------------------------------------------------
// Visibility payload
//
// One encoding, written by both the hardware and the software rasterizer:
//
//   bits [6:0]    triangle index within its cluster
//   bits [25:7]   global decoded cluster index
//
// Both write the same 64-bit key - depth in the high 32 bits, this payload in
// the low 32 - so an atomicMin resolves nearest-wins in software, a hardware
// depth test resolves it in hardware, and a merge can compare the two without
// knowing which produced a given pixel. A resolve reaches the geometry directly
// through decodedClusterGeometry[globalCluster], which is why the global index
// is the right thing to carry rather than an index into a per-frame visible
// list.
//
// This replaces a software-only packing that truncated every field
// (cluster 12 bits, page 8, page-local cluster 6, triangle 6) against real
// ranges of 524288 clusters, 65536 pages and 128 triangles, and so could not
// round-trip at all. That truncation is why the software path could not be made
// authoritative.
//
// The instance index is deliberately not in the payload; six bits remain spare.
// A resolve recovers the instance from the cluster's owning object, which is
// exact while one instance draws each object - the terrain case. Geometry drawn
// by several instances at once needs those spare bits or a wider key.
// ---------------------------------------------------------------------------
constexpr uint32_t VisibilityPayloadTriangleBits = 7u;
constexpr uint32_t VisibilityPayloadTriangleMask =
    (1u << VisibilityPayloadTriangleBits) - 1u;
constexpr uint32_t VisibilityPayloadClusterBits = 19u;
constexpr uint32_t VisibilityPayloadClusterMask =
    (1u << VisibilityPayloadClusterBits) - 1u;
static_assert(
    ClusterSize <= VisibilityPayloadTriangleMask + 1u,
    "the visibility payload cannot address every triangle in a cluster");
static_assert(
    VisibilityPayloadTriangleBits + VisibilityPayloadClusterBits <= 32u,
    "the visibility payload does not fit the low half of the visibility key");

constexpr uint32_t packVisibilityPayload(
    const uint32_t globalClusterIndex,
    const uint32_t triangleIndex)
{
  return ((globalClusterIndex & VisibilityPayloadClusterMask)
          << VisibilityPayloadTriangleBits) |
      (triangleIndex & VisibilityPayloadTriangleMask);
}

constexpr uint32_t visibilityPayloadCluster(const uint32_t payload)
{
  return (payload >> VisibilityPayloadTriangleBits) &
      VisibilityPayloadClusterMask;
}

constexpr uint32_t visibilityPayloadTriangle(const uint32_t payload)
{
  return payload & VisibilityPayloadTriangleMask;
}
constexpr uint32_t MaxBVHLevels = 2;
constexpr uint32_t VMESH_MAGIC = 0x564D4743; // VMGC
constexpr uint32_t VMESH_VERSION = 5;
constexpr uint32_t VMESH_ENDIAN_LITTLE = 0x12345678;
constexpr uint32_t MAX_GROUPS_PER_PAGE = 64u;
constexpr uint32_t MAX_TEXTURES_PER_MATERIAL = 32u;

enum MeshletCompression : uint32_t
{
  MESHLET_RAW = 0,
  MESHLET_MINIZ = 1,
  MESHLET_LZ4 = 2
};

struct VirtualGeometryPageDescriptor
{
  uint64_t file_offset;
  uint32_t compressed_size;
  uint32_t uncompressed_size;
  uint32_t hierarchy_offset;
  uint32_t hierarchy_count;
  uint32_t meshlet_count;
  uint32_t max_hierarchy_depth;
};

struct VirtualGeometryMetadata
{
  uint32_t magic;
  uint32_t version;
  uint32_t endian_tag;

  uint32_t total_meshlet_count;
  uint32_t hierarchy_node_count;

  uint32_t page_count;
  uint32_t root_page_index;
  uint32_t shape_count;
  uint32_t material_count;
  uint32_t skeleton_count;
  uint32_t max_page_size;

  uint64_t hierarchy_offset;
  uint64_t hierarchy_size;

  uint64_t shape_table_offset;
  uint64_t shape_table_size;

  uint64_t material_table_offset;
  uint64_t material_table_size;

  uint64_t skeleton_table_offset;
  uint64_t skeleton_table_size;

  uint64_t page_table_offset;
  uint64_t page_table_size;

  uint64_t page_dependency_offset;
  uint64_t page_dependency_size;

  uint64_t page_install_update_offset;
  uint64_t page_install_update_size;

  uint64_t page_uninstall_update_offset;
  uint64_t page_uninstall_update_size;

  uint64_t page_data_offset;

  uint32_t quantization_factor;
  uint32_t unit_scale_bits;

  uint32_t flags;
};

struct VirtualGeometryShapeInfo
{
  uint32_t root_node_index = 0u;
  uint32_t root_page_index = 0u;
  uint32_t hierarchy_node_count = 0u;
  uint32_t materialIndex = 0u;
};

struct MeshPartInfo
{
  uint32_t dominantBoneIndex = UINT32_MAX;
};

struct PageDependencyEntry
{
  uint32_t page_id;
  uint32_t dependency_count;
};

// ─────────────────────────────────────────────────────────────────────────────
// Streaming update types
// ─────────────────────────────────────────────────────────────────────────────

// Hierarchy node flag bit layout:
//   bit 0: base "leaf node" flag (topology)
//   bit 1: legacy node-level streaming marker (kept for diagnostics)
//   bit 2: synthetic traversal-forcing internal node
//   bits [15:8]: per-cluster streaming-leaf mask (up to 8 clusters/group)
//   bits [23:16]: per-cluster enabled mask (up to 8 clusters/group)
static constexpr uint32_t HIERARCHY_LEAF_FLAG = (1u << 0);
static constexpr uint32_t STREAMING_LEAF_FLAG = (1u << 1); // legacy aggregate flag
static constexpr uint32_t HIERARCHY_FORCE_TRAVERSAL_FLAG = (1u << 2);
// The leaf's clusters carry their own streaming-leaf state in
// DecodedClusterCullData::flags (GPU-generated terrain pages: hundreds of
// clusters per page, children arriving per quadrant), not in the masks below.
static constexpr uint32_t HIERARCHY_DECODED_CLUSTER_FLAGS_FLAG = (1u << 3);
static constexpr uint32_t DECODED_CLUSTER_FLAG_STREAMING_LEAF = (1u << 0);
static constexpr uint32_t HIERARCHY_STREAMING_MASK_SHIFT = 8u;
static constexpr uint32_t HIERARCHY_ENABLED_MASK_SHIFT = 16u;
static constexpr uint32_t HIERARCHY_STREAMING_MASK_BITS = (0xFFu << HIERARCHY_STREAMING_MASK_SHIFT);
static constexpr uint32_t HIERARCHY_ENABLED_MASK_BITS = (0xFFu << HIERARCHY_ENABLED_MASK_SHIFT);

// Per-page rewrite for one hierarchy leaf node.
// Each bit corresponds to one child cluster in [0, child_count):
//   streamingLeafsBitset: clusters currently on the streaming boundary
//   enabledClustersBitset: clusters currently enabled in the installed DAG cut
struct HierarchyClusterFlagsUpdate
{
  uint32_t hierarchyNodeIndex = UINT32_MAX;
  uint8_t streamingLeafsBitset = 0u;
  uint8_t enabledClustersBitset = 0u;
  uint16_t _padding = 0u;
};

struct PageUpdateList
{
  std::vector<HierarchyClusterFlagsUpdate> hierarchyUpdates;
};

struct VirtualGeometryBuildPage
{
  struct GroupSpan
  {
    uint32_t globalGroupId = UINT32_MAX;
    uint32_t localGroupIndex = 0u;
    uint32_t clusterOffset = 0u;
    uint32_t clusterCount = 0u;
  };

  uint32_t clusterOffset;
  uint32_t clusterCount;
  uint32_t hierarchyOffset;
  uint32_t hierarchyCount;
  uint32_t maxHierarchyDepth;
  std::vector<GroupSpan> groups;
  std::vector<uint32_t> dependencies;
  PageUpdateList installUpdates;
  PageUpdateList uninstallUpdates;
};

struct QuantizationConfig
{
  uint8_t quantization_factor;
  float unit_scale;
  // Pad every meshlet's index buffer to Config::ClusterSize triangles by
  // repeating the last vertex index.  Enabled by default so that the
  // DRAW_INDIRECT_COUNT_DISABLED flat-vertex-stream path emits proper
  // zero-area triangles instead of relying on the VS out-of-range early-exit.
  bool padMeshlets;

  QuantizationConfig() : quantization_factor(4), unit_scale(100.0f), padMeshlets(true)
  {
  }
};

// How triangles are cut into clusters and clusters into groups, at LOD0 and
// after each simplification. METIS (the default, and what every existing asset
// was built with) weighs triangle adjacency by normal and UV similarity, but
// runs serialised on GKlib globals: one 318k-triangle voxel house spent 7.2 of
// its 9.4 s build there. Meshopt is meshoptimizer's lock-free spatial meshlet
// split and cluster partitioner, for meshes - voxel surfaces - whose clusters
// and groups only need to be compact.
enum class VirtualGeometryClusterizer : uint8_t
{
  Metis,
  Meshopt,
};

struct VirtualGeometryBuildSettings
{
  // Treat clusters with nothing finer in this DAG as streaming leaves. For a
  // lone asset the finest level is a true leaf (error zero, always drawn). A
  // terrain chunk sharing one runtime object gives its finest clusters their
  // voxel size as error so a resident finer chunk can replace them; the
  // streaming mark is then what keeps the frontier drawing until it does, and
  // residency clears it per quadrant as children arrive.
  bool finestClustersAreStreamingLeaves = false;
  uint32_t maxGroupsPerPage = MAX_GROUPS_PER_PAGE;
  // Byte budget for one page, on top of the group and cluster counts. Zero
  // (the default) packs by count alone, which is what makes page size a
  // function of how much geometry the groups happen to hold: measured on
  // terrain, 979 pages over 159.1 MiB of page buffer, ~166 KiB each with the
  // spread that implies. A budget keeps them even, which is what the streaming
  // budgets, the install cost and the disk reads are all sized against.
  //
  // The estimate the packer measures against is not the encoder's output - it
  // runs before encoding - so this is a target, not a guarantee. A group that
  // exceeds the budget on its own is still placed, because a page must hold at
  // least one.
  uint64_t maxPageBytes = 0u;
  // Let a group's clusters straddle a page boundary rather than pushing the
  // whole group to the next page.
  //
  // A page is packed until the next group does not fit, and the tail it leaves
  // is up to one group - eight clusters, about 800 bytes each, so ~4% of a
  // 128 KiB page and more when the clusters are large. Splitting fills the
  // page exactly: clusters 0..k-1 land on page A and the rest at the start of
  // page B, which is A's successor, so the group is complete exactly when B is
  // resident and B already depends on A.
  //
  // A cluster is never split - its encoded offsets are page-local word offsets
  // into that page's blocks. What changes is the hierarchy: a group spanning
  // N pages becomes N leaf nodes, one per page, under a synthetic internal
  // node in the group's original slot. No page format or shader change.
  //
  // Off by default: it only pays where pages are packed to a byte budget
  // (`maxPageBytes`) or clusters are large, and leaving it off keeps every
  // existing asset byte-identical.
  bool splitGroupsAcrossPages = false;
  uint32_t maxRootPageGroups = 0u; // 0 => use maxGroupsPerPage for the root page too
  bool padPagesToMaxSize = false;
  // Phase 9c: threads for the front half's fan-outs (over mesh parts, and
  // over the groups of a level when parts are few). 0 chooses from the job
  // count and the machine, 1 is serial; the encoded asset is byte-identical
  // either way - `VirtualGeometryPackerDeterminism` pins that.
  uint32_t frontThreads = 0u;
  VirtualGeometryClusterizer clusterizer = VirtualGeometryClusterizer::Metis;
  // Simplify by geometry alone, for a surface whose attributes are baked after
  // the DAG is built (a welded voxel surface): normals and UVs neither steer
  // nor add to the error, only a group's border locks a vertex, vertices are
  // welded by position inside each group, closed pieces with no border vertex
  // may be pruned whole, and the sloppy fallback is never used. Off keeps
  // every existing asset byte-identical.
  bool simplifyPositionsOnly = false;
  // Vertices a Meshopt cluster is cut to, under MaxVertices, which stays the
  // hard limit. Zero is MaxVertices. A voxel surface's atlas copies every
  // vertex on a chart border, so its clusters leave room for that.
  uint32_t clusterMaxVertices = 0u;
  // Error of the finest clusters, which every coarser level then exceeds. A
  // terrain chunk sharing one runtime object gives its finest clusters its
  // voxel size (with finestClustersAreStreamingLeaves), so a resident finer
  // chunk replaces them. Zero is the lone-asset convention.
  float finestClusterError = 0.0f;
  // Grid (asset units) that positions-only simplification snaps the vertices
  // it moves to, before it measures the level's error. Set it to the encoder's
  // quantisation step (QuantizationConfig) so the stored error includes the
  // rounding: a 10 cm voxel lattice is exact on a 12.5 mm grid, where the
  // default 0.625 mm step spent seven bits per coordinate on nothing. Zero
  // leaves positions where meshoptimizer put them.
  float positionGrid = 0.0f;
};

// ─────────────────────────────────────────────────────────────────────────────
// Bone weight (one influence)
// ─────────────────────────────────────────────────────────────────────────────
struct BoneWeight
{
  float weight;
  uint32_t boneIndex;
};

// ─────────────────────────────────────────────────────────────────────────────
// Per-page vertex buffers
// ─────────────────────────────────────────────────────────────────────────────
struct VertexBuffers
{
  std::vector<uint32_t> positions; // bit-packed quantised positions
  std::vector<uint32_t> normals;   // oct-encoded, pack2x16snorm
  std::vector<float> uvs;          // 2 floats per vertex, uncompressed
  std::vector<uint8_t> indices;    // triangle indices, uint8 per entry
  // Bone weights stored as interleaved (weight-as-float-bits, boneIndex) pairs,
  // packed into uint32 words.  Layout per vertex:
  //   [weight_u32_0, boneIndex_0, weight_u32_1, boneIndex_1, ...]
  // The number of influences per vertex is uniform within each meshlet and
  // stored in Meshlet::boneWeightsPerVertex.
  std::vector<uint32_t> boneWeights;
};

// ─────────────────────────────────────────────────────────────────────────────
// LOD bounds (sphere + error)
// ─────────────────────────────────────────────────────────────────────────────
struct LODBounds
{
  float center[3];
  float radius;
  float error;
};

struct ClusterCone
{
  float axis[3];
  float cutoff;
};

// ─────────────────────────────────────────────────────────────────────────────
// Meshlet descriptor (per-cluster metadata stored inside each page)
//
// New fields vs. original:
//   self / parent  — LOD bounds copied from the source ClusterBuildData
//   boneWeightOffset      — word offset into VertexBuffers::boneWeights
//   boneWeightsPerVertex  — number of (weight,boneIndex) pairs per vertex
// ─────────────────────────────────────────────────────────────────────────────
struct Meshlet
{
  // ── position bitstream ───────────────────────────────────────────────────
  uint32_t start_vertex_position_bit;
  uint32_t start_vertex_attribute_id; // normal / uv base index
  uint32_t start_index_id;

  uint8_t vertex_count;
  uint8_t triangle_count;
  uint32_t quantized_position_span_x;
  uint32_t quantized_position_span_y;
  uint32_t quantized_position_span_z;
  uint8_t vertex_position_quantization_factor;

  int32_t min_vertex_position_channel_x;
  int32_t min_vertex_position_channel_y;
  int32_t min_vertex_position_channel_z;

  // ── LOD bounds (from original cluster) ──────────────────────────────────
  LODBounds self;   // bounds of this cluster at its own LOD level
  LODBounds parent; // bounds of the simplified parent group
  ClusterCone cone; // normal cone used for cluster backface culling

  // ── bone weights ─────────────────────────────────────────────────────────
  uint32_t boneWeightOffset;    // word offset into VertexBuffers::boneWeights
  uint8_t boneWeightsPerVertex; // influences per vertex (uniform per meshlet)
  uint8_t pageLocalGroupIndex;  // local group index inside the owning page
  uint8_t clusterIndexInGroup;  // child index inside the local group
  uint8_t boneWeightPadding[1]; // reserved

  Meshlet()
      : start_vertex_position_bit(0), start_vertex_attribute_id(0), start_index_id(0), vertex_count(0), triangle_count(0), quantized_position_span_x(0), quantized_position_span_y(0),
        quantized_position_span_z(0), vertex_position_quantization_factor(0), min_vertex_position_channel_x(0), min_vertex_position_channel_y(0), min_vertex_position_channel_z(0), self{}, parent{}, cone{},
        boneWeightOffset(0), boneWeightsPerVertex(0), pageLocalGroupIndex(0), clusterIndexInGroup(0), boneWeightPadding{}
  {
  }
};

struct VirtualGeometryHierarchy
{
  float max_x, max_y, max_z;
  float min_x, min_y, min_z;
  float max_center_x, max_center_y, max_center_z;
  float max_radius;
  float min_lod_error;
  float max_parent_lod_error;
  uint32_t child_start;
  uint32_t child_count;
  uint32_t pageIndex;
  uint32_t meshPartIndex;
  uint32_t flags;
};

struct VirtualGeometryPage
{
  std::vector<Meshlet> meshlets;
  uint32_t groupCount = 0u;
  std::vector<uint32_t> groupMeshletOffsets;
  VertexBuffers vertexBuffers;
  PageUpdateList installUpdates;
  PageUpdateList uninstallUpdates;
  std::vector<uint32_t> dependencies;
};

// Wall-clock milliseconds per stage of one asset build (phase 9b of
// docs/terrain-vg-unified-hierarchy-plan.md). The front half is route-specific:
// the triangle route fills partition / simplify / group, the voxel route fills
// `partition` with its meshing and grouping and leaves the other two zero. The
// back half - hierarchy, packPages (pages + rewrites), encode, compress, write -
// is shared by both routes and is what phase 9 parallelises.
struct VirtualGeometryBuildTimings
{
  // Triangle route: clusterising the source triangles into LOD0 clusters.
  double lod0Clusters = 0.0;
  double partition = 0.0;
  // `simplify` and `group` are summed over groups, so with `frontThreads`
  // above one they are CPU time and may exceed the wall time of the level.
  double simplify = 0.0;
  double group = 0.0;
  double hierarchy = 0.0;
  // Multi-shape assets only: merging the per-part and per-shape DAGs into one.
  double merge = 0.0;
  double packPages = 0.0;
  double encode = 0.0;
  double compress = 0.0;
  double write = 0.0;
  uint32_t frontThreads = 0u;
  uint32_t encodeThreads = 0u;
  uint32_t compressThreads = 0u;
};

struct VirtualGeometryEncodedData
{
  std::vector<VirtualGeometryHierarchy> hierarchy;
  std::vector<VirtualGeometryShapeInfo> shapes;
  std::vector<std::string> materialFiles;
  std::vector<MeshPartInfo> meshParts;
  QuantizationConfig quantizationConfig;
  VirtualGeometryBuildSettings buildSettings;
  std::vector<VirtualGeometryPage> pages;
  rendering::animation::Skeleton skeleton;
  uint32_t root_node_index;
  uint32_t rootPageIndex;
  VirtualGeometryBuildTimings timings;

  VirtualGeometryEncodedData() : root_node_index(0), rootPageIndex(0)
  {
  }
};

struct ClusterGroupInfo
{
  std::vector<int32_t> simplifiedClusterIndices;
  std::vector<int32_t> originalClusterIndices;
  uint32_t lodLevel;
};

struct Vertex
{
  float pos[3];
  float norm[3];
  float uv[2];
  // Bone weights for this vertex.  May be empty if no skinning data.
  std::vector<BoneWeight> boneWeights;

  Vertex()
  {
    pos[0] = pos[1] = pos[2] = 0.f;
    norm[0] = norm[1] = norm[2] = 0.f;
    uv[0] = uv[1] = 0.f;
  }

  Vertex(float x, float y, float z)
  {
    pos[0] = x;
    pos[1] = y;
    pos[2] = z;
    norm[0] = norm[1] = norm[2] = 0.f;
    uv[0] = uv[1] = 0.f;
  }

  bool operator==(const Vertex &o) const
  {
    if (pos[0] != o.pos[0] || pos[1] != o.pos[1] || pos[2] != o.pos[2])
      return false;
    if (norm[0] != o.norm[0] || norm[1] != o.norm[1] || norm[2] != o.norm[2])
      return false;
    if (uv[0] != o.uv[0] || uv[1] != o.uv[1])
      return false;
    if (boneWeights.size() != o.boneWeights.size())
      return false;
    for (size_t i = 0; i < boneWeights.size(); ++i)
      if (boneWeights[i].weight != o.boneWeights[i].weight || boneWeights[i].boneIndex != o.boneWeights[i].boneIndex)
        return false;
    return true;
  }
};

struct VirtualGeometryCluster
{
  std::vector<Vertex> vertices;
  std::vector<uint32_t> indices;
  uint32_t groupId = UINT32_MAX;
  uint32_t meshPartIndex = UINT32_MAX;
  LODBounds self;
  LODBounds parent;
  ClusterCone cone{};
};

struct VirtualGeometryBuildData
{
  std::vector<VirtualGeometryCluster> clusters;
  std::vector<VirtualGeometryHierarchy> lodLevelHierarchy;
  std::vector<VirtualGeometryShapeInfo> shapes;
  std::vector<std::string> materialFiles;
  std::vector<MeshPartInfo> meshParts;
  VirtualGeometryBuildSettings buildSettings;
  std::vector<VirtualGeometryBuildPage> pages;
  std::vector<ClusterGroupInfo> groupInfos;
  std::vector<std::vector<uint32_t>> groupDAG;
  std::vector<std::vector<uint32_t>> clusterDAG;
  std::vector<uint32_t> buildToLinearizedClusterIndex;
  rendering::animation::Skeleton skeleton;
  VirtualGeometryBuildTimings timings;
};

} // namespace virtualgeometry
