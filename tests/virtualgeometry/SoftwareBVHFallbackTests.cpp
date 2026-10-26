#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "editor/virtualgeometry/FallbackMeshBuilder.hpp"
#include "editor/virtualgeometry/VirtualGeometryEncoder.hpp"
#include "softwarebvh/CWBVHBuilder.hpp"
#include "softwarebvh/RBVHBuilder.hpp"
#include "softwarebvh/SWRBVHFallbackMeshFile.hpp"
#include "softwarebvh/SWRTFallbackMeshFile.hpp"
#include "virtualgeometry/FallbackMeshFile.hpp"

using namespace virtualgeometry;

namespace
{

namespace fs = std::filesystem;

constexpr float kEpsilon = 1.0e-5f;

bool floatEqual(const float left, const float right, const float epsilon = kEpsilon)
{
  return std::fabs(left - right) <= epsilon;
}

uint32_t nextPowerOfTwo(const uint32_t value)
{
  if (value <= 1u)
    return 1u;

  uint32_t result = 1u;
  while (result < value)
    result <<= 1u;
  return result;
}

fs::path sourceRoot()
{
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

fs::path suzanneAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/suzanne.obj";
}

fs::path stanfordBunnyAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/stanford-bunny.obj";
}

fs::path sponzaAssetPath()
{
  return sourceRoot() / "assets/meshes/obj/sponza.obj";
}

void compareBounds(const float expectedMin[3], const float expectedMax[3], const float actualMin[3], const float actualMax[3])
{
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    assert(floatEqual(expectedMin[axis], actualMin[axis]));
    assert(floatEqual(expectedMax[axis], actualMax[axis]));
  }
}

uint32_t maxRbvhCardExtent(const softwarebvh::RBVHBuildResult &build)
{
  uint32_t result = 0u;
  for (const softwarebvh::RBVHCardPrimitive &card : build.cards)
  {
    result = std::max(result, std::max(card.atlasWidth, card.atlasHeight));
  }
  return result;
}

uint32_t coveredPixelCount(const softwarebvh::RBVHBuildResult &build)
{
  uint32_t result = 0u;
  for (const uint8_t coverage : build.coverageAtlasR8)
  {
    result += coverage != 0u ? 1u : 0u;
  }
  return result;
}

FallbackMeshData makeSingleTriangleMesh()
{
  FallbackMeshData mesh{};
  mesh.vertices.resize(3u);
  mesh.indices = {0u, 1u, 2u};
  mesh.sections = {FallbackMeshSection{.indexOffset = 0u, .indexCount = 3u, .materialIndex = 0u}};
  mesh.materialFiles = {"materials/triangle.vmat"};

  mesh.vertices[0u].position[0] = 0.0f;
  mesh.vertices[0u].position[1] = 0.0f;
  mesh.vertices[0u].position[2] = 0.0f;
  mesh.vertices[1u].position[0] = 1.0f;
  mesh.vertices[1u].position[1] = 0.0f;
  mesh.vertices[1u].position[2] = 0.0f;
  mesh.vertices[2u].position[0] = 0.0f;
  mesh.vertices[2u].position[1] = 1.0f;
  mesh.vertices[2u].position[2] = 0.0f;

  for (FallbackMeshVertex &vertex : mesh.vertices)
  {
    vertex.normal[2] = 1.0f;
  }

  mesh.boundsMin[0] = 0.0f;
  mesh.boundsMin[1] = 0.0f;
  mesh.boundsMin[2] = 0.0f;
  mesh.boundsMax[0] = 1.0f;
  mesh.boundsMax[1] = 1.0f;
  mesh.boundsMax[2] = 0.0f;
  return mesh;
}

FallbackMeshData makeOpposingTrianglePairMesh()
{
  FallbackMeshData mesh = makeSingleTriangleMesh();
  mesh.indices = {0u, 1u, 2u, 0u, 2u, 1u};
  mesh.sections = {FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  return mesh;
}

FallbackMeshData makeBentCornerMesh()
{
  FallbackMeshData mesh{};
  mesh.vertices.resize(6u);
  mesh.indices = {0u, 1u, 2u, 2u, 1u, 3u, 0u, 2u, 4u, 4u, 2u, 5u};
  mesh.sections = {FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/bent_corner.vmat"};

  const float positions[6][3] = {
    {0.0f, 0.0f, 0.0f},
    {1.0f, 0.0f, 0.0f},
    {0.0f, 1.0f, 0.0f},
    {1.0f, 1.0f, 0.0f},
    {0.0f, 0.0f, 1.0f},
    {0.0f, 1.0f, 1.0f},
  };
  for (uint32_t vertexIndex = 0u; vertexIndex < 6u; ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      mesh.vertices[vertexIndex].position[axis] = positions[vertexIndex][axis];
  }

  mesh.boundsMin[0] = 0.0f;
  mesh.boundsMin[1] = 0.0f;
  mesh.boundsMin[2] = 0.0f;
  mesh.boundsMax[0] = 1.0f;
  mesh.boundsMax[1] = 1.0f;
  mesh.boundsMax[2] = 1.0f;
  return mesh;
}

FallbackMeshData makeUnitCubeMesh()
{
  FallbackMeshData mesh{};
  mesh.vertices.resize(8u);
  mesh.indices = {
    0u, 1u, 2u, 2u, 1u, 3u, 4u, 6u, 5u, 6u, 7u, 5u, 0u, 2u, 4u, 4u, 2u, 6u, 1u, 5u, 3u, 3u, 5u, 7u, 0u, 4u, 1u, 1u, 4u, 5u, 2u, 3u, 6u, 6u, 3u, 7u,
  };
  mesh.sections = {FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/cube.vmat"};

  const float positions[8][3] = {
    {-0.5f, -0.5f, -0.5f},
    {+0.5f, -0.5f, -0.5f},
    {-0.5f, +0.5f, -0.5f},
    {+0.5f, +0.5f, -0.5f},
    {-0.5f, -0.5f, +0.5f},
    {+0.5f, -0.5f, +0.5f},
    {-0.5f, +0.5f, +0.5f},
    {+0.5f, +0.5f, +0.5f},
  };
  for (uint32_t vertexIndex = 0u; vertexIndex < 8u; ++vertexIndex)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
      mesh.vertices[vertexIndex].position[axis] = positions[vertexIndex][axis];
    mesh.vertices[vertexIndex].normal[2] = 1.0f;
  }

  mesh.boundsMin[0] = -0.5f;
  mesh.boundsMin[1] = -0.5f;
  mesh.boundsMin[2] = -0.5f;
  mesh.boundsMax[0] = +0.5f;
  mesh.boundsMax[1] = +0.5f;
  mesh.boundsMax[2] = +0.5f;
  return mesh;
}

FallbackMeshData makePlanarGridMesh(const uint32_t gridSize)
{
  assert(gridSize > 0u);

  FallbackMeshData mesh{};
  const uint32_t vertexCountPerAxis = gridSize + 1u;
  mesh.vertices.resize(static_cast<size_t>(vertexCountPerAxis) * static_cast<size_t>(vertexCountPerAxis));
  mesh.indices.reserve(static_cast<size_t>(gridSize) * static_cast<size_t>(gridSize) * 6u);

  for (uint32_t y = 0u; y < vertexCountPerAxis; ++y)
  {
    for (uint32_t x = 0u; x < vertexCountPerAxis; ++x)
    {
      FallbackMeshVertex &vertex = mesh.vertices[static_cast<size_t>(y) * vertexCountPerAxis + x];
      vertex.position[0] = static_cast<float>(x) / static_cast<float>(gridSize);
      vertex.position[1] = static_cast<float>(y) / static_cast<float>(gridSize);
      vertex.position[2] = 0.0f;
      vertex.normal[2] = 1.0f;
      vertex.uv[0] = vertex.position[0];
      vertex.uv[1] = vertex.position[1];
    }
  }

  for (uint32_t y = 0u; y < gridSize; ++y)
  {
    for (uint32_t x = 0u; x < gridSize; ++x)
    {
      const uint32_t v00 = y * vertexCountPerAxis + x;
      const uint32_t v10 = v00 + 1u;
      const uint32_t v01 = v00 + vertexCountPerAxis;
      const uint32_t v11 = v01 + 1u;
      mesh.indices.insert(mesh.indices.end(), {v00, v10, v11, v00, v11, v01});
    }
  }

  mesh.sections = {FallbackMeshSection{.indexOffset = 0u, .indexCount = static_cast<uint32_t>(mesh.indices.size()), .materialIndex = 0u}};
  mesh.materialFiles = {"materials/grid.vmat"};
  mesh.boundsMin[0] = 0.0f;
  mesh.boundsMin[1] = 0.0f;
  mesh.boundsMin[2] = 0.0f;
  mesh.boundsMax[0] = 1.0f;
  mesh.boundsMax[1] = 1.0f;
  mesh.boundsMax[2] = 0.0f;
  return mesh;
}

FallbackMeshData loadObjFallbackMesh(const fs::path &assetPath)
{
  assert(fs::exists(assetPath));

  const fs::path tempRoot = fs::temp_directory_path() / ("SoftwareBVHFallbackTests_" + assetPath.stem().string());
  const fs::path materialsDir = tempRoot / "materials";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  assert(!error);

  const VirtualGeometrySourceAsset asset = VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), materialsDir.string());
  assert(!asset.vertices.empty());
  assert(!asset.shapes.empty());

  const FallbackMeshData mesh = FallbackMeshBuilder::buildOriginal(asset);
  fs::remove_all(tempRoot, error);
  return mesh;
}

VirtualGeometrySourceAsset loadObjSourceAssetForFallbackTest(const fs::path &assetPath, const std::string &label)
{
  assert(fs::exists(assetPath));

  const fs::path tempRoot = fs::temp_directory_path() / ("SoftwareBVHFallbackTests_" + label);
  const fs::path materialsDir = tempRoot / "materials";
  std::error_code error;
  fs::remove_all(tempRoot, error);
  fs::create_directories(materialsDir, error);
  assert(!error);

  VirtualGeometrySourceAsset asset = VirtualGeometryEncoder::loadAssetFromOBJFile(assetPath.string(), materialsDir.string());
  fs::remove_all(tempRoot, error);
  return asset;
}

void computeMeshBounds(const FallbackMeshData &mesh, float outMin[3], float outMax[3])
{
  assert(!mesh.vertices.empty());
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outMin[axis] = mesh.vertices.front().position[axis];
    outMax[axis] = mesh.vertices.front().position[axis];
  }

  for (const FallbackMeshVertex &vertex : mesh.vertices)
  {
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      outMin[axis] = std::min(outMin[axis], vertex.position[axis]);
      outMax[axis] = std::max(outMax[axis], vertex.position[axis]);
    }
  }
}

void computeTriangleBounds(const FallbackMeshData &mesh, const uint32_t primitiveIndex, float outMin[3], float outMax[3])
{
  const uint32_t baseIndex = primitiveIndex * 3u;
  assert(baseIndex + 2u < mesh.indices.size());

  const uint32_t firstVertex = mesh.indices[baseIndex];
  assert(firstVertex < mesh.vertices.size());
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    outMin[axis] = mesh.vertices[firstVertex].position[axis];
    outMax[axis] = mesh.vertices[firstVertex].position[axis];
  }

  for (uint32_t corner = 1u; corner < 3u; ++corner)
  {
    const uint32_t vertexIndex = mesh.indices[baseIndex + corner];
    assert(vertexIndex < mesh.vertices.size());
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      outMin[axis] = std::min(outMin[axis], mesh.vertices[vertexIndex].position[axis]);
      outMax[axis] = std::max(outMax[axis], mesh.vertices[vertexIndex].position[axis]);
    }
  }
}

void compareTriangleTransforms(const softwarebvh::CWBVHBuilder::TriBaryTransform &expected, const softwarebvh::CWBVHBuilder::TriBaryTransform &actual)
{
  for (uint32_t element = 0u; element < 4u; ++element)
  {
    assert(floatEqual(expected.row0[element], actual.row0[element]));
    assert(floatEqual(expected.row1[element], actual.row1[element]));
    assert(floatEqual(expected.row2[element], actual.row2[element]));
  }
}

void compareBuildResults(const softwarebvh::CWBVHBuilder::BuildResult &expected, const softwarebvh::CWBVHBuilder::BuildResult &actual)
{
  assert(expected.primitiveCount == actual.primitiveCount);
  assert(expected.rootNodeIndex == actual.rootNodeIndex);
  assert(expected.primitiveRemap == actual.primitiveRemap);
  compareBounds(expected.localBounds.minPoint, expected.localBounds.maxPoint, actual.localBounds.minPoint, actual.localBounds.maxPoint);
  assert(expected.triangleTransforms.size() == actual.triangleTransforms.size());
  assert(expected.canonicalNodes.size() == actual.canonicalNodes.size());

  for (size_t transformIndex = 0u; transformIndex < expected.triangleTransforms.size(); ++transformIndex)
    compareTriangleTransforms(expected.triangleTransforms[transformIndex], actual.triangleTransforms[transformIndex]);

  for (size_t nodeIndex = 0u; nodeIndex < expected.canonicalNodes.size(); ++nodeIndex)
  {
    const auto &expectedNode = expected.canonicalNodes[nodeIndex];
    const auto &actualNode = actual.canonicalNodes[nodeIndex];
    assert(expectedNode.parent == actualNode.parent);
    assert(expectedNode.left == actualNode.left);
    assert(expectedNode.right == actualNode.right);
    assert(expectedNode.primitiveIndex == actualNode.primitiveIndex);
    assert(expectedNode.subtreePrimitiveCount == actualNode.subtreePrimitiveCount);
    assert(expectedNode.isLeaf == actualNode.isLeaf);
    compareBounds(expectedNode.bounds.minPoint, expectedNode.bounds.maxPoint, actualNode.bounds.minPoint, actualNode.bounds.maxPoint);
  }
}

using Vec3 = std::array<float, 3u>;

Vec3 loadPosition(const FallbackMeshData &mesh, const uint32_t vertexIndex)
{
  assert(vertexIndex < mesh.vertices.size());
  return {
    mesh.vertices[vertexIndex].position[0],
    mesh.vertices[vertexIndex].position[1],
    mesh.vertices[vertexIndex].position[2],
  };
}

Vec3 add(const Vec3 &left, const Vec3 &right)
{
  return {left[0] + right[0], left[1] + right[1], left[2] + right[2]};
}

Vec3 subtract(const Vec3 &left, const Vec3 &right)
{
  return {left[0] - right[0], left[1] - right[1], left[2] - right[2]};
}

Vec3 scale(const Vec3 &value, const float factor)
{
  return {value[0] * factor, value[1] * factor, value[2] * factor};
}

float dot(const Vec3 &left, const Vec3 &right)
{
  return left[0] * right[0] + left[1] * right[1] + left[2] * right[2];
}

Vec3 loadArray3(const float value[3])
{
  return {value[0], value[1], value[2]};
}

float dotRow(const float row[4], const Vec3 &point)
{
  return row[0] * point[0] + row[1] * point[1] + row[2] * point[2] + row[3];
}

float dotRowDirection(const float row[4], const Vec3 &direction)
{
  return row[0] * direction[0] + row[1] * direction[1] + row[2] * direction[2];
}

Vec3 cross(const Vec3 &left, const Vec3 &right)
{
  return {
    left[1] * right[2] - left[2] * right[1],
    left[2] * right[0] - left[0] * right[2],
    left[0] * right[1] - left[1] * right[0],
  };
}

float length(const Vec3 &value)
{
  return std::sqrt(dot(value, value));
}

float distance(const Vec3 &left, const Vec3 &right)
{
  return length(subtract(left, right));
}

Vec3 normalize(const Vec3 &value)
{
  const float valueLength = length(value);
  assert(valueLength > kEpsilon);
  return scale(value, 1.0f / valueLength);
}

Vec3 evaluateTriangleTransform(const softwarebvh::CWBVHBuilder::TriBaryTransform &transform, const Vec3 &origin, const Vec3 &direction)
{
  const float oz = -dotRow(transform.row2, origin);
  const float dz = dotRowDirection(transform.row2, direction);
  const float t = oz / dz;

  const float u = dotRow(transform.row0, origin) + t * dotRowDirection(transform.row0, direction);
  const float v = dotRow(transform.row1, origin) + t * dotRowDirection(transform.row1, direction);
  return {u, v, t};
}

float triangleRowTolerance(const float row[4], const Vec3 &mappedPoint, const float expected)
{
  const float rowScale = std::fabs(row[0] * mappedPoint[0]) + std::fabs(row[1] * mappedPoint[1]) + std::fabs(row[2] * mappedPoint[2]) + std::fabs(row[3]) + std::fabs(expected);
  return 1.0e-4f + rowScale * 1.0e-5f;
}

float triangleRayTolerance(const float row[4], const Vec3 &origin, const Vec3 &direction, const float t, const float expected)
{
  const float rayScale = std::fabs(dotRow(row, origin)) + std::fabs(t * dotRowDirection(row, direction)) + std::fabs(expected);
  return 2.0e-3f + rayScale * 1.0e-5f;
}

void verifyTrianglePointMapping(const softwarebvh::CWBVHBuilder::TriBaryTransform &transform, const Vec3 &point, const float expectedU, const float expectedV)
{
  assert(floatEqual(dotRow(transform.row0, point), expectedU, triangleRowTolerance(transform.row0, point, expectedU)));
  assert(floatEqual(dotRow(transform.row1, point), expectedV, triangleRowTolerance(transform.row1, point, expectedV)));
  assert(floatEqual(dotRow(transform.row2, point), 0.0f, triangleRowTolerance(transform.row2, point, 0.0f)));
}

void verifyWoopTriangleTransforms(const std::string &label, const FallbackMeshData &mesh, const softwarebvh::CWBVHBuilder::BuildResult &build, const bool verifyRayHits)
{
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  assert(build.triangleTransforms.size() == primitiveCount);
  uint32_t verifiedCount = 0u;
  uint32_t skippedDegenerateCount = 0u;

  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    const uint32_t indexBase = primitiveIndex * 3u;
    const Vec3 p0 = loadPosition(mesh, mesh.indices[indexBase + 0u]);
    const Vec3 p1 = loadPosition(mesh, mesh.indices[indexBase + 1u]);
    const Vec3 p2 = loadPosition(mesh, mesh.indices[indexBase + 2u]);
    const auto &transform = build.triangleTransforms[primitiveIndex];
    const Vec3 normalVector = cross(subtract(p1, p0), subtract(p2, p0));
    if (length(normalVector) <= 1.0e-8f)
    {
      skippedDegenerateCount += 1u;
      continue;
    }

    verifyTrianglePointMapping(transform, p0, 0.0f, 0.0f);
    verifyTrianglePointMapping(transform, p1, 1.0f, 0.0f);
    verifyTrianglePointMapping(transform, p2, 0.0f, 1.0f);

    const Vec3 centroid = scale(add(add(p0, p1), p2), 1.0f / 3.0f);
    verifyTrianglePointMapping(transform, centroid, 1.0f / 3.0f, 1.0f / 3.0f);

    if (verifyRayHits)
    {
      const Vec3 normal = normalize(normalVector);
      const float edgeScale = std::max({length(subtract(p1, p0)), length(subtract(p2, p0)), length(subtract(p2, p1)), 1.0f});
      const Vec3 rayOrigin = add(centroid, scale(normal, edgeScale));
      const Vec3 rayDirection = scale(normal, -1.0f);
      const Vec3 hit = evaluateTriangleTransform(transform, rayOrigin, rayDirection);
      const float uTolerance = triangleRayTolerance(transform.row0, rayOrigin, rayDirection, hit[2], 1.0f / 3.0f);
      const float vTolerance = triangleRayTolerance(transform.row1, rayOrigin, rayDirection, hit[2], 1.0f / 3.0f);

      assert(hit[2] > 0.0f);
      if (!floatEqual(hit[0], 1.0f / 3.0f, uTolerance) || !floatEqual(hit[1], 1.0f / 3.0f, vTolerance))
      {
        std::cerr << "Woop ray mismatch in " << label << " primitive " << primitiveIndex << ": hit=(" << hit[0] << ", " << hit[1] << ", " << hit[2] << "), tolerances=(" << uTolerance << ", " << vTolerance << ")\n";
      }
      assert(floatEqual(hit[0], 1.0f / 3.0f, uTolerance));
      assert(floatEqual(hit[1], 1.0f / 3.0f, vTolerance));
      assert(hit[0] >= -uTolerance && hit[1] >= -vTolerance && hit[0] + hit[1] <= 1.0f + uTolerance + vTolerance);
    }
    verifiedCount += 1u;
  }

  std::cout << "  [OK] " << label << ": verified " << verifiedCount << " Woop triangle transforms";
  if (skippedDegenerateCount > 0u)
    std::cout << ", skipped " << skippedDegenerateCount << " degenerate triangles";
  std::cout << "\n";
}

void compareMeshes(const FallbackMeshData &expected, const FallbackMeshData &actual)
{
  assert(expected.lodLevel == actual.lodLevel);
  assert(floatEqual(expected.targetError, actual.targetError));
  assert(floatEqual(expected.relativeError, actual.relativeError));
  assert(expected.vertices.size() == actual.vertices.size());
  assert(expected.indices == actual.indices);
  assert(expected.sections.size() == actual.sections.size());
  assert(expected.materialFiles == actual.materialFiles);
  compareBounds(expected.boundsMin, expected.boundsMax, actual.boundsMin, actual.boundsMax);

  for (size_t sectionIndex = 0u; sectionIndex < expected.sections.size(); ++sectionIndex)
  {
    assert(expected.sections[sectionIndex].indexOffset == actual.sections[sectionIndex].indexOffset);
    assert(expected.sections[sectionIndex].indexCount == actual.sections[sectionIndex].indexCount);
    assert(expected.sections[sectionIndex].materialIndex == actual.sections[sectionIndex].materialIndex);
  }

  for (size_t vertexIndex = 0u; vertexIndex < expected.vertices.size(); ++vertexIndex)
  {
    const FallbackMeshVertex &expectedVertex = expected.vertices[vertexIndex];
    const FallbackMeshVertex &actualVertex = actual.vertices[vertexIndex];
    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      assert(floatEqual(expectedVertex.position[axis], actualVertex.position[axis]));
      assert(floatEqual(expectedVertex.normal[axis], actualVertex.normal[axis]));
    }
    for (uint32_t axis = 0u; axis < 2u; ++axis)
      assert(floatEqual(expectedVertex.uv[axis], actualVertex.uv[axis]));
    for (uint32_t influence = 0u; influence < 4u; ++influence)
    {
      assert(expectedVertex.boneIndices[influence] == actualVertex.boneIndices[influence]);
      assert(floatEqual(expectedVertex.boneWeights[influence], actualVertex.boneWeights[influence]));
      assert(floatEqual(expectedVertex.surfaceBaseColorOpacity[influence], actualVertex.surfaceBaseColorOpacity[influence]));
      assert(floatEqual(expectedVertex.surfaceNormalRoughness[influence], actualVertex.surfaceNormalRoughness[influence]));
      assert(floatEqual(expectedVertex.surfaceEmissiveMetallic[influence], actualVertex.surfaceEmissiveMetallic[influence]));
      assert(expectedVertex.surfaceFlags[influence] == actualVertex.surfaceFlags[influence]);
    }
  }

  assert(expected.triangleSurfaceHints.size() == actual.triangleSurfaceHints.size());
  for (size_t hintIndex = 0u; hintIndex < expected.triangleSurfaceHints.size(); ++hintIndex)
  {
    const FallbackMeshTriangleSurfaceHint &expectedHint = expected.triangleSurfaceHints[hintIndex];
    const FallbackMeshTriangleSurfaceHint &actualHint = actual.triangleSurfaceHints[hintIndex];
    assert(floatEqual(expectedHint.uvCircleCenter[0], actualHint.uvCircleCenter[0]));
    assert(floatEqual(expectedHint.uvCircleCenter[1], actualHint.uvCircleCenter[1]));
    assert(floatEqual(expectedHint.uvCircleRadius, actualHint.uvCircleRadius));
    assert(floatEqual(expectedHint.textureMipLevel, actualHint.textureMipLevel));
    assert(expectedHint.materialIndex == actualHint.materialIndex);
  }
}

void verifyBuildAgainstMesh(const std::string &label, const FallbackMeshData &mesh, const bool verifyRayHits)
{
  const softwarebvh::CWBVHBuilder::BuildResult build = softwarebvh::CWBVHBuilder::build(mesh);
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);

  assert(!mesh.vertices.empty());
  assert(primitiveCount > 0u);
  assert(build.primitiveCount == primitiveCount);
  assert(build.rootNodeIndex != softwarebvh::CWBVHBuilder::INVALID_NODE);
  assert(build.rootNodeIndex < build.canonicalNodes.size());
  assert(build.canonicalNodes.size() == primitiveCount * 2u - 1u);
  verifyWoopTriangleTransforms(label, mesh, build, verifyRayHits);

  float meshMin[3]{};
  float meshMax[3]{};
  computeMeshBounds(mesh, meshMin, meshMax);
  compareBounds(meshMin, meshMax, mesh.boundsMin, mesh.boundsMax);
  compareBounds(meshMin, meshMax, build.localBounds.minPoint, build.localBounds.maxPoint);

  std::vector<uint32_t> primitiveVisitCount(primitiveCount, 0u);

  const auto validateNode = [&](auto &&self, const uint32_t nodeIndex, const uint32_t expectedParent, float outMin[3], float outMax[3]) -> uint32_t
  {
    assert(nodeIndex < build.canonicalNodes.size());
    const auto &node = build.canonicalNodes[nodeIndex];
    assert(node.parent == expectedParent);

    if (node.isLeaf)
    {
      assert(node.left == softwarebvh::CWBVHBuilder::INVALID_NODE);
      assert(node.right == softwarebvh::CWBVHBuilder::INVALID_NODE);
      assert(node.primitiveIndex < primitiveCount);
      primitiveVisitCount[node.primitiveIndex] += 1u;
      computeTriangleBounds(mesh, node.primitiveIndex, outMin, outMax);
      compareBounds(outMin, outMax, node.bounds.minPoint, node.bounds.maxPoint);
      assert(node.subtreePrimitiveCount == 1u);
      return 1u;
    }

    assert(node.left != softwarebvh::CWBVHBuilder::INVALID_NODE);
    assert(node.right != softwarebvh::CWBVHBuilder::INVALID_NODE);

    float leftMin[3]{};
    float leftMax[3]{};
    float rightMin[3]{};
    float rightMax[3]{};
    const uint32_t leftCount = self(self, node.left, nodeIndex, leftMin, leftMax);
    const uint32_t rightCount = self(self, node.right, nodeIndex, rightMin, rightMax);

    for (uint32_t axis = 0u; axis < 3u; ++axis)
    {
      outMin[axis] = std::min(leftMin[axis], rightMin[axis]);
      outMax[axis] = std::max(leftMax[axis], rightMax[axis]);
    }
    compareBounds(outMin, outMax, node.bounds.minPoint, node.bounds.maxPoint);
    assert(node.subtreePrimitiveCount == leftCount + rightCount);
    return leftCount + rightCount;
  };

  float rootMin[3]{};
  float rootMax[3]{};
  const uint32_t visitedPrimitiveCount = validateNode(validateNode, build.rootNodeIndex, softwarebvh::CWBVHBuilder::INVALID_NODE, rootMin, rootMax);
  compareBounds(meshMin, meshMax, rootMin, rootMax);
  assert(visitedPrimitiveCount == primitiveCount);
  assert(build.canonicalNodes[build.rootNodeIndex].subtreePrimitiveCount == primitiveCount);

  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
  {
    assert(primitiveVisitCount[primitiveIndex] == 1u);
  }

  std::cout << "  [OK] " << label << ": " << mesh.vertices.size() << " vertices, " << primitiveCount << " triangles, " << build.canonicalNodes.size() << " BVH2 nodes\n";
}

struct RBVHTraceHit
{
  bool hit = false;
  float t = std::numeric_limits<float>::infinity();
  uint32_t cardIndex = softwarebvh::CWBVHBuilder::INVALID_PRIMITIVE_INDEX;
};

struct RBVHSamplePoint
{
  Vec3 point{};
  Vec3 normal{};
  uint32_t cardIndex = softwarebvh::CWBVHBuilder::INVALID_PRIMITIVE_INDEX;
  uint32_t atlasX = 0u;
  uint32_t atlasY = 0u;
};

virtualgeometry::AABB cardBounds(const softwarebvh::RBVHCardPrimitive &card)
{
  virtualgeometry::AABB bounds{};
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    bounds.minPoint[axis] = card.boundsMin[axis];
    bounds.maxPoint[axis] = card.boundsMax[axis];
  }
  return bounds;
}

bool intersectAABB(const virtualgeometry::AABB &bounds, const Vec3 &rayOrigin, const Vec3 &rayDirection, float &outTMin, float &outTMax)
{
  float tMin = 0.0f;
  float tMax = std::numeric_limits<float>::infinity();
  for (uint32_t axis = 0u; axis < 3u; ++axis)
  {
    if (std::fabs(rayDirection[axis]) <= 1.0e-8f)
    {
      if (rayOrigin[axis] < bounds.minPoint[axis] || rayOrigin[axis] > bounds.maxPoint[axis])
        return false;
      continue;
    }

    float t0 = (bounds.minPoint[axis] - rayOrigin[axis]) / rayDirection[axis];
    float t1 = (bounds.maxPoint[axis] - rayOrigin[axis]) / rayDirection[axis];
    if (t0 > t1)
      std::swap(t0, t1);
    tMin = std::max(tMin, t0);
    tMax = std::min(tMax, t1);
    if (tMin > tMax)
      return false;
  }

  outTMin = tMin;
  outTMax = tMax;
  return true;
}

float decodeCardDepth(const softwarebvh::RBVHBuildResult &build, const softwarebvh::RBVHCardPrimitive &card, const uint32_t atlasX, const uint32_t atlasY)
{
  assert(atlasX < build.atlasSize);
  assert(atlasY < build.atlasSize);
  const uint32_t pixelIndex = atlasY * build.atlasSize + atlasX;
  assert(pixelIndex < build.depthAtlasR16.size());
  const float depth01 = static_cast<float>(build.depthAtlasR16[pixelIndex]) / 65535.0f;
  return card.depthMin + depth01 * (card.depthMax - card.depthMin);
}

Vec3 reconstructCardPoint(const softwarebvh::RBVHBuildResult &build, const uint32_t cardIndex, const uint32_t atlasX, const uint32_t atlasY)
{
  assert(cardIndex < build.cards.size());
  const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
  assert(atlasX >= card.atlasX && atlasX < card.atlasX + card.atlasWidth);
  assert(atlasY >= card.atlasY && atlasY < card.atlasY + card.atlasHeight);

  const float localU = ((static_cast<float>(atlasX - card.atlasX) + 0.5f) / static_cast<float>(card.atlasWidth) * 2.0f - 1.0f) * card.halfExtentU;
  const float localV = ((static_cast<float>(atlasY - card.atlasY) + 0.5f) / static_cast<float>(card.atlasHeight) * 2.0f - 1.0f) * card.halfExtentV;
  const float absoluteDepth = decodeCardDepth(build, card, atlasX, atlasY);
  const float originDepth = dot(loadArray3(card.origin), loadArray3(card.axisN));
  return add(add(add(loadArray3(card.origin), scale(loadArray3(card.axisU), localU)), scale(loadArray3(card.axisV), localV)), scale(loadArray3(card.axisN), absoluteDepth - originDepth));
}

bool sampleCardSignedDistance(const softwarebvh::RBVHBuildResult &build, const softwarebvh::RBVHCardPrimitive &card, const Vec3 &point, float &outDifference)
{
  if (card.atlasWidth == 0u || card.atlasHeight == 0u || build.atlasSize == 0u)
    return false;

  const Vec3 local = subtract(point, loadArray3(card.origin));
  const float u = dot(local, loadArray3(card.axisU));
  const float v = dot(local, loadArray3(card.axisV));
  if (u < -card.halfExtentU || u > card.halfExtentU || v < -card.halfExtentV || v > card.halfExtentV)
    return false;

  auto sampleTexel = [&](const uint32_t localX, const uint32_t localY, float &outDepth)
  {
    if (localX >= card.atlasWidth || localY >= card.atlasHeight)
      return false;
    const uint32_t atlasX = card.atlasX + localX;
    const uint32_t atlasY = card.atlasY + localY;
    const uint32_t pixelIndex = atlasY * build.atlasSize + atlasX;
    if (pixelIndex >= build.coverageAtlasR8.size() || build.coverageAtlasR8[pixelIndex] == 0u)
      return false;
    outDepth = decodeCardDepth(build, card, atlasX, atlasY);
    return true;
  };

  const uint32_t tileMaxX = card.atlasWidth - 1u;
  const uint32_t tileMaxY = card.atlasHeight - 1u;
  const float uvX = u / card.halfExtentU * 0.5f + 0.5f;
  const float uvY = v / card.halfExtentV * 0.5f + 0.5f;
  const uint32_t nearestX = std::min(tileMaxX, static_cast<uint32_t>(std::clamp(std::floor(uvX * static_cast<float>(card.atlasWidth)), 0.0f, static_cast<float>(tileMaxX))));
  const uint32_t nearestY = std::min(tileMaxY, static_cast<uint32_t>(std::clamp(std::floor(uvY * static_cast<float>(card.atlasHeight)), 0.0f, static_cast<float>(tileMaxY))));
  const float texelX = uvX * static_cast<float>(card.atlasWidth) - 0.5f;
  const float texelY = uvY * static_cast<float>(card.atlasHeight) - 0.5f;
  const uint32_t baseX = static_cast<uint32_t>(std::clamp(std::floor(texelX), 0.0f, static_cast<float>(tileMaxX)));
  const uint32_t baseY = static_cast<uint32_t>(std::clamp(std::floor(texelY), 0.0f, static_cast<float>(tileMaxY)));
  const uint32_t nextX = std::min(baseX + 1u, tileMaxX);
  const uint32_t nextY = std::min(baseY + 1u, tileMaxY);
  const float blendX = std::clamp(texelX - static_cast<float>(baseX), 0.0f, 1.0f);
  const float blendY = std::clamp(texelY - static_cast<float>(baseY), 0.0f, 1.0f);

  float d00 = 0.0f;
  float d10 = 0.0f;
  float d01 = 0.0f;
  float d11 = 0.0f;
  float surfaceDepth = 0.0f;
  if (sampleTexel(baseX, baseY, d00) && sampleTexel(nextX, baseY, d10) && sampleTexel(baseX, nextY, d01) && sampleTexel(nextX, nextY, d11))
  {
    const float depthX0 = d00 + (d10 - d00) * blendX;
    const float depthX1 = d01 + (d11 - d01) * blendX;
    surfaceDepth = depthX0 + (depthX1 - depthX0) * blendY;
  }
  else if (!sampleTexel(nearestX, nearestY, surfaceDepth))
  {
    return false;
  }

  outDifference = dot(point, loadArray3(card.axisN)) - surfaceDepth;
  return true;
}

bool traceCardHeightField(const softwarebvh::RBVHBuildResult &build, const uint32_t cardIndex, const Vec3 &rayOrigin, const Vec3 &rayDirection, float &outT)
{
  assert(cardIndex < build.cards.size());
  const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
  float tEnter = 0.0f;
  float tExit = 0.0f;
  if (!intersectAABB(cardBounds(card), rayOrigin, rayDirection, tEnter, tExit))
    return false;

  const Vec3 segmentStart = add(rayOrigin, scale(rayDirection, tEnter));
  const Vec3 segmentEnd = add(rayOrigin, scale(rayDirection, tExit));
  const Vec3 segmentDelta = subtract(segmentEnd, segmentStart);
  auto cardTexelCoord = [&](const Vec3 &point, float &outX, float &outY)
  {
    const Vec3 local = subtract(point, loadArray3(card.origin));
    const float u = dot(local, loadArray3(card.axisU));
    const float v = dot(local, loadArray3(card.axisV));
    outX = (u / card.halfExtentU * 0.5f + 0.5f) * static_cast<float>(card.atlasWidth);
    outY = (v / card.halfExtentV * 0.5f + 0.5f) * static_cast<float>(card.atlasHeight);
  };

  float startTexelX = 0.0f;
  float startTexelY = 0.0f;
  float endTexelX = 0.0f;
  float endTexelY = 0.0f;
  cardTexelCoord(segmentStart, startTexelX, startTexelY);
  cardTexelCoord(segmentEnd, endTexelX, endTexelY);
  const float texelSpan = std::max(std::fabs(endTexelX - startTexelX), std::fabs(endTexelY - startTexelY));
  const uint32_t stepCount = std::min(std::max(static_cast<uint32_t>(std::ceil(std::max(texelSpan, 1.0f))) * 2u, 8u), 512u);
  const float epsilon = std::max(card.samplingError * 1.5f, 1.0e-4f);
  bool previousValid = false;
  float previousS = 0.0f;
  float previousDifference = 0.0f;
  for (uint32_t step = 0u; step <= stepCount; ++step)
  {
    const float s = static_cast<float>(step) / static_cast<float>(stepCount);
    const Vec3 point = add(segmentStart, scale(segmentDelta, s));
    float difference = 0.0f;
    if (!sampleCardSignedDistance(build, card, point, difference))
    {
      previousValid = false;
      continue;
    }

    if (std::fabs(difference) <= epsilon)
    {
      outT = tEnter + (tExit - tEnter) * s;
      return true;
    }

    if (previousValid && previousDifference * difference <= 0.0f)
    {
      float lowerS = previousS;
      float lowerDifference = previousDifference;
      float upperS = s;
      float upperDifference = difference;
      float hitS = (lowerS + upperS) * 0.5f;
      for (uint32_t refineStep = 0u; refineStep < 5u; ++refineStep)
      {
        const float secantDenom = upperDifference - lowerDifference;
        float candidateS = std::fabs(secantDenom) > 1.0e-8f
            ? upperS - upperDifference * (upperS - lowerS) / secantDenom
            : (lowerS + upperS) * 0.5f;
        candidateS = std::clamp(candidateS, lowerS, upperS);
        const Vec3 candidatePoint = add(segmentStart, scale(segmentDelta, candidateS));
        float candidateDifference = 0.0f;
        if (!sampleCardSignedDistance(build, card, candidatePoint, candidateDifference))
          break;

        hitS = candidateS;
        if (std::fabs(candidateDifference) <= epsilon)
        {
          outT = tEnter + (tExit - tEnter) * hitS;
          return true;
        }

        if (lowerDifference * candidateDifference <= 0.0f)
        {
          upperS = candidateS;
          upperDifference = candidateDifference;
        }
        else
        {
          lowerS = candidateS;
          lowerDifference = candidateDifference;
        }
      }

      const float finalDenom = std::fabs(lowerDifference) + std::fabs(upperDifference);
      const float finalAlpha = finalDenom > 1.0e-8f ? std::fabs(lowerDifference) / finalDenom : 0.5f;
      hitS = lowerS + (upperS - lowerS) * finalAlpha;
      outT = tEnter + (tExit - tEnter) * hitS;
      return true;
    }

    previousValid = true;
    previousS = s;
    previousDifference = difference;
  }

  return false;
}

void traceRBVHNode(const softwarebvh::RBVHBuildResult &build, const uint32_t nodeIndex, const Vec3 &rayOrigin, const Vec3 &rayDirection, RBVHTraceHit &hit)
{
  if (nodeIndex >= build.cardBuild.canonicalNodes.size())
    return;

  const softwarebvh::CWBVHBuilder::CanonicalNode &node = build.cardBuild.canonicalNodes[nodeIndex];
  float tEnter = 0.0f;
  float tExit = 0.0f;
  if (!intersectAABB(node.bounds, rayOrigin, rayDirection, tEnter, tExit) || tEnter > hit.t)
    return;

  if (node.isLeaf)
  {
    assert(node.primitiveIndex < build.cardBuild.primitiveRemap.size());
    const uint32_t cardIndex = build.cardBuild.primitiveRemap[node.primitiveIndex];
    float cardT = 0.0f;
    if (traceCardHeightField(build, cardIndex, rayOrigin, rayDirection, cardT) && cardT < hit.t)
    {
      hit.hit = true;
      hit.t = cardT;
      hit.cardIndex = cardIndex;
    }
    return;
  }

  traceRBVHNode(build, node.left, rayOrigin, rayDirection, hit);
  traceRBVHNode(build, node.right, rayOrigin, rayDirection, hit);
}

RBVHTraceHit traceRBVH(const softwarebvh::RBVHBuildResult &build, const Vec3 &rayOrigin, const Vec3 &rayDirection)
{
  RBVHTraceHit hit{};
  if (build.cardBuild.rootNodeIndex != softwarebvh::CWBVHBuilder::INVALID_NODE)
    traceRBVHNode(build, build.cardBuild.rootNodeIndex, rayOrigin, rayDirection, hit);
  return hit;
}

std::vector<RBVHSamplePoint> collectRBVHSamples(const softwarebvh::RBVHBuildResult &build, const uint32_t maxSamples)
{
  std::vector<RBVHSamplePoint> samples;
  samples.reserve(maxSamples);
  for (uint32_t cardIndex = 0u; cardIndex < build.cards.size() && samples.size() < maxSamples; ++cardIndex)
  {
    const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
    bool found = false;
    for (uint32_t y = 0u; y < card.atlasHeight && !found; ++y)
    {
      for (uint32_t x = 0u; x < card.atlasWidth; ++x)
      {
        const uint32_t atlasX = card.atlasX + x;
        const uint32_t atlasY = card.atlasY + y;
        const uint32_t pixelIndex = atlasY * build.atlasSize + atlasX;
        if (pixelIndex >= build.coverageAtlasR8.size() || build.coverageAtlasR8[pixelIndex] != 1u)
          continue;

        samples.push_back(
            RBVHSamplePoint{
              .point = reconstructCardPoint(build, cardIndex, atlasX, atlasY),
              .normal = loadArray3(card.axisN),
              .cardIndex = cardIndex,
              .atlasX = atlasX,
              .atlasY = atlasY,
            });
        found = true;
        break;
      }
    }
  }
  return samples;
}

void verifyRBVHBuild(const std::string &label, const FallbackMeshData &mesh, const softwarebvh::RBVHBuildOptions &options, const uint32_t traceSampleCount)
{
  const softwarebvh::RBVHBuildResult build = softwarebvh::RBVHBuilder::build(mesh, options);
  const uint32_t primitiveCount = static_cast<uint32_t>(mesh.indices.size() / 3u);
  assert(primitiveCount > 0u);
  assert(!build.cards.empty());
  assert(build.cardBuild.primitiveCount == build.cards.size());
  assert(build.cardBuild.primitiveRemap.size() == build.cards.size());
  assert(build.depthAtlasR16.size() == build.coverageAtlasR8.size());
  assert(build.albedoAtlasRGBA8.empty());
  assert(!build.depthMinMaxMipAtlasRG16.empty());
  assert(build.depthMinMaxMipAtlasRG16 == softwarebvh::RBVHBuilder::buildDepthMinMaxMipAtlas(build.atlasSize, build.depthAtlasR16, build.coverageAtlasR8));
  assert(build.atlasSize > 0u);

  std::vector<uint32_t> primitiveVisitCount(primitiveCount, 0u);
  uint32_t coveredPixelCount = 0u;
  for (uint32_t cardIndex = 0u; cardIndex < build.cards.size(); ++cardIndex)
  {
    const softwarebvh::RBVHCardPrimitive &card = build.cards[cardIndex];
    assert(card.sourcePrimitiveCount > 0u);
    assert(card.sourcePrimitiveOffset + card.sourcePrimitiveCount <= build.sourcePrimitiveIndices.size());
    assert(card.atlasWidth >= options.minCardResolution);
    assert(card.atlasHeight >= options.minCardResolution);
    assert(card.atlasWidth <= options.maxCardResolution);
    assert(card.atlasHeight <= options.maxCardResolution);
    assert(card.atlasX + card.atlasWidth <= build.atlasSize);
    assert(card.atlasY + card.atlasHeight <= build.atlasSize);
    const uint32_t cardAlignment = std::min(nextPowerOfTwo(std::max(card.atlasWidth, card.atlasHeight)), build.atlasSize);
    assert(card.atlasX % cardAlignment == 0u);
    assert(card.atlasY % cardAlignment == 0u);
    assert(card.depthMin <= card.depthMax);

    for (uint32_t sourceIndex = 0u; sourceIndex < card.sourcePrimitiveCount; ++sourceIndex)
    {
      const uint32_t primitiveIndex = build.sourcePrimitiveIndices[card.sourcePrimitiveOffset + sourceIndex];
      assert(primitiveIndex < primitiveCount);
      primitiveVisitCount[primitiveIndex] += 1u;
    }

    for (uint32_t y = 0u; y < card.atlasHeight; ++y)
    {
      for (uint32_t x = 0u; x < card.atlasWidth; ++x)
      {
        const uint32_t pixelIndex = (card.atlasY + y) * build.atlasSize + card.atlasX + x;
        if (build.coverageAtlasR8[pixelIndex] != 0u)
          coveredPixelCount += 1u;
      }
    }
  }

  for (uint32_t primitiveIndex = 0u; primitiveIndex < primitiveCount; ++primitiveIndex)
    assert(primitiveVisitCount[primitiveIndex] >= 1u);
  assert(coveredPixelCount > 0u);

  std::vector<uint32_t> cardVisitCount(build.cards.size(), 0u);
  for (const uint32_t cardIndex : build.cardBuild.primitiveRemap)
  {
    assert(cardIndex < build.cards.size());
    cardVisitCount[cardIndex] += 1u;
  }
  for (uint32_t cardIndex = 0u; cardIndex < build.cards.size(); ++cardIndex)
    assert(cardVisitCount[cardIndex] == 1u);

  const std::vector<RBVHSamplePoint> samples = collectRBVHSamples(build, traceSampleCount);
  assert(!samples.empty());
  for (const RBVHSamplePoint &sample : samples)
  {
    const float rayDistance = std::max(distance(loadArray3(build.cards[sample.cardIndex].boundsMin), loadArray3(build.cards[sample.cardIndex].boundsMax)), 1.0f);
    const Vec3 rayDirection = scale(normalize(sample.normal), -1.0f);
    const Vec3 rayOrigin = add(sample.point, scale(sample.normal, rayDistance));
    float directCardT = 0.0f;
    assert(traceCardHeightField(build, sample.cardIndex, rayOrigin, rayDirection, directCardT));
    const RBVHTraceHit hit = traceRBVH(build, rayOrigin, rayDirection);
    assert(hit.hit);
  }

  std::cout << "  [OK] " << label << ": " << build.cards.size() << " RBVH cards, " << build.atlasSize << "x" << build.atlasSize << " atlas, traced " << samples.size() << " card samples\n";
}

struct RBVHInstance
{
  const softwarebvh::RBVHBuildResult *build = nullptr;
  Vec3 translation{};
};

RBVHTraceHit traceRBVHInstances(const std::vector<RBVHInstance> &instances, const Vec3 &rayOrigin, const Vec3 &rayDirection, uint32_t &outInstanceIndex)
{
  RBVHTraceHit bestHit{};
  outInstanceIndex = UINT32_MAX;
  for (uint32_t instanceIndex = 0u; instanceIndex < instances.size(); ++instanceIndex)
  {
    const RBVHInstance &instance = instances[instanceIndex];
    const RBVHTraceHit hit = traceRBVH(*instance.build, subtract(rayOrigin, instance.translation), rayDirection);
    if (hit.hit && hit.t < bestHit.t)
    {
      bestHit = hit;
      outInstanceIndex = instanceIndex;
    }
  }
  return bestHit;
}

void testRBVHBuildAndTrace()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 32u;
  options.maxAtlasSize = 8192u;
  options.projectedAreaThreshold = 0.55f;
  options.targetTileResolution = 128.0f;

  verifyRBVHBuild("RBVH single triangle", makeSingleTriangleMesh(), options, 1u);
  const softwarebvh::RBVHBuildResult opposingBuild = softwarebvh::RBVHBuilder::build(makeOpposingTrianglePairMesh(), options);
  assert(opposingBuild.cards.size() == 2u);
  const FallbackMeshData planarGrid = makePlanarGridMesh(8u);
  const softwarebvh::RBVHBuildResult planarGridBuild = softwarebvh::RBVHBuilder::build(planarGrid, options);
  assert(planarGridBuild.cards.size() == 1u);
  assert(planarGridBuild.cards.front().sourcePrimitiveCount == planarGrid.indices.size() / 3u);
  const FallbackMeshData densePlanarGrid = makePlanarGridMesh(64u);
  const softwarebvh::RBVHBuildResult densePlanarGridBuild = softwarebvh::RBVHBuilder::build(densePlanarGrid, options);
  assert(densePlanarGridBuild.cards.size() == 1u);
  assert(densePlanarGridBuild.cards.front().sourcePrimitiveCount == densePlanarGrid.indices.size() / 3u);
  verifyRBVHBuild("RBVH planar grid", planarGrid, options, 4u);

  softwarebvh::RBVHBuildOptions variableResolutionOptions{};
  variableResolutionOptions.projectedAreaThreshold = 0.55f;
  variableResolutionOptions.targetTileResolution = static_cast<float>(variableResolutionOptions.maxCardResolution);
  const softwarebvh::RBVHBuildResult variableResolutionGridBuild = softwarebvh::RBVHBuilder::build(densePlanarGrid, variableResolutionOptions);
  assert(variableResolutionGridBuild.cards.size() == 1u);
  assert(variableResolutionGridBuild.cards.front().sourcePrimitiveCount == densePlanarGrid.indices.size() / 3u);
  assert(variableResolutionGridBuild.cards.front().atlasWidth > options.maxCardResolution);
  assert(variableResolutionGridBuild.cards.front().atlasHeight > options.maxCardResolution);
  assert(variableResolutionGridBuild.cards.front().atlasWidth <= variableResolutionOptions.maxCardResolution);
  assert(variableResolutionGridBuild.cards.front().atlasHeight <= variableResolutionOptions.maxCardResolution);

  verifyRBVHBuild("RBVH unit cube", makeUnitCubeMesh(), options, 8u);
  verifyRBVHBuild("RBVH Stanford Bunny", loadObjFallbackMesh(stanfordBunnyAssetPath()), options, 16u);
}

void testRBVHDepthMipsIncludeDilatedCoverage()
{
  const uint32_t atlasSize = 2u;
  const std::vector<uint16_t> depthAtlas = {123u, 0xffffu, 0xffffu, 0xffffu};
  const std::vector<uint8_t> coverageAtlas = {2u, 0u, 0u, 0u};
  const std::vector<uint32_t> mips = softwarebvh::RBVHBuilder::buildDepthMinMaxMipAtlas(atlasSize, depthAtlas, coverageAtlas);
  assert(mips.size() == 1u);
  assert((mips.front() & 0xffffu) == 123u);
  assert((mips.front() >> 16u) == 123u);
}

void testRBVHMultipleInstances()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 16u;
  options.maxAtlasSize = 8192u;
  options.maxCards = 32768u;
  options.projectedAreaThreshold = 0.35f;

  const FallbackMeshData bunnyMesh = loadObjFallbackMesh(stanfordBunnyAssetPath());
  const FallbackMeshData sponzaMesh = loadObjFallbackMesh(sponzaAssetPath());
  const softwarebvh::RBVHBuildResult bunnyBuild = softwarebvh::RBVHBuilder::build(bunnyMesh, options);
  const softwarebvh::RBVHBuildResult sponzaBuild = softwarebvh::RBVHBuilder::build(sponzaMesh, options);
  assert(!bunnyBuild.cards.empty());
  assert(!sponzaBuild.cards.empty());
  assert(sponzaBuild.cards.size() <= options.maxCards);

  const std::vector<RBVHSamplePoint> bunnySamples = collectRBVHSamples(bunnyBuild, 1u);
  const std::vector<RBVHSamplePoint> sponzaSamples = collectRBVHSamples(sponzaBuild, 1u);
  assert(!bunnySamples.empty());
  assert(!sponzaSamples.empty());

  const float separation = 2.0f * std::max(distance(loadArray3(bunnyMesh.boundsMin), loadArray3(bunnyMesh.boundsMax)), distance(loadArray3(sponzaMesh.boundsMin), loadArray3(sponzaMesh.boundsMax))) + 10.0f;
  const std::vector<RBVHInstance> instances{
    RBVHInstance{.build = &bunnyBuild, .translation = {-separation, 0.0f, 0.0f}},
    RBVHInstance{.build = &sponzaBuild, .translation = {+separation, 0.0f, 0.0f}},
    RBVHInstance{.build = &bunnyBuild, .translation = {0.0f, +separation, 0.0f}},
  };

  const std::array<RBVHSamplePoint, 3u> samples{bunnySamples.front(), sponzaSamples.front(), bunnySamples.front()};
  for (uint32_t instanceIndex = 0u; instanceIndex < instances.size(); ++instanceIndex)
  {
    const RBVHInstance &instance = instances[instanceIndex];
    const RBVHSamplePoint &sample = samples[instanceIndex];
    const Vec3 rayDirection = scale(normalize(sample.normal), -1.0f);
    const Vec3 worldPoint = add(sample.point, instance.translation);
    const Vec3 rayOrigin = add(worldPoint, scale(sample.normal, separation * 0.25f));
    uint32_t hitInstanceIndex = UINT32_MAX;
    const RBVHTraceHit hit = traceRBVHInstances(instances, rayOrigin, rayDirection, hitInstanceIndex);
    assert(hit.hit);
    assert(hitInstanceIndex == instanceIndex);
  }

  std::cout << "  [OK] RBVH multi-instance Bunny/Sponza trace: " << bunnyBuild.cards.size() << " bunny cards, " << sponzaBuild.cards.size() << " sponza cards\n";
}

void testSWRBVHLodsIgnoreFallbackMeshLodMetadata()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 64u;
  options.maxAtlasSize = 1024u;
  options.projectedAreaThreshold = 0.55f;

  FallbackMeshData lod0Mesh = makePlanarGridMesh(16u);
  FallbackMeshData lod3Mesh = lod0Mesh;
  lod0Mesh.lodLevel = 0u;
  lod3Mesh.lodLevel = 3u;

  const softwarebvh::SWRBVHFallbackMeshData lod0Data = softwarebvh::SWRBVHFallbackMeshFile::build(lod0Mesh, options);
  const softwarebvh::SWRBVHFallbackMeshData lod3Data = softwarebvh::SWRBVHFallbackMeshFile::build(lod3Mesh, options);
  assert(lod0Data.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(lod3Data.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);

  for (uint32_t lodIndex = 0u; lodIndex < softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodIndex)
  {
    const softwarebvh::RBVHBuildResult &lod0Build = lod0Data.rbvhBuilds[lodIndex];
    const softwarebvh::RBVHBuildResult &lod3Build = lod3Data.rbvhBuilds[lodIndex];
    assert(lod0Build.lodLevel == lodIndex);
    assert(lod3Build.lodLevel == lodIndex);
    assert(lod0Build.cards.size() == lod3Build.cards.size());
    assert(lod0Build.atlasSize == lod3Build.atlasSize);
    assert(maxRbvhCardExtent(lod0Build) == maxRbvhCardExtent(lod3Build));
  }
}

void testSWRBVHLodsCanUseOriginalSourceMesh()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 32u;
  options.maxAtlasSize = 512u;
  options.projectedAreaThreshold = 0.55f;
  options.splitOverlapFraction = 0.10f;

  FallbackMeshData fallbackMesh = makeSingleTriangleMesh();
  FallbackMeshData sourceMesh = makePlanarGridMesh(8u);
  fallbackMesh.lodLevel = 3u;
  sourceMesh.lodLevel = 0u;

  const softwarebvh::SWRBVHFallbackMeshData data = softwarebvh::SWRBVHFallbackMeshFile::build(fallbackMesh, sourceMesh, options);
  assert(data.fallbackMesh.indices.size() == fallbackMesh.indices.size());
  assert(data.triangleBuild.primitiveCount == 0u);
  assert(data.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  for (const softwarebvh::RBVHBuildResult &build : data.rbvhBuilds)
  {
    uint32_t representedPrimitiveCount = 0u;
    for (const softwarebvh::RBVHCardPrimitive &card : build.cards)
    {
      representedPrimitiveCount += card.sourcePrimitiveCount;
    }
    assert(representedPrimitiveCount >= sourceMesh.indices.size() / 3u);
  }
}

void testSWRBVHLodsLoosenConeAndPruneTree()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 32u;
  options.maxAtlasSize = 512u;
  options.refinementCriterion = softwarebvh::RBVHRefinementCriterion::NormalCone;
  options.minNormalDot = 0.9f;
  options.projectedAreaThreshold = 0.1f;
  options.targetTileResolution = 128.0f;

  const FallbackMeshData mesh = makeBentCornerMesh();
  const softwarebvh::SWRBVHFallbackMeshData data = softwarebvh::SWRBVHFallbackMeshFile::build(mesh, options);
  assert(data.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(data.rbvhBuilds.front().cards.size() >= data.rbvhBuilds.back().cards.size());
  assert(maxRbvhCardExtent(data.rbvhBuilds.front()) >= maxRbvhCardExtent(data.rbvhBuilds.back()));
}

void testSWRBVHLodsStayProgressiveForSponza()
{
  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 16u;
  options.maxAtlasSize = 2048u;
  options.maxCards = 4096u;
  options.projectedAreaThreshold = 0.55f;

  const FallbackMeshData sponzaMesh = loadObjFallbackMesh(sponzaAssetPath());
  const softwarebvh::SWRBVHFallbackMeshData data = softwarebvh::SWRBVHFallbackMeshFile::build(sponzaMesh, options);
  assert(data.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(!data.rbvhBuilds[0u].cards.empty());
  assert(!data.rbvhBuilds[1u].cards.empty());

  const size_t lod0Cards = data.rbvhBuilds[0u].cards.size();
  const size_t lod1Cards = data.rbvhBuilds[1u].cards.size();
  assert(lod1Cards >= lod0Cards / 2u);

  for (uint32_t lodIndex = 1u; lodIndex < softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodIndex)
  {
    assert(data.rbvhBuilds[lodIndex].cards.size() <= data.rbvhBuilds[lodIndex - 1u].cards.size());
    assert(coveredPixelCount(data.rbvhBuilds[lodIndex]) > 0u);
  }
}

void testRuntimeProxyFallbackKeepsOriginalRBVHSourceForSponza()
{
  const VirtualGeometrySourceAsset sourceAsset = loadObjSourceAssetForFallbackTest(sponzaAssetPath(), "sponza_proxy_rbvh");
  const FallbackMeshData originalMesh = FallbackMeshBuilder::buildOriginal(sourceAsset);
  const FallbackMeshData proxyMesh = FallbackMeshBuilder::buildProxy(sourceAsset);
  assert(!proxyMesh.vertices.empty());
  assert(!proxyMesh.indices.empty());
  assert(proxyMesh.indices.size() % 3u == 0u);
  assert(proxyMesh.indices.size() < originalMesh.indices.size());
  std::cout << "  [INFO] Sponza proxy simplification: "
            << (originalMesh.indices.size() / 3u) << " original triangles, "
            << (proxyMesh.indices.size() / 3u) << " proxy triangles\n";
  assert(proxyMesh.indices.size() <= originalMesh.indices.size() / 2u);
  compareBounds(originalMesh.boundsMin, originalMesh.boundsMax, proxyMesh.boundsMin, proxyMesh.boundsMax);

  softwarebvh::RBVHBuildOptions options{};
  options.minCardResolution = 4u;
  options.maxCardResolution = 64u;
  options.maxAtlasSize = 2048u;
  options.maxCards = 4096u;
  options.projectedAreaThreshold = 0.55f;
  options.minNormalDot = 0.5f;
  options.splitOverlapFraction = 0.0f;
  options.cardBoundsPadding = 5.0e-4f;

  const softwarebvh::RBVHBuildResult build = softwarebvh::RBVHBuilder::build(originalMesh, options);
  assert(!build.cards.empty());
  assert(build.cards.size() <= options.maxCards);
  assert(coveredPixelCount(build) > 0u);
  std::cout << "  [OK] Sponza original RBVH runtime build: "
            << (originalMesh.indices.size() / 3u) << " original triangles, "
            << (proxyMesh.indices.size() / 3u) << " proxy CWBVH triangles, "
            << build.cards.size() << " cards\n";
}

void testBuilderBounds()
{
  verifyBuildAgainstMesh("single triangle", makeSingleTriangleMesh(), true);
  verifyBuildAgainstMesh("unit cube", makeUnitCubeMesh(), true);
  verifyBuildAgainstMesh("Suzanne", loadObjFallbackMesh(suzanneAssetPath()), false);
  verifyBuildAgainstMesh("Sponza", loadObjFallbackMesh(sponzaAssetPath()), false);
}

void testFallbackCollectionRoundTrip()
{
  const FallbackMeshData triangle = makeSingleTriangleMesh();
  const FallbackMeshData cube = makeUnitCubeMesh();
  const std::vector<FallbackMeshData> meshes{triangle, cube};

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHFallbackTests_collection.fallback.mesh";
  std::error_code error;
  fs::remove(tempPath, error);

  assert(FallbackMeshFile::save(tempPath.string(), meshes));

  std::vector<FallbackMeshData> loadedMeshes;
  assert(FallbackMeshFile::load(tempPath.string(), loadedMeshes));
  assert(loadedMeshes.size() == meshes.size());
  compareMeshes(triangle, loadedMeshes[0u]);
  compareMeshes(cube, loadedMeshes[1u]);

  FallbackMeshData loadedEntry;
  assert(FallbackMeshFile::loadEntry(tempPath.string(), 1u, loadedEntry));
  compareMeshes(cube, loadedEntry);

  FallbackMeshData mergedMesh;
  assert(FallbackMeshFile::load(tempPath.string(), mergedMesh));
  assert(mergedMesh.vertices.size() == triangle.vertices.size() + cube.vertices.size());
  assert(mergedMesh.indices.size() == triangle.indices.size() + cube.indices.size());

  fs::remove(tempPath, error);
}

void testSWRTCollectionRoundTrip()
{
  const softwarebvh::SWRTFallbackMeshData triangle = softwarebvh::SWRTFallbackMeshFile::build(makeSingleTriangleMesh());
  const softwarebvh::SWRTFallbackMeshData cube = softwarebvh::SWRTFallbackMeshFile::build(makeUnitCubeMesh());
  const std::vector<softwarebvh::SWRTFallbackMeshData> data{triangle, cube};

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHFallbackTests_collection.SWRTFallback.mesh";
  std::error_code error;
  fs::remove(tempPath, error);

  assert(softwarebvh::SWRTFallbackMeshFile::save(tempPath.string(), data));

  std::vector<softwarebvh::SWRTFallbackMeshData> loadedData;
  assert(softwarebvh::SWRTFallbackMeshFile::load(tempPath.string(), loadedData));
  assert(loadedData.size() == data.size());
  compareMeshes(triangle.fallbackMesh, loadedData[0u].fallbackMesh);
  compareMeshes(cube.fallbackMesh, loadedData[1u].fallbackMesh);
  compareBuildResults(triangle.build, loadedData[0u].build);
  compareBuildResults(cube.build, loadedData[1u].build);
  verifyWoopTriangleTransforms("SWRT triangle roundtrip", loadedData[0u].fallbackMesh, loadedData[0u].build, true);
  verifyWoopTriangleTransforms("SWRT cube roundtrip", loadedData[1u].fallbackMesh, loadedData[1u].build, true);

  softwarebvh::SWRTFallbackMeshData loadedEntry;
  assert(softwarebvh::SWRTFallbackMeshFile::loadEntry(tempPath.string(), 0u, loadedEntry));
  compareMeshes(triangle.fallbackMesh, loadedEntry.fallbackMesh);
  compareBuildResults(triangle.build, loadedEntry.build);
  verifyWoopTriangleTransforms("SWRT entry roundtrip", loadedEntry.fallbackMesh, loadedEntry.build, true);

  assert(softwarebvh::SWRTFallbackMeshFile::loadEntry(tempPath.string(), 1u, loadedEntry));
  compareMeshes(cube.fallbackMesh, loadedEntry.fallbackMesh);
  compareBuildResults(cube.build, loadedEntry.build);
  verifyWoopTriangleTransforms("SWRT second entry roundtrip", loadedEntry.fallbackMesh, loadedEntry.build, true);

  fs::remove(tempPath, error);
}

void testSWRBVHCollectionRoundTrip()
{
  const softwarebvh::SWRBVHFallbackMeshData triangle = softwarebvh::SWRBVHFallbackMeshFile::build(makeSingleTriangleMesh());
  const softwarebvh::SWRBVHFallbackMeshData cube = softwarebvh::SWRBVHFallbackMeshFile::build(makeUnitCubeMesh());
  const std::vector<softwarebvh::SWRBVHFallbackMeshData> data{triangle, cube};

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHFallbackTests_collection.SWRBVH.mesh";
  std::error_code error;
  fs::remove(tempPath, error);

  assert(softwarebvh::SWRBVHFallbackMeshFile::save(tempPath.string(), data));

  std::vector<softwarebvh::SWRBVHFallbackMeshData> loadedData;
  assert(softwarebvh::SWRBVHFallbackMeshFile::load(tempPath.string(), loadedData));
  assert(loadedData.size() == data.size());
  assert(loadedData[0u].fallbackMesh.vertices.empty());
  assert(loadedData[0u].fallbackMesh.indices.empty());
  assert(loadedData[1u].fallbackMesh.vertices.empty());
  assert(loadedData[1u].fallbackMesh.indices.empty());
  assert(loadedData[0u].triangleBuild.primitiveCount == 0u);
  assert(loadedData[1u].triangleBuild.primitiveCount == 0u);
  assert(triangle.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(cube.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(loadedData[0u].rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(loadedData[1u].rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(!loadedData[0u].rbvhBuild.cards.empty());
  assert(!loadedData[1u].rbvhBuild.cards.empty());
  assert(loadedData[0u].rbvhBuild.cardBuild.primitiveCount == loadedData[0u].rbvhBuild.cards.size());
  assert(loadedData[1u].rbvhBuild.cardBuild.primitiveCount == loadedData[1u].rbvhBuild.cards.size());
  assert(loadedData[0u].rbvhBuild.depthAtlasR16.size() == loadedData[0u].rbvhBuild.coverageAtlasR8.size());
  assert(loadedData[1u].rbvhBuild.depthAtlasR16.size() == loadedData[1u].rbvhBuild.coverageAtlasR8.size());
  assert(loadedData[0u].rbvhBuild.albedoAtlasRGBA8.empty());
  assert(loadedData[1u].rbvhBuild.albedoAtlasRGBA8.empty());
  assert(!loadedData[0u].rbvhBuild.depthMinMaxMipAtlasRG16.empty());
  assert(!loadedData[1u].rbvhBuild.depthMinMaxMipAtlasRG16.empty());
  assert(loadedData[0u].rbvhBuild.depthMinMaxMipAtlasRG16 == triangle.rbvhBuild.depthMinMaxMipAtlasRG16);
  assert(loadedData[1u].rbvhBuild.depthMinMaxMipAtlasRG16 == cube.rbvhBuild.depthMinMaxMipAtlasRG16);
  for (uint32_t lodIndex = 0u; lodIndex < softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodIndex)
  {
    assert(loadedData[0u].rbvhBuilds[lodIndex].lodLevel == lodIndex);
    assert(loadedData[1u].rbvhBuilds[lodIndex].lodLevel == lodIndex);
    assert(loadedData[0u].rbvhBuilds[lodIndex].cardBuild.primitiveCount == loadedData[0u].rbvhBuilds[lodIndex].cards.size());
    assert(loadedData[1u].rbvhBuilds[lodIndex].cardBuild.primitiveCount == loadedData[1u].rbvhBuilds[lodIndex].cards.size());
    assert(loadedData[0u].rbvhBuilds[lodIndex].depthMinMaxMipAtlasRG16 == triangle.rbvhBuilds[lodIndex].depthMinMaxMipAtlasRG16);
    assert(loadedData[1u].rbvhBuilds[lodIndex].depthMinMaxMipAtlasRG16 == cube.rbvhBuilds[lodIndex].depthMinMaxMipAtlasRG16);
  }
  assert(fs::exists(tempPath.string() + ".entry0.depth.pgm"));
  assert(!fs::exists(tempPath.string() + ".entry0.albedo.ppm"));
  assert(fs::exists(tempPath.string() + ".entry0.lod3.depth.pgm"));

  softwarebvh::SWRBVHFallbackMeshData loadedEntry;
  assert(softwarebvh::SWRBVHFallbackMeshFile::loadEntry(tempPath.string(), 1u, loadedEntry));
  assert(loadedEntry.fallbackMesh.vertices.empty());
  assert(loadedEntry.fallbackMesh.indices.empty());
  assert(loadedEntry.triangleBuild.primitiveCount == 0u);
  assert(loadedEntry.rbvhBuilds.size() == softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT);
  assert(loadedEntry.rbvhBuild.cardBuild.primitiveCount == loadedEntry.rbvhBuild.cards.size());
  assert(loadedEntry.rbvhBuild.depthMinMaxMipAtlasRG16 == cube.rbvhBuild.depthMinMaxMipAtlasRG16);

  fs::remove(tempPath, error);
  fs::remove(tempPath.string() + ".entry0.depth.pgm", error);
  fs::remove(tempPath.string() + ".entry0.albedo.ppm", error);
  fs::remove(tempPath.string() + ".entry1.depth.pgm", error);
  fs::remove(tempPath.string() + ".entry1.albedo.ppm", error);
  for (uint32_t entryIndex = 0u; entryIndex < 2u; ++entryIndex)
  {
    for (uint32_t lodIndex = 0u; lodIndex < softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodIndex)
    {
      fs::remove(tempPath.string() + ".entry" + std::to_string(entryIndex) + ".lod" + std::to_string(lodIndex) + ".depth.pgm", error);
      fs::remove(tempPath.string() + ".entry" + std::to_string(entryIndex) + ".lod" + std::to_string(lodIndex) + ".albedo.ppm", error);
    }
  }
}

void testSWRBVHLoadsRepairStaleCardBuild()
{
  const FallbackMeshData cubeMesh = makeUnitCubeMesh();
  softwarebvh::SWRBVHFallbackMeshData data = softwarebvh::SWRBVHFallbackMeshFile::build(cubeMesh);
  const softwarebvh::CWBVHBuilder::BuildResult triangleBuild = softwarebvh::CWBVHBuilder::build(cubeMesh);
  assert(!data.rbvhBuilds.empty());
  for (softwarebvh::RBVHBuildResult &build : data.rbvhBuilds)
  {
    build.cardBuild = triangleBuild;
    build.cardBuild.primitiveCount = build.cards.empty() ? 1u : 0u;
    assert(build.cardBuild.primitiveCount != build.cards.size());
  }
  data.rbvhBuild = data.rbvhBuilds.front();

  const fs::path tempPath = fs::temp_directory_path() / "SoftwareBVHFallbackTests_stale_card_build.SWRBVH.mesh";
  std::error_code error;
  fs::remove(tempPath, error);
  assert(softwarebvh::SWRBVHFallbackMeshFile::save(tempPath.string(), data));

  softwarebvh::SWRBVHFallbackMeshData loaded;
  assert(softwarebvh::SWRBVHFallbackMeshFile::load(tempPath.string(), loaded));
  assert(loaded.rbvhBuild.cardBuild.primitiveCount == loaded.rbvhBuild.cards.size());
  assert(loaded.rbvhBuild.cardBuild.primitiveRemap.size() == loaded.rbvhBuild.cards.size());
  for (const softwarebvh::RBVHBuildResult &build : loaded.rbvhBuilds)
  {
    assert(build.cardBuild.primitiveCount == build.cards.size());
    assert(build.cardBuild.primitiveRemap.size() == build.cards.size());
  }

  fs::remove(tempPath, error);
  fs::remove(tempPath.string() + ".depth.pgm", error);
  fs::remove(tempPath.string() + ".albedo.ppm", error);
  for (uint32_t lodIndex = 0u; lodIndex < softwarebvh::SWRBVH_FALLBACK_MESH_LOD_COUNT; ++lodIndex)
  {
    fs::remove(tempPath.string() + ".lod" + std::to_string(lodIndex) + ".depth.pgm", error);
    fs::remove(tempPath.string() + ".lod" + std::to_string(lodIndex) + ".albedo.ppm", error);
  }
}

} // namespace

int main()
{
  testBuilderBounds();
  testRBVHBuildAndTrace();
  testRBVHDepthMipsIncludeDilatedCoverage();
  testRBVHMultipleInstances();
  testSWRBVHLodsIgnoreFallbackMeshLodMetadata();
  testSWRBVHLodsCanUseOriginalSourceMesh();
  testSWRBVHLodsLoosenConeAndPruneTree();
  testSWRBVHLodsStayProgressiveForSponza();
  testRuntimeProxyFallbackKeepsOriginalRBVHSourceForSponza();
  testFallbackCollectionRoundTrip();
  testSWRTCollectionRoundTrip();
  testSWRBVHCollectionRoundTrip();
  testSWRBVHLoadsRepairStaleCardBuild();

  std::cout << "SoftwareBVHFallbackTests passed\n";
  return 0;
}
