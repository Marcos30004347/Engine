#include "VirtualGeometryPartitioner.hpp"
#include "meshoptimizer.h"
#include "metis.h"
#include <cassert>
#include <map>

using namespace virtualgeometry;

const uint32_t UseMetis = 1;
const uint32_t GroupSize = 7;
const int SameMeshPartWeightMultiplier = 4;

std::vector<std::vector<int32_t>> VirtualGeometryPartitioner::partitionMetis(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap)
{
  std::vector<std::vector<int32_t>> vertexClusters(remap.size());
  for (uint64_t i = 0; i < pending.size(); ++i)
  {
    const VirtualGeometryCluster &c = clusters[pending[i]];
    for (uint32_t idx : c.indices)
    {
      int32_t v = remap[idx];
      if (vertexClusters[v].empty() || vertexClusters[v].back() != static_cast<int32_t>(i))
        vertexClusters[v].push_back(static_cast<int32_t>(i));
    }
  }

  std::map<std::pair<int32_t, int32_t>, int32_t> adjacency;
  for (uint64_t v = 0; v < vertexClusters.size(); ++v)
    for (uint64_t i = 0; i < vertexClusters[v].size(); ++i)
      for (uint64_t j = i + 1; j < vertexClusters[v].size(); ++j)
      {
        int32_t a = vertexClusters[v][i], b = vertexClusters[v][j];
        adjacency[{std::min(a, b), std::max(a, b)}]++;
      }

  std::vector<std::vector<std::pair<int32_t, int32_t>>> neighbors(pending.size());
  for (const auto &e : adjacency)
  {
    const int32_t left = e.first.first;
    const int32_t right = e.first.second;
    int32_t weight = e.second;

    const uint32_t leftMeshPartIndex = clusters[pending[static_cast<size_t>(left)]].meshPartIndex;
    const uint32_t rightMeshPartIndex = clusters[pending[static_cast<size_t>(right)]].meshPartIndex;
    if (leftMeshPartIndex != UINT32_MAX && leftMeshPartIndex == rightMeshPartIndex)
      weight *= SameMeshPartWeightMultiplier;

    neighbors[left].push_back({right, weight});
    neighbors[right].push_back({left, weight});
  }

  std::vector<int32_t> xadj(pending.size() + 1), adjncy, adjwgt, part(pending.size());
  for (uint64_t i = 0; i < pending.size(); ++i)
  {
    for (const auto &nb : neighbors[i])
    {
      adjncy.push_back(nb.first);
      adjwgt.push_back(nb.second);
    }
    xadj[i + 1] = static_cast<int32_t>(adjncy.size());
  }

  int32_t opts[METIS_NOPTIONS];
  METIS_SetDefaultOptions(opts);
  opts[METIS_OPTION_SEED] = 42;
  opts[METIS_OPTION_UFACTOR] = 200;
  int32_t nvtxs = static_cast<int32_t>(pending.size()), ncon = 1, edgecut = 0;
  int32_t nparts = static_cast<int32_t>((pending.size() + GroupSize - 1) / GroupSize);
  if (nparts > 1)
  {
    int32_t r = METIS_PartGraphRecursive(&nvtxs, &ncon, xadj.data(), adjncy.data(), nullptr, nullptr, adjwgt.data(), &nparts, nullptr, nullptr, opts, &edgecut, part.data());
    assert(r == METIS_OK);
  }

  std::vector<std::vector<int32_t>> result(nparts);
  for (uint64_t i = 0; i < part.size(); ++i)
    result[part[i]].push_back(pending[i]);
  return result;
}

std::vector<std::vector<int32_t>> VirtualGeometryPartitioner::partition(const std::vector<VirtualGeometryCluster> &clusters, const std::vector<int32_t> &pending, const std::vector<uint32_t> &remap)
{
  return partitionMetis(clusters, pending, remap);
}

void VirtualGeometryPartitioner::lockBoundaryVertices(std::vector<uint8_t> &locks, const std::vector<std::vector<int32_t>> &groups, const std::vector<VirtualGeometryCluster> &clusters, const std::vector<uint32_t> &remap)
{
  std::vector<int32_t> groupMap(locks.size(), -1);
  for (uint64_t i = 0; i < groups.size(); ++i)
    for (int32_t ci : groups[i])
      for (uint32_t vi : clusters[ci].indices)
      {
        uint32_t r = remap[vi];
        if (groupMap[r] == -1 || groupMap[r] == static_cast<int32_t>(i))
          groupMap[r] = static_cast<int32_t>(i);
        else
          groupMap[r] = -2;
      }
  for (uint64_t i = 0; i < locks.size(); ++i)
    locks[i] = (groupMap[remap[i]] == -2);
}

std::vector<VirtualGeometryCluster> VirtualGeometryPartitioner::clusterizeMetis(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
  std::vector<uint32_t> shadowIndices(indices.size());
  meshopt_generateShadowIndexBuffer(shadowIndices.data(), indices.data(), indices.size(), &vertices[0].pos[0], vertices.size(), sizeof(float) * 3, sizeof(Vertex));

  size_t triCount = indices.size() / 3;

  // Track which triangle and which corner (0-2) each shadow vertex belongs to,
  // so we can recover the actual vertex for UV/normal lookup.
  struct TriCorner
  {
    int tri, corner;
  };
  std::vector<std::vector<TriCorner>> triPerVert(vertices.size());
  for (size_t i = 0; i < indices.size(); ++i)
    triPerVert[shadowIndices[i]].push_back({(int)(i / 3), (int)(i % 3)});

  // Build edge weights from UV + normal similarity.
  // For each pair of triangles sharing a shadow vertex, compute:
  //   normSim  = dot(nA, nB) remapped to [0,1]
  //   uvSim    = 1 / (1 + squared_uv_distance)   (no sqrt needed)
  //   weight  += normSim * uvSim * kSimScale  (clamped to >= 1)
  const float kSimScale = 100.0f;
  std::map<std::pair<int, int>, int> adjacency;
  for (size_t sv = 0; sv < vertices.size(); ++sv)
  {
    const auto &tcs = triPerVert[sv];
    for (size_t ti = 0; ti < tcs.size(); ++ti)
      for (size_t tj = ti + 1; tj < tcs.size(); ++tj)
      {
        int ia = tcs[ti].tri, ib = tcs[tj].tri;
        const Vertex &va = vertices[indices[ia * 3 + tcs[ti].corner]];
        const Vertex &vb = vertices[indices[ib * 3 + tcs[tj].corner]];

        float nd = va.norm[0] * vb.norm[0] + va.norm[1] * vb.norm[1] + va.norm[2] * vb.norm[2];
        float normSim = (nd + 1.0f) * 0.5f;

        float du = va.uv[0] - vb.uv[0], dv = va.uv[1] - vb.uv[1];
        float uvSim = 1.0f / (1.0f + du * du + dv * dv);

        int w = std::max(1, (int)(normSim * uvSim * kSimScale));
        adjacency[{std::min(ia, ib), std::max(ia, ib)}] += w;
      }
  }

  std::vector<std::vector<std::pair<int, int>>> neighbors(triCount);
  for (const auto &e : adjacency)
  {
    neighbors[e.first.first].push_back({e.first.second, e.second});
    neighbors[e.first.second].push_back({e.first.first, e.second});
  }

  std::vector<int> xadj(triCount + 1), adjncy, adjwgt, part(triCount);
  for (size_t i = 0; i < triCount; ++i)
  {
    for (const auto &nb : neighbors[i])
    {
      adjncy.push_back(nb.first);
      adjwgt.push_back(nb.second);
    }
    xadj[i + 1] = static_cast<int>(adjncy.size());
  }

  int opts[METIS_NOPTIONS];
  METIS_SetDefaultOptions(opts);
  opts[METIS_OPTION_SEED] = 42;
  opts[METIS_OPTION_UFACTOR] = 200;

  int nvtxs = static_cast<int>(triCount), ncon = 1, edgecut = 0;
  int nparts = static_cast<int>((triCount + ClusterSize - MetisSlop - 1) / (ClusterSize - MetisSlop));
  if (nparts > 1)
  {
    int r = METIS_PartGraphRecursive(&nvtxs, &ncon, xadj.data(), adjncy.data(), nullptr, nullptr, adjwgt.data(), &nparts, nullptr, nullptr, opts, &edgecut, part.data());
    assert(r == METIS_OK);
    (void)r;
  }

  std::vector<VirtualGeometryCluster> clusters(nparts);
  for (size_t i = 0; i < triCount; ++i)
  {
    clusters[part[i]].indices.push_back(indices[i * 3 + 0]);
    clusters[part[i]].indices.push_back(indices[i * 3 + 1]);
    clusters[part[i]].indices.push_back(indices[i * 3 + 2]);
  }
  for (int i = 0; i < nparts; ++i)
    clusters[i].parent.error = std::numeric_limits<float>::max();

  std::vector<VirtualGeometryCluster> result;
  for (int i = 0; i < nparts; ++i)
  {
    if (clusters[i].indices.size() > ClusterSize * 3)
    {
      auto splits = clusterizeMetis(vertices, clusters[i].indices);
      assert(splits.size() > 1);
      result.insert(result.end(), splits.begin(), splits.end());
    }
    else
      result.push_back(clusters[i]);
  }
  return result;
}

std::vector<VirtualGeometryCluster> VirtualGeometryPartitioner::clusterize(const std::vector<Vertex> &vertices, const std::vector<uint32_t> &indices)
{
  if (UseMetis)
    return clusterizeMetis(vertices, indices);

  const size_t minTriangles = (ClusterSize / 3) & ~3;
  const float splitFactor = 2.f;

  size_t maxMeshlets = meshopt_buildMeshletsBound(indices.size(), MaxVertices, minTriangles);
  std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
  std::vector<uint32_t> meshletVerts(maxMeshlets * MaxVertices);
  std::vector<unsigned char> meshletTris(maxMeshlets * MaxTriangles * 3);

  size_t meshletCount = meshopt_buildMeshletsFlex(
      meshlets.data(), meshletVerts.data(), meshletTris.data(), indices.data(), indices.size(), &vertices[0].pos[0], vertices.size(), sizeof(Vertex), MaxVertices, minTriangles, MaxTriangles, 0.f, splitFactor);
  meshlets.resize(meshletCount);

  std::vector<VirtualGeometryCluster> clusters(meshletCount);
  for (size_t i = 0; i < meshletCount; ++i)
  {
    const meshopt_Meshlet &m = meshlets[i];
    meshopt_optimizeMeshlet(&meshletVerts[m.vertex_offset], &meshletTris[m.triangle_offset], m.triangle_count, m.vertex_count);
    clusters[i].indices.resize(m.triangle_count * 3);
    for (size_t j = 0; j < m.triangle_count * 3; ++j)
      clusters[i].indices[j] = meshletVerts[m.vertex_offset + meshletTris[m.triangle_offset + j]];
    clusters[i].parent.error = std::numeric_limits<float>::max();
  }
  return clusters;
}
