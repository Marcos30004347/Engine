#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"
#include <vector>

namespace virtualgeometry
{
class VirtualGeometrySimplifier
{
public:
  static std::vector<uint32_t> simplify(const std::vector<Vertex> &vertices, std::vector<Vertex> &outVertices, const std::vector<uint32_t> &indices, const std::vector<uint8_t> *locks, uint64_t targetCount, float *outError = nullptr);
  // Sloppy fallback. Locks are always respected; if any locked vertex is present
  // in the working set, this falls back to simplify() to preserve constraints.
  static std::vector<uint32_t> simplifySloppy(const std::vector<Vertex> &vertices, std::vector<Vertex> &outVertices, const std::vector<uint32_t> &indices, const std::vector<uint8_t> *locks, uint64_t targetCount, float *outError = nullptr);
};

} // namespace virtualgeometry
