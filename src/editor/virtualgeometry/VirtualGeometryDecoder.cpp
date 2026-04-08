#include "VirtualGeometryDecoder.hpp"

namespace virtualgeometry
{

void VirtualGeometryDecoder::decodeMeshlet(
    const VirtualGeometryEncodedData &encoded,
    uint32_t page_index,
    uint32_t meshlet_index,
    std::vector<Vertex> &out_vertices,
    std::vector<uint32_t> &out_indices)
{
  const VirtualGeometryPage &page = encoded.pages[page_index];

  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<uint8_t> indices8;

  VirtualGeometryCompressor::decodePositions(page, meshlet_index, positions, encoded.quantizationConfig);

  VirtualGeometryCompressor::decodeNormals(page, meshlet_index, normals);
  VirtualGeometryCompressor::decodeUVs(page, meshlet_index, uvs);
  VirtualGeometryCompressor::decodeIndices(page, meshlet_index, indices8);

  const uint32_t vertex_count = static_cast<uint32_t>(positions.size() / 3);

  out_vertices.resize(vertex_count);

  for (uint32_t i = 0; i < vertex_count; ++i)
  {
    Vertex &v = out_vertices[i];

    v.pos[0] = positions[i * 3 + 0];
    v.pos[1] = positions[i * 3 + 1];
    v.pos[2] = positions[i * 3 + 2];

    v.norm[0] = normals[i * 3 + 0];
    v.norm[1] = normals[i * 3 + 1];
    v.norm[2] = normals[i * 3 + 2];

    v.uv[0] = uvs[i * 2 + 0];
    v.uv[1] = uvs[i * 2 + 1];
  }

  out_indices.clear();
  out_indices.reserve(indices8.size());

  for (uint8_t i : indices8)
    out_indices.push_back(static_cast<uint32_t>(i));
}

} // namespace virtualgeometry
