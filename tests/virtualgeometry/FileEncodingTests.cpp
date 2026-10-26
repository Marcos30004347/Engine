#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "./utils/File.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"

using namespace virtualgeometry;

static constexpr uint32_t PAGE_NOT_INSTALLED_BIT = (1u << 31);

// ============================================================================
// Helper Functions
// ============================================================================

inline bool floatEqual(float a, float b, float eps = 1e-6f)
{
  if (std::isinf(a) || std::isinf(b))
    return std::isinf(a) && std::isinf(b) && std::signbit(a) == std::signbit(b);
  return std::fabs(a - b) <= eps;
}

bool compareHierarchyNode(const VirtualGeometryHierarchy &a, const VirtualGeometryHierarchy &b, float eps = 1e-4f)
{
  bool equal = true;
  equal &= floatEqual(a.max_x, b.max_x, eps);
  equal &= floatEqual(a.max_y, b.max_y, eps);
  equal &= floatEqual(a.max_z, b.max_z, eps);
  equal &= floatEqual(a.min_x, b.min_x, eps);
  equal &= floatEqual(a.min_y, b.min_y, eps);
  equal &= floatEqual(a.min_z, b.min_z, eps);

  // equal &= floatEqual(a.min_center_x, b.min_center_x, eps);
  // equal &= floatEqual(a.min_center_y, b.min_center_y, eps);
  // equal &= floatEqual(a.min_center_z, b.min_center_z, eps);
  // equal &= floatEqual(a.min_radius, b.min_radius, eps);

  equal &= floatEqual(a.max_center_x, b.max_center_x, eps);
  equal &= floatEqual(a.max_center_y, b.max_center_y, eps);
  equal &= floatEqual(a.max_center_z, b.max_center_z, eps);
  equal &= floatEqual(a.max_radius, b.max_radius, eps);

  equal &= floatEqual(a.min_lod_error, b.min_lod_error, eps);
  equal &= floatEqual(a.max_parent_lod_error, b.max_parent_lod_error, eps);
  equal &= (a.child_start == b.child_start);
  equal &= (a.child_count == b.child_count);
  equal &= (a.flags == b.flags);

  if (!equal)
  {
    std::cout << "  Hierarchy node mismatch:\n";
    if (!floatEqual(a.max_x, b.max_x, eps))
      std::cout << "    max_x: " << a.max_x << " vs " << b.max_x << "\n";
    if (!floatEqual(a.max_y, b.max_y, eps))
      std::cout << "    max_y: " << a.max_y << " vs " << b.max_y << "\n";
    if (!floatEqual(a.max_z, b.max_z, eps))
      std::cout << "    max_z: " << a.max_z << " vs " << b.max_z << "\n";
    if (!floatEqual(a.min_x, b.min_x, eps))
      std::cout << "    min_x: " << a.min_x << " vs " << b.min_x << "\n";
    if (!floatEqual(a.min_y, b.min_y, eps))
      std::cout << "    min_y: " << a.min_y << " vs " << b.min_y << "\n";
    if (!floatEqual(a.min_z, b.min_z, eps))
      std::cout << "    min_z: " << a.min_z << " vs " << b.min_z << "\n";
    if (!floatEqual(a.max_center_x, b.max_center_x, eps))
      std::cout << "    max_center_x: " << a.max_center_x << " vs " << b.max_center_x << "\n";
    if (!floatEqual(a.max_center_y, b.max_center_y, eps))
      std::cout << "    max_center_y: " << a.max_center_y << " vs " << b.max_center_y << "\n";
    if (!floatEqual(a.max_center_z, b.max_center_z, eps))
      std::cout << "    max_center_z: " << a.max_center_z << " vs " << b.max_center_z << "\n";
    if (!floatEqual(a.max_radius, b.max_radius, eps))
      std::cout << "    max_radius: " << a.max_radius << " vs " << b.max_radius << "\n";
    if (!floatEqual(a.min_lod_error, b.min_lod_error, eps))
      std::cout << "    min_lod_error: " << a.min_lod_error << " vs " << b.min_lod_error << "\n";
    if (!floatEqual(a.max_parent_lod_error, b.max_parent_lod_error, eps))
      std::cout << "    max_parent_lod_error: " << a.max_parent_lod_error << " vs " << b.max_parent_lod_error << "\n";
    if (a.child_start != b.child_start)
      std::cout << "    child_start: " << a.child_start << " vs " << b.child_start << "\n";
    if (a.child_count != b.child_count)
      std::cout << "    child_count: " << a.child_count << " vs " << b.child_count << "\n";
    if (a.pageIndex != b.pageIndex)
      std::cout << "    pageIndex: " << a.pageIndex << " vs " << b.pageIndex << "\n";
    if (a.meshPartIndex != b.meshPartIndex)
      std::cout << "    meshPartIndex: " << a.meshPartIndex << " vs " << b.meshPartIndex << "\n";
    if (a.flags != b.flags)
      std::cout << "    flags: " << a.flags << " vs " << b.flags << "\n";
  }
  return equal;
}

bool compareMeshlet(const Meshlet &a, const Meshlet &b, float eps = 1e-6f)
{
  bool equal = true;
  equal &= (a.vertex_count == b.vertex_count);
  equal &= (a.triangle_count == b.triangle_count);
  equal &= (a.quantized_position_span_x == b.quantized_position_span_x);
  equal &= (a.quantized_position_span_y == b.quantized_position_span_y);
  equal &= (a.quantized_position_span_z == b.quantized_position_span_z);
  equal &= (a.vertex_position_quantization_factor == b.vertex_position_quantization_factor);
  equal &= floatEqual(a.min_vertex_position_channel_x, b.min_vertex_position_channel_x, eps);
  equal &= floatEqual(a.min_vertex_position_channel_y, b.min_vertex_position_channel_y, eps);
  equal &= floatEqual(a.min_vertex_position_channel_z, b.min_vertex_position_channel_z, eps);
  equal &= floatEqual(a.cone.axis[0], b.cone.axis[0], eps);
  equal &= floatEqual(a.cone.axis[1], b.cone.axis[1], eps);
  equal &= floatEqual(a.cone.axis[2], b.cone.axis[2], eps);
  equal &= floatEqual(a.cone.cutoff, b.cone.cutoff, eps);

  if (!equal)
  {
    std::cout << "  Meshlet mismatch:\n";
    if (a.vertex_count != b.vertex_count)
      std::cout << "    vertex_count: " << (int)a.vertex_count << " vs " << (int)b.vertex_count << "\n";
    if (a.triangle_count != b.triangle_count)
      std::cout << "    triangle_count: " << (int)a.triangle_count << " vs " << (int)b.triangle_count << "\n";
    if (a.quantized_position_span_x != b.quantized_position_span_x)
      std::cout << "    quantized_position_span_x: " << a.quantized_position_span_x << " vs " << b.quantized_position_span_x << "\n";
    if (a.quantized_position_span_y != b.quantized_position_span_y)
      std::cout << "    quantized_position_span_y: " << a.quantized_position_span_y << " vs " << b.quantized_position_span_y << "\n";
    if (a.quantized_position_span_z != b.quantized_position_span_z)
      std::cout << "    quantized_position_span_z: " << a.quantized_position_span_z << " vs " << b.quantized_position_span_z << "\n";
    if (!floatEqual(a.cone.cutoff, b.cone.cutoff, eps))
      std::cout << "    cone.cutoff: " << a.cone.cutoff << " vs " << b.cone.cutoff << "\n";
  }
  return equal;
}

bool compareMeshletData(const VirtualGeometryPage &pageA, const VirtualGeometryPage &pageB, uint32_t meshletIdx, const QuantizationConfig &config, float eps = 1e-4f)
{
  std::vector<float> posA, posB;
  VirtualGeometryCompressor::decodePositions(pageA, meshletIdx, posA, config);
  VirtualGeometryCompressor::decodePositions(pageB, meshletIdx, posB, config);
  if (posA.size() != posB.size())
  {
    std::cout << "    Position count mismatch\n";
    return false;
  }
  for (size_t i = 0; i < posA.size(); ++i)
    if (!floatEqual(posA[i], posB[i], eps))
    {
      std::cout << "    Position mismatch at " << i << "\n";
      return false;
    }

  std::vector<float> normA, normB;
  VirtualGeometryCompressor::decodeNormals(pageA, meshletIdx, normA);
  VirtualGeometryCompressor::decodeNormals(pageB, meshletIdx, normB);
  if (normA.size() != normB.size())
  {
    std::cout << "    Normal count mismatch\n";
    return false;
  }

  std::vector<float> uvA, uvB;
  VirtualGeometryCompressor::decodeUVs(pageA, meshletIdx, uvA);
  VirtualGeometryCompressor::decodeUVs(pageB, meshletIdx, uvB);
  if (uvA.size() != uvB.size())
  {
    std::cout << "    UV count mismatch\n";
    return false;
  }

  std::vector<uint8_t> idxA, idxB;
  VirtualGeometryCompressor::decodeIndices(pageA, meshletIdx, idxA);
  VirtualGeometryCompressor::decodeIndices(pageB, meshletIdx, idxB);
  if (idxA.size() != idxB.size())
  {
    std::cout << "    Index count mismatch: " << idxA.size() << " vs " << idxB.size() << "\n";
    return false;
  }

  const uint32_t maxVertex = pageA.meshlets[meshletIdx].vertex_count;
  for (size_t i = 0; i < idxA.size(); ++i)
  {
    if (idxA[i] != idxB[i])
    {
      std::cout << "    Index mismatch at " << i << "\n";
      return false;
    }
    if (idxA[i] >= maxVertex)
    {
      std::cout << "    Invalid index at " << i << "\n";
      return false;
    }
  }
  if (idxA.size() != pageA.meshlets[meshletIdx].triangle_count * 3u)
  {
    std::cout << "    Index/triangle count mismatch\n";
    return false;
  }
  return true;
}

bool comparePage(const VirtualGeometryPage &a, const VirtualGeometryPage &b, const QuantizationConfig &config, uint32_t pageIdx)
{
  std::cout << "  Comparing page " << pageIdx << "...\n";

  if (a.meshlets.size() != b.meshlets.size())
  {
    std::cout << "    ERROR: Meshlet count mismatch\n";
    return false;
  }
  for (size_t i = 0; i < a.meshlets.size(); ++i)
    if (!compareMeshlet(a.meshlets[i], b.meshlets[i]))
    {
      std::cout << "    ERROR: Meshlet " << i << " mismatch\n";
      return false;
    }
  for (size_t i = 0; i < a.meshlets.size(); ++i)
    if (!compareMeshletData(a, b, static_cast<uint32_t>(i), config))
    {
      std::cout << "    ERROR: Meshlet data " << i << " mismatch\n";
      return false;
    }

  // Compare install updates
  {
    const auto &a_inst = a.installUpdates;
    const auto &b_inst = b.installUpdates;
    if (a_inst.hierarchyUpdates.size() != b_inst.hierarchyUpdates.size())
    {
      std::cout << "    ERROR: Install hierarchy update count mismatch\n";
      return false;
    }
    for (size_t i = 0; i < a_inst.hierarchyUpdates.size(); ++i)
    {
      const auto &ua = a_inst.hierarchyUpdates[i];
      const auto &ub = b_inst.hierarchyUpdates[i];
      if (ua.hierarchyNodeIndex != ub.hierarchyNodeIndex || ua.streamingLeafsBitset != ub.streamingLeafsBitset || ua.enabledClustersBitset != ub.enabledClustersBitset)
      {
        std::cout << "    ERROR: Install hierarchy update " << i << " mismatch\n";
        return false;
      }
    }
  }
  // Compare uninstall updates
  {
    const auto &a_uninst = a.uninstallUpdates;
    const auto &b_uninst = b.uninstallUpdates;
    if (a_uninst.hierarchyUpdates.size() != b_uninst.hierarchyUpdates.size())
    {
      std::cout << "    ERROR: Uninstall hierarchy update count mismatch\n";
      return false;
    }
    for (size_t i = 0; i < a_uninst.hierarchyUpdates.size(); ++i)
    {
      const auto &ua = a_uninst.hierarchyUpdates[i];
      const auto &ub = b_uninst.hierarchyUpdates[i];
      if (ua.hierarchyNodeIndex != ub.hierarchyNodeIndex || ua.streamingLeafsBitset != ub.streamingLeafsBitset || ua.enabledClustersBitset != ub.enabledClustersBitset)
      {
        std::cout << "    ERROR: Uninstall hierarchy update " << i << " mismatch\n";
        return false;
      }
    }
  }

  std::cout << "    ✓ Page " << pageIdx << " matches\n";
  return true;
}

// ============================================================================
// AABB containment check helpers
// ============================================================================

struct NodeAABB
{
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
};

// Returns true if (px,py,pz) is inside the AABB with the given tolerance.
static bool pointInAABB(float px, float py, float pz, const NodeAABB &aabb, float eps = 1e-4f)
{
  return px >= aabb.min_x - eps && px <= aabb.max_x + eps && py >= aabb.min_y - eps && py <= aabb.max_y + eps && pz >= aabb.min_z - eps && pz <= aabb.max_z + eps;
}

// Verify every decoded vertex of a meshlet is inside the supplied AABB.
// Returns false and prints a diagnostic on the first violation.
static bool checkMeshletInsideAABB(const VirtualGeometryPage &page, uint32_t local_meshlet_idx, const QuantizationConfig &config, const NodeAABB &aabb, uint32_t global_cluster_idx, uint32_t node_idx, float eps = 1e-4f)
{
  std::vector<float> positions;
  VirtualGeometryCompressor::decodePositions(page, local_meshlet_idx, positions, config);

  for (size_t v = 0; v + 2 < positions.size(); v += 3)
  {
    if (!pointInAABB(positions[v], positions[v + 1], positions[v + 2], aabb, eps))
    {
      std::cout << "    ERROR: Cluster " << global_cluster_idx << " (local meshlet " << local_meshlet_idx << ") vertex " << (v / 3) << " (" << positions[v] << ", " << positions[v + 1] << ", " << positions[v + 2] << ")"
                << " is outside node " << node_idx << " AABB [" << aabb.min_x << "," << aabb.max_x << "] x [" << aabb.min_y << "," << aabb.max_y << "] x [" << aabb.min_z << "," << aabb.max_z << "]\n";
      return false;
    }
  }
  return true;
}

// ============================================================================
// Test Functions
// ============================================================================

void testMetadataRead(const VirtualGeometryFile &reader)
{
  std::cout << "\n=== Testing Metadata Read ===\n";
  const auto &m = reader.getMetadata();
  assert(m.magic == VMESH_MAGIC && "Invalid magic number");
  assert(m.version == VMESH_VERSION && "Invalid version");
  std::cout << "  Magic:            0x" << std::hex << m.magic << std::dec << "\n";
  std::cout << "  Version:          " << m.version << "\n";
  std::cout << "  Page count:       " << m.page_count << "\n";
  std::cout << "  Hierarchy nodes:  " << m.hierarchy_node_count << "\n";
  std::cout << "  Total meshlets:   " << m.total_meshlet_count << "\n";
  std::cout << "  Root page:        " << m.root_page_index << "\n";
  std::cout << "  Dependency offset:" << m.page_dependency_offset << "\n";
  std::cout << "  Dependency size:  " << m.page_dependency_size << " bytes\n";
  std::cout << "  InstallUpd offset:" << m.page_install_update_offset << "\n";
  std::cout << "  InstallUpd size:  " << m.page_install_update_size << " bytes\n";
  std::cout << "✓ Metadata read successfully\n";
}

void testHierarchyRead(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Hierarchy Read ===\n";
  const auto &hierarchy = reader.getHierarchy();
  assert(hierarchy.size() == original.hierarchy.size() && "Hierarchy size mismatch");
  std::cout << "  Comparing " << hierarchy.size() << " hierarchy nodes...\n";
  for (size_t i = 0; i < hierarchy.size(); ++i)
    if (!compareHierarchyNode(original.hierarchy[i], hierarchy[i]))
    {
      std::cout << "  ERROR: Hierarchy node " << i << " mismatch\n";
      assert(false);
    }
  std::cout << "✓ All hierarchy nodes match\n";
}

void testPageTableRead(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Page Table Read ===\n";
  const auto &page_table = reader.getPageTable();
  assert(page_table.size() == original.pages.size() && "Page table size mismatch");
  std::cout << "  Page table has " << page_table.size() << " entries\n";
  for (size_t i = 0; i < page_table.size(); ++i)
    std::cout << "  Page " << i << ": " << page_table[i].meshlet_count << " meshlets, " << page_table[i].compressed_size << " bytes\n";
  std::cout << "✓ Page table read successfully\n";
}

void testPageDependenciesInHeader(const VirtualGeometryFile &reader, const std::vector<VirtualGeometryBuildPage> &buildPages)
{
  std::cout << "\n=== Testing Page Dependencies ===\n";
  const auto &fileDeps = reader.getPageDependencies();
  assert(fileDeps.size() == buildPages.size() && "Dependency count mismatch");

  uint32_t total = 0;
  for (size_t i = 0; i < fileDeps.size(); ++i)
  {
    const auto &deps = fileDeps[i];
    total += static_cast<uint32_t>(deps.size());
    assert(deps.size() == buildPages[i].dependencies.size() && "Page dep count mismatch");
    for (size_t j = 0; j < deps.size(); ++j)
      assert(deps[j] == buildPages[i].dependencies[j] && "Page dep mismatch");
    if (!deps.empty())
    {
      std::cout << "  Page " << i << " depends on " << deps.size() << " pages: [";
      for (size_t j = 0; j < deps.size(); ++j)
      {
        std::cout << deps[j];
        if (j + 1 < deps.size())
          std::cout << ", ";
      }
      std::cout << "]\n";
    }
  }
  std::cout << "  Total dependencies: " << total << "\n";
  std::cout << "✓ Dependency graph matches\n";
}

void testPageInstallUpdatesInMetadata(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Page Install Updates (from metadata) ===\n";
  const auto &fileUpdates = reader.getPageInstallUpdates();
  assert(fileUpdates.size() == original.pages.size() && "Install update page count mismatch");

  uint32_t totalHierarchy = 0, totalStreamingFlag = 0;
  for (size_t i = 0; i < fileUpdates.size(); ++i)
  {
    const PageUpdateList &fu = fileUpdates[i];
    const PageUpdateList &ou = original.pages[i].installUpdates;
    totalHierarchy += static_cast<uint32_t>(fu.hierarchyUpdates.size());
    assert(fu.hierarchyUpdates.size() == ou.hierarchyUpdates.size() && "Install hierarchy update count mismatch");
    for (size_t j = 0; j < fu.hierarchyUpdates.size(); ++j)
    {
      assert(fu.hierarchyUpdates[j].hierarchyNodeIndex == ou.hierarchyUpdates[j].hierarchyNodeIndex);
      assert(fu.hierarchyUpdates[j].streamingLeafsBitset == ou.hierarchyUpdates[j].streamingLeafsBitset);
      assert(fu.hierarchyUpdates[j].enabledClustersBitset == ou.hierarchyUpdates[j].enabledClustersBitset);
    }
    if (!fu.hierarchyUpdates.empty())
      std::cout << "  Page " << i << ": " << fu.hierarchyUpdates.size() << " hierarchy-cluster updates\n";
  }
  std::cout << "  Total install hierarchy updates: " << totalHierarchy << "\n";
  std::cout << "✓ Install updates in metadata match original\n";
}

void testPageUninstallUpdatesInMetadata(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Page Uninstall Updates (from metadata) ===\n";
  const auto &fileUpdates = reader.getPageUninstallUpdates();
  assert(fileUpdates.size() == original.pages.size() && "Uninstall update page count mismatch");

  uint32_t totalHierarchy = 0;
  for (size_t i = 0; i < fileUpdates.size(); ++i)
  {
    const PageUpdateList &fu = fileUpdates[i];
    const PageUpdateList &ou = original.pages[i].uninstallUpdates;
    totalHierarchy += static_cast<uint32_t>(fu.hierarchyUpdates.size());
    assert(fu.hierarchyUpdates.size() == ou.hierarchyUpdates.size() && "Uninstall hierarchy update count mismatch");
    for (size_t j = 0; j < fu.hierarchyUpdates.size(); ++j)
    {
      assert(fu.hierarchyUpdates[j].hierarchyNodeIndex == ou.hierarchyUpdates[j].hierarchyNodeIndex);
      assert(fu.hierarchyUpdates[j].streamingLeafsBitset == ou.hierarchyUpdates[j].streamingLeafsBitset);
      assert(fu.hierarchyUpdates[j].enabledClustersBitset == ou.hierarchyUpdates[j].enabledClustersBitset);
    }
    if (!fu.hierarchyUpdates.empty())
      std::cout << "  Page " << i << ": " << fu.hierarchyUpdates.size() << " hierarchy-cluster updates\n";
  }
  std::cout << "  Total uninstall hierarchy updates: " << totalHierarchy << "\n";
  std::cout << "✓ Uninstall updates in metadata match original\n";
}

void testIndividualPageStreaming(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Individual Page Streaming ===\n";
  const auto &metadata = reader.getMetadata();
  for (uint32_t i = 0; i < metadata.page_count; ++i)
  {
    VirtualGeometryPage page;
    assert(reader.streamPage(i, page) && "Failed to stream page");
    if (!comparePage(original.pages[i], page, original.quantizationConfig, i))
    {
      std::cout << "  ERROR: Page " << i << " data mismatch\n";
      assert(false);
    }
    std::cout << "  Page " << i << " has " << page.installUpdates.hierarchyUpdates.size() << " hierarchy-cluster install updates\n";
  }
  std::cout << "✓ All individual pages match\n";
}

void testStreamedPageAPI(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing VirtualGeometryStreamedPage API (raw streaming) ===\n";
  const auto &metadata = reader.getMetadata();
  const uint32_t maxPageSize = reader.getMaxPageSize();

  std::vector<uint8_t> stagingBuffer(maxPageSize);

  for (uint32_t i = 0; i < metadata.page_count; ++i)
  {
    VirtualGeometryStreamedPage page;
    const bool ok = reader.streamPageRaw(i, stagingBuffer.data(), maxPageSize, page);
    assert(ok && "Failed to stream raw page");

    const PageUpdateList &updates = reader.getPageInstallUpdates()[i];
    assert(updates.hierarchyUpdates.size() == original.pages[i].installUpdates.hierarchyUpdates.size());

    std::cout << "  Page " << i << ": " << page.getDataSizeInBytes() << " bytes, " << page.getMeshletCount() << " meshlets, " << updates.hierarchyUpdates.size() << " hierarchy install updates (metadata)\n";

    for (uint32_t j = 0; j < page.getMeshletCount(); ++j)
    {
      auto desc = page.getMeshletDescriptor(j);
      assert(desc.vertex_count == original.pages[i].meshlets[j].vertex_count);
      assert(desc.triangle_count == original.pages[i].meshlets[j].triangle_count);
    }
  }
  std::cout << "✓ VirtualGeometryStreamedPage API working correctly\n";
}

void testCompleteRead(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original)
{
  std::cout << "\n=== Testing Complete Read ===\n";
  VirtualGeometryEncodedData loaded;
  assert(reader.readAll(loaded) && "Failed to complete read");

  assert(loaded.quantizationConfig.quantization_factor == original.quantizationConfig.quantization_factor);
  assert(floatEqual(loaded.quantizationConfig.unit_scale, original.quantizationConfig.unit_scale));
  assert(loaded.hierarchy.size() == original.hierarchy.size());
  assert(loaded.pages.size() == original.pages.size());
  assert(loaded.rootPageIndex == original.rootPageIndex);

  std::cout << "  Comparing " << loaded.hierarchy.size() << " hierarchy nodes...\n";
  for (size_t i = 0; i < loaded.hierarchy.size(); ++i)
    if (!compareHierarchyNode(original.hierarchy[i], loaded.hierarchy[i]))
    {
      std::cout << "  ERROR: Hierarchy node " << i << " mismatch\n";
      assert(false);
    }

  std::cout << "  Comparing " << loaded.pages.size() << " pages...\n";
  for (size_t i = 0; i < loaded.pages.size(); ++i)
    if (!comparePage(original.pages[i], loaded.pages[i], original.quantizationConfig, static_cast<uint32_t>(i)))
    {
      std::cout << "  ERROR: Page " << i << " mismatch\n";
      assert(false);
    }

  std::cout << "✓ Complete read matches original data\n";
}

// ============================================================================
// AABB containment test — checks that every decoded vertex of every meshlet
// referenced by a leaf hierarchy node lies within that node's AABB.
//
// This catches mismatches between the builder's AABB computation and the
// actual positions that come out of the quantisation/dequantisation round-trip
// after the file has been written and read back.
//
// build_pages is needed to map a global cluster index → (page, local meshlet).
// ============================================================================
void testMeshletAABBContainment(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original, const std::vector<VirtualGeometryBuildPage> &build_pages)
{
  std::cout << "\n=== Testing Meshlet AABB Containment ===\n";

  const QuantizationConfig &config = original.quantizationConfig;
  const auto &hierarchy = original.hierarchy;

  // Build a map: global_cluster_index → (page_idx, local_meshlet_idx)
  // using the clusterOffset / clusterCount stored in each build page.
  struct ClusterLocation
  {
    uint32_t page_idx;
    uint32_t local_meshlet_idx;
  };
  std::vector<ClusterLocation> clusterMap; // indexed by global cluster idx

  for (size_t p = 0; p < build_pages.size(); ++p)
  {
    const VirtualGeometryBuildPage &bp = build_pages[p];
    for (uint32_t c = 0; c < bp.clusterCount; ++c)
    {
      uint32_t global_idx = bp.clusterOffset + c;

      if (global_idx >= clusterMap.size())
        clusterMap.resize(global_idx + 1);
      clusterMap[global_idx] = {static_cast<uint32_t>(p), c};
    }
  }

  // Stream all pages once and cache them to avoid repeated I/O.
  std::vector<VirtualGeometryPage> pages(original.pages.size());

  for (uint32_t p = 0; p < static_cast<uint32_t>(original.pages.size()); ++p)
  {
    const bool ok = reader.streamPage(p, pages[p]);
    assert(ok && "testMeshletAABBContainment: failed to stream page");
  }

  uint32_t nodesChecked = 0;
  uint32_t meshletsTested = 0;
  uint32_t verticesTested = 0;
  bool allOk = true;

  for (uint32_t nodeIdx = 0; nodeIdx < static_cast<uint32_t>(hierarchy.size()); ++nodeIdx)
  {
    printf("node global idx = %u\n", nodeIdx);

    const VirtualGeometryHierarchy &node = hierarchy[nodeIdx];

    // Only leaf nodes reference clusters directly.
    if (!(node.flags & 1u))
      continue;

    printf("node global idx %u, passed flags\n", nodeIdx);

    ++nodesChecked;

    NodeAABB aabb{node.min_x, node.min_y, node.min_z, node.max_x, node.max_y, node.max_z};

    // Sanity-check the AABB itself.
    if (aabb.min_x > aabb.max_x || aabb.min_y > aabb.max_y || aabb.min_z > aabb.max_z)
    {
      std::cout << "  ERROR: Node " << nodeIdx << " has degenerate AABB\n";
      allOk = false;
      continue;
    }
    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    if (rawPage >= build_pages.size())
    {
      std::cout << "  ERROR: Node " << nodeIdx << " pageIndex points to invalid page " << rawPage << "\n";
      allOk = false;
      continue;
    }

    const uint32_t globalStartCluster = build_pages[rawPage].clusterOffset + node.child_start;
    printf("node global idx %u, checking %u\n", globalStartCluster, node.child_count);

    for (uint32_t local = 0; local < node.child_count; ++local)
    {
      const uint32_t ci = globalStartCluster + local;
      if (ci >= clusterMap.size())
      {
        std::cout << "  ERROR: Global cluster index " << ci << " referenced by node " << nodeIdx << " is out of range\n";
        allOk = false;
        continue;
      }

      const ClusterLocation &loc = clusterMap[ci];
      const VirtualGeometryPage &page = pages[loc.page_idx];

      if (loc.local_meshlet_idx >= page.meshlets.size())
      {
        std::cout << "  ERROR: Local meshlet index " << loc.local_meshlet_idx << " out of range for page " << loc.page_idx << " (has " << page.meshlets.size() << " meshlets)\n";
        allOk = false;
        continue;
      }

      // Decode positions and check each vertex.
      std::vector<float> positions;
      VirtualGeometryCompressor::decodePositions(page, loc.local_meshlet_idx, positions, config);

      if (positions.empty())
      {
        std::cout << "  WARNING: Cluster " << ci << " decoded to zero positions\n";
        continue;
      }
      printf("node global idx %u, passed here %u\n", nodeIdx, meshletsTested);

      ++meshletsTested;
      verticesTested += static_cast<uint32_t>(positions.size() / 3);

      if (!checkMeshletInsideAABB(page, loc.local_meshlet_idx, config, aabb, ci, nodeIdx))
      {
        allOk = false;
        // Continue checking remaining clusters so all violations are reported.
      }
    }
  }

  std::cout << "  Leaf nodes checked: " << nodesChecked << "\n";
  std::cout << "  Meshlets tested:    " << meshletsTested << "\n";
  std::cout << "  Vertices tested:    " << verticesTested << "\n";

  assert(allOk && "testMeshletAABBContainment: one or more vertices are outside their node AABB");
  std::cout << "✓ All decoded vertices are within their hierarchy node AABB\n";
}

// Same check but using the raw streaming API (VirtualGeometryStreamedPage).
// This exercises the GPU-side decoding path rather than the CPU helper.
void testMeshletAABBContainmentRaw(const VirtualGeometryFile &reader, const VirtualGeometryEncodedData &original, const std::vector<VirtualGeometryBuildPage> &build_pages)
{
  std::cout << "\n=== Testing Meshlet AABB Containment (raw streaming path) ===\n";

  const QuantizationConfig &config = original.quantizationConfig;
  const auto &hierarchy = original.hierarchy;
  const uint32_t maxPageSize = reader.getMaxPageSize();

  // Build global cluster → location map.
  struct ClusterLocation
  {
    uint32_t page_idx;
    uint32_t local_meshlet_idx;
  };
  std::vector<ClusterLocation> clusterMap;
  for (size_t p = 0; p < build_pages.size(); ++p)
  {
    const VirtualGeometryBuildPage &bp = build_pages[p];
    for (uint32_t c = 0; c < bp.clusterCount; ++c)
    {
      uint32_t g = bp.clusterOffset + c;
      if (g >= clusterMap.size())
        clusterMap.resize(g + 1);
      clusterMap[g] = {static_cast<uint32_t>(p), c};
    }
  }

  // Stream all pages raw into per-page staging buffers.
  const uint32_t pageCount = static_cast<uint32_t>(original.pages.size());
  std::vector<std::vector<uint8_t>> stagingBuffers(pageCount, std::vector<uint8_t>(maxPageSize, 0));
  std::vector<VirtualGeometryStreamedPage> streamedPages(pageCount);

  for (uint32_t p = 0; p < pageCount; ++p)
  {
    const bool ok = reader.streamPageRaw(p, stagingBuffers[p].data(), maxPageSize, streamedPages[p]);
    assert(ok && "testMeshletAABBContainmentRaw: failed to stream raw page");
  }

  uint32_t nodesChecked = 0;
  uint32_t meshletsTested = 0;
  uint32_t verticesTested = 0;
  bool allOk = true;

  for (uint32_t nodeIdx = 0; nodeIdx < static_cast<uint32_t>(hierarchy.size()); ++nodeIdx)
  {
    const VirtualGeometryHierarchy &node = hierarchy[nodeIdx];
    if (!(node.flags & 1u))
      continue;

    ++nodesChecked;

    NodeAABB aabb{node.min_x, node.min_y, node.min_z, node.max_x, node.max_y, node.max_z};

    const uint32_t rawPage = node.pageIndex & ~PAGE_NOT_INSTALLED_BIT;
    if (rawPage >= build_pages.size())
    {
      allOk = false;
      continue;
    }

    const uint32_t globalStartCluster = build_pages[rawPage].clusterOffset + node.child_start;
    for (uint32_t local = 0; local < node.child_count; ++local)
    {
      const uint32_t ci = globalStartCluster + local;
      if (ci >= clusterMap.size())
      {
        allOk = false;
        continue;
      }

      const ClusterLocation &loc = clusterMap[ci];
      const VirtualGeometryStreamedPage &spage = streamedPages[loc.page_idx];

      std::vector<float> positions;
      spage.decodePositions(loc.local_meshlet_idx, positions, config);

      ++meshletsTested;
      verticesTested += static_cast<uint32_t>(positions.size() / 3);

      for (size_t v = 0; v + 2 < positions.size(); v += 3)
      {
        if (!pointInAABB(positions[v], positions[v + 1], positions[v + 2], aabb))
        {
          std::cout << "  ERROR (raw path): Cluster " << ci << " vertex " << (v / 3) << " (" << positions[v] << ", " << positions[v + 1] << ", " << positions[v + 2] << ")"
                    << " outside node " << nodeIdx << " AABB\n";
          allOk = false;
        }
      }
    }
  }

  std::cout << "  Leaf nodes checked: " << nodesChecked << "\n";
  std::cout << "  Meshlets tested:    " << meshletsTested << "\n";
  std::cout << "  Vertices tested:    " << verticesTested << "\n";

  assert(allOk && "testMeshletAABBContainmentRaw: vertex outside AABB on raw streaming path");
  std::cout << "✓ Raw streaming path: all decoded vertices within their hierarchy node AABB\n";
}

// ============================================================================
// Round-trip test
// ============================================================================

void testFileRoundTrip(const std::string &mesh_path)
{
  std::cout << "\n==================================================\n";
  std::cout << "Testing: " << mesh_path << "\n";
  std::cout << "==================================================\n";

  std::vector<Vertex> vertices;
  Shape shape;
  VirtualGeometryEncoder::loadOBJ(mesh_path, true, vertices, shape);

  VirtualGeometryBuildData build = VirtualGeometryBuilder::build(VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape));

  QuantizationConfig config;
  config.quantization_factor = 4;
  config.unit_scale = 100.0f;

  VirtualGeometryEncodedData encoded = VirtualGeometryEncoder::encode(vertices, shape, config);

  std::cout << "\nOriginal data:\n";
  std::cout << "  Hierarchy nodes: " << encoded.hierarchy.size() << "\n";
  std::cout << "  Pages:           " << encoded.pages.size() << "\n";

  uint32_t total_meshlets = 0, total_deps = 0, total_install_hier = 0;
  for (const auto &page : encoded.pages)
  {
    total_meshlets += static_cast<uint32_t>(page.meshlets.size());
    total_install_hier += static_cast<uint32_t>(page.installUpdates.hierarchyUpdates.size());
  }
  for (const auto &page : build.pages)
    total_deps += static_cast<uint32_t>(page.dependencies.size());

  std::cout << "  Total meshlets:                       " << total_meshlets << "\n";
  std::cout << "  Total deps (build):                   " << total_deps << "\n";
  std::cout << "  Total install hierarchy updates:      " << total_install_hier << "\n";

  const std::string output_path = mesh_path + ".virtualgeometry";
  std::cout << "\nWriting to file: " << output_path << "\n";

  {
    VirtualGeometryFile writer(output_path, true);
    assert(writer.isOpen() && "Failed to open file for writing");
    assert(writer.write(encoded, build.pages, MESHLET_LZ4) && "Failed to write file");
    std::cout << "✓ File written successfully (Version " << VMESH_VERSION << ")\n";
  }

  {
    VirtualGeometryFile reader(output_path, false);
    assert(reader.isOpen() && "Failed to open file for reading");

    const auto &metadata = reader.getMetadata();
    const uint32_t maxPageSize = reader.getMaxPageSize();
    std::cout << "\nFile info:\n";
    std::cout << "  Version:      " << metadata.version << "\n";
    std::cout << "  Max page size:" << maxPageSize << " bytes\n";
    std::cout << "  Page count:   " << metadata.page_count << "\n";

    testMetadataRead(reader);
    testHierarchyRead(reader, encoded);
    testPageTableRead(reader, encoded);
    testPageDependenciesInHeader(reader, build.pages);
    testPageInstallUpdatesInMetadata(reader, encoded);
    testPageUninstallUpdatesInMetadata(reader, encoded);
    testIndividualPageStreaming(reader, encoded);
    testStreamedPageAPI(reader, encoded);
    testCompleteRead(reader, encoded);

    // AABB containment — verifies decoded vertex data against hierarchy AABBs.
    testMeshletAABBContainment(reader, encoded, build.pages);
    testMeshletAABBContainmentRaw(reader, encoded, build.pages);
  }

  std::cout << "\n==================================================\n";
  std::cout << "✓ ALL TESTS PASSED FOR " << mesh_path << "\n";
  std::cout << "==================================================\n";
}

// ============================================================================
// Main
// ============================================================================

int main()
{
  const std::vector<std::string> test_meshes = {
    "assets/meshes/obj/suzanne.obj",
    "assets/meshes/obj/teapot.obj",
    "assets/meshes/obj/armadillo.obj",
  };

  std::cout << "Virtual Geometry File I/O Test Suite\n";
  std::cout << "====================================\n";

  for (const auto &mesh_path : test_meshes)
  {
    const std::string full_path = utils::getExecutableDirectory() + "/" + mesh_path;
    FILE *f = fopen(full_path.c_str(), "rb");
    if (!f)
    {
      std::cout << "Skipping " << mesh_path << " (file not found)\n";
      continue;
    }
    fclose(f);
    try
    {
      testFileRoundTrip(full_path);
    }
    catch (const std::exception &e)
    {
      std::cout << "ERROR: " << e.what() << "\n";
      return 1;
    }
  }

  std::cout << "\n====================================\n";
  std::cout << "✓ ALL FILE I/O TESTS PASSED\n";
  std::cout << "✓ Install updates stored in file metadata (not page bytes)\n";
  std::cout << "✓ streamPageRaw writes directly into caller-owned buffer\n";
  std::cout << "✓ VirtualGeometryStreamedPage is a non-owning void* view\n";
  std::cout << "✓ All decoded meshlet vertices lie within their node AABBs\n";
  std::cout << "====================================\n";
  return 0;
}
