#pragma once

#include "virtualgeometry/VirtualGeometryData.hpp"
#include <vector>

namespace virtualgeometry
{

class VirtualGeometryPartitioner
{
public:
  // `vertices` supplies positions for the meshopt partitioner; remap welds
  // vertex indices by position for both partitioners.
  static std::vector<std::vector<int32_t>> partition(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap,
                                                     const std::vector<Vertex> &vertices, VirtualGeometryClusterizer clusterizer = VirtualGeometryClusterizer::Metis);
  static void lockBoundaryVertices(std::vector<uint8_t> &locks, const std::vector<std::vector<int32_t>> &groups, const std::vector<VirtualGeometryCluster> &clusters, const std::vector<uint32_t> &remap);
  // Returns clusters whose indices are global (into the vertices array passed in).
  // The vertices field is left empty; it is populated later during linearization.
  static std::vector<VirtualGeometryCluster> clusterize(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices,
                                                        VirtualGeometryClusterizer clusterizer = VirtualGeometryClusterizer::Metis,
                                                        uint32_t maxVertices = 0u);

private:
  static std::vector<std::vector<int32_t>> partitionMeshopt(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap,
                                                            const std::vector<Vertex> &vertices);
  static std::vector<std::vector<int32_t>> partitionMetis(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap);
  static std::vector<VirtualGeometryCluster> clusterizeMeshopt(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices, uint32_t maxVertices);
  static std::vector<VirtualGeometryCluster> clusterizeMetis(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices);
};

} // namespace virtualgeometry
