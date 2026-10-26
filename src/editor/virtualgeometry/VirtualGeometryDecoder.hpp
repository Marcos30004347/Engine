#pragma once

#include <vector>

#include "VirtualGeometryCompressor.hpp"

namespace virtualgeometry
{

class VirtualGeometryDecoder
{
public:
  static void decodeMeshlet(const VirtualGeometryEncodedData &encoded, uint32_t page_index, uint32_t meshlet_index, std::vector<Vertex> &out_vertices, std::vector<uint32_t> &out_indices);
};

} // namespace virtualgeometry
