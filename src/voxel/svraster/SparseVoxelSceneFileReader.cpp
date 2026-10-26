#include "voxel/svraster/SparseVoxelSceneFile.hpp"
#include "voxel/svraster/SparseVoxelGenerator.hpp"

#include "lz4.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <set>

namespace voxel::svraster
{
namespace
{

constexpr uint32_t kSceneSolidOpaqueFlag = 1u << 0u;
constexpr uint32_t kScenePagedHierarchyFlag = 1u << 1u;
constexpr uint32_t kAppearanceEncodingSRGB8PBR = 1u;
constexpr uint32_t kPagePayloadHeaderSize = 64u;
constexpr uint32_t kPagePayloadMagic = 0x47505356u;
constexpr uint32_t kMaximumChunkCount = 64u;
constexpr uint32_t kRequiredChunkCount = 9u;
constexpr uint32_t kPageNodeBytes = sizeof(SparseVoxelLODNodeGPU) + sizeof(SparseVoxelPathLevelGPU) + sizeof(SparseVoxelCenterLengthGPU) + sizeof(SparseVoxelMaterialGPU);

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

bool checkedAdd(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (right > std::numeric_limits<uint64_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

bool checkedMultiply(const uint64_t left, const uint64_t right, uint64_t &result)
{
  if (left != 0u && right > std::numeric_limits<uint64_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

struct ByteReader
{
  const uint8_t *data = nullptr;
  size_t size = 0u;

  bool contains(const uint64_t offset, const uint64_t byteCount) const
  {
    uint64_t end = 0u;
    return checkedAdd(offset, byteCount, end) && end <= size;
  }

  uint32_t u32(const size_t offset) const
  {
    return static_cast<uint32_t>(data[offset]) | (static_cast<uint32_t>(data[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(data[offset + 2u]) << 16u) | (static_cast<uint32_t>(data[offset + 3u]) << 24u);
  }

  uint64_t u64(const size_t offset) const
  {
    uint64_t value = 0u;
    for (uint32_t byte = 0u; byte < 8u; ++byte)
      value |= static_cast<uint64_t>(data[offset + byte]) << (byte * 8u);
    return value;
  }

  float f32(const size_t offset) const
  {
    const uint32_t bits = u32(offset);
    float result = 0.0f;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
  }
};

bool openFile(const std::string &path, std::ifstream &file, uint64_t &size, std::string *error)
{
  file.open(path, std::ios::binary | std::ios::ate);
  if (!file)
    return fail(error, "Failed to open virtual sparse voxel file: " + path);
  const std::streampos end = file.tellg();
  if (end < 0)
    return fail(error, "Failed to query virtual sparse voxel file size: " + path);
  size = static_cast<uint64_t>(end);
  file.seekg(0, std::ios::beg);
  return static_cast<bool>(file) || fail(error, "Failed to seek virtual sparse voxel file: " + path);
}

bool readExact(std::ifstream &file, const uint64_t offset, uint8_t *destination, const uint64_t byteCount, std::string *error)
{
  if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) || byteCount > static_cast<uint64_t>(std::numeric_limits<std::streamsize>::max()))
    return fail(error, "Virtual sparse voxel read exceeds stream limits");
  file.clear();
  file.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
  if (!file)
    return fail(error, "Failed to seek within virtual sparse voxel file");
  if (byteCount != 0u)
    file.read(reinterpret_cast<char *>(destination), static_cast<std::streamsize>(byteCount));
  if (!file)
    return fail(error, "Virtual sparse voxel file is truncated");
  return true;
}

bool readBytes(std::ifstream &file, const uint64_t offset, const uint64_t byteCount, std::vector<uint8_t> &bytes, std::string *error)
{
  if (byteCount > std::numeric_limits<size_t>::max())
    return fail(error, "Virtual sparse voxel payload exceeds addressable memory");
  bytes.resize(static_cast<size_t>(byteCount));
  return readExact(file, offset, bytes.data(), byteCount, error);
}

bool checksumFileRange(std::ifstream &file, const uint64_t offset, const uint64_t byteCount, const uint64_t expected, std::string *error)
{
  std::array<uint8_t, 64u * 1024u> buffer{};
  uint64_t hash = 14695981039346656037ull;
  uint64_t cursor = 0u;
  while (cursor < byteCount)
  {
    const uint64_t count = std::min<uint64_t>(buffer.size(), byteCount - cursor);
    if (!readExact(file, offset + cursor, buffer.data(), count, error))
      return false;
    for (uint64_t index = 0u; index < count; ++index)
    {
      hash ^= buffer[static_cast<size_t>(index)];
      hash *= 1099511628211ull;
    }
    cursor += count;
  }
  if (hash != expected)
    return fail(error, "Virtual sparse voxel chunk checksum mismatch");
  return true;
}

bool supportedChunkType(const VirtualSparseVoxelFileChunkType type)
{
  const uint32_t value = static_cast<uint32_t>(type);
  return value >= static_cast<uint32_t>(VirtualSparseVoxelFileChunkType::Scene) && value <= static_cast<uint32_t>(VirtualSparseVoxelFileChunkType::MaterialReferences);
}

const VirtualSparseVoxelFileChunkRecord *findChunk(const VirtualSparseVoxelSceneMetadata &metadata, const VirtualSparseVoxelFileChunkType type)
{
  const auto iterator = std::find_if(metadata.chunks.begin(), metadata.chunks.end(), [type](const auto &chunk) { return chunk.type == type; });
  return iterator == metadata.chunks.end() ? nullptr : &*iterator;
}

bool readChunk(
    std::ifstream &file,
    const VirtualSparseVoxelSceneMetadata &metadata,
    const VirtualSparseVoxelFileChunkType type,
    std::vector<uint8_t> &bytes,
    std::string *error)
{
  const VirtualSparseVoxelFileChunkRecord *chunk = findChunk(metadata, type);
  if (chunk == nullptr)
    return fail(error, "Virtual sparse voxel file is missing a required chunk");
  return readBytes(file, chunk->fileOffset, chunk->storedSize, bytes, error);
}

bool fixedChunkLayout(
    const VirtualSparseVoxelSceneMetadata &metadata,
    const VirtualSparseVoxelFileChunkType type,
    const uint32_t recordSize,
    const uint64_t recordCount,
    std::string *error)
{
  const VirtualSparseVoxelFileChunkRecord *chunk = findChunk(metadata, type);
  if (chunk == nullptr || chunk->recordSize != recordSize || chunk->recordCount != recordCount)
    return fail(error, "Virtual sparse voxel chunk record layout does not match scene metadata");
  return true;
}

bool rangeWithin(const uint64_t first, const uint64_t count, const uint64_t limit)
{
  uint64_t end = 0u;
  return checkedAdd(first, count, end) && end <= limit;
}

bool validAffineTransform(const std::array<float, 16> &matrix)
{
  for (const float value : matrix)
    if (!std::isfinite(value))
      return false;
  constexpr float epsilon = 1.0e-6f;
  if (std::abs(matrix[3]) > epsilon || std::abs(matrix[7]) > epsilon || std::abs(matrix[11]) > epsilon || std::abs(matrix[15] - 1.0f) > epsilon)
    return false;
  const float determinant =
      matrix[0] * (matrix[5] * matrix[10] - matrix[9] * matrix[6]) -
      matrix[4] * (matrix[1] * matrix[10] - matrix[9] * matrix[2]) +
      matrix[8] * (matrix[1] * matrix[6] - matrix[5] * matrix[2]);
  return std::isfinite(determinant) && std::abs(determinant) > epsilon;
}

bool finiteNonNegativeHalf(const uint16_t value)
{
  return (value & 0x8000u) == 0u && (value & 0x7c00u) != 0x7c00u;
}

bool parseSceneChunk(const std::vector<uint8_t> &bytes, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  const ByteReader reader{bytes.data(), bytes.size()};
  if (bytes.size() != 64u || reader.u32(0u) != 1u)
    return fail(error, "Unsupported virtual sparse voxel scene schema");
  metadata.modelCount = reader.u32(4u);
  metadata.instanceCount = reader.u32(8u);
  metadata.sceneFlags = reader.u32(12u);
  const uint64_t nodeCount = reader.u64(16u);
  metadata.pageCount = reader.u32(24u);
  metadata.appearanceEncoding = reader.u32(28u);
  metadata.tlasFanout = reader.u32(32u);
  metadata.tlasLeafSize = reader.u32(36u);
  if (nodeCount > std::numeric_limits<uint32_t>::max())
    return fail(error, "Virtual sparse voxel node count exceeds the GPU limit");
  metadata.nodeCount = static_cast<uint32_t>(nodeCount);
  if (metadata.sceneFlags != (kSceneSolidOpaqueFlag | kScenePagedHierarchyFlag) || metadata.appearanceEncoding != kAppearanceEncodingSRGB8PBR ||
      reader.u64(40u) != metadata.sourceContentHash || reader.u64(48u) != metadata.buildSettingsHash || reader.u64(56u) != 0u)
    return fail(error, "Virtual sparse voxel scene metadata is invalid");
  if (metadata.modelCount == 0u || metadata.instanceCount == 0u || metadata.nodeCount == 0u || metadata.pageCount == 0u ||
      metadata.tlasFanout < 2u || metadata.tlasFanout > 8u || metadata.tlasLeafSize == 0u || metadata.tlasLeafSize > 64u)
    return fail(error, "Virtual sparse voxel scene counts or TLAS settings are invalid");
  return true;
}

bool parseModels(const std::vector<uint8_t> &bytes, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  const ByteReader reader{bytes.data(), bytes.size()};
  metadata.models.reserve(metadata.modelCount);
  uint64_t expectedFirst = 0u;
  for (uint32_t index = 0u; index < metadata.modelCount; ++index)
  {
    const size_t offset = static_cast<size_t>(index) * 64u;
    VirtualSparseVoxelFileModelRecord model{};
    model.rootCenter = {reader.f32(offset), reader.f32(offset + 4u), reader.f32(offset + 8u)};
    model.rootExtent = reader.f32(offset + 12u);
    model.maxLevel = reader.u32(offset + 16u);
    model.flags = reader.u32(offset + 20u);
    const uint64_t firstNode = reader.u64(offset + 24u);
    const uint64_t nodeCount = reader.u64(offset + 32u);
    model.firstRoot = reader.u32(offset + 40u);
    model.rootCount = reader.u32(offset + 44u);
    model.rootPage = reader.u32(offset + 48u);
    model.pageCount = reader.u32(offset + 52u);
    if (reader.u64(offset + 56u) != 0u || firstNode > std::numeric_limits<uint32_t>::max() || nodeCount > std::numeric_limits<uint32_t>::max())
      return fail(error, "Virtual sparse voxel model record exceeds version-1 limits");
    model.firstNode = static_cast<uint32_t>(firstNode);
    model.nodeCount = static_cast<uint32_t>(nodeCount);
    if (!std::isfinite(model.rootCenter[0]) || !std::isfinite(model.rootCenter[1]) || !std::isfinite(model.rootCenter[2]) || !std::isfinite(model.rootExtent) || model.rootExtent <= 0.0f ||
        model.maxLevel == 0u || model.maxLevel > kMaxOctreeLevels || model.flags != 0u || model.firstNode != expectedFirst || model.nodeCount == 0u ||
        !rangeWithin(model.firstNode, model.nodeCount, metadata.nodeCount) || model.firstRoot < model.firstNode ||
        !rangeWithin(model.firstRoot, model.rootCount, uint64_t{model.firstNode} + model.nodeCount) || model.rootCount == 0u || model.rootCount > 8u ||
        model.rootPage >= metadata.pageCount || model.pageCount == 0u || model.pageCount > metadata.pageCount)
      return fail(error, "Virtual sparse voxel model metadata is invalid");
    expectedFirst += model.nodeCount;
    metadata.models.push_back(model);
  }
  if (expectedFirst != metadata.nodeCount)
    return fail(error, "Virtual sparse voxel model node ranges do not cover the scene");
  return true;
}

bool parseInstances(const std::vector<uint8_t> &bytes, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  const ByteReader reader{bytes.data(), bytes.size()};
  metadata.instances.reserve(metadata.instanceCount);
  for (uint32_t index = 0u; index < metadata.instanceCount; ++index)
  {
    const size_t offset = static_cast<size_t>(index) * 80u;
    VirtualSparseVoxelFileInstanceRecord instance{};
    for (uint32_t element = 0u; element < 16u; ++element)
      instance.modelToWorld[element] = reader.f32(offset + element * 4u);
    instance.blasIndex = reader.u32(offset + 64u);
    instance.flags = reader.u32(offset + 68u);
    if (reader.u64(offset + 72u) != 0u || instance.blasIndex >= metadata.modelCount || instance.flags != 0u || !validAffineTransform(instance.modelToWorld))
      return fail(error, "Virtual sparse voxel instance metadata is invalid");
    metadata.instances.push_back(instance);
  }
  return true;
}

bool parsePages(const std::vector<uint8_t> &bytes, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  const VirtualSparseVoxelFileChunkRecord *payloadChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PagePayloads);
  const VirtualSparseVoxelFileChunkRecord *dependencyChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageDependencies);
  const VirtualSparseVoxelFileChunkRecord *installChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageInstallUpdates);
  const VirtualSparseVoxelFileChunkRecord *uninstallChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageUninstallUpdates);
  if (payloadChunk == nullptr || dependencyChunk == nullptr || installChunk == nullptr || uninstallChunk == nullptr)
    return fail(error, "Virtual sparse voxel page support chunks are missing");
  const ByteReader reader{bytes.data(), bytes.size()};
  metadata.pages.reserve(metadata.pageCount);
  std::vector<uint32_t> modelPageCounts(metadata.modelCount, 0u);
  std::vector<uint32_t> modelRootPageCounts(metadata.modelCount, 0u);
  std::vector<uint64_t> modelNextNode(metadata.modelCount, 0u);
  for (uint32_t modelIndex = 0u; modelIndex < metadata.modelCount; ++modelIndex)
    modelNextNode[modelIndex] = metadata.models[modelIndex].firstNode;
  std::vector<std::pair<uint64_t, uint64_t>> nodeRanges;
  std::vector<std::pair<uint64_t, uint64_t>> payloadRanges;
  uint32_t previousLevel = 0u;
  for (uint32_t pageIndex = 0u; pageIndex < metadata.pageCount; ++pageIndex)
  {
    const size_t offset = static_cast<size_t>(pageIndex) * kVirtualSparseVoxelFilePageRecordSize;
    VirtualSparseVoxelFilePageRecord page{};
    page.modelIndex = reader.u32(offset);
    page.flags = reader.u32(offset + 4u);
    const uint64_t firstNode = reader.u64(offset + 8u);
    page.nodeCount = reader.u32(offset + 16u);
    page.minLevel = reader.u32(offset + 20u);
    page.maxLevel = reader.u32(offset + 24u);
    page.dependencyCount = reader.u32(offset + 28u);
    page.dependencyFirst = reader.u64(offset + 32u);
    page.installFirst = reader.u64(offset + 40u);
    page.installCount = reader.u32(offset + 48u);
    page.uninstallCount = reader.u32(offset + 52u);
    page.uninstallFirst = reader.u64(offset + 56u);
    page.payloadOffset = reader.u64(offset + 64u);
    page.storedSize = reader.u64(offset + 72u);
    page.uncompressedSize = reader.u64(offset + 80u);
    page.checksum = reader.u64(offset + 88u);
    page.localBoundsMin = {reader.f32(offset + 96u), reader.f32(offset + 100u), reader.f32(offset + 104u)};
    page.localBoundsMax = {reader.f32(offset + 108u), reader.f32(offset + 112u), reader.f32(offset + 116u)};
    if (firstNode > std::numeric_limits<uint32_t>::max())
      return fail(error, "Virtual sparse voxel page node index exceeds the GPU limit");
    page.firstNode = static_cast<uint32_t>(firstNode);
    if (page.modelIndex >= metadata.modelCount || (page.flags & ~(kVirtualSparseVoxelPageRoot | kVirtualSparseVoxelPageCompressedLZ4)) != 0u ||
        page.nodeCount == 0u || page.minLevel == 0u || page.minLevel != page.maxLevel || page.maxLevel > metadata.models[page.modelIndex].maxLevel ||
        !rangeWithin(page.firstNode, page.nodeCount, metadata.nodeCount) || !rangeWithin(page.dependencyFirst, page.dependencyCount, dependencyChunk->recordCount) ||
        !rangeWithin(page.installFirst, page.installCount, installChunk->recordCount) || !rangeWithin(page.uninstallFirst, page.uninstallCount, uninstallChunk->recordCount) ||
        page.installCount != page.uninstallCount || page.payloadOffset % kVirtualSparseVoxelFileAlignment != 0u ||
        !rangeWithin(page.payloadOffset, page.storedSize, payloadChunk->storedSize) || page.storedSize == 0u)
      return fail(error, "Virtual sparse voxel page metadata is invalid");
    uint64_t expectedSize = 0u;
    if (!checkedMultiply(page.nodeCount, kPageNodeBytes, expectedSize) || !checkedAdd(expectedSize, kPagePayloadHeaderSize, expectedSize) || page.uncompressedSize != expectedSize ||
        ((page.flags & kVirtualSparseVoxelPageCompressedLZ4) == 0u && page.storedSize != page.uncompressedSize) ||
        ((page.flags & kVirtualSparseVoxelPageCompressedLZ4) != 0u && (page.storedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) || page.uncompressedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()))))
      return fail(error, "Virtual sparse voxel page payload sizes are invalid");
    const VirtualSparseVoxelFileModelRecord &model = metadata.models[page.modelIndex];
    const float rootHalf = model.rootExtent * 0.5f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float rootMin = model.rootCenter[axis] - rootHalf;
      const float rootMax = model.rootCenter[axis] + rootHalf;
      if (!std::isfinite(page.localBoundsMin[axis]) || !std::isfinite(page.localBoundsMax[axis]) || page.localBoundsMin[axis] > page.localBoundsMax[axis] ||
          page.localBoundsMin[axis] < rootMin || page.localBoundsMax[axis] > rootMax)
        return fail(error, "Virtual sparse voxel page local bounds are invalid");
    }
    if (reader.u64(offset + 120u) != 0u)
      return fail(error, "Virtual sparse voxel page reserved metadata is nonzero");
    if (page.firstNode < model.firstNode || !rangeWithin(page.firstNode, page.nodeCount, uint64_t{model.firstNode} + model.nodeCount) || page.minLevel < previousLevel ||
        page.firstNode != modelNextNode[page.modelIndex])
      return fail(error, "Virtual sparse voxel pages are not in canonical coarse-to-fine order");
    modelNextNode[page.modelIndex] += page.nodeCount;
    previousLevel = page.minLevel;
    if ((page.flags & kVirtualSparseVoxelPageRoot) != 0u)
    {
      if (page.firstNode != model.firstRoot || page.nodeCount != model.rootCount || page.minLevel != 1u || page.dependencyCount != 0u || page.installCount != 0u || model.rootPage != pageIndex)
        return fail(error, "Virtual sparse voxel root page is invalid");
      ++modelRootPageCounts[page.modelIndex];
    }
    else if (page.minLevel == 1u)
      return fail(error, "Virtual sparse voxel level-one page is not marked as a root page");
    ++modelPageCounts[page.modelIndex];
    nodeRanges.emplace_back(page.firstNode, uint64_t{page.firstNode} + page.nodeCount);
    payloadRanges.emplace_back(page.payloadOffset, page.payloadOffset + page.storedSize);
    metadata.pages.push_back(page);
  }
  std::sort(nodeRanges.begin(), nodeRanges.end());
  uint64_t expectedNode = 0u;
  for (const auto &range : nodeRanges)
  {
    if (range.first != expectedNode)
      return fail(error, "Virtual sparse voxel pages overlap or do not cover every node");
    expectedNode = range.second;
  }
  if (expectedNode != metadata.nodeCount)
    return fail(error, "Virtual sparse voxel pages do not cover every node");
  std::sort(payloadRanges.begin(), payloadRanges.end());
  for (size_t index = 1u; index < payloadRanges.size(); ++index)
    if (payloadRanges[index].first < payloadRanges[index - 1u].second)
      return fail(error, "Virtual sparse voxel page payloads overlap");
  for (uint32_t modelIndex = 0u; modelIndex < metadata.modelCount; ++modelIndex)
    if (modelRootPageCounts[modelIndex] != 1u || modelPageCounts[modelIndex] != metadata.models[modelIndex].pageCount ||
        modelNextNode[modelIndex] != uint64_t{metadata.models[modelIndex].firstNode} + metadata.models[modelIndex].nodeCount)
      return fail(error, "Virtual sparse voxel model page counts are invalid");
  return true;
}

bool parseDependenciesAndRewrites(
    const std::vector<uint8_t> &dependencyBytes,
    const std::vector<uint8_t> &installBytes,
    const std::vector<uint8_t> &uninstallBytes,
    VirtualSparseVoxelSceneMetadata &metadata,
    std::string *error)
{
  const ByteReader dependencies{dependencyBytes.data(), dependencyBytes.size()};
  const ByteReader installs{installBytes.data(), installBytes.size()};
  const ByteReader uninstalls{uninstallBytes.data(), uninstallBytes.size()};
  metadata.pageDependencies.reserve(dependencyBytes.size() / 4u);
  for (size_t offset = 0u; offset < dependencyBytes.size(); offset += 4u)
    metadata.pageDependencies.push_back(dependencies.u32(offset));
  auto parseRewrites = [](const ByteReader &reader, const size_t byteCount, std::vector<VirtualSparseVoxelPageRewrite> &result) {
    result.reserve(byteCount / 16u);
    for (size_t offset = 0u; offset < byteCount; offset += 16u)
      result.push_back({reader.u32(offset), reader.u32(offset + 4u), reader.u32(offset + 8u), reader.u32(offset + 12u)});
  };
  parseRewrites(installs, installBytes.size(), metadata.installUpdates);
  parseRewrites(uninstalls, uninstallBytes.size(), metadata.uninstallUpdates);

  std::vector<std::pair<uint64_t, uint64_t>> dependencyRanges;
  std::vector<std::pair<uint64_t, uint64_t>> installRanges;
  std::vector<std::pair<uint64_t, uint64_t>> uninstallRanges;

  for (uint32_t pageIndex = 0u; pageIndex < metadata.pageCount; ++pageIndex)
  {
    const VirtualSparseVoxelFilePageRecord &page = metadata.pages[pageIndex];
    if (page.dependencyCount != 0u)
      dependencyRanges.emplace_back(page.dependencyFirst, page.dependencyFirst + page.dependencyCount);
    if (page.installCount != 0u)
      installRanges.emplace_back(page.installFirst, page.installFirst + page.installCount);
    if (page.uninstallCount != 0u)
      uninstallRanges.emplace_back(page.uninstallFirst, page.uninstallFirst + page.uninstallCount);
    std::set<uint32_t> pageDependencySet;
    for (uint32_t index = 0u; index < page.dependencyCount; ++index)
    {
      const uint32_t dependency = metadata.pageDependencies[static_cast<size_t>(page.dependencyFirst + index)];
      if (dependency >= pageIndex || metadata.pages[dependency].modelIndex != page.modelIndex || !pageDependencySet.insert(dependency).second)
        return fail(error, "Virtual sparse voxel page dependency is not an earlier page of the same model");
    }
    std::vector<std::pair<uint64_t, uint64_t>> childRanges;
    uint32_t previousNode = 0u;
    for (uint32_t index = 0u; index < page.installCount; ++index)
    {
      const VirtualSparseVoxelPageRewrite &install = metadata.installUpdates[static_cast<size_t>(page.installFirst + index)];
      const VirtualSparseVoxelPageRewrite &uninstall = metadata.uninstallUpdates[static_cast<size_t>(page.uninstallFirst + index)];
      const VirtualSparseVoxelFileModelRecord &model = metadata.models[page.modelIndex];
      if (install.flags != 0u || install.childCount == 0u || install.childCount > 8u || install.nodeIndex < model.firstNode || install.nodeIndex >= model.firstNode + model.nodeCount ||
          install.firstChild < page.firstNode || !rangeWithin(install.firstChild, install.childCount, uint64_t{page.firstNode} + page.nodeCount) ||
          uninstall.nodeIndex != install.nodeIndex || uninstall.firstChild != install.firstChild || uninstall.childCount != 0u || uninstall.flags != 0u)
        return fail(error, "Virtual sparse voxel page rewrite is invalid");
      if (index != 0u && install.nodeIndex <= previousNode)
        return fail(error, "Virtual sparse voxel page rewrites are not in canonical parent order");
      previousNode = install.nodeIndex;
      uint32_t parentPage = kInvalidSparseVoxelIndex;
      for (uint32_t candidate = 0u; candidate < pageIndex; ++candidate)
      {
        const auto &candidatePage = metadata.pages[candidate];
        if (install.nodeIndex >= candidatePage.firstNode && install.nodeIndex < candidatePage.firstNode + candidatePage.nodeCount)
        {
          parentPage = candidate;
          break;
        }
      }
      if (parentPage == kInvalidSparseVoxelIndex || pageDependencySet.count(parentPage) == 0u)
        return fail(error, "Virtual sparse voxel page rewrite parent is not declared as a dependency");
      childRanges.emplace_back(install.firstChild, uint64_t{install.firstChild} + install.childCount);
    }
    if ((page.flags & kVirtualSparseVoxelPageRoot) == 0u)
    {
      std::sort(childRanges.begin(), childRanges.end());
      uint64_t expectedChild = page.firstNode;
      for (const auto &range : childRanges)
      {
        if (range.first != expectedChild)
          return fail(error, "Virtual sparse voxel page rewrites overlap or do not install every child");
        expectedChild = range.second;
      }
      if (expectedChild != uint64_t{page.firstNode} + page.nodeCount)
        return fail(error, "Virtual sparse voxel page rewrites do not install every child");
    }
  }
  const auto coversRecords = [](std::vector<std::pair<uint64_t, uint64_t>> ranges, const uint64_t count) {
    std::sort(ranges.begin(), ranges.end());
    uint64_t cursor = 0u;
    for (const auto &range : ranges)
    {
      if (range.first != cursor)
        return false;
      cursor = range.second;
    }
    return cursor == count;
  };
  if (!coversRecords(std::move(dependencyRanges), metadata.pageDependencies.size()) ||
      !coversRecords(std::move(installRanges), metadata.installUpdates.size()) ||
      !coversRecords(std::move(uninstallRanges), metadata.uninstallUpdates.size()))
    return fail(error, "Virtual sparse voxel page record ranges overlap or leave unused records");
  return true;
}

bool parseSourceInfo(const std::vector<uint8_t> &bytes, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  const ByteReader reader{bytes.data(), bytes.size()};
  if (bytes.size() < 32u || reader.u32(0u) != 1u)
    return fail(error, "Unsupported virtual sparse voxel source-info schema");
  const uint32_t pathSize = reader.u32(8u);
  const uint32_t optionsSize = reader.u32(12u);
  uint64_t expected = 32u;
  if (!checkedAdd(expected, pathSize, expected) || !checkedAdd(expected, optionsSize, expected) || expected != bytes.size() ||
      reader.u64(16u) != metadata.sourceContentHash || reader.u64(24u) != metadata.buildSettingsHash)
    return fail(error, "Virtual sparse voxel source-info payload is invalid");
  metadata.sourceInfo.importerVersion = reader.u32(4u);
  metadata.sourceInfo.sourceRelativePath.assign(reinterpret_cast<const char *>(bytes.data() + 32u), pathSize);
  metadata.sourceInfo.normalizedBuildOptions.assign(reinterpret_cast<const char *>(bytes.data() + 32u + pathSize), optionsSize);
  if (metadata.sourceInfo.sourceRelativePath.find('\0') != std::string::npos || metadata.sourceInfo.normalizedBuildOptions.find('\0') != std::string::npos)
    return fail(error, "Virtual sparse voxel source-info strings contain an embedded NUL");
  return true;
}

bool parseMaterialReferences(
    const std::vector<uint8_t> &bytes,
    VirtualSparseVoxelSceneMetadata &metadata,
    std::string *error)
{
  const ByteReader reader{bytes.data(), bytes.size()};
  if (!reader.contains(0u, 8u) || reader.u32(0u) != 1u)
    return fail(error, "Virtual sparse voxel material-reference payload header is invalid");
  const uint32_t count = reader.u32(4u);
  if (count > (bytes.size() - 8u) / 20u)
    return fail(error, "Virtual sparse voxel material-reference count exceeds its payload");
  size_t offset = 8u;
  metadata.voxelMaterialReferences.clear();
  metadata.voxelMaterialReferences.reserve(count);
  std::set<uint32_t> sourceMaterialIds;
  for (uint32_t index = 0u; index < count; ++index)
  {
    if (!reader.contains(offset, 20u))
      return fail(error, "Virtual sparse voxel material-reference payload is truncated");
    SparseVoxelMaterialAssetReference reference{};
    reference.sourceMaterialId = reader.u32(offset);
    reference.contextFields = reader.u32(offset + 4u);
    reference.assetHash = reader.u64(offset + 8u);
    const uint32_t stringSize = reader.u32(offset + 16u);
    offset += 20u;
    if (!reader.contains(offset, stringSize))
      return fail(error, "Virtual sparse voxel material asset ID is truncated");
    reference.assetId.assign(
        reinterpret_cast<const char *>(bytes.data() + offset), stringSize);
    offset += stringSize;
    if (reference.assetId.empty() || reference.assetId.find('\0') != std::string::npos ||
        !sourceMaterialIds.insert(reference.sourceMaterialId).second)
      return fail(error, "Virtual sparse voxel material-reference payload is invalid");
    metadata.voxelMaterialReferences.push_back(std::move(reference));
  }
  if (offset != bytes.size())
    return fail(error, "Virtual sparse voxel material-reference payload has trailing bytes");
  return true;
}

bool modelForGlobalNode(const VirtualSparseVoxelSceneMetadata &metadata, const uint32_t globalNode, uint32_t &modelIndex)
{
  for (uint32_t index = 0u; index < metadata.models.size(); ++index)
  {
    const auto &model = metadata.models[index];
    if (globalNode >= model.firstNode && globalNode < model.firstNode + model.nodeCount)
    {
      modelIndex = index;
      return true;
    }
  }
  return false;
}

} // namespace

bool readVirtualSparseVoxelSceneMetadataImpl(const std::string &path, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  metadata = {};
  if (error != nullptr)
    error->clear();
  std::ifstream file;
  uint64_t actualFileSize = 0u;
  if (!openFile(path, file, actualFileSize, error))
    return false;
  if (actualFileSize < kVirtualSparseVoxelFileHeaderSize)
    return fail(error, "Virtual sparse voxel file is shorter than its fixed header");
  std::array<uint8_t, kVirtualSparseVoxelFileHeaderSize> headerBytes{};
  if (!readExact(file, 0u, headerBytes.data(), headerBytes.size(), error))
    return false;
  const ByteReader header{headerBytes.data(), headerBytes.size()};
  if (std::memcmp(headerBytes.data(), "VSVO", 4u) != 0)
    return fail(error, "Virtual sparse voxel file magic is invalid");
  const uint32_t chunkCount = header.u32(20u);
  const uint64_t tableOffset = header.u64(32u);
  metadata.totalUncompressedSize = header.u64(40u);
  metadata.sourceContentHash = header.u64(48u);
  metadata.buildSettingsHash = header.u64(56u);
  metadata.fileSize = header.u64(64u);
  if (header.u32(4u) != kVirtualSparseVoxelFileVersion || header.u32(8u) != kVirtualSparseVoxelFileEndianMarker ||
      header.u32(12u) != kVirtualSparseVoxelFileHeaderSize || header.u32(16u) != kVirtualSparseVoxelFileChunkRecordSize || header.u32(24u) != 0u || header.u32(28u) != 0u ||
      tableOffset != kVirtualSparseVoxelFileHeaderSize || chunkCount < kRequiredChunkCount || chunkCount > kMaximumChunkCount || metadata.fileSize != actualFileSize ||
      header.u64(72u) != 0u || header.u64(80u) != 0u || header.u64(88u) != 0u)
    return fail(error, "Virtual sparse voxel fixed header is invalid or unsupported");
  uint64_t tableSize = 0u;
  uint64_t tableEnd = 0u;
  if (!checkedMultiply(chunkCount, kVirtualSparseVoxelFileChunkRecordSize, tableSize) || !checkedAdd(tableOffset, tableSize, tableEnd) || tableEnd > actualFileSize)
    return fail(error, "Virtual sparse voxel chunk table is truncated or overflows");
  std::vector<uint8_t> tableBytes;
  if (!readBytes(file, tableOffset, tableSize, tableBytes, error))
    return false;
  const ByteReader table{tableBytes.data(), tableBytes.size()};
  std::set<uint32_t> types;
  std::vector<std::pair<uint64_t, uint64_t>> ranges;
  uint64_t totalUncompressed = 0u;
  metadata.chunks.reserve(chunkCount);
  for (uint32_t index = 0u; index < chunkCount; ++index)
  {
    const size_t offset = static_cast<size_t>(index) * kVirtualSparseVoxelFileChunkRecordSize;
    VirtualSparseVoxelFileChunkRecord chunk{};
    chunk.type = static_cast<VirtualSparseVoxelFileChunkType>(table.u32(offset));
    chunk.flags = table.u32(offset + 4u);
    chunk.fileOffset = table.u64(offset + 8u);
    chunk.storedSize = table.u64(offset + 16u);
    chunk.uncompressedSize = table.u64(offset + 24u);
    chunk.recordSize = table.u32(offset + 32u);
    chunk.recordCount = table.u64(offset + 40u);
    chunk.checksum = table.u64(offset + 48u);
    uint64_t recordBytes = 0u;
    uint64_t chunkEnd = 0u;
    if (!supportedChunkType(chunk.type) || chunk.flags != 0u || table.u32(offset + 36u) != 0u || table.u64(offset + 56u) != 0u ||
        !types.insert(static_cast<uint32_t>(chunk.type)).second || chunk.recordSize == 0u || !checkedMultiply(chunk.recordSize, chunk.recordCount, recordBytes) ||
        recordBytes != chunk.storedSize || chunk.storedSize != chunk.uncompressedSize || chunk.fileOffset % kVirtualSparseVoxelFileAlignment != 0u || chunk.fileOffset < tableEnd ||
        !checkedAdd(chunk.fileOffset, chunk.storedSize, chunkEnd) || chunkEnd > actualFileSize || !checkedAdd(totalUncompressed, chunk.uncompressedSize, totalUncompressed))
      return fail(error, "Virtual sparse voxel chunk record is invalid, duplicated, or out of range");
    ranges.emplace_back(chunk.fileOffset, chunkEnd);
    metadata.chunks.push_back(chunk);
  }
  for (uint32_t required = 1u; required <= kRequiredChunkCount; ++required)
    if (types.count(required) == 0u)
      return fail(error, "Virtual sparse voxel file is missing a required chunk");
  if (totalUncompressed != metadata.totalUncompressedSize)
    return fail(error, "Virtual sparse voxel total uncompressed size is inconsistent");
  std::sort(ranges.begin(), ranges.end());
  for (size_t index = 1u; index < ranges.size(); ++index)
    if (ranges[index].first < ranges[index - 1u].second)
      return fail(error, "Virtual sparse voxel chunks overlap");
  for (const auto &chunk : metadata.chunks)
    if (chunk.type != VirtualSparseVoxelFileChunkType::PagePayloads && !checksumFileRange(file, chunk.fileOffset, chunk.storedSize, chunk.checksum, error))
      return false;

  std::vector<uint8_t> sceneBytes;
  if (!readChunk(file, metadata, VirtualSparseVoxelFileChunkType::Scene, sceneBytes, error) || !parseSceneChunk(sceneBytes, metadata, error))
    return false;
  if (!fixedChunkLayout(metadata, VirtualSparseVoxelFileChunkType::Scene, 64u, 1u, error) ||
      !fixedChunkLayout(metadata, VirtualSparseVoxelFileChunkType::Models, 64u, metadata.modelCount, error) ||
      !fixedChunkLayout(metadata, VirtualSparseVoxelFileChunkType::Instances, 80u, metadata.instanceCount, error) ||
      !fixedChunkLayout(metadata, VirtualSparseVoxelFileChunkType::Pages, kVirtualSparseVoxelFilePageRecordSize, metadata.pageCount, error))
    return false;
  const VirtualSparseVoxelFileChunkRecord *dependencies = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageDependencies);
  const VirtualSparseVoxelFileChunkRecord *installs = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageInstallUpdates);
  const VirtualSparseVoxelFileChunkRecord *uninstalls = findChunk(metadata, VirtualSparseVoxelFileChunkType::PageUninstallUpdates);
  const VirtualSparseVoxelFileChunkRecord *payloads = findChunk(metadata, VirtualSparseVoxelFileChunkType::PagePayloads);
  const VirtualSparseVoxelFileChunkRecord *sourceInfo = findChunk(metadata, VirtualSparseVoxelFileChunkType::SourceInfo);
  if (dependencies->recordSize != 4u || installs->recordSize != 16u || uninstalls->recordSize != 16u || payloads->recordSize != 1u || sourceInfo->recordSize != 1u)
    return fail(error, "Virtual sparse voxel variable chunk record layout is invalid");
  const VirtualSparseVoxelFileChunkRecord *sourceIds = findChunk(metadata, VirtualSparseVoxelFileChunkType::SourceIds);
  metadata.hasSourceIds = sourceIds != nullptr;
  if (sourceIds != nullptr && (sourceIds->recordSize != 4u || sourceIds->recordCount != metadata.nodeCount))
    return fail(error, "Virtual sparse voxel SourceIds chunk layout is invalid");
  const VirtualSparseVoxelFileChunkRecord *materialReferences = findChunk(metadata, VirtualSparseVoxelFileChunkType::MaterialReferences);
  if (materialReferences != nullptr && materialReferences->recordSize != 1u)
    return fail(error, "Virtual sparse voxel MaterialReferences chunk layout is invalid");

  std::vector<uint8_t> modelBytes;
  std::vector<uint8_t> instanceBytes;
  std::vector<uint8_t> pageBytes;
  std::vector<uint8_t> dependencyBytes;
  std::vector<uint8_t> installBytes;
  std::vector<uint8_t> uninstallBytes;
  std::vector<uint8_t> sourceInfoBytes;
  if (!readChunk(file, metadata, VirtualSparseVoxelFileChunkType::Models, modelBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::Instances, instanceBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::Pages, pageBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::PageDependencies, dependencyBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::PageInstallUpdates, installBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::PageUninstallUpdates, uninstallBytes, error) ||
      !readChunk(file, metadata, VirtualSparseVoxelFileChunkType::SourceInfo, sourceInfoBytes, error) ||
      !parseModels(modelBytes, metadata, error) || !parseInstances(instanceBytes, metadata, error) || !parsePages(pageBytes, metadata, error) ||
      !parseDependenciesAndRewrites(dependencyBytes, installBytes, uninstallBytes, metadata, error) || !parseSourceInfo(sourceInfoBytes, metadata, error))
    return false;
  if (materialReferences != nullptr)
  {
    std::vector<uint8_t> materialReferenceBytes;
    if (!readChunk(file, metadata, VirtualSparseVoxelFileChunkType::MaterialReferences, materialReferenceBytes, error) ||
        !parseMaterialReferences(materialReferenceBytes, metadata, error))
      return false;
  }
  return true;
}

bool readVirtualSparseVoxelSceneMetadata(const std::string &path, VirtualSparseVoxelSceneMetadata &metadata, std::string *error)
{
  metadata = {};
  VirtualSparseVoxelSceneMetadata parsed;
  if (!readVirtualSparseVoxelSceneMetadataImpl(path, parsed, error))
    return false;
  metadata = std::move(parsed);
  return true;
}

bool readVirtualSparseVoxelPage(
    const std::string &path,
    const VirtualSparseVoxelSceneMetadata &metadata,
    const uint32_t pageIndex,
    VirtualSparseVoxelPageData &pageData,
    std::string *error)
{
  pageData = {};
  VirtualSparseVoxelPageData decodedPage;
  if (error != nullptr)
    error->clear();
  if (pageIndex >= metadata.pages.size())
    return fail(error, "Virtual sparse voxel page index is out of range");
  const VirtualSparseVoxelFileChunkRecord *payloadChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PagePayloads);
  if (payloadChunk == nullptr)
    return fail(error, "Virtual sparse voxel metadata has no page-payload chunk");
  const VirtualSparseVoxelFilePageRecord &page = metadata.pages[pageIndex];
  if (!rangeWithin(page.payloadOffset, page.storedSize, payloadChunk->storedSize))
    return fail(error, "Virtual sparse voxel page range is outside its payload chunk");
  std::ifstream file;
  uint64_t actualSize = 0u;
  if (!openFile(path, file, actualSize, error))
    return false;
  if (actualSize != metadata.fileSize)
    return fail(error, "Virtual sparse voxel file size changed after metadata was read");
  std::vector<uint8_t> stored;
  if (!readBytes(file, payloadChunk->fileOffset + page.payloadOffset, page.storedSize, stored, error))
    return false;
  if (page.uncompressedSize > std::numeric_limits<size_t>::max())
    return fail(error, "Virtual sparse voxel page exceeds addressable memory");
  std::vector<uint8_t> unpacked(static_cast<size_t>(page.uncompressedSize));
  if ((page.flags & kVirtualSparseVoxelPageCompressedLZ4) != 0u)
  {
    if (page.storedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()) || page.uncompressedSize > static_cast<uint64_t>(std::numeric_limits<int>::max()))
      return fail(error, "Virtual sparse voxel page exceeds LZ4 limits");
    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char *>(stored.data()),
        reinterpret_cast<char *>(unpacked.data()),
        static_cast<int>(stored.size()),
        static_cast<int>(unpacked.size()));
    if (decoded != static_cast<int>(unpacked.size()))
      return fail(error, "Failed to decompress virtual sparse voxel page");
  }
  else
    unpacked = std::move(stored);
  if (virtualSparseVoxelFileChecksum(unpacked.data(), unpacked.size()) != page.checksum)
    return fail(error, "Virtual sparse voxel page checksum mismatch");
  const ByteReader reader{unpacked.data(), unpacked.size()};
  const uint32_t nodesOffset = reader.u32(24u);
  const uint32_t pathsOffset = reader.u32(28u);
  const uint32_t centersOffset = reader.u32(32u);
  const uint32_t materialsOffset = reader.u32(36u);
  const uint64_t expectedNodesEnd = uint64_t{nodesOffset} + uint64_t{page.nodeCount} * sizeof(SparseVoxelLODNodeGPU);
  const uint64_t expectedPathsEnd = uint64_t{pathsOffset} + uint64_t{page.nodeCount} * sizeof(SparseVoxelPathLevelGPU);
  const uint64_t expectedCentersEnd = uint64_t{centersOffset} + uint64_t{page.nodeCount} * sizeof(SparseVoxelCenterLengthGPU);
  const uint64_t expectedMaterialsEnd = uint64_t{materialsOffset} + uint64_t{page.nodeCount} * sizeof(SparseVoxelMaterialGPU);
  if (unpacked.size() < kPagePayloadHeaderSize || reader.u32(0u) != kPagePayloadMagic || reader.u32(4u) != 1u || reader.u32(8u) != pageIndex ||
      reader.u32(12u) != page.modelIndex || reader.u32(16u) != page.firstNode || reader.u32(20u) != page.nodeCount || nodesOffset != kPagePayloadHeaderSize ||
      pathsOffset != expectedNodesEnd || centersOffset != expectedPathsEnd || materialsOffset != expectedCentersEnd || expectedMaterialsEnd != unpacked.size() ||
      reader.u32(40u) != sizeof(SparseVoxelLODNodeGPU) || reader.u32(44u) != sizeof(SparseVoxelPathLevelGPU) ||
      reader.u32(48u) != sizeof(SparseVoxelCenterLengthGPU) || reader.u32(52u) != sizeof(SparseVoxelMaterialGPU) || reader.u64(56u) != 0u)
    return fail(error, "Virtual sparse voxel page payload header is invalid");

  decodedPage.pageIndex = pageIndex;
  decodedPage.modelIndex = page.modelIndex;
  decodedPage.firstNode = page.firstNode;
  decodedPage.nodes.reserve(page.nodeCount);
  decodedPage.pathsLevels.reserve(page.nodeCount);
  decodedPage.centersLengths.reserve(page.nodeCount);
  decodedPage.materials.reserve(page.nodeCount);
  for (uint32_t index = 0u; index < page.nodeCount; ++index)
  {
    const size_t offset = nodesOffset + static_cast<size_t>(index) * 16u;
    SparseVoxelLODNodeGPU node{reader.u32(offset), reader.u32(offset + 4u), reader.u32(offset + 8u), reader.u32(offset + 12u)};
    if (node.childCount != 0u || node.voxelIndex != page.firstNode + index || node.level != page.minLevel)
      return fail(error, "Virtual sparse voxel streamed node metadata is invalid");
    decodedPage.nodes.push_back(node);
  }
  for (uint32_t index = 0u; index < page.nodeCount; ++index)
  {
    const size_t offset = pathsOffset + static_cast<size_t>(index) * 16u;
    SparseVoxelPathLevelGPU pathLevel{reader.u32(offset), reader.u32(offset + 4u), reader.u32(offset + 8u), reader.u32(offset + 12u)};
    const uint64_t path = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
    if (pathLevel.flags != 0u || pathLevel.level != page.minLevel || !isCanonicalOctreePath(path, pathLevel.level))
      return fail(error, "Virtual sparse voxel streamed path metadata is invalid");
    decodedPage.pathsLevels.push_back(pathLevel);
  }
  std::array<float, 3> computedBoundsMin{
    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
  std::array<float, 3> computedBoundsMax{
    -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
  const VirtualSparseVoxelFileModelRecord &model = metadata.models[page.modelIndex];
  const float rootHalf = model.rootExtent * 0.5f;
  for (uint32_t index = 0u; index < page.nodeCount; ++index)
  {
    const size_t offset = centersOffset + static_cast<size_t>(index) * 16u;
    SparseVoxelCenterLengthGPU center{{reader.f32(offset), reader.f32(offset + 4u), reader.f32(offset + 8u)}, reader.f32(offset + 12u)};
    if (!std::isfinite(center.center[0]) || !std::isfinite(center.center[1]) || !std::isfinite(center.center[2]) || !std::isfinite(center.length) || center.length <= 0.0f)
      return fail(error, "Virtual sparse voxel streamed center or length is invalid");
    const SparseVoxelPathLevelGPU &pathLevel = decodedPage.pathsLevels[index];
    const uint64_t path = static_cast<uint64_t>(pathLevel.pathLo) | (static_cast<uint64_t>(pathLevel.pathHi) << 32u);
    const std::array<uint32_t, 3> coordinate = decodeOctreePath(path, pathLevel.level);
    const float expectedLength = model.rootExtent / static_cast<float>(uint32_t{1} << pathLevel.level);
    const std::array<float, 3> expectedCenter{
      model.rootCenter[0] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * expectedLength,
      model.rootCenter[1] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * expectedLength,
      model.rootCenter[2] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * expectedLength,
    };
    if (center.length != expectedLength || center.center[0] != expectedCenter[0] || center.center[1] != expectedCenter[1] || center.center[2] != expectedCenter[2])
      return fail(error, "Virtual sparse voxel streamed center or length disagrees with its canonical path");
    const float halfLength = center.length * 0.5f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float rootMin = model.rootCenter[axis] - rootHalf;
      const float rootMax = model.rootCenter[axis] + rootHalf;
      const float nodeMin = center.center[axis] - halfLength;
      const float nodeMax = center.center[axis] + halfLength;
      computedBoundsMin[axis] = std::min(computedBoundsMin[axis], std::max(rootMin, std::nextafter(nodeMin, -std::numeric_limits<float>::infinity())));
      computedBoundsMax[axis] = std::max(computedBoundsMax[axis], std::min(rootMax, std::nextafter(nodeMax, std::numeric_limits<float>::infinity())));
    }
    decodedPage.centersLengths.push_back(center);
  }
  if (computedBoundsMin != page.localBoundsMin || computedBoundsMax != page.localBoundsMax)
    return fail(error, "Virtual sparse voxel page local bounds do not exactly cover its nodes");
  for (uint32_t index = 0u; index < page.nodeCount; ++index)
  {
    const size_t offset = materialsOffset + static_cast<size_t>(index) * 32u;
    const SparseVoxelMaterialGPU material{reader.u32(offset), reader.u32(offset + 4u), reader.u32(offset + 8u), reader.u32(offset + 12u),
                                          reader.u32(offset + 16u), reader.u32(offset + 20u), reader.u32(offset + 24u), reader.u32(offset + 28u)};
    if ((material.albedoRGBA8 >> 24u) != 0xffu || (material.roughnessMetallicUNorm8 & 0xffff0000u) != 0u || (material.emissiveB16F & 0xffff0000u) != 0u ||
        !finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveRG16F)) || !finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveRG16F >> 16u)) ||
        !finiteNonNegativeHalf(static_cast<uint16_t>(material.emissiveB16F)) ||
        material.flags != 0u ||
        !sparseVoxelMaterialSecondaryLayerIsCanonical(material))
      return fail(error, "Virtual sparse voxel streamed material is invalid");
    decodedPage.materials.push_back(material);
  }
  pageData = std::move(decodedPage);
  return true;
}

bool readVirtualSparseVoxelScene(const std::string &path, SparseVoxelScene &scene, std::string *error)
{
  scene = {};
  SparseVoxelScene loaded;
  VirtualSparseVoxelSceneMetadata metadata;
  if (!readVirtualSparseVoxelSceneMetadata(path, metadata, error))
    return false;
  {
    std::ifstream file;
    uint64_t actualSize = 0u;
    if (!openFile(path, file, actualSize, error))
      return false;
    const VirtualSparseVoxelFileChunkRecord *payloadChunk = findChunk(metadata, VirtualSparseVoxelFileChunkType::PagePayloads);
    if (payloadChunk == nullptr || !checksumFileRange(file, payloadChunk->fileOffset, payloadChunk->storedSize, payloadChunk->checksum, error))
      return false;
  }
  loaded.sourceInfo = metadata.sourceInfo;
  loaded.voxelMaterialReferences = metadata.voxelMaterialReferences;
  loaded.sourceContentHash = metadata.sourceContentHash;
  loaded.buildSettingsHash = metadata.buildSettingsHash;
  loaded.tlasFanout = metadata.tlasFanout;
  loaded.tlasLeafSize = metadata.tlasLeafSize;
  loaded.models.resize(metadata.modelCount);
  for (uint32_t modelIndex = 0u; modelIndex < metadata.modelCount; ++modelIndex)
  {
    const VirtualSparseVoxelFileModelRecord &record = metadata.models[modelIndex];
    SparseVoxelBLAS &model = loaded.models[modelIndex];
    model.rootCenter = record.rootCenter;
    model.rootExtent = record.rootExtent;
    model.maxLevel = record.maxLevel;
    model.flags = record.flags;
    model.firstRoot = record.firstRoot - record.firstNode;
    model.rootCount = record.rootCount;
    model.hierarchyNodes.resize(record.nodeCount);
    model.octreePaths.resize(record.nodeCount);
    model.octreeLevels.resize(record.nodeCount);
    model.centersLengths.resize(record.nodeCount);
    model.materials.resize(record.nodeCount);
    if (metadata.hasSourceIds)
      model.sourceIds.resize(record.nodeCount);
  }
  loaded.instances.reserve(metadata.instanceCount);
  for (const auto &record : metadata.instances)
    loaded.instances.push_back({record.blasIndex, record.flags, record.modelToWorld});

  for (uint32_t pageIndex = 0u; pageIndex < metadata.pageCount; ++pageIndex)
  {
    VirtualSparseVoxelPageData page;
    if (!readVirtualSparseVoxelPage(path, metadata, pageIndex, page, error))
      return false;
    const VirtualSparseVoxelFileModelRecord &record = metadata.models[page.modelIndex];
    SparseVoxelBLAS &model = loaded.models[page.modelIndex];
    const uint32_t localFirst = page.firstNode - record.firstNode;
    for (uint32_t index = 0u; index < page.nodes.size(); ++index)
    {
      SparseVoxelLODNodeGPU node = page.nodes[index];
      if (node.firstChild != 0u)
      {
        if (node.firstChild < record.firstNode || node.firstChild >= record.firstNode + record.nodeCount)
          return fail(error, "Virtual sparse voxel node child pointer crosses model boundaries");
        node.firstChild -= record.firstNode;
      }
      node.voxelIndex -= record.firstNode;
      model.hierarchyNodes[localFirst + index] = node;
      model.octreePaths[localFirst + index] = static_cast<uint64_t>(page.pathsLevels[index].pathLo) | (static_cast<uint64_t>(page.pathsLevels[index].pathHi) << 32u);
      model.octreeLevels[localFirst + index] = static_cast<uint8_t>(page.pathsLevels[index].level);
      model.centersLengths[localFirst + index] = page.centersLengths[index];
      model.materials[localFirst + index] = page.materials[index];
    }
  }
  for (const VirtualSparseVoxelPageRewrite &update : metadata.installUpdates)
  {
    uint32_t modelIndex = 0u;
    if (!modelForGlobalNode(metadata, update.nodeIndex, modelIndex))
      return fail(error, "Virtual sparse voxel install rewrite references no model");
    const auto &record = metadata.models[modelIndex];
    if (update.firstChild < record.firstNode || !rangeWithin(update.firstChild, update.childCount, uint64_t{record.firstNode} + record.nodeCount))
      return fail(error, "Virtual sparse voxel install rewrite crosses model boundaries");
    SparseVoxelLODNodeGPU &node = loaded.models[modelIndex].hierarchyNodes[update.nodeIndex - record.firstNode];
    if (node.firstChild != update.firstChild - record.firstNode)
      return fail(error, "Virtual sparse voxel install rewrite disagrees with page node metadata");
    node.childCount = update.childCount;
  }

  if (metadata.hasSourceIds)
  {
    std::ifstream file;
    uint64_t actualSize = 0u;
    if (!openFile(path, file, actualSize, error))
      return false;
    std::vector<uint8_t> sourceIdBytes;
    if (!readChunk(file, metadata, VirtualSparseVoxelFileChunkType::SourceIds, sourceIdBytes, error))
      return false;
    const ByteReader reader{sourceIdBytes.data(), sourceIdBytes.size()};
    for (uint32_t modelIndex = 0u; modelIndex < metadata.modelCount; ++modelIndex)
    {
      const auto &record = metadata.models[modelIndex];
      for (uint32_t index = 0u; index < record.nodeCount; ++index)
        loaded.models[modelIndex].sourceIds[index] = reader.u32(static_cast<size_t>(record.firstNode + index) * 4u);
    }
  }
  std::string validationError;
  if (!validateSparseVoxelSceneSource(loaded, &validationError))
    return fail(error, "Loaded virtual sparse voxel scene is invalid: " + validationError);
  scene = std::move(loaded);
  return true;
}

} // namespace voxel::svraster
