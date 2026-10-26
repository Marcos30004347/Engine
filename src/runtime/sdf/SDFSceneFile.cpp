#include "sdf/SDFSceneFile.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <utility>
#include <vector>

#include "lz4.h"

namespace sdf
{
namespace
{

static_assert(sizeof(VSDFHeader) == 96u, "VSDFHeader layout changed");
static_assert(sizeof(VSDFInstanceDirectoryEntry) == 96u, "VSDFInstanceDirectoryEntry layout changed");
static_assert(sizeof(VSDFInstancePayloadHeader) == 288u, "VSDFInstancePayloadHeader layout changed");
static_assert(sizeof(VSDFNodeDisk) == 208u, "VSDFNodeDisk layout changed");
static_assert(sizeof(VSDFBrickDisk) == 16u, "VSDFBrickDisk layout changed");
static_assert(sizeof(VSDFTracePageDisk) == 16u, "VSDFTracePageDisk layout changed");

uint64_t alignTo(uint64_t value, uint64_t alignment)
{
  return (value + alignment - 1u) & ~(alignment - 1u);
}

void setError(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
}

template <typename T>
bool readStruct(const std::vector<uint8_t> &bytes, uint64_t offset, T &out)
{
  if (offset > bytes.size() || sizeof(T) > bytes.size() - static_cast<size_t>(offset))
    return false;
  std::memcpy(&out, bytes.data() + offset, sizeof(T));
  return true;
}

template <typename T>
void appendStruct(std::vector<uint8_t> &bytes, const T &value)
{
  const size_t offset = bytes.size();
  bytes.resize(offset + sizeof(T));
  std::memcpy(bytes.data() + offset, &value, sizeof(T));
}

void appendPadding(std::vector<uint8_t> &bytes, uint64_t alignment)
{
  bytes.resize(static_cast<size_t>(alignTo(bytes.size(), alignment)), 0u);
}

bool validateRange(uint64_t offset, uint64_t size, uint64_t totalSize)
{
  return offset <= totalSize && size <= totalSize - offset;
}

std::vector<uint8_t> serializePayload(VSDFInstanceWriteData instance)
{
  appendPadding(instance.distances, 1u);

  VSDFInstancePayloadHeader header = instance.header;
  header.payloadVersion = VSDF_VERSION;
  header.nodeCount = static_cast<uint32_t>(instance.nodes.size());
  header.brickCount = static_cast<uint32_t>(instance.bricks.size());
  header.distanceByteCount = static_cast<uint32_t>(instance.distances.size());
  header.brickResolution = VSDF_BRICK_RESOLUTION;
  header.nodesOffset = static_cast<uint32_t>(sizeof(VSDFInstancePayloadHeader));
  header.bricksOffset = header.nodesOffset + static_cast<uint32_t>(instance.nodes.size() * sizeof(VSDFNodeDisk));
  header.distancesOffset = header.bricksOffset + static_cast<uint32_t>(instance.bricks.size() * sizeof(VSDFBrickDisk));
  header.triangleDebugOffset = 0u;
  header.traceGridOffset = header.distancesOffset + static_cast<uint32_t>(instance.distances.size());
  header.traceGridByteCount = static_cast<uint32_t>(instance.traceGrid.size());
  header.traceGridResolution = VSDF_TRACE_GRID_RESOLUTION;
  header.tracePageTableOffset = header.traceGridOffset + static_cast<uint32_t>(instance.traceGrid.size());
  header.tracePageTableEntryCount = static_cast<uint32_t>(instance.tracePageTable.size());
  header.tracePagesOffset = header.tracePageTableOffset + static_cast<uint32_t>(instance.tracePageTable.size() * sizeof(uint32_t));
  header.tracePageCount = static_cast<uint32_t>(instance.tracePages.size());
  header.tracePageMasksOffset = header.tracePagesOffset + static_cast<uint32_t>(instance.tracePages.size() * sizeof(VSDFTracePageDisk));
  header.tracePageMaskWordCount = static_cast<uint32_t>(instance.tracePageMasks.size());
  header.traceRootResolution = VSDF_TRACE_ROOT_RESOLUTION;
  header.tracePageResolution = VSDF_TRACE_PAGE_RESOLUTION;
  header.traceBrickMapOffset = header.tracePageMasksOffset + static_cast<uint32_t>(instance.tracePageMasks.size() * sizeof(uint32_t));
  header.traceBrickMapWordCount = static_cast<uint32_t>(instance.traceBrickMap.size());
  header.traceBrickMapFormat = 2u;
  header.traceAabbTreeOffset = header.traceBrickMapOffset + static_cast<uint32_t>(instance.traceBrickMap.size() * sizeof(uint32_t));
  header.traceAabbTreeWordCount = static_cast<uint32_t>(instance.traceAabbTree.size());
  header.traceAabbTreeLayout = 1u;

  std::vector<uint8_t> payload;
  payload.reserve(header.traceAabbTreeOffset + instance.traceAabbTree.size() * sizeof(uint32_t));
  appendStruct(payload, header);
  for (const VSDFNodeDisk &node : instance.nodes)
    appendStruct(payload, node);
  for (const VSDFBrickDisk &brick : instance.bricks)
    appendStruct(payload, brick);
  payload.insert(payload.end(), instance.distances.begin(), instance.distances.end());
  payload.insert(payload.end(), instance.traceGrid.begin(), instance.traceGrid.end());
  for (uint32_t entry : instance.tracePageTable)
    appendStruct(payload, entry);
  for (const VSDFTracePageDisk &page : instance.tracePages)
    appendStruct(payload, page);
  for (uint32_t word : instance.tracePageMasks)
    appendStruct(payload, word);
  for (uint32_t word : instance.traceBrickMap)
    appendStruct(payload, word);
  for (uint32_t word : instance.traceAabbTree)
    appendStruct(payload, word);
  return payload;
}

} // namespace

bool SDFSceneFile::load(const std::string &path, std::string *error)
{
  fileBytes_.clear();
  instances_.clear();
  header_ = {};

  std::ifstream file(path, std::ios::binary);
  if (!file)
  {
    setError(error, "Failed to open VSDF file: " + path);
    return false;
  }

  file.seekg(0, std::ios::end);
  const std::streamoff fileSize = file.tellg();
  if (fileSize < static_cast<std::streamoff>(sizeof(VSDFHeader)))
  {
    setError(error, "VSDF file is too small: " + path);
    return false;
  }
  file.seekg(0, std::ios::beg);

  fileBytes_.resize(static_cast<size_t>(fileSize));
  file.read(reinterpret_cast<char *>(fileBytes_.data()), fileSize);
  if (!file)
  {
    setError(error, "Failed to read VSDF file: " + path);
    fileBytes_.clear();
    return false;
  }

  if (!readStruct(fileBytes_, 0u, header_))
  {
    setError(error, "Failed to read VSDF header");
    return false;
  }

  if (header_.magic != VSDF_MAGIC)
  {
    setError(error, "Invalid VSDF magic");
    return false;
  }
  if (header_.version != VSDF_VERSION)
  {
    setError(error, "Unsupported VSDF version");
    return false;
  }
  if (header_.headerSize != sizeof(VSDFHeader))
  {
    setError(error, "Unexpected VSDF header size");
    return false;
  }
  if (header_.instanceDirectoryStride != sizeof(VSDFInstanceDirectoryEntry))
  {
    setError(error, "Unexpected VSDF instance directory stride");
    return false;
  }

  const uint64_t directorySize = static_cast<uint64_t>(header_.instanceCount) * header_.instanceDirectoryStride;
  if (!validateRange(header_.instanceDirectoryOffset, directorySize, fileBytes_.size()))
  {
    setError(error, "VSDF instance directory is out of range");
    return false;
  }

  instances_.resize(header_.instanceCount);
  for (uint32_t i = 0u; i < header_.instanceCount; ++i)
  {
    const uint64_t offset = header_.instanceDirectoryOffset + static_cast<uint64_t>(i) * header_.instanceDirectoryStride;
    if (!readStruct(fileBytes_, offset, instances_[i]))
    {
      setError(error, "Failed to read VSDF instance directory entry");
      return false;
    }

    const VSDFInstanceDirectoryEntry &entry = instances_[i];
    if (entry.compression != VSDF_COMPRESSION_NONE && entry.compression != VSDF_COMPRESSION_LZ4)
    {
      setError(error, "Unsupported VSDF compression mode");
      return false;
    }
    if (!validateRange(entry.instancePayloadOffset, entry.compressedSize, fileBytes_.size()))
    {
      setError(error, "VSDF instance payload is out of range");
      return false;
    }
    if (entry.uncompressedSize < sizeof(VSDFInstancePayloadHeader))
    {
      setError(error, "VSDF instance payload is too small");
      return false;
    }
    if ((entry.instancePayloadOffset & 15u) != 0u)
    {
      setError(error, "VSDF instance payload is not 16-byte aligned");
      return false;
    }
  }

  std::vector<std::pair<uint64_t, uint64_t>> payloadRanges;
  payloadRanges.reserve(instances_.size());
  for (const VSDFInstanceDirectoryEntry &entry : instances_)
    payloadRanges.emplace_back(entry.instancePayloadOffset, entry.instancePayloadOffset + entry.compressedSize);
  std::sort(payloadRanges.begin(), payloadRanges.end());
  for (size_t i = 1u; i < payloadRanges.size(); ++i)
  {
    if (payloadRanges[i].first < payloadRanges[i - 1u].second)
    {
      setError(error, "VSDF instance payload ranges overlap");
      return false;
    }
  }

  return true;
}

bool SDFSceneFile::loadInstance(uint32_t instanceIndex, VSDFDecodedInstance &outInstance, std::string *error) const
{
  outInstance = {};
  if (instanceIndex >= instances_.size())
  {
    setError(error, "VSDF instance index out of range");
    return false;
  }

  const VSDFInstanceDirectoryEntry &entry = instances_[instanceIndex];
  std::vector<uint8_t> decompressed(static_cast<size_t>(entry.uncompressedSize));
  const char *source = reinterpret_cast<const char *>(fileBytes_.data() + entry.instancePayloadOffset);

  if (entry.compression == VSDF_COMPRESSION_NONE)
  {
    if (entry.compressedSize != entry.uncompressedSize)
    {
      setError(error, "Uncompressed VSDF payload has mismatched sizes");
      return false;
    }
    std::memcpy(decompressed.data(), source, decompressed.size());
  }
  else
  {
    if (entry.compressedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) ||
        entry.uncompressedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()))
    {
      setError(error, "VSDF payload is too large for LZ4 block decompression");
      return false;
    }
    const int decodedSize = LZ4_decompress_safe(
        source,
        reinterpret_cast<char *>(decompressed.data()),
        static_cast<int>(entry.compressedSize),
        static_cast<int>(entry.uncompressedSize));
    if (decodedSize != static_cast<int>(entry.uncompressedSize))
    {
      setError(error, "Failed to decompress VSDF LZ4 instance payload");
      return false;
    }
  }

  if (!readStruct(decompressed, 0u, outInstance.header))
  {
    setError(error, "Failed to read VSDF instance payload header");
    return false;
  }

  const VSDFInstancePayloadHeader &header = outInstance.header;
  if (header.payloadVersion != VSDF_VERSION || header.brickResolution != VSDF_BRICK_RESOLUTION ||
      header.traceGridResolution != VSDF_TRACE_GRID_RESOLUTION ||
      header.traceRootResolution != VSDF_TRACE_ROOT_RESOLUTION ||
      header.tracePageResolution != VSDF_TRACE_PAGE_RESOLUTION ||
      header.traceBrickMapFormat != 2u || header.traceAabbTreeLayout != 1u)
  {
    setError(error, "Unsupported VSDF instance payload version, brick resolution, trace grid resolution, trace brick map format, or trace AABB layout");
    return false;
  }

  const uint64_t nodesSize = static_cast<uint64_t>(header.nodeCount) * sizeof(VSDFNodeDisk);
  const uint64_t bricksSize = static_cast<uint64_t>(header.brickCount) * sizeof(VSDFBrickDisk);
  const uint64_t tracePagesSize = static_cast<uint64_t>(header.tracePageCount) * sizeof(VSDFTracePageDisk);
  if (!validateRange(header.nodesOffset, nodesSize, decompressed.size()) ||
      !validateRange(header.bricksOffset, bricksSize, decompressed.size()) ||
      !validateRange(header.distancesOffset, header.distanceByteCount, decompressed.size()) ||
      !validateRange(header.traceGridOffset, header.traceGridByteCount, decompressed.size()) ||
      !validateRange(header.tracePageTableOffset, static_cast<uint64_t>(header.tracePageTableEntryCount) * sizeof(uint32_t), decompressed.size()) ||
      !validateRange(header.tracePagesOffset, tracePagesSize, decompressed.size()) ||
      !validateRange(header.tracePageMasksOffset, static_cast<uint64_t>(header.tracePageMaskWordCount) * sizeof(uint32_t), decompressed.size()) ||
      !validateRange(header.traceBrickMapOffset, static_cast<uint64_t>(header.traceBrickMapWordCount) * sizeof(uint32_t), decompressed.size()) ||
      !validateRange(header.traceAabbTreeOffset, static_cast<uint64_t>(header.traceAabbTreeWordCount) * sizeof(uint32_t), decompressed.size()))
  {
    setError(error, "VSDF instance payload table is out of range");
    return false;
  }
  if (header.traceGridByteCount != 0u && header.traceGridByteCount != VSDF_TRACE_GRID_CELL_COUNT)
  {
    setError(error, "VSDF instance trace grid has unexpected size");
    return false;
  }
  if (header.traceAabbTreeWordCount != VSDF_TRACE_AABB_TREE_WORD_COUNT)
  {
    setError(error, "VSDF instance trace AABB tree has unexpected size");
    return false;
  }
  if (header.tracePageTableEntryCount != VSDF_TRACE_ROOT_CELL_COUNT ||
      header.tracePageMaskWordCount != header.tracePageCount * VSDF_TRACE_PAGE_MASK_WORD_COUNT)
  {
    setError(error, "VSDF instance sparse trace page table has unexpected size");
    return false;
  }

  outInstance.nodes.resize(header.nodeCount);
  outInstance.bricks.resize(header.brickCount);
  outInstance.distances.resize(header.distanceByteCount);
  outInstance.traceGrid.resize(header.traceGridByteCount);
  outInstance.tracePageTable.resize(header.tracePageTableEntryCount);
  outInstance.tracePages.resize(header.tracePageCount);
  outInstance.tracePageMasks.resize(header.tracePageMaskWordCount);
  outInstance.traceBrickMap.resize(header.traceBrickMapWordCount);
  outInstance.traceAabbTree.resize(header.traceAabbTreeWordCount);

  if (nodesSize != 0u)
    std::memcpy(outInstance.nodes.data(), decompressed.data() + header.nodesOffset, static_cast<size_t>(nodesSize));
  if (bricksSize != 0u)
    std::memcpy(outInstance.bricks.data(), decompressed.data() + header.bricksOffset, static_cast<size_t>(bricksSize));
  if (header.distanceByteCount != 0u)
    std::memcpy(outInstance.distances.data(), decompressed.data() + header.distancesOffset, header.distanceByteCount);
  if (header.traceGridByteCount != 0u)
    std::memcpy(outInstance.traceGrid.data(), decompressed.data() + header.traceGridOffset, header.traceGridByteCount);
  if (header.tracePageTableEntryCount != 0u)
    std::memcpy(outInstance.tracePageTable.data(), decompressed.data() + header.tracePageTableOffset, static_cast<size_t>(header.tracePageTableEntryCount) * sizeof(uint32_t));
  if (tracePagesSize != 0u)
    std::memcpy(outInstance.tracePages.data(), decompressed.data() + header.tracePagesOffset, static_cast<size_t>(tracePagesSize));
  if (header.tracePageMaskWordCount != 0u)
    std::memcpy(outInstance.tracePageMasks.data(), decompressed.data() + header.tracePageMasksOffset, static_cast<size_t>(header.tracePageMaskWordCount) * sizeof(uint32_t));
  if (header.traceBrickMapWordCount != 0u)
    std::memcpy(outInstance.traceBrickMap.data(), decompressed.data() + header.traceBrickMapOffset, static_cast<size_t>(header.traceBrickMapWordCount) * sizeof(uint32_t));
  if (header.traceAabbTreeWordCount != 0u)
    std::memcpy(outInstance.traceAabbTree.data(), decompressed.data() + header.traceAabbTreeOffset, static_cast<size_t>(header.traceAabbTreeWordCount) * sizeof(uint32_t));

  uint32_t referencedBrickCount = 0u;
  for (uint32_t pageIndex : outInstance.tracePageTable)
  {
    if (pageIndex != VSDF_TRACE_INVALID_BRICK && pageIndex >= outInstance.tracePages.size())
    {
      setError(error, "VSDF instance sparse trace page table references an out-of-range page");
      return false;
    }
  }
  for (const VSDFTracePageDisk &page : outInstance.tracePages)
  {
    if (page.maskOffset + VSDF_TRACE_PAGE_MASK_WORD_COUNT > outInstance.tracePageMasks.size() ||
        page.brickOffset + page.childCount > outInstance.traceBrickMap.size())
    {
      setError(error, "VSDF instance sparse trace page references out-of-range data");
      return false;
    }
    uint32_t maskBitCount = 0u;
    for (uint32_t word = 0u; word < VSDF_TRACE_PAGE_MASK_WORD_COUNT; ++word)
      maskBitCount += static_cast<uint32_t>(__builtin_popcount(outInstance.tracePageMasks[page.maskOffset + word]));
    if (maskBitCount != page.childCount)
    {
      setError(error, "VSDF instance sparse trace page child count does not match its mask");
      return false;
    }
    referencedBrickCount += page.childCount;
  }
  if (referencedBrickCount != outInstance.traceBrickMap.size())
  {
    setError(error, "VSDF instance sparse trace brick map has unreferenced entries");
    return false;
  }

  return true;
}

bool SDFSceneFile::save(
    const std::string &path,
    const std::vector<VSDFInstanceWriteData> &instances,
    uint64_t sourceGeometryHash,
    uint64_t buildSettingsHash,
    std::string *error)
{
  std::vector<std::vector<uint8_t>> compressedPayloads;
  compressedPayloads.reserve(instances.size());

  std::vector<VSDFInstanceDirectoryEntry> directory;
  directory.reserve(instances.size());

  for (const VSDFInstanceWriteData &instance : instances)
  {
    const std::vector<uint8_t> payload = serializePayload(instance);
    if (payload.size() > static_cast<size_t>(std::numeric_limits<int>::max()))
    {
      setError(error, "VSDF payload is too large for LZ4 block compression");
      return false;
    }

    std::vector<uint8_t> compressed;
    const int maxCompressedSize = LZ4_compressBound(static_cast<int>(payload.size()));
    compressed.resize(static_cast<size_t>(maxCompressedSize));
    const int compressedSize = LZ4_compress_default(
        reinterpret_cast<const char *>(payload.data()),
        reinterpret_cast<char *>(compressed.data()),
        static_cast<int>(payload.size()),
        maxCompressedSize);
    if (compressedSize <= 0)
    {
      setError(error, "Failed to compress VSDF payload");
      return false;
    }
    compressed.resize(static_cast<size_t>(compressedSize));

    VSDFInstanceDirectoryEntry entry = instance.directory;
    entry.compression = VSDF_COMPRESSION_LZ4;
    entry.distanceFormat = VSDF_DISTANCE_R8_SNORM;
    entry.compressedSize = compressed.size();
    entry.uncompressedSize = payload.size();
    directory.push_back(entry);
    compressedPayloads.push_back(std::move(compressed));
  }

  VSDFHeader header{};
  header.headerSize = sizeof(VSDFHeader);
  header.instanceCount = static_cast<uint32_t>(directory.size());
  header.instanceDirectoryStride = sizeof(VSDFInstanceDirectoryEntry);
  header.instanceDirectoryOffset = alignTo(sizeof(VSDFHeader), 16u);
  header.stringTableOffset = header.instanceDirectoryOffset + static_cast<uint64_t>(directory.size()) * sizeof(VSDFInstanceDirectoryEntry);
  header.stringTableOffset = alignTo(header.stringTableOffset, 16u);
  header.stringTableSize = 0u;
  header.sourceGeometryHash = sourceGeometryHash;
  header.buildSettingsHash = buildSettingsHash;

  bool hasBounds = false;
  for (const VSDFInstanceDirectoryEntry &entry : directory)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      if (!hasBounds)
      {
        header.sceneBoundsMin[axis] = entry.worldBoundsMin[axis];
        header.sceneBoundsMax[axis] = entry.worldBoundsMax[axis];
      }
      else
      {
        header.sceneBoundsMin[axis] = std::min(header.sceneBoundsMin[axis], entry.worldBoundsMin[axis]);
        header.sceneBoundsMax[axis] = std::max(header.sceneBoundsMax[axis], entry.worldBoundsMax[axis]);
      }
    }
    hasBounds = true;
  }

  uint64_t payloadOffset = header.stringTableOffset;
  for (size_t i = 0u; i < directory.size(); ++i)
  {
    payloadOffset = alignTo(payloadOffset, 16u);
    directory[i].instancePayloadOffset = payloadOffset;
    payloadOffset += compressedPayloads[i].size();
  }

  std::vector<uint8_t> bytes;
  appendStruct(bytes, header);
  appendPadding(bytes, 16u);
  for (const VSDFInstanceDirectoryEntry &entry : directory)
    appendStruct(bytes, entry);
  appendPadding(bytes, 16u);
  for (const std::vector<uint8_t> &payload : compressedPayloads)
  {
    appendPadding(bytes, 16u);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
  }

  std::ofstream file(path, std::ios::binary);
  if (!file)
  {
    setError(error, "Failed to open VSDF file for writing: " + path);
    return false;
  }
  file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  if (!file)
  {
    setError(error, "Failed to write VSDF file: " + path);
    return false;
  }

  return true;
}

} // namespace sdf
