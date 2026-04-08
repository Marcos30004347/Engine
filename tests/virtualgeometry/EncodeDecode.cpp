#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#include "./utils/File.hpp"
#include "editor/virtualgeometry/VirtualGeometryCompressor.hpp"
#include "virtualgeometry/VirtualGeometryData.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "editor/virtualgeometry/VirtualGeometryHierarchyRewrites.hpp"

#include "../../thirdparty/tiny_obj_loader/tiny_obj_loader.h"

using namespace virtualgeometry;

// static void printHierarchyNode(const VirtualGeometryHierarchy &n, size_t index, const char *label)
// {
//   std::cout << label << "[" << index << "] "
//             << "AABB=[(" << n.min_x << ", " << n.min_y << ", " << n.min_z << ") -> (" << n.max_x << ", " << n.max_y << ", " << n.max_z << ")] "
//             << "Center=(" << n.center_x << ", " << n.center_y << ", " << n.center_z << ") "
//             << "R=" << n.radius << " "
//             << "child_start=" << n.child_start << " "
//             << "child_count=" << n.child_count << " "
//             << "min_lod_error=" << n.min_lod_error << " "
//             << "max_parent_lod_error=" << n.max_parent_lod_error << " "
//             << "flags=0x" << std::hex << n.flags << std::dec << "\n";
// }

static void printHierarchyPair(const VirtualGeometryHierarchy &a, const VirtualGeometryHierarchy &b, size_t index)
{
  // std::cout << "--------------------------------------------------\n";
  // printHierarchyNode(a, index, "BUILDER ");
  // printHierarchyNode(b, index, "ENCODED ");
}

inline bool floatEqual(float a, float b, float eps)
{
  return std::fabs(a - b) <= eps;
}

inline float vec3Dot(const float a[3], const float b[3])
{
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline float vec3Length(const float v[3])
{
  return std::sqrt(vec3Dot(v, v));
}

// ============================================================================
// Hierarchy validation (Builder vs Encoded)
// ============================================================================

void testHierarchyAgainstBuilder(const std::vector<VirtualGeometryHierarchy> &builder, const std::vector<VirtualGeometryHierarchy> &encoded)
{
  assert(builder.size() == encoded.size());

  for (size_t i = 0; i < builder.size(); ++i)
  {

    const auto &a = builder[i];
    const auto &b = encoded[i];

    assert(floatEqual(a.min_x, b.min_x, 1e-4f));
    assert(floatEqual(a.min_y, b.min_y, 1e-4f));
    assert(floatEqual(a.min_z, b.min_z, 1e-4f));

    assert(floatEqual(a.max_x, b.max_x, 1e-4f));
    assert(floatEqual(a.max_y, b.max_y, 1e-4f));
    assert(floatEqual(a.max_z, b.max_z, 1e-4f));

    // assert(floatEqual(a.min_center_x, b.min_center_x, 1e-4f));
    // assert(floatEqual(a.min_center_y, b.min_center_y, 1e-4f));
    // assert(floatEqual(a.min_center_z, b.min_center_z, 1e-4f));
    // assert(floatEqual(a.min_radius, b.min_radius, 1e-4f));
    assert(floatEqual(a.max_center_x, b.max_center_x, 1e-4f));
    assert(floatEqual(a.max_center_y, b.max_center_y, 1e-4f));
    assert(floatEqual(a.max_center_z, b.max_center_z, 1e-4f));
    assert(floatEqual(a.max_radius, b.max_radius, 1e-4f));

    assert(a.child_start == b.child_start);
    assert(a.child_count == b.child_count);
    assert(a.flags == b.flags);
  }
}

// ============================================================================
// Meshlet validation (Builder cluster vs Decoded meshlet)
// ============================================================================

void testMeshletAgainstCluster(
    const VirtualGeometryCluster &cluster,
    const VirtualGeometryPage &page,
    uint32_t meshlet_index,
    const QuantizationConfig &config,
    float position_epsilon)
{
  std::vector<float> positions;
  std::vector<float> normals;
  std::vector<float> uvs;
  std::vector<uint8_t> indices;

  VirtualGeometryCompressor::decodePositions(page, meshlet_index, positions, config);
  VirtualGeometryCompressor::decodeNormals(page, meshlet_index, normals);
  VirtualGeometryCompressor::decodeUVs(page, meshlet_index, uvs);
  VirtualGeometryCompressor::decodeIndices(page, meshlet_index, indices);

  const Meshlet &meshlet = page.meshlets[meshlet_index];

  // --- Counts ---
  assert(meshlet.vertex_count == cluster.vertices.size());
  assert(meshlet.triangle_count * 3 == cluster.indices.size());

  // --- Positions ---
  for (size_t i = 0; i < cluster.vertices.size(); ++i)
  {
    const Vertex &v = cluster.vertices[i];

    assert(floatEqual(positions[i * 3 + 0], v.pos[0], position_epsilon));
    assert(floatEqual(positions[i * 3 + 1], v.pos[1], position_epsilon));
    assert(floatEqual(positions[i * 3 + 2], v.pos[2], position_epsilon));
  }

  // --- Normals (length + direction) ---
  for (size_t i = 0; i < cluster.vertices.size(); ++i)
  {
    float decoded[3] = {normals[i * 3 + 0], normals[i * 3 + 1], normals[i * 3 + 2]};

    float len = vec3Length(decoded);
    assert(floatEqual(len, 1.0f, 1e-3f));

    const Vertex &v = cluster.vertices[i];
    float dot = decoded[0] * v.norm[0] + decoded[1] * v.norm[1] + decoded[2] * v.norm[2];

    if (!floatEqual(v.norm[0] + v.norm[1] + v.norm[2], 0.0f, __FLT_EPSILON__))
    {
      // Allow small angular error from octahedral encoding
      assert(dot > 0.999f);
    }
  }

  // --- UVs ---
  for (size_t i = 0; i < cluster.vertices.size(); ++i)
  {
    const Vertex &v = cluster.vertices[i];
    assert(floatEqual(uvs[i * 2 + 0], v.uv[0], 1e-6f));
    assert(floatEqual(uvs[i * 2 + 1], v.uv[1], 1e-6f));
  }

  for (size_t i = 0; i < cluster.indices.size(); ++i)
  {
    assert(indices[i] == (uint8_t)(cluster.indices[i]));
  }
}

void assertHierarchyEqual(const std::vector<VirtualGeometryHierarchy> &a, const std::vector<VirtualGeometryHierarchy> &b)
{
  assert(a.size() == b.size());

  for (size_t i = 0; i < a.size(); ++i)
  {
    printHierarchyPair(a[i], b[i], i);

    const auto &x = a[i];
    const auto &y = b[i];

    const float eps = 1e-6f;

    assert(floatEqual(x.min_lod_error, y.min_lod_error, eps));
    assert(floatEqual(x.max_parent_lod_error, y.max_parent_lod_error, eps));
    assert(x.flags == y.flags);
    assert(x.child_start != UINT32_MAX);
    assert(x.child_count == y.child_count);
  }
}


int main()
{
  std::vector<std::string> mesh_paths = {
    "assets/meshes/obj/suzanne.obj",
    "assets/meshes/obj/teapot.obj",
    "assets/meshes/obj/stanford-bunny.obj",
    "assets/meshes/obj/armadillo.obj",
  };

  QuantizationConfig config;
  config.quantization_factor = 4;
  config.unit_scale = 100.0f;

  const float position_epsilon = (1.0f / (1u << config.quantization_factor)) / config.unit_scale;

  std::cout << "Virtual mesh encode/decode validation\n";
  std::cout << "Position epsilon: " << position_epsilon << "\n";

  for (const auto &relative_path : mesh_paths)
  {
    std::string full_path = utils::getExecutableDirectory() + "/" + relative_path;

    std::cout << "\nTesting: " << relative_path << "\n";

    std::vector<Vertex> vertices;
    Shape shape;

    VirtualGeometryEncoder::loadOBJ(full_path, true, vertices, shape);

    VirtualGeometryBuildData build = VirtualGeometryBuilder::build(VirtualGeometryBuilder::buildLOD0Clusters(vertices, shape));
    VirtualGeometryEncodedData encoded = VirtualGeometryEncoder::encode(vertices, shape, config);

    // testHierarchyAgainstBuilder(build.lodLevelHierarchy, encoded.hierarchy);

    for (size_t page_idx = 0; page_idx < build.pages.size(); ++page_idx)
    {
      const VirtualGeometryBuildPage &build_page = build.pages[page_idx];
      const VirtualGeometryPage &encoded_page = encoded.pages[page_idx];

      for (uint32_t i = 0; i < build_page.clusterCount; ++i)
      {
        uint32_t cluster_idx = build_page.clusterOffset + i;
        testMeshletAgainstCluster(build.clusters[cluster_idx], encoded_page, i, config, position_epsilon);
      }
    }

    std::cout << "  ✓ Passed\n";
  }

  std::cout << "\nAll virtual geometry tests passed ✓\n";
  return 0;
}
