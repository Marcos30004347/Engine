// VirtualGeometryCompressor.cpp
#include "VirtualGeometryCompressor.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

#define CHECK_AABBS

namespace virtualgeometry
{

// ============================================================================
// Internal bit-packing helpers
// ============================================================================

namespace
{

inline uint8_t calculate_bits_needed(uint32_t range)
{
  if (range == 0)
    return 1;
  return static_cast<uint8_t>(std::ceil(std::log2(static_cast<float>(range))));
}

inline uint8_t calculate_bits_needed_from_span(uint32_t span)
{
  uint8_t bits = 0u;
  while (span != 0u)
  {
    ++bits;
    span >>= 1u;
  }
  return bits;
}

inline void write_bits(std::vector<uint32_t> &buf, uint32_t &bit_offset, uint32_t value, uint8_t num_bits)
{
  if (num_bits == 0)
    return;
  uint32_t wi = bit_offset / 32, bi = bit_offset % 32;
  while (buf.size() <= wi + 1)
    buf.push_back(0);
  uint32_t mask = (num_bits == 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
  value &= mask;
  buf[wi] |= (value << bi);
  if (bi + num_bits > 32)
    buf[wi + 1] |= (value >> (num_bits - ((bi + num_bits) - 32)));
  bit_offset += num_bits;
}

inline uint32_t read_bits(const std::vector<uint32_t> &buf, uint32_t &bit_offset, uint8_t num_bits)
{
  if (num_bits == 0)
    return 0;
  uint32_t wi = bit_offset / 32, bi = bit_offset % 32;
  assert(wi < buf.size());
  uint32_t value = buf[wi] >> bi;
  if (bi + num_bits > 32)
  {
    assert(wi + 1 < buf.size());
    value |= (buf[wi + 1] << (32 - bi));
  }
  uint32_t mask = (num_bits == 32) ? 0xFFFFFFFFu : ((1u << num_bits) - 1u);
  value &= mask;
  bit_offset += num_bits;
  return value;
}

inline float clamp_snorm(float v)
{
  return std::max(-1.f, std::min(1.f, v));
}

inline uint32_t f32_bits(float f)
{
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}
inline float bits_f32(uint32_t u)
{
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

struct MeshletAABB
{
  float min_x, min_y, min_z;
  float max_x, max_y, max_z;
};

inline MeshletAABB computeMeshletAABB(const Meshlet &meshlet, const QuantizationConfig &config)
{
  const float dq = static_cast<float>(1u << meshlet.vertex_position_quantization_factor) * config.unit_scale;
  const float minX = static_cast<float>(meshlet.min_vertex_position_channel_x) / dq;
  const float minY = static_cast<float>(meshlet.min_vertex_position_channel_y) / dq;
  const float minZ = static_cast<float>(meshlet.min_vertex_position_channel_z) / dq;

  return MeshletAABB{
      .min_x = minX,
      .min_y = minY,
      .min_z = minZ,
      .max_x = (static_cast<float>(meshlet.min_vertex_position_channel_x) + static_cast<float>(meshlet.quantized_position_span_x)) / dq,
      .max_y = (static_cast<float>(meshlet.min_vertex_position_channel_y) + static_cast<float>(meshlet.quantized_position_span_y)) / dq,
      .max_z = (static_cast<float>(meshlet.min_vertex_position_channel_z) + static_cast<float>(meshlet.quantized_position_span_z)) / dq,
  };
}

} // anonymous namespace

// ============================================================================
// Octahedral encoding
// ============================================================================

void VirtualGeometryCompressor::octahedralEncode(const float n[3], float &ox, float &oy)
{
  float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (len < 1e-6f)
  {
    ox = oy = 0.f;
    return;
  }
  float nx = n[0] / len, ny = n[1] / len, nz = n[2] / len;
  float l1 = std::abs(nx) + std::abs(ny) + std::abs(nz);
  ox = nx / l1;
  oy = ny / l1;
  if (nz < 0.f)
  {
    float ox2 = ox, oy2 = oy;
    ox = (1.f - std::abs(oy2)) * (ox2 >= 0.f ? 1.f : -1.f);
    oy = (1.f - std::abs(ox2)) * (oy2 >= 0.f ? 1.f : -1.f);
  }
}

void VirtualGeometryCompressor::octahedralDecode(float ox, float oy, float out[3])
{
  out[0] = ox;
  out[1] = oy;
  out[2] = 1.f - std::abs(ox) - std::abs(oy);
  if (out[2] < 0.f)
  {
    float x = out[0], y = out[1];
    out[0] = (1.f - std::abs(y)) * (x >= 0.f ? 1.f : -1.f);
    out[1] = (1.f - std::abs(x)) * (y >= 0.f ? 1.f : -1.f);
  }
  float len = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (len > 1e-6f)
  {
    out[0] /= len;
    out[1] /= len;
    out[2] /= len;
  }
}

// ============================================================================
// SNORM packing
// ============================================================================

uint32_t VirtualGeometryCompressor::pack2x16Snorm(float x, float y)
{
  x = clamp_snorm(x);
  y = clamp_snorm(y);
  auto xs = static_cast<int16_t>(std::round(x * 32767.f));
  auto ys = static_cast<int16_t>(std::round(y * 32767.f));
  return (static_cast<uint32_t>(static_cast<uint16_t>(xs))) | (static_cast<uint32_t>(static_cast<uint16_t>(ys)) << 16);
}

void VirtualGeometryCompressor::unpack2x16Snorm(uint32_t p, float &ox, float &oy)
{
  ox = clamp_snorm(static_cast<float>(static_cast<int16_t>(p & 0xFFFFu)) / 32767.f);
  oy = clamp_snorm(static_cast<float>(static_cast<int16_t>((p >> 16) & 0xFFFFu)) / 32767.f);
}

// ============================================================================
// Position encoding
// ============================================================================

void VirtualGeometryCompressor::encodeMeshletPositions(const std::vector<Vertex> &vertices, const QuantizationConfig &config, Meshlet &meshlet, std::vector<uint32_t> &pos_buf)
{
  if (vertices.empty())
    return;

  const float qf = static_cast<float>(1 << config.quantization_factor) * config.unit_scale;

  struct IVec3
  {
    int32_t x, y, z;
  };
  std::vector<IVec3> qp(vertices.size());
  IVec3 mn = {INT32_MAX, INT32_MAX, INT32_MAX};
  IVec3 mx = {INT32_MIN, INT32_MIN, INT32_MIN};

  for (size_t i = 0; i < vertices.size(); ++i)
  {
    qp[i].x = static_cast<int32_t>(std::round(vertices[i].pos[0] * qf));
    qp[i].y = static_cast<int32_t>(std::round(vertices[i].pos[1] * qf));
    qp[i].z = static_cast<int32_t>(std::round(vertices[i].pos[2] * qf));
    mn.x = std::min(mn.x, qp[i].x);
    mx.x = std::max(mx.x, qp[i].x);
    mn.y = std::min(mn.y, qp[i].y);
    mx.y = std::max(mx.y, qp[i].y);
    mn.z = std::min(mn.z, qp[i].z);
    mx.z = std::max(mx.z, qp[i].z);
  }

  const uint32_t spanX = static_cast<uint32_t>(mx.x - mn.x);
  const uint32_t spanY = static_cast<uint32_t>(mx.y - mn.y);
  const uint32_t spanZ = static_cast<uint32_t>(mx.z - mn.z);
  const uint8_t bx = calculate_bits_needed(spanX + 1u);
  const uint8_t by = calculate_bits_needed(spanY + 1u);
  const uint8_t bz = calculate_bits_needed(spanZ + 1u);

  meshlet.quantized_position_span_x = spanX;
  meshlet.quantized_position_span_y = spanY;
  meshlet.quantized_position_span_z = spanZ;
  meshlet.vertex_position_quantization_factor = config.quantization_factor;
  meshlet.min_vertex_position_channel_x = mn.x;
  meshlet.min_vertex_position_channel_y = mn.y;
  meshlet.min_vertex_position_channel_z = mn.z;
  meshlet.start_vertex_position_bit = static_cast<uint32_t>(pos_buf.size() * 32);

  uint32_t bit_offset = meshlet.start_vertex_position_bit;
  for (const auto &v : qp)
  {
    write_bits(pos_buf, bit_offset, static_cast<uint32_t>(v.x - mn.x), bx);
    write_bits(pos_buf, bit_offset, static_cast<uint32_t>(v.y - mn.y), by);
    write_bits(pos_buf, bit_offset, static_cast<uint32_t>(v.z - mn.z), bz);
  }
}

// ============================================================================
// Normal encoding
// ============================================================================

void VirtualGeometryCompressor::encodeMeshletNormals(const std::vector<Vertex> &vertices, std::vector<uint32_t> &normal_buf)
{
  for (const auto &v : vertices)
  {
    float ox, oy;
    octahedralEncode(v.norm, ox, oy);
    normal_buf.push_back(pack2x16Snorm(ox, oy));
  }
}

// ============================================================================
// Bone weight encoding
// ============================================================================

void VirtualGeometryCompressor::encodeMeshletBoneWeights(const std::vector<Vertex> &vertices, Meshlet &meshlet, std::vector<uint32_t> &bw_buf)
{
  if (vertices.empty())
    return;

  const uint8_t infPerVert = static_cast<uint8_t>(vertices[0].boneWeights.size());
  if (infPerVert == 0)
  {
    meshlet.boneWeightsPerVertex = 0;
    meshlet.boneWeightOffset = 0;
    return;
  }

  meshlet.boneWeightsPerVertex = infPerVert;
  meshlet.boneWeightOffset = static_cast<uint32_t>(bw_buf.size());

  for (const auto &vert : vertices)
  {
    // All vertices in a meshlet must have the same influence count.
    assert(vert.boneWeights.size() == infPerVert);
    for (const auto &bw : vert.boneWeights)
    {
      bw_buf.push_back(f32_bits(bw.weight));
      bw_buf.push_back(bw.boneIndex);
    }
  }
}

// ============================================================================
// Main encode entry point
// ============================================================================

VirtualGeometryEncodedData VirtualGeometryCompressor::encode(const VirtualGeometryBuildData &build_data, const QuantizationConfig &config)
{
  VirtualGeometryEncodedData result;
  result.quantizationConfig = config;
  result.buildSettings = build_data.buildSettings;
  result.hierarchy = build_data.lodLevelHierarchy;
  result.shapes = build_data.shapes;
  result.materialFiles = build_data.materialFiles;
  result.meshParts = build_data.meshParts;
  result.skeleton = build_data.skeleton;
  result.root_node_index = 0;
  result.rootPageIndex = result.shapes.empty() ? 0u : result.shapes.front().root_page_index;

  result.pages.resize(build_data.pages.size());

  for (size_t page_idx = 0; page_idx < build_data.pages.size(); ++page_idx)
  {
    const VirtualGeometryBuildPage &bp = build_data.pages[page_idx];
    VirtualGeometryPage &rp = result.pages[page_idx];

    rp.installUpdates = bp.installUpdates;
    rp.uninstallUpdates = bp.uninstallUpdates;
    rp.dependencies = bp.dependencies;
    rp.meshlets.reserve(bp.clusterCount);
    rp.groupCount = static_cast<uint32_t>(bp.groups.size());
    rp.groupMeshletOffsets.resize(rp.groupCount, UINT32_MAX);

    uint32_t page_attr_offset = 0;
    uint32_t page_idx_offset = 0;

    uint32_t ci = 0u;
    for (const auto &groupSpan : bp.groups)
    {
      assert(groupSpan.localGroupIndex < rp.groupMeshletOffsets.size());
      rp.groupMeshletOffsets[groupSpan.localGroupIndex] = static_cast<uint32_t>(rp.meshlets.size());

      for (uint32_t clusterInGroup = 0u; clusterInGroup < groupSpan.clusterCount; ++clusterInGroup, ++ci)
      {
        const uint32_t gci = bp.clusterOffset + ci;
        assert(gci < build_data.clusters.size());
        const VirtualGeometryCluster &cluster = build_data.clusters[gci];

        Meshlet m;
        assert(cluster.vertices.size() <= 255 && "Meshlet vertex count exceeds uint8_t range");
        m.vertex_count = static_cast<uint8_t>(cluster.vertices.size());
        m.triangle_count = static_cast<uint8_t>(cluster.indices.size() / 3);
        m.pageLocalGroupIndex = static_cast<uint8_t>(groupSpan.localGroupIndex);
        m.clusterIndexInGroup = static_cast<uint8_t>(clusterInGroup);

        // ── LOD bounds ────────────────────────────────────────────────────
        m.self = cluster.self;
        m.parent = cluster.parent;
        m.cone = cluster.cone;

        // ── Positions ─────────────────────────────────────────────────────
        encodeMeshletPositions(cluster.vertices, config, m, rp.vertexBuffers.positions);

        // ── Normals / UVs ─────────────────────────────────────────────────
        m.start_vertex_attribute_id = page_attr_offset;
        encodeMeshletNormals(cluster.vertices, rp.vertexBuffers.normals);
        for (const auto &v : cluster.vertices)
        {
          rp.vertexBuffers.uvs.push_back(v.uv[0]);
          rp.vertexBuffers.uvs.push_back(v.uv[1]);
        }
        page_attr_offset += static_cast<uint32_t>(cluster.vertices.size());

        // ── Bone weights ───────────────────────────────────────────────────
        encodeMeshletBoneWeights(cluster.vertices, m, rp.vertexBuffers.boneWeights);

        // ── Indices ────────────────────────────────────────────────────────
        m.start_index_id = page_idx_offset;
        for (uint32_t idx : cluster.indices)
        {
          assert(idx < 256);
          rp.vertexBuffers.indices.push_back(static_cast<uint8_t>(idx));
        }
        if (config.padMeshlets && !cluster.indices.empty())
        {
          const uint8_t lastIdx = static_cast<uint8_t>(cluster.indices.back());
          const size_t targetSize = static_cast<size_t>(m.start_index_id) + ClusterSize * 3u;
          while (rp.vertexBuffers.indices.size() < targetSize)
            rp.vertexBuffers.indices.push_back(lastIdx);
        }
        while (rp.vertexBuffers.indices.size() % 4 != 0)
          rp.vertexBuffers.indices.push_back(0u);
        page_idx_offset = static_cast<uint32_t>(rp.vertexBuffers.indices.size());

        rp.meshlets.push_back(m);
      }
    }

    assert(ci == bp.clusterCount);
  }

  // ── Rebuild leaf node AABBs from quantized meshlet bounds exactly ────────
  for (auto &node : result.hierarchy)
  {
    if (!(node.flags & 1u))
      continue;

    if (node.child_count == 0u || node.child_start == UINT32_MAX)
      continue;

    const uint32_t pageIdx = node.pageIndex & ~(1u << 31);
    assert(pageIdx < result.pages.size());
    const VirtualGeometryPage &page = result.pages[pageIdx];

    node.min_x = node.min_y = node.min_z = std::numeric_limits<float>::max();
    node.max_x = node.max_y = node.max_z = -std::numeric_limits<float>::max();

    for (uint32_t localIdx = node.child_start; localIdx < node.child_start + node.child_count; ++localIdx)
    {
      assert(localIdx < page.meshlets.size());
      const MeshletAABB meshletBounds = computeMeshletAABB(page.meshlets[localIdx], config);
      node.min_x = std::min(node.min_x, meshletBounds.min_x);
      node.max_x = std::max(node.max_x, meshletBounds.max_x);
      node.min_y = std::min(node.min_y, meshletBounds.min_y);
      node.max_y = std::max(node.max_y, meshletBounds.max_y);
      node.min_z = std::min(node.min_z, meshletBounds.min_z);
      node.max_z = std::max(node.max_z, meshletBounds.max_z);
    }
  }

  // Bottom-up re-derivation of inner node AABBs.
  for (int32_t i = static_cast<int32_t>(result.hierarchy.size()) - 1; i >= 0; --i)
  {
    auto &node = result.hierarchy[i];
    if (node.flags & 1u)
      continue;
    node.min_x = node.min_y = node.min_z = std::numeric_limits<float>::max();
    node.max_x = node.max_y = node.max_z = -std::numeric_limits<float>::max();
    for (uint32_t ci = node.child_start; ci < node.child_start + node.child_count; ++ci)
    {
      const auto &ch = result.hierarchy[ci];
      node.min_x = std::min(node.min_x, ch.min_x);
      node.max_x = std::max(node.max_x, ch.max_x);
      node.min_y = std::min(node.min_y, ch.min_y);
      node.max_y = std::max(node.max_y, ch.max_y);
      node.min_z = std::min(node.min_z, ch.min_z);
      node.max_z = std::max(node.max_z, ch.max_z);
    }
  }

#ifdef CHECK_AABBS
  for (const auto &node : result.hierarchy)
  {
    if (!(node.flags & 1u))
      continue;

    // child_start is a LOCAL page-relative index after buildPagesAndRewrites.
    // Use pageIndex to find the owning page directly.
    uint32_t pageIdx = node.pageIndex & ~(1u << 31);
    assert(pageIdx < result.pages.size());
    const VirtualGeometryPage &page = result.pages[pageIdx];

    for (uint32_t j = 0; j < node.child_count; ++j)
    {
      uint32_t localIdx = node.child_start + j;
      assert(localIdx < page.meshlets.size());

      std::vector<float> pos;
      decodePositions(page, localIdx, pos, config);

      for (size_t v = 0; v < pos.size(); v += 3)
      {
        assert(pos[v] >= node.min_x && pos[v] <= node.max_x);
        assert(pos[v + 1] >= node.min_y && pos[v + 1] <= node.max_y);
        assert(pos[v + 2] >= node.min_z && pos[v + 2] <= node.max_z);
      }
    }
  }
#endif

  return result;
}

// ============================================================================
// Decode — positions
// ============================================================================

void VirtualGeometryCompressor::decodePositions(const VirtualGeometryPage &page, uint32_t midx, std::vector<float> &out, const QuantizationConfig &config)
{
  assert(midx < page.meshlets.size());
  const Meshlet &m = page.meshlets[midx];
  out.clear();
  out.reserve(m.vertex_count * 3);

  const uint8_t bx = calculate_bits_needed_from_span(m.quantized_position_span_x);
  const uint8_t by = calculate_bits_needed_from_span(m.quantized_position_span_y);
  const uint8_t bz = calculate_bits_needed_from_span(m.quantized_position_span_z);

  float dq = static_cast<float>(1 << m.vertex_position_quantization_factor) * config.unit_scale;
  uint32_t bit_offset = m.start_vertex_position_bit;

  for (uint32_t i = 0; i < m.vertex_count; ++i)
  {
    uint32_t x = read_bits(page.vertexBuffers.positions, bit_offset, bx);
    uint32_t y = read_bits(page.vertexBuffers.positions, bit_offset, by);
    uint32_t z = read_bits(page.vertexBuffers.positions, bit_offset, bz);
    out.push_back((static_cast<float>(static_cast<int32_t>(x) + m.min_vertex_position_channel_x)) / dq);
    out.push_back((static_cast<float>(static_cast<int32_t>(y) + m.min_vertex_position_channel_y)) / dq);
    out.push_back((static_cast<float>(static_cast<int32_t>(z) + m.min_vertex_position_channel_z)) / dq);
  }
}

// ============================================================================
// Decode — normals
// ============================================================================

void VirtualGeometryCompressor::decodeNormals(const VirtualGeometryPage &page, uint32_t midx, std::vector<float> &out)
{
  assert(midx < page.meshlets.size());
  const Meshlet &m = page.meshlets[midx];
  out.clear();
  out.reserve(m.vertex_count * 3);

  uint32_t base = m.start_vertex_attribute_id;
  for (uint32_t i = 0; i < m.vertex_count; ++i)
  {
    assert(base + i < page.vertexBuffers.normals.size());
    float ox, oy;
    unpack2x16Snorm(page.vertexBuffers.normals[base + i], ox, oy);
    float n[3];
    octahedralDecode(ox, oy, n);
    out.push_back(n[0]);
    out.push_back(n[1]);
    out.push_back(n[2]);
  }
}

// ============================================================================
// Decode — UVs
// ============================================================================

void VirtualGeometryCompressor::decodeUVs(const VirtualGeometryPage &page, uint32_t midx, std::vector<float> &out)
{
  assert(midx < page.meshlets.size());
  const Meshlet &m = page.meshlets[midx];
  out.clear();
  out.reserve(m.vertex_count * 2);

  uint32_t base = m.start_vertex_attribute_id * 2;
  for (uint32_t i = 0; i < m.vertex_count; ++i)
  {
    assert(base + i * 2 + 1 < page.vertexBuffers.uvs.size());
    out.push_back(page.vertexBuffers.uvs[base + i * 2 + 0]);
    out.push_back(page.vertexBuffers.uvs[base + i * 2 + 1]);
  }
}

// ============================================================================
// Decode — indices
// ============================================================================

void VirtualGeometryCompressor::decodeIndices(const VirtualGeometryPage &page, uint32_t midx, std::vector<uint8_t> &out)
{
  assert(midx < page.meshlets.size());
  const Meshlet &m = page.meshlets[midx];
  out.clear();

  uint32_t base = m.start_index_id;
  uint32_t count = static_cast<uint32_t>(m.triangle_count) * 3u;
  out.reserve(count);
  for (uint32_t i = 0; i < count; ++i)
  {
    assert(base + i < page.vertexBuffers.indices.size());
    out.push_back(page.vertexBuffers.indices[base + i]);
  }
}

// ============================================================================
// Decode — bone weights
// ============================================================================

void VirtualGeometryCompressor::decodeBoneWeights(const VirtualGeometryPage &page, uint32_t midx, std::vector<BoneWeight> &out)
{
  assert(midx < page.meshlets.size());
  const Meshlet &m = page.meshlets[midx];
  out.clear();

  if (m.boneWeightsPerVertex == 0)
    return;

  const uint32_t total = static_cast<uint32_t>(m.vertex_count) * m.boneWeightsPerVertex;
  out.reserve(total);

  // Each influence occupies 2 consecutive words: [weight_bits, boneIndex].
  uint32_t base = m.boneWeightOffset;
  for (uint32_t i = 0; i < total; ++i)
  {
    assert(base + i * 2 + 1 < page.vertexBuffers.boneWeights.size());
    BoneWeight bw;
    bw.weight = bits_f32(page.vertexBuffers.boneWeights[base + i * 2 + 0]);
    bw.boneIndex = page.vertexBuffers.boneWeights[base + i * 2 + 1];
    out.push_back(bw);
  }
}

} // namespace virtualgeometry
