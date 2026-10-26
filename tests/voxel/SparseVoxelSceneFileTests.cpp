#include "voxel/svraster/SparseVoxelGenerator.hpp"
#include "voxel/svraster/SparseVoxelScene.hpp"
#include "voxel/svraster/SparseVoxelSceneFile.hpp"

#include "lz4.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#undef assert
#define assert(condition)                                                                                                                                        \
  do                                                                                                                                                             \
  {                                                                                                                                                              \
    if (!(condition))                                                                                                                                            \
    {                                                                                                                                                            \
      std::fprintf(stderr, "test assertion failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__);                                                           \
      std::abort();                                                                                                                                              \
    }                                                                                                                                                            \
  } while (false)

namespace
{

using namespace voxel::svraster;

uint32_t readU32(const std::vector<uint8_t> &bytes, const size_t offset)
{
  assert(offset + 4u <= bytes.size());
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
         (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) | (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint64_t readU64(const std::vector<uint8_t> &bytes, const size_t offset)
{
  assert(offset + 8u <= bytes.size());
  uint64_t value = 0u;
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    value |= static_cast<uint64_t>(bytes[offset + byte]) << (byte * 8u);
  return value;
}

float readF32(const std::vector<uint8_t> &bytes, const size_t offset)
{
  const uint32_t bits = readU32(bytes, offset);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::array<float, 16> identityMatrix()
{
  return {1.0f, 0.0f, 0.0f, 0.0f,
          0.0f, 1.0f, 0.0f, 0.0f,
          0.0f, 0.0f, 1.0f, 0.0f,
          0.0f, 0.0f, 0.0f, 1.0f};
}

SparseVoxelCenterLengthGPU centerLength(const SparseVoxelBLAS &model, const uint64_t path, const uint32_t level)
{
  const auto coordinate = decodeOctreePath(path, level);
  const float length = model.rootExtent / static_cast<float>(uint32_t{1} << level);
  return {{
            model.rootCenter[0] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[0]) + 0.5f) * length,
            model.rootCenter[1] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[1]) + 0.5f) * length,
            model.rootCenter[2] - model.rootExtent * 0.5f + (static_cast<float>(coordinate[2]) + 0.5f) * length,
          },
          length};
}

SparseVoxelScene tinyScene(const bool sourceIds = false)
{
  SparseVoxelScene scene{};
  SparseVoxelBLAS model{};
  model.rootCenter = {0.25f, -0.5f, 1.0f};
  model.rootExtent = 4.0f;
  model.maxLevel = 2u;
  model.firstRoot = 0u;
  model.rootCount = 8u;
  SparseVoxelMaterialGPU material{};
  material.albedoRGBA8 = 0xff604020u;
  material.roughnessMetallicUNorm8 = 0x8040u;
  material.emissiveRG16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(1.0f));
  material.emissiveB16F = static_cast<uint32_t>(floatToHalfRoundToNearestEven(0.5f));
  material.materialId = 7u;
  material.reserved0 = 0xff203050u;
  material.reserved1 =
      kSparseVoxelMaterialBlendTopBottomFaces |
      kSparseVoxelMaterialBlendSideFaces;

  for (uint32_t level = 1u; level <= 2u; ++level)
  {
    const uint32_t resolution = 1u << level;
    std::vector<uint64_t> paths;
    for (uint32_t z = 0u; z < resolution; ++z)
      for (uint32_t y = 0u; y < resolution; ++y)
        for (uint32_t x = 0u; x < resolution; ++x)
          paths.push_back(encodeOctreePath(x, y, z, level));
    std::sort(paths.begin(), paths.end());
    for (const uint64_t path : paths)
    {
      const uint32_t nodeIndex = static_cast<uint32_t>(model.hierarchyNodes.size());
      const bool parent = level == 1u;
      model.hierarchyNodes.push_back({parent ? 8u + nodeIndex * 8u : 0u, parent ? 8u : 0u, nodeIndex, level});
      model.octreePaths.push_back(path);
      model.octreeLevels.push_back(static_cast<uint8_t>(level));
      model.centersLengths.push_back(centerLength(model, path, level));
      model.materials.push_back(material);
      if (sourceIds)
        model.sourceIds.push_back(1000u + nodeIndex);
    }
  }
  scene.models.push_back(std::move(model));
  scene.instances.push_back({0u, 0u, identityMatrix()});
  scene.sourceInfo = {17u, "fixtures/tiny.obj", "max-level=2;solid=1"};
  scene.sourceContentHash = 0x0123456789abcdefull;
  scene.buildSettingsHash = 0xfedcba9876543210ull;
  return scene;
}

struct ChunkRecord
{
  uint32_t type = 0u;
  uint32_t flags = 0u;
  uint64_t offset = 0u;
  uint64_t storedSize = 0u;
  uint64_t uncompressedSize = 0u;
  uint32_t recordSize = 0u;
  uint64_t recordCount = 0u;
  uint64_t checksum = 0u;
};

std::vector<ChunkRecord> chunks(const std::vector<uint8_t> &bytes)
{
  const uint32_t count = readU32(bytes, 20u);
  const uint64_t tableOffset = readU64(bytes, 32u);
  std::vector<ChunkRecord> result;
  for (uint32_t index = 0u; index < count; ++index)
  {
    const size_t offset = static_cast<size_t>(tableOffset + uint64_t{index} * kVirtualSparseVoxelFileChunkRecordSize);
    result.push_back({
      readU32(bytes, offset),
      readU32(bytes, offset + 4u),
      readU64(bytes, offset + 8u),
      readU64(bytes, offset + 16u),
      readU64(bytes, offset + 24u),
      readU32(bytes, offset + 32u),
      readU64(bytes, offset + 40u),
      readU64(bytes, offset + 48u),
    });
    assert(readU32(bytes, offset + 36u) == 0u);
    assert(readU64(bytes, offset + 56u) == 0u);
  }
  return result;
}

std::vector<uint8_t> chunkPayload(const std::vector<uint8_t> &bytes, const ChunkRecord &chunk)
{
  assert(chunk.flags == 0u);
  assert(chunk.storedSize == chunk.uncompressedSize);
  assert(chunk.offset + chunk.storedSize <= bytes.size());
  return std::vector<uint8_t>(bytes.begin() + chunk.offset, bytes.begin() + chunk.offset + chunk.storedSize);
}

struct PageRecord
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

std::vector<PageRecord> pageRecords(const std::vector<uint8_t> &bytes, const std::vector<ChunkRecord> &records)
{
  const auto payload = chunkPayload(bytes, records[3]);
  std::vector<PageRecord> pages;
  for (uint32_t pageIndex = 0u; pageIndex < records[3].recordCount; ++pageIndex)
  {
    const size_t offset = pageIndex * kVirtualSparseVoxelFilePageRecordSize;
    pages.push_back({
      readU32(payload, offset),
      readU32(payload, offset + 4u),
      static_cast<uint32_t>(readU64(payload, offset + 8u)),
      readU32(payload, offset + 16u),
      readU32(payload, offset + 20u),
      readU32(payload, offset + 24u),
      readU32(payload, offset + 28u),
      readU64(payload, offset + 32u),
      readU64(payload, offset + 40u),
      readU32(payload, offset + 48u),
      readU32(payload, offset + 52u),
      readU64(payload, offset + 56u),
      readU64(payload, offset + 64u),
      readU64(payload, offset + 72u),
      readU64(payload, offset + 80u),
      readU64(payload, offset + 88u),
      {readF32(payload, offset + 96u), readF32(payload, offset + 100u), readF32(payload, offset + 104u)},
      {readF32(payload, offset + 108u), readF32(payload, offset + 112u), readF32(payload, offset + 116u)},
    });
    assert(readU64(payload, offset + 120u) == 0u);
  }
  return pages;
}

std::vector<uint8_t> unpackPage(const std::vector<uint8_t> &bytes, const std::vector<ChunkRecord> &records, const PageRecord &page)
{
  const ChunkRecord &payloadChunk = records[7];
  assert(page.payloadOffset + page.storedSize <= payloadChunk.storedSize);
  const uint8_t *stored = bytes.data() + payloadChunk.offset + page.payloadOffset;
  std::vector<uint8_t> result(page.uncompressedSize);
  if ((page.flags & kVirtualSparseVoxelPageCompressedLZ4) == 0u)
  {
    assert(page.storedSize == page.uncompressedSize);
    std::memcpy(result.data(), stored, result.size());
  }
  else
  {
    assert(page.storedSize <= static_cast<uint64_t>(INT32_MAX) && page.uncompressedSize <= static_cast<uint64_t>(INT32_MAX));
    const int decoded = LZ4_decompress_safe(
        reinterpret_cast<const char *>(stored),
        reinterpret_cast<char *>(result.data()),
        static_cast<int>(page.storedSize),
        static_cast<int>(page.uncompressedSize));
    assert(decoded == static_cast<int>(page.uncompressedSize));
  }
  assert(page.checksum == virtualSparseVoxelFileChecksum(result.data(), result.size()));
  return result;
}

std::pair<std::array<float, 3>, std::array<float, 3>> expectedPageBounds(
    const SparseVoxelScene &scene,
    const PageRecord &page)
{
  uint32_t modelBase = 0u;
  for (uint32_t modelIndex = 0u; modelIndex < page.modelIndex; ++modelIndex)
    modelBase += static_cast<uint32_t>(scene.models[modelIndex].hierarchyNodes.size());
  const SparseVoxelBLAS &model = scene.models[page.modelIndex];
  const uint32_t localFirst = page.firstNode - modelBase;
  const float rootHalf = model.rootExtent * 0.5f;
  std::array<float, 3> boundsMin{
    std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
  std::array<float, 3> boundsMax{
    -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const SparseVoxelCenterLengthGPU &center = model.centersLengths[localFirst + nodeOffset];
    const float halfLength = center.length * 0.5f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float rootMin = model.rootCenter[axis] - rootHalf;
      const float rootMax = model.rootCenter[axis] + rootHalf;
      boundsMin[axis] = std::min(boundsMin[axis], std::max(rootMin, std::nextafter(center.center[axis] - halfLength, -std::numeric_limits<float>::infinity())));
      boundsMax[axis] = std::max(boundsMax[axis], std::min(rootMax, std::nextafter(center.center[axis] + halfLength, std::numeric_limits<float>::infinity())));
    }
  }
  return {boundsMin, boundsMax};
}

void testGoldenUncompressedLayout()
{
  const SparseVoxelScene scene = tinyScene();
  std::vector<uint8_t> bytes;
  std::string error;
  assert(serializeVirtualSparseVoxelScene(scene, {VirtualSparseVoxelFileCompression::None}, bytes, &error));
  assert(error.empty());
  assert(std::memcmp(bytes.data(), "VSVO", 4u) == 0);
  assert(readU32(bytes, 4u) == kVirtualSparseVoxelFileVersion);
  assert(readU32(bytes, 8u) == kVirtualSparseVoxelFileEndianMarker);
  assert(readU32(bytes, 12u) == kVirtualSparseVoxelFileHeaderSize);
  assert(readU32(bytes, 16u) == kVirtualSparseVoxelFileChunkRecordSize);
  assert(readU32(bytes, 20u) == 9u);
  assert(readU64(bytes, 32u) == kVirtualSparseVoxelFileHeaderSize);
  assert(readU64(bytes, 40u) == 6680u);
  assert(readU64(bytes, 48u) == scene.sourceContentHash);
  assert(readU64(bytes, 56u) == scene.buildSettingsHash);
  assert(readU64(bytes, 64u) == 7376u && bytes.size() == 7376u);
  assert(readU64(bytes, 72u) == 0u && readU64(bytes, 80u) == 0u && readU64(bytes, 88u) == 0u);

  const std::array<uint32_t, 9> expectedRecordSizes = {64u, 64u, 80u, kVirtualSparseVoxelFilePageRecordSize, 4u, 16u, 16u, 1u, 1u};
  const std::array<uint64_t, 9> expectedRecordCounts = {1u, 1u, 1u, 2u, 1u, 8u, 8u, 5888u, 68u};
  const std::array<uint64_t, 9> expectedOffsets = {672u, 736u, 800u, 880u, 1136u, 1152u, 1280u, 1408u, 7296u};
  const auto records = chunks(bytes);
  uint64_t totalUncompressed = 0u;
  for (uint32_t index = 0u; index < records.size(); ++index)
  {
    const ChunkRecord &record = records[index];
    assert(record.type == index + 1u);
    assert(record.flags == 0u);
    assert(record.recordSize == expectedRecordSizes[index]);
    assert(record.recordCount == expectedRecordCounts[index]);
    assert(record.offset == expectedOffsets[index]);
    assert(record.offset % kVirtualSparseVoxelFileAlignment == 0u);
    const auto payload = chunkPayload(bytes, record);
    assert(record.checksum == virtualSparseVoxelFileChecksum(payload.data(), payload.size()));
    totalUncompressed += payload.size();
  }
  assert(totalUncompressed == readU64(bytes, 40u));

  const auto sceneChunk = chunkPayload(bytes, records[0]);
  assert(readU32(sceneChunk, 4u) == 1u && readU32(sceneChunk, 8u) == 1u);
  assert(readU64(sceneChunk, 16u) == 72u);
  assert(readU32(sceneChunk, 24u) == 2u);

  const auto pages = pageRecords(bytes, records);
  assert(pages.size() == 2u);
  const std::array<float, 3> expectedBoundsMin{-1.75f, -2.5f, -1.0f};
  const std::array<float, 3> expectedBoundsMax{2.25f, 1.5f, 3.0f};
  assert(pages[0].flags == kVirtualSparseVoxelPageRoot);
  assert(pages[0].firstNode == 0u && pages[0].nodeCount == 8u && pages[0].dependencyCount == 0u);
  assert(pages[0].localBoundsMin == expectedBoundsMin);
  assert(pages[0].localBoundsMax == expectedBoundsMax);
  assert(pages[0].payloadOffset == 0u && pages[0].storedSize == 704u && pages[0].uncompressedSize == 704u);
  assert(pages[1].flags == 0u);
  assert(pages[1].firstNode == 8u && pages[1].nodeCount == 64u && pages[1].dependencyCount == 1u);
  assert(pages[1].installCount == 8u && pages[1].uninstallCount == 8u);
  assert(pages[1].payloadOffset == 704u && pages[1].storedSize == 5184u && pages[1].uncompressedSize == 5184u);
  for (uint32_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
  {
    const auto payload = unpackPage(bytes, records, pages[pageIndex]);
    assert(readU32(payload, 0u) == 0x47505356u);
    assert(readU32(payload, 8u) == pageIndex);
    assert(readU32(payload, 16u) == pages[pageIndex].firstNode);
    assert(readU32(payload, 20u) == pages[pageIndex].nodeCount);
    const uint32_t nodesOffset = readU32(payload, 24u);
    for (uint32_t node = 0u; node < pages[pageIndex].nodeCount; ++node)
      assert(readU32(payload, nodesOffset + node * sizeof(SparseVoxelLODNodeGPU) + 4u) == 0u);
  }
}

std::vector<VirtualSparseVoxelPageRewrite> rewrites(
    const std::vector<uint8_t> &bytes,
    const ChunkRecord &chunk,
    const uint64_t first,
    const uint32_t count)
{
  const auto payload = chunkPayload(bytes, chunk);
  std::vector<VirtualSparseVoxelPageRewrite> result;
  for (uint32_t index = 0u; index < count; ++index)
  {
    const size_t offset = static_cast<size_t>((first + index) * sizeof(VirtualSparseVoxelPageRewrite));
    result.push_back({readU32(payload, offset), readU32(payload, offset + 4u), readU32(payload, offset + 8u), readU32(payload, offset + 12u)});
  }
  return result;
}

void validateResidentCut(
    const SparseVoxelScene &scene,
    const std::vector<bool> &resident,
    const std::vector<uint32_t> &childCounts,
    const uint32_t expectedSelected)
{
  const SparseVoxelBLAS &model = scene.models[0];
  std::vector<uint32_t> stack;
  for (uint32_t root = 0u; root < model.rootCount; ++root)
    stack.push_back(model.firstRoot + root);
  std::set<uint32_t> selected;
  while (!stack.empty())
  {
    const uint32_t node = stack.back();
    stack.pop_back();
    assert(resident[node]);
    if (childCounts[node] == 0u)
    {
      assert(selected.insert(node).second);
      continue;
    }
    const SparseVoxelLODNodeGPU &authored = model.hierarchyNodes[node];
    assert(childCounts[node] == authored.childCount);
    for (uint32_t child = 0u; child < authored.childCount; ++child)
      stack.push_back(authored.firstChild + child);
  }
  assert(selected.size() == expectedSelected);
}

void testPageRewritePrefixesAndDependencies()
{
  const SparseVoxelScene scene = tinyScene();
  std::vector<uint8_t> bytes;
  const VirtualSparseVoxelSceneWriteOptions options{VirtualSparseVoxelFileCompression::None, 704u};
  assert(serializeVirtualSparseVoxelScene(scene, options, bytes));
  const auto records = chunks(bytes);
  const auto pages = pageRecords(bytes, records);
  assert(pages.size() == 9u);
  const auto dependencyWords = chunkPayload(bytes, records[4]);
  std::vector<bool> installed(pages.size(), false);
  std::vector<bool> resident(72u, false);
  std::vector<uint32_t> childCounts(72u, 0u);
  for (uint32_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
  {
    const PageRecord &page = pages[pageIndex];
    for (uint32_t dependency = 0u; dependency < page.dependencyCount; ++dependency)
    {
      const uint32_t dependencyPage = readU32(dependencyWords, static_cast<size_t>((page.dependencyFirst + dependency) * sizeof(uint32_t)));
      assert(dependencyPage < pageIndex && installed[dependencyPage]);
    }
    for (uint32_t node = 0u; node < page.nodeCount; ++node)
      resident[page.firstNode + node] = true;
    for (const auto &update : rewrites(bytes, records[5], page.installFirst, page.installCount))
    {
      assert(resident[update.nodeIndex]);
      childCounts[update.nodeIndex] = update.childCount;
    }
    installed[pageIndex] = true;
    validateResidentCut(scene, resident, childCounts, 8u + 7u * pageIndex);
  }
  for (uint32_t pageIndex = static_cast<uint32_t>(pages.size()); pageIndex-- > 1u;)
  {
    const PageRecord &page = pages[pageIndex];
    for (const auto &update : rewrites(bytes, records[6], page.uninstallFirst, page.uninstallCount))
      childCounts[update.nodeIndex] = update.childCount;
    for (uint32_t node = 0u; node < page.nodeCount; ++node)
      resident[page.firstNode + node] = false;
    installed[pageIndex] = false;
    validateResidentCut(scene, resident, childCounts, 8u + 7u * (pageIndex - 1u));
  }
}

void testDeterministicIndependentLZ4Pages()
{
  const SparseVoxelScene scene = tinyScene();
  std::vector<uint8_t> none;
  std::vector<uint8_t> compressedA;
  std::vector<uint8_t> compressedB;
  assert(serializeVirtualSparseVoxelScene(scene, {VirtualSparseVoxelFileCompression::None}, none));
  assert(serializeVirtualSparseVoxelScene(scene, {VirtualSparseVoxelFileCompression::LZ4}, compressedA));
  assert(serializeVirtualSparseVoxelScene(scene, {VirtualSparseVoxelFileCompression::LZ4}, compressedB));
  assert(compressedA == compressedB);
  assert(compressedA.size() < none.size());
  const auto noneRecords = chunks(none);
  const auto compressedRecords = chunks(compressedA);
  const auto nonePages = pageRecords(none, noneRecords);
  const auto compressedPages = pageRecords(compressedA, compressedRecords);
  assert(nonePages.size() == compressedPages.size());
  for (uint32_t pageIndex = 0u; pageIndex < nonePages.size(); ++pageIndex)
  {
    assert((compressedPages[pageIndex].flags & kVirtualSparseVoxelPageCompressedLZ4) != 0u);
    assert(unpackPage(none, noneRecords, nonePages[pageIndex]) == unpackPage(compressedA, compressedRecords, compressedPages[pageIndex]));
  }
}

void testMultiModelLevelOrderingAndPageLimits()
{
  SparseVoxelScene scene = tinyScene();
  scene.models.push_back(scene.models.front());
  scene.instances.push_back({1u, 0u, identityMatrix()});
  std::string error;
  assert(validateSparseVoxelSceneSource(scene, &error));

  const VirtualSparseVoxelSceneWriteOptions options{VirtualSparseVoxelFileCompression::None, 704u};
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> repeat;
  assert(serializeVirtualSparseVoxelScene(scene, options, bytes, &error));
  assert(serializeVirtualSparseVoxelScene(scene, options, repeat, &error));
  assert(bytes == repeat);
  const auto records = chunks(bytes);
  const auto pages = pageRecords(bytes, records);
  const auto dependencyWords = chunkPayload(bytes, records[4]);
  assert(pages.size() == 18u);

  std::array<uint32_t, 2> expectedFirstNode{0u, 72u};
  uint32_t previousLevel = 0u;
  for (uint32_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
  {
    const PageRecord &page = pages[pageIndex];
    assert(page.modelIndex < scene.models.size());
    assert(page.minLevel == page.maxLevel);
    assert(page.minLevel >= previousLevel);
    assert(page.firstNode == expectedFirstNode[page.modelIndex]);
    expectedFirstNode[page.modelIndex] += page.nodeCount;
    previousLevel = page.minLevel;
    const auto expectedBounds = expectedPageBounds(scene, page);
    assert(page.localBoundsMin == expectedBounds.first && page.localBoundsMax == expectedBounds.second);
    if ((page.flags & kVirtualSparseVoxelPageRoot) != 0u)
    {
      assert(page.minLevel == 1u && page.dependencyCount == 0u && page.installCount == 0u);
    }
    else
    {
      assert(page.dependencyCount == 1u && page.installCount == 1u && page.uninstallCount == 1u);
      const uint32_t dependency = readU32(dependencyWords, static_cast<size_t>(page.dependencyFirst * sizeof(uint32_t)));
      assert(dependency < pageIndex);
      assert(pages[dependency].modelIndex == page.modelIndex);
      assert(pages[dependency].maxLevel + 1u == page.minLevel);
    }
  }
  assert(expectedFirstNode[0] == 72u && expectedFirstNode[1] == 144u);

  assert(!serializeVirtualSparseVoxelScene(scene, {VirtualSparseVoxelFileCompression::None, 703u}, bytes, &error));
  assert(bytes.empty());
  assert(error.find("sibling group") != std::string::npos);
  assert(!serializeVirtualSparseVoxelScene(scene, {static_cast<VirtualSparseVoxelFileCompression>(99u), 704u}, bytes, &error));
  assert(bytes.empty());
  assert(error.find("compression") != std::string::npos);
}

std::vector<uint8_t> readFile(const std::filesystem::path &path)
{
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  assert(file);
  const std::streamsize size = file.tellg();
  file.seekg(0, std::ios::beg);
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.read(reinterpret_cast<char *>(bytes.data()), size);
  assert(file);
  return bytes;
}

void testVSVOAtomicWriteAndValidation()
{
  const std::filesystem::path path = std::filesystem::temp_directory_path() / "VirtualSparseVoxelSceneG14.vsvo";
  std::filesystem::remove(path);
  std::filesystem::remove(path.string() + ".tmp");
  SparseVoxelScene scene = tinyScene(true);
  std::string error;
  assert(writeVirtualSparseVoxelScene(path.string(), scene, {}, &error));
  const auto first = readFile(path);
  const auto records = chunks(first);
  assert(records.size() == 10u);
  assert(records.back().type == static_cast<uint32_t>(VirtualSparseVoxelFileChunkType::SourceIds));
  const auto sourceIds = chunkPayload(first, records.back());
  assert(records.back().recordCount == 72u && readU32(sourceIds, 0u) == 1000u && readU32(sourceIds, sourceIds.size() - 4u) == 1071u);
  assert(writeVirtualSparseVoxelScene(path.string(), scene, {}, &error));
  assert(first == readFile(path));
  assert(std::memcmp(first.data(), "VSVO", 4u) == 0);
  std::filesystem::remove(path);

  scene.models[0].materials[0].albedoRGBA8 &= 0x00ffffffu;
  std::vector<uint8_t> bytes;
  assert(!serializeVirtualSparseVoxelScene(scene, {}, bytes, &error));
  assert(bytes.empty());
  assert(error.find("invalid virtual sparse voxel scene") != std::string::npos);

  SparseVoxelScene invalidCenter = tinyScene();
  invalidCenter.models[0].centersLengths[0].center[0] += 0.125f;
  assert(!serializeVirtualSparseVoxelScene(invalidCenter, {}, bytes, &error));
  assert(bytes.empty());
  assert(error.find("canonical path") != std::string::npos);
}

void testGeneratedExteriorShellHierarchyIsPaged()
{
  const SparseVoxelScene scene = makeExteriorShellScene(kDefaultExteriorShellMaterialSeed);
  std::string error;
  assert(validateSparseVoxelSceneSource(scene, &error));
  assert(scene.models.size() == 1u && scene.models[0].hierarchyNodes.size() > kExteriorShellVoxelCount);
  std::vector<uint8_t> bytes;
  assert(serializeVirtualSparseVoxelScene(scene, {}, bytes, &error));
  const auto records = chunks(bytes);
  const auto pages = pageRecords(bytes, records);
  assert(pages.size() > kExteriorShellLevel);
  assert((pages.front().flags & kVirtualSparseVoxelPageRoot) != 0u);
  assert(pages.front().nodeCount == 8u && pages.front().maxLevel == 1u);
  uint64_t pagedNodes = 0u;
  uint32_t previousLevel = 0u;
  uint32_t expectedFirstNode = 0u;
  for (const PageRecord &page : pages)
  {
    const auto expectedBounds = expectedPageBounds(scene, page);
    assert(page.minLevel == page.maxLevel);
    assert(page.minLevel >= previousLevel);
    assert(page.firstNode == expectedFirstNode);
    assert(page.localBoundsMin == expectedBounds.first);
    assert(page.localBoundsMax == expectedBounds.second);
    previousLevel = page.minLevel;
    expectedFirstNode += page.nodeCount;
    pagedNodes += page.nodeCount;
    const auto payload = unpackPage(bytes, records, page);
    assert(readU32(payload, 20u) == page.nodeCount);
  }
  assert(pagedNodes == scene.models[0].hierarchyNodes.size());

  const auto dependencyWords = chunkPayload(bytes, records[4]);
  std::vector<bool> wanted(pages.size(), false);
  const auto includeDependencies = [&](const auto &self, const uint32_t pageIndex) -> void {
    if (wanted[pageIndex])
      return;
    wanted[pageIndex] = true;
    const PageRecord &page = pages[pageIndex];
    for (uint32_t dependency = 0u; dependency < page.dependencyCount; ++dependency)
      self(self, readU32(dependencyWords, static_cast<size_t>((page.dependencyFirst + dependency) * 4u)));
  };
  includeDependencies(includeDependencies, 0u);
  for (uint32_t pageIndex = 1u; pageIndex < pages.size(); ++pageIndex)
    if (pageIndex % 3u == 1u || pageIndex + 1u == pages.size())
      includeDependencies(includeDependencies, pageIndex);

  std::vector<bool> installed(pages.size(), false);
  std::vector<bool> resident(scene.models[0].hierarchyNodes.size(), false);
  std::vector<uint32_t> childCounts(scene.models[0].hierarchyNodes.size(), 0u);
  std::vector<uint32_t> installOrder;
  uint32_t expectedSelected = 0u;
  for (;;)
  {
    uint32_t readyPage = kInvalidSparseVoxelIndex;
    for (uint32_t pageIndex = static_cast<uint32_t>(pages.size()); pageIndex-- > 0u;)
    {
      if (!wanted[pageIndex] || installed[pageIndex])
        continue;
      const PageRecord &page = pages[pageIndex];
      bool ready = true;
      for (uint32_t dependency = 0u; dependency < page.dependencyCount; ++dependency)
      {
        const uint32_t dependencyPage = readU32(dependencyWords, static_cast<size_t>((page.dependencyFirst + dependency) * 4u));
        ready &= installed[dependencyPage];
      }
      if (ready)
      {
        readyPage = pageIndex;
        break;
      }
    }
    if (readyPage == kInvalidSparseVoxelIndex)
      break;
    const PageRecord &page = pages[readyPage];
    for (uint32_t node = 0u; node < page.nodeCount; ++node)
      resident[page.firstNode + node] = true;
    if ((page.flags & kVirtualSparseVoxelPageRoot) != 0u)
      expectedSelected += page.nodeCount;
    for (const auto &update : rewrites(bytes, records[5], page.installFirst, page.installCount))
    {
      childCounts[update.nodeIndex] = update.childCount;
      expectedSelected += update.childCount - 1u;
    }
    installed[readyPage] = true;
    installOrder.push_back(readyPage);
    validateResidentCut(scene, resident, childCounts, expectedSelected);
  }
  assert(installOrder.size() == static_cast<size_t>(std::count(wanted.begin(), wanted.end(), true)));
  assert(installOrder.size() < pages.size());
  assert(!std::is_sorted(installOrder.begin(), installOrder.end()));

  for (uint32_t orderIndex = static_cast<uint32_t>(installOrder.size()); orderIndex-- > 1u;)
  {
    const PageRecord &page = pages[installOrder[orderIndex]];
    for (const auto &update : rewrites(bytes, records[6], page.uninstallFirst, page.uninstallCount))
    {
      const uint32_t previousCount = childCounts[update.nodeIndex];
      assert(previousCount > 0u);
      childCounts[update.nodeIndex] = 0u;
      expectedSelected -= previousCount - 1u;
    }
    for (uint32_t node = 0u; node < page.nodeCount; ++node)
      resident[page.firstNode + node] = false;
    validateResidentCut(scene, resident, childCounts, expectedSelected);
  }
  assert(expectedSelected == scene.models[0].rootCount);
}

template <typename T>
bool exactVector(const std::vector<T> &left, const std::vector<T> &right)
{
  return left.size() == right.size() && (left.empty() || std::memcmp(left.data(), right.data(), left.size() * sizeof(T)) == 0);
}

void assertScenesExactlyEqual(const SparseVoxelScene &expected, const SparseVoxelScene &actual)
{
  assert(expected.models.size() == actual.models.size());
  assert(expected.instances.size() == actual.instances.size());
  assert(expected.sourceInfo.importerVersion == actual.sourceInfo.importerVersion);
  assert(expected.sourceInfo.sourceRelativePath == actual.sourceInfo.sourceRelativePath);
  assert(expected.sourceInfo.normalizedBuildOptions == actual.sourceInfo.normalizedBuildOptions);
  assert(expected.voxelMaterialReferences == actual.voxelMaterialReferences);
  assert(expected.sourceContentHash == actual.sourceContentHash);
  assert(expected.buildSettingsHash == actual.buildSettingsHash);
  assert(expected.tlasFanout == actual.tlasFanout);
  assert(expected.tlasLeafSize == actual.tlasLeafSize);
  for (size_t index = 0u; index < expected.models.size(); ++index)
  {
    const SparseVoxelBLAS &left = expected.models[index];
    const SparseVoxelBLAS &right = actual.models[index];
    assert(left.rootCenter == right.rootCenter);
    assert(left.rootExtent == right.rootExtent);
    assert(left.maxLevel == right.maxLevel);
    assert(left.flags == right.flags);
    assert(left.firstRoot == right.firstRoot);
    assert(left.rootCount == right.rootCount);
    assert(exactVector(left.hierarchyNodes, right.hierarchyNodes));
    assert(left.octreePaths == right.octreePaths);
    assert(left.octreeLevels == right.octreeLevels);
    assert(exactVector(left.centersLengths, right.centersLengths));
    assert(exactVector(left.materials, right.materials));
    assert(left.sourceIds == right.sourceIds);
  }
  for (size_t index = 0u; index < expected.instances.size(); ++index)
  {
    assert(expected.instances[index].blasIndex == actual.instances[index].blasIndex);
    assert(expected.instances[index].flags == actual.instances[index].flags);
    assert(expected.instances[index].modelToWorld == actual.instances[index].modelToWorld);
  }
}

void writeRawFile(const std::filesystem::path &path, const std::vector<uint8_t> &bytes)
{
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  assert(file);
  file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  assert(file);
}

void writeTestU32(std::vector<uint8_t> &bytes, const size_t offset, const uint32_t value)
{
  assert(offset + 4u <= bytes.size());
  for (uint32_t byte = 0u; byte < 4u; ++byte)
    bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8u));
}

void writeTestU64(std::vector<uint8_t> &bytes, const size_t offset, const uint64_t value)
{
  assert(offset + 8u <= bytes.size());
  for (uint32_t byte = 0u; byte < 8u; ++byte)
    bytes[offset + byte] = static_cast<uint8_t>(value >> (byte * 8u));
}

void writeTestF32(std::vector<uint8_t> &bytes, const size_t offset, const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  writeTestU32(bytes, offset, bits);
}

size_t chunkTableEntry(const std::vector<uint8_t> &bytes, const uint32_t chunkIndex)
{
  return static_cast<size_t>(readU64(bytes, 32u) + uint64_t{chunkIndex} * kVirtualSparseVoxelFileChunkRecordSize);
}

void updateChunkChecksum(std::vector<uint8_t> &bytes, const uint32_t chunkIndex)
{
  const auto records = chunks(bytes);
  const ChunkRecord &record = records[chunkIndex];
  const uint64_t checksum = virtualSparseVoxelFileChecksum(bytes.data() + record.offset, static_cast<size_t>(record.storedSize));
  writeTestU64(bytes, chunkTableEntry(bytes, chunkIndex) + 48u, checksum);
}

std::filesystem::path g15Path(const std::string &suffix)
{
  return std::filesystem::temp_directory_path() / ("VirtualSparseVoxelSceneG15-" + suffix + ".vsvo");
}

void testMetadataIndependentPagesAndWholeSceneRoundTrip()
{
  for (const VirtualSparseVoxelFileCompression compression : {VirtualSparseVoxelFileCompression::None, VirtualSparseVoxelFileCompression::LZ4})
  {
    const SparseVoxelScene expected = tinyScene(true);
    const VirtualSparseVoxelSceneWriteOptions options{compression, 704u};
    const std::filesystem::path path = g15Path(compression == VirtualSparseVoxelFileCompression::None ? "none" : "lz4");
    std::filesystem::remove(path);
    std::string error;
    assert(writeVirtualSparseVoxelScene(path.string(), expected, options, &error));

    VirtualSparseVoxelSceneMetadata metadata;
    assert(readVirtualSparseVoxelSceneMetadata(path.string(), metadata, &error));
    assert(error.empty());
    assert(metadata.modelCount == 1u && metadata.instanceCount == 1u && metadata.nodeCount == 72u && metadata.pageCount == 9u);
    assert(metadata.models.size() == 1u && metadata.models[0].pageCount == 9u && metadata.models[0].rootPage == 0u);
    assert(metadata.hasSourceIds);
    std::vector<bool> seen(metadata.nodeCount, false);
    for (uint32_t pageIndex = metadata.pageCount; pageIndex-- > 0u;)
    {
      VirtualSparseVoxelPageData page;
      assert(readVirtualSparseVoxelPage(path.string(), metadata, pageIndex, page, &error));
      assert(page.pageIndex == pageIndex && page.nodes.size() == metadata.pages[pageIndex].nodeCount);
      for (uint32_t node = 0u; node < page.nodes.size(); ++node)
      {
        assert(page.nodes[node].childCount == 0u);
        assert(page.nodes[node].voxelIndex == page.firstNode + node);
        assert(!seen[page.firstNode + node]);
        seen[page.firstNode + node] = true;
      }
    }
    assert(std::find(seen.begin(), seen.end(), false) == seen.end());

    SparseVoxelScene loaded;
    assert(readVirtualSparseVoxelScene(path.string(), loaded, &error));
    assertScenesExactlyEqual(expected, loaded);
    std::vector<uint8_t> original = readFile(path);
    std::vector<uint8_t> rewritten;
    assert(serializeVirtualSparseVoxelScene(loaded, options, rewritten, &error));
    assert(original == rewritten);
    std::filesystem::remove(path);
  }
}

void testGeneratedHierarchyWholeSceneRoundTrip()
{
  const SparseVoxelScene expected = makeExteriorShellScene(kDefaultExteriorShellMaterialSeed);
  const std::filesystem::path path = g15Path("exterior-shell");
  std::string error;
  assert(writeVirtualSparseVoxelScene(path.string(), expected, {}, &error));
  SparseVoxelScene loaded;
  assert(readVirtualSparseVoxelScene(path.string(), loaded, &error));
  assertScenesExactlyEqual(expected, loaded);
  std::filesystem::remove(path);
}

void testMaterialAssetReferencesAndLegacySceneCompatibility()
{
  std::string error;
  SparseVoxelScene referenced = tinyScene(true);
  referenced.voxelMaterialReferences = {
    {7u, 0x13u, 0x6c8e9cf570932bd1ull, "terrain/grass-soil"},
    {19u, 0x20u, 0x070ce8f3eb14c952ull, "terrain/porous-rock"},
  };
  const std::filesystem::path referencedPath =
      g15Path("material-references");
  assert(writeVirtualSparseVoxelScene(
      referencedPath.string(), referenced, {}, &error));
  VirtualSparseVoxelSceneMetadata metadata{};
  assert(readVirtualSparseVoxelSceneMetadata(
      referencedPath.string(), metadata, &error));
  assert(metadata.voxelMaterialReferences ==
      referenced.voxelMaterialReferences);
  SparseVoxelScene loaded{};
  assert(readVirtualSparseVoxelScene(referencedPath.string(), loaded, &error));
  assertScenesExactlyEqual(referenced, loaded);
  std::filesystem::remove(referencedPath);

  // A version-1 scene without the optional phase-6 chunk remains valid and
  // adapts to an empty reference table.
  const SparseVoxelScene legacy = tinyScene(true);
  const std::filesystem::path legacyPath = g15Path("legacy-no-material-refs");
  assert(writeVirtualSparseVoxelScene(legacyPath.string(), legacy, {}, &error));
  assert(readVirtualSparseVoxelScene(legacyPath.string(), loaded, &error));
  assert(loaded.voxelMaterialReferences.empty());
  assertScenesExactlyEqual(legacy, loaded);
  std::filesystem::remove(legacyPath);

  SparseVoxelScene duplicate = referenced;
  duplicate.voxelMaterialReferences.push_back(
      {7u, 0u, 1u, "terrain/duplicate"});
  std::vector<uint8_t> rejected;
  assert(!serializeVirtualSparseVoxelScene(duplicate, {}, rejected, &error));
  assert(error.find("duplicate") != std::string::npos);
}

void expectMetadataFailure(const std::vector<uint8_t> &bytes, const std::string &suffix)
{
  const std::filesystem::path path = g15Path(suffix);
  writeRawFile(path, bytes);
  VirtualSparseVoxelSceneMetadata metadata;
  std::string error;
  assert(!readVirtualSparseVoxelSceneMetadata(path.string(), metadata, &error));
  assert(!error.empty());
  std::filesystem::remove(path);
}

void testRejectedHeaderChunkAndSchemaCorruption()
{
  std::vector<uint8_t> valid;
  assert(serializeVirtualSparseVoxelScene(tinyScene(), {VirtualSparseVoxelFileCompression::None, 704u}, valid));

  std::vector<uint8_t> damaged(valid.begin(), valid.begin() + 40u);
  expectMetadataFailure(damaged, "truncated-header");
  damaged = valid;
  damaged[0] = 'X';
  expectMetadataFailure(damaged, "magic");
  damaged = valid;
  writeTestU32(damaged, 20u, 0xffffffffu);
  expectMetadataFailure(damaged, "chunk-count");
  damaged = valid;
  damaged[static_cast<size_t>(chunks(damaged)[0].offset)] ^= 1u;
  expectMetadataFailure(damaged, "chunk-checksum");
  damaged = valid;
  writeTestU64(damaged, chunkTableEntry(damaged, 1u) + 8u, chunks(damaged)[0].offset);
  expectMetadataFailure(damaged, "chunk-overlap");
  damaged = valid;
  writeTestU32(damaged, chunkTableEntry(damaged, 8u), static_cast<uint32_t>(VirtualSparseVoxelFileChunkType::Scene));
  expectMetadataFailure(damaged, "duplicate-chunk");
  damaged = valid;
  writeTestU32(damaged, chunkTableEntry(damaged, 3u) + 32u, 96u);
  expectMetadataFailure(damaged, "obsolete-page-record-size");

  damaged = valid;
  const auto records = chunks(damaged);
  writeTestU32(damaged, static_cast<size_t>(records[0].offset + 12u), 0u);
  updateChunkChecksum(damaged, 0u);
  expectMetadataFailure(damaged, "scene-schema");

  damaged = valid;
  const auto pageRecordsBefore = pageRecords(damaged, chunks(damaged));
  assert(pageRecordsBefore[1].dependencyCount == 1u);
  writeTestU32(damaged, static_cast<size_t>(chunks(damaged)[4].offset), 1u);
  updateChunkChecksum(damaged, 4u);
  expectMetadataFailure(damaged, "forward-dependency");

  damaged = valid;
  const size_t pageTable = static_cast<size_t>(chunks(damaged)[3].offset);
  std::swap_ranges(
      damaged.begin() + pageTable + kVirtualSparseVoxelFilePageRecordSize,
      damaged.begin() + pageTable + 2u * kVirtualSparseVoxelFilePageRecordSize,
      damaged.begin() + pageTable + 2u * kVirtualSparseVoxelFilePageRecordSize);
  updateChunkChecksum(damaged, 3u);
  expectMetadataFailure(damaged, "reordered-same-level-pages");

  damaged = valid;
  const size_t firstPage = static_cast<size_t>(chunks(damaged)[3].offset);
  writeTestF32(damaged, firstPage + 96u, -std::numeric_limits<float>::infinity());
  updateChunkChecksum(damaged, 3u);
  expectMetadataFailure(damaged, "non-finite-page-bounds");

  damaged = valid;
  writeTestF32(damaged, firstPage + 96u, -2.0f);
  updateChunkChecksum(damaged, 3u);
  expectMetadataFailure(damaged, "page-bounds-outside-root");
}

void testRejectedPageAndReconstructedSceneCorruption()
{
  std::vector<uint8_t> valid;
  assert(serializeVirtualSparseVoxelScene(tinyScene(), {VirtualSparseVoxelFileCompression::None, 704u}, valid));

  std::vector<uint8_t> damaged = valid;
  auto records = chunks(damaged);
  auto pages = pageRecords(damaged, records);
  const size_t firstPayload = static_cast<size_t>(records[7].offset + pages[0].payloadOffset);
  damaged[firstPayload] ^= 1u;
  const std::filesystem::path checksumPath = g15Path("page-checksum");
  writeRawFile(checksumPath, damaged);
  VirtualSparseVoxelSceneMetadata metadata;
  std::string error;
  assert(readVirtualSparseVoxelSceneMetadata(checksumPath.string(), metadata, &error));
  VirtualSparseVoxelPageData page;
  assert(!readVirtualSparseVoxelPage(checksumPath.string(), metadata, 0u, page, &error));
  assert(error.find("checksum") != std::string::npos);
  std::filesystem::remove(checksumPath);

  damaged = valid;
  writeTestU64(damaged, chunkTableEntry(damaged, 7u) + 48u, chunks(damaged)[7].checksum ^ 1u);
  const std::filesystem::path aggregateChecksumPath = g15Path("aggregate-payload-checksum");
  writeRawFile(aggregateChecksumPath, damaged);
  assert(readVirtualSparseVoxelSceneMetadata(aggregateChecksumPath.string(), metadata, &error));
  assert(readVirtualSparseVoxelPage(aggregateChecksumPath.string(), metadata, 0u, page, &error));
  SparseVoxelScene aggregateLoaded;
  assert(!readVirtualSparseVoxelScene(aggregateChecksumPath.string(), aggregateLoaded, &error));
  assert(error.find("chunk checksum") != std::string::npos);
  assert(aggregateLoaded.models.empty());
  std::filesystem::remove(aggregateChecksumPath);

  damaged = valid;
  records = chunks(damaged);
  pages = pageRecords(damaged, records);
  writeTestF32(damaged, static_cast<size_t>(records[3].offset + 96u), pages[0].localBoundsMin[0] + 0.25f);
  updateChunkChecksum(damaged, 3u);
  const std::filesystem::path inexactBoundsPath = g15Path("inexact-page-bounds");
  writeRawFile(inexactBoundsPath, damaged);
  assert(readVirtualSparseVoxelSceneMetadata(inexactBoundsPath.string(), metadata, &error));
  assert(!readVirtualSparseVoxelPage(inexactBoundsPath.string(), metadata, 0u, page, &error));
  assert(error.find("bounds") != std::string::npos);
  assert(page.nodes.empty());
  std::filesystem::remove(inexactBoundsPath);

  damaged = valid;
  records = chunks(damaged);
  pages = pageRecords(damaged, records);
  const size_t canonicalPayload = static_cast<size_t>(records[7].offset + pages[0].payloadOffset);
  const size_t firstCenter = canonicalPayload + readU32(damaged, canonicalPayload + 32u);
  writeTestF32(damaged, firstCenter, readF32(damaged, firstCenter) + 0.125f);
  const uint64_t canonicalPageChecksum = virtualSparseVoxelFileChecksum(damaged.data() + canonicalPayload, static_cast<size_t>(pages[0].uncompressedSize));
  writeTestU64(damaged, static_cast<size_t>(records[3].offset + 88u), canonicalPageChecksum);
  updateChunkChecksum(damaged, 3u);
  const std::filesystem::path canonicalPath = g15Path("noncanonical-center");
  writeRawFile(canonicalPath, damaged);
  assert(readVirtualSparseVoxelSceneMetadata(canonicalPath.string(), metadata, &error));
  assert(!readVirtualSparseVoxelPage(canonicalPath.string(), metadata, 0u, page, &error));
  assert(error.find("canonical path") != std::string::npos);
  assert(page.nodes.empty());
  std::filesystem::remove(canonicalPath);

  damaged = valid;
  records = chunks(damaged);
  pages = pageRecords(damaged, records);
  const size_t payload = static_cast<size_t>(records[7].offset + pages[0].payloadOffset);
  writeTestU32(damaged, payload, 0u);
  const uint64_t badHeaderPageChecksum = virtualSparseVoxelFileChecksum(damaged.data() + payload, static_cast<size_t>(pages[0].uncompressedSize));
  writeTestU64(damaged, static_cast<size_t>(records[3].offset + 88u), badHeaderPageChecksum);
  updateChunkChecksum(damaged, 3u);
  updateChunkChecksum(damaged, 7u);
  const std::filesystem::path headerPath = g15Path("page-header");
  writeRawFile(headerPath, damaged);
  assert(readVirtualSparseVoxelSceneMetadata(headerPath.string(), metadata, &error));
  assert(!readVirtualSparseVoxelPage(headerPath.string(), metadata, 0u, page, &error));
  assert(error.find("header") != std::string::npos);
  std::filesystem::remove(headerPath);

  damaged = valid;
  records = chunks(damaged);
  pages = pageRecords(damaged, records);
  const size_t materialOffset = static_cast<size_t>(records[7].offset + pages[0].payloadOffset + 64u + pages[0].nodeCount * 48u);
  writeTestU32(damaged, materialOffset, readU32(damaged, materialOffset) & 0x00ffffffu);
  const uint64_t badMaterialPageChecksum = virtualSparseVoxelFileChecksum(
      damaged.data() + records[7].offset + pages[0].payloadOffset, static_cast<size_t>(pages[0].uncompressedSize));
  writeTestU64(damaged, static_cast<size_t>(records[3].offset + 88u), badMaterialPageChecksum);
  updateChunkChecksum(damaged, 3u);
  updateChunkChecksum(damaged, 7u);
  const std::filesystem::path materialPath = g15Path("material");
  writeRawFile(materialPath, damaged);
  SparseVoxelScene loaded;
  assert(!readVirtualSparseVoxelScene(materialPath.string(), loaded, &error));
  assert(error.find("material") != std::string::npos);
  assert(loaded.models.empty());
  std::filesystem::remove(materialPath);

  std::vector<uint8_t> compressed;
  assert(serializeVirtualSparseVoxelScene(tinyScene(), {VirtualSparseVoxelFileCompression::LZ4, 704u}, compressed));
  records = chunks(compressed);
  pages = pageRecords(compressed, records);
  assert((pages[0].flags & kVirtualSparseVoxelPageCompressedLZ4) != 0u);
  compressed[static_cast<size_t>(records[7].offset + pages[0].payloadOffset)] ^= 0xffu;
  const std::filesystem::path lz4Path = g15Path("lz4-page");
  writeRawFile(lz4Path, compressed);
  assert(readVirtualSparseVoxelSceneMetadata(lz4Path.string(), metadata, &error));
  assert(!readVirtualSparseVoxelPage(lz4Path.string(), metadata, 0u, page, &error));
  assert(!error.empty());
  std::filesystem::remove(lz4Path);
}

} // namespace

int main(const int argc, const char **argv)
{
  const bool runG14 = argc == 1 || std::string(argv[1]) == "--g14";
  const bool runG15 = argc == 1 || std::string(argv[1]) == "--g15";
  assert(runG14 || runG15);
  if (runG14)
  {
    testGoldenUncompressedLayout();
    testPageRewritePrefixesAndDependencies();
    testDeterministicIndependentLZ4Pages();
    testMultiModelLevelOrderingAndPageLimits();
    testVSVOAtomicWriteAndValidation();
    testGeneratedExteriorShellHierarchyIsPaged();
  }
  if (runG15)
  {
    testMetadataIndependentPagesAndWholeSceneRoundTrip();
    testGeneratedHierarchyWholeSceneRoundTrip();
    testMaterialAssetReferencesAndLegacySceneCompatibility();
    testRejectedHeaderChunkAndSchemaCorruption();
    testRejectedPageAndReconstructedSceneCorruption();
  }
  return 0;
}
