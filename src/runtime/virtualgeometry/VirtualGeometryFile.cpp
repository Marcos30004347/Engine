// VirtualGeometryFile.cpp
#include "VirtualGeometryFile.hpp"
#include "algorithm/BinaryIO.hpp"
#include "lz4.h"
#include <algorithm>
#include <cassert>
#include <climits>
#include <cstring>
#include <iostream>

#define USE_MINIZ
#ifdef USE_MINIZ
#include "miniz/miniz.h"
#endif

namespace virtualgeometry
{

static inline uint32_t f32_to_u32(float v)
{
  uint32_t u;
  std::memcpy(&u, &v, 4);
  return u;
}
static inline float u32_to_f32(uint32_t v)
{
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

static inline uint32_t quantizedSpanToBits(uint32_t span)
{
  if (span == 0u)
    return 0u;

  uint32_t bits = 0u;
  while (span != 0u)
  {
    ++bits;
    span >>= 1u;
  }
  return bits;
}

static void writeStringRaw(FILE *file, const std::string &value)
{
  write_u32(file, static_cast<uint32_t>(value.size()));
  if (!value.empty())
    std::fwrite(value.data(), 1u, value.size(), file);
}

static bool readStringRaw(FILE *file, std::string &value)
{
  const uint32_t length = read_u32(file);
  value.resize(length);
  return length == 0u || std::fread(value.data(), 1u, length, file) == length;
}

// ============================================================================
// PageBuffer::encode
//
// Page binary layout:
//   Header  : PAGE_HEADER_WORDS words
//     [0] num_meshlets
//     [1] position_data_size   (words)
//     [2] normal_data_size     (words)
//     [3] uv_data_size         (words)
//     [4] index_data_size      (words, padded to uint32)
//     [5] bone_weight_data_size(words)
//     [6] dependency_count
//     [7] group_count
//     [8 .. 8 + MAX_GROUPS_PER_PAGE - 1] local group -> first meshlet index
//   Meshlet descriptor table : num_meshlets * MESHLET_DESC_WORDS words
//   Position block
//   Normal block
//   UV block
//   Index block
//   Bone-weight block
//   Dependency list
// ============================================================================

PageBuffer PageBuffer::encode(const VirtualGeometryPage &page)
{
  PageBuffer buffer;
  auto &out = buffer.data;

  const uint32_t num_meshlets = static_cast<uint32_t>(page.meshlets.size());
  const uint32_t position_data_size = static_cast<uint32_t>(page.vertexBuffers.positions.size());
  const uint32_t normal_data_size = static_cast<uint32_t>(page.vertexBuffers.normals.size());
  const uint32_t uv_data_size = static_cast<uint32_t>(page.vertexBuffers.uvs.size());
  const uint32_t index_data_size = static_cast<uint32_t>((page.vertexBuffers.indices.size() + 3) / 4);
  const uint32_t bone_weight_data_size = static_cast<uint32_t>(page.vertexBuffers.boneWeights.size());

  // ── Header (PAGE_HEADER_WORDS words) ────────────────────────────────────
  out.push_back(num_meshlets);
  out.push_back(position_data_size);
  out.push_back(normal_data_size);
  out.push_back(uv_data_size);
  out.push_back(index_data_size);
  out.push_back(bone_weight_data_size);
  out.push_back(static_cast<uint32_t>(page.dependencies.size()));
  out.push_back(page.groupCount);
  for (uint32_t groupIndex = 0u; groupIndex < MAX_GROUPS_PER_PAGE; ++groupIndex)
  {
    const uint32_t groupMeshletOffset = groupIndex < page.groupMeshletOffsets.size() ? page.groupMeshletOffsets[groupIndex] : UINT32_MAX;
    out.push_back(groupMeshletOffset);
  }

  // ── Meshlet descriptor table (MESHLET_DESC_WORDS words per entry) ────────
  for (const auto &m : page.meshlets)
  {
    const uint32_t bits_per_vert = quantizedSpanToBits(m.quantized_position_span_x) + quantizedSpanToBits(m.quantized_position_span_y) + quantizedSpanToBits(m.quantized_position_span_z);
    const uint32_t pos_words = (bits_per_vert * m.vertex_count + 31) / 32;
    const uint32_t idx_words = (m.triangle_count * 3u + 3u) / 4u;
    const uint32_t pos_word_off = m.start_vertex_position_bit / 32;
    const uint32_t norm_off = m.start_vertex_attribute_id;
    const uint32_t uv_off = m.start_vertex_attribute_id * 2;
    const uint32_t idx_word_off = m.start_index_id / 4;

    // [0..16] — existing geometry fields
    out.push_back(pos_word_off);
    out.push_back(pos_words);
    out.push_back(norm_off);
    out.push_back(m.vertex_count);
    out.push_back(uv_off);
    out.push_back(m.vertex_count * 2u);
    out.push_back(idx_word_off);
    out.push_back(idx_words);
    out.push_back(m.vertex_count);
    out.push_back(m.triangle_count);
    out.push_back(m.quantized_position_span_x);
    out.push_back(m.quantized_position_span_y);
    out.push_back(m.quantized_position_span_z);
    out.push_back(m.vertex_position_quantization_factor);
    out.push_back(floatToUint32(static_cast<float>(m.min_vertex_position_channel_x)));
    out.push_back(floatToUint32(static_cast<float>(m.min_vertex_position_channel_y)));
    out.push_back(floatToUint32(static_cast<float>(m.min_vertex_position_channel_z)));

    // [17..21] — self LOD bounds
    out.push_back(floatToUint32(m.self.center[0]));
    out.push_back(floatToUint32(m.self.center[1]));
    out.push_back(floatToUint32(m.self.center[2]));
    out.push_back(floatToUint32(m.self.radius));
    out.push_back(floatToUint32(m.self.error));

    // [22..26] — parent LOD bounds
    out.push_back(floatToUint32(m.parent.center[0]));
    out.push_back(floatToUint32(m.parent.center[1]));
    out.push_back(floatToUint32(m.parent.center[2]));
    out.push_back(floatToUint32(m.parent.radius));
    out.push_back(floatToUint32(m.parent.error));

    // [27..30] — cluster cone
    out.push_back(floatToUint32(m.cone.axis[0]));
    out.push_back(floatToUint32(m.cone.axis[1]));
    out.push_back(floatToUint32(m.cone.axis[2]));
    out.push_back(floatToUint32(m.cone.cutoff));

    // [31..33] — bone weights + page-local grouping
    out.push_back(m.boneWeightOffset);
    out.push_back(static_cast<uint32_t>(m.boneWeightsPerVertex));
    out.push_back((static_cast<uint32_t>(m.pageLocalGroupIndex) & 0x3Fu) | ((static_cast<uint32_t>(m.clusterIndexInGroup) & 0x7u) << 6u));
  }

  // ── Vertex data blocks ───────────────────────────────────────────────────
  out.insert(out.end(), page.vertexBuffers.positions.begin(), page.vertexBuffers.positions.end());
  out.insert(out.end(), page.vertexBuffers.normals.begin(), page.vertexBuffers.normals.end());

  for (float uv : page.vertexBuffers.uvs)
    out.push_back(floatToUint32(uv));

  // Indices packed 4-per-word
  for (size_t i = 0; i < page.vertexBuffers.indices.size(); i += 4)
  {
    uint32_t packed = 0;
    for (size_t j = 0; j < 4 && (i + j) < page.vertexBuffers.indices.size(); ++j)
      packed |= static_cast<uint32_t>(page.vertexBuffers.indices[i + j]) << (j * 8);
    out.push_back(packed);
  }

  // Bone weights — already stored as uint32 words
  out.insert(out.end(), page.vertexBuffers.boneWeights.begin(), page.vertexBuffers.boneWeights.end());

  // Dependency list
  for (uint32_t dep : page.dependencies)
    out.push_back(dep);

  return buffer;
}

// ============================================================================
// PageBuffer::readHeader
// ============================================================================

PageBuffer::PageHeader PageBuffer::readHeader(const PageBuffer &buffer)
{
  assert(buffer.data.size() >= PAGE_HEADER_WORDS);
  PageHeader h;
  h.num_meshlets = buffer.data[0];
  h.position_data_size = buffer.data[1];
  h.normal_data_size = buffer.data[2];
  h.uv_data_size = buffer.data[3];
  h.index_data_size = buffer.data[4];
  h.bone_weight_data_size = buffer.data[5];
  h.dependency_count = buffer.data[6];
  h.group_count = buffer.data[7];
  return h;
}

// ============================================================================
// PageBuffer::decode
// ============================================================================

void PageBuffer::decode(const PageBuffer &buffer, VirtualGeometryPage &page, const QuantizationConfig & /*config*/)
{
  const auto &data = buffer.data;
  size_t off = 0;

  assert(data.size() >= PAGE_HEADER_WORDS);
  const uint32_t num_meshlets = data[off++];
  const uint32_t position_data_size = data[off++];
  const uint32_t normal_data_size = data[off++];
  const uint32_t uv_data_size = data[off++];
  const uint32_t index_data_size = data[off++];
  const uint32_t bone_weight_data_size = data[off++];
  const uint32_t dependency_count = data[off++];
  page.groupCount = data[off++];
  page.groupMeshletOffsets.resize(page.groupCount, UINT32_MAX);
  for (uint32_t groupIndex = 0u; groupIndex < MAX_GROUPS_PER_PAGE; ++groupIndex)
  {
    const uint32_t groupOffset = data[off++];
    if (groupIndex < page.groupMeshletOffsets.size())
      page.groupMeshletOffsets[groupIndex] = groupOffset;
  }

  page.meshlets.resize(num_meshlets);

  // Intermediate to hold raw word values before we reconstruct Meshlet fields.
  struct MI
  {
    uint32_t pos_off, pos_words;
    uint32_t norm_off, norm_count;
    uint32_t uv_off, uv_count;
    uint32_t idx_off, idx_words;
    uint32_t vert_count, tri_count;
    uint32_t span_x, span_y, span_z, qf;
    uint32_t min_x, min_y, min_z;
    // LOD bounds
    uint32_t self_cx, self_cy, self_cz, self_r, self_err;
    uint32_t par_cx, par_cy, par_cz, par_r, par_err;
    uint32_t cone_axis_x, cone_axis_y, cone_axis_z, cone_cutoff;
    // Bone weights
    uint32_t bw_offset, bw_per_vert, group_cluster;
  };

  std::vector<MI> mis(num_meshlets);
  for (uint32_t i = 0; i < num_meshlets; ++i)
  {
    assert(off + MESHLET_DESC_WORDS <= data.size());
    MI &mi = mis[i];
    mi.pos_off = data[off++];
    mi.pos_words = data[off++];
    mi.norm_off = data[off++];
    mi.norm_count = data[off++];
    mi.uv_off = data[off++];
    mi.uv_count = data[off++];
    mi.idx_off = data[off++];
    mi.idx_words = data[off++];
    mi.vert_count = data[off++];
    mi.tri_count = data[off++];
    mi.span_x = data[off++];
    mi.span_y = data[off++];
    mi.span_z = data[off++];
    mi.qf = data[off++];
    mi.min_x = data[off++];
    mi.min_y = data[off++];
    mi.min_z = data[off++];
    mi.self_cx = data[off++];
    mi.self_cy = data[off++];
    mi.self_cz = data[off++];
    mi.self_r = data[off++];
    mi.self_err = data[off++];
    mi.par_cx = data[off++];
    mi.par_cy = data[off++];
    mi.par_cz = data[off++];
    mi.par_r = data[off++];
    mi.par_err = data[off++];
    mi.cone_axis_x = data[off++];
    mi.cone_axis_y = data[off++];
    mi.cone_axis_z = data[off++];
    mi.cone_cutoff = data[off++];
    mi.bw_offset = data[off++];
    mi.bw_per_vert = data[off++];
    mi.group_cluster = data[off++];
  }

  // ── Vertex data blocks ───────────────────────────────────────────────────

  page.vertexBuffers.positions.resize(position_data_size);
  for (uint32_t i = 0; i < position_data_size; ++i)
    page.vertexBuffers.positions[i] = data[off++];

  page.vertexBuffers.normals.resize(normal_data_size);
  for (uint32_t i = 0; i < normal_data_size; ++i)
    page.vertexBuffers.normals[i] = data[off++];

  page.vertexBuffers.uvs.resize(uv_data_size);
  for (uint32_t i = 0; i < uv_data_size; ++i)
    page.vertexBuffers.uvs[i] = uint32ToFloat(data[off++]);

  const uint32_t padded_bytes = index_data_size * 4u;
  page.vertexBuffers.indices.resize(padded_bytes);
  for (uint32_t i = 0; i < index_data_size; ++i)
  {
    uint32_t packed = data[off++];
    page.vertexBuffers.indices[i * 4 + 0] = (packed >> 0) & 0xFF;
    page.vertexBuffers.indices[i * 4 + 1] = (packed >> 8) & 0xFF;
    page.vertexBuffers.indices[i * 4 + 2] = (packed >> 16) & 0xFF;
    page.vertexBuffers.indices[i * 4 + 3] = (packed >> 24) & 0xFF;
  }

  page.vertexBuffers.boneWeights.resize(bone_weight_data_size);
  for (uint32_t i = 0; i < bone_weight_data_size; ++i)
    page.vertexBuffers.boneWeights[i] = data[off++];

  // ── Reconstruct Meshlet structs ──────────────────────────────────────────

  for (size_t i = 0; i < num_meshlets; ++i)
  {
    const MI &mi = mis[i];
    Meshlet &m = page.meshlets[i];

    m.vertex_count = static_cast<uint8_t>(mi.vert_count);
    m.triangle_count = static_cast<uint8_t>(mi.tri_count);

    m.quantized_position_span_x = mi.span_x;
    m.quantized_position_span_y = mi.span_y;
    m.quantized_position_span_z = mi.span_z;
    m.vertex_position_quantization_factor = static_cast<uint8_t>(mi.qf);

    // The min values were stored as reinterpreted float of the int32 → round-trip.
    m.min_vertex_position_channel_x = static_cast<int32_t>(uint32ToFloat(mi.min_x));
    m.min_vertex_position_channel_y = static_cast<int32_t>(uint32ToFloat(mi.min_y));
    m.min_vertex_position_channel_z = static_cast<int32_t>(uint32ToFloat(mi.min_z));

    m.start_vertex_position_bit = mi.pos_off * 32;
    m.start_vertex_attribute_id = mi.norm_off;
    m.start_index_id = mi.idx_off * 4u;

    // LOD bounds
    m.self.center[0] = uint32ToFloat(mi.self_cx);
    m.self.center[1] = uint32ToFloat(mi.self_cy);
    m.self.center[2] = uint32ToFloat(mi.self_cz);
    m.self.radius = uint32ToFloat(mi.self_r);
    m.self.error = uint32ToFloat(mi.self_err);

    m.parent.center[0] = uint32ToFloat(mi.par_cx);
    m.parent.center[1] = uint32ToFloat(mi.par_cy);
    m.parent.center[2] = uint32ToFloat(mi.par_cz);
    m.parent.radius = uint32ToFloat(mi.par_r);
    m.parent.error = uint32ToFloat(mi.par_err);

    m.cone.axis[0] = uint32ToFloat(mi.cone_axis_x);
    m.cone.axis[1] = uint32ToFloat(mi.cone_axis_y);
    m.cone.axis[2] = uint32ToFloat(mi.cone_axis_z);
    m.cone.cutoff = uint32ToFloat(mi.cone_cutoff);

    // Bone weights
    m.boneWeightOffset = mi.bw_offset;
    m.boneWeightsPerVertex = static_cast<uint8_t>(mi.bw_per_vert);
    m.pageLocalGroupIndex = static_cast<uint8_t>(mi.group_cluster & 0x3Fu);
    m.clusterIndexInGroup = static_cast<uint8_t>((mi.group_cluster >> 6u) & 0x7u);
  }

  // Dependency list
  page.dependencies.resize(dependency_count);
  for (uint32_t i = 0; i < dependency_count; ++i)
  {
    assert(off < data.size());
    page.dependencies[i] = data[off++];
  }

  // installUpdates / uninstallUpdates are loaded from file metadata, not page bytes
}

// ============================================================================
// VirtualGeometryStreamedPage
// ============================================================================

VirtualGeometryStreamedPage::VirtualGeometryStreamedPage() = default;

VirtualGeometryStreamedPage::VirtualGeometryStreamedPage(const void *data, uint32_t size_in_bytes, uint32_t page_size) : data_(static_cast<const uint32_t *>(data)), data_size_bytes_(size_in_bytes), page_size_(page_size)
{
  computeOffsets();
}

void VirtualGeometryStreamedPage::computeOffsets()
{
  if (!data_ || data_size_bytes_ < PAGE_HEADER_WORDS * sizeof(uint32_t))
    return;

  meshlet_count_ = data_[0];
  const uint32_t pos_sz = data_[1];
  const uint32_t norm_sz = data_[2];
  const uint32_t uv_sz = data_[3];
  const uint32_t idx_sz = data_[4];
  const uint32_t bw_sz = data_[5];
  dependency_count_ = data_[6];
  group_count_ = data_[7];
  for (uint32_t groupIndex = 0u; groupIndex < MAX_GROUPS_PER_PAGE; ++groupIndex)
    group_meshlet_offsets_[groupIndex] = data_[8 + groupIndex];

  uint32_t off = PAGE_HEADER_WORDS;
  off += meshlet_count_ * MESHLET_DESC_WORDS;

  position_data_offset_ = off;
  off += pos_sz;
  normal_data_offset_ = off;
  off += norm_sz;
  uv_data_offset_ = off;
  off += uv_sz;
  index_data_offset_ = off;
  off += idx_sz;
  bone_weight_data_offset_ = off;
  off += bw_sz;
  dependencies_offset_ = off;
}

uint32_t VirtualGeometryStreamedPage::getMeshletCount() const
{
  return meshlet_count_;
}
uint32_t VirtualGeometryStreamedPage::getGroupCount() const
{
  return group_count_;
}
uint32_t VirtualGeometryStreamedPage::getGroupMeshletOffset(uint32_t group_index) const
{
  if (group_index >= group_count_ || group_index >= MAX_GROUPS_PER_PAGE)
    return UINT32_MAX;
  return group_meshlet_offsets_[group_index];
}
uint32_t VirtualGeometryStreamedPage::getClustersStartOffset() const
{
  return PAGE_HEADER_WORDS * sizeof(uint32_t);
}
uint32_t VirtualGeometryStreamedPage::getClustersDataSizeInBytes() const
{
  return meshlet_count_ * MESHLET_DESC_WORDS * sizeof(uint32_t);
}

VirtualGeometryStreamedPage::MeshletDescriptor VirtualGeometryStreamedPage::getMeshletDescriptor(uint32_t midx) const
{
  MeshletDescriptor desc{};
  if (!data_ || midx >= meshlet_count_)
    return desc;

  const uint32_t off = PAGE_HEADER_WORDS + midx * MESHLET_DESC_WORDS;
  desc.position_offset = data_[off + 0];
  desc.position_count = data_[off + 1];
  desc.normal_offset = data_[off + 2];
  desc.normal_count = data_[off + 3];
  desc.uv_offset = data_[off + 4];
  desc.uv_count = data_[off + 5];
  desc.index_offset = data_[off + 6];
  desc.index_count = data_[off + 7];
  desc.vertex_count = data_[off + 8];
  desc.triangle_count = data_[off + 9];
  desc.quantized_position_span_x = data_[off + 10];
  desc.quantized_position_span_y = data_[off + 11];
  desc.quantized_position_span_z = data_[off + 12];
  desc.quantization_factor = static_cast<uint8_t>(data_[off + 13]);
  desc.min_position_x = u32_to_f32(data_[off + 14]);
  desc.min_position_y = u32_to_f32(data_[off + 15]);
  desc.min_position_z = u32_to_f32(data_[off + 16]);

  desc.self.center[0] = u32_to_f32(data_[off + 17]);
  desc.self.center[1] = u32_to_f32(data_[off + 18]);
  desc.self.center[2] = u32_to_f32(data_[off + 19]);
  desc.self.radius = u32_to_f32(data_[off + 20]);
  desc.self.error = u32_to_f32(data_[off + 21]);

  desc.parent.center[0] = u32_to_f32(data_[off + 22]);
  desc.parent.center[1] = u32_to_f32(data_[off + 23]);
  desc.parent.center[2] = u32_to_f32(data_[off + 24]);
  desc.parent.radius = u32_to_f32(data_[off + 25]);
  desc.parent.error = u32_to_f32(data_[off + 26]);

  desc.cone.axis[0] = u32_to_f32(data_[off + 27]);
  desc.cone.axis[1] = u32_to_f32(data_[off + 28]);
  desc.cone.axis[2] = u32_to_f32(data_[off + 29]);
  desc.cone.cutoff = u32_to_f32(data_[off + 30]);

  desc.bone_weight_offset = data_[off + 31];
  desc.bone_weights_per_vertex = static_cast<uint8_t>(data_[off + 32]);
  desc.page_local_group_index = static_cast<uint8_t>(data_[off + 33] & 0x3Fu);
  desc.cluster_index_in_group = static_cast<uint8_t>((data_[off + 33] >> 6u) & 0x7u);

  return desc;
}

void VirtualGeometryStreamedPage::decodePositions(uint32_t midx, std::vector<float> &out, const QuantizationConfig &config) const
{
  out.clear();
  if (!data_ || midx >= meshlet_count_)
    return;

  const MeshletDescriptor d = getMeshletDescriptor(midx);
  const float dq = static_cast<float>(1 << d.quantization_factor) * config.unit_scale;
  const uint32_t bitsX = quantizedSpanToBits(d.quantized_position_span_x);
  const uint32_t bitsY = quantizedSpanToBits(d.quantized_position_span_y);
  const uint32_t bitsZ = quantizedSpanToBits(d.quantized_position_span_z);

  const uint32_t *base = data_ + position_data_offset_ + d.position_offset;
  uint32_t bit_off = 0;
  out.reserve(d.vertex_count * 3);

  for (uint32_t v = 0; v < d.vertex_count; ++v)
  {
    uint32_t qx = 0, qy = 0, qz = 0;
    for (uint32_t b = 0; b < bitsX; ++b)
    {
      uint32_t wi = bit_off / 32, bi = bit_off % 32;
      qx |= ((base[wi] >> bi) & 1) << b;
      ++bit_off;
    }
    for (uint32_t b = 0; b < bitsY; ++b)
    {
      uint32_t wi = bit_off / 32, bi = bit_off % 32;
      qy |= ((base[wi] >> bi) & 1) << b;
      ++bit_off;
    }
    for (uint32_t b = 0; b < bitsZ; ++b)
    {
      uint32_t wi = bit_off / 32, bi = bit_off % 32;
      qz |= ((base[wi] >> bi) & 1) << b;
      ++bit_off;
    }

    out.push_back((static_cast<float>(qx) + d.min_position_x) / dq);
    out.push_back((static_cast<float>(qy) + d.min_position_y) / dq);
    out.push_back((static_cast<float>(qz) + d.min_position_z) / dq);
  }
}

void VirtualGeometryStreamedPage::decodeNormals(uint32_t midx, std::vector<float> &out) const
{
  out.clear();
  if (!data_ || midx >= meshlet_count_)
    return;

  const MeshletDescriptor d = getMeshletDescriptor(midx);
  const uint32_t *base = data_ + normal_data_offset_ + d.normal_offset;
  out.reserve(d.vertex_count * 3);

  for (uint32_t v = 0; v < d.vertex_count; ++v)
  {
    const uint32_t packed = base[v];
    // Octahedral snorm16x2
    float ox = static_cast<float>(static_cast<int16_t>(packed & 0xFFFF)) / 32767.f;
    float oy = static_cast<float>(static_cast<int16_t>((packed >> 16) & 0xFFFF)) / 32767.f;

    float n[3];
    n[0] = ox;
    n[1] = oy;
    n[2] = 1.f - std::abs(ox) - std::abs(oy);
    if (n[2] < 0.f)
    {
      float x = n[0], y = n[1];
      n[0] = (1.f - std::abs(y)) * (x >= 0.f ? 1.f : -1.f);
      n[1] = (1.f - std::abs(x)) * (y >= 0.f ? 1.f : -1.f);
    }
    float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (len > 0.f)
    {
      n[0] /= len;
      n[1] /= len;
      n[2] /= len;
    }
    out.push_back(n[0]);
    out.push_back(n[1]);
    out.push_back(n[2]);
  }
}

void VirtualGeometryStreamedPage::decodeUVs(uint32_t midx, std::vector<float> &out) const
{
  out.clear();
  if (!data_ || midx >= meshlet_count_)
    return;

  const MeshletDescriptor d = getMeshletDescriptor(midx);
  const uint32_t *base = data_ + uv_data_offset_ + d.uv_offset;
  out.reserve(d.vertex_count * 2);
  for (uint32_t v = 0; v < d.vertex_count * 2; ++v)
    out.push_back(u32_to_f32(base[v]));
}

void VirtualGeometryStreamedPage::decodeIndices(uint32_t midx, std::vector<uint8_t> &out) const
{
  out.clear();
  if (!data_ || midx >= meshlet_count_)
    return;

  const MeshletDescriptor d = getMeshletDescriptor(midx);
  const uint32_t total = d.triangle_count * 3;
  out.reserve(total);
  const uint32_t *base = data_ + index_data_offset_ + d.index_offset;
  for (uint32_t i = 0; i < d.index_count && out.size() < total; ++i)
  {
    const uint32_t packed = base[i];
    for (uint32_t j = 0; j < 4 && out.size() < total; ++j)
      out.push_back(static_cast<uint8_t>((packed >> (j * 8)) & 0xFF));
  }
}

void VirtualGeometryStreamedPage::decodeBoneWeights(uint32_t midx, std::vector<BoneWeight> &out) const
{
  out.clear();
  if (!data_ || midx >= meshlet_count_)
    return;

  const MeshletDescriptor d = getMeshletDescriptor(midx);
  if (d.bone_weights_per_vertex == 0)
    return;

  const uint32_t total = d.vertex_count * d.bone_weights_per_vertex;
  out.reserve(total);

  // Each influence: [weight_u32, boneIndex_u32]
  const uint32_t *base = data_ + bone_weight_data_offset_ + d.bone_weight_offset;
  for (uint32_t i = 0; i < total; ++i)
  {
    BoneWeight bw;
    bw.weight = u32_to_f32(base[i * 2 + 0]);
    bw.boneIndex = base[i * 2 + 1];
    out.push_back(bw);
  }
}

// ============================================================================
// VirtualGeometryFile
// ============================================================================

VirtualGeometryFile::VirtualGeometryFile() = default;

VirtualGeometryFile::VirtualGeometryFile(const std::string &path, bool write_mode) : path_(path), write_mode_(write_mode)
{
  file_ = fopen(path.c_str(), write_mode ? "wb" : "rb");
  if (file_ && !write_mode)
    loadMetadataAndTables();
}

VirtualGeometryFile::~VirtualGeometryFile()
{
  close();
}

VirtualGeometryFile::VirtualGeometryFile(VirtualGeometryFile &&o) noexcept
    : path_(std::move(o.path_)), file_(o.file_), write_mode_(o.write_mode_), metadata_(std::move(o.metadata_)), hierarchy_(std::move(o.hierarchy_)), shapes_(std::move(o.shapes_)), mesh_parts_(std::move(o.mesh_parts_)),
      skeleton_(std::move(o.skeleton_)), page_table_(std::move(o.page_table_)), page_dependencies_(std::move(o.page_dependencies_)), page_install_updates_(std::move(o.page_install_updates_)),
      page_uninstall_updates_(std::move(o.page_uninstall_updates_))
{
  o.file_ = nullptr;
}

VirtualGeometryFile &VirtualGeometryFile::operator=(VirtualGeometryFile &&o) noexcept
{
  if (this != &o)
  {
    close();
    path_ = std::move(o.path_);
    file_ = o.file_;
    write_mode_ = o.write_mode_;
    metadata_ = std::move(o.metadata_);
    hierarchy_ = std::move(o.hierarchy_);
    shapes_ = std::move(o.shapes_);
    mesh_parts_ = std::move(o.mesh_parts_);
    skeleton_ = std::move(o.skeleton_);
    page_table_ = std::move(o.page_table_);
    page_dependencies_ = std::move(o.page_dependencies_);
    page_install_updates_ = std::move(o.page_install_updates_);
    page_uninstall_updates_ = std::move(o.page_uninstall_updates_);
    o.file_ = nullptr;
  }
  return *this;
}

void VirtualGeometryFile::close()
{
  if (file_)
  {
    fclose(file_);
    file_ = nullptr;
  }
}

bool VirtualGeometryFile::isOpen() const
{
  return file_ != nullptr;
}
const std::string &VirtualGeometryFile::getPath() const
{
  return path_;
}

const VirtualGeometryMetadata &VirtualGeometryFile::getMetadata() const
{
  return metadata_;
}
const std::vector<VirtualGeometryHierarchy> &VirtualGeometryFile::getHierarchy() const
{
  return hierarchy_;
}
const std::vector<VirtualGeometryShapeInfo> &VirtualGeometryFile::getShapes() const
{
  return shapes_;
}
const std::vector<std::string> &VirtualGeometryFile::getMaterialFiles() const
{
  return material_files_;
}
const std::vector<MeshPartInfo> &VirtualGeometryFile::getMeshParts() const
{
  return mesh_parts_;
}
const rendering::animation::Skeleton &VirtualGeometryFile::getSkeleton() const
{
  return skeleton_;
}
const std::vector<VirtualGeometryPageDescriptor> &VirtualGeometryFile::getPageTable() const
{
  return page_table_;
}
const std::vector<std::vector<uint32_t>> &VirtualGeometryFile::getPageDependencies() const
{
  return page_dependencies_;
}
const std::vector<PageUpdateList> &VirtualGeometryFile::getPageInstallUpdates() const
{
  return page_install_updates_;
}
const std::vector<PageUpdateList> &VirtualGeometryFile::getPageUninstallUpdates() const
{
  return page_uninstall_updates_;
}
uint32_t VirtualGeometryFile::getMaxPageSize() const
{
  return metadata_.max_page_size;
}

size_t VirtualGeometryFile::getInstanceCount() const
{
  return std::max<size_t>(1u, shapes_.size());
}

VirtualGeometryFile::InstanceHierarchyData VirtualGeometryFile::getHierarchyForInstance(uint32_t instance_index) const
{
  InstanceHierarchyData result;

  if (shapes_.empty())
  {
    result.hierarchy = hierarchy_;
    return result;
  }

  if (instance_index >= shapes_.size())
    return result;

  const VirtualGeometryShapeInfo &shape = shapes_[instance_index];
  result.hierarchyNodeOffsetInFile = shape.root_node_index;
  if (shape.hierarchy_node_count == 0u || shape.root_node_index >= hierarchy_.size())
    return result;

  const size_t hierarchyBegin = static_cast<size_t>(shape.root_node_index);
  const size_t hierarchyEnd = std::min(hierarchy_.size(), hierarchyBegin + static_cast<size_t>(shape.hierarchy_node_count));
  result.hierarchy.assign(hierarchy_.begin() + static_cast<std::ptrdiff_t>(hierarchyBegin), hierarchy_.begin() + static_cast<std::ptrdiff_t>(hierarchyEnd));

  for (VirtualGeometryHierarchy &node : result.hierarchy)
  {
    if ((node.flags & HIERARCHY_LEAF_FLAG) == 0u && node.child_start != UINT32_MAX)
      node.child_start -= shape.root_node_index;
  }

  return result;
}

uint32_t VirtualGeometryFile::getRootPageForInstance(uint32_t instance_index) const
{
  if (shapes_.empty())
    return metadata_.root_page_index;

  if (instance_index >= shapes_.size())
    return UINT32_MAX;

  return shapes_[instance_index].root_page_index;
}

// ── I/O primitives ───────────────────────────────────────────────────────────

void VirtualGeometryFile::writeU32(FILE *f, uint32_t v)
{
  write_u32(f, v);
}
void VirtualGeometryFile::writeU64(FILE *f, uint64_t v)
{
  write_u64(f, v);
}
void VirtualGeometryFile::writeF32(FILE *f, float v)
{
  write_f32(f, v);
}
bool VirtualGeometryFile::readU32(FILE *f, uint32_t &v)
{
  v = read_u32(f);
  return true;
}
bool VirtualGeometryFile::readU64(FILE *f, uint64_t &v)
{
  v = read_u64(f);
  return true;
}
bool VirtualGeometryFile::readF32(FILE *f, float &v)
{
  v = read_f32(f);
  return true;
}

// ── loadMetadataAndTables ────────────────────────────────────────────────────

bool VirtualGeometryFile::loadMetadataAndTables()
{
  if (!file_)
    return false;
  fseek(file_, 0, SEEK_SET);

  if (!readU32(file_, metadata_.magic) || !readU32(file_, metadata_.version))
    return false;
  if (metadata_.magic != VMESH_MAGIC)
    return false;

  if (metadata_.version != VMESH_VERSION)
    return false;

  if (!readU32(file_, metadata_.endian_tag) || !readU32(file_, metadata_.total_meshlet_count) || !readU32(file_, metadata_.hierarchy_node_count) || !readU32(file_, metadata_.page_count) ||
      !readU32(file_, metadata_.root_page_index) || !readU32(file_, metadata_.shape_count) || !readU32(file_, metadata_.material_count) || !readU32(file_, metadata_.skeleton_count) ||
      !readU32(file_, metadata_.max_page_size) || !readU64(file_, metadata_.hierarchy_offset) || !readU64(file_, metadata_.hierarchy_size) || !readU64(file_, metadata_.shape_table_offset) ||
      !readU64(file_, metadata_.shape_table_size) || !readU64(file_, metadata_.material_table_offset) || !readU64(file_, metadata_.material_table_size) || !readU64(file_, metadata_.skeleton_table_offset) ||
      !readU64(file_, metadata_.skeleton_table_size))
    return false;

  if (!readU64(file_, metadata_.page_table_offset) || !readU64(file_, metadata_.page_table_size) || !readU64(file_, metadata_.page_dependency_offset) || !readU64(file_, metadata_.page_dependency_size) ||
      !readU64(file_, metadata_.page_install_update_offset) || !readU64(file_, metadata_.page_install_update_size) || !readU64(file_, metadata_.page_uninstall_update_offset) ||
      !readU64(file_, metadata_.page_uninstall_update_size) || !readU64(file_, metadata_.page_data_offset) || !readU32(file_, metadata_.quantization_factor) || !readU32(file_, metadata_.unit_scale_bits) ||
      !readU32(file_, metadata_.flags))
    return false;

  // Hierarchy
  fseek(file_, metadata_.hierarchy_offset, SEEK_SET);
  hierarchy_.resize(metadata_.hierarchy_node_count);
  for (uint32_t i = 0; i < metadata_.hierarchy_node_count; ++i)
  {
    struct H
    {
      uint32_t d[HIERARCHY_WORDS];
    } h;
    if (fread(&h, sizeof(uint32_t), HIERARCHY_WORDS, file_) != HIERARCHY_WORDS)
      return false;
    auto &n = hierarchy_[i];
    n.max_x = u32_to_f32(h.d[0]);
    n.max_y = u32_to_f32(h.d[1]);
    n.max_z = u32_to_f32(h.d[2]);
    n.min_x = u32_to_f32(h.d[3]);
    n.min_y = u32_to_f32(h.d[4]);
    n.min_z = u32_to_f32(h.d[5]);
    n.max_center_x = u32_to_f32(h.d[6]);
    n.max_center_y = u32_to_f32(h.d[7]);
    n.max_center_z = u32_to_f32(h.d[8]);
    n.max_radius = u32_to_f32(h.d[9]);
    n.min_lod_error = u32_to_f32(h.d[10]);
    n.max_parent_lod_error = u32_to_f32(h.d[11]);
    n.child_start = h.d[12];
    n.child_count = h.d[13];
    n.pageIndex = h.d[14];
    n.meshPartIndex = h.d[15];
    n.flags = h.d[16];
  }

  shapes_.clear();
  fseek(file_, metadata_.shape_table_offset, SEEK_SET);
  shapes_.resize(metadata_.shape_count);
  for (uint32_t i = 0; i < metadata_.shape_count; ++i)
  {
    auto &shape = shapes_[i];
    if (!readU32(file_, shape.root_node_index) || !readU32(file_, shape.root_page_index) || !readU32(file_, shape.hierarchy_node_count) || !readU32(file_, shape.materialIndex))
      return false;
  }

  material_files_.clear();
  if (metadata_.material_table_size > 0u)
  {
    fseek(file_, metadata_.material_table_offset, SEEK_SET);
    material_files_.resize(metadata_.material_count);
    for (uint32_t materialIndex = 0u; materialIndex < metadata_.material_count; ++materialIndex)
      if (!readStringRaw(file_, material_files_[materialIndex]))
        return false;
  }

  mesh_parts_.clear();
  skeleton_.clear();
  if (metadata_.skeleton_table_size > 0u)
  {
    fseek(file_, metadata_.skeleton_table_offset, SEEK_SET);

    uint32_t meshPartCount = 0u;
    uint32_t boneCount = 0u;
    if (!readU32(file_, meshPartCount) || !readU32(file_, boneCount))
      return false;

    mesh_parts_.resize(meshPartCount);
    std::vector<uint32_t> meshPartToBoneIndex(meshPartCount, UINT32_MAX);
    for (uint32_t meshPartIndex = 0u; meshPartIndex < meshPartCount; ++meshPartIndex)
    {
      uint32_t dominantBoneIndex = UINT32_MAX;
      if (!readU32(file_, dominantBoneIndex))
        return false;
      mesh_parts_[meshPartIndex].dominantBoneIndex = dominantBoneIndex;
      meshPartToBoneIndex[meshPartIndex] = dominantBoneIndex;
    }

    rendering::animation::Skeleton loadedSkeleton;
    math::Mat4f defaultTransform = math::Mat4f::identity();
    for (uint32_t element = 0u; element < 16u; ++element)
      if (!readF32(file_, defaultTransform.data[element]))
        return false;
    loadedSkeleton.setDefaultTransform(defaultTransform);

    for (uint32_t boneIndex = 0u; boneIndex < boneCount; ++boneIndex)
    {
      rendering::animation::Skeleton::Bone bone;
      uint32_t parentIndexRaw = UINT32_MAX;
      if (!readU32(file_, parentIndexRaw))
        return false;
      bone.parentIndex = parentIndexRaw == UINT32_MAX ? -1 : static_cast<int32_t>(parentIndexRaw);
      if (!readStringRaw(file_, bone.name))
        return false;
      for (uint32_t element = 0u; element < 16u; ++element)
        if (!readF32(file_, bone.defaultLocalTransform.data[element]))
          return false;
      for (uint32_t element = 0u; element < 16u; ++element)
        if (!readF32(file_, bone.inverseBindMatrix.data[element]))
          return false;
      loadedSkeleton.addBone(bone);
    }

    loadedSkeleton.setMeshPartToBoneIndex(std::move(meshPartToBoneIndex));
    skeleton_ = std::move(loadedSkeleton);
  }

  // Page table
  fseek(file_, metadata_.page_table_offset, SEEK_SET);
  page_table_.resize(metadata_.page_count);
  for (uint32_t i = 0; i < metadata_.page_count; ++i)
  {
    auto &d = page_table_[i];
    if (!readU64(file_, d.file_offset) || !readU32(file_, d.compressed_size) || !readU32(file_, d.uncompressed_size) || !readU32(file_, d.hierarchy_offset) || !readU32(file_, d.hierarchy_count) ||
        !readU32(file_, d.meshlet_count) || !readU32(file_, d.max_hierarchy_depth))
      return false;
  }

  // Page dependency lists
  fseek(file_, metadata_.page_dependency_offset, SEEK_SET);
  page_dependencies_.resize(metadata_.page_count);
  for (uint32_t i = 0; i < metadata_.page_count; ++i)
  {
    uint32_t cnt;
    if (!readU32(file_, cnt))
      return false;
    page_dependencies_[i].resize(cnt);
    for (uint32_t j = 0; j < cnt; ++j)
      if (!readU32(file_, page_dependencies_[i][j]))
        return false;
  }

  // Page install-update lists
  fseek(file_, metadata_.page_install_update_offset, SEEK_SET);
  page_install_updates_.resize(metadata_.page_count);
  for (uint32_t i = 0; i < metadata_.page_count; ++i)
    if (!deserializePageUpdateList(file_, page_install_updates_[i]))
      return false;

  // Page uninstall-update lists
  fseek(file_, metadata_.page_uninstall_update_offset, SEEK_SET);
  page_uninstall_updates_.resize(metadata_.page_count);
  for (uint32_t i = 0; i < metadata_.page_count; ++i)
    if (!deserializePageUpdateList(file_, page_uninstall_updates_[i]))
      return false;

  return true;
}

// ── streamPage (decoded, slow path) ─────────────────────────────────────────

bool VirtualGeometryFile::streamPage(uint32_t page_index, VirtualGeometryPage &page) const
{
  if (!file_ || write_mode_ || page_index >= page_table_.size())
    return false;

  const auto &desc = page_table_[page_index];
  fseek(file_, desc.file_offset, SEEK_SET);

  uint32_t compression_type, uncompressed_size;
  if (!readU32(file_, compression_type) || !readU32(file_, uncompressed_size))
    return false;

  std::vector<uint8_t> compressed(desc.compressed_size);
  if (fread(compressed.data(), 1, desc.compressed_size, file_) != desc.compressed_size)
    return false;

  std::vector<uint8_t> decompressed;
#ifdef USE_MINIZ
  if (compression_type == MESHLET_MINIZ)
  {
    decompressed.resize(uncompressed_size);
    mz_ulong dl = uncompressed_size;
    if (mz_uncompress(decompressed.data(), &dl, compressed.data(), compressed.size()) != MZ_OK || dl != uncompressed_size)
      return false;
  }
  else
#endif
      if (compression_type == MESHLET_LZ4)
  {
    decompressed.resize(uncompressed_size);
    const int decodedSize = LZ4_decompress_safe(reinterpret_cast<const char *>(compressed.data()), reinterpret_cast<char *>(decompressed.data()), static_cast<int>(compressed.size()), static_cast<int>(uncompressed_size));
    if (decodedSize < 0 || static_cast<uint32_t>(decodedSize) != uncompressed_size)
      return false;
  }
  else
  {
    decompressed = compressed;
  }

  PageBuffer buf;
  buf.data.resize(uncompressed_size / sizeof(uint32_t));
  std::memcpy(buf.data.data(), decompressed.data(), uncompressed_size);

  QuantizationConfig cfg;
  std::memcpy(&cfg.unit_scale, &metadata_.unit_scale_bits, sizeof(float));
  cfg.quantization_factor = static_cast<uint8_t>(metadata_.quantization_factor);
  PageBuffer::decode(buf, page, cfg);

  if (page_index < page_install_updates_.size())
    page.installUpdates = page_install_updates_[page_index];
  if (page_index < page_uninstall_updates_.size())
    page.uninstallUpdates = page_uninstall_updates_[page_index];

  return true;
}

// ── streamPageRaw ────────────────────────────────────────────────────────────

bool VirtualGeometryFile::streamPageRaw(uint32_t page_index, void *buffer, uint32_t buffer_size_bytes, VirtualGeometryStreamedPage &out_page) const
{
  if (!file_ || write_mode_ || page_index >= page_table_.size())
    return false;

  const auto &desc = page_table_[page_index];
  fseek(file_, desc.file_offset, SEEK_SET);

  uint32_t compression_type, uncompressed_size;
  if (!readU32(file_, compression_type) || !readU32(file_, uncompressed_size))
    return false;

  assert(buffer_size_bytes >= uncompressed_size && "streamPageRaw: buffer too small");
  if (buffer_size_bytes < uncompressed_size)
    return false;

  std::vector<uint8_t> compressed(desc.compressed_size);
  if (fread(compressed.data(), 1, desc.compressed_size, file_) != desc.compressed_size)
    return false;

#ifdef USE_MINIZ
  if (compression_type == MESHLET_MINIZ)
  {
    mz_ulong dl = uncompressed_size;
    if (mz_uncompress(static_cast<unsigned char *>(buffer), &dl, compressed.data(), compressed.size()) != MZ_OK || dl != uncompressed_size)
      return false;
  }
  else
#endif
      if (compression_type == MESHLET_LZ4)
  {
    const int decodedSize = LZ4_decompress_safe(reinterpret_cast<const char *>(compressed.data()), static_cast<char *>(buffer), static_cast<int>(compressed.size()), static_cast<int>(uncompressed_size));
    if (decodedSize < 0 || static_cast<uint32_t>(decodedSize) != uncompressed_size)
      return false;
  }
  else
  {
    std::memcpy(buffer, compressed.data(), uncompressed_size);
  }

  out_page = VirtualGeometryStreamedPage(buffer, uncompressed_size, metadata_.max_page_size == 0u ? uncompressed_size : metadata_.max_page_size);
  return true;
}

// ── readAll ──────────────────────────────────────────────────────────────────

bool VirtualGeometryFile::readAll(VirtualGeometryEncodedData &data) const
{
  if (!file_ || write_mode_)
    return false;
  std::memcpy(&data.quantizationConfig.unit_scale, &metadata_.unit_scale_bits, sizeof(float));
  data.quantizationConfig.quantization_factor = static_cast<uint8_t>(metadata_.quantization_factor);
  data.hierarchy = hierarchy_;
  data.shapes = shapes_;
  data.materialFiles = material_files_;
  data.meshParts = mesh_parts_;
  data.skeleton = skeleton_;
  data.pages.resize(metadata_.page_count);
  for (uint32_t i = 0; i < metadata_.page_count; ++i)
    if (!streamPage(i, data.pages[i]))
      return false;
  data.rootPageIndex = metadata_.root_page_index;
  return true;
}

// ── PageUpdateList deserialization ───────────────────────────────────────────
bool VirtualGeometryFile::deserializePageUpdateList(FILE *f, PageUpdateList &list)
{
  uint32_t hCount;
  if (!readU32(f, hCount))
    return false;
  list.hierarchyUpdates.resize(hCount);
  for (auto &u : list.hierarchyUpdates)
  {
    uint32_t masksPacked = 0u;
    if (!readU32(f, u.hierarchyNodeIndex))
      return false;
    if (!readU32(f, masksPacked))
      return false;
    u.streamingLeafsBitset = static_cast<uint8_t>(masksPacked & 0xFFu);
    u.enabledClustersBitset = static_cast<uint8_t>((masksPacked >> 8u) & 0xFFu);
  }
  return true;
}

} // namespace virtualgeometry
