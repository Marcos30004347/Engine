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
constexpr uint32_t MaxBVHLevels = 2;
constexpr uint32_t VMESH_MAGIC = 0x564D4743; // VMGC
constexpr uint32_t VMESH_VERSION = 5;
constexpr uint32_t VMESH_ENDIAN_LITTLE = 0x12345678;
constexpr uint32_t MAX_GROUPS_PER_PAGE = 64u;
constexpr uint32_t MAX_TEXTURES_PER_MATERIAL = 5u;

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

struct VirtualGeometryBuildSettings
{
  uint32_t maxGroupsPerPage = MAX_GROUPS_PER_PAGE;
  uint32_t maxRootPageGroups = 0u; // 0 => use maxGroupsPerPage for the root page too
  bool padPagesToMaxSize = false;
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
};

} // namespace virtualgeometry
