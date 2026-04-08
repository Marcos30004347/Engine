#pragma once

#include <string>
#include <vector>

#include "virtualgeometry/VirtualGeometryData.hpp"

namespace virtualgeometry
{

struct Shape
{
  std::vector<uint32_t> indices;
};

struct MeshPart
{
  Shape shape;
  uint32_t dominantBoneIndex = UINT32_MAX;
};

struct ShapeBuildInput
{
  std::vector<MeshPart> meshParts;
  uint32_t materialIndex = 0u;
  const rendering::animation::Skeleton *skeleton = nullptr;
};

class VirtualGeometryBuilder
{
public:
  // Clusterize a raw mesh into LOD0 clusters.
  // Each returned cluster has local indices (indexing into cluster.vertices).
  // self bounds are pre-computed; parent.error is set to max (unsimplified).
  static std::vector<VirtualGeometryCluster> buildLOD0Clusters(
      std::vector<Vertex> &vertices,
      const Shape &shape,
      const rendering::animation::Skeleton *skeleton = nullptr);

  // Build the full LOD hierarchy from a set of clusters.
  // Input clusters must have local indices and populated vertices fields,
  // as produced by buildLOD0Clusters.
  static VirtualGeometryBuildData build(std::vector<VirtualGeometryCluster> clusters, const VirtualGeometryBuildSettings &settings = {});

  // Build a multi-shape virtual geometry asset. Each shape gets an independent
  // LOD chain and hierarchy root before page packing merges the disconnected
  // DAGs into one shared file layout.
  static VirtualGeometryBuildData build(
      std::vector<Vertex> &vertices,
      const std::vector<Shape> &shapes,
      const VirtualGeometryBuildSettings &settings = {});

  static VirtualGeometryBuildData build(
      std::vector<Vertex> &vertices,
      const std::vector<ShapeBuildInput> &shapes,
      const VirtualGeometryBuildSettings &settings = {});

  static VirtualGeometryBuildData buildFromGLTFFile(
      const std::string &path,
      const VirtualGeometryBuildSettings &settings = {});
};

} // namespace virtualgeometry

namespace std
{
template <> struct hash<virtualgeometry::Vertex>
{
  size_t operator()(const virtualgeometry::Vertex &v) const noexcept
  {
    size_t h1 = std::hash<float>{}(v.pos[0]);
    size_t h2 = std::hash<float>{}(v.pos[1]);
    size_t h3 = std::hash<float>{}(v.pos[2]);
    size_t h4 = std::hash<float>{}(v.norm[0]);
    size_t h5 = std::hash<float>{}(v.norm[1]);
    size_t h6 = std::hash<float>{}(v.norm[2]);
    size_t h7 = std::hash<float>{}(v.uv[0]);
    size_t h8 = std::hash<float>{}(v.uv[1]);
    size_t h9 = 0u;

    for (const auto &boneWeight : v.boneWeights)
    {
      const size_t weightHash = std::hash<float>{}(boneWeight.weight);
      const size_t boneHash = std::hash<uint32_t>{}(boneWeight.boneIndex);
      h9 ^= weightHash + 0x9e3779b9u + (h9 << 6u) + (h9 >> 2u);
      h9 ^= boneHash + 0x9e3779b9u + (h9 << 6u) + (h9 >> 2u);
    }

    return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3) ^ (h5 << 4) ^ (h6 << 5) ^ (h7 << 6) ^ (h8 << 7) ^ (h9 << 8);
  }
};
} // namespace std
