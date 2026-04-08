// ============================================================================
// VirtualGeometryHardwareDrawPass — Isolated Test (ENHANCED DEBUG VERSION)
// ============================================================================

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "./utils/File.hpp"

#include "math/math.hpp"
#include "os/File.hpp"
#include "os/Logger.hpp"
#include "rendering/core/Camera.hpp"
#include "rendering/gpgpu/RenderToQuadPass.hpp"
#include "rendering/gpu/RenderGraph.hpp"
#include "rendering/gpu/backend/vulkan/VulkanRHI.hpp"
#include "time/TimeSpan.hpp"
#include "editor/virtualgeometry/VirtualGeometryBuilder.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "virtualgeometry/VirtualGeometryFile.hpp"
#include "virtualgeometry/VirtualGeometryScene.hpp"
#include "virtualgeometry/rendering/VirtualGeometryHardwareDrawPass.hpp"
#include "window/sdl3/SDL3Window.hpp"
#include "window/window.hpp"

using namespace rendering;
using namespace backend;
using namespace virtualgeometry;
using namespace virtualgeometry::gpgpu;

// ============================================================================
// GPU-layout structures (must match WGSL exactly)
// ============================================================================

struct VisibleClusterInfo_CPU
{
  uint32_t pageIndex;
  uint32_t pageLocalClusterIndex;
  uint32_t instanceIndex;
  // Stores the parent leaf hierarchy node index, written by processClusters
  // via the clusterQueue element's _padding (set by processHierarchyNodes).
  uint32_t _padding; // parentNodeIndex
};
static_assert(sizeof(VisibleClusterInfo_CPU) == 16);

static constexpr uint32_t HW_VISIBLE_CLUSTER_COUNT_INDEX = 2u;
static constexpr uint32_t COUNTER_SLOTS = 16u;

// ============================================================================
// Hardcoded culling results from integration test log (suzanne.obj, 2026-02-25)
// ============================================================================

struct MeshletRef
{
  uint32_t pageIndex;
  uint32_t pageLocalClusterIndex;
  uint32_t triangleCount;
  // Parent leaf hierarchy node index — stored in VisibleClusterInfo._padding.
  // Sourced from the culling integration test output.
  uint32_t parentNodeIndex;
};

static std::vector<MeshletRef> getHardcodedCullingResults()
{
  struct Entry
  {
    uint32_t page, localIdx, triCount, parentNodeIndex;
  };
  // Source: integration test log (suzanne.obj)
  //   HW Visible Clusters for suzanne (10 clusters)
  //   [0]  pageIndex=0  pageLocalClusterIdx=12  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [1]  pageIndex=0  pageLocalClusterIdx=13  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [2]  pageIndex=0  pageLocalClusterIdx=14  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [3]  pageIndex=0  pageLocalClusterIdx=15  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [4]  pageIndex=0  pageLocalClusterIdx=16  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [5]  pageIndex=0  pageLocalClusterIdx=17  instanceIndex=0  parentNodeIndex=5 [LEAF]
  //   [6]  pageIndex=0  pageLocalClusterIdx=8   instanceIndex=0  parentNodeIndex=6 [LEAF]
  //   [7]  pageIndex=0  pageLocalClusterIdx=9   instanceIndex=0  parentNodeIndex=6 [LEAF]
  //   [8]  pageIndex=0  pageLocalClusterIdx=10  instanceIndex=0  parentNodeIndex=6 [LEAF]
  //   [9]  pageIndex=0  pageLocalClusterIdx=11  instanceIndex=0  parentNodeIndex=6 [LEAF]
  //   Triangle counts from HW DrawIndirectCommands (vtxCount / 3):
  static const Entry kEntries[] = {
    {0, 12, 115, 5},
    {0, 13, 127, 5},
    {0, 14, 108, 5},
    {0, 15, 102, 5},
    {0, 16, 122, 5},
    {0, 17, 105, 5},
    {0, 8, 69, 6},
    {0, 9, 76, 6},
    {0, 10, 72, 6},
    {0, 11, 72, 6},
  };

  std::vector<MeshletRef> refs;
  for (const auto &e : kEntries)
    refs.push_back({e.page, e.localIdx, e.triCount, e.parentNodeIndex});
  return refs;
}

// ============================================================================
// CPU buffer builders
// ============================================================================

static std::vector<uint32_t> buildCounters(uint32_t hwVisibleCount)
{
  std::vector<uint32_t> c(COUNTER_SLOTS, 0u);
  c[HW_VISIBLE_CLUSTER_COUNT_INDEX] = hwVisibleCount;
  return c;
}

static std::vector<VisibleClusterInfo_CPU> buildVisibleClusterInfos(const std::vector<MeshletRef> &m)
{
  std::vector<VisibleClusterInfo_CPU> out;
  out.reserve(m.size());
  for (const auto &r : m)
    out.push_back({r.pageIndex, r.pageLocalClusterIndex, /*instanceIndex=*/0u, r.parentNodeIndex});
  return out;
}

static std::vector<uint32_t> buildDrawIndirectCommands(const std::vector<MeshletRef> &m)
{
  std::vector<uint32_t> cmds;
  cmds.reserve(m.size() * 4u);
  for (uint32_t i = 0; i < static_cast<uint32_t>(m.size()); ++i)
  {
    cmds.push_back(m[i].triangleCount * 3u); // vertexCount
    cmds.push_back(1u);                      // instanceCount
    cmds.push_back(0u);                      // firstVertex
    cmds.push_back(i);                       // firstInstance (draw-ID)
  }
  return cmds;
}

// ============================================================================
// [DBG-3] CPU shader mirror — mirrors vs_main bit-for-bit
// ============================================================================
namespace cpu_shader_mirror
{

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
  return {(minQX + static_cast<float>(qx)) / dequantScale, (minQY + static_cast<float>(qy)) / dequantScale, (minQZ + static_cast<float>(qz)) / dequantScale};
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
  float nx = ox, ny = oy, nz = 1.0f - std::abs(ox) - std::abs(oy);
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
    if (idxOff != cumulativeIdxWords)
    {
      os::Logger::errorf(
          "  [CPU-DECODE] Padding invariant VIOLATED at meshlet %u: "
          "idxOff=%u expected=%u",
          m,
          idxOff,
          cumulativeIdxWords);
    }
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

  const uint32_t numCorners = triCount * 3u;
  out.indices.resize(numCorners);
  for (uint32_t c = 0u; c < numCorners; ++c)
    out.indices[c] = decodeIndex(pageWords, idxDataBase, idxOff, c);

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

} // namespace cpu_shader_mirror

static void printCameraDiagnostic(const rendering::Camera &cam, const math::Vec3f &objectCentre, float nearPlane, float farPlane)
{
  os::Logger::log("\n========================================");
  os::Logger::log(" Camera Diagnostic");
  os::Logger::log("========================================");

  const math::Vec3f &pos = cam.getPosition();
  os::Logger::logf("  Camera world pos : (%.4f, %.4f, %.4f)", pos[0], pos[1], pos[2]);
  os::Logger::logf("  Object centre    : (%.4f, %.4f, %.4f)", objectCentre[0], objectCentre[1], objectCentre[2]);

  const math::Mat4f &view = cam.getViewMatrix();
  const float vz = view.at(2, 0) * objectCentre[0] + view.at(2, 1) * objectCentre[1] + view.at(2, 2) * objectCentre[2] + view.at(2, 3);

  os::Logger::logf("  View-space z of centre : %.4f", vz);
  os::Logger::logf("  Near: %.4f  Far: %.4f", nearPlane, farPlane);
  os::Logger::logf("  Is centre between near and far? %s", (vz > nearPlane && vz < farPlane) ? "YES" : "NO — OBJECT MAY NOT BE VISIBLE");

  os::Logger::log("  View matrix (row-major, first 3 rows):");
  for (int r = 0; r < 3; ++r)
    os::Logger::logf("    [%.4f, %.4f, %.4f, %.4f]", view.at(r, 0), view.at(r, 1), view.at(r, 2), view.at(r, 3));

  const math::Mat4f &proj = cam.getProjectionMatrix();
  os::Logger::log("  Projection matrix diagonal:");
  os::Logger::logf("    P[0][0]=%.4f  P[1][1]=%.4f  P[2][2]=%.6f  P[3][2]=%.6f", proj.at(0, 0), proj.at(1, 1), proj.at(2, 2), proj.at(3, 2));
  os::Logger::log("========================================\n");
}

struct FrustumPlane
{
  float x, y, z, w;
};

static std::array<FrustumPlane, 6> buildFrustumPlanes(const rendering::Camera &cam)
{
  const math::Mat4f &view = cam.getViewMatrix();
  const math::Mat4f &proj = cam.getProjectionMatrix();

  math::Mat4f vp;
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c)
    {
      float v = 0.f;
      for (int k = 0; k < 4; ++k)
        v += proj.at(r, k) * view.at(k, c);
      vp.at(r, c) = v;
    }

  auto row = [&](int r) -> std::array<float, 4>
  {
    return {vp.at(r, 0), vp.at(r, 1), vp.at(r, 2), vp.at(r, 3)};
  };
  auto neg = [](std::array<float, 4> a) -> std::array<float, 4>
  {
    return {-a[0], -a[1], -a[2], -a[3]};
  };
  auto plane = [](std::array<float, 4> a, std::array<float, 4> b) -> FrustumPlane
  {
    FrustumPlane p;
    p.x = a[0] + b[0];
    p.y = a[1] + b[1];
    p.z = a[2] + b[2];
    p.w = a[3] + b[3];
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

  auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
  return {
    plane(r3, r0),
    plane(r3, neg(r0)),
    plane(r3, r1),
    plane(r3, neg(r1)),
    plane(r3, neg(r2)),
    plane(r2, {}),
  };
}

static bool pointInFrustum(const std::array<FrustumPlane, 6> &planes, float px, float py, float pz)
{
  for (const auto &pl : planes)
    if (pl.x * px + pl.y * py + pl.z * pz + pl.w < 0.f)
      return false;
  return true;
}

static bool runCPUDecodeValidation(
    const std::vector<MeshletRef> &meshlets,
    VirtualGeometryScene &scene,
    rendering::RenderGraph *renderGraph,
    const std::string &vgPath,
    const QuantizationConfig &qcfg,
    const rendering::Camera &cam,
    const std::vector<PageTableEntry> &pageTable)
{
  os::Logger::log("\n========================================");
  os::Logger::log("[DBG-3] CPU Decode Validation");
  os::Logger::log("========================================");

  const auto frustum = buildFrustumPlanes(cam);

  VirtualGeometryFile vgFile(vgPath, /*write=*/false);
  if (!vgFile.isOpen())
  {
    os::Logger::error("  ERROR: Cannot open .virtualgeometry for reference decode.");
    return false;
  }

  const uint32_t pageIndex = 0u;
  const PageTableEntry &entry = pageTable[pageIndex];
  if (entry.isInstalled == 0u || entry.size == 0u)
  {
    os::Logger::error("  ERROR: Page 0 is not installed or has zero size.");
    return false;
  }

  os::Logger::logf("  Page 0: bufferOffset=%u  size=%u  isInstalled=%u", entry.bufferOffset, entry.size, entry.isInstalled);

  std::vector<uint32_t> gpuPageWords(entry.size / sizeof(uint32_t));
  renderGraph->bufferRead(
      scene.pagesBuffer,
      entry.bufferOffset,
      entry.size,
      [&](const void *data)
      {
        std::memcpy(gpuPageWords.data(), data, entry.size);
      });

  os::Logger::logf("  GPU page word count: %zu", gpuPageWords.size());
  if (gpuPageWords.size() < 6u)
  {
    os::Logger::error("  ERROR: Page too small to contain header.");
    return false;
  }

  os::Logger::log("  GPU page header words [0..7]:");
  for (uint32_t i = 0; i < std::min<uint32_t>(8u, static_cast<uint32_t>(gpuPageWords.size())); ++i)
    os::Logger::logf("    word[%u] = %u (0x%08X)", i, gpuPageWords[i], gpuPageWords[i]);

  const uint32_t numMeshlets = gpuPageWords[0];
  os::Logger::logf("  Page 0 num_meshlets: %u", numMeshlets);

  float aabbMinX = 1e30f, aabbMinY = 1e30f, aabbMinZ = 1e30f;
  float aabbMaxX = -1e30f, aabbMaxY = -1e30f, aabbMaxZ = -1e30f;

  bool allPassed = true;
  uint32_t clustersDecoded = 0u;
  uint32_t clustersPositionMatch = 0u;
  uint32_t clustersNormalsOk = 0u;
  uint32_t clustersInFrustum = 0u;

  for (uint32_t ci = 0; ci < static_cast<uint32_t>(meshlets.size()); ++ci)
  {
    const MeshletRef &m = meshlets[ci];
    const uint32_t mi = m.pageLocalClusterIndex;

    if (mi >= numMeshlets)
    {
      os::Logger::errorf("  ERROR cluster[%u]: pageLocalClusterIndex=%u >= numMeshlets=%u", ci, mi, numMeshlets);
      allPassed = false;
      continue;
    }

    const uint32_t descBase = cpu_shader_mirror::PAGE_HEADER_WORDS + mi * cpu_shader_mirror::MESHLET_DESC_WORDS;
    os::Logger::logf("  [cluster %u] meshlet local idx=%u  parentNodeIndex=%u  descriptor words:", ci, mi, m.parentNodeIndex);
    for (uint32_t w = 0; w < cpu_shader_mirror::MESHLET_DESC_WORDS; ++w)
      os::Logger::logf("    desc[%u+%u=%u] = %u (0x%08X)", descBase, w, descBase + w, gpuPageWords[descBase + w], gpuPageWords[descBase + w]);

    cpu_shader_mirror::DecodedMeshlet decoded = cpu_shader_mirror::decodeMeshlet(gpuPageWords.data(), 0u, mi, qcfg.unit_scale);

    if (decoded.positions.empty())
    {
      os::Logger::errorf("  ERROR cluster[%u]: decodeMeshlet returned no positions.", ci);
      allPassed = false;
      continue;
    }
    ++clustersDecoded;

    const uint32_t vtxCount = static_cast<uint32_t>(decoded.positions.size());
    const uint32_t triCount = static_cast<uint32_t>(decoded.indices.size()) / 3u;

    if (triCount != m.triangleCount)
    {
      os::Logger::errorf("  ERROR cluster[%u]: decoded triCount=%u but hardcoded says %u", ci, triCount, m.triangleCount);
      allPassed = false;
    }

    if (ci == 0u)
    {
      os::Logger::logf("  First decoded meshlet: vtxCount=%u  triCount=%u", vtxCount, triCount);
      const uint32_t logV = std::min(vtxCount, 8u);
      for (uint32_t v = 0; v < logV; ++v)
      {
        const auto &p = decoded.positions[v];
        const auto &n = decoded.normals[v];
        os::Logger::logf("    v[%u]  pos=(%.5f, %.5f, %.5f)  norm=(%.4f, %.4f, %.4f)", v, p[0], p[1], p[2], n[0], n[1], n[2]);
        aabbMinX = std::min(aabbMinX, p[0]);
        aabbMinY = std::min(aabbMinY, p[1]);
        aabbMinZ = std::min(aabbMinZ, p[2]);
        aabbMaxX = std::max(aabbMaxX, p[0]);
        aabbMaxY = std::max(aabbMaxY, p[1]);
        aabbMaxZ = std::max(aabbMaxZ, p[2]);
      }
      os::Logger::log("  Indices (first 12):");
      for (uint32_t k = 0; k < std::min<uint32_t>(12u, static_cast<uint32_t>(decoded.indices.size())); ++k)
        os::Logger::logf("    idx[%u] = %u", k, decoded.indices[k]);
    }

    {
      const uint32_t maxPageSz = vgFile.getMaxPageSize();
      std::vector<uint8_t> stagingBuf(maxPageSz);
      VirtualGeometryStreamedPage streamedPage;
      if (!vgFile.streamPageRaw(pageIndex, stagingBuf.data(), maxPageSz, streamedPage))
      {
        os::Logger::errorf("  ERROR cluster[%u]: streamPageRaw failed.", ci);
        allPassed = false;
      }
      else
      {
        std::vector<float> refPos;
        streamedPage.decodePositions(mi, refPos, qcfg);

        bool posOk = (refPos.size() == vtxCount * 3u);
        if (!posOk)
        {
          os::Logger::errorf("  ERROR cluster[%u]: ref pos count=%zu expected=%u", ci, refPos.size(), vtxCount * 3u);
          allPassed = false;
        }
        else
        {
          constexpr float kTol = 1e-4f;
          uint32_t mismatches = 0u;
          for (uint32_t v = 0; v < vtxCount; ++v)
          {
            const float dx = decoded.positions[v][0] - refPos[v * 3 + 0];
            const float dy = decoded.positions[v][1] - refPos[v * 3 + 1];
            const float dz = decoded.positions[v][2] - refPos[v * 3 + 2];
            if (std::abs(dx) > kTol || std::abs(dy) > kTol || std::abs(dz) > kTol)
            {
              ++mismatches;
              if (mismatches <= 2u)
                os::Logger::errorf(
                    "  MISMATCH cluster[%u] v[%u]: mirror=(%.6f,%.6f,%.6f) ref=(%.6f,%.6f,%.6f)",
                    ci,
                    v,
                    decoded.positions[v][0],
                    decoded.positions[v][1],
                    decoded.positions[v][2],
                    refPos[v * 3 + 0],
                    refPos[v * 3 + 1],
                    refPos[v * 3 + 2]);
            }
          }
          if (mismatches == 0u)
            ++clustersPositionMatch;
          else
          {
            allPassed = false;
            os::Logger::errorf("  cluster[%u]: %u position mismatches.", ci, mismatches);
          }
        }
      }
    }

    {
      bool normOk = true;
      for (uint32_t v = 0; v < vtxCount; ++v)
      {
        const auto &n = decoded.normals[v];
        const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (std::abs(len - 1.0f) > 1e-3f)
        {
          os::Logger::errorf("  ERROR cluster[%u] normal[%u] length=%.5f", ci, v, len);
          normOk = false;
          allPassed = false;
        }
      }
      if (normOk)
        ++clustersNormalsOk;
    }

    {
      bool anyVisible = false;
      for (uint32_t v = 0; v < vtxCount && !anyVisible; ++v)
      {
        const auto &p = decoded.positions[v];
        if (pointInFrustum(frustum, p[0], p[1], p[2]))
          anyVisible = true;
      }
      if (anyVisible)
        ++clustersInFrustum;
      else
        os::Logger::warningf("  WARNING cluster[%u] (page=%u, localIdx=%u, parentNode=%u): no vertex passed CPU frustum test.", ci, m.pageIndex, m.pageLocalClusterIndex, m.parentNodeIndex);
    }
  }

  for (const auto &m : meshlets)
  {
    const uint32_t mi = m.pageLocalClusterIndex;
    if (mi >= numMeshlets)
      continue;
    auto decoded = cpu_shader_mirror::decodeMeshlet(gpuPageWords.data(), 0u, mi, qcfg.unit_scale);
    for (const auto &p : decoded.positions)
    {
      aabbMinX = std::min(aabbMinX, p[0]);
      aabbMinY = std::min(aabbMinY, p[1]);
      aabbMinZ = std::min(aabbMinZ, p[2]);
      aabbMaxX = std::max(aabbMaxX, p[0]);
      aabbMaxY = std::max(aabbMaxY, p[1]);
      aabbMaxZ = std::max(aabbMaxZ, p[2]);
    }
  }

  os::Logger::log("\n  --- CPU Decode Summary ---");
  os::Logger::logf("  Clusters decoded            : %u / %zu", clustersDecoded, meshlets.size());
  os::Logger::logf("  Positions match reference   : %u / %u", clustersPositionMatch, clustersDecoded);
  os::Logger::logf("  Normals unit-length         : %u / %u", clustersNormalsOk, clustersDecoded);
  os::Logger::logf("  >=1 vertex in CPU frustum   : %u / %zu", clustersInFrustum, meshlets.size());
  os::Logger::logf("  Decoded geometry AABB :");
  os::Logger::logf("    min=(%.5f, %.5f, %.5f)", aabbMinX, aabbMinY, aabbMinZ);
  os::Logger::logf("    max=(%.5f, %.5f, %.5f)", aabbMaxX, aabbMaxY, aabbMaxZ);
  os::Logger::logf("    centre=(%.5f, %.5f, %.5f)", (aabbMinX + aabbMaxX) * 0.5f, (aabbMinY + aabbMaxY) * 0.5f, (aabbMinZ + aabbMaxZ) * 0.5f);

  if (clustersInFrustum == 0u)
  {
    os::Logger::error(
        "  CRITICAL: Zero clusters visible in CPU frustum!\n"
        "  Possible causes:\n"
        "    1. Object is at the wrong position (check model matrix / instance transform)\n"
        "    2. Camera look-at is wrong\n"
        "    3. Frustum plane extraction is wrong for reverseZ=true\n"
        "    4. The hardcoded AABB centre is stale vs actual encoded positions");
    allPassed = false;
  }

  os::Logger::log("========================================\n");
  return allPassed;
}

static bool verifyGPUBuffers(
    rendering::RenderGraph *renderGraph,
    rendering::Buffer countersBuffer,
    rendering::Buffer clusterInfosBuffer,
    rendering::Buffer drawIndirectBuffer,
    const std::vector<uint32_t> &cpuCounters,
    const std::vector<VisibleClusterInfo_CPU> &cpuClusterInfos,
    const std::vector<uint32_t> &cpuDrawCmds,
    uint32_t hwVisibleCount)
{
  os::Logger::log("\n========================================");
  os::Logger::log("[DBG-2] GPU Buffer Readback Verification");
  os::Logger::log("========================================");
  bool ok = true;

  {
    std::vector<uint32_t> gpuCounters(COUNTER_SLOTS, 0u);
    renderGraph->bufferRead(
        countersBuffer,
        0,
        COUNTER_SLOTS * sizeof(uint32_t),
        [&](const void *d)
        {
          std::memcpy(gpuCounters.data(), d, COUNTER_SLOTS * sizeof(uint32_t));
        });

    os::Logger::log("  Counters buffer (first 8 slots):");
    for (uint32_t i = 0; i < 8u; ++i)
      os::Logger::logf("    slot[%u]: CPU=%u  GPU=%u  %s", i, cpuCounters[i], gpuCounters[i], (gpuCounters[i] == cpuCounters[i]) ? "OK" : "MISMATCH");

    if (gpuCounters[HW_VISIBLE_CLUSTER_COUNT_INDEX] != hwVisibleCount)
    {
      os::Logger::errorf("  ERROR: HW_VISIBLE_CLUSTER_COUNT slot %u: GPU=%u expected=%u", HW_VISIBLE_CLUSTER_COUNT_INDEX, gpuCounters[HW_VISIBLE_CLUSTER_COUNT_INDEX], hwVisibleCount);
      ok = false;
    }
    else
      os::Logger::logf("  OK: HW_VISIBLE_CLUSTER_COUNT = %u", hwVisibleCount);
  }

  {
    const uint32_t byteSize = hwVisibleCount * sizeof(VisibleClusterInfo_CPU);
    std::vector<VisibleClusterInfo_CPU> gpuInfos(hwVisibleCount);
    renderGraph->bufferRead(
        clusterInfosBuffer,
        0,
        byteSize,
        [&](const void *d)
        {
          std::memcpy(gpuInfos.data(), d, byteSize);
        });

    os::Logger::logf("  VisibleClusterInfos (%u entries):", hwVisibleCount);
    for (uint32_t i = 0; i < hwVisibleCount; ++i)
    {
      const bool match =
          (gpuInfos[i].pageIndex == cpuClusterInfos[i].pageIndex && gpuInfos[i].pageLocalClusterIndex == cpuClusterInfos[i].pageLocalClusterIndex && gpuInfos[i].instanceIndex == cpuClusterInfos[i].instanceIndex &&
           gpuInfos[i]._padding == cpuClusterInfos[i]._padding);
      os::Logger::logf(
          "    [%u] page=%u localCluster=%u instance=%u parentNode=%u  %s", i, gpuInfos[i].pageIndex, gpuInfos[i].pageLocalClusterIndex, gpuInfos[i].instanceIndex, gpuInfos[i]._padding, match ? "OK" : "MISMATCH");
      if (!match)
        ok = false;
    }
  }

  {
    const uint32_t byteSize = hwVisibleCount * 4u * sizeof(uint32_t);
    std::vector<uint32_t> gpuCmds(hwVisibleCount * 4u);
    renderGraph->bufferRead(
        drawIndirectBuffer,
        0,
        byteSize,
        [&](const void *d)
        {
          std::memcpy(gpuCmds.data(), d, byteSize);
        });

    os::Logger::logf("  DrawIndirect commands (%u):", hwVisibleCount);
    for (uint32_t i = 0; i < hwVisibleCount; ++i)
    {
      const uint32_t base = i * 4u;
      const bool match = (gpuCmds[base + 0] == cpuDrawCmds[base + 0] && gpuCmds[base + 1] == cpuDrawCmds[base + 1] && gpuCmds[base + 2] == cpuDrawCmds[base + 2] && gpuCmds[base + 3] == cpuDrawCmds[base + 3]);
      os::Logger::logf(
          "    cmd[%u]  vtxCount=%u(exp=%u)  instCount=%u  firstVtx=%u  firstInst=%u  %s", i, gpuCmds[base + 0], cpuDrawCmds[base + 0], gpuCmds[base + 1], gpuCmds[base + 2], gpuCmds[base + 3], match ? "OK" : "MISMATCH");
      if (!match)
        ok = false;
    }
  }

  os::Logger::log(ok ? "  All buffer readbacks PASSED." : "  ERROR: One or more buffer readbacks FAILED.");
  os::Logger::log("========================================\n");
  return ok;
}

// ============================================================================
// Helpers — build a RenderToQuadPass::Settings for one cell in a 3×2 grid
// ============================================================================

// Layout:
//
//  ┌──────────┬──────────┬──────────┐
//  │  cell 0  │  cell 1  │  cell 2  │   row 0  (top)
//  │ worldPos │  normal  │    uv    │
//  ├──────────┼──────────┼──────────┤
//  │  cell 3  │  cell 4  │  cell 5  │   row 1  (bottom)
//  │  nodeId  │  triId   │instanceId│
//  └──────────┴──────────┴──────────┘
//
//  Each cell is (vpW/3) × (vpH/2) pixels.

static rendering::passes::RenderToQuadPass::Settings makeGridCellSettings(
    uint32_t cellIndex, // 0..5
    uint32_t vpW,
    uint32_t vpH,
    uint32_t sourceMipLevel = 0,
    uint32_t sourceLayer = 0)
{
  const uint32_t cols = 3u;
  const uint32_t cellW = vpW / cols;
  const uint32_t cellH = vpH / 2u;
  const uint32_t col = cellIndex % cols;
  const uint32_t row = cellIndex / cols;

  rendering::passes::RenderToQuadPass::Settings s;
  s.viewPortWidth = vpW;
  s.viewPortHeight = vpH;
  s.quad = rendering::Rect2D(col * cellW, row * cellH, cellW, cellH);
  s.sourceMipLevel = sourceMipLevel;
  s.sourceLayer = sourceLayer;
  return s;
}

// ============================================================================
// main
// ============================================================================
int main()
{
  os::Logger::start(100);

  // -------------------------------------------------------------------------
  // [1] Encode .obj → .virtualgeometry
  // -------------------------------------------------------------------------
  std::string exeDir = utils::getExecutableDirectory();
  std::string objPath = exeDir + "/assets/meshes/obj/suzanne.obj";
  std::string vgPath = objPath + ".virtualgeometry";
  std::string objName = "suzanne";

  os::Logger::log("[1] Encoding suzanne.obj...");

  std::vector<Vertex> vertices;
  Shape shape;
  VirtualGeometryEncoder::loadOBJ(objPath, true, vertices, shape);

  VirtualGeometryBuildData build = VirtualGeometryBuilder::build(VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape));

  QuantizationConfig qcfg;
  qcfg.quantization_factor = 4;
  qcfg.unit_scale = 100.0f;

  VirtualGeometryEncodedData encoded = VirtualGeometryEncoder::encode(vertices, shape, qcfg);

  {
    VirtualGeometryFile writer(vgPath, /*write=*/true);
    if (!writer.isOpen())
      throw std::runtime_error("Cannot open VG file for writing: " + vgPath);
    if (!writer.write(encoded, build.pages, MESHLET_LZ4))
      throw std::runtime_error("Failed to write VG file: " + vgPath);
  }

  const uint32_t pageCount = static_cast<uint32_t>(encoded.pages.size());
  const uint32_t hierarchySize = static_cast<uint32_t>(encoded.hierarchy.size());
  os::Logger::logf("  Pages: %u  Hierarchy nodes: %u", pageCount, hierarchySize);

  // -------------------------------------------------------------------------
  // [2] Window + RHI
  // -------------------------------------------------------------------------
  os::Logger::log("[2] Creating window and Vulkan RHI...");

  window::Window *window = new window::sdl3::SDL3Window(window::WindowSurface_Vulkan, "VG HW Draw Pass (isolated debug)", 1920, 1080);

  DeviceRequiredLimits limits = {0, 0, 0};
  DeviceFeatures features = DeviceFeatures::DeviceFeatures_Compute | DeviceFeatures::DeviceFeatures_Subgroup_Basic | DeviceFeatures::DeviceFeatures_Subgroup_Shuffle | DeviceFeatures::DeviceFeatures_Timestamp;

  vulkan::VulkanRHI *rhi = new vulkan::VulkanRHI(vulkan::Vulkan_1_2, limits, features, {});

  std::vector<VkSurfaceKHR> surfaces;
  surfaces.push_back(window->getVulkanSurface(rhi->getInstance()));
  rhi->init(surfaces);

  SwapChain swapChain = rhi->createSwapChain(0, window->getWidth(), window->getHeight());

  RenderGraph *renderGraph = new RenderGraph(rhi);
  renderGraph->addSwapChainImages(swapChain);

  const uint32_t kVW = rhi->getSwapChainImagesWidth(swapChain);
  const uint32_t kVH = rhi->getSwapChainImagesHeight(swapChain);

  // -------------------------------------------------------------------------
  // [3] Scene
  // -------------------------------------------------------------------------
  os::Logger::log("[3] Building VirtualGeometryScene...");

  const uint32_t kHierarchyBufBytes = std::max(hierarchySize * static_cast<uint32_t>(sizeof(VirtualGeometryHierarchy)) * 4u, 4096u);
  const uint64_t kPagesBufBytes = 256ull * 1024 * 1024;

  VirtualGeometryScene scene(renderGraph, kHierarchyBufBytes, kPagesBufBytes);
  scene.registerObjectForStreaming(objName, vgPath);

  InstanceId instId = scene.instantiateObjectInstance(objName, math::Vec3f(0.0f, 0.0f, 0.0f), math::Quatf::identity(), 1.0f);
  scene.updateInstanceBuffer();

  // -------------------------------------------------------------------------
  // [4] Hardcoded culling results
  // -------------------------------------------------------------------------
  os::Logger::log("[4] Using hardcoded culling results...");

  std::vector<MeshletRef> meshlets = getHardcodedCullingResults();
  const uint32_t hwVisibleCount = static_cast<uint32_t>(meshlets.size());

  // -------------------------------------------------------------------------
  // [5] Build CPU-side data
  // -------------------------------------------------------------------------
  os::Logger::log("[5] Building counters / visibleClusterInfos / drawIndirect...");

  auto cpuCounters = buildCounters(hwVisibleCount);
  auto cpuClusterInfos = buildVisibleClusterInfos(meshlets);
  auto cpuDrawCmds = buildDrawIndirectCommands(meshlets);

  // -------------------------------------------------------------------------
  // [6] Create GPU buffers + upload
  // -------------------------------------------------------------------------
  os::Logger::log("[6] Creating and uploading GPU buffers...");

  const uint64_t kCountersBufBytes = COUNTER_SLOTS * sizeof(uint32_t);

  Buffer cpuCountersBuffer = renderGraph->createBuffer(
      BufferInfo{
        .name = "cpu_CountersBuffer.buffer",
        .size = kCountersBufBytes,
        .scratch = false,
        .usage = BufferUsage::BufferUsage_Storage | BufferUsage::BufferUsage_Push | BufferUsage::BufferUsage_CopySrc,
      });
  const auto MAX_VISIBLE_CLUSTERS = 1024U;
  auto drawIndirectBufferSize = static_cast<uint64_t>(MAX_VISIBLE_CLUSTERS) * 4u * sizeof(uint32_t);
  auto drawIndirectBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryDrawIndirectBuffer",
        .size = drawIndirectBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_Indirect | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_Push,
      });

  auto visibleClusterInfosBufferSize = static_cast<uint64_t>(MAX_VISIBLE_CLUSTERS) * sizeof(VisibleClusterInfo);
  auto visibleClusterInfosBuffer = renderGraph->createBuffer(
      rendering::BufferInfo{
        .name = "VirtualGeometryVisibleClusterInfosBuffer",
        .size = visibleClusterInfosBufferSize,
        .scratch = false,
        .usage = rendering::BufferUsage::BufferUsage_Storage | rendering::BufferUsage::BufferUsage_CopySrc | rendering::BufferUsage::BufferUsage_Push,
      });

  renderGraph->bufferWrite(cpuCountersBuffer, 0, kCountersBufBytes, cpuCounters.data());
  renderGraph->bufferWrite(visibleClusterInfosBuffer, 0, hwVisibleCount * sizeof(VisibleClusterInfo_CPU), cpuClusterInfos.data());
  renderGraph->bufferWrite(drawIndirectBuffer, 0, hwVisibleCount * 4u * sizeof(uint32_t), cpuDrawCmds.data());

  constexpr bool kReverseZ = true;
  constexpr float kClearDepth = 0.0f;
  constexpr float kFovY = 60.0f * (3.14159265f / 180.0f);
  constexpr float kAspect = 16.0f / 9.0f;
  constexpr float kNear = 0.1f;
  constexpr float kFar = 10000.0f;

  const math::Vec3f kObjectCentre(-2.49406f, 1.25169f, 4.10390f);
  constexpr float kRadius = 1.73547f;
  constexpr float kCamDist = kRadius * 3.0f + 1.0f;

  math::Vec3f camPos(kObjectCentre[0], kObjectCentre[1], kObjectCentre[2] + kCamDist);
  math::Vec3f camForward(0.0f, 0.0f, -1.0f);

  rendering::Camera cam(kFovY, camPos, camForward, kReverseZ);
  cam.setAspectRatio(kAspect);
  cam.setNearFar(kNear, kFar);
  cam.lookAt(kObjectCentre);
  cam.updateMatrices();

  printCameraDiagnostic(cam, kObjectCentre, kNear, kFar);

  std::vector<PageTableEntry> pageTable(pageCount);
  renderGraph->bufferRead(
      scene.pageTableBuffer,
      0,
      pageCount * sizeof(PageTableEntry),
      [&](const void *d)
      {
        std::memcpy(pageTable.data(), d, pageCount * sizeof(PageTableEntry));
      });

  os::Logger::log("  Page table:");
  for (uint32_t pi = 0; pi < pageCount; ++pi)
    os::Logger::logf("    page[%u]: isInstalled=%u  bufferOffset=%u  size=%u", pi, pageTable[pi].isInstalled, pageTable[pi].bufferOffset, pageTable[pi].size);

  bool buffersOk = verifyGPUBuffers(renderGraph, cpuCountersBuffer, visibleClusterInfosBuffer, drawIndirectBuffer, cpuCounters, cpuClusterInfos, cpuDrawCmds, hwVisibleCount);

  if (!buffersOk)
    os::Logger::error("  WARNING: Buffer verification failed — draw output will be wrong.");

  bool decodeOk = runCPUDecodeValidation(meshlets, scene, renderGraph, vgPath, qcfg, cam, pageTable);
  if (!decodeOk)
    os::Logger::error("  WARNING: CPU decode validation failed — check encoder / page data.");

  // -------------------------------------------------------------------------
  // [8] Render targets
  // -------------------------------------------------------------------------
  os::Logger::log("[8] Creating render targets...");

  Texture depthTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "DepthTexture",
        .format = Format::Format_Depth32Float,
        .height = kVH,
        .width = kVW,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_DepthStencilAttachment,
      });

  Texture packedGeometryIdsLoTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "PackedGeometryIdsLoTexture",
        .format = Format::Format_R32Uint,
        .height = kVH,
        .width = kVW,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      });
  Texture packedGeometryIdsHiTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "PackedGeometryIdsHiTexture",
        .format = Format::Format_R32Uint,
        .height = kVH,
        .width = kVW,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      });
  Texture materialIdTexture = renderGraph->createTexture(
      TextureInfo{
        .name = "MaterialIdTexture",
        .format = Format::Format_R32Uint,
        .height = kVH,
        .width = kVW,
        .memoryProperties = BufferUsage::BufferUsage_None,
        .mipLevels = 1u,
        .usage = ImageUsage::ImageUsage_ColorAttachment | ImageUsage::ImageUsage_Sampled,
      });
  TextureView depthView = TextureView{
    .texture = depthTexture,
    .access = AccessPattern::DEPTH_STENCIL_ATTACHMENT_WRITE | AccessPattern::DEPTH_STENCIL_ATTACHMENT_READ,
    .layout = ResourceLayout::DEPTH_STENCIL_ATTACHMENT,
    .index = 0,
    .flags = ImageAspectFlags::Depth,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };

  // Virtual swapchain texture — overridden each frame with the real image.
  Texture swapChainTexture = renderGraph->createTexture(
      TextureInfo{
        .isVirtual = true,
        .name = "ColorAttatchmentTexture",
        .height = kVH,
        .width = kVW,
        .format = rhi->getSwapChainFormat(swapChain),
        .depth = 1,
        .memoryProperties = rendering::BufferUsage::BufferUsage_None,
        .mipLevels = 1,
        .usage = rendering::ImageUsage::ImageUsage_ColorAttachment,
      });

  TextureView swapChainView = TextureView{
    .texture = swapChainTexture,
    .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
    .layout = ResourceLayout::COLOR_ATTACHMENT,
    .index = 0,
    .flags = ImageAspectFlags::Color,
    .baseArrayLayer = 0,
    .baseMipLevel = 0,
    .layerCount = 1,
    .levelCount = 1,
  };

  VirtualGeometryHardwareDrawPass::FrameTarget frameTarget{
    .packedGeometryIdsLoView =
        TextureView{
          .texture = packedGeometryIdsLoTexture,
          .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
          .layout = ResourceLayout::COLOR_ATTACHMENT,
          .index = 0,
          .flags = ImageAspectFlags::Color,
          .baseArrayLayer = 0,
          .baseMipLevel = 0,
          .layerCount = 1,
          .levelCount = 1},
    .packedGeometryIdsHiView =
        TextureView{
          .texture = packedGeometryIdsHiTexture,
          .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
          .layout = ResourceLayout::COLOR_ATTACHMENT,
          .index = 0,
          .flags = ImageAspectFlags::Color,
          .baseArrayLayer = 0,
          .baseMipLevel = 0,
          .layerCount = 1,
          .levelCount = 1},
    .materialIdView =
        TextureView{
          .texture = materialIdTexture,
          .access = AccessPattern::COLOR_ATTACHMENT_WRITE,
          .layout = ResourceLayout::COLOR_ATTACHMENT,
          .index = 0,
          .flags = ImageAspectFlags::Color,
          .baseArrayLayer = 0,
          .baseMipLevel = 0,
          .layerCount = 1,
          .levelCount = 1},
    .depthView = depthView,
    .depthTexture = depthTexture,
    .colorView = swapChainView,
    .colorTexture = swapChainTexture,
  };

  // -------------------------------------------------------------------------
  // [9] Register G-buffer draw pass
  // -------------------------------------------------------------------------
  os::Logger::log("[9] Registering VirtualGeometryHardwareDrawPass...");

  VirtualGeometryHardwareDrawPass::Settings hwSettings;
  hwSettings.viewPortWidth = kVW;
  hwSettings.viewPortHeight = kVH;
  hwSettings.colorFormat = rhi->getSwapChainFormat(swapChain);
  hwSettings.depthFormat = Format::Format_Depth32Float;
  hwSettings.maxDrawnClusters = MAX_VISIBLE_CLUSTERS;

  auto *hwDrawPass = renderGraph->registerPass<VirtualGeometryHardwareDrawPass>(
      "hwDrawPass",
      0,
      scene,
      drawIndirectBuffer,
      visibleClusterInfosBuffer,
      cpuCountersBuffer,
      frameTarget,
      /*shouldClearDepth=*/true,
      kClearDepth,
      hwSettings);

  // -------------------------------------------------------------------------
  // [10] Register 2 × RenderToQuadPass
  //
  //  ┌──────────┬──────────┐
  //  │ packedLo │ packedHi │
  //  └──────────┴──────────┘
  // -------------------------------------------------------------------------
  os::Logger::log("[10] Registering RenderToQuadPass instances...");

  struct QuadDef
  {
    const char *name;
    Texture srcTex;
  };

  const QuadDef quadDefs[2] = {
    {"quadPass_PackedLo", packedGeometryIdsLoTexture},
    {"quadPass_PackedHi", packedGeometryIdsHiTexture},
  };

  rendering::passes::RenderToQuadPass *quadPasses[2] = {};

  for (uint32_t i = 0; i < 2u; ++i)
  {
    rendering::passes::RenderToQuadPass::Settings settings = makeGridCellSettings(i, kVW, kVH);

    quadPasses[i] = renderGraph->registerPass<rendering::passes::RenderToQuadPass>(
        quadDefs[i].name,
        static_cast<uint32_t>(i + 1), // index — must be unique per pass
        swapChainTexture,             // output texture
        rhi->getSwapChainFormat(swapChain),
        quadDefs[i].srcTex, // input texture
        /*sourceMipLevel=*/0u,
        /*sourceLayer=*/0u,
        settings);
  }

  // -------------------------------------------------------------------------
  // [11] Compile
  // -------------------------------------------------------------------------
  os::Logger::log("[11] Compiling render graph...");
  renderGraph->compile();

  os::Logger::logf("Setup complete. buffersOk=%s  decodeOk=%s  Rendering %u hardcoded clusters.", buffersOk ? "YES" : "NO", decodeOk ? "YES" : "NO", hwVisibleCount);

  // -------------------------------------------------------------------------
  // Frame loop
  // -------------------------------------------------------------------------
  constexpr uint32_t kMaxFrames = 30u * 60u;
  uint32_t frameCounter = 0u;
  float deltaTime = 0.0f;

  while (!window->shouldClose() && frameCounter < kMaxFrames)
  {
    auto frameStart = lib::time::TimeSpan::now();

    const math::Mat4f &view = cam.getViewMatrix();
    const math::Mat4f &proj = cam.getProjectionMatrix();
    const math::Vec3f cp = cam.getPosition();

    math::Vec4f vp;
    vp[0] = cp[0];
    vp[1] = cp[1];
    vp[2] = cp[2];
    vp[3] = 1.0f;
    os::Logger::warningf("Camera position = (%f, %f, %f), direction = (%f, %f, %f)", cp[0], cp[1], cp[2], camForward[0], camForward[1], camForward[2]);

    hwDrawPass->updateUniforms(view.data, proj.data, vp.data, kVW, kVH, kNear, kFar, /*hiZLevels=*/1u);

    RenderGraph::Frame frame;
    RenderGraph::Overrides overrides;

    // Point the virtual swapchain texture at the real current image.
    auto currentSwapView = rhi->getCurrentSwapChainTextureView(swapChain);

    overrides.textureOverrides.emplace(
        "ColorAttatchmentTexture",
        RenderGraph::TextureOverride{
          .textureId = currentSwapView.resourceId,
          .layout = rendering::ResourceLayout::UNDEFINED,
        });

    renderGraph->run(frame, overrides);
    renderGraph->waitFrame(frame);

    auto frameEnd = lib::time::TimeSpan::now();

    rhi->present(swapChain, currentSwapView.resourceId, rendering::ResourceLayout::COLOR_ATTACHMENT);

    window->update();
    deltaTime = (frameEnd - frameStart).milliseconds();
    ++frameCounter;

    if (frameCounter % 60u == 0u || frameCounter <= 5u)
      os::Logger::logf("Frame %u | FPS=%.1f | dt=%.2fms | buffersOk=%s | decodeOk=%s", frameCounter, 1000.0f / deltaTime, deltaTime, buffersOk ? "Y" : "N", decodeOk ? "Y" : "N");
  }

  os::Logger::logf("Ran %u frames. Shutting down.", frameCounter);

  os::Logger::log("\n========================================");
  os::Logger::log(" Diagnostic Summary");
  os::Logger::log("========================================");
  os::Logger::logf("   Reverse-Z config  : clearDepth=%.1f  reverseZ=%s  %s", kClearDepth, kReverseZ ? "true" : "false", (kReverseZ && kClearDepth == 0.0f) ? "CORRECT" : "CHECK");
  os::Logger::logf("   Pipeline depth op : depthCompareOp must be GREATER for reverseZ");
  os::Logger::logf("   Buffer readback   : %s", buffersOk ? "PASS" : "FAIL");
  os::Logger::logf("   CPU decode        : %s", decodeOk ? "PASS" : "FAIL");

  rhi->waitIdle();
  renderGraph->removeSwapChainImages(swapChain);
  rhi->destroySwapChain(swapChain);
  delete renderGraph;
  os::Logger::shutdown();
  delete window;
  delete rhi;
  return 0;
}
