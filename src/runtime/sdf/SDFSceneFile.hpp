#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace sdf
{

constexpr uint32_t VSDF_MAGIC = 0x46445356u; // "VSDF"
constexpr uint32_t VSDF_VERSION = 9u;
constexpr uint32_t VSDF_BRICK_RESOLUTION = 8u;
constexpr uint32_t VSDF_BRICK_CELL_COUNT = VSDF_BRICK_RESOLUTION * VSDF_BRICK_RESOLUTION * VSDF_BRICK_RESOLUTION;
constexpr uint32_t VSDF_BRICK_DISTANCE_RESOLUTION = VSDF_BRICK_RESOLUTION + 1u;
constexpr uint32_t VSDF_BRICK_DISTANCE_COUNT = VSDF_BRICK_DISTANCE_RESOLUTION * VSDF_BRICK_DISTANCE_RESOLUTION * VSDF_BRICK_DISTANCE_RESOLUTION;
constexpr uint32_t VSDF_BRICK_DISTANCE_STRIDE = (VSDF_BRICK_DISTANCE_COUNT + 3u) & ~3u;
constexpr uint32_t VSDF_TRACE_GRID_RESOLUTION = 64u;
constexpr uint32_t VSDF_TRACE_GRID_CELL_COUNT = VSDF_TRACE_GRID_RESOLUTION * VSDF_TRACE_GRID_RESOLUTION * VSDF_TRACE_GRID_RESOLUTION;
constexpr uint32_t VSDF_TRACE_ROOT_RESOLUTION = 8u;
constexpr uint32_t VSDF_TRACE_ROOT_CELL_COUNT = VSDF_TRACE_ROOT_RESOLUTION * VSDF_TRACE_ROOT_RESOLUTION * VSDF_TRACE_ROOT_RESOLUTION;
constexpr uint32_t VSDF_TRACE_PAGE_RESOLUTION = 8u;
constexpr uint32_t VSDF_TRACE_PAGE_CELL_COUNT = VSDF_TRACE_PAGE_RESOLUTION * VSDF_TRACE_PAGE_RESOLUTION * VSDF_TRACE_PAGE_RESOLUTION;
constexpr uint32_t VSDF_TRACE_PAGE_MASK_WORD_COUNT = VSDF_TRACE_PAGE_CELL_COUNT / 32u;
constexpr uint32_t VSDF_TRACE_INVALID_BRICK = 0xffffffffu;
constexpr uint32_t VSDF_TRACE_AABB_BOTTOM_NODE_COUNT = 16u * 16u * 16u;
constexpr uint32_t VSDF_TRACE_AABB_MID_NODE_COUNT = 4u * 4u * 4u;
constexpr uint32_t VSDF_TRACE_AABB_BOTTOM_WORD_OFFSET = 0u;
constexpr uint32_t VSDF_TRACE_AABB_MID_WORD_OFFSET = VSDF_TRACE_AABB_BOTTOM_NODE_COUNT;
constexpr uint32_t VSDF_TRACE_AABB_ROOT_WORD_OFFSET = VSDF_TRACE_AABB_MID_WORD_OFFSET + VSDF_TRACE_AABB_MID_NODE_COUNT * 6u;
constexpr uint32_t VSDF_TRACE_AABB_TREE_WORD_COUNT = VSDF_TRACE_AABB_ROOT_WORD_OFFSET + 6u;

enum VSDFCompression : uint32_t
{
  VSDF_COMPRESSION_NONE = 0u,
  VSDF_COMPRESSION_LZ4 = 1u,
};

enum VSDFDistanceFormat : uint32_t
{
  VSDF_DISTANCE_R8_SNORM = 0u,
  VSDF_DISTANCE_R16_SNORM = 1u,
  VSDF_DISTANCE_R16_FLOAT = 2u,
};

struct VSDFHeader
{
  uint32_t magic = VSDF_MAGIC;
  uint32_t version = VSDF_VERSION;
  uint32_t headerSize = 0u;
  uint32_t flags = 0u;

  uint32_t instanceCount = 0u;
  uint32_t instanceDirectoryStride = 0u;
  uint64_t instanceDirectoryOffset = 0u;

  uint64_t stringTableOffset = 0u;
  uint64_t stringTableSize = 0u;

  float sceneBoundsMin[3] = {0.0f, 0.0f, 0.0f};
  uint32_t _padding0 = 0u;
  float sceneBoundsMax[3] = {0.0f, 0.0f, 0.0f};
  uint32_t _padding1 = 0u;

  uint64_t sourceGeometryHash = 0u;
  uint64_t buildSettingsHash = 0u;
};

struct VSDFInstanceDirectoryEntry
{
  uint32_t instanceId = 0u;
  uint32_t sourceMeshIndex = 0u;
  uint32_t materialRangeBegin = 0u;
  uint32_t materialRangeCount = 0u;

  uint64_t instancePayloadOffset = 0u;
  uint64_t compressedSize = 0u;
  uint64_t uncompressedSize = 0u;

  uint32_t compression = VSDF_COMPRESSION_LZ4;
  uint32_t distanceFormat = VSDF_DISTANCE_R8_SNORM;
  uint32_t maxLevel = 0u;
  uint32_t flags = 0u;

  float worldBoundsMin[3] = {0.0f, 0.0f, 0.0f};
  float conservativeScale = 1.0f;
  float worldBoundsMax[3] = {0.0f, 0.0f, 0.0f};
  float requestedError = 0.0f;
  uint32_t _padding0[2] = {0u, 0u};
};

struct VSDFInstancePayloadHeader
{
  uint32_t payloadVersion = VSDF_VERSION;
  uint32_t nodeCount = 0u;
  uint32_t brickCount = 0u;
  uint32_t distanceByteCount = 0u;

  uint32_t rootNode = 0u;
  uint32_t brickResolution = VSDF_BRICK_RESOLUTION;
  uint32_t narrowBandVoxels = 4u;
  uint32_t maxLevel = 0u;

  float localBoundsMin[3] = {0.0f, 0.0f, 0.0f};
  float rootVoxelSize = 1.0f;
  float localBoundsMax[3] = {0.0f, 0.0f, 0.0f};
  float requestedError = 0.0f;

  float localToWorld[16] = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f};
  float worldToLocal[16] = {
      1.0f, 0.0f, 0.0f, 0.0f,
      0.0f, 1.0f, 0.0f, 0.0f,
      0.0f, 0.0f, 1.0f, 0.0f,
      0.0f, 0.0f, 0.0f, 1.0f};

  uint32_t nodesOffset = 0u;
  uint32_t bricksOffset = 0u;
  uint32_t distancesOffset = 0u;
  uint32_t triangleDebugOffset = 0u;

  uint32_t traceGridOffset = 0u;
  uint32_t traceGridByteCount = 0u;
  uint32_t traceGridResolution = VSDF_TRACE_GRID_RESOLUTION;
  uint32_t _padding0 = 0u;

  uint32_t tracePageTableOffset = 0u;
  uint32_t tracePageTableEntryCount = 0u;
  uint32_t tracePagesOffset = 0u;
  uint32_t tracePageCount = 0u;

  uint32_t tracePageMasksOffset = 0u;
  uint32_t tracePageMaskWordCount = 0u;
  uint32_t traceRootResolution = VSDF_TRACE_ROOT_RESOLUTION;
  uint32_t tracePageResolution = VSDF_TRACE_PAGE_RESOLUTION;

  uint32_t traceBrickMapOffset = 0u;
  uint32_t traceBrickMapWordCount = 0u;
  uint32_t traceBrickMapFormat = 2u;
  uint32_t _padding1 = 0u;

  uint32_t traceAabbTreeOffset = 0u;
  uint32_t traceAabbTreeWordCount = 0u;
  uint32_t traceAabbTreeLayout = 1u;
  uint32_t _padding2 = 0u;
};

struct VSDFNodeDisk
{
  uint32_t brickIndex = 0u;
  uint32_t firstChild = UINT32_MAX;
  uint32_t childMaskWords[16] = {};
  uint32_t validMaskWords[16] = {};
  uint32_t traceMaskWords[16] = {};
  uint16_t level = 0u;
  uint16_t flags = 0u;
  uint32_t _padding0 = 0u;
};

struct VSDFBrickDisk
{
  uint32_t distanceOffset = 0u;
  uint32_t minMaxDistancePacked = 0u;
  float voxelSize = 1.0f;
  float maxError = 0.0f;
};

struct VSDFTracePageDisk
{
  uint32_t maskOffset = 0u;
  uint32_t brickOffset = 0u;
  uint32_t childCount = 0u;
  uint32_t _padding0 = 0u;
};

struct VSDFDecodedInstance
{
  VSDFInstancePayloadHeader header;
  std::vector<VSDFNodeDisk> nodes;
  std::vector<VSDFBrickDisk> bricks;
  std::vector<uint8_t> distances;
  std::vector<uint8_t> traceGrid;
  std::vector<uint32_t> tracePageTable;
  std::vector<VSDFTracePageDisk> tracePages;
  std::vector<uint32_t> tracePageMasks;
  std::vector<uint32_t> traceBrickMap;
  std::vector<uint32_t> traceAabbTree;
};

struct VSDFInstanceWriteData
{
  VSDFInstanceDirectoryEntry directory;
  VSDFInstancePayloadHeader header;
  std::vector<VSDFNodeDisk> nodes;
  std::vector<VSDFBrickDisk> bricks;
  std::vector<uint8_t> distances;
  std::vector<uint8_t> traceGrid;
  std::vector<uint32_t> tracePageTable;
  std::vector<VSDFTracePageDisk> tracePages;
  std::vector<uint32_t> tracePageMasks;
  std::vector<uint32_t> traceBrickMap;
  std::vector<uint32_t> traceAabbTree;
};

class SDFSceneFile
{
public:
  bool load(const std::string &path, std::string *error = nullptr);
  bool loadInstance(uint32_t instanceIndex, VSDFDecodedInstance &outInstance, std::string *error = nullptr) const;

  const VSDFHeader &header() const { return header_; }
  const std::vector<VSDFInstanceDirectoryEntry> &instances() const { return instances_; }

  static bool save(
      const std::string &path,
      const std::vector<VSDFInstanceWriteData> &instances,
      uint64_t sourceGeometryHash,
      uint64_t buildSettingsHash,
      std::string *error = nullptr);

private:
  std::vector<uint8_t> fileBytes_;
  VSDFHeader header_{};
  std::vector<VSDFInstanceDirectoryEntry> instances_;
};

} // namespace sdf
