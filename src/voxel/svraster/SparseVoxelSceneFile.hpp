#pragma once

#include "voxel/svraster/SparseVoxelScene.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voxel::svraster
{

constexpr uint32_t kVirtualSparseVoxelFileVersion = 1u;
constexpr uint32_t kVirtualSparseVoxelFileEndianMarker = 0x01020304u;
constexpr uint32_t kVirtualSparseVoxelFileHeaderSize = 96u;
constexpr uint32_t kVirtualSparseVoxelFileChunkRecordSize = 64u;
constexpr uint32_t kVirtualSparseVoxelFilePageRecordSize = 128u;
constexpr uint32_t kVirtualSparseVoxelFileAlignment = 16u;
constexpr uint32_t kVirtualSparseVoxelDefaultPageBytes = 64u * 1024u;

enum class VirtualSparseVoxelFileCompression : uint32_t
{
  None = 0u,
  LZ4 = 1u,
};

enum class VirtualSparseVoxelFileChunkType : uint32_t
{
  Scene = 1u,
  Models = 2u,
  Instances = 3u,
  Pages = 4u,
  PageDependencies = 5u,
  PageInstallUpdates = 6u,
  PageUninstallUpdates = 7u,
  PagePayloads = 8u,
  SourceInfo = 9u,
  SourceIds = 10u,
  DebugNames = 11u,
  MaterialReferences = 12u,
  DensityReserved = 0x100u,
};

constexpr uint32_t kVirtualSparseVoxelPageRoot = 1u << 0u;
constexpr uint32_t kVirtualSparseVoxelPageCompressedLZ4 = 1u << 1u;

struct VirtualSparseVoxelPageRewrite
{
  uint32_t nodeIndex = kInvalidSparseVoxelIndex;
  uint32_t firstChild = 0u;
  uint32_t childCount = 0u;
  uint32_t flags = 0u;
};
static_assert(sizeof(VirtualSparseVoxelPageRewrite) == 16u);

struct VirtualSparseVoxelSceneWriteOptions
{
  VirtualSparseVoxelFileCompression compression = VirtualSparseVoxelFileCompression::LZ4;
  uint32_t targetUncompressedPageBytes = kVirtualSparseVoxelDefaultPageBytes;
};

struct VirtualSparseVoxelFileChunkRecord
{
  VirtualSparseVoxelFileChunkType type{};
  uint32_t flags = 0u;
  uint64_t fileOffset = 0u;
  uint64_t storedSize = 0u;
  uint64_t uncompressedSize = 0u;
  uint32_t recordSize = 0u;
  uint64_t recordCount = 0u;
  uint64_t checksum = 0u;
};

struct VirtualSparseVoxelFileModelRecord
{
  std::array<float, 3> rootCenter{};
  float rootExtent = 0.0f;
  uint32_t maxLevel = 0u;
  uint32_t flags = 0u;
  uint32_t firstNode = 0u;
  uint32_t nodeCount = 0u;
  uint32_t firstRoot = 0u;
  uint32_t rootCount = 0u;
  uint32_t rootPage = kInvalidSparseVoxelIndex;
  uint32_t pageCount = 0u;
};

struct VirtualSparseVoxelFileInstanceRecord
{
  uint32_t blasIndex = 0u;
  uint32_t flags = 0u;
  std::array<float, 16> modelToWorld{};
};

struct VirtualSparseVoxelFilePageRecord
{
  uint32_t modelIndex = 0u;
  uint32_t flags = 0u;
  uint32_t firstNode = 0u;
  uint32_t nodeCount = 0u;
  uint32_t minLevel = 0u;
  uint32_t maxLevel = 0u;
  uint32_t dependencyCount = 0u;
  uint64_t dependencyFirst = 0u;
  uint64_t installFirst = 0u;
  uint32_t installCount = 0u;
  uint32_t uninstallCount = 0u;
  uint64_t uninstallFirst = 0u;
  uint64_t payloadOffset = 0u;
  uint64_t storedSize = 0u;
  uint64_t uncompressedSize = 0u;
  uint64_t checksum = 0u;
  std::array<float, 3> localBoundsMin{};
  std::array<float, 3> localBoundsMax{};
};

struct VirtualSparseVoxelSceneMetadata
{
  uint64_t fileSize = 0u;
  uint64_t totalUncompressedSize = 0u;
  uint64_t sourceContentHash = 0u;
  uint64_t buildSettingsHash = 0u;
  uint32_t modelCount = 0u;
  uint32_t instanceCount = 0u;
  uint32_t nodeCount = 0u;
  uint32_t pageCount = 0u;
  uint32_t sceneFlags = 0u;
  uint32_t appearanceEncoding = 0u;
  uint32_t tlasFanout = 0u;
  uint32_t tlasLeafSize = 0u;
  bool hasSourceIds = false;
  std::vector<VirtualSparseVoxelFileChunkRecord> chunks;
  std::vector<VirtualSparseVoxelFileModelRecord> models;
  std::vector<VirtualSparseVoxelFileInstanceRecord> instances;
  std::vector<VirtualSparseVoxelFilePageRecord> pages;
  std::vector<uint32_t> pageDependencies;
  std::vector<VirtualSparseVoxelPageRewrite> installUpdates;
  std::vector<VirtualSparseVoxelPageRewrite> uninstallUpdates;
  SparseVoxelSourceInfo sourceInfo{};
  std::vector<SparseVoxelMaterialAssetReference> voxelMaterialReferences;
};

struct VirtualSparseVoxelPageData
{
  uint32_t pageIndex = 0u;
  uint32_t modelIndex = 0u;
  uint32_t firstNode = 0u;
  std::vector<SparseVoxelLODNodeGPU> nodes;
  std::vector<SparseVoxelPathLevelGPU> pathsLevels;
  std::vector<SparseVoxelCenterLengthGPU> centersLengths;
  std::vector<SparseVoxelMaterialGPU> materials;
};

uint64_t virtualSparseVoxelFileChecksum(const uint8_t *data, size_t size);
bool serializeVirtualSparseVoxelScene(
    const SparseVoxelScene &scene,
    const VirtualSparseVoxelSceneWriteOptions &options,
    std::vector<uint8_t> &bytes,
    std::string *error = nullptr);
bool writeVirtualSparseVoxelScene(
    const std::string &path,
    const SparseVoxelScene &scene,
    const VirtualSparseVoxelSceneWriteOptions &options = {},
    std::string *error = nullptr);
bool readVirtualSparseVoxelSceneMetadata(
    const std::string &path,
    VirtualSparseVoxelSceneMetadata &metadata,
    std::string *error = nullptr);
bool readVirtualSparseVoxelPage(
    const std::string &path,
    const VirtualSparseVoxelSceneMetadata &metadata,
    uint32_t pageIndex,
    VirtualSparseVoxelPageData &page,
    std::string *error = nullptr);
bool readVirtualSparseVoxelScene(
    const std::string &path,
    SparseVoxelScene &scene,
    std::string *error = nullptr);

} // namespace voxel::svraster
