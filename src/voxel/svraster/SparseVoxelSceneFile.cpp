#include "voxel/svraster/SparseVoxelSceneFile.hpp"

#include "lz4.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace voxel::svraster
{
namespace
{

constexpr uint32_t kSceneSolidOpaqueFlag = 1u << 0u;
constexpr uint32_t kScenePagedHierarchyFlag = 1u << 1u;
constexpr uint32_t kAppearanceEncodingSRGB8PBR = 1u;
constexpr uint32_t kPagePayloadHeaderSize = 64u;
constexpr uint32_t kPageNodeBytes = sizeof(SparseVoxelLODNodeGPU) + sizeof(SparseVoxelPathLevelGPU) + sizeof(SparseVoxelCenterLengthGPU) + sizeof(SparseVoxelMaterialGPU);

struct Chunk
{
  VirtualSparseVoxelFileChunkType type{};
  uint32_t recordSize = 0u;
  uint64_t recordCount = 0u;
  uint64_t fileOffset = 0u;
  std::vector<uint8_t> payload;
};

struct Page
{
  uint32_t modelIndex = 0u;
  uint32_t flags = 0u;
  uint32_t firstNode = 0u;
  uint32_t nodeCount = 0u;
  uint32_t minLevel = 0u;
  uint32_t maxLevel = 0u;
  std::vector<uint32_t> dependencies;
  std::vector<VirtualSparseVoxelPageRewrite> installUpdates;
  std::vector<VirtualSparseVoxelPageRewrite> uninstallUpdates;
  uint64_t dependencyFirst = 0u;
  uint64_t installFirst = 0u;
  uint64_t uninstallFirst = 0u;
  uint64_t payloadOffset = 0u;
  uint64_t uncompressedSize = 0u;
  uint64_t payloadChecksum = 0u;
  std::array<float, 3> localBoundsMin{};
  std::array<float, 3> localBoundsMax{};
  std::vector<uint8_t> storedPayload;
};

bool fail(std::string *error, const std::string &message)
{
  if (error != nullptr)
    *error = message;
  return false;
}

void appendU8(std::vector<uint8_t> &bytes, const uint8_t value)
{
  bytes.push_back(value);
}

void appendU16(std::vector<uint8_t> &bytes, const uint16_t value)
{
  appendU8(bytes, static_cast<uint8_t>(value));
  appendU8(bytes, static_cast<uint8_t>(value >> 8u));
}

void appendU32(std::vector<uint8_t> &bytes, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
    appendU8(bytes, static_cast<uint8_t>(value >> shift));
}

void appendU64(std::vector<uint8_t> &bytes, const uint64_t value)
{
  for (uint32_t shift = 0u; shift < 64u; shift += 8u)
    appendU8(bytes, static_cast<uint8_t>(value >> shift));
}

void appendF32(std::vector<uint8_t> &bytes, const float value)
{
  uint32_t bits = 0u;
  std::memcpy(&bits, &value, sizeof(bits));
  appendU32(bytes, bits);
}

void writeU32(std::vector<uint8_t> &bytes, const size_t offset, const uint32_t value)
{
  for (uint32_t shift = 0u; shift < 32u; shift += 8u)
    bytes[offset + shift / 8u] = static_cast<uint8_t>(value >> shift);
}

void writeU64(std::vector<uint8_t> &bytes, const size_t offset, const uint64_t value)
{
  for (uint32_t shift = 0u; shift < 64u; shift += 8u)
    bytes[offset + shift / 8u] = static_cast<uint8_t>(value >> shift);
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

bool alignUp(const uint64_t value, uint64_t &result)
{
  uint64_t adjusted = 0u;
  if (!checkedAdd(value, kVirtualSparseVoxelFileAlignment - 1u, adjusted))
    return false;
  result = adjusted & ~(uint64_t{kVirtualSparseVoxelFileAlignment} - 1u);
  return true;
}

void appendPadding(std::vector<uint8_t> &bytes)
{
  while (bytes.size() % kVirtualSparseVoxelFileAlignment != 0u)
    bytes.push_back(0u);
}

bool totalNodeCount(const SparseVoxelScene &scene, uint32_t &result)
{
  uint64_t count = 0u;
  for (const SparseVoxelBLAS &model : scene.models)
  {
    if (!checkedAdd(count, model.hierarchyNodes.size(), count) || count > std::numeric_limits<uint32_t>::max())
      return false;
  }
  result = static_cast<uint32_t>(count);
  return true;
}

std::vector<uint32_t> modelNodeBases(const SparseVoxelScene &scene)
{
  std::vector<uint32_t> result(scene.models.size());
  uint32_t base = 0u;
  for (uint32_t modelIndex = 0u; modelIndex < scene.models.size(); ++modelIndex)
  {
    result[modelIndex] = base;
    base += static_cast<uint32_t>(scene.models[modelIndex].hierarchyNodes.size());
  }
  return result;
}

bool finalizePage(
    const SparseVoxelScene &scene,
    const std::vector<uint32_t> &modelBases,
    std::vector<Page> &pages,
    Page &page,
    std::vector<uint32_t> &nodePages,
    std::string *error)
{
  const SparseVoxelBLAS &model = scene.models[page.modelIndex];
  const uint32_t localFirstNode = page.firstNode - modelBases[page.modelIndex];
  const float rootHalf = model.rootExtent * 0.5f;
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    page.localBoundsMin[axis] = std::numeric_limits<float>::infinity();
    page.localBoundsMax[axis] = -std::numeric_limits<float>::infinity();
  }
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const SparseVoxelCenterLengthGPU &centerLength = model.centersLengths[localFirstNode + nodeOffset];
    const float halfLength = centerLength.length * 0.5f;
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      const float rootMin = model.rootCenter[axis] - rootHalf;
      const float rootMax = model.rootCenter[axis] + rootHalf;
      const float nodeMin = centerLength.center[axis] - halfLength;
      const float nodeMax = centerLength.center[axis] + halfLength;
      page.localBoundsMin[axis] = std::min(page.localBoundsMin[axis], std::max(rootMin, std::nextafter(nodeMin, -std::numeric_limits<float>::infinity())));
      page.localBoundsMax[axis] = std::max(page.localBoundsMax[axis], std::min(rootMax, std::nextafter(nodeMax, std::numeric_limits<float>::infinity())));
    }
  }
  std::sort(page.dependencies.begin(), page.dependencies.end());
  page.dependencies.erase(std::unique(page.dependencies.begin(), page.dependencies.end()), page.dependencies.end());
  std::sort(page.installUpdates.begin(), page.installUpdates.end(), [](const auto &left, const auto &right) { return left.nodeIndex < right.nodeIndex; });
  page.uninstallUpdates.reserve(page.installUpdates.size());
  for (const VirtualSparseVoxelPageRewrite &update : page.installUpdates)
    page.uninstallUpdates.push_back({update.nodeIndex, update.firstChild, 0u, 0u});
  const uint32_t pageIndex = static_cast<uint32_t>(pages.size());
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
    nodePages[page.firstNode + nodeOffset] = pageIndex;
  pages.push_back(std::move(page));
  page = {};
  return true;
}

bool buildPages(
    const SparseVoxelScene &scene,
    const std::vector<uint32_t> &modelBases,
    const uint32_t targetPageBytes,
    std::vector<Page> &pages,
    std::string *error)
{
  uint32_t nodeCount = 0u;
  if (!totalNodeCount(scene, nodeCount))
    return fail(error, "Virtual sparse voxel hierarchy exceeds the 32-bit GPU node limit");
  if (targetPageBytes < kPagePayloadHeaderSize + 8u * kPageNodeBytes)
    return fail(error, "Virtual sparse voxel target page size cannot contain one complete sibling group");
  std::vector<uint32_t> nodePages(nodeCount, kInvalidSparseVoxelIndex);

  for (uint32_t modelIndex = 0u; modelIndex < scene.models.size(); ++modelIndex)
  {
    const SparseVoxelBLAS &model = scene.models[modelIndex];
    Page root{};
    root.modelIndex = modelIndex;
    root.flags = kVirtualSparseVoxelPageRoot;
    root.firstNode = modelBases[modelIndex] + model.firstRoot;
    root.nodeCount = model.rootCount;
    root.minLevel = 1u;
    root.maxLevel = 1u;
    if (!finalizePage(scene, modelBases, pages, root, nodePages, error))
      return false;
  }

  uint32_t maximumLevel = 0u;
  for (const SparseVoxelBLAS &model : scene.models)
    maximumLevel = std::max(maximumLevel, model.maxLevel);
  const uint32_t maximumNodesPerPage = (targetPageBytes - kPagePayloadHeaderSize) / kPageNodeBytes;
  for (uint32_t level = 2u; level <= maximumLevel; ++level)
  {
    for (uint32_t modelIndex = 0u; modelIndex < scene.models.size(); ++modelIndex)
    {
      const SparseVoxelBLAS &model = scene.models[modelIndex];
      const uint32_t modelBase = modelBases[modelIndex];
      Page page{};
      for (uint32_t parentIndex = 0u; parentIndex < model.hierarchyNodes.size(); ++parentIndex)
      {
        const SparseVoxelLODNodeGPU &parent = model.hierarchyNodes[parentIndex];
        if (parent.level + 1u != level || parent.childCount == 0u)
          continue;
        if (page.nodeCount != 0u && page.nodeCount + parent.childCount > maximumNodesPerPage)
          if (!finalizePage(scene, modelBases, pages, page, nodePages, error))
            return false;
        if (page.nodeCount == 0u)
        {
          page.modelIndex = modelIndex;
          page.firstNode = modelBase + parent.firstChild;
          page.minLevel = level;
          page.maxLevel = level;
        }
        if (page.firstNode + page.nodeCount != modelBase + parent.firstChild)
          return fail(error, "Virtual sparse voxel sibling groups are not contiguous in breadth-first order");
        const uint32_t dependency = nodePages[modelBase + parentIndex];
        if (dependency == kInvalidSparseVoxelIndex)
          return fail(error, "Virtual sparse voxel child page has no resident parent dependency");
        page.dependencies.push_back(dependency);
        page.installUpdates.push_back({modelBase + parentIndex, modelBase + parent.firstChild, parent.childCount, 0u});
        page.nodeCount += parent.childCount;
      }
      if (page.nodeCount != 0u)
        if (!finalizePage(scene, modelBases, pages, page, nodePages, error))
          return false;
    }
  }
  for (const uint32_t page : nodePages)
    if (page == kInvalidSparseVoxelIndex)
      return fail(error, "Virtual sparse voxel hierarchy contains a node not assigned to a page");
  return true;
}

std::vector<uint8_t> makePagePayload(
    const SparseVoxelScene &scene,
    const std::vector<uint32_t> &modelBases,
    const Page &page,
    const uint32_t pageIndex)
{
  const SparseVoxelBLAS &model = scene.models[page.modelIndex];
  const uint32_t localFirstNode = page.firstNode - modelBases[page.modelIndex];
  const uint32_t nodesOffset = kPagePayloadHeaderSize;
  const uint32_t pathsOffset = nodesOffset + page.nodeCount * sizeof(SparseVoxelLODNodeGPU);
  const uint32_t centersOffset = pathsOffset + page.nodeCount * sizeof(SparseVoxelPathLevelGPU);
  const uint32_t materialsOffset = centersOffset + page.nodeCount * sizeof(SparseVoxelCenterLengthGPU);
  std::vector<uint8_t> payload;
  payload.reserve(materialsOffset + page.nodeCount * sizeof(SparseVoxelMaterialGPU));
  appendU32(payload, 0x47505356u); // VSPG
  appendU32(payload, 1u);
  appendU32(payload, pageIndex);
  appendU32(payload, page.modelIndex);
  appendU32(payload, page.firstNode);
  appendU32(payload, page.nodeCount);
  appendU32(payload, nodesOffset);
  appendU32(payload, pathsOffset);
  appendU32(payload, centersOffset);
  appendU32(payload, materialsOffset);
  appendU32(payload, sizeof(SparseVoxelLODNodeGPU));
  appendU32(payload, sizeof(SparseVoxelPathLevelGPU));
  appendU32(payload, sizeof(SparseVoxelCenterLengthGPU));
  appendU32(payload, sizeof(SparseVoxelMaterialGPU));
  appendU64(payload, 0u);
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const uint32_t localNodeIndex = localFirstNode + nodeOffset;
    SparseVoxelLODNodeGPU node = model.hierarchyNodes[localNodeIndex];
    node.firstChild = node.childCount == 0u ? 0u : modelBases[page.modelIndex] + node.firstChild;
    node.childCount = 0u;
    node.voxelIndex = page.firstNode + nodeOffset;
    appendU32(payload, node.firstChild);
    appendU32(payload, node.childCount);
    appendU32(payload, node.voxelIndex);
    appendU32(payload, node.level);
  }
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const uint32_t localNodeIndex = localFirstNode + nodeOffset;
    const uint64_t path = model.octreePaths[localNodeIndex];
    appendU32(payload, static_cast<uint32_t>(path));
    appendU32(payload, static_cast<uint32_t>(path >> 32u));
    appendU32(payload, model.octreeLevels[localNodeIndex]);
    appendU32(payload, 0u);
  }
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const SparseVoxelCenterLengthGPU &centerLength = model.centersLengths[localFirstNode + nodeOffset];
    appendF32(payload, centerLength.center[0]);
    appendF32(payload, centerLength.center[1]);
    appendF32(payload, centerLength.center[2]);
    appendF32(payload, centerLength.length);
  }
  for (uint32_t nodeOffset = 0u; nodeOffset < page.nodeCount; ++nodeOffset)
  {
    const SparseVoxelMaterialGPU &material = model.materials[localFirstNode + nodeOffset];
    appendU32(payload, material.albedoRGBA8);
    appendU32(payload, material.roughnessMetallicUNorm8);
    appendU32(payload, material.emissiveRG16F);
    appendU32(payload, material.emissiveB16F);
    appendU32(payload, material.materialId);
    appendU32(payload, material.flags);
    appendU32(payload, material.reserved0);
    appendU32(payload, material.reserved1);
  }
  return payload;
}

bool compressPage(Page &page, const std::vector<uint8_t> &payload, const VirtualSparseVoxelFileCompression compression, std::string *error)
{
  page.uncompressedSize = payload.size();
  page.payloadChecksum = virtualSparseVoxelFileChecksum(payload.data(), payload.size());
  if (compression == VirtualSparseVoxelFileCompression::None)
  {
    page.storedPayload = payload;
    return true;
  }
  if (compression != VirtualSparseVoxelFileCompression::LZ4)
    return fail(error, "Unsupported virtual sparse voxel page compression");
  if (payload.size() > static_cast<size_t>(INT_MAX))
    return fail(error, "Virtual sparse voxel page exceeds the LZ4 size limit");
  const int bound = LZ4_compressBound(static_cast<int>(payload.size()));
  page.storedPayload.resize(static_cast<size_t>(bound));
  const int size = LZ4_compress_default(
      reinterpret_cast<const char *>(payload.data()),
      reinterpret_cast<char *>(page.storedPayload.data()),
      static_cast<int>(payload.size()),
      bound);
  if (size <= 0)
    return fail(error, "Failed to compress a virtual sparse voxel page");
  page.storedPayload.resize(static_cast<size_t>(size));
  page.flags |= kVirtualSparseVoxelPageCompressedLZ4;
  return true;
}

std::vector<uint8_t> scenePayload(const SparseVoxelScene &scene, const uint32_t nodeCount, const uint32_t pageCount)
{
  std::vector<uint8_t> payload;
  appendU32(payload, 1u);
  appendU32(payload, static_cast<uint32_t>(scene.models.size()));
  appendU32(payload, static_cast<uint32_t>(scene.instances.size()));
  appendU32(payload, kSceneSolidOpaqueFlag | kScenePagedHierarchyFlag);
  appendU64(payload, nodeCount);
  appendU32(payload, pageCount);
  appendU32(payload, kAppearanceEncodingSRGB8PBR);
  appendU32(payload, scene.tlasFanout);
  appendU32(payload, scene.tlasLeafSize);
  appendU64(payload, scene.sourceContentHash);
  appendU64(payload, scene.buildSettingsHash);
  appendU64(payload, 0u);
  return payload;
}

std::vector<uint8_t> modelPayload(const SparseVoxelScene &scene, const std::vector<uint32_t> &modelBases, const std::vector<Page> &pages)
{
  std::vector<uint8_t> payload;
  for (uint32_t modelIndex = 0u; modelIndex < scene.models.size(); ++modelIndex)
  {
    const SparseVoxelBLAS &model = scene.models[modelIndex];
    uint32_t rootPage = kInvalidSparseVoxelIndex;
    uint32_t pageCount = 0u;
    for (uint32_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
    {
      if (pages[pageIndex].modelIndex != modelIndex)
        continue;
      if ((pages[pageIndex].flags & kVirtualSparseVoxelPageRoot) != 0u)
        rootPage = pageIndex;
      ++pageCount;
    }
    appendF32(payload, model.rootCenter[0]);
    appendF32(payload, model.rootCenter[1]);
    appendF32(payload, model.rootCenter[2]);
    appendF32(payload, model.rootExtent);
    appendU32(payload, model.maxLevel);
    appendU32(payload, model.flags);
    appendU64(payload, modelBases[modelIndex]);
    appendU64(payload, model.hierarchyNodes.size());
    appendU32(payload, modelBases[modelIndex] + model.firstRoot);
    appendU32(payload, model.rootCount);
    appendU32(payload, rootPage);
    appendU32(payload, pageCount);
    appendU64(payload, 0u);
  }
  return payload;
}

std::vector<uint8_t> instancePayload(const SparseVoxelScene &scene)
{
  std::vector<uint8_t> payload;
  for (const SparseVoxelInstance &instance : scene.instances)
  {
    for (const float value : instance.modelToWorld)
      appendF32(payload, value);
    appendU32(payload, instance.blasIndex);
    appendU32(payload, instance.flags);
    appendU64(payload, 0u);
  }
  return payload;
}

std::vector<uint8_t> pageTablePayload(const std::vector<Page> &pages)
{
  std::vector<uint8_t> payload;
  for (const Page &page : pages)
  {
    appendU32(payload, page.modelIndex);
    appendU32(payload, page.flags);
    appendU64(payload, page.firstNode);
    appendU32(payload, page.nodeCount);
    appendU32(payload, page.minLevel);
    appendU32(payload, page.maxLevel);
    appendU32(payload, static_cast<uint32_t>(page.dependencies.size()));
    appendU64(payload, page.dependencyFirst);
    appendU64(payload, page.installFirst);
    appendU32(payload, static_cast<uint32_t>(page.installUpdates.size()));
    appendU32(payload, static_cast<uint32_t>(page.uninstallUpdates.size()));
    appendU64(payload, page.uninstallFirst);
    appendU64(payload, page.payloadOffset);
    appendU64(payload, page.storedPayload.size());
    appendU64(payload, page.uncompressedSize);
    appendU64(payload, page.payloadChecksum);
    for (const float value : page.localBoundsMin)
      appendF32(payload, value);
    for (const float value : page.localBoundsMax)
      appendF32(payload, value);
    appendU64(payload, 0u);
  }
  return payload;
}

std::vector<uint8_t> dependencyPayload(const std::vector<Page> &pages)
{
  std::vector<uint8_t> payload;
  for (const Page &page : pages)
    for (const uint32_t dependency : page.dependencies)
      appendU32(payload, dependency);
  return payload;
}

std::vector<uint8_t> rewritePayload(const std::vector<Page> &pages, const bool install)
{
  std::vector<uint8_t> payload;
  for (const Page &page : pages)
  {
    const auto &updates = install ? page.installUpdates : page.uninstallUpdates;
    for (const VirtualSparseVoxelPageRewrite &update : updates)
    {
      appendU32(payload, update.nodeIndex);
      appendU32(payload, update.firstChild);
      appendU32(payload, update.childCount);
      appendU32(payload, update.flags);
    }
  }
  return payload;
}

std::vector<uint8_t> sourceInfoPayload(const SparseVoxelScene &scene)
{
  std::vector<uint8_t> payload;
  appendU32(payload, 1u);
  appendU32(payload, scene.sourceInfo.importerVersion);
  appendU32(payload, static_cast<uint32_t>(scene.sourceInfo.sourceRelativePath.size()));
  appendU32(payload, static_cast<uint32_t>(scene.sourceInfo.normalizedBuildOptions.size()));
  appendU64(payload, scene.sourceContentHash);
  appendU64(payload, scene.buildSettingsHash);
  payload.insert(payload.end(), scene.sourceInfo.sourceRelativePath.begin(), scene.sourceInfo.sourceRelativePath.end());
  payload.insert(payload.end(), scene.sourceInfo.normalizedBuildOptions.begin(), scene.sourceInfo.normalizedBuildOptions.end());
  return payload;
}

std::vector<uint8_t> sourceIdPayload(const SparseVoxelScene &scene)
{
  std::vector<uint8_t> payload;
  for (const SparseVoxelBLAS &model : scene.models)
    for (const uint32_t sourceId : model.sourceIds)
      appendU32(payload, sourceId);
  return payload;
}

std::vector<uint8_t> materialReferencePayload(const SparseVoxelScene &scene)
{
  std::vector<uint8_t> payload;
  appendU32(payload, 1u);
  appendU32(payload, static_cast<uint32_t>(scene.voxelMaterialReferences.size()));
  for (const SparseVoxelMaterialAssetReference &reference : scene.voxelMaterialReferences)
  {
    appendU32(payload, reference.sourceMaterialId);
    appendU32(payload, reference.contextFields);
    appendU64(payload, reference.assetHash);
    appendU32(payload, static_cast<uint32_t>(reference.assetId.size()));
    payload.insert(payload.end(), reference.assetId.begin(), reference.assetId.end());
  }
  return payload;
}

Chunk makeChunk(const VirtualSparseVoxelFileChunkType type, const uint32_t recordSize, const uint64_t recordCount, std::vector<uint8_t> payload)
{
  return {type, recordSize, recordCount, 0u, std::move(payload)};
}

} // namespace

uint64_t virtualSparseVoxelFileChecksum(const uint8_t *data, const size_t size)
{
  uint64_t hash = 14695981039346656037ull;
  for (size_t index = 0u; index < size; ++index)
  {
    hash ^= data[index];
    hash *= 1099511628211ull;
  }
  return hash;
}

bool serializeVirtualSparseVoxelScene(
    const SparseVoxelScene &scene,
    const VirtualSparseVoxelSceneWriteOptions &options,
    std::vector<uint8_t> &bytes,
    std::string *error)
{
  bytes.clear();
  if (error != nullptr)
    error->clear();
  std::string validationError;
  if (!validateSparseVoxelSceneSource(scene, &validationError))
    return fail(error, "Cannot write invalid virtual sparse voxel scene: " + validationError);
  if (scene.models.size() > std::numeric_limits<uint32_t>::max() || scene.instances.size() > std::numeric_limits<uint32_t>::max() ||
      scene.sourceInfo.sourceRelativePath.size() > std::numeric_limits<uint32_t>::max() || scene.sourceInfo.normalizedBuildOptions.size() > std::numeric_limits<uint32_t>::max())
    return fail(error, "Virtual sparse voxel scene count exceeds the version-1 file limit");
  for (const SparseVoxelMaterialAssetReference &reference : scene.voxelMaterialReferences)
    if (reference.assetId.size() > std::numeric_limits<uint32_t>::max())
      return fail(error, "Virtual sparse voxel material asset ID exceeds the version-1 file limit");
  uint32_t nodeCount = 0u;
  if (!totalNodeCount(scene, nodeCount))
    return fail(error, "Virtual sparse voxel hierarchy exceeds the 32-bit GPU node limit");
  const std::vector<uint32_t> modelBases = modelNodeBases(scene);
  std::vector<Page> pages;
  if (!buildPages(scene, modelBases, options.targetUncompressedPageBytes, pages, error))
    return false;

  std::vector<uint8_t> pagePayloads;
  uint64_t dependencyCount = 0u;
  uint64_t installCount = 0u;
  uint64_t uninstallCount = 0u;
  for (uint32_t pageIndex = 0u; pageIndex < pages.size(); ++pageIndex)
  {
    Page &page = pages[pageIndex];
    page.dependencyFirst = dependencyCount;
    page.installFirst = installCount;
    page.uninstallFirst = uninstallCount;
    dependencyCount += page.dependencies.size();
    installCount += page.installUpdates.size();
    uninstallCount += page.uninstallUpdates.size();
    const std::vector<uint8_t> unpacked = makePagePayload(scene, modelBases, page, pageIndex);
    if (!compressPage(page, unpacked, options.compression, error))
      return false;
    appendPadding(pagePayloads);
    page.payloadOffset = pagePayloads.size();
    pagePayloads.insert(pagePayloads.end(), page.storedPayload.begin(), page.storedPayload.end());
  }

  std::vector<Chunk> chunks;
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::Scene, 64u, 1u, scenePayload(scene, nodeCount, static_cast<uint32_t>(pages.size()))));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::Models, 64u, scene.models.size(), modelPayload(scene, modelBases, pages)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::Instances, 80u, scene.instances.size(), instancePayload(scene)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::Pages, kVirtualSparseVoxelFilePageRecordSize, pages.size(), pageTablePayload(pages)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::PageDependencies, 4u, dependencyCount, dependencyPayload(pages)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::PageInstallUpdates, 16u, installCount, rewritePayload(pages, true)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::PageUninstallUpdates, 16u, uninstallCount, rewritePayload(pages, false)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::PagePayloads, 1u, pagePayloads.size(), std::move(pagePayloads)));
  chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::SourceInfo, 1u, 0u, sourceInfoPayload(scene)));
  chunks.back().recordCount = chunks.back().payload.size();
  const bool hasSourceIds = !scene.models.empty() && !scene.models.front().sourceIds.empty();
  if (hasSourceIds)
    chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::SourceIds, 4u, nodeCount, sourceIdPayload(scene)));
  if (!scene.voxelMaterialReferences.empty())
  {
    chunks.push_back(makeChunk(VirtualSparseVoxelFileChunkType::MaterialReferences, 1u, 0u, materialReferencePayload(scene)));
    chunks.back().recordCount = chunks.back().payload.size();
  }

  uint64_t tableSize = 0u;
  uint64_t fileOffset = 0u;
  if (!checkedMultiply(chunks.size(), kVirtualSparseVoxelFileChunkRecordSize, tableSize) || !checkedAdd(kVirtualSparseVoxelFileHeaderSize, tableSize, fileOffset) || !alignUp(fileOffset, fileOffset))
    return fail(error, "Virtual sparse voxel chunk table size overflow");
  uint64_t totalUncompressed = 0u;
  for (Chunk &chunk : chunks)
  {
    chunk.fileOffset = fileOffset;
    if (!checkedAdd(totalUncompressed, chunk.payload.size(), totalUncompressed) || !checkedAdd(fileOffset, chunk.payload.size(), fileOffset) || !alignUp(fileOffset, fileOffset))
      return fail(error, "Virtual sparse voxel payload size overflow");
  }
  if (fileOffset > std::numeric_limits<size_t>::max())
    return fail(error, "Virtual sparse voxel file exceeds the addressable memory size");

  bytes.assign(static_cast<size_t>(fileOffset), 0u);
  std::memcpy(bytes.data(), "VSVO", 4u);
  writeU32(bytes, 4u, kVirtualSparseVoxelFileVersion);
  writeU32(bytes, 8u, kVirtualSparseVoxelFileEndianMarker);
  writeU32(bytes, 12u, kVirtualSparseVoxelFileHeaderSize);
  writeU32(bytes, 16u, kVirtualSparseVoxelFileChunkRecordSize);
  writeU32(bytes, 20u, static_cast<uint32_t>(chunks.size()));
  writeU64(bytes, 32u, kVirtualSparseVoxelFileHeaderSize);
  writeU64(bytes, 40u, totalUncompressed);
  writeU64(bytes, 48u, scene.sourceContentHash);
  writeU64(bytes, 56u, scene.buildSettingsHash);
  writeU64(bytes, 64u, bytes.size());
  for (uint32_t chunkIndex = 0u; chunkIndex < chunks.size(); ++chunkIndex)
  {
    const Chunk &chunk = chunks[chunkIndex];
    const size_t entry = kVirtualSparseVoxelFileHeaderSize + chunkIndex * kVirtualSparseVoxelFileChunkRecordSize;
    writeU32(bytes, entry, static_cast<uint32_t>(chunk.type));
    writeU64(bytes, entry + 8u, chunk.fileOffset);
    writeU64(bytes, entry + 16u, chunk.payload.size());
    writeU64(bytes, entry + 24u, chunk.payload.size());
    writeU32(bytes, entry + 32u, chunk.recordSize);
    writeU64(bytes, entry + 40u, chunk.recordCount);
    writeU64(bytes, entry + 48u, virtualSparseVoxelFileChecksum(chunk.payload.data(), chunk.payload.size()));
    std::copy(chunk.payload.begin(), chunk.payload.end(), bytes.begin() + static_cast<size_t>(chunk.fileOffset));
  }
  return true;
}

bool writeVirtualSparseVoxelScene(
    const std::string &path,
    const SparseVoxelScene &scene,
    const VirtualSparseVoxelSceneWriteOptions &options,
    std::string *error)
{
  std::vector<uint8_t> bytes;
  if (!serializeVirtualSparseVoxelScene(scene, options, bytes, error))
    return false;
  const std::string temporaryPath = path + ".tmp";
  {
    std::ofstream file(temporaryPath, std::ios::binary | std::ios::trunc);
    if (!file)
      return fail(error, "Failed to open virtual sparse voxel temporary file for writing: " + temporaryPath);
    file.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!file)
    {
      file.close();
      std::remove(temporaryPath.c_str());
      return fail(error, "Failed while writing virtual sparse voxel temporary file: " + temporaryPath);
    }
  }
  SparseVoxelScene verifiedScene{};
  std::string verificationError;
  if (!readVirtualSparseVoxelScene(temporaryPath, verifiedScene, &verificationError) ||
      verifiedScene.sourceContentHash != scene.sourceContentHash || verifiedScene.buildSettingsHash != scene.buildSettingsHash ||
      verifiedScene.models.size() != scene.models.size() || verifiedScene.instances.size() != scene.instances.size() ||
      verifiedScene.voxelMaterialReferences != scene.voxelMaterialReferences)
  {
    std::remove(temporaryPath.c_str());
    return fail(error, verificationError.empty() ? "Virtual sparse voxel temporary-file verification failed" :
                                                   "Virtual sparse voxel temporary-file verification failed: " + verificationError);
  }
  if (std::rename(temporaryPath.c_str(), path.c_str()) != 0)
  {
    std::remove(temporaryPath.c_str());
    return fail(error, "Failed to atomically replace virtual sparse voxel file: " + path);
  }
  return true;
}

} // namespace voxel::svraster
