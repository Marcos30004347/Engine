// VirtualGeometryFile.hpp
#pragma once
#include "VirtualGeometryData.hpp"
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace virtualgeometry
{

// ============================================================================
// PageBuffer
//
// Serialises / deserialises a single VirtualGeometryPage to/from a flat
// array of uint32 words.
//
// Meshlet descriptor table layout (words per entry):
//
//  [0]  position_offset        (word offset into position block)
//  [1]  position_word_count
//  [2]  normal_offset          (entry index into normal block)
//  [3]  vertex_count           (also normal count)
//  [4]  uv_offset              (float-pair index = normal_offset * 2)
//  [5]  uv_count               (vertex_count * 2)
//  [6]  index_offset           (word offset into index block)
//  [7]  index_word_count
//  [8]  vertex_count           (copy)
//  [9]  triangle_count
//  [10] quantized_position_span_x
//  [11] quantized_position_span_y
//  [12] quantized_position_span_z
//  [13] quantization_factor
//  [14] min_position_x         (reinterpreted float)
//  [15] min_position_y
//  [16] min_position_z
//  --- LOD bounds (self) ---
//  [17] self.center[0]
//  [18] self.center[1]
//  [19] self.center[2]
//  [20] self.radius
//  [21] self.error
//  --- LOD bounds (parent) ---
//  [22] parent.center[0]
//  [23] parent.center[1]
//  [24] parent.center[2]
//  [25] parent.radius
//  [26] parent.error
//  --- Cluster cone ---
//  [27] cone.axis[0]
//  [28] cone.axis[1]
//  [29] cone.axis[2]
//  [30] cone.cutoff
//  --- Bone weights ---
//  [31] bone_weight_offset      (word offset into bone_weight block)
//  [32] bone_weights_per_vertex
//  [33] packed_group_cluster    (bits [5:0] = local group, [8:6] = cluster-in-group)
//
//  Total: MESHLET_DESC_WORDS = 34
// ============================================================================

static constexpr uint32_t PAGE_HEADER_WORDS = 8u + MAX_GROUPS_PER_PAGE;
static constexpr uint32_t MESHLET_DESC_WORDS = 34u;
static constexpr uint32_t HIERARCHY_WORDS = 17u;

class PageBuffer
{
public:
  std::vector<uint32_t> data;

  static PageBuffer encode(const VirtualGeometryPage &page);
  static void decode(const PageBuffer &buffer, VirtualGeometryPage &page, const QuantizationConfig &config);

  struct PageHeader
  {
    uint32_t num_meshlets;
    uint32_t position_data_size;
    uint32_t normal_data_size;
    uint32_t uv_data_size;
    uint32_t index_data_size;
    uint32_t bone_weight_data_size;
    uint32_t dependency_count;
    uint32_t group_count;
  };

  static PageHeader readHeader(const PageBuffer &buffer);

private:
  static inline uint32_t floatToUint32(float f)
  {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  }
  static inline float uint32ToFloat(uint32_t u)
  {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
  }
};

// ============================================================================
// VirtualGeometryStreamedPage
//
// Non-owning view over a raw decompressed page buffer.  Decodes data
// directly from the buffer without secondary copies.
// ============================================================================
class VirtualGeometryStreamedPage
{
public:
  VirtualGeometryStreamedPage();
  VirtualGeometryStreamedPage(const void *data, uint32_t size_in_bytes, uint32_t page_size);

  VirtualGeometryStreamedPage(const VirtualGeometryStreamedPage &) = delete;
  VirtualGeometryStreamedPage &operator=(const VirtualGeometryStreamedPage &) = delete;
  VirtualGeometryStreamedPage(VirtualGeometryStreamedPage &&) noexcept = default;
  VirtualGeometryStreamedPage &operator=(VirtualGeometryStreamedPage &&) noexcept = default;

  bool isValid() const
  {
    return data_ != nullptr;
  }
  uint32_t getPageSize() const
  {
    return page_size_;
  }
  const void *getData() const
  {
    return data_;
  }
  uint32_t getDataSizeInBytes() const
  {
    return data_size_bytes_;
  }
  uint32_t getMeshletCount() const;
  uint32_t getGroupCount() const;
  uint32_t getGroupMeshletOffset(uint32_t group_index) const;

  uint32_t getClustersStartOffset() const;
  uint32_t getClustersDataSizeInBytes() const;

  struct MeshletDescriptor
  {
    // Geometry
    uint32_t position_offset, position_count;
    uint32_t normal_offset, normal_count;
    uint32_t uv_offset, uv_count;
    uint32_t index_offset, index_count;
    uint32_t vertex_count, triangle_count;
    uint32_t quantized_position_span_x, quantized_position_span_y, quantized_position_span_z;
    uint8_t quantization_factor;
    float min_position_x, min_position_y, min_position_z;
    // LOD bounds
    LODBounds self;
    LODBounds parent;
    ClusterCone cone;
    // Bone weights
    uint32_t bone_weight_offset;
    uint8_t bone_weights_per_vertex;
    uint8_t page_local_group_index;
    uint8_t cluster_index_in_group;
  };

  MeshletDescriptor getMeshletDescriptor(uint32_t meshlet_index) const;

  void decodePositions(uint32_t meshlet_index, std::vector<float> &positions, const QuantizationConfig &config) const;
  void decodeNormals(uint32_t meshlet_index, std::vector<float> &normals) const;
  void decodeUVs(uint32_t meshlet_index, std::vector<float> &uvs) const;
  void decodeIndices(uint32_t meshlet_index, std::vector<uint8_t> &indices) const;
  void decodeBoneWeights(uint32_t meshlet_index, std::vector<BoneWeight> &bone_weights) const;

private:
  const uint32_t *data_ = nullptr;
  uint32_t data_size_bytes_ = 0;
  uint32_t page_size_ = 0;

  uint32_t meshlet_count_ = 0;
  uint32_t group_count_ = 0;
  uint32_t position_data_offset_ = 0; // word offsets from data_
  uint32_t normal_data_offset_ = 0;
  uint32_t uv_data_offset_ = 0;
  uint32_t index_data_offset_ = 0;
  uint32_t bone_weight_data_offset_ = 0;
  uint32_t dependencies_offset_ = 0;
  uint32_t dependency_count_ = 0;
  uint32_t group_meshlet_offsets_[MAX_GROUPS_PER_PAGE] = {};

  void computeOffsets();
};

// ============================================================================
// VirtualGeometryFile
// ============================================================================
class VirtualGeometryFile
{
public:
  struct InstanceHierarchyData
  {
    uint32_t hierarchyNodeOffsetInFile = 0u;
    std::vector<VirtualGeometryHierarchy> hierarchy;
  };

  VirtualGeometryFile();
  VirtualGeometryFile(const std::string &path, bool write_mode = false);
  ~VirtualGeometryFile();

  VirtualGeometryFile(const VirtualGeometryFile &) = delete;
  VirtualGeometryFile &operator=(const VirtualGeometryFile &) = delete;
  VirtualGeometryFile(VirtualGeometryFile &&other) noexcept;
  VirtualGeometryFile &operator=(VirtualGeometryFile &&other) noexcept;

  bool isOpen() const;
  const std::string &getPath() const;

  bool write(const VirtualGeometryEncodedData &data, const std::vector<VirtualGeometryBuildPage> &build_pages, MeshletCompression compression = MESHLET_LZ4);

  const VirtualGeometryMetadata &getMetadata() const;
  const std::vector<VirtualGeometryHierarchy> &getHierarchy() const;
  const std::vector<VirtualGeometryShapeInfo> &getShapes() const;
  const std::vector<std::string> &getMaterialFiles() const;
  const std::vector<MeshPartInfo> &getMeshParts() const;
  const rendering::animation::Skeleton &getSkeleton() const;
  const std::vector<VirtualGeometryPageDescriptor> &getPageTable() const;
  const std::vector<std::vector<uint32_t>> &getPageDependencies() const;
  const std::vector<PageUpdateList> &getPageInstallUpdates() const;
  const std::vector<PageUpdateList> &getPageUninstallUpdates() const;

  uint32_t getMaxPageSize() const;
  size_t getInstanceCount() const;
  InstanceHierarchyData getHierarchyForInstance(uint32_t instance_index) const;
  uint32_t getRootPageForInstance(uint32_t instance_index) const;

  bool streamPage(uint32_t page_index, VirtualGeometryPage &page) const;
  bool streamPageRaw(uint32_t page_index, void *buffer, uint32_t buffer_size_bytes, VirtualGeometryStreamedPage &out_page) const;

  bool readAll(VirtualGeometryEncodedData &data) const;

private:
  void close();
  bool loadMetadataAndTables();

  bool writeMetadata(const VirtualGeometryMetadata &metadata);
  bool writeShapes(const std::vector<VirtualGeometryShapeInfo> &shapes);
  bool writeMaterialTable(const std::vector<std::string> &materialFiles);
  bool writeSkeletonTable(const rendering::animation::Skeleton &skeleton, const std::vector<MeshPartInfo> &meshParts);
  bool writePageDependencies(const std::vector<std::vector<uint32_t>> &dependencies);
  bool writePageUpdateLists(const std::vector<PageUpdateList> &lists);
  static void serializePageUpdateList(FILE *f, const PageUpdateList &list);
  static bool deserializePageUpdateList(FILE *f, PageUpdateList &list);

  static void writeU32(FILE *f, uint32_t v);
  static void writeU64(FILE *f, uint64_t v);
  static void writeF32(FILE *f, float v);
  static bool readU32(FILE *f, uint32_t &v);
  static bool readU64(FILE *f, uint64_t &v);
  static bool readF32(FILE *f, float &v);

  std::string path_;
  FILE *file_ = nullptr;
  bool write_mode_ = false;

  VirtualGeometryMetadata metadata_;
  std::vector<VirtualGeometryHierarchy> hierarchy_;
  std::vector<VirtualGeometryShapeInfo> shapes_;
  std::vector<std::string> material_files_;
  std::vector<MeshPartInfo> mesh_parts_;
  rendering::animation::Skeleton skeleton_;
  std::vector<VirtualGeometryPageDescriptor> page_table_;
  std::vector<std::vector<uint32_t>> page_dependencies_;
  std::vector<PageUpdateList> page_install_updates_;
  std::vector<PageUpdateList> page_uninstall_updates_;
};

} // namespace virtualgeometry
