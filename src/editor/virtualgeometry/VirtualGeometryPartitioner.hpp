#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"
#include <vector>

namespace virtualgeometry
{

class VirtualGeometryPartitioner
{
public:
  static std::vector<std::vector<int32_t>> partition(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap);
  static void lockBoundaryVertices(std::vector<uint8_t> &locks, const std::vector<std::vector<int32_t>> &groups, const std::vector<VirtualGeometryCluster> &clusters, const std::vector<uint32_t> &remap);
  // Returns clusters whose indices are global (into the vertices array passed in).
  // The vertices field is left empty; it is populated later during linearization.
  static std::vector<VirtualGeometryCluster> clusterize(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);

private:
  static std::vector<std::vector<int32_t>> partitionMetis(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap);
  static std::vector<VirtualGeometryCluster> clusterizeMetis(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
};

} // namespace virtualgeometry
