#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "./utils/File.hpp"

#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpgpu/CopyBufferPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryCullingMultipleDispatchesPass.hpp"

using namespace rendering;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

struct QueueElement
{
  uint32_t index;
  uint32_t instanceIndex;
  uint32_t pageIndex;
  uint32_t _padding; // In clusterQueue: stores parent leaf nodeIndex (set by processHierarchyNodes).
                     // In hierarchyQueueWrite: stores parent nodeIndex for inner nodes.
};

static_assert(sizeof(QueueElement) == 16, "QueueElement must be 16 bytes");
static_assert(sizeof(VisibleClusterInfo) == 16, "VisibleClusterInfo must be 16 bytes");

struct DrawIndirectCommand
{
  uint32_t vertexCount;
  uint32_t instanceCount;
  uint32_t firstVertex;
  uint32_t firstInstance;
};
static_assert(sizeof(DrawIndirectCommand) == 16, "DrawIndirectCommand must be 16 bytes");

struct CullingCounters
{
  uint32_t hierarchyQueueSize;
  uint32_t clusterQueueSize;
  uint32_t visibleClusterHardwareCount;
  uint32_t readQueueSize;
  uint32_t visibleClusterSoftwareCount;
};

static constexpr uint32_t SENTINEL_VALUE = 0xFFFFFFFFu;
static constexpr uint32_t MAX_QUEUE_ELEMENTS = 1u << 20;

namespace cpu_shader_mirror
{

// mirrors WGSL extractBits() — unchanged
inline uint32_t extractBits(const uint32_t *words, uint32_t baseWord, uint32_t bitOffset, uint32_t numBits)
{
  if (numBits == 0u)
    return 0u;

  const uint32_t wordIdx = baseWord + (bitOffset / 32u);
  const uint32_t bitInWord = bitOffset % 32u;

  uint32_t value = words[wordIdx] >> bitInWord;
  if (bitInWord + numBits > 32u)
  {
    const uint32_t overflow = (bitInWord + numBits) - 32u;
    value |= words[wordIdx + 1u] << (numBits - overflow);
  }

  const uint32_t mask = (numBits == 32u) ? 0xFFFFFFFFu : ((1u << numBits) - 1u);
  return value & mask;
}

inline std::array<float, 3>
decodePosition(const uint32_t *words, uint32_t posDataBase, uint32_t posOff, uint32_t bitsX, uint32_t bitsY, uint32_t bitsZ, uint32_t qFactor, float minQX, float minQY, float minQZ, float unitScale, uint32_t vertexIdx)
{
  const uint32_t bitsPerVertex = bitsX + bitsY + bitsZ;
  const uint32_t startBit = vertexIdx * bitsPerVertex;
  const uint32_t base = posDataBase + posOff;

  uint32_t bitCursor = startBit;
  const uint32_t qx = extractBits(words, base, bitCursor, bitsX);
  bitCursor += bitsX;
  const uint32_t qy = extractBits(words, base, bitCursor, bitsY);
  bitCursor += bitsY;
  const uint32_t qz = extractBits(words, base, bitCursor, bitsZ);

  const float dequantScale = static_cast<float>(1u << qFactor) * unitScale;
  return {
    (minQX + static_cast<float>(qx)) / dequantScale,
    (minQY + static_cast<float>(qy)) / dequantScale,
    (minQZ + static_cast<float>(qz)) / dequantScale,
  };
}

inline std::array<float, 3> decodeNormal(const uint32_t *words, uint32_t normDataBase, uint32_t normOff, uint32_t vertexIdx)
{
  const uint32_t packed = words[normDataBase + normOff + vertexIdx];

  const int32_t rawX = static_cast<int32_t>(packed & 0xFFFFu);
  const int32_t rawY = static_cast<int32_t>((packed >> 16u) & 0xFFFFu);
  const int32_t sX = (rawX >= 32768) ? rawX - 65536 : rawX;
  const int32_t sY = (rawY >= 32768) ? rawY - 65536 : rawY;

  float ox = std::max(-1.0f, std::min(1.0f, static_cast<float>(sX) / 32767.0f));
  float oy = std::max(-1.0f, std::min(1.0f, static_cast<float>(sY) / 32767.0f));

  float nx = ox, ny = oy;
  float nz = 1.0f - std::abs(ox) - std::abs(oy);
  if (nz < 0.0f)
  {
    const float oldX = nx, oldY = ny;
    nx = (1.0f - std::abs(oldY)) * (oldX >= 0.0f ? 1.0f : -1.0f);
    ny = (1.0f - std::abs(oldX)) * (oldY >= 0.0f ? 1.0f : -1.0f);
  }
  const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
  if (len > 1e-6f)
  {
    nx /= len;
    ny /= len;
    nz /= len;
  }
  return {nx, ny, nz};
}

inline std::array<float, 2> decodeUV(const uint32_t *words, uint32_t uvDataBase, uint32_t uvOff, uint32_t vertexIdx)
{
  const uint32_t base = uvDataBase + uvOff + vertexIdx * 2u;
  float u, v;
  std::memcpy(&u, &words[base + 0u], sizeof(float));
  std::memcpy(&v, &words[base + 1u], sizeof(float));
  return {u, v};
}

inline uint32_t decodeIndex(const uint32_t *words, uint32_t idxDataBase, uint32_t idxOff, uint32_t indexIdx)
{
  const uint32_t word = words[idxDataBase + idxOff + indexIdx / 4u];
  const uint32_t shift = (indexIdx % 4u) * 8u;
  return (word >> shift) & 0xFFu;
}

struct DecodedMeshlet
{
  std::vector<std::array<float, 3>> positions;
  std::vector<std::array<float, 3>> normals;
  std::vector<std::array<float, 2>> uvs;
  std::vector<uint32_t> indices;
};

static constexpr uint32_t PAGE_HEADER_WORDS = virtualgeometry::PAGE_HEADER_WORDS;
static constexpr uint32_t MESHLET_DESC_WORDS = virtualgeometry::MESHLET_DESC_WORDS;

inline DecodedMeshlet decodeMeshlet(const uint32_t *pageWords, uint32_t pageWordBase, uint32_t meshletLocalIdx, float unitScale)
{
  DecodedMeshlet out;

  const uint32_t numMeshlets = pageWords[pageWordBase + 0u];
  const uint32_t posDataSize = pageWords[pageWordBase + 1u];
  const uint32_t normDataSize = pageWords[pageWordBase + 2u];
  const uint32_t uvDataSize = pageWords[pageWordBase + 3u];

  const uint32_t meshletTableEnd = pageWordBase + PAGE_HEADER_WORDS + numMeshlets * MESHLET_DESC_WORDS;
  const uint32_t posDataBase = meshletTableEnd;
  const uint32_t normDataBase = posDataBase + posDataSize;
  const uint32_t uvDataBase = normDataBase + normDataSize;
  const uint32_t idxDataBase = uvDataBase + uvDataSize;

  uint32_t cumulativeIdxWords = 0u;

  for (uint32_t m = 0u; m <= meshletLocalIdx; ++m)
  {
    const uint32_t descBase = pageWordBase + PAGE_HEADER_WORDS + m * MESHLET_DESC_WORDS;

    const uint32_t idxOff = pageWords[descBase + 6u];
    const uint32_t triCount = pageWords[descBase + 9u];

    assert(
        idxOff == cumulativeIdxWords && "Encoder 4-byte padding invariant violated: "
                                        "start_index_id / 4 does not match padded cumulative index offset. "
                                        "Check VirtualGeometryCompressor::encode index padding.");

    const uint32_t rawBytes = triCount * 3u;
    const uint32_t paddedBytes = (rawBytes + 3u) & ~3u;
    cumulativeIdxWords += paddedBytes / 4u;
  }

  const uint32_t descBase = pageWordBase + PAGE_HEADER_WORDS + meshletLocalIdx * MESHLET_DESC_WORDS;

  const uint32_t posOff = pageWords[descBase + 0u];
  const uint32_t normOff = pageWords[descBase + 2u];
  const uint32_t uvOff = pageWords[descBase + 4u];
  const uint32_t idxOff = pageWords[descBase + 6u];
  const uint32_t vtxCount = pageWords[descBase + 8u];
  const uint32_t triCount = pageWords[descBase + 9u];
  const uint32_t bitsX = pageWords[descBase + 10u];
  const uint32_t bitsY = pageWords[descBase + 11u];
  const uint32_t bitsZ = pageWords[descBase + 12u];
  const uint32_t qFactor = pageWords[descBase + 13u];

  float minQX, minQY, minQZ;
  std::memcpy(&minQX, &pageWords[descBase + 14u], sizeof(float));
  std::memcpy(&minQY, &pageWords[descBase + 15u], sizeof(float));
  std::memcpy(&minQZ, &pageWords[descBase + 16u], sizeof(float));

  // Decode indices
  const uint32_t numCorners = triCount * 3u;
  out.indices.resize(numCorners);
  for (uint32_t c = 0u; c < numCorners; ++c)
    out.indices[c] = decodeIndex(pageWords, idxDataBase, idxOff, c);

  // Decode per-vertex attributes
  out.positions.resize(vtxCount);
  out.normals.resize(vtxCount);
  out.uvs.resize(vtxCount);

  for (uint32_t v = 0u; v < vtxCount; ++v)
  {
    out.positions[v] = decodePosition(pageWords, posDataBase, posOff, bitsX, bitsY, bitsZ, qFactor, minQX, minQY, minQZ, unitScale, v);
    out.normals[v] = decodeNormal(pageWords, normDataBase, normOff, v);
    out.uvs[v] = decodeUV(pageWords, uvDataBase, uvOff, v);
  }

  return out;
}

inline std::array<float, 3> transformPoint(const float m[16], float x, float y, float z)
{
  const float w = m[3 * 4 + 0] * x + m[3 * 4 + 1] * y + m[3 * 4 + 2] * z + m[3 * 4 + 3];
  const float iw = (w != 0.f) ? 1.f / w : 1.f;
  return {
    (m[0 * 4 + 0] * x + m[0 * 4 + 1] * y + m[0 * 4 + 2] * z + m[0 * 4 + 3]) * iw,
    (m[1 * 4 + 0] * x + m[1 * 4 + 1] * y + m[1 * 4 + 2] * z + m[1 * 4 + 3]) * iw,
    (m[2 * 4 + 0] * x + m[2 * 4 + 1] * y + m[2 * 4 + 2] * z + m[2 * 4 + 3]) * iw,
  };
}

} // namespace cpu_shader_mirror

// ============================================================================
// Misc helpers
// ============================================================================

static void computeHierarchyAABB(const std::vector<VirtualGeometryHierarchy> &nodes, math::Vec3f &outMin, math::Vec3f &outMax)
{
  outMin = math::Vec3f(1e30f, 1e30f, 1e30f);
  outMax = math::Vec3f(-1e30f, -1e30f, -1e30f);
  for (const auto &n : nodes)
  {
    if (outMin[0] > n.min_x)
      outMin[0] = n.min_x;
    if (outMin[1] > n.min_y)
      outMin[1] = n.min_y;
    if (outMin[2] > n.min_z)
      outMin[2] = n.min_z;
    if (outMax[0] < n.max_x)
      outMax[0] = n.max_x;
    if (outMax[1] < n.max_y)
      outMax[1] = n.max_y;
    if (outMax[2] < n.max_z)
      outMax[2] = n.max_z;
  }
}

struct TestResult
{
  std::string meshName;
  uint32_t hwVisibleClusters = 0;
  uint32_t swVisibleClusters = 0;
  uint32_t pagesWithPriority = 0;
  uint32_t clustersDecoded = 0;
  uint32_t clustersMatchingReference = 0;
  uint32_t clustersAllVerticesInAABB = 0;
  uint32_t clustersAnyVertexInFrustum = 0;
  // Padding/nodeIndex validation: counts how many visible clusters have a
  // _padding value that resolves to a valid leaf hierarchy node whose
  // pageIndex matches the cluster's own pageIndex.
  uint32_t clustersWithValidNodeIndexInPadding = 0;
  bool success = false;
};

static std::string sentinelOrInt(uint32_t v)
{
  return std::to_string(v);
}

static void logHierarchyNodes(const std::string &objName, const std::vector<VirtualGeometryHierarchy> &nodes, const std::vector<PageTableEntry> &pageTable)
{
  std::ostringstream ss;
  ss << "\n--- Hierarchy Nodes for " << objName << " (" << nodes.size() << " total) ---\n";
  for (uint32_t i = 0; i < static_cast<uint32_t>(nodes.size()); ++i)
  {
    const auto &n = nodes[i];
    const bool isLeaf = (n.flags & 1u) != 0u;
    const bool hasPage = (n.pageIndex != SENTINEL_VALUE);
    const bool installed = hasPage && (n.pageIndex < static_cast<uint32_t>(pageTable.size())) && (pageTable[n.pageIndex].isInstalled != 0u);
    ss << "  node[" << i << "]\n"
       << "    aabb      : min=(" << n.min_x << ", " << n.min_y << ", " << n.min_z << ") max=(" << n.max_x << ", " << n.max_y << ", " << n.max_z << ")\n"
       << "    center    : (" << n.max_center_x << ", " << n.max_center_y << ", " << n.max_center_z << ")\n"
       << "    radius    : " << n.max_radius << "\n"
       << "    lod_error : min=" << n.min_lod_error << "  max_parent=" << n.max_parent_lod_error << "\n"
       << "    children  : start=" << sentinelOrInt(n.child_start) << "  count=" << n.child_count << "\n"
       << "    pageIndex : " << sentinelOrInt(n.pageIndex) << "  installed=" << (hasPage ? (installed ? "yes" : "no") : "n/a") << "\n"
       << "    flags     : 0x" << std::hex << n.flags << std::dec << (isLeaf ? "  [LEAF]" : "  [INNER]") << "\n";
  }
  os::Logger::log(ss.str());
}

// Logs queue elements.  For the cluster queue, _padding holds the parent leaf
// nodeIndex stashed by processHierarchyNodes; for hierarchy queues it holds
// the parent inner nodeIndex (or 0 for root elements).
static void logHierarchyQueue(const std::string &label, const std::vector<QueueElement> &elements, uint32_t validCount, bool isClusterQueue = false)
{
  std::ostringstream ss;
  ss << "\n--- " << label << " (reported valid=" << validCount << ", buffer capacity=" << elements.size() << ") ---\n";
  uint32_t nonSentinel = 0u;
  for (uint32_t i = 0; i < validCount; ++i)
  {
    const auto &e = elements[i];
    if (e.index == SENTINEL_VALUE && e.instanceIndex == SENTINEL_VALUE && e.pageIndex & (1 << 31))
      break;
    ss << "  [" << i << "]"
       << "  index=" << sentinelOrInt(e.index) << "  instanceIndex=" << sentinelOrInt(e.instanceIndex) << "  pageIndex=" << sentinelOrInt(e.pageIndex);
    if (isClusterQueue)
      ss << "  parentNodeIndex=" << sentinelOrInt(e._padding);
    else
      ss << "  parentNodeIndex=" << sentinelOrInt(e._padding);
    ss << "\n";
    ++nonSentinel;
  }
  if (nonSentinel == 0u)
    ss << "  (empty)\n";
  os::Logger::log(ss.str());
}

static void logVisibleClusters(const std::string &label, const std::vector<VisibleClusterInfo> &list, const std::vector<VirtualGeometryHierarchy> &hierarchyNodes)
{
  std::ostringstream ss;
  ss << "\n--- " << label << " (" << list.size() << " clusters) ---\n";
  for (uint32_t i = 0; i < static_cast<uint32_t>(list.size()); ++i)
  {
    const auto &vc = list[i];
    // _padding stores the parent leaf nodeIndex set by processClusters (via
    // the clusterQueue element's _padding which was written by processHierarchyNodes).
    const uint32_t nodeIdx = vc._padding;
    std::string nodeDesc = sentinelOrInt(nodeIdx);
    if (nodeIdx != SENTINEL_VALUE && nodeIdx < static_cast<uint32_t>(hierarchyNodes.size()))
    {
      const auto &n = hierarchyNodes[nodeIdx];
      const bool isLeaf = (n.flags & 1u) != 0u;
      nodeDesc += isLeaf ? " [LEAF]" : " [INNER — unexpected!]";
    }
    ss << "  [" << i << "]"
       << "  pageIndex=" << sentinelOrInt(vc.pageIndex) << "  pageLocalClusterIdx=" << sentinelOrInt(vc.pageLocalClusterIndex) << "  instanceIndex=" << sentinelOrInt(vc.instanceIndex) << "  parentNodeIndex=" << nodeDesc
       << "\n";
  }
  os::Logger::log(ss.str());
}

static void logDrawIndirectCommands(const std::string &label, const std::vector<DrawIndirectCommand> &cmds, uint32_t clusterCount)
{
  std::ostringstream ss;
  ss << "\n--- " << label << " DrawIndirectCommands"
     << " (written=" << clusterCount << ", buffer capacity=" << cmds.size() << ") ---\n";
  if (clusterCount == 0u)
  {
    ss << "  (none)\n";
    os::Logger::log(ss.str());
    return;
  }

  uint64_t totalVertices = 0u, totalTriangles = 0u;
  for (uint32_t i = 0; i < clusterCount && i < static_cast<uint32_t>(cmds.size()); ++i)
  {
    const auto &cmd = cmds[i];
    totalVertices += cmd.vertexCount;
    totalTriangles += cmd.vertexCount / 3u;
    ss << "  draw[" << i << "]"
       << "  vtxCount=" << cmd.vertexCount << "  instCount=" << cmd.instanceCount << "  firstVtx=" << cmd.firstVertex << "  firstInst=" << cmd.firstInstance << "  (tris=" << (cmd.vertexCount / 3u) << ")\n";
    if (cmd.instanceCount != 1u)
      ss << "    WARNING: instanceCount != 1\n";
    if (cmd.firstVertex != 0u)
      ss << "    WARNING: firstVertex != 0\n";
    if (cmd.firstInstance != i)
      ss << "    WARNING: firstInstance mismatch\n";
    if (cmd.vertexCount == 0u)
      ss << "    WARNING: vertexCount == 0\n";
  }
  ss << "  TOTAL: vertices=" << totalVertices << "  triangles=" << totalTriangles << "\n";
  os::Logger::log(ss.str());
}

static void logPagePriorities(const std::string &objName, const std::vector<uint32_t> &pagePriorities, const std::vector<PageTableEntry> &pageTable)
{
  std::ostringstream ss;
  ss << "\n--- Page Priorities for " << objName << " (" << pagePriorities.size() << " pages) ---\n";
  for (uint32_t pi = 0; pi < static_cast<uint32_t>(pagePriorities.size()); ++pi)
  {
    const bool installed = (pi < static_cast<uint32_t>(pageTable.size())) && (pageTable[pi].isInstalled != 0u);
    ss << "  page[" << pi << "]"
       << "  priority=" << pagePriorities[pi] << "  installed=" << (installed ? "yes" : "no");
    if (pagePriorities[pi] > 0u)
      ss << "  <-- has priority";
    ss << "\n";
  }
  os::Logger::log(ss.str());
}

static void logQueueState(const std::string &objName, const CullingCounters &c)
{
  std::ostringstream ss;
  ss << "\n--- Queue State for " << objName << " ---\n"
     << "  hierarchyQueueSize          : " << c.hierarchyQueueSize << "\n"
     << "  clusterQueueSize            : " << c.clusterQueueSize << "\n"
     << "  visibleClusterHardwareCount : " << c.visibleClusterHardwareCount << "\n"
     << "  visibleClusterSoftwareCount : " << c.visibleClusterSoftwareCount << "\n";
  os::Logger::log(ss.str());
}

static bool validateVisibleClusters(const std::string &label, const std::vector<VisibleClusterInfo> &list, uint32_t pageCount, const std::vector<PageTableEntry> &pageTable)
{
  for (uint32_t i = 0; i < static_cast<uint32_t>(list.size()); ++i)
  {
    const auto &vc = list[i];
    if ((vc.pageIndex & (1 << 31)) || vc.pageLocalClusterIndex == SENTINEL_VALUE || vc.instanceIndex == SENTINEL_VALUE)
    {
      os::Logger::errorf("  ERROR [%s]: visible cluster[%u] contains sentinel values", label.c_str(), i);
      return false;
    }
    if (vc.pageIndex >= pageCount || pageTable[vc.pageIndex].isInstalled == 0u)
    {
      os::Logger::errorf("  ERROR [%s]: visible cluster[%u] references uninstalled page %u", label.c_str(), i, vc.pageIndex);
      return false;
    }
  }
  return true;
}

// Validates that the _padding field of each VisibleClusterInfo resolves to a
// valid leaf hierarchy node whose pageIndex matches the cluster's own pageIndex.
// Returns the number of clusters that passed this check.
static uint32_t validateAndCountVisibleClusterNodeIndexPadding(const std::string &label, const std::vector<VisibleClusterInfo> &list, const std::vector<VirtualGeometryHierarchy> &hierarchyNodes, bool &outAnyError)
{
  uint32_t validCount = 0u;
  const uint32_t nodeCount = static_cast<uint32_t>(hierarchyNodes.size());

  for (uint32_t i = 0; i < static_cast<uint32_t>(list.size()); ++i)
  {
    const auto &vc = list[i];
    const uint32_t nodeIdx = vc._padding;

    if (nodeIdx == SENTINEL_VALUE)
    {
      os::Logger::errorf(
          "  ERROR [%s]: visible cluster[%u] _padding (parentNodeIndex) is SENTINEL"
          " — processClusters did not propagate the nodeIndex correctly",
          label.c_str(),
          i);
      outAnyError = true;
      continue;
    }

    if (nodeIdx >= nodeCount)
    {
      os::Logger::errorf(
          "  ERROR [%s]: visible cluster[%u] _padding (parentNodeIndex) = %u"
          " is out of range (hierarchy size = %u)",
          label.c_str(),
          i,
          nodeIdx,
          nodeCount);
      outAnyError = true;
      continue;
    }

    const auto &node = hierarchyNodes[nodeIdx];
    const bool isLeaf = (node.flags & 1u) != 0u;
    if (!isLeaf)
    {
      os::Logger::errorf(
          "  ERROR [%s]: visible cluster[%u] _padding (parentNodeIndex) = %u"
          " points to a non-leaf node (flags=0x%x) — expected a leaf",
          label.c_str(),
          i,
          nodeIdx,
          node.flags);
      outAnyError = true;
      continue;
    }

    if (node.pageIndex != vc.pageIndex)
    {
      os::Logger::errorf(
          "  ERROR [%s]: visible cluster[%u] _padding (parentNodeIndex) = %u"
          " leaf node has pageIndex=%u but cluster's pageIndex=%u — mismatch",
          label.c_str(),
          i,
          nodeIdx,
          node.pageIndex,
          vc.pageIndex);
      outAnyError = true;
      continue;
    }

    // Check the cluster's page-local index falls within the node's child range.
    if (node.child_start != SENTINEL_VALUE && (vc.pageLocalClusterIndex < node.child_start || vc.pageLocalClusterIndex >= node.child_start + node.child_count))
    {
      os::Logger::errorf(
          "  ERROR [%s]: visible cluster[%u] pageLocalClusterIndex=%u"
          " is outside parent node[%u] child range [%u, %u)",
          label.c_str(),
          i,
          vc.pageLocalClusterIndex,
          nodeIdx,
          node.child_start,
          node.child_start + node.child_count);
      outAnyError = true;
      continue;
    }

    ++validCount;
  }
  return validCount;
}

static bool validateDrawIndirectCommands(const std::string &label, const std::vector<DrawIndirectCommand> &cmds, uint32_t clusterCount)
{
  bool valid = true;
  for (uint32_t i = 0; i < clusterCount && i < static_cast<uint32_t>(cmds.size()); ++i)
  {
    const auto &cmd = cmds[i];
    if (cmd.instanceCount != 1u)
    {
      os::Logger::errorf("  ERROR [%s]: draw[%u] instanceCount=%u", label.c_str(), i, cmd.instanceCount);
      valid = false;
    }
    if (cmd.firstVertex != 0u)
    {
      os::Logger::errorf("  ERROR [%s]: draw[%u] firstVertex=%u", label.c_str(), i, cmd.firstVertex);
      valid = false;
    }
    if (cmd.firstInstance != i)
    {
      os::Logger::errorf("  ERROR [%s]: draw[%u] firstInstance=%u expected %u", label.c_str(), i, cmd.firstInstance, i);
      valid = false;
    }
    if (cmd.vertexCount == 0u)
    {
      os::Logger::errorf("  ERROR [%s]: draw[%u] vertexCount=0", label.c_str(), i);
      valid = false;
    }
    if (cmd.vertexCount % 3u != 0u)
    {
      os::Logger::errorf("  ERROR [%s]: draw[%u] vertexCount=%u not multiple of 3", label.c_str(), i, cmd.vertexCount);
      valid = false;
    }
  }
  return valid;
}

// ============================================================================
// Main test function
// ============================================================================

TestResult runTest(rendering::RHI *rhi, const std::string &meshPath, const std::string &objName)
{
  TestResult result;
  result.meshName = objName;

  os::Logger::logf("\n==================================================");
  os::Logger::logf("Testing: %s", objName.c_str());
  os::Logger::logf("==================================================");

  // -------------------------------------------------------------------------
  // [1] Encode
  // -------------------------------------------------------------------------
  os::Logger::logf("[1] Encoding %s...", objName.c_str());

  std::vector<Vertex> vertices;
  Shape shape;
  VirtualGeometryEncoder::loadOBJ(meshPath, true, vertices, shape);

  VirtualGeometryBuildData build = VirtualGeometryBuilder::build(VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape));

  QuantizationConfig qcfg;
  qcfg.quantization_factor = 4;
  qcfg.unit_scale = 100.0f;

  VirtualGeometryEncodedData encoded = VirtualGeometryEncoder::encode(vertices, shape, qcfg);

  std::string vgPath = meshPath + ".virtualgeometry";
  {
    VirtualGeometryFile writer(vgPath, true);
    if (!writer.isOpen())
      throw std::runtime_error("Cannot open VG file for writing: " + vgPath);
    if (!writer.write(encoded, build.pages, MESHLET_LZ4))
      throw std::runtime_error("Failed to write VG file: " + vgPath);
  }

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf("  Pages: %u  Hierarchy nodes: %u", pageCount, hierarchySize);

  // -------------------------------------------------------------------------
  // [2] Scene
  // -------------------------------------------------------------------------
  os::Logger::log("[2] Building VirtualGeometryScene...");

  const uint32_t kHierarchyBufBytes = std::max(hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
  const uint64_t kPagesBufBytes = 256ull * 1024 * 1024;

  os::Logger::log("[3] Creating render graph...");

  RenderGraph *renderGraph = new RenderGraph(rhi);

  VirtualGeometryScene scene(renderGraph, kHierarchyBufBytes, kPagesBufBytes);
  scene.registerObjectForStreaming(objName, vgPath);

  // Read back hierarchy to position the camera.
  std::vector<VirtualGeometryHierarchy> cpuHierarchy(hierarchySize);
  renderGraph->bufferRead(
      scene.hierarchyBuffer,
      0,
      hierarchySize * sizeof(VirtualGeometryHierarchy),
      [&](const void *data)
      {
        std::memcpy(cpuHierarchy.data(), data, hierarchySize * sizeof(VirtualGeometryHierarchy));
      });

  math::Vec3f aabbMin, aabbMax;
  computeHierarchyAABB(cpuHierarchy, aabbMin, aabbMax);
  math::Vec3f center = (aabbMin + aabbMax) * 0.5f;
  float radius = (aabbMax - aabbMin).length() * 0.5f;

  float camDist = radius * 3.0f + 1.0f;
  math::Vec3f camPos(center[0], center[1], center[2] + camDist);
  math::Vec3f camForward(0.0f, 0.0f, -1.0f);

  constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
  constexpr float kAspect = 16.0f / 9.0f;
  constexpr float kNear = 0.1f;
  constexpr float kFar = 10000.0f;
  constexpr uint32_t kVW = 1920u;
  constexpr uint32_t kVH = 1080u;
  constexpr float kError = 1.0f;
  constexpr uint32_t kHiZLvls = 1u;

  rendering::Camera cam(kFovY, camPos, camForward, true);
  cam.setAspectRatio(kAspect);
  cam.setNearFar(kNear, kFar);
  cam.lookAt(center);
  cam.updateMatrices();

  const math::Vec3f &vp = cam.getPosition();
  os::Logger::logf("  Camera pos:    (%.3f, %.3f, %.3f)", vp[0], vp[1], vp[2]);
  os::Logger::logf("  Object centre: (%.3f, %.3f, %.3f)", center[0], center[1], center[2]);
  os::Logger::logf("  Object radius: %.3f", radius);

  // -------------------------------------------------------------------------
  // [4] Instance
  // -------------------------------------------------------------------------
  os::Logger::log("[4] Creating instance...");

  InstanceId instId = scene.instantiateObjectInstance(objName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
  scene.updateInstanceBuffer();

  // -------------------------------------------------------------------------
  // [5] Staging buffers
  // -------------------------------------------------------------------------
  os::Logger::log("[5] Creating staging buffers...");

  const uint64_t hwSwClusterInfoBytes = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
  const uint64_t hwSwDrawIndirectBytes = static_cast<uint64_t>(VirtualGeometryScene::MAX_VISIBLE_CLUSTERS) * sizeof(DrawIndirectCommand);
  const uint64_t priorityBufBytes = scene.getMaxPagesInScene() * sizeof(uint32_t);
  const uint64_t hierarchyNodeBytes = hierarchySize * sizeof(VirtualGeometryHierarchy);
  const uint64_t hierarchyQueueBytes = 1024u * 1024u * sizeof(QueueElement);

  auto makeStaging = [&](const std::string &name, uint64_t size)
  {
    return renderGraph->createBuffer(
        BufferInfo{
          .name = objName + "_" + name,
          .size = size,
          .usage = BufferUsage::BufferUsage_CopyDst | BufferUsage::BufferUsage_Pull,
        });
  };

  Buffer hwVisibleInfoStaging = makeStaging("HWVisibleInfoStaging.buffer", hwSwClusterInfoBytes);
  Buffer hwDrawIndirectStaging = makeStaging("HWDrawIndirectStaging.buffer", hwSwDrawIndirectBytes);
  Buffer swVisibleInfoStaging = makeStaging("SWVisibleInfoStaging.buffer", hwSwClusterInfoBytes);
  Buffer swDrawIndirectStaging = makeStaging("SWDrawIndirectStaging.buffer", hwSwDrawIndirectBytes);
  Buffer priorityStaging = makeStaging("PriorityStaging.buffer", priorityBufBytes);
  Buffer hierarchyQueueAStaging = makeStaging("HierarchyQueueAStaging.buffer", hierarchyQueueBytes);
  Buffer hierarchyQueueBStaging = makeStaging("HierarchyQueueBStaging.buffer", hierarchyQueueBytes);
  Buffer hierarchyNodeStaging = makeStaging("HierarchyNodeStaging.buffer", hierarchyNodeBytes);

  // -------------------------------------------------------------------------
  // [6] Pass
  // -------------------------------------------------------------------------
  VirtualGeometryCullingMultipleDispatchesPass::Settings passSettings;
  passSettings.maxHierarchyLevels = 3;
  passSettings.clustersQueueBufferSize = MAX_QUEUE_ELEMENTS * sizeof(QueueElement);
  passSettings.errorTreshhold = kError;
  passSettings.viewPortWidth = kVW;
  passSettings.viewPortHeight = kVH;

  rendering::Texture dummyDepth = renderGraph->createTexture(
      TextureInfo{
        .name = objName + "_DummyDepth.texture",
        .format = rendering::Format::Format_Depth32Float,
        .height = kVH,
        .width = kVW,
        .memoryProperties = rendering::BufferUsage::BufferUsage_Storage,
        .mipLevels = 1u,
        .usage = rendering::ImageUsage::ImageUsage_Storage,
      });

  os::Logger::log("[6] Registering pass...");
  auto *cullingPass = renderGraph->registerPass<VirtualGeometryCullingMultipleDispatchesPass>(objName + "_cullingPass", 0, scene, passSettings, dummyDepth);

  os::Logger::log("[7] Uploading uniforms...");
  {
    const math::Mat4f &view = cam.getViewMatrix();
    const math::Mat4f &proj = cam.getProjectionMatrix();
    float viewArr[16], projArr[16], vpArr[4];
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
      {
        viewArr[r * 4 + c] = view.at(r, c);
        projArr[r * 4 + c] = proj.at(r, c);
      }
    vpArr[0] = vp[0];
    vpArr[1] = vp[1];
    vpArr[2] = vp[2];
    vpArr[3] = 1.0f;
    cullingPass->updateUniforms(viewArr, projArr, vpArr, kVW, kVH, kNear, kFar, kHiZLvls);
  }

  // Copy passes (order 1..8).
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHWVisibleClusters", 1, cullingPass->getHWVisibleClusterInfosBuffer(), 0, hwSwClusterInfoBytes, hwVisibleInfoStaging, 0, hwSwClusterInfoBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHWDrawIndirect", 2, cullingPass->getHWDrawIndirectBuffer(), 0, hwSwDrawIndirectBytes, hwDrawIndirectStaging, 0, hwSwDrawIndirectBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copySWVisibleClusters", 3, cullingPass->getSWVisibleClusterInfosBuffer(), 0, hwSwClusterInfoBytes, swVisibleInfoStaging, 0, hwSwClusterInfoBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copySWDrawIndirect", 4, cullingPass->getSWDrawIndirectBuffer(), 0, hwSwDrawIndirectBytes, swDrawIndirectStaging, 0, hwSwDrawIndirectBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyPagePriorities", 5, scene.pagePriorityBuffer, 0, pageCount * sizeof(uint32_t), priorityStaging, 0, pageCount * sizeof(uint32_t));
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyQueueA", 6, cullingPass->getHierarchyQueueA(), 0, hierarchyQueueBytes, hierarchyQueueAStaging, 0, hierarchyQueueBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyQueueB", 7, cullingPass->getHierarchyQueueB(), 0, hierarchyQueueBytes, hierarchyQueueBStaging, 0, hierarchyQueueBytes);
  renderGraph->registerPass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyNodes", 8, scene.hierarchyBuffer, 0, hierarchyNodeBytes, hierarchyNodeStaging, 0, hierarchyNodeBytes);

  os::Logger::log("[8] Compiling and running render graph...");
  renderGraph->compile();

  RenderGraph::Frame frame;
  RenderGraph::Overrides overrides;
  renderGraph->run(frame, overrides);
  renderGraph->waitFrame(frame);

  // -------------------------------------------------------------------------
  // Readback counters
  // -------------------------------------------------------------------------
  CullingCounters counters{};
  renderGraph->bufferRead(
      cullingPass->getCullingStatisticsBuffer(),
      0,
      sizeof(CullingCounters),
      [&](const void *d)
      {
        std::memcpy(&counters, d, sizeof(CullingCounters));
      });

  logQueueState(objName, counters);

  const uint32_t hwCount = counters.visibleClusterHardwareCount;
  const uint32_t swCount = counters.visibleClusterSoftwareCount;
  os::Logger::logf("  HW visible cluster count: %u", hwCount);
  os::Logger::logf("  SW visible cluster count: %u", swCount);

  // -------------------------------------------------------------------------
  // Readback HW visible clusters + draw commands
  // -------------------------------------------------------------------------
  std::vector<VisibleClusterInfo> hwVisibleList(hwCount);
  if (hwCount > 0u)
    renderGraph->bufferRead(
        hwVisibleInfoStaging,
        0,
        hwCount * sizeof(VisibleClusterInfo),
        [&](const void *d)
        {
          std::memcpy(hwVisibleList.data(), d, hwCount * sizeof(VisibleClusterInfo));
        });

  const uint32_t maxDrawCmdCapacity = static_cast<uint32_t>(hwSwDrawIndirectBytes / sizeof(DrawIndirectCommand));
  std::vector<DrawIndirectCommand> hwDrawCmds(maxDrawCmdCapacity);
  if (hwCount > 0u)
    renderGraph->bufferRead(
        hwDrawIndirectStaging,
        0,
        hwCount * sizeof(DrawIndirectCommand),
        [&](const void *d)
        {
          std::memcpy(hwDrawCmds.data(), d, hwCount * sizeof(DrawIndirectCommand));
        });

  // -------------------------------------------------------------------------
  // Readback SW visible clusters + draw commands
  // -------------------------------------------------------------------------
  std::vector<VisibleClusterInfo> swVisibleList(swCount);
  if (swCount > 0u)
    renderGraph->bufferRead(
        swVisibleInfoStaging,
        0,
        swCount * sizeof(VisibleClusterInfo),
        [&](const void *d)
        {
          std::memcpy(swVisibleList.data(), d, swCount * sizeof(VisibleClusterInfo));
        });

  std::vector<DrawIndirectCommand> swDrawCmds(maxDrawCmdCapacity);
  if (swCount > 0u)
    renderGraph->bufferRead(
        swDrawIndirectStaging,
        0,
        swCount * sizeof(DrawIndirectCommand),
        [&](const void *d)
        {
          std::memcpy(swDrawCmds.data(), d, swCount * sizeof(DrawIndirectCommand));
        });

  // -------------------------------------------------------------------------
  // Readback page priorities + page table
  // -------------------------------------------------------------------------
  std::vector<uint32_t> pagePriorities(pageCount, 0u);
  renderGraph->bufferRead(
      priorityStaging,
      0,
      pageCount * sizeof(uint32_t),
      [&](const void *d)
      {
        std::memcpy(pagePriorities.data(), d, pageCount * sizeof(uint32_t));
      });

  std::vector<PageTableEntry> pageTable(pageCount);
  renderGraph->bufferRead(
      scene.pageTableBuffer,
      0,
      pageCount * sizeof(PageTableEntry),
      [&](const void *d)
      {
        std::memcpy(pageTable.data(), d, pageCount * sizeof(PageTableEntry));
      });

  // -------------------------------------------------------------------------
  // Readback hierarchy queues + nodes
  // -------------------------------------------------------------------------
  const uint32_t queueElementCapacity = static_cast<uint32_t>(hierarchyQueueBytes / sizeof(QueueElement));

  std::vector<QueueElement> hierarchyQueueAData(queueElementCapacity);
  renderGraph->bufferRead(
      hierarchyQueueAStaging,
      0,
      hierarchyQueueBytes,
      [&](const void *d)
      {
        std::memcpy(hierarchyQueueAData.data(), d, hierarchyQueueBytes);
      });

  std::vector<QueueElement> hierarchyQueueBData(queueElementCapacity);
  renderGraph->bufferRead(
      hierarchyQueueBStaging,
      0,
      hierarchyQueueBytes,
      [&](const void *d)
      {
        std::memcpy(hierarchyQueueBData.data(), d, hierarchyQueueBytes);
      });

  std::vector<VirtualGeometryHierarchy> gpuHierarchyNodes(hierarchySize);
  renderGraph->bufferRead(
      hierarchyNodeStaging,
      0,
      hierarchyNodeBytes,
      [&](const void *d)
      {
        std::memcpy(gpuHierarchyNodes.data(), d, hierarchyNodeBytes);
      });

  // -------------------------------------------------------------------------
  // Logging
  // -------------------------------------------------------------------------
  const uint32_t N = passSettings.maxHierarchyLevels;
  const bool lastWrittenIsA = (N > 0u) && (N % 2u == 1u);

  logHierarchyNodes(objName, gpuHierarchyNodes, pageTable);
  logHierarchyQueue("HierarchyQueueA for " + objName + (lastWrittenIsA ? "  [LAST WRITTEN]" : "  [last read]"), hierarchyQueueAData, counters.hierarchyQueueSize, /*isClusterQueue=*/false);
  logHierarchyQueue("HierarchyQueueB for " + objName + (!lastWrittenIsA ? "  [LAST WRITTEN]" : "  [last read]"), hierarchyQueueBData, counters.hierarchyQueueSize, /*isClusterQueue=*/false);
  logVisibleClusters("HW Visible Clusters for " + objName, hwVisibleList, gpuHierarchyNodes);
  logDrawIndirectCommands("HW " + objName, hwDrawCmds, hwCount);
  logVisibleClusters("SW Visible Clusters for " + objName, swVisibleList, gpuHierarchyNodes);
  logDrawIndirectCommands("SW " + objName, swDrawCmds, swCount);
  logPagePriorities(objName, pagePriorities, pageTable);

  // -------------------------------------------------------------------------
  // Structural validation
  // -------------------------------------------------------------------------
  bool valid = true;

  if (!validateVisibleClusters("HW", hwVisibleList, pageCount, pageTable))
    valid = false;
  else if (hwCount > 0u)
    os::Logger::log("  OK [HW]: All visible clusters are valid");
  else
    os::Logger::warning("  WARNING [HW]: no clusters visible (check error threshold / camera)");

  if (!validateDrawIndirectCommands("HW", hwDrawCmds, hwCount))
    valid = false;
  else if (hwCount > 0u)
    os::Logger::log("  OK [HW]: All draw indirect commands are valid");

  if (!validateVisibleClusters("SW", swVisibleList, pageCount, pageTable))
    valid = false;
  else if (swCount > 0u)
    os::Logger::log("  OK [SW]: All visible clusters are valid");
  else
    os::Logger::log("  INFO [SW]: no clusters routed to software rasterizer (expected)");

  if (!validateDrawIndirectCommands("SW", swDrawCmds, swCount))
    valid = false;
  else if (swCount > 0u)
    os::Logger::log("  OK [SW]: All draw indirect commands are valid");

  // ---- Validate _padding (parentNodeIndex) in HW visible clusters ---------
  uint32_t hwPaddingValid = 0u;
  if (hwCount > 0u)
  {
    bool paddingError = false;
    hwPaddingValid = validateAndCountVisibleClusterNodeIndexPadding("HW", hwVisibleList, gpuHierarchyNodes, paddingError);
    if (paddingError)
      valid = false;
    else
      os::Logger::logf("  OK [HW]: All %u visible clusters have a valid parentNodeIndex in _padding", hwPaddingValid);
  }

  uint32_t swPaddingValid = 0u;
  if (swCount > 0u)
  {
    bool paddingError = false;
    swPaddingValid = validateAndCountVisibleClusterNodeIndexPadding("SW", swVisibleList, gpuHierarchyNodes, paddingError);
    if (paddingError)
      valid = false;
    else
      os::Logger::logf("  OK [SW]: All %u visible clusters have a valid parentNodeIndex in _padding", swPaddingValid);
  }

  // =========================================================================
  // CPU-SIDE VERTEX DECODE + GEOMETRIC VALIDATION  (shader-mirror version)
  // =========================================================================

  // ---- [DECODE-1]  CPU frustum planes (Gribb-Hartmann) --------------------
  struct FrustumPlane
  {
    float x, y, z, w;
  };

  const math::Mat4f &cpuView = cam.getViewMatrix();
  const math::Mat4f &cpuProj = cam.getProjectionMatrix();

  auto matMul = [](const math::Mat4f &A, const math::Mat4f &B) -> math::Mat4f
  {
    math::Mat4f C;
    for (int r = 0; r < 4; ++r)
      for (int c = 0; c < 4; ++c)
      {
        float v = 0.f;
        for (int k = 0; k < 4; ++k)
          v += A.at(r, k) * B.at(k, c);
        C.at(r, c) = v;
      }
    return C;
  };

  const math::Mat4f viewProj = matMul(cpuProj, cpuView);

  auto rowOf = [&](int r) -> std::array<float, 4>
  {
    return {viewProj.at(r, 0), viewProj.at(r, 1), viewProj.at(r, 2), viewProj.at(r, 3)};
  };
  auto neg4 = [](std::array<float, 4> a) -> std::array<float, 4>
  {
    return {-a[0], -a[1], -a[2], -a[3]};
  };
  auto makePlane = [&](std::array<float, 4> add, std::array<float, 4> sub) -> FrustumPlane
  {
    FrustumPlane p;
    p.x = add[0] + sub[0];
    p.y = add[1] + sub[1];
    p.z = add[2] + sub[2];
    p.w = add[3] + sub[3];
    float len = std::sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
    if (len > 1e-7f)
    {
      p.x /= len;
      p.y /= len;
      p.z /= len;
      p.w /= len;
    }
    return p;
  };

  auto r0 = rowOf(0), r1 = rowOf(1), r2 = rowOf(2), r3 = rowOf(3);

  const std::array<FrustumPlane, 6> frustumPlanes = {
    makePlane(r3, r0),       // left
    makePlane(r3, neg4(r0)), // right
    makePlane(r3, r1),       // bottom
    makePlane(r3, neg4(r1)), // top
    // NOTE: this is due to reverseZ in Camera
    makePlane(r3, neg4(r2)), // near
    makePlane(r2, {}),       // far
  };

  auto pointInFrustum = [&](float px, float py, float pz) -> bool
  {
    for (const auto &pl : frustumPlanes)
      if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0.f)
        return false;
    return true;
  };

  // 1 % relative tolerance for AABB containment (covers quantization error).
  auto pointInAABB = [](float px, float py, float pz, float mnX, float mnY, float mnZ, float mxX, float mxY, float mxZ) -> bool
  {
    constexpr float kEps = 0.01f;
    auto ext = [&](float sz)
    {
      return kEps * std::max(std::abs(sz), 1e-4f);
    };
    float ex = ext(mxX - mnX), ey = ext(mxY - mnY), ez = ext(mxZ - mnZ);
    return px >= mnX - ex && px <= mxX + ex && py >= mnY - ey && py <= mxY + ey && pz >= mnZ - ez && pz <= mxZ + ez;
  };

  // ---- [DECODE-2]  Open .vg file for reference ----------------------------
  os::Logger::log("[CPU-DECODE] Opening .virtualgeometry file for reference decode...");

  VirtualGeometryFile vgFile(vgPath, /*write=*/false);
  if (!vgFile.isOpen())
  {
    os::Logger::error("  ERROR: cannot re-open .virtualgeometry file");
    valid = false;
  }

  // ---- [DECODE-3]  Read back raw page bytes from GPU pagesBuffer ----------
  os::Logger::log("[CPU-DECODE] Reading back raw page words from GPU pagesBuffer...");

  std::unordered_map<uint32_t, std::vector<uint32_t>> rawGpuPageWords;

  for (uint32_t pi = 0; pi < pageCount; ++pi)
  {
    const PageTableEntry &entry = pageTable[pi];
    if (entry.isInstalled == 0u || entry.size == 0u)
      continue;

    assert(entry.size % sizeof(uint32_t) == 0);
    const uint32_t wordCount = entry.size / sizeof(uint32_t);
    auto &words = rawGpuPageWords[pi];
    words.resize(wordCount);

    renderGraph->bufferRead(
        scene.pagesBuffer,
        entry.bufferOffset,
        entry.size,
        [&words](const void *data)
        {
          std::memcpy(words.data(), data, words.size() * sizeof(uint32_t));
        });
  }

  // ---- [DECODE-4]  Per-cluster validation loop ----------------------------
  os::Logger::log("[CPU-DECODE] Decoding and validating all HW-visible clusters via shader mirror...");

  const float instanceUnitScale = qcfg.unit_scale;

  // Row-major identity.
  constexpr float kIdentity[16] = {
    1,
    0,
    0,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    1,
    0,
    0,
    0,
    0,
    1,
  };

  uint32_t clustersDecoded = 0u;
  uint32_t clustersMatchingReferencePositions = 0u;
  uint32_t clustersWithAllVerticesInAABB = 0u;
  uint32_t clustersWithAtLeastOneVertexVisible = 0u;

  for (uint32_t ci = 0; ci < hwCount; ++ci)
  {
    const VisibleClusterInfo &vc = hwVisibleList[ci];
    const uint32_t pi = vc.pageIndex;
    const uint32_t mi = vc.pageLocalClusterIndex;
    // _padding carries the parent leaf nodeIndex written by processClusters.
    const uint32_t parentNodeIdx = vc._padding;

    // -- Resolve owning leaf node via _padding (preferred) ------------------
    // Fall back to the old linear search if _padding is invalid (e.g. sentinel).
    const VirtualGeometryHierarchy *ownerNode = nullptr;
    if (parentNodeIdx != SENTINEL_VALUE && parentNodeIdx < static_cast<uint32_t>(gpuHierarchyNodes.size()))
    {
      const VirtualGeometryHierarchy &candidate = gpuHierarchyNodes[parentNodeIdx];
      if ((candidate.flags & 1u) && candidate.pageIndex == pi)
        ownerNode = &candidate;
    }

    // Fallback linear search in case _padding is stale or wrong.
    if (ownerNode == nullptr)
    {
      for (const auto &node : gpuHierarchyNodes)
      {
        if (!(node.flags & 1u))
          continue;
        if (node.pageIndex != pi)
          continue;
        if (node.child_start == SENTINEL_VALUE)
          continue;
        if (mi >= node.child_start && mi < node.child_start + node.child_count)
        {
          ownerNode = &node;
          os::Logger::warningf(
              "  WARNING [CPU-DECODE]: cluster[%u] _padding=%s did not directly"
              " resolve to its leaf node; found via linear search instead.",
              ci,
              sentinelOrInt(parentNodeIdx).c_str());
          break;
        }
      }
    }

    // -- Get GPU page words -------------------------------------------------
    auto wordsIt = rawGpuPageWords.find(pi);
    if (wordsIt == rawGpuPageWords.end())
    {
      os::Logger::errorf("  ERROR [CPU-DECODE]: cluster[%u] page %u not in rawGpuPageWords", ci, pi);
      valid = false;
      continue;
    }
    const uint32_t *pageWords = wordsIt->second.data();
    const uint32_t pageWordBase = 0u;

    const cpu_shader_mirror::DecodedMeshlet decoded = cpu_shader_mirror::decodeMeshlet(pageWords, pageWordBase, mi, instanceUnitScale);

    if (decoded.positions.empty())
    {
      os::Logger::errorf("  ERROR [CPU-DECODE]: cluster[%u] decodeMeshlet returned empty positions", ci);
      valid = false;
      continue;
    }

    ++clustersDecoded;
    const uint32_t vtxCount = static_cast<uint32_t>(decoded.positions.size());

    // Log the first cluster in detail.
    if (ci == 0u)
    {
      std::ostringstream ss;
      ss << "\n--- Shader-mirror decoded cluster[0]"
         << "  page=" << pi << "  meshlet=" << mi << "  parentNodeIndex=" << sentinelOrInt(parentNodeIdx) << "  vtxCount=" << vtxCount << "  triCount=" << (decoded.indices.size() / 3u) << " ---\n";
      const uint32_t logV = std::min(vtxCount, 8u);
      for (uint32_t v = 0; v < logV; ++v)
      {
        const auto &p = decoded.positions[v];
        const auto &n = decoded.normals[v];
        const auto &uv = decoded.uvs[v];
        ss << "  v[" << v << "]"
           << "  pos=(" << p[0] << ", " << p[1] << ", " << p[2] << ")"
           << "  norm=(" << n[0] << ", " << n[1] << ", " << n[2] << ")"
           << "  uv=(" << uv[0] << ", " << uv[1] << ")\n";
      }
      if (logV < vtxCount)
        ss << "  ... (" << (vtxCount - logV) << " more)\n";
      ss << "  indices[0..5]:";
      for (uint32_t k = 0; k < std::min<uint32_t>(6, decoded.indices.size()); ++k)
        ss << " " << decoded.indices[k];
      ss << "\n";
      os::Logger::log(ss.str());
    }

    // ---- [TEST A] Normals are unit length ---------------------------------
    for (uint32_t v = 0; v < vtxCount; ++v)
    {
      const auto &n = decoded.normals[v];
      const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (std::abs(len - 1.0f) > 1e-3f)
      {
        os::Logger::errorf("  ERROR [CPU-DECODE]: cluster[%u] normal[%u] length=%.6f (expected ~1.0)", ci, v, len);
        valid = false;
      }
    }

    // ---- [TEST B] Compare positions against VirtualGeometryStreamedPage --
    bool positionMatchOk = true;
    if (vgFile.isOpen())
    {
      const uint32_t maxPageSz = vgFile.getMaxPageSize();
      std::vector<uint8_t> fileStagingBuf(maxPageSz);
      VirtualGeometryStreamedPage fileStreamedPage;

      if (vgFile.streamPageRaw(pi, fileStagingBuf.data(), maxPageSz, fileStreamedPage))
      {
        // -- Positions --
        std::vector<float> refPositions;
        fileStreamedPage.decodePositions(mi, refPositions, qcfg);

        if (refPositions.size() != vtxCount * 3u)
        {
          os::Logger::errorf(
              "  ERROR [CPU-DECODE]: cluster[%u] position count:"
              " shader-mirror=%u  streamedPage=%zu",
              ci,
              vtxCount * 3u,
              refPositions.size());
          positionMatchOk = false;
          valid = false;
        }
        else
        {
          constexpr float kPosTol = 1e-4f;
          uint32_t mismatches = 0u;
          for (uint32_t v = 0; v < vtxCount; ++v)
          {
            const float sx = decoded.positions[v][0];
            const float sy = decoded.positions[v][1];
            const float sz = decoded.positions[v][2];
            const float rx = refPositions[v * 3 + 0];
            const float ry = refPositions[v * 3 + 1];
            const float rz = refPositions[v * 3 + 2];

            if (std::abs(sx - rx) > kPosTol || std::abs(sy - ry) > kPosTol || std::abs(sz - rz) > kPosTol)
            {
              ++mismatches;
              if (mismatches <= 3u)
                os::Logger::errorf(
                    "  ERROR [CPU-DECODE]: cluster[%u] vertex[%u]"
                    " shader=(%.6f,%.6f,%.6f)"
                    " ref=(%.6f,%.6f,%.6f)"
                    " delta=(%.6f,%.6f,%.6f)",
                    ci,
                    v,
                    sx,
                    sy,
                    sz,
                    rx,
                    ry,
                    rz,
                    sx - rx,
                    sy - ry,
                    sz - rz);
            }
          }
          if (mismatches > 0u)
          {
            os::Logger::errorf("  ERROR [CPU-DECODE]: cluster[%u] %u/%u position mismatches", ci, mismatches, vtxCount);
            positionMatchOk = false;
            valid = false;
          }
        }

        // -- UVs (stored as raw f32 bits → must be bit-exact) --
        {
          std::vector<float> refUVs;
          fileStreamedPage.decodeUVs(mi, refUVs);
          if (refUVs.size() == vtxCount * 2u)
          {
            for (uint32_t v = 0; v < vtxCount; ++v)
            {
              if (decoded.uvs[v][0] != refUVs[v * 2 + 0] || decoded.uvs[v][1] != refUVs[v * 2 + 1])
              {
                os::Logger::errorf(
                    "  ERROR [CPU-DECODE]: cluster[%u] UV[%u]"
                    " shader=(%.6f,%.6f) ref=(%.6f,%.6f)",
                    ci,
                    v,
                    decoded.uvs[v][0],
                    decoded.uvs[v][1],
                    refUVs[v * 2 + 0],
                    refUVs[v * 2 + 1]);
                positionMatchOk = false;
                valid = false;
              }
            }
          }
        }

        // -- Indices (must be bit-exact) --
        {
          std::vector<uint8_t> refIndices;
          fileStreamedPage.decodeIndices(mi, refIndices);
          if (refIndices.size() == decoded.indices.size())
          {
            for (uint32_t k = 0; k < static_cast<uint32_t>(decoded.indices.size()); ++k)
            {
              if (decoded.indices[k] != static_cast<uint32_t>(refIndices[k]))
              {
                os::Logger::errorf(
                    "  ERROR [CPU-DECODE]: cluster[%u] index[%u]"
                    " shader=%u ref=%u",
                    ci,
                    k,
                    decoded.indices[k],
                    refIndices[k]);
                positionMatchOk = false;
                valid = false;
                break;
              }
            }
          }
        }
      }
      else
      {
        os::Logger::errorf("  ERROR [CPU-DECODE]: cluster[%u] streamPageRaw failed (pi=%u)", ci, pi);
        valid = false;
      }

      if (positionMatchOk)
        ++clustersMatchingReferencePositions;
    }

    // ---- [TEST C] All vertices inside owner hierarchy node AABB -----------
    bool allInAABB = true;
    if (ownerNode != nullptr)
    {
      for (uint32_t v = 0; v < vtxCount; ++v)
      {
        const float px = decoded.positions[v][0];
        const float py = decoded.positions[v][1];
        const float pz = decoded.positions[v][2];

        if (!pointInAABB(px, py, pz, ownerNode->min_x, ownerNode->min_y, ownerNode->min_z, ownerNode->max_x, ownerNode->max_y, ownerNode->max_z))
        {
          if (allInAABB)
            os::Logger::errorf(
                "  ERROR [CPU-DECODE]: cluster[%u] vertex[%u]"
                " (%.4f,%.4f,%.4f) outside node AABB"
                " [(%.4f,%.4f,%.4f)-(%.4f,%.4f,%.4f)]",
                ci,
                v,
                px,
                py,
                pz,
                ownerNode->min_x,
                ownerNode->min_y,
                ownerNode->min_z,
                ownerNode->max_x,
                ownerNode->max_y,
                ownerNode->max_z);
          allInAABB = false;
          valid = false;
        }
      }
      if (allInAABB)
        ++clustersWithAllVerticesInAABB;
    }
    else
    {
      os::Logger::warningf(
          "  WARNING [CPU-DECODE]: cluster[%u] page=%u meshlet=%u parentNodeIndex=%s:"
          " no owning leaf node found (skipping AABB test)",
          ci,
          pi,
          mi,
          sentinelOrInt(parentNodeIdx).c_str());
    }

    // ---- [TEST D] Apply model matrix, test frustum visibility -------------
    bool anyVertexVisible = false;
    for (uint32_t v = 0; v < vtxCount && !anyVertexVisible; ++v)
    {
      const float lx = decoded.positions[v][0];
      const float ly = decoded.positions[v][1];
      const float lz = decoded.positions[v][2];

      const auto wp = cpu_shader_mirror::transformPoint(kIdentity, lx, ly, lz);

      if (pointInFrustum(wp[0], wp[1], wp[2]))
        anyVertexVisible = true;
    }

    if (!anyVertexVisible)
    {
      os::Logger::warningf(
          "  WARNING [CPU-DECODE]: cluster[%u] page=%u meshlet=%u parentNodeIndex=%s:"
          " no world-space vertex passed the CPU frustum test"
          " (cluster AABB may straddle a frustum plane)",
          ci,
          pi,
          mi,
          sentinelOrInt(parentNodeIdx).c_str());
    }
    else
    {
      ++clustersWithAtLeastOneVertexVisible;
    }
  } // end per-cluster loop

  // ---- Summary -------------------------------------------------------------
  {
    std::ostringstream ss;
    ss << "\n--- CPU-Decode (shader-mirror) Summary for " << objName << " ---\n"
       << "  Clusters decoded                    : " << clustersDecoded << " / " << hwCount << "\n"
       << "  Positions match streamedPage ref    : " << clustersMatchingReferencePositions << " / " << clustersDecoded << "\n"
       << "  All vertices inside node AABB       : " << clustersWithAllVerticesInAABB << " / " << clustersDecoded << "\n"
       << "  >=1 world-space vertex in frustum   : " << clustersWithAtLeastOneVertexVisible << " / " << hwCount << "\n"
       << "  Valid parentNodeIndex in _padding   : " << hwPaddingValid << " / " << hwCount << "\n";
    os::Logger::log(ss.str());
  }

  if (hwCount > 0u)
  {
    if (clustersWithAtLeastOneVertexVisible == 0u)
    {
      os::Logger::error(
          "  ERROR [CPU-DECODE]: no visible cluster has any world-space vertex"
          " inside the camera frustum.  Check camera placement and model transform.");
      valid = false;
    }
    else if (clustersWithAtLeastOneVertexVisible < hwCount)
    {
      os::Logger::warningf(
          "  WARNING [CPU-DECODE]: %u / %u clusters had zero frustum vertices"
          " (may be acceptable for clusters straddling frustum planes).",
          hwCount - clustersWithAtLeastOneVertexVisible,
          hwCount);
    }
    else
    {
      os::Logger::log("  OK [CPU-DECODE]: all clusters have >=1 world-space vertex in the frustum.");
    }

    if (clustersMatchingReferencePositions == clustersDecoded && clustersDecoded == hwCount)
      os::Logger::log("  OK [CPU-DECODE]: shader-mirror positions match streamedPage reference for all clusters.");

    if (clustersWithAllVerticesInAABB == clustersDecoded && clustersDecoded > 0u)
      os::Logger::log("  OK [CPU-DECODE]: all decoded vertices lie inside their hierarchy node AABB.");

    if (hwPaddingValid == hwCount)
      os::Logger::log("  OK [CPU-DECODE]: all HW visible cluster _padding fields contain valid leaf parentNodeIndex.");
    else
      os::Logger::errorf(
          "  ERROR [CPU-DECODE]: only %u / %u HW visible clusters had a valid"
          " parentNodeIndex in _padding.",
          hwPaddingValid,
          hwCount);
  }

  // -------------------------------------------------------------------------
  // Statistics
  // -------------------------------------------------------------------------
  uint32_t pagesInstalled = 0u;
  uint32_t pagesWithPriority = 0u;
  for (uint32_t pi = 0; pi < pageCount; ++pi)
  {
    if (pageTable[pi].isInstalled != 0u)
      ++pagesInstalled;
    if (pagePriorities[pi] > 0u)
      ++pagesWithPriority;
  }
  {
    std::ostringstream ss;
    ss << "\n--- Results for " << objName << " ---\n"
       << "  HW visible clusters : " << hwCount << "\n"
       << "  SW visible clusters : " << swCount << "\n"
       << "  Pages installed     : " << pagesInstalled << " / " << pageCount << "\n"
       << "  Pages with priority : " << pagesWithPriority << "\n"
       << "  HW _padding valid   : " << hwPaddingValid << " / " << hwCount << "\n"
       << "  SW _padding valid   : " << swPaddingValid << " / " << swCount << "\n";
    os::Logger::log(ss.str());
  }

  // -------------------------------------------------------------------------
  // Cleanup
  // -------------------------------------------------------------------------
  renderGraph->removePass<VirtualGeometryCullingMultipleDispatchesPass>(objName + "_cullingPass");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHWVisibleClusters");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHWDrawIndirect");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copySWVisibleClusters");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copySWDrawIndirect");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyPagePriorities");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyQueueA");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyQueueB");
  renderGraph->removePass<rendering::gpgpu::CopyBufferPass>(objName + "_copyHierarchyNodes");

  renderGraph->deleteBuffer(hwVisibleInfoStaging);
  renderGraph->deleteBuffer(hwDrawIndirectStaging);
  renderGraph->deleteBuffer(swVisibleInfoStaging);
  renderGraph->deleteBuffer(swDrawIndirectStaging);
  renderGraph->deleteBuffer(priorityStaging);
  renderGraph->deleteBuffer(hierarchyQueueAStaging);
  renderGraph->deleteBuffer(hierarchyQueueBStaging);
  renderGraph->deleteBuffer(hierarchyNodeStaging);
  renderGraph->deleteTexture(dummyDepth);

  delete renderGraph;

  result.hwVisibleClusters = hwCount;
  result.swVisibleClusters = swCount;
  result.pagesWithPriority = pagesWithPriority;
  result.clustersDecoded = clustersDecoded;
  result.clustersMatchingReference = clustersMatchingReferencePositions;
  result.clustersAllVerticesInAABB = clustersWithAllVerticesInAABB;
  result.clustersAnyVertexInFrustum = clustersWithAtLeastOneVertexVisible;
  result.clustersWithValidNodeIndexInPadding = hwPaddingValid;
  result.success = valid;
  return result;
}

// ============================================================================
// main
// ============================================================================

int main()
{
  const std::vector<std::string> test_meshes = {
    "assets/meshes/obj/suzanne.obj",
    // "assets/meshes/obj/teapot.obj",
    // "assets/meshes/obj/armadillo.obj",
  };

  os::Logger::start();
  os::Logger::log("VirtualGeometryCullingMultipleDispatchesPass - Integration Test");
  os::Logger::log("================================================================");

  DeviceRequiredLimits limits = {
    .minimumMemory = 0,
    .minimumComputeSharedMemory = 0,
    .minimumComputeWorkGroupInvocations = 0,
  };

  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle | DeviceFeatures::DeviceFeatures_Timestamp;

  rendering::backend::vulkan::VulkanRHI *rhi = new rendering::backend::vulkan::VulkanRHI(rendering::backend::vulkan::Vulkan_1_2, limits, features, {});

  auto surfaces = std::vector<VkSurfaceKHR>();
  rhi->init(surfaces);

  std::string exeDir = utils::getExecutableDirectory();
  std::vector<TestResult> results;

  for (const auto &meshRel : test_meshes)
  {
    std::string fullPath = exeDir + "/" + meshRel;

    FILE *f = fopen(fullPath.c_str(), "rb");
    if (!f)
    {
      os::Logger::warningf("Skipping %s (not found)", meshRel.c_str());
      continue;
    }
    fclose(f);

    std::string name = meshRel;
    auto slash = name.rfind('/');
    if (slash != std::string::npos)
      name = name.substr(slash + 1);
    auto dot = name.rfind('.');
    if (dot != std::string::npos)
      name = name.substr(0, dot);

    try
    {
      results.push_back(runTest(rhi, fullPath, name));
    }
    catch (const std::exception &e)
    {
      os::Logger::errorf("EXCEPTION for %s: %s", name.c_str(), e.what());
      TestResult r;
      r.meshName = name;
      r.success = false;
      results.push_back(r);
    }
  }

  // -------------------------------------------------------------------------
  // Summary
  // -------------------------------------------------------------------------
  os::Logger::log("\n================================================================");
  os::Logger::log("Summary");
  os::Logger::log("================================================================");
  os::Logger::log("  Mesh\tHW\tSW\tPriPages\tDecoded\tRefMatch\tInAABB\tInFrustum\tPadValid\tOK?");
  os::Logger::log("  ----\t--\t--\t--------\t-------\t--------\t------\t---------\t--------\t---");

  bool allOk = true;
  for (const auto &r : results)
  {
    std::ostringstream ss;
    ss << "  " << r.meshName << "\t" << r.hwVisibleClusters << "\t" << r.swVisibleClusters << "\t" << r.pagesWithPriority << "\t" << r.clustersDecoded << "\t" << r.clustersMatchingReference << "\t"
       << r.clustersAllVerticesInAABB << "\t" << r.clustersAnyVertexInFrustum << "\t" << r.clustersWithValidNodeIndexInPadding << "\t" << (r.success ? "OK" : "FAIL");
    os::Logger::log(ss.str());
    if (!r.success)
      allOk = false;
  }

  os::Logger::log("");
  os::Logger::log(allOk && !results.empty() ? "ALL TESTS PASSED" : "SOME TESTS FAILED");
  os::Logger::log("================================================================");

  os::Logger::shutdown();
  delete rhi;
  return allOk ? 0 : 1;
}
